CC ?= gcc
CFLAGS ?= -Wall -Wextra -g

TARGET = aesdsocket

all: default
default: $(TARGET)

$(TARGET): aesdsocket.c
	$(CC) $(CFLAGS) -o $(TARGET) aesdsocket.c

clean:
	rm -f $(TARGET)

.PHONY: all default clean
