#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_CORES 32

typedef struct {
    unsigned long user;
    unsigned long nice;
    unsigned long system;
    unsigned long idle;
    unsigned long iowait;
    unsigned long irq;
    unsigned long softirq;
    unsigned long steal;
    unsigned long guest;
    unsigned long guest_nice;
} CpuStats;

void read_cpu_stats(CpuStats *stats, int num_cores) {
    FILE *fp = fopen("/proc/stat", "r");
    if (!fp) {
        perror("Failed to open /proc/stat");
        exit(EXIT_FAILURE);
    }

    char line[256];
    int core_id = 0;

    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "cpu ", 4) == 0) {
            // Skip the total CPU line
            continue;
        } else if (strncmp(line, "cpu", 3) == 0) {
            if (core_id >= num_cores) {
                break; // Stop reading if we've reached the maximum number of cores
            }
            sscanf(line, "cpu%d %lu %lu %lu %lu %lu %lu %lu %lu %lu",
                   &core_id, &stats[core_id].user, &stats[core_id].nice,
                   &stats[core_id].system, &stats[core_id].idle,
                   &stats[core_id].iowait, &stats[core_id].irq,
                   &stats[core_id].softirq, &stats[core_id].steal,
                   &stats[core_id].guest);
            core_id++;
        }
    }

    fclose(fp);
}

double calculate_cpu_utilization(CpuStats *prev_stats, CpuStats *curr_stats) {
    unsigned long prev_total = prev_stats->user + prev_stats->nice +
                              prev_stats->system + prev_stats->idle +
                              prev_stats->iowait + prev_stats->irq +
                              prev_stats->softirq + prev_stats->steal +
                              prev_stats->guest;

    unsigned long curr_total = curr_stats->user + curr_stats->nice +
                              curr_stats->system + curr_stats->idle +
                              curr_stats->iowait + curr_stats->irq +
                              curr_stats->softirq + curr_stats->steal +
                              curr_stats->guest;

    unsigned long prev_idle = prev_stats->idle + prev_stats->iowait;
    unsigned long curr_idle = curr_stats->idle + curr_stats->iowait;

    double idle_diff = curr_idle - prev_idle;
    double total_diff = curr_total - prev_total;

    if (total_diff == 0) {
        return 0.0; // Avoid division by zero
    }

    return ((total_diff - idle_diff) / total_diff) * 100.0;
}

int main() {
    CpuStats prev_stats[MAX_CORES];
    CpuStats curr_stats[MAX_CORES];

    int num_cores = MAX_CORES;

    read_cpu_stats(prev_stats, num_cores);

    // Sleep for a second to get the next set of stats
    usleep(500e3);

    read_cpu_stats(curr_stats, num_cores);

    for (int i = 0; i < num_cores; i++) {
        double util = calculate_cpu_utilization(&prev_stats[i], &curr_stats[i]);
        printf("Core %d: %.2f%%\n", i, util);
    }

    return 0;
}
