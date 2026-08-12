#ifndef __PLATFORM_SENSOR_H
#define __PLATFORM_SENSOR_H

#include "dev_sensor.h"

int platform_sensor_init(struct SensorDev *dev);
int platform_sensor_read(struct SensorDev *dev, SensorData *out_data);

#endif
