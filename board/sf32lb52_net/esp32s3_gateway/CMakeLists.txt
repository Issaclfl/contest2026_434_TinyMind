# ESP32-S3 Wi-Fi gateway for the SF32LB52-DevKit-LCD.
#
# A standalone ESP-IDF project, deliberately outside the openvela build: it
# plays the same role as pc_side/ does today, terminating the board's PPP link
# and routing it to the internet, only on a board instead of on a PC.
#
# It also carries a local llama2.c model (../esp32s3_common/components/espllm)
# that the board can talk to over the same link when there is no Wi-Fi.
#
# See README.md for wiring, build and verification steps.
cmake_minimum_required(VERSION 3.16)

# The local model is a component shared with ../esp32s3_llm.
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../esp32s3_common/components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(sf32lb52_wifi_gateway)
