#pragma once

#include <stdbool.h>
#include <stdint.h>

#define ASCII_START 0x30
#define DC_ADDR_START 0x20
#define CG_ADDR_START 0x40

#define GRID_SIZE 5
#define NUM_GRID 8
#define MAX_IDX (NUM_GRID - 1)

#define COL_SIZE 8
#define NUM_COL (GRID_SIZE * NUM_GRID)

extern uint8_t vram[NUM_GRID][GRID_SIZE];
extern const uint8_t util_bars[COL_SIZE];
extern const uint8_t partial_col[8];
extern const uint8_t partial_col_up[8];

uint8_t* flatten_vram(void);
uint8_t get_util_bar(uint8_t idx);

bool vfd_write_dc(uint8_t begin_idx, const uint8_t* addrs, uint8_t len);
bool vfd_direct_write_vram(uint8_t begin_idx, uint8_t* ram_grid,
                           uint8_t num_grid);
bool vfd_update_all_vram(void);
bool vfd_display_at(uint8_t idx, uint8_t vram_addr);
bool vfd_display_all_vram(void);
bool vfd_display_string(uint8_t idx, const char* str);

void vfd_init(void);
