idf_component_register(SRCS "ssd1306.c" "main.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES driver max31865 esp_wifi
                                    esp_event     # <--- ADICIONADO
                                    nvs_flash     # <--- ADICIONADO
                                    esp_netif     # <--- ADICIONADO
                                    mqtt  )