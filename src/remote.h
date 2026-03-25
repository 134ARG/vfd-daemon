#pragma once

#include <stdbool.h>

typedef struct {
    double cpu_util, ram_util, gpu_util;
    double cpu_temp, gpu_temp;
    double net_rx_bytes, net_tx_bytes;
    int uptime_sec;
    int failed_units;
} metrics_snapshot_t;

void start_remote_reader(int port);
void get_remote_metrics_full(metrics_snapshot_t* out);
void get_remote_metrics(double* cpu, double* ram, double* gpu);
bool is_remote_connected(void);
