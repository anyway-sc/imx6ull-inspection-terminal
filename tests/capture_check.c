#include <stdio.h>

#include "../drivers/sr04/sr04_capture.h"

static unsigned int checks;
static unsigned int failures;

#define CHECK(condition, label) do { \
    ++checks; \
    if (!(condition)) { \
        fprintf(stderr, "FAIL: %s (line %d)\n", label, __LINE__); \
        ++failures; \
    } \
} while (0)

static void check_rejected_width(unsigned long long width, const char *label)
{
    struct vehicle_sr04_sample sample = {123, 456};

    CHECK(vehicle_sr04_convert(width, &sample) != 0, label);
    CHECK(sample.distance_mm == 123 && sample.pulse_us == 456,
           "failed conversion preserves previous output");
}

int main(void)
{
    struct vehicle_sr04_capture capture = {0};
    struct vehicle_sr04_sample sample = {0, 0};
    const unsigned long long start = 1000000000ULL;

    CHECK(!vehicle_sr04_edge(&capture, 1, start), "idle ignores rising edge");
    CHECK(capture.phase == VEHICLE_SR04_IDLE, "idle stays idle");

    vehicle_sr04_begin(&capture, start);
    CHECK(!vehicle_sr04_edge(&capture, 0, start + 100000),
           "falling edge before rising cannot finish");
    CHECK(capture.phase == VEHICLE_SR04_WAIT_RISE,
           "still waiting for a rising edge");
    CHECK(!vehicle_sr04_edge(&capture, 1, start + 200000),
           "rising edge starts timing without completing");
    CHECK(!vehicle_sr04_edge(&capture, 1, start + 300000),
           "duplicate rising edge is ignored");
    CHECK(vehicle_sr04_edge(&capture, 0, start + 2200000),
           "falling edge completes the pulse");
    CHECK(capture.phase == VEHICLE_SR04_DONE && capture.pulse_ns == 2000000,
           "2 ms pulse uses the first rising edge");
    CHECK(!vehicle_sr04_convert(capture.pulse_ns, &sample) &&
           sample.distance_mm == 343 && sample.pulse_us == 2000,
           "2 ms round trip gives 343 mm, not 686 mm or 34 mm");
    CHECK(!vehicle_sr04_edge(&capture, 0, start + 2300000),
           "completed measurement ignores further edges");

    vehicle_sr04_begin(&capture, start);
    CHECK(!vehicle_sr04_edge(&capture, 1, start - 1),
           "pending edge timestamp from before rearm is ignored");
    CHECK(capture.phase == VEHICLE_SR04_WAIT_RISE && !capture.pulse_ns,
           "rearm clears the previous completed result");
    vehicle_sr04_edge(&capture, 1, start + 500000);
    CHECK(!vehicle_sr04_edge(&capture, 0, start + 400000),
           "out of order timestamp cannot underflow width");
    CHECK(capture.phase == VEHICLE_SR04_WAIT_FALL,
           "waiting after out of order timestamp");
    CHECK(vehicle_sr04_edge(&capture, 0, start + 1500000),
           "a subsequent valid falling edge completes");
    CHECK(!vehicle_sr04_convert(capture.pulse_ns, &sample) &&
           sample.distance_mm == 172 && sample.pulse_us == 1000,
           "fractional millimetres round consistently");

    vehicle_sr04_begin(&capture, start);
    CHECK(vehicle_sr04_edge(&capture, 1, start + VEHICLE_SR04_TIMEOUT_NS),
           "first rising edge at deadline reports timeout");
    CHECK(capture.phase == VEHICLE_SR04_TIMED_OUT,
           "deadline does not become a new measurement");
    CHECK(!vehicle_sr04_edge(&capture, 0, start + VEHICLE_SR04_TIMEOUT_NS + 1),
           "late falling edge cannot repair a timed out sample");

    vehicle_sr04_begin(&capture, start);
    vehicle_sr04_edge(&capture, 1, start + 200000);
    CHECK(vehicle_sr04_edge(&capture, 0, start + VEHICLE_SR04_TIMEOUT_NS),
           "falling edge at deadline reports timeout");
    CHECK(capture.phase == VEHICLE_SR04_TIMED_OUT && !capture.pulse_ns,
           "expired pulse is not published");

    vehicle_sr04_begin(&capture, start);
    vehicle_sr04_edge(&capture, 1, start + 200000);
    vehicle_sr04_cancel(&capture);
    CHECK(!vehicle_sr04_edge(&capture, 0, start + 2200000) &&
           capture.phase == VEHICLE_SR04_IDLE,
           "signal cancellation ignores a later falling edge");
    vehicle_sr04_begin(&capture, start + 100000000);
    CHECK(!vehicle_sr04_edge(&capture, 0, start + 100100000) &&
           capture.phase == VEHICLE_SR04_WAIT_RISE,
           "next request requires its own rising edge");

    CHECK(!vehicle_sr04_convert(1166181, &sample) && sample.distance_mm == 200 &&
           sample.pulse_us == 1166, "20 cm example with nanosecond input");
    CHECK(!vehicle_sr04_convert(116618, &sample) && sample.distance_mm == 20,
           "nominal 2 cm lower limit");
    CHECK(!vehicle_sr04_convert(23323615, &sample) && sample.distance_mm == 4000,
           "nominal 4 m upper limit");
    check_rejected_width(0, "zero is not a valid distance");
    check_rejected_width(10000, "short noise spike is rejected");
    check_rejected_width(110787, "nominal 19 mm is out of range");
    check_rejected_width(23329446, "nominal 4001 mm is out of range");
    check_rejected_width(30000000, "30 ms echo exceeds nominal 4 m range");
    check_rejected_width(VEHICLE_SR04_TIMEOUT_NS + 1,
                          "width longer than capture deadline is rejected");
    check_rejected_width(~0ULL, "huge input is rejected before multiplication");

    if (failures)
        return 1;
    printf("PASS: %u assertions; edge ordering, cancellation, deadlines and units.\n",
           checks);
    puts("No GPIO, board, IRQ or kernel module was executed.");
    return 0;
}
