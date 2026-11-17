#include "sr04.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char fixture[] = "sr04-read-fixture.txt";
static unsigned int checks;

#define CHECK(condition) do { \
    ++checks; \
    if (!(condition)) { \
        fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); \
        return EXIT_FAILURE; \
    } \
} while (0)

static int write_fixture(const char *bytes, size_t length)
{
    FILE *file = fopen(fixture, "wb");
    if (file == NULL)
        return -1;
    int result = fwrite(bytes, 1, length, file) == length ? 0 : -1;
    if (fclose(file) != 0)
        result = -1;
    return result;
}

int main(void)
{
#ifdef _WIN32
    _fmode = _O_BINARY;
#endif
    const struct {
        const char *text;
        unsigned int distance;
        unsigned int pulse;
    } valid[] = {
        {"91,529\n", 91, 529}, {"343,2000", 343, 2000},
        {"20,117\n", 20, 117}, {"4000,23324\n", 4000, 23324},
        {"200,1166\n", 200, 1166}, {"00091,00529\n", 91, 529},
        {"20,114\n", 20, 114}, {"4000,23327\n", 4000, 23327}
    };
    struct sr04_sample sample = {123, 456};
    for (size_t i = 0; i < sizeof(valid) / sizeof(valid[0]); ++i) {
        CHECK(write_fixture(valid[i].text, strlen(valid[i].text)) == 0);
        CHECK(sr04_read_value(fixture, &sample) == 0);
        CHECK(sample.distance_mm == valid[i].distance && sample.pulse_us == valid[i].pulse);
    }
    const char *invalid[] = {
        "", "91", "91,", ",529", "91,529,1", "91 529\n", "91;529\n",
        "91.0,529\n", "+91,529\n", "-91,529\n", "91,-529\n", "91,+529\n",
        "0,0\n", "19,111\n", "4001,23330\n", "91,0\n", "91,40001\n",
        "91,2000\n", "20,100\n", "4000,24000\n", "91,529 \n", " 91,529\n",
        "91,529\r\n", "91,529\n\n", "9999999999999999999999999999999999999",
        "91,999999999999999999999999999999999999999"
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        sample.distance_mm = 123;
        sample.pulse_us = 456;
        CHECK(write_fixture(invalid[i], strlen(invalid[i])) == 0);
        CHECK(sr04_read_value(fixture, &sample) == -1);
        CHECK(sample.distance_mm == 123 && sample.pulse_us == 456);
    }
    const char embedded_nul[] = "91,529\0junk\n";
    CHECK(write_fixture(embedded_nul, sizeof(embedded_nul) - 1) == 0);
    CHECK(sr04_read_value(fixture, &sample) == -1 && errno == EINVAL);
    CHECK(sr04_read_value(NULL, &sample) == -1 && errno == EINVAL);
    CHECK(sr04_read_value(fixture, NULL) == -1 && errno == EINVAL);
    CHECK(remove(fixture) == 0);
    CHECK(sr04_read_value(fixture, &sample) == -1 && errno == ENOENT);
    CHECK(sample.distance_mm == 123 && sample.pulse_us == 456);
    printf("PASS: %u assertions; SR04 fixture parsing and I/O errors, no hardware.\n", checks);
    return EXIT_SUCCESS;
}
