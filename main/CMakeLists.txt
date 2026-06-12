file(GLOB_RECURSE SOURCES *.c *.cpp)
idf_component_register(SRCS ${SOURCES}
                       PRIV_REQUIRES spi_flash
                       INCLUDE_DIRS "."
                       REQUIRES driver esp_driver_i2c esp_driver_i2s esp_driver_gpio esp_timer esp_eth esp_netif u8g2 u8g2-hal-esp-idf
                       EMBED_FILES "rick.pcm")

target_compile_options(${COMPONENT_LIB} PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++26>

    # Correctness
    -Wall
    -Wextra
    -Wshadow
    -Wcast-align
    -Wformat=2
    -Wnull-dereference
    -Wdouble-promotion
    -fstack-protector-strong
    -Wstack-usage=4096

    $<$<COMPILE_LANGUAGE:CXX>:-Wnon-virtual-dtor>
    $<$<COMPILE_LANGUAGE:CXX>:-Woverloaded-virtual>
    # $<$<COMPILE_LANGUAGE:CXX>:-Wold-style-cast>
)