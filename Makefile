# Phase 1 build for accel_critical (sched_ext)
#
# Prereq: clone https://github.com/sched-ext/scx and point SCX_DIR at it:
#   git clone https://github.com/sched-ext/scx ~/scx
#   make SCX_DIR=~/scx

SCX_DIR   ?= $(HOME)/scx
SCX_INC   := $(SCX_DIR)/scheds/include
BUILD     := build
CLANG     ?= clang
BPFTOOL   ?= bpftool
ARCH      := $(shell uname -m | sed 's/x86_64/x86/; s/aarch64/arm64/; s/riscv64/riscv/')

VMLINUX_H := $(BUILD)/vmlinux.h
BPF_OBJ   := $(BUILD)/accel_critical.bpf.o
SKEL_H    := $(BUILD)/accel_critical.bpf.skel.h
LOADER    := accel_critical

BPF_CFLAGS := -g -O2 -Wall -target bpf -D__TARGET_ARCH_$(ARCH) \
              -I$(BUILD) -I$(SCX_INC) -I$(SCX_INC)/bpf-compat
USER_CFLAGS := -g -O2 -Wall -I$(BUILD) -I$(SCX_INC)
USER_LIBS   := -lbpf -lelf -lz -lpthread

.PHONY: all clean check

all: $(LOADER)

$(BUILD):
	mkdir -p $(BUILD)

$(VMLINUX_H): | $(BUILD)
	$(BPFTOOL) btf dump file /sys/kernel/btf/vmlinux format c > $@

$(BPF_OBJ): src/accel_critical.bpf.c $(VMLINUX_H)
	$(CLANG) $(BPF_CFLAGS) -c $< -o $@

$(SKEL_H): $(BPF_OBJ)
	$(BPFTOOL) gen skeleton $< name accel_critical > $@

$(LOADER): src/accel_critical.c $(SKEL_H)
	$(CC) $(USER_CFLAGS) $< -o $@ $(USER_LIBS)

# Sanity-check the environment before building.
check:
	@echo "kernel:  $$(uname -r)"
	@test -d $(SCX_INC)/scx || { echo "MISSING: $(SCX_INC)/scx (set SCX_DIR)"; exit 1; }
	@test -e /sys/kernel/btf/vmlinux || echo "MISSING: /sys/kernel/btf/vmlinux (need CONFIG_DEBUG_INFO_BTF=y)"
	@test -e /sys/kernel/sched_ext || echo "MISSING: /sys/kernel/sched_ext (need CONFIG_SCHED_CLASS_EXT=y)"
	@which $(CLANG) $(BPFTOOL) >/dev/null || echo "MISSING: clang and/or bpftool"

clean:
	rm -rf $(BUILD) $(LOADER)
