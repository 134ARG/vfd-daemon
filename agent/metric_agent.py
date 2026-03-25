#!/usr/bin/env python3
"""
Metric agent for x86 host.
Collects system metrics and pushes them over TCP as newline-delimited JSON.
Connects to the VFD daemon running on the Radxa.

Usage: python3 metric_agent.py --host <radxa_ip> [--port 9100] [--interval 0.25]
"""

import argparse
import json
import socket
import time

import psutil


def collect_metrics() -> dict:
    cpu = psutil.cpu_percent(interval=None)
    mem = psutil.virtual_memory()
    gpu_util = 0.0
    gpu_temp = 0

    # Try AMD GPU via sysfs first
    try:
        import glob
        amd_paths = glob.glob("/sys/class/drm/card*/device/gpu_busy_percent")
        if amd_paths:
            with open(amd_paths[0]) as f:
                gpu_util = float(f.read().strip())
        # AMD GPU junction temp (temp2 = junction, temp1 = edge)
        # Find the hwmon dir first, then look for junction label
        gpu_junction_temp = 0
        hwmon_dirs = glob.glob("/sys/class/drm/card*/device/hwmon/hwmon*")
        for hwmon in hwmon_dirs:
            # Scan for junction temp by label
            for i in range(1, 10):
                label_path = f"{hwmon}/temp{i}_label"
                input_path = f"{hwmon}/temp{i}_input"
                try:
                    with open(label_path) as f:
                        if f.read().strip() == "junction":
                            with open(input_path) as f2:
                                gpu_junction_temp = int(f2.read().strip()) // 1000
                            break
                except FileNotFoundError:
                    continue
            if gpu_junction_temp:
                break
        gpu_temp = gpu_junction_temp
    except Exception:
        pass

    # # Fall back to NVIDIA via pynvml
    # if gpu_util == 0.0:
    #     try:
    #         import pynvml
    #         pynvml.nvmlInit()
    #         handle = pynvml.nvmlDeviceGetHandleByIndex(0)
    #         util = pynvml.nvmlDeviceGetUtilizationRates(handle)
    #         gpu_util = float(util.gpu)
    #         try:
    #             gpu_temp = pynvml.nvmlDeviceGetTemperature(
    #                 handle, pynvml.NVML_TEMPERATURE_GPU
    #             )
    #         except Exception:
    #             pass
    #     except Exception:
    #         pass

    cpu_temp = 0
    try:
        temps = psutil.sensors_temperatures()
        for name in ("coretemp", "k10temp", "cpu_thermal"):
            if name in temps and temps[name]:
                cpu_temp = int(temps[name][0].current)
                break
    except Exception:
        pass

    net = psutil.net_io_counters()

    return {
        "ts": int(time.time()),
        "metrics": {
            "cpu": {"util": round(cpu, 1)},
            "ram": {
                "util": round(mem.percent, 1),
                "total_mb": mem.total // (1024 * 1024),
                "used_mb": mem.used // (1024 * 1024),
            },
            "gpu": {"util": round(gpu_util, 1), "temp": gpu_temp},
            "temp": {"cpu": cpu_temp, "gpu": gpu_temp},
            "net": {
                "rx_bytes": net.bytes_recv,
                "tx_bytes": net.bytes_sent,
            },
        },
    }


def push_loop(host: str, port: int, interval: float):
    # Prime cpu_percent
    psutil.cpu_percent(interval=None)

    while True:
        try:
            print(f"Connecting to {host}:{port}...")
            sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            sock.connect((host, port))
            print(f"Connected to {host}:{port}")

            while True:
                data = collect_metrics()
                line = json.dumps(data, separators=(",", ":")) + "\n"
                sock.sendall(line.encode())
                time.sleep(interval)

        except (ConnectionRefusedError, ConnectionResetError, BrokenPipeError, OSError) as e:
            print(f"Connection lost ({e}), retrying in 2s...")
            try:
                sock.close()
            except Exception:
                pass
            time.sleep(2)


def main():
    parser = argparse.ArgumentParser(description="Metric agent for VFD display")
    parser.add_argument("--host", required=True, help="VFD daemon host (Radxa IP)")
    parser.add_argument("--port", type=int, default=9100, help="VFD daemon port (default: 9100)")
    parser.add_argument(
        "--interval",
        type=float,
        default=0.25,
        help="Push interval in seconds (default: 0.25 = 4Hz)",
    )
    args = parser.parse_args()

    push_loop(args.host, args.port, args.interval)


if __name__ == "__main__":
    main()
