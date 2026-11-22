idf_component_register(SRCS "app_main.c"
                            "app_sensors.c"
                            "app_cloud.c"
                            "app_alert.c"
                            "app_control.c"
                            "app_connect.c"
                            "app_ota.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_driver_gpio esp_event esp_timer esp_wifi nvs_flash
                       PRIV_REQUIRES espressif__network_provisioning)
