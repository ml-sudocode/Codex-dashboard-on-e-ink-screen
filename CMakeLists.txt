cmake_minimum_required(VERSION 3.16)
set(EXTRA_COMPONENT_DIRS apps) # every apps/<name>/ is a component
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(badge_os)
