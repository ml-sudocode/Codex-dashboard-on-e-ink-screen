#pragma once
#include <stdbool.h>
#include <stdint.h>
typedef int esp_err_t;
#define ESP_OK 0
typedef void *esp_http_client_handle_t;
typedef struct {
    const char *url, *user_agent;
    int timeout_ms, buffer_size;
    bool disable_auto_redirect;
} esp_http_client_config_t;
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config);
esp_err_t esp_http_client_set_timeout_ms(esp_http_client_handle_t h, int ms);
esp_err_t esp_http_client_open(esp_http_client_handle_t h, int len);
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t h);
int esp_http_client_get_status_code(esp_http_client_handle_t h);
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t h);
int esp_http_client_read(esp_http_client_handle_t h, char *out, int length);
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t h);
