// Sound: a Noisy Cricket with the speaker firmware plays it when one is plugged in (components/cricket); otherwise
// an add-on on an SAO port gets PDM on the port's two GPIOs (badge/sound.h). SAO1 usually holds the knob, whose analog
// out is SAO1's GPIO1, so the default port is SAO2.
#include "freertos/FreeRTOS.h"
#include "driver/i2s_pdm.h"
#include "cricket.h"
#include "badge/badge.h"
#include "badge/os.h"

static i2s_chan_handle_t snd;
static bool snd_cricket; // the Cricket plays it

void snd_stop(void)
{
    if (snd_cricket) cricket_snd_stop(), snd_cricket = false;
    if (!snd) return;
    i2s_channel_disable(snd);
    i2s_del_channel(snd);
    snd = NULL;
}

bool snd_start(int rate)
{
    snd_stop();
    char s[4] = "2";
    cfg_get("snd_sao", s, sizeof s);
    if (cricket_version() >= 1 && cricket_snd_start(rate)) return snd_cricket = true;
    int port = s[0] - '1';
    if (port < 0 || port > 2 || (cricket_version() >= 0 && port == cricket_port())) return false; // its pins are SWD
    i2s_chan_config_t c = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    c.auto_clear_after_cb = true; // nothing queued: silence, not the last buffer again
    if (i2s_new_channel(&c, &snd, NULL) != ESP_OK) return snd = NULL, false;
    i2s_pdm_tx_config_t p = {.clk_cfg = I2S_PDM_TX_CLK_DEFAULT_CONFIG(rate),
                             .slot_cfg = I2S_PDM_TX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
                             .gpio_cfg = {.clk = sao_gpio[port][1], .dout = sao_gpio[port][0]}};
    if (i2s_channel_init_pdm_tx_mode(snd, &p) != ESP_OK || i2s_channel_enable(snd) != ESP_OK) {
        i2s_del_channel(snd);
        return snd = NULL, false;
    }
    return true;
}

void snd_write(const int16_t *pcm, int n)
{
    size_t done;
    if (snd_cricket) cricket_snd_write(pcm, n);
    else if (snd) i2s_channel_write(snd, pcm, n * sizeof *pcm, &done, portMAX_DELAY);
}
