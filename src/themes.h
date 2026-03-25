#pragma once

#include "worklist.h"

/* Theme workload functions */
void cpu_util_monitor(void);
void cpu_util_line_monitor(void);
void triple_bar_monitor(void);
void remote_triple_bar_monitor(void);
void remote_cpu_monitor(void);

/* Debug workloads */
void set_grid_bar(uint8_t bar_idx, uint8_t bar_total_num,
                  uint8_t grid_start_idx, uint8_t grid_total_num,
                  uint8_t util_idx);
void debug_show_util_bars(void);
void debug_clean_display(void);
void debug_show_damn(void);
