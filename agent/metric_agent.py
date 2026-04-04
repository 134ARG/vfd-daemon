#!/usr/bin/env python3
"""
Metric agent for x86 host.
Collects system metrics and pushes them over TCP as newline-delimited JSON.
Connects to the VFD daemon running on the Radxa.

Usage: python3 metric_agent.py --host <IP> [--port 9100] [--interval 0.25]
"""

import argparse
import json
import socket
import time
import glob
import subprocess
import os
import psutil

# --- Global Caches ---
BOOT_TIME = psutil.boot_time()
AMD_GPU_BUSY_PATH = None
AMD_GPU_TEMP_PATH = None
CPU_TMP_PATH = None
LAST_SYSTEMD_CHECK = 0
CACHED_FAILED_UNITS = 0

def init_hardware_paths():
    """Finds and caches static hardware sysfs paths so we don't glob at 4Hz."""
    global AMD_GPU_BUSY_PATH, AMD_GPU_TEMP_PATH, CPU_TMP_PATH
    
    # Cache AMD GPU utilization path
    amd_paths = glob.glob("/sys/class/drm/card*/device/gpu_busy_percent")
    if amd_paths:
        print(f'find gpu busy path:{amd_paths[0]}')
        AMD_GPU_BUSY_PATH = amd_paths[0]
        
    # Cache AMD GPU junction temp path
    hwmon_dirs = glob.glob("/sys/class/drm/card*/device/hwmon/hwmon*")
    for hwmon in hwmon_dirs:
        for i in range(1, 10):
            label_path = f"{hwmon}/temp{i}_label"
            input_path = f"{hwmon}/temp{i}_input"
            try:
                with open(label_path) as f:
                    if f.read().strip() == "junction":
                        if os.path.exists(input_path):
                            print(f'find gpu temp path:{input_path}')
                            AMD_GPU_TEMP_PATH = input_path
                        break
            except FileNotFoundError:
                continue
        if AMD_GPU_TEMP_PATH:
            break

    # Cache AMD CPU temp path
    hwmon_dirs = glob.glob("/sys/class/hwmon/hwmon*")
    for hwmon in hwmon_dirs:
        for i in range(1, 10):
            label_path = f"{hwmon}/temp{i}_label"
            input_path = f"{hwmon}/temp{i}_input"
            try:
                with open(label_path) as f:
                    if f.read().strip() == "Tctl":
                        if os.path.exists(input_path):
                            print(f'find cpu temp path:{input_path}')
                            CPU_TMP_PATH = input_path
                        break
            except FileNotFoundError:
                continue
        if CPU_TMP_PATH:
            break


def collect_metrics() -> dict:
    global LAST_SYSTEMD_CHECK, CACHED_FAILED_UNITS
    
    current_time = time.time()
    
    # Fast metrics
    cpu = psutil.cpu_percent(interval=None)
    mem = psutil.virtual_memory()
    net = psutil.net_io_counters()
    
    # Uptime calculated via math, no file I/O required
    uptime_sec = int(current_time - BOOT_TIME)
    
    gpu_util = 0.0
    gpu_temp = 0

    # Fast direct file reads using cached paths
    if AMD_GPU_BUSY_PATH:
        try:
            with open(AMD_GPU_BUSY_PATH, 'rb', buffering=0) as f:
                gpu_util = float(f.read().strip())
        except Exception:
            pass
            
    if AMD_GPU_TEMP_PATH:
        try:
            with open(AMD_GPU_TEMP_PATH, 'rb', buffering=0) as f:
                gpu_temp = int(f.read().strip()) // 1000
        except Exception:
            pass

    cpu_temp = 0

    if CPU_TMP_PATH:
        try:
            with open(CPU_TMP_PATH, 'rb', buffering=0) as f:
                cpu_temp = int(f.read().strip()) // 1000
        except Exception:
            pass

    # Systemd check throttled to once every 20 seconds
    if current_time - LAST_SYSTEMD_CHECK > 20.0:
        try:
            result = subprocess.run(
                ["systemctl", "--no-legend", "--plain", "--state=failed", "list-units"],
                capture_output=True, text=True, timeout=2,
            )
            CACHED_FAILED_UNITS = len([l for l in result.stdout.strip().splitlines() if l.strip()])
            LAST_SYSTEMD_CHECK = current_time
        except Exception:
            pass

    return {
        "ts": int(current_time),
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
            "sys": {"uptime": uptime_sec, "failed_units": CACHED_FAILED_UNITS},
        },
    }

def push_loop(host: str, port: int, interval: float):
    # Prime cpu_percent
    psutil.cpu_percent(interval=None)
    
    # Prime hardware paths
    init_hardware_paths()

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