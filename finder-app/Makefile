# Assignment 2 - finder-app Makefile

# If CROSS_COMPILE is not set, this becomes "gcc"
CC      ?= $(CROSS_COMPILE)gcc
CFLAGS  ?= -Wall -Wextra -Werror -std=gnu11
LDFLAGS ?=

TARGET  := writer
SRCS    := writer.c
OBJS    := $(SRCS:.c=.o)

.PHONY: all clean

# Default target
all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

# Clean target
clean:
	rm -f $(TARGET) $(OBJS)