#include "sr501.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char fixture[] = "sr501-read-fixture.txt";
static unsigned int checks;
#define CHECK(condition) do { ++checks; if (!(condition)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); return EXIT_FAILURE; \
} } while (0)

static int write_fixture(const char *text, size_t length)
{
    FILE *file = fopen(fixture, "wb");
    if (!file)
        return -1;
    int result = fwrite(text, 1, length, file) == length ? 0 : -1;
    if (fclose(file) != 0)
        result = -1;
    return result;
}

int main(void)
{
#ifdef _WIN32
    _fmode = _O_BINARY;
#endif
    const char *valid[] = {"0", "0\n", "1", "1\n"};
    unsigned int level = 99;
    for (size_t i = 0; i < sizeof(valid) / sizeof(valid[0]); ++i) {
        CHECK(write_fixture(valid[i], strlen(valid[i])) == 0);
        CHECK(sr501_read_value(fixture, &level) == 0);
        CHECK(level == (unsigned int)(valid[i][0] - '0'));
    }
    const char *invalid[] = {"", "2", "-1", "+1", "00", " 1", "1 ", "1\r\n", "1\n0", "1\n\n", "100000000000000000"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        level = 99;
        CHECK(write_fixture(invalid[i], strlen(invalid[i])) == 0);
        CHECK(sr501_read_value(fixture, &level) == -1);
        CHECK(level == 99);
    }
    CHECK(write_fixture("1\0x", 3) == 0);
    CHECK(sr501_read_value(fixture, &level) == -1 && errno == EINVAL);
    CHECK(sr501_read_value(NULL, &level) == -1 && errno == EINVAL);
    CHECK(sr501_read_value(fixture, NULL) == -1 && errno == EINVAL);
    CHECK(sr501_read_sample(NULL) == -1 && errno == EINVAL);
    CHECK(remove(fixture) == 0);
    CHECK(sr501_read_value(fixture, &level) == -1 && errno == ENOENT);
    CHECK(level == 99);
    printf("PASS: %u assertions; SR501 text, bounds and file errors.\n", checks);
    return EXIT_SUCCESS;
}
