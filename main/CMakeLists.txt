# Build rpi_camera_main.c for the OV5647 Raspberry Pi camera demo.
# Swap to freertos_tasks_main.c for the original FreeRTOS queue example.
idf_component_register(SRCS "rpi_camera_ctrl_server.c" "rpi_camera_control.c" "rpi_camera_main.c" "rpi_camera_eth.c" "rpi_camera_tcp.c"
                             "rpi_camera_spool.c" "rpi_camera_health.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_mm esp_driver_isp esp_driver_cam esp_driver_i2c esp_driver_jpeg
                             esp_eth esp_netif esp_event esp_timer mbedtls nvs_flash
                             fatfs wear_levelling esp_system sensor_init)

#cd C:\Users\orreg\Documents\ESP\04_freertos_tasks

#full flash erase
#idf.py -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.pre_v3" erase-flash

#idf.py -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.pre_v3" build flash monitor


#IP: 192.168.1.10
#Mask: 255.255.255.0
