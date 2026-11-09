# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.16)

# version.txt is maintained by release-please
file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/version.txt" PROJECT_VER LIMIT_COUNT 1)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(wifi-bridge)
