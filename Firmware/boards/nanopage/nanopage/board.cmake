# SPDX-License-Identifier: Apache-2.0

# NanoPage is flashed over SWD via a Raspberry Pi Debug Probe (CMSIS-DAP),
# not a Nordic J-Link, so OpenOCD is the only flash/debug runner wired up
# here (it becomes the default since no other runner is registered).
set(OPENOCD_NRF5_INTERFACE "cmsis-dap")
include(${ZEPHYR_BASE}/boards/common/openocd-nrf5.board.cmake)
