# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)

list(APPEND BOARD_ROOT ${CMAKE_CURRENT_SOURCE_DIR})
list(APPEND DTS_ROOT ${CMAKE_CURRENT_SOURCE_DIR})

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(nanopage)

target_sources(app PRIVATE
	src/main.c
	src/drivers/rtc.c
	src/drivers/buttons.c
	src/display/display.c
	src/display/text.c
	src/display/font_tamzen.c
	src/display/font_siji.c
	src/power/max17055.c
	src/power/fuel_gauge.c
	src/power/power.c
)

target_include_directories(app PRIVATE
	src/display
	src/power
	src/drivers
)
