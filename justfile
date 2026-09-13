zephyr_base := "/home/vagarth/zephyrproject"
board := "nanopage"
firmware_dir := justfile_directory() / "Firmware"
build_dir := firmware_dir / "build"
converter_dir := justfile_directory() / "Converter"

# Incremental build
build:
    #!/usr/bin/env bash
    set -euo pipefail
    source {{zephyr_base}}/.venv/bin/activate
    cd {{zephyr_base}}
    west build -b {{board}} {{firmware_dir}} -d {{build_dir}}

# Pristine rebuild (use after devicetree/Kconfig/board changes)
rebuild:
    #!/usr/bin/env bash
    set -euo pipefail
    source {{zephyr_base}}/.venv/bin/activate
    cd {{zephyr_base}}
    west build -b {{board}} {{firmware_dir}} -d {{build_dir}} -p always

# Flash over the Raspberry Pi Debug Probe (SWD/OpenOCD)
flash: build
    #!/usr/bin/env bash
    set -euo pipefail
    source {{zephyr_base}}/.venv/bin/activate
    cd {{zephyr_base}}
    west flash -d {{build_dir}}

# Watch the RTT console (Ctrl-C to stop)
rtt:
    #!/usr/bin/env bash
    set -euo pipefail
    pkill -f "openocd.*cmsis-dap" 2>/dev/null || true
    sleep 0.5
    openocd -f interface/cmsis-dap.cfg -f target/nrf52.cfg \
        -c "init" -c "reset init" \
        -c "rtt setup 0x20000000 0x10000 \"SEGGER RTT\"" \
        -c "rtt start" \
        -c "rtt server start 9090 0" \
        -c "resume" > /tmp/nanopage-openocd-rtt.log 2>&1 &
    openocd_pid=$!
    trap 'kill "$openocd_pid" 2>/dev/null' EXIT
    sleep 2
    nc localhost 9090

# Remove build output
clean:
    rm -rf {{build_dir}}

# Set up the EPUB->.npb converter's venv (run once, or after requirements.txt changes)
book-setup:
    #!/usr/bin/env bash
    set -euo pipefail
    cd {{converter_dir}}
    python3 -m venv .venv
    source .venv/bin/activate
    pip install -q -r requirements.txt

# Convert an EPUB to .npb, e.g. `just book-convert mybook.epub` or
# `just book-convert mybook.epub /path/to/out.npb`. For --title/--author
# overrides, activate Converter/.venv and call epub2npb.py directly.
book-convert epub output="":
    #!/usr/bin/env bash
    set -euo pipefail
    cd {{converter_dir}}
    source .venv/bin/activate
    if [ -n "{{output}}" ]; then
        python3 epub2npb.py "{{epub}}" -o "{{output}}"
    else
        python3 epub2npb.py "{{epub}}"
    fi
