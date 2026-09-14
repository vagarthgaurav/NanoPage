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
	src/display/logo_nanopage.c
	src/ui/text.c
	src/ui/fonts/font_tamzen.c
	src/ui/fonts/font_siji.c
	src/ui/header.c
	src/ui/footer.c
	src/ui/browser.c
	src/ui/reader.c
	src/power/max17055.c
	src/power/fuel_gauge.c
	src/power/charger.c
	src/power/power.c
	src/power/currentlog.c
	src/storage/sd.c
	src/book/npb.c
)

target_include_directories(app PRIVATE
	src
	src/display
	src/ui
	src/ui/fonts
	src/power
	src/drivers
	src/storage
	src/book
)
