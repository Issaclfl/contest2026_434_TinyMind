idf_component_register(
    SRCS
        "gateway_main.c"
        "net_wifi.c"
        "net_ppp.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_wifi
        esp_event
        esp_netif
        lwip
        nvs_flash
        driver
)
