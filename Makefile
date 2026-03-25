SYSROOT := $(HOME)/test-field/cross-compile/sysroot
CFLAGS  := -O3 -Wall

SRCS := $(wildcard src/*.c)

# x86_64 (native)
X64_CC     := gcc
X64_INC    := -I./lib/x64/static
X64_LIB    := -L./lib/x64/static -l:libch347.a -lpthread

# aarch64 (cross)
ARM64_CC   := aarch64-linux-gnu-gcc
ARM64_INC  := -I./lib/aarch64/static
ARM64_LIB  := -L./lib/aarch64/static -l:libch347.a -lpthread -B$(SYSROOT)/usr/lib/aarch64-linux-gnu
ARM64_SYSROOT := --sysroot=$(SYSROOT) -isystem $(SYSROOT)/usr/include/aarch64-linux-gnu

.PHONY: all x86 arm64 cpu clean

all: x86 arm64

x86: vfd-daemon-x86

arm64: vfd-daemon-arm64

cpu:
	$(X64_CC) $(CFLAGS) ./cpu_util.c -o cpu-util

vfd-daemon-x86: $(SRCS)
	$(X64_CC) $(CFLAGS) $(SRCS) -o $@ $(X64_INC) $(X64_LIB)

vfd-daemon-arm64: $(SRCS)
	$(ARM64_CC) $(CFLAGS) $(ARM64_SYSROOT) $(SRCS) -o $@ $(ARM64_INC) $(ARM64_LIB)

clean:
	rm -f vfd-daemon-x86 vfd-daemon-arm64 cpu-util
