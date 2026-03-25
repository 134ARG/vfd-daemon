#include <fcntl.h>
#include <getopt.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
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

#define ASCII_START 0x30
#define DC_ADDR_START 0x20
#define CG_ADDR_START 0x40

#define GRID_SIZE 5
#define NUM_GRID 8
#define MAX_IDX (NUM_GRID - 1)

#define COL_SIZE 8
#define NUM_COL (GRID_SIZE * NUM_GRID)

// -----------------------
// SPI backend abstraction
// -----------------------

typedef enum { SPI_MODE_CH347, SPI_MODE_SPIDEV } spi_mode_t;

typedef struct {
    bool (*init)(const char* device_path);
    bool (*write)(int len, uint8_t* buffer);
    void (*close)(void);
} spi_backend_t;

static spi_backend_t* spi = NULL;

// -----------------------
// CH347 backend
// -----------------------

static int ch347_fd = -1;
static mSpiCfgS ch347_config = {0};

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

static spi_backend_t ch347_backend = {
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

static spi_backend_t spidev_backend = {
    .init = spidev_init,
    .write = spidev_write,
    .close = spidev_close,
};

// -----------------------
// SPI write wrapper
// -----------------------

bool spi_write(int len, uint8_t* buffer) { return spi->write(len, buffer); }

// -----------------------
// VFD constants
// -----------------------

// progress bar
const uint8_t util_bars[COL_SIZE] = {
    0x00, 0x40, 0x60, 0x70, 0x78, 0x7c, 0x7e, 0x7f,
};

uint8_t get_util_bar(uint8_t idx) { return util_bars[idx]; }

uint8_t vram[NUM_GRID][GRID_SIZE] = {0};

uint8_t* flatten_vram(void) { return (uint8_t*)vram; }

// -----------------------
// Worklist
// -----------------------

typedef void(workload_func)(void);

typedef struct worklist {
    workload_func* func_ptr;
    uint32_t repeat;
    uint32_t inter_delay_ms;
    uint32_t post_delay_ms;
    struct worklist* next;
} worklist;

worklist* list_head = NULL;

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
// Remote metrics (TCP client)
// -----------------------

typedef struct {
    double cpu_util;
    double ram_util;
    double gpu_util;
    time_t last_update;
    pthread_mutex_t lock;
    bool connected;
} remote_metrics_t;

static remote_metrics_t remote_metrics = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
};

// Minimal JSON number extractor: find "key":value and return the number
static double json_get_number(const char* json, const char* key) {
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    const char* p = strstr(json, pattern);
    if (!p) return 0.0;
    p += strlen(pattern);
    while (*p == ' ') p++;
    return atof(p);
}

// Find a nested object by key and return pointer to its content
static const char* json_find_object(const char* json, const char* key) {
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    const char* p = strstr(json, pattern);
    if (!p) return NULL;
    p += strlen(pattern);
    while (*p == ' ') p++;
    if (*p == '{') return p;
    return p; // might be a value, not object
}

static void parse_metrics_json(const char* json) {
    const char* cpu = json_find_object(json, "cpu");
    const char* ram = json_find_object(json, "ram");
    const char* gpu = json_find_object(json, "gpu");

    pthread_mutex_lock(&remote_metrics.lock);
    if (cpu) remote_metrics.cpu_util = json_get_number(cpu, "util");
    if (ram) remote_metrics.ram_util = json_get_number(ram, "util");
    if (gpu) remote_metrics.gpu_util = json_get_number(gpu, "util");
    remote_metrics.last_update = time(NULL);
    remote_metrics.connected = true;
    pthread_mutex_unlock(&remote_metrics.lock);
}

static int tcp_listen(int port) {
    int srv = socket(AF_INET6, SOCK_STREAM, 0);
    if (srv < 0) {
        perror("socket");
        return -1;
    }

    int opt = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    // Allow both IPv4 and IPv6
    int off = 0;
    setsockopt(srv, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof(off));

    struct sockaddr_in6 addr = {
        .sin6_family = AF_INET6,
        .sin6_port = htons(port),
        .sin6_addr = in6addr_any,
    };

    if (bind(srv, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(srv);
        return -1;
    }

    if (listen(srv, 2) < 0) {
        perror("listen");
        close(srv);
        return -1;
    }

    return srv;
}

static void* remote_reader_thread(void* arg) {
    int listen_port = *(int*)arg;
    char buf[4096];
    int buf_len = 0;

    int srv_fd = tcp_listen(listen_port);
    if (srv_fd < 0) {
        fprintf(stderr, "Remote: failed to listen on port %d\n", listen_port);
        return NULL;
    }
    fprintf(stderr, "Remote: listening on port %d\n", listen_port);

    while (1) {
        struct sockaddr_storage client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int fd = accept(srv_fd, (struct sockaddr*)&client_addr, &addr_len);
        if (fd < 0) {
            perror("accept");
            sleep(1);
            continue;
        }
        fprintf(stderr, "Remote: agent connected\n");
        pthread_mutex_lock(&remote_metrics.lock);
        remote_metrics.connected = true;
        pthread_mutex_unlock(&remote_metrics.lock);
        buf_len = 0;

        while (1) {
            int n = read(fd, buf + buf_len, sizeof(buf) - buf_len - 1);
            if (n <= 0) break;
            buf_len += n;
            buf[buf_len] = '\0';

            char* line_start = buf;
            char* newline;
            while ((newline = strchr(line_start, '\n')) != NULL) {
                *newline = '\0';
                if (newline > line_start) {
                    parse_metrics_json(line_start);
                }
                line_start = newline + 1;
            }

            int remaining = buf_len - (line_start - buf);
            if (remaining > 0) {
                memmove(buf, line_start, remaining);
            }
            buf_len = remaining;
        }

        close(fd);
        fprintf(stderr, "Remote: agent disconnected, waiting for reconnect\n");
        pthread_mutex_lock(&remote_metrics.lock);
        remote_metrics.connected = false;
        pthread_mutex_unlock(&remote_metrics.lock);
    }
    return NULL;
}

static void start_remote_reader(int port) {
    static int listen_port;
    listen_port = port;
    pthread_t tid;
    pthread_create(&tid, NULL, remote_reader_thread, &listen_port);
    pthread_detach(tid);
}

// Get remote metrics snapshot (thread-safe)
static void get_remote_metrics(double* cpu, double* ram, double* gpu) {
    pthread_mutex_lock(&remote_metrics.lock);
    *cpu = remote_metrics.cpu_util;
    *ram = remote_metrics.ram_util;
    *gpu = remote_metrics.gpu_util;
    pthread_mutex_unlock(&remote_metrics.lock);
}

// -----------------------
// workloads
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
// CPU utilization monitor
// -----------------------
// Layout: [C][P][U][bar3][bar4][bar5][bar6][bar7]
// Grids 3-7 = 5 grids × 5 cols = 25 columns for the bar
// Fills from right (grid7 col4) to left (grid3 col0)
// Boundary column uses partial row fill for sub-column resolution

#define BAR_START_GRID 3
#define BAR_NUM_GRIDS 5
#define BAR_TOTAL_COLS (BAR_NUM_GRIDS * GRID_SIZE) // 25

#define COL_FULL 0x7f
#define COL_EMPTY 0x00

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

// Partial fill: index 0..7 = how many rows lit from bottom
static const uint8_t partial_col[8] = {
    0x00, 0x01, 0x03, 0x07, 0x0f, 0x1f, 0x3f, 0x7f,
};

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
// CPU util monitor - theme 2: bordered bar
// -----------------------
// Layout: [C][P][U][bar3][bar4][bar5][bar6][bar7]
// A 1px rectangular border spanning all 25 columns:
//   - Top row (bit6) + bottom row (bit0) always on = 0x41
//   - Left edge (grid3 col0) and right edge (grid7 col4) = 0x7f
// Fill: solid vertical lines (0x7f) from right to left inside the box

#define BORDER_TB 0x41  // top + bottom row only
#define BORDER_FULL 0x7f

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
// Layout: [C][R][G][bar3][bar4][bar5][bar6][bar7]
// Grids 3-7: 25 columns, 7 rows per column
//   CPU: rows 5-6 (bits 5,6) — 2 rows × 25 cols = 50 steps
//   sep: row 4 (bit 4) — always off (gap)
//   RAM: rows 2-3 (bits 2,3) — 2 rows × 25 cols = 50 steps
//   sep: row 1 (bit 1) — always off (gap)
//   GPU: row 0 (bit 0) — 1 row × 25 cols = 25 steps... too few
// Better: CPU rows 5-6, RAM rows 3-4, GPU rows 1-2, separators rows 0 (unused)
// Actually: 3 bars × 2 rows = 6, + 1 unused = 7. No separators needed if
// the bars are visually distinct by position.
//
// Final layout (top to bottom):
//   bit 6: CPU top
//   bit 5: CPU bottom
//   bit 4: (separator - always off)
//   bit 3: RAM top
//   bit 2: RAM bottom
//   bit 1: (separator - always off)
//   bit 0: GPU (single row — 25 steps)
//
// With 2 rows: fill_cols full columns + partial (0 or 1 extra row)
// Each bar: right to left, theme-1 style (pixel-level resolution)

#define CPU_BITS_FULL 0x60  // bits 5+6
#define CPU_BIT_PARTIAL 0x20 // bit 5 only (bottom row of CPU band)
#define RAM_BITS_FULL 0x0c  // bits 2+3
#define RAM_BIT_PARTIAL 0x04 // bit 2 only (bottom row of RAM band)
#define GPU_BIT_FULL 0x01   // bit 0

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
        unsigned long load = 0, total = 0;
        if (fscanf(fp, "%lu@%*lu", &load) == 1) {
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
// Theme 3 remote: Triple stacked bars from remote metrics
// -----------------------

static float rtri_cpu_level = 0.0f;
static float rtri_ram_level = 0.0f;
static float rtri_gpu_level = 0.0f;

void remote_triple_bar_monitor(void) {
    static bool first = true;

    if (first) {
        first = false;
        for (int g = BAR_START_GRID; g < BAR_START_GRID + BAR_NUM_GRIDS; g++)
            memset(vram[g], 0, GRID_SIZE);
        vfd_update_all_vram();
        vfd_write_dc(0, (uint8_t[]){'C', 'R', 'G', 3, 4, 5, 6, 7}, 8);
        uint8_t show[] = {0xe8};
        spi_write(sizeof(show), show);
        return;
    }

    double cpu_pct, ram_pct, gpu_pct;
    get_remote_metrics(&cpu_pct, &ram_pct, &gpu_pct);

    if (cpu_pct > 100.0) cpu_pct = 100.0;
    if (ram_pct > 100.0) ram_pct = 100.0;
    if (gpu_pct > 100.0) gpu_pct = 100.0;

    float cpu_target = (float)(cpu_pct / 100.0 * 50);
    float ram_target = (float)(ram_pct / 100.0 * 50);
    float gpu_target = (float)(gpu_pct / 100.0 * 25);

    float d;
    d = cpu_target - rtri_cpu_level;
    rtri_cpu_level += (d > 1.0f || d < -1.0f) ? d * 0.3f : d;
    d = ram_target - rtri_ram_level;
    rtri_ram_level += (d > 1.0f || d < -1.0f) ? d * 0.3f : d;
    d = gpu_target - rtri_gpu_level;
    rtri_gpu_level += (d > 1.0f || d < -1.0f) ? d * 0.3f : d;

    if (rtri_cpu_level < 0) rtri_cpu_level = 0;
    if (rtri_cpu_level > 50) rtri_cpu_level = 50;
    if (rtri_ram_level < 0) rtri_ram_level = 0;
    if (rtri_ram_level > 50) rtri_ram_level = 50;
    if (rtri_gpu_level < 0) rtri_gpu_level = 0;
    if (rtri_gpu_level > 25) rtri_gpu_level = 25;

    for (int g = BAR_START_GRID; g < BAR_START_GRID + BAR_NUM_GRIDS; g++)
        memset(vram[g], 0, GRID_SIZE);

    render_2row_bar((int)(rtri_cpu_level + 0.5f), CPU_BITS_FULL, CPU_BIT_PARTIAL);
    render_2row_bar((int)(rtri_ram_level + 0.5f), RAM_BITS_FULL, RAM_BIT_PARTIAL);
    render_1row_bar((int)(rtri_gpu_level + 0.5f), GPU_BIT_FULL);

    vfd_update_all_vram();
    vfd_write_dc(0, (uint8_t[]){'C', 'R', 'G', 3, 4, 5, 6, 7}, 8);

    uint8_t show[] = {0xe8};
    spi_write(sizeof(show), show);
}

// -----------------------
// Remote CPU-only monitor (theme 1 with remote data)
// -----------------------

static float remote_displayed_level = 0.0f;

void remote_cpu_monitor(void) {
    static bool first = true;

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

const uint32_t duty_ns = 50e3;

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

// -----------------------
// CLI and main
// -----------------------

static void usage(const char* prog) {
    fprintf(stderr,
            "Usage: %s -m <ch347|spidev> [-d <device_path>] [-t <1|2|3>] "
            "[-s <host:port>]\n"
            "  -m  SPI backend mode (required)\n"
            "  -d  device path (default: /dev/hidraw6 for ch347, "
            "/dev/spidev0.0 for spidev)\n"
            "  -t  display theme (default: 1)\n"
            "      1 = CPU filled bar\n"
            "      2 = CPU bordered bar\n"
            "      3 = CPU+RAM+GPU stacked bars\n"
            "  -s  listen port for remote metric agent (e.g. -s 9100)\n"
            "  -h  show this help\n",
            prog);
}

int main(int argc, char* argv[]) {
    spi_mode_t mode = -1;
    const char* device_path = NULL;
    const char* source_arg = NULL;
    int theme = 1;
    int opt;

    while ((opt = getopt(argc, argv, "m:d:t:s:h")) != -1) {
        switch (opt) {
            case 'm':
                if (strcmp(optarg, "ch347") == 0) {
                    mode = SPI_MODE_CH347;
                } else if (strcmp(optarg, "spidev") == 0) {
                    mode = SPI_MODE_SPIDEV;
                } else {
                    fprintf(stderr, "Unknown mode: %s\n", optarg);
                    usage(argv[0]);
                    return 1;
                }
                break;
            case 'd':
                device_path = optarg;
                break;
            case 't':
                theme = atoi(optarg);
                if (theme < 1 || theme > 3) {
                    fprintf(stderr, "Invalid theme: %s (use 1, 2, or 3)\n", optarg);
                    usage(argv[0]);
                    return 1;
                }
                break;
            case 's':
                source_arg = optarg;
                break;
            case 'h':
            default:
                usage(argv[0]);
                return opt == 'h' ? 0 : 1;
        }
    }

    if ((int)mode == -1) {
        fprintf(stderr, "Error: -m is required\n");
        usage(argv[0]);
        return 1;
    }

    // Set defaults if no device path given
    if (!device_path) {
        device_path =
            (mode == SPI_MODE_CH347) ? "/dev/hidraw6" : "/dev/spidev0.0";
    }

    // Select backend
    spi = (mode == SPI_MODE_CH347) ? &ch347_backend : &spidev_backend;

    if (!spi->init(device_path)) {
        return 1;
    }

    // VFD init
    uint8_t dim[] = {0xe0, 0x07};
    uint8_t lightness[] = {0xe4, 0x3F};
    uint8_t show[] = {0xe8};
    spi_write(sizeof(dim), dim);
    spi_write(sizeof(lightness), lightness);
    spi_write(sizeof(show), show);

    sleep(1);

    // Start remote listener if given
    bool use_remote = false;
    if (source_arg) {
        int listen_port = atoi(source_arg);
        if (listen_port <= 0 || listen_port > 65535) {
            fprintf(stderr, "Invalid port: %s\n", source_arg);
            return 1;
        }
        use_remote = true;
        start_remote_reader(listen_port);
    }

    // Select monitor workload based on theme and source
    init_worklist();
    workload_func* monitor;
    if (use_remote) {
        switch (theme) {
            case 3:  monitor = &remote_triple_bar_monitor; break;
            default: monitor = &remote_cpu_monitor; break;
        }
    } else {
        switch (theme) {
            case 2:  monitor = &cpu_util_line_monitor; break;
            case 3:  monitor = &triple_bar_monitor; break;
            default: monitor = &cpu_util_monitor; break;
        }
    }
    add_to_worklist(monitor, UINT32_MAX, 100, 0); // 10fps

    main_loop();

    // never reach
    free_worklist();
    spi->close();

    return 0;
}
