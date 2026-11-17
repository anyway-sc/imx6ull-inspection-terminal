#include <stdio.h>

#include "../drivers/dht11/dht11_frame.h"

static int check_case(const unsigned char raw[5], int expected_result,
                      unsigned int humidity, unsigned int temperature,
                      const char *name)
{
    struct vehicle_dht11_sample sample = {777, 888};
    int result = vehicle_dht11_decode_frame(raw, &sample);

    if (result != expected_result ||
        (result == 0 && (sample.humidity_tenths != humidity ||
                         sample.temperature_tenths != temperature)) ||
        (result != 0 && (sample.humidity_tenths != 777 ||
                         sample.temperature_tenths != 888))) {
        fprintf(stderr, "FAIL: %s, result=%d, humidity=%u, temperature=%u\n",
                name, result, sample.humidity_tenths, sample.temperature_tenths);
        return 1;
    }
    return 0;
}

int main(void)
{
    const unsigned char room[] = {55, 0, 24, 0, 79};
    const unsigned char fractional[] = {63, 5, 26, 3, 97};
    const unsigned char bad_checksum[] = {55, 0, 24, 0, 78};
    const unsigned char invalid_fraction[] = {55, 10, 24, 0, 89};
    const unsigned char invalid_temp[] = {55, 0, 51, 0, 106};
    const unsigned char invalid_humidity[] = {101, 0, 24, 0, 125};
    const unsigned char upper_bound[] = {100, 0, 50, 0, 150};
    const unsigned char overflow_sum[] = {255, 0, 1, 0, 0};
    int failed = 0;

    failed += check_case(room, 0, 550, 240, "humidity first, temperature second");
    failed += check_case(fractional, 0, 635, 263, "fractional readings");
    failed += check_case(bad_checksum, VEHICLE_DHT11_FRAME_CHECKSUM, 0, 0,
                         "checksum rejects corrupt frame");
    failed += check_case(invalid_fraction, VEHICLE_DHT11_FRAME_FORMAT, 0, 0,
                         "reject invalid decimal byte");
    failed += check_case(invalid_temp, VEHICLE_DHT11_FRAME_FORMAT, 0, 0,
                         "reject temperature outside DHT11 range");
    failed += check_case(invalid_humidity, VEHICLE_DHT11_FRAME_FORMAT, 0, 0,
                         "reject humidity above 100 percent");
    failed += check_case(upper_bound, 0, 1000, 500, "upper bounds");
    failed += check_case(overflow_sum, VEHICLE_DHT11_FRAME_FORMAT, 0, 0,
                         "8-bit checksum then physical validation");
    if (failed)
        return 1;
    puts("PASS: 8 frame cases; no GPIO or hardware was accessed.");
    return 0;
}
