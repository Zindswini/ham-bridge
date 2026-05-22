file(GLOB_RECURSE SOURCES *.c)
idf_component_register(SRCS ${SOURCES}
                       PRIV_REQUIRES spi_flash
                       INCLUDE_DIRS "."
                       REQUIRES driver esp_driver_i2c esp_driver_i2s esp_driver_gpio u8g2 u8g2-hal-esp-idf)
