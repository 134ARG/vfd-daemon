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
            "Usage: %s -m <ch347|spidev> -s <port> [-d <device_path>] "
            "[-t <1|3>]\n"
            "  -m  SPI backend mode (required)\n"
            "  -s  listen port for metric agent (required)\n"
            "  -d  device path (default: auto-detect for ch347, "
            "/dev/spidev0.0 for spidev)\n"
            "  -t  display theme (default: 3)\n"
            "      1 = CPU filled bar\n"
            "      3 = CPU+RAM+GPU stacked bars + temps + net\n"
            "  -h  show this help\n",
            prog);
}

int main(int argc, char* argv[]) {
    spi_mode_t mode = -1;
    const char* device_path = NULL;
    int listen_port = 0;
    int theme = 3;
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
                if (theme != 1 && theme != 3) {
                    fprintf(stderr, "Invalid theme: %s (use 1 or 3)\n", optarg);
                    usage(argv[0]);
                    return 1;
                }
                break;
            case 's':
                listen_port = atoi(optarg);
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

    if (listen_port <= 0 || listen_port > 65535) {
        fprintf(stderr, "Error: -s <port> is required\n");
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

    spi = (mode == SPI_MODE_CH347) ? &ch347_backend : &spidev_backend;

    if (mode == SPI_MODE_CH347)
        ch347_setup_signal();

    if (!spi->init(device_path)) {
        return 1;
    }

    vfd_init();
    sleep(1);

    start_remote_reader(listen_port);

    init_worklist();
    workload_func* monitor = (theme == 1) ? &cpu_monitor : &triple_bar_monitor;
    add_to_worklist(monitor, UINT32_MAX, 50, 0);

    main_loop();

    free_worklist();
    spi->close();
    return 0;
}
