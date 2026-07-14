CC := gcc
CFLAGS := -Wall -Wextra -g -std=c11 -pthread
NASM := nasm
SRC := $(wildcard src/*.c)
OBJ := $(SRC:.c=.o)
BIN := mini_hv

GUEST_ASM := $(wildcard guest/payloads/*.asm)
GUEST_BIN := $(GUEST_ASM:.asm=.bin)

INITRAMFS_DIR := initramfs_build
BUSYBOX := $(shell command -v busybox 2>/dev/null)

.PHONY: all guest initramfs run test clean

all: $(BIN) guest

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

guest: $(GUEST_BIN)

guest/payloads/%.bin: guest/payloads/%.asm
	$(NASM) -f bin -o $@ $<

# Rebuilds initramfs_build/ from the tracked init
# Re-run whenever guest/initramfs/init changes.
initramfs: guest/initramfs/init
	@if [ -z "$(BUSYBOX)" ]; then \
		echo "busybox not found on PATH; install busybox-static (apt install busybox-static)"; \
		exit 1; \
	fi
	rm -rf $(INITRAMFS_DIR)
	mkdir -p $(INITRAMFS_DIR)/bin $(INITRAMFS_DIR)/dev $(INITRAMFS_DIR)/proc \
		$(INITRAMFS_DIR)/sys $(INITRAMFS_DIR)/etc $(INITRAMFS_DIR)/sbin
	cp guest/initramfs/init $(INITRAMFS_DIR)/init
	chmod +x $(INITRAMFS_DIR)/init
	cp $(BUSYBOX) $(INITRAMFS_DIR)/bin/busybox
	chmod +x $(INITRAMFS_DIR)/bin/busybox
	cd $(INITRAMFS_DIR) && find . | cpio -o -H newc 2>/dev/null | gzip > ../initramfs.cpio.gz

run: all
	./$(BIN)

test: tests/test_kvm
	./tests/test_kvm

tests/test_kvm: tests/test_kvm.c
	$(CC) $(CFLAGS) -o $@ $<

tests/test_api_client: tests/test_api_client.c
	$(CC) $(CFLAGS) -o $@ $<

# Starts a throwaway mini_hv instance on a test socket, runs the API client
# smoke test against it, and tears the daemon down regardless of outcome.
# Requires bzImage/initramfs.cpio.gz in the repo root (same precondition as
# `make run`).
test-api: $(BIN) tests/test_api_client
	@sock=/tmp/mini_hv_test_$$$$.sock; \
	./$(BIN) $$sock & pid=$$!; \
	trap "kill $$pid 2>/dev/null; rm -f $$sock" EXIT; \
	for i in $$(seq 1 50); do [ -S $$sock ] && break; sleep 0.1; done; \
	./tests/test_api_client $$sock; rc=$$?; \
	exit $$rc

clean:
	rm -f $(OBJ) $(BIN) tests/test_kvm tests/test_api_client guest/payloads/*.bin *.sock
	rm -rf $(INITRAMFS_DIR) initramfs.cpio.gz vm-logs