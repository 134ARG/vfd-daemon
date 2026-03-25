#include "spi.h"

#include <dirent.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "ch347_lib.h"

// spidev ioctl definitions (avoid including linux/spi/spidev.h which
// conflicts with ch347 headers)
#ifndef SPI_IOC_MAGIC
#define SPI_IOC_MAGIC 'k'
#define SPI_MODE_3 0x03
struct spi_ioc_transfer {
    uint64_t tx_buf;
    uint64_t rx_buf;
    uint32_t len;
    uint32_t speed_hz;
    uint16_t delay_usecs;
    uint8_t bits_per_word;
    uint8_t cs_change;
    uint8_t tx_nbits;
    uint8_t rx_nbits;
    uint8_t word_delay_usecs;
    uint8_t pad;
};
#define SPI_IOC_WR_MODE _IOW(SPI_IOC_MAGIC, 1, uint8_t)
#define SPI_IOC_WR_BITS_PER_WORD _IOW(SPI_IOC_MAGIC, 3, uint8_t)
#define SPI_IOC_WR_MAX_SPEED_HZ _IOW(SPI_IOC_MAGIC, 4, uint32_t)
#define SPI_IOC_MESSAGE(N) _IOW(SPI_IOC_MAGIC, 0, struct spi_ioc_transfer[N])
#endif

spi_backend_t* spi = NULL;

// -----------------------
// CH347 backend
// -----------------------

static int ch347_fd = -1;
static mSpiCfgS ch347_config = {0};

// Auto-detect CH347T hidraw device by scanning sysfs for VID:PID 1a86:55dc
// The CH347T exposes two HID interfaces: input0 (UART) and input1 (SPI+I2C).
// We want input1.
char* ch347_auto_detect(void) {
    DIR* d = opendir("/sys/class/hidraw");
    if (!d) return NULL;

    struct dirent* ent;
    char* best = NULL;
    while ((ent = readdir(d)) != NULL) {
        if (strncmp(ent->d_name, "hidraw", 6) != 0) continue;

        char uevent_path[512];
        snprintf(uevent_path, sizeof(uevent_path),
                 "/sys/class/hidraw/%s/device/uevent", ent->d_name);

        FILE* fp = fopen(uevent_path, "r");
        if (!fp) continue;

        char line[256];
        bool vid_pid_match = false;
        bool is_spi_iface = false;
        while (fgets(line, sizeof(line), fp)) {
            if (strstr(line, "1A86") && strstr(line, "55DC"))
                vid_pid_match = true;
            // input1 = SPI+I2C interface
            if (strstr(line, "HID_PHYS=") && strstr(line, "/input1"))
                is_spi_iface = true;
        }
        fclose(fp);

        if (vid_pid_match && is_spi_iface) {
            best = malloc(64);
            snprintf(best, 64, "/dev/%s", ent->d_name);
            break;
        }
    }
    closedir(d);

    if (best)
        fprintf(stderr, "CH347: auto-detected %s\n", best);
    return best;
}

static bool ch347_init(const char* device_path) {
    ch347_config.iMode = 0x03;
    ch347_config.iClock = 0x03;
    ch347_config.iByteOrder = 0x00;
    ch347_config.iSpiWriteReadInterval = 0x0002;
    ch347_config.iSpiOutDefaultData = 0xff;
    ch347_config.iChipSelect = 0x8000;
    ch347_config.CS1Polarity = 0x00;
    ch347_config.CS2Polarity = 0x00;
    ch347_config.iIsAutoDeativeCS = 0x0001;
    ch347_config.iActiveDelay = 0x0002;
    ch347_config.iDelayDeactive = 0x0002;

    ch347_fd = CH347OpenDevice(device_path);
    if (ch347_fd == -1) {
        fprintf(stderr, "CH347: failed to open %s\n", device_path);
        return false;
    }

    if (!CH347SPI_Init(ch347_fd, &ch347_config)) {
        fprintf(stderr, "CH347: failed to init SPI\n");
        return false;
    }

    fprintf(stderr, "CH347: SPI init success on %s\n", device_path);
    return true;
}

static bool ch347_write(int len, uint8_t* buffer) {
    if (!CH347SPI_Write(ch347_fd, false, 0x80, len, 1, buffer)) {
        fprintf(stderr, "CH347: SPI write failed\n");
        return false;
    }
    return true;
}

static void ch347_close(void) {
    if (ch347_fd >= 0) {
        CH347CloseDevice(ch347_fd);
        ch347_fd = -1;
    }
}

spi_backend_t ch347_backend = {
    .init = ch347_init,
    .write = ch347_write,
    .close = ch347_close,
};

// -----------------------
// Linux spidev backend
// -----------------------

static int spidev_fd = -1;

static bool spidev_init(const char* device_path) {
    spidev_fd = open(device_path, O_RDWR);
    if (spidev_fd < 0) {
        perror("spidev: open");
        return false;
    }

    uint8_t mode = SPI_MODE_3;
    uint8_t bits = 8;
    uint32_t speed = 500000; // 500kHz, safe default

    if (ioctl(spidev_fd, SPI_IOC_WR_MODE, &mode) < 0 ||
        ioctl(spidev_fd, SPI_IOC_WR_BITS_PER_WORD, &bits) < 0 ||
        ioctl(spidev_fd, SPI_IOC_WR_MAX_SPEED_HZ, &speed) < 0) {
        perror("spidev: ioctl config");
        close(spidev_fd);
        spidev_fd = -1;
        return false;
    }

    fprintf(stderr, "spidev: init success on %s (mode 3, %d Hz)\n",
            device_path, speed);
    return true;
}

static bool spidev_write(int len, uint8_t* buffer) {
    struct spi_ioc_transfer xfer = {
        .tx_buf = (unsigned long)buffer,
        .len = len,
        .speed_hz = 500000,
        .bits_per_word = 8,
    };

    if (ioctl(spidev_fd, SPI_IOC_MESSAGE(1), &xfer) < 0) {
        perror("spidev: write");
        return false;
    }
    return true;
}

static void spidev_close(void) {
    if (spidev_fd >= 0) {
        close(spidev_fd);
        spidev_fd = -1;
    }
}

spi_backend_t spidev_backend = {
    .init = spidev_init,
    .write = spidev_write,
    .close = spidev_close,
};

// -----------------------
// SPI write wrapper
// -----------------------

bool spi_write(int len, uint8_t* buffer) { return spi->write(len, buffer); }
