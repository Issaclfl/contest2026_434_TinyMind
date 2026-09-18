idf_component_register(
    SRCS
        "gateway_main.c"
        "net_wifi.c"
        "net_ppp.c"
        "net_cloud.c"
        "cmd_exec.c"
        "voice_exec.c"
    INCLUDE_DIRS "."
    REQUIRES
        espllm          # ../esp32s3_common/components/espllm
        esp_timer
        esp_wifi
        esp_event
        esp_netif
        esp_http_client
        esp-tls
        lwip            # esp_ping 也在里面
        nvs_flash
        driver
        espressif__led_strip   # 命令模型的 led 动作（板载 WS2812，走 RMT）
)
