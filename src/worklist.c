#include "worklist.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

static worklist* list_head = NULL;

void init_worklist(void) {
    list_head = calloc(sizeof(worklist), 1);
    list_head->next = list_head;
}

worklist* add_to_worklist(workload_func* func, uint32_t repeat,
                          uint32_t inter_delay_ms, uint32_t post_delay_ms) {
    worklist* new_entry = calloc(sizeof(worklist), 1);
    new_entry->func_ptr = func;
    new_entry->repeat = repeat;
    new_entry->inter_delay_ms = inter_delay_ms;
    new_entry->post_delay_ms = post_delay_ms;
    new_entry->next = list_head;
    worklist* current = list_head;
    while (current->next != list_head) {
        current = current->next;
    }
    current->next = new_entry;
    return new_entry;
}

void free_worklist(void) {
    worklist* current = list_head->next;
    while (current != list_head) {
        worklist* next = current->next;
        free(current);
        current = next;
    }
    free(list_head);
    list_head = NULL;
}

void main_loop(void) {
    worklist* current = list_head;
    uint32_t repeat = current->repeat;
    while (true) {
        if (!current->func_ptr || !repeat) {
            usleep(current->post_delay_ms * 1e3);
            current = current->next;
            repeat = current->repeat;
            continue;
        }

        current->func_ptr();
        repeat--;
        usleep(current->inter_delay_ms * 1e3);
    }
}
