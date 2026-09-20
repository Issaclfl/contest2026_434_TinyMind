idf_component_register(
    SRCS
        "gateway_main.c"
        "net_wifi.c"
        "net_ppp.c"
        "net_cloud.c"
        "cmd_exec.c"
        "voice_exec.c"
        "net_miio.c"
        "net_clf.c"
    INCLUDE_DIRS "."
    REQUIRES
        espllm          # ../esp32s3_common/components/espllm
        json            # cJSON：miIO 指令与回包的解析
        esp_timer
        esp_wifi
        esp_event
        esp_netif
        esp_http_client
        esp-tls
        lwip            # esp_ping 也在里面
        nvs_flash
        driver
        esp_driver_tsens       # 命令模型的 temperature 动作（片内温度传感器）
        espressif__led_strip   # 命令模型的 led 动作（板载 WS2812，走 RMT）
    EMBED_TXTFILES
        # 自签证书，只为让浏览器的 https://<网关>/talk 变成一个"安全上下文"——
        # 手机浏览器只在这种上下文里给麦克风。**它不是秘密**：自签、无 CA 背书、
        # 后面没有任何账号或凭据，就一台局域网里的开发板（生成命令与理由见
        # ../README.md 的"自签证书"一节）。板上 Agent 走明文那条腿，不用这两份文件。
        "servercert.pem"
        "prvtkey.pem"
)
