#ifndef VEHICLE_DHT11_H
#define VEHICLE_DHT11_H

/* 十分之一单位：600表示60.0%RH，260表示26.0°C。 */
struct dht11_sample {
    unsigned int humidity_tenths;
    unsigned int temperature_tenths;
};

int dht11_read_value(const char *path, struct dht11_sample *sample);
int dht11_read_sample(struct dht11_sample *sample);

#endif
