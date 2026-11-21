#ifndef VEHICLE_DESCRIPTOR_UTIL_H
#define VEHICLE_DESCRIPTOR_UTIL_H

#include <fcntl.h>
/* Windows 仅运行解析/采集线程模拟；Linux 打开设备时原子设置 CLOEXEC。 */
#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif

#endif
