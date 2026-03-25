#pragma once

#include <stdint.h>

typedef void(workload_func)(void);

typedef struct worklist {
    workload_func* func_ptr;
    uint32_t repeat;
    uint32_t inter_delay_ms;
    uint32_t post_delay_ms;
    struct worklist* next;
} worklist;

void init_worklist(void);
worklist* add_to_worklist(workload_func* func, uint32_t repeat,
                          uint32_t inter_delay_ms, uint32_t post_delay_ms);
void free_worklist(void);
void main_loop(void);
