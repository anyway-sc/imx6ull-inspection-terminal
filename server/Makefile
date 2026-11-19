CC = arm-linux-gnueabihf-gcc
CFLAGS ?= -std=gnu99 -Wall -Wextra -Wpedantic -O0 -g
LDLIBS ?= -lrt
TARGET = tcp_ping_server
SOURCES = tcp_ping_server.c

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SOURCES)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SOURCES) $(LDFLAGS) $(LDLIBS) -o $@

clean:
	rm -f $(TARGET)
