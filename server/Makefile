# server/Makefile

TARGET := aesdsocket
SRCS   := aesdsocket.c
OBJS   := $(SRCS:.c=.o)

CC ?= $(CROSS_COMPILE)gcc

CFLAGS ?= -Wall -Wextra -Werror -O2
CFLAGS += -DUSE_AESD_CHAR_DEVICE=1
LDFLAGS ?=
LDFLAGS += -pthread

.PHONY: all default clean

all: $(TARGET)
default: all

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(TARGET) $(OBJS)