#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum { SPI_MODE_CH347, SPI_MODE_SPIDEV } spi_mode_t;

typedef struct {
    bool (*init)(const char* device_path);
    bool (*write)(int len, uint8_t* buffer);
    void (*close)(void);
} spi_backend_t;

extern spi_backend_t* spi;
extern spi_backend_t ch347_backend;
extern spi_backend_t spidev_backend;

bool spi_write(int len, uint8_t* buffer);
int spi_get_fail_count(void);
char* ch347_auto_detect(void);
void ch347_setup_signal(void);
