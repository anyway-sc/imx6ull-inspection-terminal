#ifndef VEHICLE_SR501_H
#define VEHICLE_SR501_H

/* 成功才更新 level；0/1 是 OUT 电平，不等于房间绝对无人/有人。 */
int sr501_read_value(const char *path, unsigned int *level);
int sr501_read_sample(unsigned int *level);

#endif
