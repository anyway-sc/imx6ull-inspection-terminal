#ifndef VEHICLE_SR04_H
#define VEHICLE_SR04_H

struct sr04_sample {
    unsigned int distance_mm;
    unsigned int pulse_us;
};

int sr04_read_value(const char *path, struct sr04_sample *sample);
int sr04_read_sample(struct sr04_sample *sample);

#endif
