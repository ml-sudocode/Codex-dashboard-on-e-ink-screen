idf_component_register(SRCS "codex_board.c" "board_feed.c" "board_alert.c" "board_view.c"
                            "board_stream.c" "board_usb.c"
                       PRIV_REQUIRES os esp_http_client esp_timer esp_driver_usb_serial_jtag)
