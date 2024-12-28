#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "ch347_lib.h"

#define DEVICE_PATH "/dev/hidraw9"
#define ASCII_START 0x30
#define DC_ADDR_START 0x20
#define CG_ADDR_START 0x40

#define GRID_SIZE 5
#define NUM_GRID 8
#define MAX_IDX (NUM_GRID - 1)

#define COL_SIZE 8
#define NUM_COL (GRID_SIZE * NUM_GRID)

mSpiCfgS init_config = {0};

// progress bar
const uint8_t util_bars[COL_SIZE] = {
    0x00, 0x40, 0x60, 0x70, 0x78, 0x7c, 0x7e, 0x7f,
};

uint8_t vram[NUM_GRID][GRID_SIZE] = {0};

uint8_t *flatten_vram(void) { return (uint8_t *)vram; }

int usb_fd = 0;

// -----------------------
// Worklist
// -----------------------

typedef void(workload_func)(void);

typedef struct worklist {
    workload_func *func_ptr;
    uint32_t repeat;
    uint32_t inter_delay_ms;
    uint32_t post_delay_ms;
    struct worklist *next;
} worklist;

worklist *list_head = NULL;

void init_worklist(void) {
    list_head = calloc(sizeof(worklist), 1);
    list_head->next = list_head;
}

worklist *add_to_worklist(workload_func *func, uint32_t repeat,
                          uint32_t inter_delay_ms, uint32_t post_delay_ms) {
    worklist *new_entry = calloc(sizeof(worklist), 1);
    new_entry->func_ptr = func;
    new_entry->repeat = repeat;
    new_entry->inter_delay_ms = inter_delay_ms;
    new_entry->post_delay_ms = post_delay_ms;
    new_entry->next = list_head;
    worklist *current = list_head;
    while (current->next != list_head) {
        current = current->next;
    }
    current->next = new_entry;
    return new_entry;
}

void free_worklist(void) {
    worklist *current = list_head->next;
    while (current != list_head) {
        worklist *next = current->next;
        free(current);
        current = next;
    }
    free(list_head);
    list_head = NULL;
}

// -----------------------
// SPI low level
// -----------------------

void init_spi_interface(void) {
    init_config.iMode = 0x03;
    init_config.iClock = 0x03;
    init_config.iByteOrder = 0x00;
    init_config.iSpiWriteReadInterval = 0x0002;
    init_config.iSpiOutDefaultData = 0xff;
    init_config.iChipSelect = 0x8000; // CS2 as the chip select
    init_config.CS1Polarity = 0x00;
    init_config.CS2Polarity = 0x00;
    init_config.iIsAutoDeativeCS = 0x0001;
    init_config.iActiveDelay = 0x0002;
    init_config.iDelayDeactive = 0x0002;

    usb_fd = CH347OpenDevice(DEVICE_PATH);
    if (usb_fd == -1)

    {
        fprintf(stderr, "SPI Init: failed to open the device!\n");
        exit(1);
    }

    if (!CH347SPI_Init(usb_fd, &init_config)) {
        fprintf(stderr, "SPI Init: failed to init spi interface!\n");
        exit(1);
    }

    fprintf(stderr, "SPI Init: success.\n");
}

bool spi_write(int len, uint8_t *buffer) {
    // fprintf(stderr, "DEBUG: SPI write: write %d bytes\n", len);
    if (!CH347SPI_Write(usb_fd, false, 0x80, len, 1, buffer)) {
        fprintf(stderr, "SPI write: failed!\n");
        return false;
    }
    return true;
}

// -----------------------
// vfd low level
// -----------------------

bool vfd_write_dc(uint8_t begin_idx, const uint8_t *addrs, uint8_t len) {
    if (begin_idx > MAX_IDX) {
        begin_idx = MAX_IDX;
    }

    if (len + begin_idx > NUM_GRID) {
        len = NUM_GRID - begin_idx;
    }

    len += 1; // for the command and start address
    uint8_t *buffer = calloc(len, 1);
    buffer[0] = DC_ADDR_START + begin_idx;
    for (int i = 1; i < len; i++) {
        buffer[i] = addrs[i - 1];
    }

    bool ret = spi_write(len, buffer);
    free(buffer);
    return ret;
}

bool vfd_direct_write_vram(uint8_t begin_idx, uint8_t *ram_grid,
                           uint8_t num_grid) {
    if (begin_idx > MAX_IDX) {
        begin_idx = MAX_IDX;
    }

    if (num_grid + begin_idx > NUM_GRID) {
        num_grid = NUM_GRID - begin_idx;
    }

    int len = GRID_SIZE * num_grid + 1; // for the command and start address
    uint8_t *buffer = calloc(len, 1);
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

// -----------------------
// workloads
// -----------------------

void debug_show_util_bars(void) {
    uint8_t *fvram = flatten_vram();

    static uint8_t bar_idx[NUM_COL] = {0};
    static bool initialized = false;
    if (!initialized) {
        for (int i = 0; i < NUM_COL; i++) {
            bar_idx[i] = rand() % COL_SIZE;
        }
        initialized = true;
    }

    for (int i = 0; i < NUM_COL; i++) {
        fvram[i] = util_bars[bar_idx[i]];
    }

    uint8_t max_idx = 2 * (COL_SIZE - 1);
    for (int i = 0; i < NUM_COL; i++) {
        uint8_t idx = bar_idx[i];
        idx = (idx + 1) % max_idx;
        bar_idx[i] = idx;
        if (idx > COL_SIZE - 1) {
            fvram[i] = util_bars[max_idx - idx];
        } else {
            fvram[i] = util_bars[idx];
        }
    }
    vfd_update_all_vram();
    vfd_display_all_vram();
}

// -----------------------
// main work loop
// -----------------------

const uint32_t duty_ns = 50e3;

void main_loop(void) {
    worklist *current = list_head;
    uint32_t repeat = current->repeat;
    while (true) {
        if (!current->func_ptr || !repeat) {
            usleep(current->post_delay_ms * 1e3);
            current = current->next;
            repeat = current->repeat;
            continue;
        }

        // do your work
        current->func_ptr();
        repeat--;
        usleep(current->inter_delay_ms * 1e3);
    }
}

int main() {
    init_spi_interface();
    uint8_t dim[] = {0xe0, 0x07};
    uint8_t lightness[] = {0xe4, 0x3F};
    uint8_t show[] = {0xe8};
    uint8_t set00[] = {0x20, 0x30};
    spi_write(sizeof(dim), dim);
    spi_write(sizeof(lightness), lightness);
    spi_write(sizeof(set00), set00);
    spi_write(sizeof(show), show);
    vfd_write_dc(0, "Init....", 8);

    sleep(1);

    init_worklist();
    add_to_worklist(&debug_show_util_bars, 20, 10, 500);

    main_loop();

    // never reach
    free_worklist();

    return 0;
}
