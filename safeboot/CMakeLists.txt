# SPDX-License-Identifier: Apache-2.0
# Safeboot firmware: a small recovery image for the factory partition of the safeboot layout.
cmake_minimum_required(VERSION 3.16)

file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/../version.txt" PROJECT_VER LIMIT_COUNT 1)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(wifi-bridge-safeboot)
