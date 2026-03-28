#include "vfd.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "spi.h"

// -----------------------
// VFD data
// -----------------------

const uint8_t util_bars[COL_SIZE] = {
    0x00, 0x40, 0x60, 0x70, 0x78, 0x7c, 0x7e, 0x7f,
};

uint8_t get_util_bar(uint8_t idx) { return util_bars[idx]; }

uint8_t vram[NUM_GRID][GRID_SIZE] = {0};

uint8_t* flatten_vram(void) { return (uint8_t*)vram; }

// Partial fill: index 0..7 = how many rows lit from bottom
const uint8_t partial_col_down[8] = {
    0x00, 0x01, 0x03, 0x07, 0x0f, 0x1f, 0x3f, 0x7f,
};

// Partial fill growing from bottom of display (bit 6) upward
// bit 6 = bottom row on screen, bit 0 = top row
const uint8_t partial_col_up[8] = {
    0x00, 0x40, 0x60, 0x70, 0x78, 0x7c, 0x7e, 0x7f,
};

// -----------------------
// vfd low level
// -----------------------

bool vfd_write_dc(uint8_t begin_idx, const uint8_t* addrs, uint8_t len) {
    if (begin_idx > MAX_IDX) {
        begin_idx = MAX_IDX;
    }

    if (len + begin_idx > NUM_GRID) {
        len = NUM_GRID - begin_idx;
    }

    len += 1;
    uint8_t* buffer = calloc(len, 1);
    buffer[0] = DC_ADDR_START + begin_idx;
    for (int i = 1; i < len; i++) {
        buffer[i] = addrs[i - 1];
    }

    bool ret = spi_write(len, buffer);
    free(buffer);
    return ret;
}

bool vfd_direct_write_vram(uint8_t begin_idx, uint8_t* ram_grid,
                           uint8_t num_grid) {
    if (begin_idx > MAX_IDX) {
        begin_idx = MAX_IDX;
    }

    if (num_grid + begin_idx > NUM_GRID) {
        num_grid = NUM_GRID - begin_idx;
    }

    int len = GRID_SIZE * num_grid + 1;
    uint8_t* buffer = calloc(len, 1);
    buffer[0] = CG_ADDR_START + begin_idx;
    for (int i = 1; i < len; i++) {
        buffer[i] = ram_grid[i - 1];
    }

    bool ret = spi_write(len, buffer);
    free(buffer);
    return ret;
}

// -----------------------
// vfd high level
// -----------------------

bool vfd_update_all_vram(void) {
    return vfd_direct_write_vram(0, flatten_vram(), 8);
}

bool vfd_display_at(uint8_t idx, uint8_t vram_addr) {
    return vfd_write_dc(idx, &vram_addr, 1);
}

bool vfd_display_all_vram(void) {
    return vfd_write_dc(0, (uint8_t[]){0, 1, 2, 3, 4, 5, 6, 7}, 8);
}

bool vfd_display_string(uint8_t idx, const char* str) {
    size_t len = strlen(str);
    return vfd_write_dc(idx, (uint8_t*)str, len);
}

// -----------------------
// VFD init sequence
// -----------------------

void vfd_init(void) {
    uint8_t dim[] = {0xe0, 0x07};
    uint8_t lightness[] = {0xe4, 0x3F};
    uint8_t show[] = {0xe8};
    spi_write(sizeof(dim), dim);
    spi_write(sizeof(lightness), lightness);
    spi_write(sizeof(show), show);
}
