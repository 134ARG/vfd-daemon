#include "remote.h"

#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

// -----------------------
// Remote metrics (TCP client)
// -----------------------

typedef struct {
    double cpu_util;
    double ram_util;
    double gpu_util;
    double cpu_temp;
    double gpu_temp;
    double net_rx_bytes;
    double net_tx_bytes;
    int uptime_sec;
    int failed_units;
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
    const char* temp = json_find_object(json, "temp");
    const char* net = json_find_object(json, "net");

    pthread_mutex_lock(&remote_metrics.lock);
    if (cpu) remote_metrics.cpu_util = json_get_number(cpu, "util");
    if (ram) remote_metrics.ram_util = json_get_number(ram, "util");
    if (gpu) remote_metrics.gpu_util = json_get_number(gpu, "util");
    if (temp) {
        remote_metrics.cpu_temp = json_get_number(temp, "cpu");
        remote_metrics.gpu_temp = json_get_number(temp, "gpu");
    }
    if (net) {
        remote_metrics.net_rx_bytes = json_get_number(net, "rx_bytes");
        remote_metrics.net_tx_bytes = json_get_number(net, "tx_bytes");
    }
    const char* sys = json_find_object(json, "sys");
    if (sys) remote_metrics.uptime_sec = (int)json_get_number(sys, "uptime");
    if (sys) remote_metrics.failed_units = (int)json_get_number(sys, "failed_units");
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

void start_remote_reader(int port) {
    static int listen_port;
    listen_port = port;
    pthread_t tid;
    pthread_create(&tid, NULL, remote_reader_thread, &listen_port);
    pthread_detach(tid);
}

void get_remote_metrics_full(metrics_snapshot_t* out) {
    pthread_mutex_lock(&remote_metrics.lock);
    out->cpu_util = remote_metrics.cpu_util;
    out->ram_util = remote_metrics.ram_util;
    out->gpu_util = remote_metrics.gpu_util;
    out->cpu_temp = remote_metrics.cpu_temp;
    out->gpu_temp = remote_metrics.gpu_temp;
    out->net_rx_bytes = remote_metrics.net_rx_bytes;
    out->net_tx_bytes = remote_metrics.net_tx_bytes;
    out->uptime_sec = remote_metrics.uptime_sec;
    out->failed_units = remote_metrics.failed_units;
    pthread_mutex_unlock(&remote_metrics.lock);
}

void get_remote_metrics(double* cpu, double* ram, double* gpu) {
    pthread_mutex_lock(&remote_metrics.lock);
    *cpu = remote_metrics.cpu_util;
    *ram = remote_metrics.ram_util;
    *gpu = remote_metrics.gpu_util;
    pthread_mutex_unlock(&remote_metrics.lock);
}

bool is_remote_connected(void) {
    pthread_mutex_lock(&remote_metrics.lock);
    bool c = remote_metrics.connected;
    pthread_mutex_unlock(&remote_metrics.lock);
    return c;
}
