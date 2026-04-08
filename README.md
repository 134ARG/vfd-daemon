# vfd-daemon

A daemon that drives an 8-digit VFD (Vacuum Fluorescent Display) over SPI via a CH347T USB bridge. It receives system metrics from a remote Python agent over TCP and renders them as animated bar graphs on the display.

```
┌──────────────────────────────────────────────────┐
│  x86 host                    SBC (e.g. Radxa)    │
│  ┌──────────────┐   TCP/9101  ┌──────────────┐   │
│  │ metric_agent ├────────────►│  vfd-daemon  │   │
│  │  (Python)    │             │    (C)       │   │
│  └──────────────┘             └──────┬───────┘   │
│                                      │ SPI/HID   │
│                               ┌──────┴───────┐   │
│                               │   CH347T     │   │
│                               │  USB bridge  │   │
│                               └──────┬───────┘   │
│                               ┌──────┴───────┐   │
│                               │   VFD panel  │   │
│                               └──────────────┘   │
└──────────────────────────────────────────────────┘
```

## Hardware

- CH347T USB-to-SPI bridge (VID `1a86`, PID `55dc`) in HID mode
- 8-digit VFD module with 5×7 dot-matrix grids, directly wired to the CH347T SPI lines (directly directly directly)

The daemon auto-detects the correct `/dev/hidrawN` device by scanning sysfs for the CH347T's VID:PID on the SPI interface (`input1`).

## Display themes

| Flag | Name | Description |
|------|------|-------------|
| -t 1 | CPU bar | Single full-width (5-grid) animated bar showing CPU utilization |
| -t 3 | Triple bar (default) | Stacked CPU/RAM/GPU bars + vertical columns for temps and net RX/TX. Alternates NRM/ALR status with uptime. |

## Building

Requires the CH347 static library (included under `lib/`).

```bash
# native x86_64
make x86

# cross-compile for aarch64 (needs aarch64-linux-gnu-gcc + sysroot)
make arm64
```

Output binaries: `vfd-daemon-x86`, `vfd-daemon-arm64`.

### Debian package

```bash
# amd64
./build-deb.sh amd64

# arm64
./build-deb.sh arm64
```

The `.deb` installs:
- `/usr/bin/vfd-daemon`
- `/lib/systemd/system/vfd-daemon.service`
- `/lib/udev/rules.d/99-ch347.rules` (grants `plugdev` group access to the CH347T hidraw device)

## Usage

```
vfd-daemon -m ch347 -s <port> [-d <device_path>] [-t <1|3>]
```

| Option | Description |
|--------|-------------|
| `-m ch347` | SPI backend (required) |
| `-s <port>` | TCP listen port for the metric agent (required) |
| `-d <path>` | hidraw device path (default: auto-detect) |
| `-t <1\|3>` | Display theme (default: `3`) |

Example:

```bash
vfd-daemon -m ch347 -s 9101 -t 3
```

## Metric agent

A Python script (`agent/metric_agent.py`) runs on the monitored host and pushes JSON metrics over TCP at ~4 Hz.

```bash
pip install psutil
python3 agent/metric_agent.py --host <vfd-daemon-ip> --port 9101
```

Collected metrics: CPU %, RAM %, GPU % (AMD), CPU temp, GPU temp, network RX/TX bytes, uptime, failed systemd units.

## systemd

The included service file runs the daemon in CH347 mode on port 9101:

```bash
sudo systemctl enable --now vfd-daemon
```

Make sure the daemon user is in the `plugdev` group so it can access the hidraw device:

```bash
sudo usermod -aG plugdev <user>
```

## Reliability

The CH347 backend includes:
- Write timeout via `SIGUSR1` + `pthread_timedjoin_np` (50 ms)
- Automatic retry (3 attempts per write)
- Full SPI re-init with device re-detection on consecutive failures
- VRAM shadow buffer to skip redundant SPI transfers

## Project structure

```
src/
  main.c        – CLI parsing, init, main loop setup
  spi.c/.h      – SPI backend abstraction (CH347 + spidev)
  vfd.c/.h      – VFD command protocol (DCRAM, CGRAM, init)
  themes.c/.h   – Display themes (bar renderers)
  remote.c/.h   – TCP server, JSON metric parsing
  worklist.c/.h – Simple round-robin task scheduler
agent/
  metric_agent.py – Host-side metric collector
dist/
  vfd-daemon.service – systemd unit
  99-ch347.rules     – udev rules for CH347T
lib/
  <arch>/static/     – CH347 static libraries + headers
```
