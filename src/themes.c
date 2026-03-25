#include "themes.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "remote.h"
#include "spi.h"
#include "vfd.h"

// -----------------------
// Bar rendering constants
// -----------------------

#define BAR_START_GRID 3
#define BAR_NUM_GRIDS 5
#define BAR_TOTAL_COLS (BAR_NUM_GRIDS * GRID_SIZE) // 25

#define COL_FULL 0x7f
#define COL_EMPTY 0x00

#define BORDER_TB 0x41  // top + bottom row only
#define BORDER_FULL 0x7f

#define CPU_BITS_FULL 0x60  // bits 5+6
#define CPU_BIT_PARTIAL 0x20 // bit 5 only (bottom row of CPU band)
#define RAM_BITS_FULL 0x0c  // bits 2+3
#define RAM_BIT_PARTIAL 0x04 // bit 2 only (bottom row of RAM band)
#define GPU_BIT_FULL 0x01   // bit 0

#define HBAR_START_GRID 4
#define HBAR_NUM_GRIDS 4
#define HBAR_TOTAL_COLS (HBAR_NUM_GRIDS * GRID_SIZE) // 20
#define VERT_GRID 3

// -----------------------
// CPU utilization reading
// -----------------------

typedef struct {
    unsigned long user, nice, system, idle, iowait, irq, softirq, steal;
} cpu_stat_t;

static bool read_total_cpu(cpu_stat_t* s) {
    FILE* fp = fopen("/proc/stat", "r");
    if (!fp) return false;
    char line[256];
    if (!fgets(line, sizeof(line), fp)) {
        fclose(fp);
        return false;
    }
    sscanf(line, "cpu %lu %lu %lu %lu %lu %lu %lu %lu",
           &s->user, &s->nice, &s->system, &s->idle,
           &s->iowait, &s->irq, &s->softirq, &s->steal);
    fclose(fp);
    return true;
}

static double calc_cpu_util(cpu_stat_t* prev, cpu_stat_t* cur) {
    unsigned long prev_total = prev->user + prev->nice + prev->system +
                               prev->idle + prev->iowait + prev->irq +
                               prev->softirq + prev->steal;
    unsigned long cur_total = cur->user + cur->nice + cur->system +
                              cur->idle + cur->iowait + cur->irq +
                              cur->softirq + cur->steal;
    unsigned long prev_idle_t = prev->idle + prev->iowait;
    unsigned long cur_idle_t = cur->idle + cur->iowait;
    double total_d = (double)(cur_total - prev_total);
    if (total_d == 0) return 0.0;
    return ((total_d - (double)(cur_idle_t - prev_idle_t)) / total_d) * 100.0;
}

// -----------------------
// Local metric reading
// -----------------------

static double read_ram_util(void) {
    FILE* fp = fopen("/proc/meminfo", "r");
    if (!fp) return 0.0;
    unsigned long total = 0, available = 0;
    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        if (sscanf(line, "MemTotal: %lu", &total) == 1) continue;
        if (sscanf(line, "MemAvailable: %lu", &available) == 1) break;
    }
    fclose(fp);
    if (total == 0) return 0.0;
    return (double)(total - available) / total * 100.0;
}

// GPU util: try reading from a common sysfs path (Mali/Panfrost on RK3566)
// Falls back to 0 if not available
static double read_gpu_util(void) {
    // Try panfrost/mali sysfs
    const char* paths[] = {
        "/sys/class/devfreq/fb000000.gpu/load",
        "/sys/class/devfreq/gpu/load",
        NULL,
    };
    for (int i = 0; paths[i]; i++) {
        FILE* fp = fopen(paths[i], "r");
        if (!fp) continue;
        unsigned long load = 0;
        if (fscanf(fp, "%lu@", &load) == 1) {
            // Some formats: "load@freq"
            fclose(fp);
            return (double)load;
        }
        fclose(fp);
    }

    // Try simple percentage file
    FILE* fp = fopen("/sys/class/devfreq/fb000000.gpu/cur_freq", "r");
    if (fp) {
        fclose(fp);
        // GPU exists but no load info — return 0
    }
    return 0.0;
}

// -----------------------
// Render functions
// -----------------------

// Render bar into vram grids 3-7
// fill_cols: fully filled columns (0..25), right to left
// frac_rows: partial fill rows for boundary column (0..6)
static void render_cpu_bar(int fill_cols, int frac_rows) {
    for (int g = BAR_START_GRID; g < BAR_START_GRID + BAR_NUM_GRIDS; g++)
        memset(vram[g], 0, GRID_SIZE);

    // Fill from rightmost column (index 24) going left
    for (int i = 0; i < fill_cols && i < BAR_TOTAL_COLS; i++) {
        int col_idx = (BAR_TOTAL_COLS - 1) - i;
        int g = BAR_START_GRID + col_idx / GRID_SIZE;
        int c = col_idx % GRID_SIZE;
        vram[g][c] = COL_FULL;
    }

    // Boundary column with partial fill
    if (fill_cols < BAR_TOTAL_COLS && frac_rows > 0) {
        int col_idx = (BAR_TOTAL_COLS - 1) - fill_cols;
        int g = BAR_START_GRID + col_idx / GRID_SIZE;
        int c = col_idx % GRID_SIZE;
        vram[g][c] = partial_col[frac_rows];
    }
}

static void render_bordered_bar(int fill_cols) {
    // Draw the empty bordered rectangle first
    for (int g = BAR_START_GRID; g < BAR_START_GRID + BAR_NUM_GRIDS; g++)
        for (int c = 0; c < GRID_SIZE; c++)
            vram[g][c] = BORDER_TB; // top + bottom border

    // Left vertical edge
    vram[BAR_START_GRID][0] = BORDER_FULL;
    // Right vertical edge
    vram[BAR_START_GRID + BAR_NUM_GRIDS - 1][GRID_SIZE - 1] = BORDER_FULL;

    // Fill from right to left (inside the border, so skip the two edge columns)
    // Fillable columns: index 1 .. 23 (skip col 0 = left edge, col 24 = right edge)
    int fillable = BAR_TOTAL_COLS - 2;
    if (fill_cols > fillable) fill_cols = fillable;

    for (int i = 0; i < fill_cols; i++) {
        // Start from col 23 (one left of right edge) going left
        int col_idx = (BAR_TOTAL_COLS - 2) - i;
        int g = BAR_START_GRID + col_idx / GRID_SIZE;
        int c = col_idx % GRID_SIZE;
        vram[g][c] = BORDER_FULL;
    }
}

// Render a 2-row bar into vram columns using given bit masks
// level: 0..50 (25 cols × 2 rows)
static void render_2row_bar(int level, uint8_t full_mask, uint8_t partial_mask) {
    int fill_cols = level / 2;
    int frac = level % 2;

    for (int i = 0; i < fill_cols && i < BAR_TOTAL_COLS; i++) {
        int col_idx = (BAR_TOTAL_COLS - 1) - i;
        int g = BAR_START_GRID + col_idx / GRID_SIZE;
        int c = col_idx % GRID_SIZE;
        vram[g][c] |= full_mask;
    }

    if (fill_cols < BAR_TOTAL_COLS && frac > 0) {
        int col_idx = (BAR_TOTAL_COLS - 1) - fill_cols;
        int g = BAR_START_GRID + col_idx / GRID_SIZE;
        int c = col_idx % GRID_SIZE;
        vram[g][c] |= partial_mask;
    }
}

// Render a 1-row bar (GPU)
// level: 0..25
static void render_1row_bar(int level, uint8_t bit_mask) {
    for (int i = 0; i < level && i < BAR_TOTAL_COLS; i++) {
        int col_idx = (BAR_TOTAL_COLS - 1) - i;
        int g = BAR_START_GRID + col_idx / GRID_SIZE;
        int c = col_idx % GRID_SIZE;
        vram[g][c] |= bit_mask;
    }
}

// Render a 2-row horizontal bar into grids 4-7
static void render_2row_hbar(int level, uint8_t full_mask,
                             uint8_t partial_mask) {
    int fill_cols = level / 2;
    int frac = level % 2;

    for (int i = 0; i < fill_cols && i < HBAR_TOTAL_COLS; i++) {
        int col_idx = (HBAR_TOTAL_COLS - 1) - i;
        int g = HBAR_START_GRID + col_idx / GRID_SIZE;
        int c = col_idx % GRID_SIZE;
        vram[g][c] |= full_mask;
    }

    if (fill_cols < HBAR_TOTAL_COLS && frac > 0) {
        int col_idx = (HBAR_TOTAL_COLS - 1) - fill_cols;
        int g = HBAR_START_GRID + col_idx / GRID_SIZE;
        int c = col_idx % GRID_SIZE;
        vram[g][c] |= partial_mask;
    }
}

// Render a 1-row horizontal bar into grids 4-7
static void render_1row_hbar(int level, uint8_t bit_mask) {
    for (int i = 0; i < level && i < HBAR_TOTAL_COLS; i++) {
        int col_idx = (HBAR_TOTAL_COLS - 1) - i;
        int g = HBAR_START_GRID + col_idx / GRID_SIZE;
        int c = col_idx % GRID_SIZE;
        vram[g][c] |= bit_mask;
    }
}

// Map network rate (bytes/sec) to 0-7 using log scale
static int net_rate_to_level(double bytes_per_sec) {
    if (bytes_per_sec < 1000) return 0;           // < 1 KB/s
    if (bytes_per_sec < 10000) return 1;           // < 10 KB/s
    if (bytes_per_sec < 100000) return 2;          // < 100 KB/s
    if (bytes_per_sec < 1000000) return 3;         // < 1 MB/s
    if (bytes_per_sec < 5000000) return 4;         // < 5 MB/s
    if (bytes_per_sec < 25000000) return 5;        // < 25 MB/s
    if (bytes_per_sec < 100000000) return 6;       // < 100 MB/s
    return 7;                                       // >= 100 MB/s
}

// -----------------------
// Connection wait indicator for remote modes
// -----------------------

// Returns true if we should skip the normal render (showing wait screen instead)
static bool show_connection_wait(void) {
    if (is_remote_connected()) return false;

    // Asymmetric flash: 800ms on, 200ms off (8 frames on, 2 frames off at 10fps)
    static int frame = 0;
    frame++;
    bool visible = (frame % 10) < 8;

    if (visible) {
        vfd_display_string(0, "CON WAIT");
    } else {
        vfd_display_string(0, "        ");
    }
    uint8_t show[] = {0xe8};
    spi_write(sizeof(show), show);
    return true;
}

// -----------------------
// Debug workloads
// -----------------------

void set_grid_bar(uint8_t bar_idx, uint8_t bar_total_num,
                  uint8_t grid_start_idx, uint8_t grid_total_num,
                  uint8_t util_idx) {
    static uint8_t bar_distribute[GRID_SIZE + 1][GRID_SIZE] = {
        {-1, -1, -1, -1, -1}, {0, 0, 0, 0, 0},  {0, 0, -1, 1, 1},
        {0, -1, 1, -1, 2},    {0, 1, 2, 3, -1}, {0, 1, 2, 3, 4},
    };

    uint8_t bar_per_grid = bar_total_num / grid_total_num;
    if (bar_per_grid > GRID_SIZE) {
        bar_per_grid = GRID_SIZE;
    }

    uint8_t grid_idx = bar_idx / bar_per_grid + grid_start_idx;
    uint8_t sub_idx = bar_idx % bar_per_grid;

    for (uint8_t i = 0; i < GRID_SIZE; i++) {
        uint8_t idx = bar_distribute[bar_per_grid][i];
        if (idx == (uint8_t)-1) {
            vram[grid_idx][i] = get_util_bar(0);
        } else if (idx == sub_idx) {
            vram[grid_idx][i] = get_util_bar(util_idx);
        }
    }
    return;
}

void debug_show_util_bars(void) {
    const uint8_t total_bars = 2 * NUM_GRID;

    static bool initialized = false;
    static uint8_t bar_idx[2 * NUM_GRID] = {0};
    if (!initialized) {
        for (int i = 0; i < total_bars; i++) {
            bar_idx[i] = rand() % COL_SIZE;
        }
        initialized = true;
    }

    uint8_t max_idx = 2 * (COL_SIZE - 1);
    for (int i = 0; i < total_bars; i++) {
        uint8_t idx = bar_idx[i];
        idx = (idx + 1) % max_idx;
        bar_idx[i] = idx;
        if (idx > COL_SIZE - 1) {
            set_grid_bar(i, total_bars, 0, 8, max_idx - idx);
        } else {
            set_grid_bar(i, total_bars, 0, 8, idx);
        }
    }
    vfd_update_all_vram();
    vfd_display_all_vram();
}

void debug_clean_display(void) { vfd_display_string(0, "        "); }

void debug_show_damn(void) {
    static uint8_t flag = 1;
    if (flag) {
        vfd_display_string(1, "Damn~!");
    } else {
        debug_clean_display();
    }
    flag = !flag;
}

// -----------------------
// Theme 1: CPU filled bar
// -----------------------

static float displayed_level = 0.0f; // smoothed value in sub-column units

void cpu_util_monitor(void) {
    static cpu_stat_t prev = {0};
    static bool first = true;

    cpu_stat_t cur;
    if (!read_total_cpu(&cur)) return;

    if (first) {
        prev = cur;
        first = false;
        render_cpu_bar(0, 0);
        vfd_update_all_vram();
        vfd_write_dc(0, (uint8_t[]){'C', 'P', 'U', 3, 4, 5, 6, 7}, 8);
        uint8_t show[] = {0xe8};
        spi_write(sizeof(show), show);
        return;
    }

    double util = calc_cpu_util(&prev, &cur);
    prev = cur;
    if (util > 100.0) util = 100.0;
    if (util < 0.0) util = 0.0;

    // Target: 25 cols × 7 rows = 175 total steps
    float target = (float)(util / 100.0 * BAR_TOTAL_COLS * 7);

    // Smooth lerp toward target
    float diff = target - displayed_level;
    if (diff > 1.0f || diff < -1.0f)
        displayed_level += diff * 0.3f;
    else
        displayed_level = target;

    if (displayed_level < 0) displayed_level = 0;
    if (displayed_level > BAR_TOTAL_COLS * 7)
        displayed_level = BAR_TOTAL_COLS * 7;

    int total_rows = (int)(displayed_level + 0.5f);
    int fill_cols = total_rows / 7;
    int frac_rows = total_rows % 7;

    render_cpu_bar(fill_cols, frac_rows);
    vfd_update_all_vram();
    vfd_write_dc(0, (uint8_t[]){'C', 'P', 'U', 3, 4, 5, 6, 7}, 8);

    uint8_t show[] = {0xe8};
    spi_write(sizeof(show), show);
}

// -----------------------
// Theme 2: CPU bordered bar
// -----------------------

static float line_displayed = 0.0f;

void cpu_util_line_monitor(void) {
    static cpu_stat_t prev = {0};
    static bool first = true;

    cpu_stat_t cur;
    if (!read_total_cpu(&cur)) return;

    if (first) {
        prev = cur;
        first = false;
        render_bordered_bar(0);
        vfd_update_all_vram();
        vfd_write_dc(0, (uint8_t[]){'C', 'P', 'U', 3, 4, 5, 6, 7}, 8);
        uint8_t show[] = {0xe8};
        spi_write(sizeof(show), show);
        return;
    }

    double util = calc_cpu_util(&prev, &cur);
    prev = cur;
    if (util > 100.0) util = 100.0;
    if (util < 0.0) util = 0.0;

    int fillable = BAR_TOTAL_COLS - 2; // 23 interior columns
    float target = (float)(util / 100.0 * fillable);

    // Animate: step one column per frame toward target
    if (line_displayed < target - 0.5f)
        line_displayed += 1.0f;
    else if (line_displayed > target + 0.5f)
        line_displayed -= 1.0f;
    else
        line_displayed = target;

    if (line_displayed < 0) line_displayed = 0;
    if (line_displayed > fillable) line_displayed = fillable;

    int fill = (int)(line_displayed + 0.5f);

    render_bordered_bar(fill);
    vfd_update_all_vram();
    vfd_write_dc(0, (uint8_t[]){'C', 'P', 'U', 3, 4, 5, 6, 7}, 8);

    uint8_t show[] = {0xe8};
    spi_write(sizeof(show), show);
}

// -----------------------
// Theme 3: Triple stacked bars (CPU + RAM + GPU)
// -----------------------

static float tri_cpu_level = 0.0f;
static float tri_ram_level = 0.0f;
static float tri_gpu_level = 0.0f;

void triple_bar_monitor(void) {
    static cpu_stat_t prev = {0};
    static bool first = true;

    cpu_stat_t cur;
    if (!read_total_cpu(&cur)) return;

    if (first) {
        prev = cur;
        first = false;
        for (int g = BAR_START_GRID; g < BAR_START_GRID + BAR_NUM_GRIDS; g++)
            memset(vram[g], 0, GRID_SIZE);
        vfd_update_all_vram();
        vfd_write_dc(0, (uint8_t[]){'C', 'R', 'G', 3, 4, 5, 6, 7}, 8);
        uint8_t show[] = {0xe8};
        spi_write(sizeof(show), show);
        return;
    }

    // Read all utils
    double cpu_pct = calc_cpu_util(&prev, &cur);
    prev = cur;
    if (cpu_pct > 100.0) cpu_pct = 100.0;
    if (cpu_pct < 0.0) cpu_pct = 0.0;

    double ram_pct = read_ram_util();
    if (ram_pct > 100.0) ram_pct = 100.0;

    double gpu_pct = read_gpu_util();
    if (gpu_pct > 100.0) gpu_pct = 100.0;

    // Targets: CPU/RAM = 50 steps (2 rows × 25 cols), GPU = 25 steps (1 row)
    float cpu_target = (float)(cpu_pct / 100.0 * 50);
    float ram_target = (float)(ram_pct / 100.0 * 50);
    float gpu_target = (float)(gpu_pct / 100.0 * 25);

    // Smooth lerp
    float d;
    d = cpu_target - tri_cpu_level;
    tri_cpu_level += (d > 1.0f || d < -1.0f) ? d * 0.3f : d;
    d = ram_target - tri_ram_level;
    tri_ram_level += (d > 1.0f || d < -1.0f) ? d * 0.3f : d;
    d = gpu_target - tri_gpu_level;
    tri_gpu_level += (d > 1.0f || d < -1.0f) ? d * 0.3f : d;

    if (tri_cpu_level < 0) tri_cpu_level = 0;
    if (tri_cpu_level > 50) tri_cpu_level = 50;
    if (tri_ram_level < 0) tri_ram_level = 0;
    if (tri_ram_level > 50) tri_ram_level = 50;
    if (tri_gpu_level < 0) tri_gpu_level = 0;
    if (tri_gpu_level > 25) tri_gpu_level = 25;

    // Clear bar grids
    for (int g = BAR_START_GRID; g < BAR_START_GRID + BAR_NUM_GRIDS; g++)
        memset(vram[g], 0, GRID_SIZE);

    // Render all three bars (OR into vram)
    render_2row_bar((int)(tri_cpu_level + 0.5f), CPU_BITS_FULL, CPU_BIT_PARTIAL);
    render_2row_bar((int)(tri_ram_level + 0.5f), RAM_BITS_FULL, RAM_BIT_PARTIAL);
    render_1row_bar((int)(tri_gpu_level + 0.5f), GPU_BIT_FULL);

    vfd_update_all_vram();
    vfd_write_dc(0, (uint8_t[]){'C', 'R', 'G', 3, 4, 5, 6, 7}, 8);

    uint8_t show[] = {0xe8};
    spi_write(sizeof(show), show);
}

// -----------------------
// Remote triple bar monitor
// -----------------------

static float rtri_cpu_level = 0.0f;
static float rtri_ram_level = 0.0f;
static float rtri_gpu_level = 0.0f;

void remote_triple_bar_monitor(void) {
    static bool first = true;
    static double prev_rx = 0, prev_tx = 0;
    static struct timespec prev_ts = {0};

    if (show_connection_wait()) {
        first = true;
        return;
    }

    if (first) {
        first = false;
        for (int g = VERT_GRID; g < HBAR_START_GRID + HBAR_NUM_GRIDS; g++)
            memset(vram[g], 0, GRID_SIZE);
        vfd_update_all_vram();
        vfd_write_dc(0, (uint8_t[]){'C', 'R', 'G', 3, 4, 5, 6, 7}, 8);
        uint8_t show[] = {0xe8};
        spi_write(sizeof(show), show);

        // Prime net counters
        metrics_snapshot_t snap;
        get_remote_metrics_full(&snap);
        prev_rx = snap.net_rx_bytes;
        prev_tx = snap.net_tx_bytes;
        clock_gettime(CLOCK_MONOTONIC, &prev_ts);
        return;
    }

    metrics_snapshot_t snap;
    get_remote_metrics_full(&snap);

    double cpu_pct = snap.cpu_util;
    double ram_pct = snap.ram_util;
    double gpu_pct = snap.gpu_util;
    if (cpu_pct > 100.0) cpu_pct = 100.0;
    if (ram_pct > 100.0) ram_pct = 100.0;
    if (gpu_pct > 100.0) gpu_pct = 100.0;

    // Horizontal bar targets: 40 steps for 2-row, 20 for 1-row
    float cpu_target = (float)(cpu_pct / 100.0 * 40);
    float ram_target = (float)(ram_pct / 100.0 * 40);
    float gpu_target = (float)(gpu_pct / 100.0 * 20);

    float d;
    d = cpu_target - rtri_cpu_level;
    rtri_cpu_level += (d > 1.0f || d < -1.0f) ? d * 0.3f : d;
    d = ram_target - rtri_ram_level;
    rtri_ram_level += (d > 1.0f || d < -1.0f) ? d * 0.3f : d;
    d = gpu_target - rtri_gpu_level;
    rtri_gpu_level += (d > 1.0f || d < -1.0f) ? d * 0.3f : d;

    if (rtri_cpu_level < 0) rtri_cpu_level = 0;
    if (rtri_cpu_level > 40) rtri_cpu_level = 40;
    if (rtri_ram_level < 0) rtri_ram_level = 0;
    if (rtri_ram_level > 40) rtri_ram_level = 40;
    if (rtri_gpu_level < 0) rtri_gpu_level = 0;
    if (rtri_gpu_level > 20) rtri_gpu_level = 20;

    // Clear grids 3-7
    for (int g = VERT_GRID; g < HBAR_START_GRID + HBAR_NUM_GRIDS; g++)
        memset(vram[g], 0, GRID_SIZE);

    // --- Horizontal bars (grids 4-7) ---
    render_2row_hbar((int)(rtri_cpu_level + 0.5f), CPU_BITS_FULL,
                     CPU_BIT_PARTIAL);
    render_2row_hbar((int)(rtri_ram_level + 0.5f), RAM_BITS_FULL,
                     RAM_BIT_PARTIAL);
    render_1row_hbar((int)(rtri_gpu_level + 0.5f), GPU_BIT_FULL);

    // --- Vertical bars (grid 3) ---
    // CPU temp: col 0, 0-100°C → 0-7 rows, bottom-to-top
    double cpu_temp = snap.cpu_temp;
    if (cpu_temp > 100) cpu_temp = 100;
    if (cpu_temp < 0) cpu_temp = 0;
    int cpu_temp_level = (int)(cpu_temp / 100.0 * 7 + 0.5);
    vram[VERT_GRID][0] = partial_col_up[cpu_temp_level];

    // GPU junction temp: col 1
    double gpu_temp = snap.gpu_temp;
    if (gpu_temp > 100) gpu_temp = 100;
    if (gpu_temp < 0) gpu_temp = 0;
    int gpu_temp_level = (int)(gpu_temp / 100.0 * 7 + 0.5);
    vram[VERT_GRID][1] = partial_col_up[gpu_temp_level];

    // Col 2: blank separator (already 0)

    // Net RX/TX rate: only recompute when counters change (agent pushes ~4Hz)
    static double rx_rate_smooth = 0, tx_rate_smooth = 0;
    struct timespec now_ts;
    clock_gettime(CLOCK_MONOTONIC, &now_ts);
    double dt = (now_ts.tv_sec - prev_ts.tv_sec) +
                (now_ts.tv_nsec - prev_ts.tv_nsec) / 1e9;

    // Only update rate when we see new counter values from the agent
    if (snap.net_rx_bytes != prev_rx || snap.net_tx_bytes != prev_tx) {
        if (dt > 0.05) {
            double rx_rate = (snap.net_rx_bytes - prev_rx) / dt;
            double tx_rate = (snap.net_tx_bytes - prev_tx) / dt;
            if (rx_rate < 0) rx_rate = 0;
            if (tx_rate < 0) tx_rate = 0;
            rx_rate_smooth = rx_rate;
            tx_rate_smooth = tx_rate;
        }
        prev_rx = snap.net_rx_bytes;
        prev_tx = snap.net_tx_bytes;
        prev_ts = now_ts;
    }

    vram[VERT_GRID][3] = partial_col_up[net_rate_to_level(rx_rate_smooth)];
    vram[VERT_GRID][4] = partial_col_up[net_rate_to_level(tx_rate_smooth)];

    vfd_update_all_vram();

    // Alternate grids 0-2: status (NRM/ALR) ↔ uptime, every 3 seconds
    static int label_frame = 0;
    label_frame++;
    bool show_uptime = (label_frame / 30) % 2;

    bool alert = snap.failed_units > 0;

    if (show_uptime) {
        int hours = snap.uptime_sec / 3600;
        if (hours > 999) hours = 999;
        char uptxt[8];
        if (hours < 10) {
            snprintf(uptxt, sizeof(uptxt), "0%dH", hours);       // "0xH"
        } else if (hours < 100) {
            snprintf(uptxt, sizeof(uptxt), "%dH", hours);        // "xxH"
        } else {
            snprintf(uptxt, sizeof(uptxt), "%d", hours);         // "xxx"
        }
        vfd_write_dc(0, (uint8_t[]){uptxt[0], uptxt[1], uptxt[2],
                                     3, 4, 5, 6, 7}, 8);
    } else if (alert) {
        // Flash ALR at 0.4s/0.4s (4 frames on, 4 frames off at 10fps)
        bool alr_visible = (label_frame % 8) < 4;
        if (alr_visible) {
            vfd_write_dc(0, (uint8_t[]){'A', 'L', 'R', 3, 4, 5, 6, 7}, 8);
        } else {
            vfd_write_dc(0, (uint8_t[]){' ', ' ', ' ', 3, 4, 5, 6, 7}, 8);
        }
    } else {
        vfd_write_dc(0, (uint8_t[]){'N', 'R', 'M', 3, 4, 5, 6, 7}, 8);
    }

    uint8_t show[] = {0xe8};
    spi_write(sizeof(show), show);
}

// -----------------------
// Remote CPU-only monitor (theme 1 with remote data)
// -----------------------

static float remote_displayed_level = 0.0f;

void remote_cpu_monitor(void) {
    static bool first = true;

    if (show_connection_wait()) {
        first = true;
        return;
    }

    if (first) {
        first = false;
        render_cpu_bar(0, 0);
        vfd_update_all_vram();
        vfd_write_dc(0, (uint8_t[]){'C', 'P', 'U', 3, 4, 5, 6, 7}, 8);
        uint8_t show[] = {0xe8};
        spi_write(sizeof(show), show);
        return;
    }

    double cpu_pct, ram_pct, gpu_pct;
    get_remote_metrics(&cpu_pct, &ram_pct, &gpu_pct);
    if (cpu_pct > 100.0) cpu_pct = 100.0;
    if (cpu_pct < 0.0) cpu_pct = 0.0;

    float target = (float)(cpu_pct / 100.0 * BAR_TOTAL_COLS * 7);
    float diff = target - remote_displayed_level;
    if (diff > 1.0f || diff < -1.0f)
        remote_displayed_level += diff * 0.3f;
    else
        remote_displayed_level = target;

    if (remote_displayed_level < 0) remote_displayed_level = 0;
    if (remote_displayed_level > BAR_TOTAL_COLS * 7)
        remote_displayed_level = BAR_TOTAL_COLS * 7;

    int total_rows = (int)(remote_displayed_level + 0.5f);
    int fill_cols = total_rows / 7;
    int frac_rows = total_rows % 7;

    render_cpu_bar(fill_cols, frac_rows);
    vfd_update_all_vram();
    vfd_write_dc(0, (uint8_t[]){'C', 'P', 'U', 3, 4, 5, 6, 7}, 8);

    uint8_t show[] = {0xe8};
    spi_write(sizeof(show), show);
}
