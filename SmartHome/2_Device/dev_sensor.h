#ifndef __DEV_SENSOR_H
#define __DEV_SENSOR_H

#include <stdint.h>

typedef enum
{
	SENSOR_DHT11 = (1 << 0),
	SENSOR_OTHERS = 0xFFFF
} SensorDevType;

typedef struct
{
	float temperature;
	float humidity;
	uint16_t light;
} SensorData;

typedef struct SensorDev
{
	uint16_t type;
	int (*Init)(struct SensorDev *dev);
	int (*Read)(struct SensorDev *dev, SensorData *out_data);
} SensorDev, *ptSensorDev;

ptSensorDev SensorDev_GetDev(SensorDevType type);

#endif
