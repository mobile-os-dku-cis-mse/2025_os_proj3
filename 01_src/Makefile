CC = gcc
CFLAGS = -Wall -Wextra -std=c99 -g
TARGET = main
SRC = main.c

all: $(TARGET)

$(TARGET): $(SRC) fs.h
	$(CC) $(CFLAGS) $(SRC) -o $(TARGET)

clean:
	rm -f $(TARGET) *.img