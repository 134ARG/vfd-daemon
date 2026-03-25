#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <unistd.h>

#include "remote.h"
#include "spi.h"
#include "themes.h"
#include "vfd.h"
#include "worklist.h"

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
        if (mode == SPI_MODE_CH347) {
            device_path = ch347_auto_detect();
            if (!device_path) {
                fprintf(stderr, "CH347: auto-detect failed, "
                        "specify path with -d\n");
                return 1;
            }
        } else {
            device_path = "/dev/spidev0.0";
        }
    }

    // Select backend
    spi = (mode == SPI_MODE_CH347) ? &ch347_backend : &spidev_backend;

    if (!spi->init(device_path)) {
        return 1;
    }

    // VFD init
    vfd_init();

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
