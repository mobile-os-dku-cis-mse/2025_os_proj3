# Minimal Makefile for project3 — builds stubs and enforces warnings-as-errors

CC := gcc
CFLAGS := -std=c11 -Iinclude -Wall -Wextra -Werror -O2
LDFLAGS :=

SRC_DIR := src
TOOLS_DIR := tools
TESTS_DIR := tests

BIN_DIR := bin

# Add mainloop binary
BIN := $(BIN_DIR)/kernel_sim $(BIN_DIR)/mk_simplefs $(BIN_DIR)/fs_tests $(BIN_DIR)/mainloop

SRCS_KERNEL := $(SRC_DIR)/demo.c $(SRC_DIR)/fs.c
OBJS_KERNEL := $(SRCS_KERNEL:.c=.o)

all: setup $(BIN_DIR)/mainloop $(BIN)

setup:
	@mkdir -p $(BIN_DIR)


$(BIN_DIR)/mainloop: $(SRC_DIR)/mainloop.c $(SRC_DIR)/fs.c
	$(CC) $(CFLAGS) -o $@ $(SRC_DIR)/mainloop.c $(SRC_DIR)/fs.c $(LDFLAGS)

$(BIN_DIR)/kernel_sim: $(SRCS_KERNEL)
	$(CC) $(CFLAGS) -o $@ $(SRCS_KERNEL) $(LDFLAGS)

$(BIN_DIR)/mk_simplefs: $(TOOLS_DIR)/mk_simplefs.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)



# Target to create the test disk image
TEST_IMG := $(TESTS_DIR)/test.img

$(BIN_DIR)/fs_all_tests: $(TESTS_DIR)/fs_all_tests.c src/fs.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# Create test disk image if it does not exist
$(TEST_IMG): $(BIN_DIR)/mk_simplefs
	echo 'dummy content' > $(TESTS_DIR)/dummy.txt
	$(BIN_DIR)/mk_simplefs $(TEST_IMG) $(TESTS_DIR)/dummy.txt
	chmod 666 $(TEST_IMG)

# Run all tests (builds test binary, creates image, runs test)
.PHONY: test
test: $(BIN_DIR)/fs_all_tests $(TEST_IMG)
	./bin/fs_all_tests

clean:
	-rm -f $(BIN_DIR)/* *.o

.PHONY: all clean setup