#ifndef AP3216C_H
#define AP3216C_H

struct ap3216c_sample {
    long ir;
    long als;
    long ps;
};

/* 成功返回 0，通过指针写入结果；失败返回 -1，保留调用方原有结果。 */
int read_sensor_value(const char *path, long *value);
int ap3216c_read_sample(struct ap3216c_sample *sample);

#endif
