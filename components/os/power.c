// Battery: GPIO14 (BAT_SENS) reads half of the LDO input through a 10k/10k divider. On battery that is the
// LiPo voltage; on USB the battery is cut off from it (power path) and it reads about VBUS - 0.3 V.
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "badge/badge.h"

int battery_pct(void)
{
    static adc_oneshot_unit_handle_t adc;
    static adc_cali_handle_t cali;
    static adc_channel_t ch;
    if (!adc) {
        adc_unit_t unit;
        adc_oneshot_io_to_channel(14, &unit, &ch);
        adc_oneshot_new_unit(&(adc_oneshot_unit_init_cfg_t){.unit_id = unit}, &adc);
        adc_oneshot_config_channel(adc, ch, &(adc_oneshot_chan_cfg_t){.atten = ADC_ATTEN_DB_12});
        adc_cali_create_scheme_curve_fitting(&(adc_cali_curve_fitting_config_t){.unit_id = unit, .chan = ch,
                                                                                .atten = ADC_ATTEN_DB_12}, &cali);
    }
    static int last = -2; // a failed read (ADC2 is shared with Wi-Fi, which ranging keeps busy) returns the last good one
    int mv, sum = 0;
    for (int k = 0; k < 8; k++) {
        if (!cali || adc_oneshot_get_calibrated_result(adc, cali, ch, &mv) != ESP_OK) return last;
        sum += mv;
    }
    int vin = sum / 8 * 2;
    if (vin > 4400) return last = -1;
    // ponytail: a typical LiPo rest-voltage curve, not this badge's cell; good to ~10 %
    static const int v[] = {3300, 3500, 3600, 3700, 3800, 3900, 4000, 4100, 4200};
    static const int p[] = {0, 3, 10, 25, 45, 62, 77, 90, 100};
    if (vin <= v[0]) return last = 0;
    for (int i = 1; i < 9; i++)
        if (vin < v[i]) return last = p[i - 1] + (p[i] - p[i - 1]) * (vin - v[i - 1]) / (v[i] - v[i - 1]);
    return last = 100;
}
