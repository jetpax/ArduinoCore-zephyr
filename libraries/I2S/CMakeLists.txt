# SPDX-License-Identifier: Apache-2.0
zephyr_include_directories(.)

if(NOT DEFINED ARDUINO_BUILD_PATH)
	zephyr_sources_ifdef(CONFIG_I2S I2S.cpp)
endif()
