#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include "lwip/netdb.h"
#include "lwip/dns.h"
#include "http_server.h"
#include "wifi.h"
#include "esp_wifi.h"

/* Components */
#include "ota-service.h"
#include "esp32-akri.h"

#define WIFI_SUCCESS 1 << 0


#include "esp_camera.h"
#include "esp_timer.h"
#include "camera_pins.h"

esp_err_t tasks_http_handler(httpd_req_t *req);
static const char *TAG = "stream";

#define PART_BOUNDARY "123456789000000000000987654321"
static const char* _STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char* _STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char* _STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

#define CONFIG_XCLK_FREQ 20000000

#include "esp_http_server.h"
#include "cJSON.h"
#include "mqtt_client.h"


static char mqtt_broker[128] = {0};
static char mqtt_topic[64] = {0};
static char mqtt_user[64] = {0};
static char mqtt_pass[64] = {0};

static esp_mqtt_client_handle_t mqtt_client = NULL;

static esp_err_t mqtt_event_handler_cb(esp_mqtt_event_handle_t event) {
    switch (event->event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT connected");
            break;
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "MQTT disconnected");
            break;
        default:
            break;
    }
    return ESP_OK;
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    mqtt_event_handler_cb(event_data);
}

esp_err_t mqtt_start_client() {
    if (mqtt_client) {
        esp_mqtt_client_stop(mqtt_client);
        esp_mqtt_client_destroy(mqtt_client);
        mqtt_client = NULL;
    }

    if (strlen(mqtt_broker) == 0) {
        ESP_LOGW(TAG, "MQTT broker URL is empty, cannot start client");
        return ESP_FAIL;
    }

    esp_mqtt_client_config_t mqtt_cfg = {
    .broker.address.uri =  mqtt_broker,
    // optionally: .verification = {...} if you use certs
    };


    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    if (!mqtt_client) {
        ESP_LOGE(TAG, "Failed to init MQTT client");
        return ESP_FAIL;
    }
    esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(mqtt_client);
    ESP_LOGI(TAG, "MQTT client started with broker %s", mqtt_broker);
    return ESP_OK;
}

static esp_err_t config_post_handler(httpd_req_t *req) {
    char content[512];
    int ret, remaining = req->content_len;

    if (remaining >= sizeof(content)) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Internal Server Error");
        return ESP_FAIL;
    }

    int read_len = 0;
    while (remaining > 0) {
        ret = httpd_req_recv(req, content + read_len, remaining);
        if (ret <= 0) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Internal Server Error");
            return ESP_FAIL;
        }
        read_len += ret;
        remaining -= ret;
    }
    content[read_len] = '\0';

    ESP_LOGI(TAG, "Received config: %s", content);

    cJSON *root = cJSON_Parse(content);
    if (!root) {
        ESP_LOGE(TAG, "Invalid JSON");
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid Json");
        return ESP_FAIL;
    }

    cJSON *broker = cJSON_GetObjectItem(root, "mqtt_broker");
    cJSON *topic = cJSON_GetObjectItem(root, "mqtt_topic");
    cJSON *user = cJSON_GetObjectItem(root, "mqtt_user");
    cJSON *pass = cJSON_GetObjectItem(root, "mqtt_pass");

    if (!broker || !cJSON_IsString(broker)) {
        ESP_LOGE(TAG, "mqtt_broker missing or invalid");
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "mqtt_broker missing or invalid");
        return ESP_FAIL;
    }

    if (!topic || !cJSON_IsString(topic)) {
        ESP_LOGE(TAG, "mqtt_topic missing or invalid");
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "mqtt_topic missing or invalid");
        return ESP_FAIL;
    }

    strncpy(mqtt_broker, broker->valuestring, sizeof(mqtt_broker) - 1);
    mqtt_broker[sizeof(mqtt_broker) - 1] = 0;

    strncpy(mqtt_topic, topic->valuestring, sizeof(mqtt_topic) - 1);
    mqtt_topic[sizeof(mqtt_topic) - 1] = 0;

    if (user && cJSON_IsString(user)) {
        strncpy(mqtt_user, user->valuestring, sizeof(mqtt_user) - 1);
        mqtt_user[sizeof(mqtt_user) - 1] = 0;
    } else {
        mqtt_user[0] = 0;
    }

    if (pass && cJSON_IsString(pass)) {
        strncpy(mqtt_pass, pass->valuestring, sizeof(mqtt_pass) - 1);
        mqtt_pass[sizeof(mqtt_pass) - 1] = 0;
    } else {
        mqtt_pass[0] = 0;
    }

    cJSON_Delete(root);

    if (mqtt_start_client() != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Internal Server Error");
        return ESP_FAIL;
    }

    httpd_resp_sendstr(req, "MQTT config updated\n");
    return ESP_OK;
}

static esp_err_t init_camera(void)
{
    camera_config_t camera_config = {
        .pin_pwdn  = CAM_PIN_PWDN,
        .pin_reset = CAM_PIN_RESET,
        .pin_xclk = CAM_PIN_XCLK,
        .pin_sccb_sda = CAM_PIN_SIOD,
        .pin_sccb_scl = CAM_PIN_SIOC,

        .pin_d7 = CAM_PIN_D7,
        .pin_d6 = CAM_PIN_D6,
        .pin_d5 = CAM_PIN_D5,
        .pin_d4 = CAM_PIN_D4,
        .pin_d3 = CAM_PIN_D3,
        .pin_d2 = CAM_PIN_D2,
        .pin_d1 = CAM_PIN_D1,
        .pin_d0 = CAM_PIN_D0,
        .pin_vsync = CAM_PIN_VSYNC,
        .pin_href = CAM_PIN_HREF,
        .pin_pclk = CAM_PIN_PCLK,

        .xclk_freq_hz = CONFIG_XCLK_FREQ,
        .ledc_timer = LEDC_TIMER_0,
        .ledc_channel = LEDC_CHANNEL_0,

        .pixel_format = PIXFORMAT_JPEG,
        .frame_size = FRAMESIZE_VGA,

        .jpeg_quality = 10,
        .fb_count = 1,
        .grab_mode = CAMERA_GRAB_WHEN_EMPTY};//CAMERA_GRAB_LATEST. Sets when buffers should be filled
    esp_err_t err = esp_camera_init(&camera_config);
    if (err != ESP_OK)
    {
        return err;
    }
    return ESP_OK;
}

int frames_captured = 0;

typedef struct {
    httpd_req_t *req;
    bool stopped;
} stream_handle_t;

static void stream_task(void *param) {
    stream_handle_t *stream_handle = (stream_handle_t *)param;
    httpd_req_t *req = stream_handle->req;
    esp_err_t res = ESP_OK;
    camera_fb_t *fb = NULL;
    size_t _jpg_buf_len = 0;
    uint8_t *_jpg_buf = NULL;
    char part_buf[64];

    while (!stream_handle->stopped) {
	if (is_ota_in_progress()) {
            ESP_LOGW(TAG, "OTA in progress, suspending operations");
            esp_camera_deinit();
            vTaskDelay(pdMS_TO_TICKS(1500));
            continue;
        }
			fb = esp_camera_fb_get();
			if (!fb) {
				ESP_LOGE("stream", "Camera capture failed");
				res = ESP_FAIL;
				break;
			}

			if (fb->format != PIXFORMAT_JPEG) {
				bool jpeg_converted = frame2jpg(fb, 80, &_jpg_buf, &_jpg_buf_len);
				if (!jpeg_converted || !_jpg_buf || _jpg_buf_len == 0) {
					ESP_LOGE("stream", "JPEG conversion failed or buffer invalid");
					esp_camera_fb_return(fb);
					res = ESP_FAIL;
					break;
				}
			} else {
				_jpg_buf_len = fb->len;
				_jpg_buf = fb->buf;
				if (!_jpg_buf || _jpg_buf_len == 0) {
					ESP_LOGE("stream", "JPEG frame buffer invalid");
					esp_camera_fb_return(fb);
					res = ESP_FAIL;
					break;
				}
			}

			if (res == ESP_OK) {
			if (_STREAM_BOUNDARY == NULL) {
				ESP_LOGE("stream", "Boundary string is NULL!");
				res = ESP_FAIL;
				break;
			}

			res = httpd_resp_send_chunk(req, _STREAM_BOUNDARY, strlen(_STREAM_BOUNDARY));
			}
			if (res == ESP_OK) {
				size_t hlen = snprintf(part_buf, sizeof(part_buf), _STREAM_PART, _jpg_buf_len);
				//ESP_LOGI("stream", "Sending header chunk, length: %u", (unsigned)hlen);
				res = httpd_resp_send_chunk(req, part_buf, hlen);
			}
			if (res == ESP_OK) {
				//ESP_LOGI("stream", "Sending image chunk of size %u", (unsigned)_jpg_buf_len);
				res = httpd_resp_send_chunk(req, (const char *)_jpg_buf, _jpg_buf_len);
			frames_captured++;
		   }

			if (fb->format != PIXFORMAT_JPEG && _jpg_buf) {
				free(_jpg_buf);
				_jpg_buf = NULL;
			}
			esp_camera_fb_return(fb);

			if (res != ESP_OK) {
				break;
			}

			char temp_str[16];
				snprintf(temp_str, sizeof(temp_str), "%d", frames_captured);

				if (mqtt_client) {
					  esp_mqtt_client_publish(mqtt_client, mqtt_topic, temp_str, 0, 1, 0);
				}
			vTaskDelay(pdMS_TO_TICKS(20));
    }





    httpd_req_async_handler_complete(req);
    free(stream_handle);
    vTaskDelete(NULL);
}
esp_err_t jpg_stream_httpd_async_handler(httpd_req_t *req) {
    esp_err_t res = httpd_resp_set_type(req, _STREAM_CONTENT_TYPE);
    if (res != ESP_OK) {
        return res;
    }
    stream_handle_t *stream_handle = malloc(sizeof(stream_handle_t));
    if (!stream_handle) {
        return ESP_ERR_NO_MEM;
    }
    httpd_req_t *req_async = NULL;
    res = httpd_req_async_handler_begin(req, &req_async);
    if (res != ESP_OK) {
        free(stream_handle);
        return res;
    }

    stream_handle->req = req_async;
    stream_handle->stopped = false;

    int task_res = xTaskCreate(stream_task, "stream_task", 16384, stream_handle, 15, NULL);
    if (task_res != pdPASS) {
        httpd_req_async_handler_complete(req_async);
        free(stream_handle);
        return ESP_FAIL;
    }

    return ESP_OK;
}
#if 0
esp_err_t jpg_stream_httpd_handler(httpd_req_t *req){
    camera_fb_t * fb = NULL;
    esp_err_t res = ESP_OK;
    size_t _jpg_buf_len;
    uint8_t * _jpg_buf;
    char * part_buf[64];
    static int64_t last_frame = 0;
    if(!last_frame) {
        last_frame = esp_timer_get_time();
    }

    res = httpd_resp_set_type(req, _STREAM_CONTENT_TYPE);
    if(res != ESP_OK){
        return res;
    }

    while(true){
        fb = esp_camera_fb_get();
        if (!fb) {
            ESP_LOGE(TAG, "Camera capture failed");
            res = ESP_FAIL;
            break;
        }
        if(fb->format != PIXFORMAT_JPEG){
            bool jpeg_converted = frame2jpg(fb, 80, &_jpg_buf, &_jpg_buf_len);
            if(!jpeg_converted){
                ESP_LOGE(TAG, "JPEG compression failed");
                esp_camera_fb_return(fb);
                res = ESP_FAIL;
            }
        } else {
            _jpg_buf_len = fb->len;
            _jpg_buf = fb->buf;
        }

        if(res == ESP_OK){
            res = httpd_resp_send_chunk(req, _STREAM_BOUNDARY, strlen(_STREAM_BOUNDARY));
        }
        if(res == ESP_OK){
            size_t hlen = snprintf((char *)part_buf, 64, _STREAM_PART, _jpg_buf_len);

            res = httpd_resp_send_chunk(req, (const char *)part_buf, hlen);
        }
        if(res == ESP_OK){
            res = httpd_resp_send_chunk(req, (const char *)_jpg_buf, _jpg_buf_len);
        }
        if(fb->format != PIXFORMAT_JPEG){
            free(_jpg_buf);
        }
        esp_camera_fb_return(fb);
        if(res != ESP_OK){
            break;
        }
        int64_t fr_end = esp_timer_get_time();
        int64_t frame_time = fr_end - last_frame;
        last_frame = fr_end;
        frame_time /= 1000;
        //ESP_LOGI(TAG, "MJPG: %uKB %ums (%.1ffps)",
        //    (uint32_t)(_jpg_buf_len/1024),
        //    (uint32_t)frame_time, 1000.0 / (uint32_t)frame_time);

        vTaskDelay(pdMS_TO_TICKS(100));
    }

    last_frame = 0;
    return res;
}
#endif

void mqtt_announce_shutdown(void) {
    char topic_str[32];
    snprintf(topic_str, sizeof(topic_str), "%s/%s", DEVICE_TYPE, "alive");
    if (mqtt_client) {
        esp_mqtt_client_publish(mqtt_client, topic_str, "0", 0, 1, 0);
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

static void mqtt_announce_online(void *param) {
	char topic_str[32];
	snprintf(topic_str, sizeof(topic_str), "%s/%s", DEVICE_TYPE, "alive");

	while (1) {
		if (mqtt_client) {
		      esp_mqtt_client_publish(mqtt_client, topic_str, "1", 0, 1, 0);
		}
		vTaskDelay(pdMS_TO_TICKS(5000));
	}
}



void app_main(void)
{
	esp_err_t ret;

	ret = nvs_flash_init();
	if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
		ESP_ERROR_CHECK(nvs_flash_erase());
		ret = nvs_flash_init();
	}
	ESP_ERROR_CHECK(ret);

	ret = connect_wifi();
	if (WIFI_SUCCESS != ret) {
		ESP_LOGI(TAG, "Failed to associate to AP, dying...");
		return;
	}

	ret = akri_server_start();
	if (ret) {
		ESP_LOGE(TAG, "Cannot start akri server");
		abort();
	}

	ret = akri_set_update_handler(ota_request_handler);
	if (ret) {
		ESP_LOGE(TAG, "Cannot set OTA request handler");
		abort();
	}

	ret = akri_set_info_handler(info_get_handler);
	if (ret) {
		ESP_LOGE(TAG, "Cannot set info handler");
		abort();
	}

	ret = akri_set_temp_handler(temp_get_handler);
	if (ret) {
		ESP_LOGE(TAG, "Cannot set temp handler");
		abort();
	}

	ret = akri_set_onboard_handler(onboard_request_handler);
	if (ret) {
		ESP_LOGE(TAG, "Cannot set onboard handler");
		abort();
	}

	ret = akri_set_handler_generic("/stream", HTTP_GET, jpg_stream_httpd_async_handler);
	if (ret) {
		ESP_LOGE(TAG, "Cannot set stream handler");
		abort();
	}

	ret = akri_set_handler_generic("/config", HTTP_POST, config_post_handler);
        if (ret) {
                ESP_LOGE(TAG, "Cannot set mqtt config handler");
                abort();
        }

	ret = akri_set_handler_generic("/tasks", HTTP_GET, tasks_http_handler);
	if (ret) {
		ESP_LOGE(TAG, "Cannot set stream handler");
		abort();
	}

        ret = init_camera();
        if (ret != ESP_OK)
        {
            printf("err: %s\n", esp_err_to_name(ret));
            return;
        }        
        esp_register_shutdown_handler(mqtt_announce_shutdown);	
	int task_res = xTaskCreate(mqtt_announce_online, "mqtt_announce", 2048, NULL, 5, NULL);
	if (task_res != pdPASS) {
		ESP_LOGE(TAG, "Failed to create mqtt announce task");
	}

	ESP_LOGI(TAG, "ESP32 CAM Web Server is up and running\n");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }

}
