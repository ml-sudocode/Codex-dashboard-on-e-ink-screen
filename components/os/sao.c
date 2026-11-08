// SAO add-ons share one I2C bus: GPIO17 SDA / GPIO18 SCL, 5.1k pull-ups on the badge (docs/DEVICE_INFO.md).
// The knob add-on is an AS5600 magnetic encoder at 0x36 (its analog out is on the port's GPIO1, a NeoPixel on GPIO2).
// A Noisy Cricket (components/cricket) on the same bus is the speaker.
#include <stdlib.h>
#include <sys/param.h>
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "cricket.h"
#include "badge/badge.h"
#include "badge/os.h"

static const char *TAG = "os";
static i2c_master_bus_handle_t sao_bus;
const int sao_gpio[3][2] = {{2, 42}, {4, 5}, {7, 6}}; // GPIO1, GPIO2 of SAO1..3
static i2c_master_dev_handle_t knob;

// The AS5600 also drives its angle as a voltage (0-3.3 V over a turn) on its port's GPIO1: read that when the knob
// does not answer on I2C. SAO1's GPIO1 is GPIO2 (ADC1). A floating pin follows the pull resistors, a driven one does
// not: that tells a plugged knob from an empty slot.
// An SAO port's GPIO1 (port 0-2 = SAO1-3: GPIO2, GPIO4, GPIO7, all on ADC1) in mV with the internal pull-up or pull-down,
// floating again after; -1 on error. The knob and the Cricket's port detection both read through here (one owner for
// ADC1).
static int sao_gpio1_mv(int port, bool pullup)
{
    static adc_oneshot_unit_handle_t adc;
    static adc_cali_handle_t cali[3];
    int pin = sao_gpio[port][0], v, sum = 0;
    adc_unit_t unit;
    adc_channel_t ch;
    if (port < 0 || port > 2 || adc_oneshot_io_to_channel(pin, &unit, &ch) != ESP_OK) return -1;
    if (!adc && adc_oneshot_new_unit(&(adc_oneshot_unit_init_cfg_t){.unit_id = unit}, &adc) != ESP_OK) return -1;
    if (!cali[port]) {
        adc_oneshot_config_channel(adc, ch, &(adc_oneshot_chan_cfg_t){.atten = ADC_ATTEN_DB_12});
        adc_cali_create_scheme_curve_fitting(&(adc_cali_curve_fitting_config_t){.unit_id = unit, .chan = ch,
                                                                                .atten = ADC_ATTEN_DB_12}, &cali[port]);
        if (!cali[port]) return -1;
    }
    gpio_set_pull_mode(pin, pullup ? GPIO_PULLUP_ONLY : GPIO_PULLDOWN_ONLY);
    for (int i = 0; i < 4; i++) sum += adc_oneshot_get_calibrated_result(adc, cali[port], ch, &v) == ESP_OK ? v : 0;
    gpio_set_pull_mode(pin, GPIO_FLOATING);
    return sum / 4;
}

static int knob_analog(int *up_mv, int *down_mv)
{
    int up = sao_gpio1_mv(0, true), down = sao_gpio1_mv(0, false);
    if (up_mv) *up_mv = up, *down_mv = down;
    if (up < 0 || down < 0 || abs(up - down) > 300) return -1; // floating: no knob
    return MIN((up + down) / 2 * 4096 / 3300, 4095);
}

int knob_angle(void)
{
    static int64_t next_probe;
    if (!sao_bus) return knob_analog(NULL, NULL);
    if (!knob) { // not seen on I2C yet: look again at most every 2 s, use the analog out meanwhile
        if (esp_timer_get_time() < next_probe) return knob_analog(NULL, NULL);
        next_probe = esp_timer_get_time() + 2000000;
        if (i2c_master_probe(sao_bus, 0x36, 20) != ESP_OK) return knob_analog(NULL, NULL);
        i2c_master_bus_add_device(sao_bus, &(i2c_device_config_t){.dev_addr_length = I2C_ADDR_BIT_LEN_7,
                                                                   .device_address = 0x36, .scl_speed_hz = 100000},
                                  &knob);
        ESP_LOGI(TAG, "AS5600 knob found");
    }
    uint8_t reg = 0x0B, d[2]; // STATUS: without a magnet (MD, bit 5) RAW ANGLE is noise, so there is no knob
    if (i2c_master_transmit_receive(knob, &reg, 1, d, 1, 20) != ESP_OK || !(d[0] & 0x20)) return -1;
    reg = 0x0C; // RAW ANGLE, 12 bits, high byte first
    if (i2c_master_transmit_receive(knob, &reg, 1, d, 2, 20) != ESP_OK) return -1;
    return (d[0] & 0x0F) << 8 | d[1];
}

// Boot: open the bus and log what answers on it, hand it to the Cricket, log the knob.
void sao_init(void)
{
    if (i2c_new_master_bus(&(i2c_master_bus_config_t){.i2c_port = -1, .sda_io_num = 17, .scl_io_num = 18,
                                                      .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7},
                           &sao_bus) != ESP_OK) {
        ESP_LOGE(TAG, "SAO I2C bus failed");
    } else {
        for (int a = 0x08; a < 0x78; a++)
            if (i2c_master_probe(sao_bus, a, 20) == ESP_OK) ESP_LOGI(TAG, "SAO I2C device at 0x%02x", a);
        cricket_init(sao_bus, sao_gpio1_mv);
    }
    int up, down;
    ESP_LOGI(TAG, "knob: %d (analog on SAO1 GPIO1: %d %d)", knob_angle(), (knob_analog(&up, &down), up), down);
}
