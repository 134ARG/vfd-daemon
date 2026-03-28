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

#define CPU_BITS_FULL 0x60
#define CPU_BIT_PARTIAL 0x20
#define RAM_BITS_FULL 0x0c
#define RAM_BIT_PARTIAL 0x04
#define GPU_BIT_FULL 0x01

#define HBAR_START_GRID 4
#define HBAR_NUM_GRIDS 4
#define HBAR_TOTAL_COLS (HBAR_NUM_GRIDS * GRID_SIZE) // 20
#define VERT_GRID 3

// -----------------------
// Render functions
// -----------------------

// Render bar into vram grids 3-7 (theme 1: full 5-grid CPU bar)
static void render_cpu_bar(int fill_cols, int frac_rows) {
    for (int g = BAR_START_GRID; g < BAR_START_GRID + BAR_NUM_GRIDS; g++)
        memset(vram[g], 0, GRID_SIZE);

    for (int i = 0; i < fill_cols && i < BAR_TOTAL_COLS; i++) {
        int col_idx = (BAR_TOTAL_COLS - 1) - i;
        int g = BAR_START_GRID + col_idx / GRID_SIZE;
        int c = col_idx % GRID_SIZE;
        vram[g][c] = COL_FULL;
    }

    if (fill_cols < BAR_TOTAL_COLS && frac_rows > 0) {
        int col_idx = (BAR_TOTAL_COLS - 1) - fill_cols;
        int g = BAR_START_GRID + col_idx / GRID_SIZE;
        int c = col_idx % GRID_SIZE;
        vram[g][c] = partial_col_down[frac_rows];
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
    if (bytes_per_sec < 1000) return 0;
    if (bytes_per_sec < 10000) return 1;
    if (bytes_per_sec < 100000) return 2;
    if (bytes_per_sec < 1000000) return 3;
    if (bytes_per_sec < 5000000) return 4;
    if (bytes_per_sec < 25000000) return 5;
    if (bytes_per_sec < 100000000) return 6;
    return 7;
}

// -----------------------
// Connection wait indicator
// -----------------------

static bool show_connection_wait(void) {
    if (is_remote_connected()) return false;

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
// Theme 1: CPU filled bar (remote)
// -----------------------

static float cpu_displayed_level = 0.0f;

void cpu_monitor(void) {
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
    float diff = target - cpu_displayed_level;
    if (diff > 1.0f || diff < -1.0f)
        cpu_displayed_level += diff * 0.3f;
    else
        cpu_displayed_level = target;

    if (cpu_displayed_level < 0) cpu_displayed_level = 0;
    if (cpu_displayed_level > BAR_TOTAL_COLS * 7)
        cpu_displayed_level = BAR_TOTAL_COLS * 7;

    int total_rows = (int)(cpu_displayed_level + 0.5f);
    int fill_cols = total_rows / 7;
    int frac_rows = total_rows % 7;

    render_cpu_bar(fill_cols, frac_rows);
    vfd_update_all_vram();
    vfd_write_dc(0, (uint8_t[]){'C', 'P', 'U', 3, 4, 5, 6, 7}, 8);

    uint8_t show[] = {0xe8};
    spi_write(sizeof(show), show);
}

// -----------------------
// Theme 3: Triple stacked bars + vertical metrics (remote)
// -----------------------

static float tri_cpu_level = 0.0f;
static float tri_ram_level = 0.0f;
static float tri_gpu_level = 0.0f;

void triple_bar_monitor(void) {
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
        vfd_write_dc(0, (uint8_t[]){'N', 'R', 'M', 3, 4, 5, 6, 7}, 8);
        uint8_t show[] = {0xe8};
        spi_write(sizeof(show), show);

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

    float cpu_target = (float)(cpu_pct / 100.0 * 40);
    float ram_target = (float)(ram_pct / 100.0 * 40);
    float gpu_target = (float)(gpu_pct / 100.0 * 20);

    float d;
    d = cpu_target - tri_cpu_level;
    tri_cpu_level += (d > 1.0f || d < -1.0f) ? d * 0.3f : d;
    d = ram_target - tri_ram_level;
    tri_ram_level += (d > 1.0f || d < -1.0f) ? d * 0.3f : d;
    d = gpu_target - tri_gpu_level;
    tri_gpu_level += (d > 1.0f || d < -1.0f) ? d * 0.3f : d;

    if (tri_cpu_level < 0) tri_cpu_level = 0;
    if (tri_cpu_level > 40) tri_cpu_level = 40;
    if (tri_ram_level < 0) tri_ram_level = 0;
    if (tri_ram_level > 40) tri_ram_level = 40;
    if (tri_gpu_level < 0) tri_gpu_level = 0;
    if (tri_gpu_level > 20) tri_gpu_level = 20;

    for (int g = VERT_GRID; g < HBAR_START_GRID + HBAR_NUM_GRIDS; g++)
        memset(vram[g], 0, GRID_SIZE);

    render_2row_hbar((int)(tri_cpu_level + 0.5f), CPU_BITS_FULL, CPU_BIT_PARTIAL);
    render_2row_hbar((int)(tri_ram_level + 0.5f), RAM_BITS_FULL, RAM_BIT_PARTIAL);
    render_1row_hbar((int)(tri_gpu_level + 0.5f), GPU_BIT_FULL);

    // Vertical bars (grid 3)
    double cpu_temp = snap.cpu_temp;
    if (cpu_temp > 100) cpu_temp = 100;
    if (cpu_temp < 0) cpu_temp = 0;
    vram[VERT_GRID][0] = partial_col_up[(int)(cpu_temp / 100.0 * 7 + 0.5)];

    double gpu_temp = snap.gpu_temp;
    if (gpu_temp > 100) gpu_temp = 100;
    if (gpu_temp < 0) gpu_temp = 0;
    vram[VERT_GRID][1] = partial_col_up[(int)(gpu_temp / 100.0 * 7 + 0.5)];

    // Net RX/TX rate
    static double rx_rate_smooth = 0, tx_rate_smooth = 0;
    struct timespec now_ts;
    clock_gettime(CLOCK_MONOTONIC, &now_ts);
    double dt = (now_ts.tv_sec - prev_ts.tv_sec) +
                (now_ts.tv_nsec - prev_ts.tv_nsec) / 1e9;

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

    // Alternate grids 0-2: status (NRM/ALR) ↔ uptime
    static int label_frame = 0;
    label_frame++;
    bool show_uptime = (label_frame / 30) % 2;
    bool alert = snap.failed_units > 0;

    if (show_uptime) {
        int hours = snap.uptime_sec / 3600;
        if (hours > 999) hours = 999;
        char uptxt[8];
        if (hours < 10)
            snprintf(uptxt, sizeof(uptxt), "0%dH", hours);
        else if (hours < 100)
            snprintf(uptxt, sizeof(uptxt), "%dH", hours);
        else
            snprintf(uptxt, sizeof(uptxt), "%d", hours);
        vfd_write_dc(0, (uint8_t[]){uptxt[0], uptxt[1], uptxt[2],
                                     3, 4, 5, 6, 7}, 8);
    } else if (alert) {
        bool alr_visible = (label_frame % 8) < 4;
        if (alr_visible)
            vfd_write_dc(0, (uint8_t[]){'A', 'L', 'R', 3, 4, 5, 6, 7}, 8);
        else
            vfd_write_dc(0, (uint8_t[]){' ', ' ', ' ', 3, 4, 5, 6, 7}, 8);
    } else {
        vfd_write_dc(0, (uint8_t[]){'N', 'R', 'M', 3, 4, 5, 6, 7}, 8);
    }

    uint8_t show[] = {0xe8};
    spi_write(sizeof(show), show);
}
