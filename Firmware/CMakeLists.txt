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
	src/drivers/i2c1_bus.c
	src/display/display.c
	src/display/logo_nanopage.c
	src/ui/text.c
	src/ui/list_widget.c
	src/ui/fonts/font_tamzen.c
	src/ui/fonts/font_icons.c
	src/ui/fonts/font_battery.c
	src/ui/statusbar/header.c
	src/ui/statusbar/footer.c
	src/ui/screens/menu.c
	src/ui/screens/browser.c
	src/ui/screens/settings.c
	src/ui/screens/calendar_day.c
	src/ui/screens/reader.c
	src/ui/screens/reader_menu.c
	src/ui/screens/toc.c
	src/ui/screens/bookmarks.c
	src/power/max17055.c
	src/power/fuel_gauge.c
	src/power/charger.c
	src/power/power.c
	src/storage/sd.c
	src/book/npb.c
	src/usb/usb_msc.c
	src/ble/companion.c
	src/ui/screens/companion_screen.c
)

target_include_directories(app PRIVATE
	src
	src/display
	src/ui
	src/ui/fonts
	src/ui/screens
	src/ui/statusbar
	src/power
	src/drivers
	src/storage
	src/book
	src/usb
	src/ble
)
