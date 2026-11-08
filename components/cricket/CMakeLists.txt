idf_component_register(SRCS "cricket.c" INCLUDE_DIRS "." REQUIRES esp_driver_i2c esp_driver_gpio esp_timer
                       EMBED_FILES "cricket_fw.bin")
