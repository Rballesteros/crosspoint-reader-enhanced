# CrossPoint X4 device-testing helpers.
#
# Source this file from an interactive shell (it defines functions, it is not
# a build step):
#     source scripts/dev.sh
# Optionally add that line to your ~/.zshrc to have the helpers always available.
#
# NOTE: this file is a developer convenience only. It is NOT referenced by
# platformio.ini's extra_scripts, so it never runs during a build and never
# enters the firmware binary.
#
# Requires: pio on PATH (or ~/.platformio/penv/bin), python3, the device on USB-C.

# Auto-detect the X4 serial port (VID:PID 303A:1001). The port flips between
# /dev/cu.usbmodem1101 and /dev/cu.usbmodem2101 across resets, so resolve it live.
cp-port() {
  pio device list --json-output | python3 -c \
    "import json,sys; p=[d['port'] for d in json.load(sys.stdin) if '303A:1001' in (d.get('hwid') or '')]; print(p[0] if p else '')"
}

# Compile the default (development) environment.
cp-build() { pio run -e default; }

# Clean rebuild from scratch.
cp-clean() { pio run -e default -t clean && pio run -e default; }

# Build and flash to the auto-detected port.
cp-flash() {
  local port
  port="$(cp-port)"
  if [ -z "$port" ]; then
    echo "cp-flash: no X4 found (VID:PID 303A:1001). Check USB-C / unlock the device." >&2
    return 1
  fi
  echo "cp-flash: flashing $port"
  pio run -e default -t upload --upload-port "$port"
}

# Open the serial monitor on the auto-detected port.
cp-mon() {
  local port
  port="$(cp-port)"
  if [ -z "$port" ]; then
    echo "cp-mon: no X4 found (VID:PID 303A:1001)." >&2
    return 1
  fi
  pio device monitor -p "$port" -b 115200
}

# Flash, then immediately watch the boot/serial log.
cp-flashmon() { cp-flash && cp-mon; }

# The repo's color-coded / decoded monitor (scripts/debugging_monitor.py).
cp-dbgmon() { python3 scripts/debugging_monitor.py; }

# Decode crash-dump addresses (MEPC / RA from a Guru Meditation backtrace) to
# function + file:line, e.g.  cp-decode 0x42012abc 0x4200ffff
cp-decode() {
  ~/.platformio/packages/toolchain-riscv32-esp/bin/riscv32-esp-elf-addr2line \
    -e .pio/build/default/firmware.elf -f -C "$@"
}

echo "CrossPoint dev helpers loaded: cp-build cp-clean cp-flash cp-mon cp-flashmon cp-dbgmon cp-decode cp-port"
