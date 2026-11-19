#ifndef VEHICLE_FAKE_DEVICES_H
#define VEHICLE_FAKE_DEVICES_H

#include "sensor_sampler.h"

void fake_reset(void);
void fake_sensor_block(enum sensor_kind kind, int blocked);
void fake_sensor_fail(enum sensor_kind kind, int fail);
unsigned int fake_sensor_calls(enum sensor_kind kind);
unsigned int fake_sensor_active(void);
int fake_timing_overlap_error(void);
void fake_camera_block(int blocked);
void fake_camera_fail(int fail);
unsigned int fake_camera_active(void);
unsigned int fake_camera_images(void);
unsigned int fake_camera_open_streams(void);
int fake_camera_owner_error(void);

#endif
