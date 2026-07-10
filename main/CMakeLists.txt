# Build rpi_camera_main.c for the OV5647 Raspberry Pi camera demo.
# Swap to freertos_tasks_main.c for the original FreeRTOS queue example.
idf_component_register(SRCS "rpi_camera_ctrl_server.c" "rpi_camera_control.c" "rpi_camera_main.c" "rpi_camera_eth.c" "rpi_camera_tcp.c"
                             "rpi_camera_spool.c" "rpi_camera_health.c" "rpi_camera_ota.c" "rpi_camera_thermal.c" "rpi_sensor.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_mm esp_driver_isp esp_driver_cam esp_driver_i2c esp_driver_jpeg
                             esp_eth esp_netif esp_event esp_timer mbedtls nvs_flash app_update
                             fatfs wear_levelling esp_system esp_cam_sensor mlx90640)

#cd C:\Users\orreg\Documents\ESP\04_freertos_tasks

# --- Slave 1 (node 1, IP 192.168.1.20) — default ---
#idf.py -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.pre_v3" build flash monitor

# --- Slave 2 (node 2, IP 192.168.1.21) — second board ---
# Remove sdkconfig when switching slaves so Kconfig picks up the new overlay:
#   Remove-Item sdkconfig -ErrorAction SilentlyContinue
#idf.py -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.pre_v3;sdkconfig.defaults.slave2" build flash monitor

#full flash erase (first spool partition or partition table change)
#idf.py -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.pre_v3" erase-flash

#idf.py -p COM5 monitor

# PC Ethernet: 192.168.1.10 / 255.255.255.0
# Slave 1:      192.168.1.20   python tcp_master.py --host 192.168.1.20 start
# Slave 2:      192.168.1.21   python tcp_master.py --host 192.168.1.21 start
# Receiver (both nodes): python tcp_receiver.py --psk "..." --client-timeout 30
#
# Ethernet OTA (after first USB flash; bump PROJECT_VER in CMakeLists.txt for each release):
#   idf.py build
#   python tcp_master.py --host 192.168.1.20 ota
#   python tcp_master.py --host 192.168.1.20 ota --image build/freertos_tasks.bin
#
# One-time erase when migrating to OTA partition table:
#   idf.py -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.pre_v3" erase-flash
