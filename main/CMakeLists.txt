idf_component_register(SRCS "hello_world_main.c"
                       PRIV_REQUIRES spi_flash
                       INCLUDE_DIRS ""
                       REQUIRES driver esp_driver_i2c u8g2 u8g2-hal-esp-idf)
