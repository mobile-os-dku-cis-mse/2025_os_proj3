CC      := gcc
CFLAGS  := -std=c11 -Wall -Wextra -Wpedantic -Wshadow -Wconversion \
           -O3 -flto -fno-plt -fstrict-aliasing -DNDEBUG \
           -D_POSIX_C_SOURCE=200809L -Iinclude
debug: CFLAGS := -std=c11 -Wall -Wextra -Wpedantic -Wshadow -Wconversion \
                 -O0 -g3 -DFS_DEBUG \
                 -D_POSIX_C_SOURCE=200809L -Iinclude
debug: LDFLAGS :=
debug: simplefs mk_simplefs
LDFLAGS := -flto
SRC     := src/simplefs.c src/util.c
BIN_DIR := .

all: simplefs mk_simplefs # test_simplefs

simplefs: $(SRC) src/main.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

mk_simplefs: $(SRC) src/mk_simplefs.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# test_simplefs: $(SRC) tests/test_simplefs.c
# 	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# Debug / sanitize build (for you, not for professor)
asan: CFLAGS := -std=c11 -Wall -Wextra -Wpedantic -Wshadow -Wconversion \
               -O1 -g3 -fsanitize=address,undefined -fno-omit-frame-pointer \
               -D_POSIX_C_SOURCE=200809L -Iinclude
asan: LDFLAGS := -fsanitize=address,undefined
asan: simplefs mk_simplefs # test_simplefs

clean:
	rm -f simplefs mk_simplefs *.o

.PHONY: all clean asan debug
