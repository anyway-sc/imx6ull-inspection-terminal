CC = arm-linux-gnueabihf-gcc
CFLAGS ?= -std=gnu99 -Wall -Wextra -Wpedantic -O0 -g
LDLIBS ?= -lrt
TARGET = tcp_ping_server
SOURCES = tcp_ping_server.c ap3216c.c icm20608.c dht11.c sr04.c sr501.c sensor_sampler.c camera_worker.c ipc_protocol.c process_manager.c
CAMERA_TARGET = $(dir $(TARGET))camera_service
CAMERA_SOURCES = camera_service.c camera_capture.c ipc_protocol.c process_manager.c
HEADERS = $(wildcard *.h)

.PHONY: all clean

all: $(TARGET) $(CAMERA_TARGET)

$(TARGET): $(SOURCES) $(HEADERS)
	$(CC) $(CPPFLAGS) $(CFLAGS) -pthread $(SOURCES) $(LDFLAGS) $(LDLIBS) -o $@

$(CAMERA_TARGET): $(CAMERA_SOURCES) $(HEADERS)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(CAMERA_SOURCES) $(LDFLAGS) $(LDLIBS) -o $@

clean:
	rm -f $(TARGET) $(CAMERA_TARGET)
