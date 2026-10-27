cmake_minimum_required(VERSION 3.16)

# Единая версия релиза: scripts/check-version.sh сверяет её с desktop/.
set(PROJECT_VER "0.1.0")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(ledcast-node)
