CC := gcc
CFLAGS := -Wall -Wextra -g -std=c11
SRC := $(wildcard src/*.c)
OBJ := $(SRC:.c=.o)
BIN := mini_hv

.PHONY: all test clean

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

test: tests/test_kvm
	./tests/test_kvm

tests/test_kvm: tests/test_kvm.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(OBJ) $(BIN) tests/test_kvm