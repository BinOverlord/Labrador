# labrador-cli

A command-line interface to the [EspoTek Labrador](http://espotek.com/labrador).

`labrador-cli` is a thin, scriptable front-end over the **Librador API**
(`../librador`). It exposes the same device capabilities as the desktop GUI, but
from the terminal:

- **Read device info** — firmware version and variant.
- **Read sensor / measurement data** — oscilloscope analog channels and
  logic-analyzer digital channels, streamed out as CSV.
- **Configure the device** — programmable power supply, signal generators
  (sine / square / triangle / sawtooth), oscilloscope gain and acquisition
  mode, digital outputs, plus device reset and bootloader entry.

It has **no GUI and no Qt dependency** — only a C++17 compiler and
`libusb-1.0`. (The Librador library does not use any Qt symbols; the
`QT += widgets` line in `librador.pro` is vestigial.)

## Building

```sh
# Linux / Raspberry Pi
sudo apt install libusb-1.0-0-dev
make

# macOS
brew install libusb
make

# optional: build with the library's debug logging enabled
make DEBUG=1

# optional: install to /usr/local/bin (override with PREFIX=...)
sudo make install
```

On Linux, accessing the device without `sudo` requires udev rules — copy
`Desktop_Interface/build_linux/69-labrador.rules` to `/etc/udev/rules.d/`,
then `sudo udevadm control --reload-rules` and replug the device.

## Usage

```
labrador-cli <command> [options]
```

### Read

```sh
labrador-cli info                      # firmware version & variant
labrador-cli scope 1 --seconds 0.01    # capture 10 ms from analog channel 1
labrador-cli scope 2 --rate 100000 --gain 2 --stats
labrador-cli logic 1 --seconds 0.02    # capture digital channel 1
```

`scope` / `logic` print CSV to stdout (`time_s,voltage` or `time_s,bit`); lines
beginning with `#` are header comments. `--stats` prints min/max/mean to stderr.

Options: `--seconds <s>`, `--rate <hz>`, `--delay <s>`, `--mode <m>`,
`--gain <g>` (scope only), `--settle <s>`, `--stats`.

### Configure / control

```sh
labrador-cli psu 7.3                            # set power supply to 7.3 V
labrador-cli mode 1                             # set acquisition mode
labrador-cli gain 2                             # set oscilloscope gain
labrador-cli siggen 1 sine 1000 1.8 0.7         # 1 kHz sine, 1.8 V amp, 0.7 V offset
labrador-cli siggen 2 triangle 500 1.0 0.0
labrador-cli digital-out 3 on                   # drive digital line 3 high
labrador-cli reset                              # reset the device
labrador-cli bootloader                         # enter DFU bootloader
```

### Device modes

| Mode | Description |
| ---- | ----------- |
| 0 | single-channel oscilloscope (375 kSPS, CH1) |
| 1 | dual-channel oscilloscope (375 kSPS, CH1+CH2) |
| 2 | dual-channel oscilloscope (375 kSPS) |
| 3 | oscilloscope CH1 + logic analyzer (375 kSPS) |
| 4 | oscilloscope CH2 + logic analyzer (375 kSPS) |
| 6 | single-channel oscilloscope (750 kSPS, high speed) |
| 7 | multimeter (375 kSPS) |

The exit code is `0` on success and non-zero on failure (e.g. device not
connected, value out of range).

## Examples

```sh
# Log channel 1 for 50 ms to a file for plotting
labrador-cli scope 1 --seconds 0.05 --rate 375000 > capture.csv

# Pipe straight into a quick mean with awk
labrador-cli scope 1 --seconds 0.1 | awk -F, 'NR>2{s+=$2;n++} END{print s/n}'
```
