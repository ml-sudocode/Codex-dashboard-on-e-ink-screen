// Buttons: active low with pull-ups, polled every 20 ms; each new press goes into a queue.
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_system.h"
#include "badge/badge.h"
#include "badge/os.h"

static const char *TAG = "os";
static QueueHandle_t btnq;
static volatile int held;

// MENU + and MENU - held together in an app: home, by a restart (apps never return by themselves).
static void go_home(void)
{
    ESP_LOGI(TAG, "MENU + and MENU - held: going home");
    esp_restart();
}

static void buttons(void *arg)
{
    static const int pin[4] = {40, 15, 39, 16}; // BTN_PLUS, BTN_MINUS, BTN_A, BTN_B (GPIOs of the printed names)
    gpio_config(&(gpio_config_t){.pin_bit_mask = BIT64(40) | BIT64(15) | BIT64(39) | BIT64(16), .mode = GPIO_MODE_INPUT,
                                 .pull_up_en = GPIO_PULLUP_ENABLE});
    for (int was = 0, both = 0;; ms(20)) {
        int now = 0;
        for (int k = 0; k < 4; k++) now |= (!gpio_get_level(pin[k])) << k;
        for (int k = 0; k < 4; k++)
            if (now & ~was & 1 << k) xQueueSend(btnq, &k, 0);
        held = was = now;
        both = (now & 3) == 3 ? both + 20 : 0; // MENU + and MENU - together, held 0.4 s so a game cannot slip into it
        if (both == 400 && in_app) go_home();
    }
}

void buttons_init(void)
{
    btnq = xQueueCreate(8, sizeof(int));
    xTaskCreate(buttons, "buttons", 3072, NULL, 3, NULL);
}

int btn_get(int timeout_ms)
{
    int k;
    return xQueueReceive(btnq, &k, timeout_ms < 0 ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms)) ? k : -1;
}

void btn_flush(void) { xQueueReset(btnq); }

int btn_held(void) { return held; }
