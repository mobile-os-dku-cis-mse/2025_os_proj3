CC = gcc
CFLAGS = -Wall -g
TARGET = filesys
SRCS = filesys.c
OBJS = $(SRCS:.c=.o)
all: $(TARGET)
$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJS)
filesys.o: filesys.c fs.h
	$(CC) $(CFLAGS) -c filesys.c
clean:
	rm -f $(TARGET) $(OBJS)