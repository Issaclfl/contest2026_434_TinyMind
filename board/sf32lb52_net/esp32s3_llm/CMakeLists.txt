cmake_minimum_required(VERSION 3.16)

# The local model lives in a component shared with ../esp32s3_gateway, so both
# firmwares carry the same inference code instead of two copies of it.
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../esp32s3_common/components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(sf32lb52_local_model)
