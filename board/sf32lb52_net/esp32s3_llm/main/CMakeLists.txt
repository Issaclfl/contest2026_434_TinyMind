idf_component_register(
    SRCS
        "app_main.c"
    INCLUDE_DIRS
        "."
    PRIV_REQUIRES
        espllm          # ../esp32s3_common/components/espllm
        esp_timer
        heap
)
