CC = arm-linux-gnueabihf-gcc
CFLAGS ?= -std=gnu99 -Wall -Wextra -Wpedantic -O0 -g
LDLIBS ?= -lrt
TARGET = tcp_ping_server
SOURCES = tcp_ping_server.c ap3216c.c icm20608.c dht11.c camera_capture.c

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SOURCES) ap3216c.h icm20608.h dht11.h camera_capture.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SOURCES) $(LDFLAGS) $(LDLIBS) -o $@

clean:
	rm -f $(TARGET)
