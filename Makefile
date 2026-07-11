CC := gcc
CFLAGS := -Wall -Wextra -g -std=c11 -pthread
NASM := nasm
SRC := $(wildcard src/*.c)
OBJ := $(SRC:.c=.o)
BIN := mini_hv

GUEST_ASM := $(wildcard guest/payloads/*.asm)
GUEST_BIN := $(GUEST_ASM:.asm=.bin)

.PHONY: all guest run test clean

all: $(BIN) guest

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

guest: $(GUEST_BIN)

guest/payloads/%.bin: guest/payloads/%.asm
	$(NASM) -f bin -o $@ $<

run: all
	./$(BIN)

test: tests/test_kvm
	./tests/test_kvm

tests/test_kvm: tests/test_kvm.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(OBJ) $(BIN) tests/test_kvm guest/payloads/*.bin