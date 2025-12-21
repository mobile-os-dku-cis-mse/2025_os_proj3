CC=gcc
CFLAGS=-std=c11 -O2 -Wall -Wextra -pedantic

BIN=mk_simplefs simplefs_demo

all: $(BIN)

mk_simplefs: src/mk_simplefs.c src/simplefs.h
	$(CC) $(CFLAGS) -o $@ src/mk_simplefs.c

simplefs_demo: src/simplefs_demo.c src/simplefs.h
	$(CC) $(CFLAGS) -o $@ src/simplefs_demo.c

clean:
	rm -f $(BIN) disk.img

.PHONY: all clean
