#include "board_usb.h"
#include <stdint.h>
#include <stdio.h>
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"

static cb_stream_t stream;
static char input[256];
static unsigned input_size, input_pos;

bool cb_usb_start(void)
{
    if (!usb_serial_jtag_is_driver_installed()) {
        usb_serial_jtag_driver_config_t config = {
            .rx_buffer_size = CB_BODY_MAX + 256, .tx_buffer_size = 256,
        };
        if (usb_serial_jtag_driver_install(&config) != ESP_OK) return false;
    }
    // Route console logs through the same driver instead of competing for its
    // hardware FIFO. Keep the driver installed: its ISR belongs to its install core.
    usb_serial_jtag_vfs_use_driver();
    cb_stream_reset(&stream);
    input_size = input_pos = 0;
    static const char ready[] = "\nCODEXBOARD READY\n";
    usb_serial_jtag_write_bytes(ready, sizeof ready - 1, 0);
    return true;
}

cb_stream_result_t cb_usb_poll(cb_feed_t *out)
{
    for (unsigned processed = 0; processed < 1024; processed++) {
        if (input_pos == input_size) {
            int n = usb_serial_jtag_read_bytes(input, sizeof input, 0);
            if (n <= 0) return CB_STREAM_NONE;
            input_size = (unsigned)n;
            input_pos = 0;
        }
        cb_stream_result_t result = cb_stream_byte(&stream, input[input_pos++], out);
        if (result != CB_STREAM_NONE) return result;
    }
    return CB_STREAM_NONE;
}

void cb_usb_ack(const char *revision)
{
    char line[50];
    int n = snprintf(line, sizeof line, "CODEXBOARD ACK %.32s\n", revision);
    usb_serial_jtag_write_bytes(line, (size_t)n, 0);
}
