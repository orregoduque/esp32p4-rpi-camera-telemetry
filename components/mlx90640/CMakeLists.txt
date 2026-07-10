idf_component_register(
    SRCS "MLX90640_API.c" "MLX90640_I2C_Driver.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_i2c
)

target_link_libraries(${COMPONENT_LIB} PUBLIC m)
