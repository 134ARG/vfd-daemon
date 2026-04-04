#define _GNU_SOURCE
#include "spi.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include <signal.h>

#include "ch347_lib.h"

// SIGUSR1 handler — intentionally empty. Interrupts blocked syscalls
// without SA_RESTART so write() returns EINTR.
static void ch347_sig_handler(int signum) { (void)signum; }

void ch347_setup_signal(void) {
    struct sigaction sa = {0};
    sa.sa_handler = ch347_sig_handler;
    sigemptyset(&sa.sa_mask);
    // No SA_RESTART — we want write() to fail with EINTR
    sigaction(SIGUSR1, &sa, NULL);
}

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
static const char* ch347_device_path_saved = NULL;

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
    ch347_config.iClock = 0x04;
    ch347_config.iByteOrder = 0x00;
    ch347_config.iSpiWriteReadInterval = 0x0002;
    ch347_config.iSpiOutDefaultData = 0xff;
    ch347_config.iChipSelect = 0x8000;
    ch347_config.CS1Polarity = 0x00;
    ch347_config.CS2Polarity = 0x00;
    ch347_config.iIsAutoDeativeCS = 0x0001;
    ch347_config.iActiveDelay = 0x0002;
    ch347_config.iDelayDeactive = 0x0002;

    ch347_device_path_saved = device_path;
    ch347_fd = CH347OpenDevice(device_path);
    if (ch347_fd == -1) {
        fprintf(stderr, "CH347: failed to open %s\n", device_path);
        return false;
    }

    // Set USB timeout before SPI init — may only work with vendor driver
    if (!CH34xSetTimeout(ch347_fd, 50, 50)) {
        fprintf(stderr, "CH347: CH34xSetTimeout not supported "
                "(HID mode), using thread-based timeout\n");
    }

    if (!CH347SPI_Init(ch347_fd, &ch347_config)) {
        fprintf(stderr, "CH347: failed to init SPI\n");
        return false;
    }

    fprintf(stderr, "CH347: SPI init success on %s\n", device_path);
    return true;
}

// CH347 write with SIGUSR1-based timeout.
// Worker thread does the blocking CH347SPI_Write. On timeout, main
// thread sends SIGUSR1 to interrupt the blocked write() syscall,
// then joins the worker cleanly.

typedef struct {
    int fd;
    int len;
    uint8_t buf[512];
    bool result;
} ch347_write_arg_t;

static void* ch347_write_worker(void* arg) {
    ch347_write_arg_t* a = (ch347_write_arg_t*)arg;
    a->result = CH347SPI_Write(a->fd, false, 0x80, a->len, 1, a->buf);
    return NULL;
}

#define CH347_WRITE_TIMEOUT_MS 50
#define CH347_WRITE_RETRIES 3

static bool ch347_stalled = false;

static bool ch347_timed_write(int fd, int len, uint8_t* buffer) {
    ch347_write_arg_t arg = { .fd = fd, .len = len, .result = false };
    memcpy(arg.buf, buffer, len);

    pthread_t tid;
    if (pthread_create(&tid, NULL, ch347_write_worker, &arg) != 0)
        return false;

    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    long ns = deadline.tv_nsec + (long)CH347_WRITE_TIMEOUT_MS * 1000000L;
    deadline.tv_sec += ns / 1000000000L;
    deadline.tv_nsec = ns % 1000000000L;

    int rc = pthread_timedjoin_np(tid, NULL, &deadline);
    if (rc != 0) {
        // Timed out — interrupt the blocked syscall and join
        pthread_kill(tid, SIGUSR1);
        pthread_join(tid, NULL);
        fprintf(stderr, "CH347: SPI write timed out (%dms)\n",
                CH347_WRITE_TIMEOUT_MS);
        return false;
    }

    return arg.result;
}

static bool ch347_write(int len, uint8_t* buffer) {
    if (ch347_stalled)
        return false;

    if (len > (int)sizeof(((ch347_write_arg_t*)0)->buf)) {
        fprintf(stderr, "CH347: write too large (%d)\n", len);
        return false;
    }

    for (int i = 0; i < CH347_WRITE_RETRIES; i++) {
        if (ch347_timed_write(ch347_fd, len, buffer))
            return true;
        fprintf(stderr, "CH347: retry %d/%d\n", i + 1, CH347_WRITE_RETRIES);
    }

    fprintf(stderr, "CH347: all retries exhausted, marking stalled\n");
    ch347_stalled = true;
    return false;
}

static void ch347_close(void) {
    if (ch347_fd >= 0) {
        CH347CloseDevice(ch347_fd);
        ch347_fd = -1;
    }
}

static bool ch347_reinit(void) {
    fprintf(stderr, "CH347: attempting full SPI re-init\n");
    ch347_close();
    usleep(500000); // 500ms settle — USB controller needs time

    const char* path = ch347_device_path_saved;
    if (!path) {
        path = ch347_auto_detect();
        if (!path) {
            fprintf(stderr, "CH347: re-init failed, device not found\n");
            return false;
        }
    }

    if (!ch347_init(path)) {
        fprintf(stderr, "CH347: re-init failed\n");
        return false;
    }
    ch347_stalled = false;
    fprintf(stderr, "CH347: re-init success\n");
    return true;
}

spi_backend_t ch347_backend = {
    .init = ch347_init,
    .write = ch347_write,
    .close = ch347_close,
};

// -----------------------
// Linux spidev backend
// -----------------------

#ifndef SPI_IOC_WR_LSB_FIRST
#define SPI_IOC_WR_LSB_FIRST _IOW(SPI_IOC_MAGIC, 2, uint8_t)
#endif

static int spidev_fd = -1;
#define SPIDEV_MAX_CHUNK 4096

static bool spidev_init(const char* device_path) {
    spidev_fd = open(device_path, O_RDWR);
    if (spidev_fd < 0) {
        perror("spidev: open");
        return false;
    }

    uint8_t mode = SPI_MODE_3;
    uint8_t bits = 8;
    // Align with CH347 iClock = 0x03 (7.5 MHz)
    uint32_t speed = 1000000; 

    if (ioctl(spidev_fd, SPI_IOC_WR_MODE, &mode) < 0 ||
        ioctl(spidev_fd, SPI_IOC_WR_BITS_PER_WORD, &bits) < 0 ||
        ioctl(spidev_fd, SPI_IOC_WR_MAX_SPEED_HZ, &speed) < 0) {
        perror("spidev: ioctl config");
        close(spidev_fd);
        spidev_fd = -1;
        return false;
    }

    uint8_t lsb_first = 1; 

if (ioctl(spidev_fd, SPI_IOC_WR_LSB_FIRST, &lsb_first) < 0) {
    perror("spidev: failed to set LSB first");
    // You might want to continue anyway, some drivers don't support 
    // the ioctl and require you to reverse bits in software, but 
    // try the ioctl first!
}

    // Reminder: If CH347 was using 0x80 (CS1), ensure your device_path 
    // is /dev/spidevX.1, not /dev/spidevX.0
    fprintf(stderr, "spidev: init success on %s (mode 3, %d Hz)\n",
            device_path, speed);
    return true;
}

static bool spidev_write(int len, uint8_t* buffer) {
    // Calculate how many 4KB chunks we need
    int num_chunks = (len + SPIDEV_MAX_CHUNK - 1) / SPIDEV_MAX_CHUNK;
    
    // Create an array of transfers
    struct spi_ioc_transfer xfers[num_chunks];
    memset(xfers, 0, sizeof(xfers));

    for (int i = 0; i < num_chunks; i++) {
        int offset = i * SPIDEV_MAX_CHUNK;
        int chunk_len = len - offset;
        if (chunk_len > SPIDEV_MAX_CHUNK) {
            chunk_len = SPIDEV_MAX_CHUNK;
        }

        xfers[i].tx_buf = (uint64_t)(uintptr_t)(buffer + offset);
        xfers[i].len = chunk_len;
        xfers[i].speed_hz = 1000000; // 7.5 MHz to match CH347T
        xfers[i].bits_per_word = 8;
        xfers[i].delay_usecs = 10; // Pause for 10 microseconds after this chunk
        
        // cs_change = 0 tells the kernel: 
        // "Do NOT toggle CS between these chunks. Only let CS go HIGH 
        // after the entire SPI_IOC_MESSAGE is finished."
        xfers[i].cs_change = 0; 
    }

    // Send all chunks to the kernel in a single command
    if (ioctl(spidev_fd, SPI_IOC_MESSAGE(num_chunks), xfers) < 0) {
        perror("spidev: write array failed");
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
// SPI health tracking
// -----------------------

#include <time.h>

static int spi_fail_count = 0;
static time_t spi_last_ok = 0;

#define SPI_MAX_CONSECUTIVE_FAILS 1

int spi_get_fail_count(void) { return spi_fail_count; }

// -----------------------
// SPI write wrapper
// -----------------------

bool spi_write(int len, uint8_t* buffer) {
    // If we've hit too many consecutive failures, try to recover
    if (spi_fail_count >= SPI_MAX_CONSECUTIVE_FAILS) {
        fprintf(stderr, "SPI: %d consecutive failures, attempting recovery\n",
                spi_fail_count);
        if (spi == &ch347_backend) {
            if (ch347_reinit()) {
                spi_fail_count = 0;
                // Re-init the VFD and restore display state
                extern void vfd_init(void);
                vfd_init();
                extern bool vfd_update_all_vram(void);
                extern bool vfd_display_all_vram(void);
                vfd_update_all_vram();
                vfd_display_all_vram();
            } else {
                // Back off before next attempt
                usleep(500000);
                return false;
            }
        }
    }

    bool ok = spi->write(len, buffer);
    if (ok) {
        if (spi_fail_count > 0) {
            fprintf(stderr, "SPI: recovered after %d consecutive failures\n",
                    spi_fail_count);
        }
        spi_fail_count = 0;
        spi_last_ok = time(NULL);
    } else {
        spi_fail_count++;
        if (spi_fail_count <= 3 || (spi_fail_count % 50) == 0) {
            fprintf(stderr, "SPI: write failed (%d consecutive)\n",
                    spi_fail_count);
        }
    }
    return ok;
}
