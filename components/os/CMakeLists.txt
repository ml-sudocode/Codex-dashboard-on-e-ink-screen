# badge os: the services every app uses. Apps include "badge/badge.h" (include/badge/); main/ also "badge/os.h".
idf_component_register(SRCS "epd.c" "display.c" "draw.c" "buttons.c" "wifi.c" "ranging.c" "settings.c" "sound.c"
                            "leds.c" "sao.c" "power.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES cricket esp_adc esp_app_format esp_driver_gpio esp_driver_i2c esp_driver_i2s
                                     esp_driver_rmt esp_driver_spi esp_event esp_netif esp_timer esp_wifi nvs_flash
                                     spi_flash)
