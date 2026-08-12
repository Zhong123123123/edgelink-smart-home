#include "dev_sensor.h"

#include "platform_sensor.h"

static int SensorDev_Init(struct SensorDev *dev)
{
	return platform_sensor_init(dev);
}

static int SensorDev_Read(struct SensorDev *dev, SensorData *out_data)
{
	return platform_sensor_read(dev, out_data);
}

static const SensorDev g_sensor_dev = {SENSOR_DHT11, SensorDev_Init, SensorDev_Read};

ptSensorDev SensorDev_GetDev(SensorDevType type)
{
	if (type == SENSOR_DHT11)
	{
		return (ptSensorDev)&g_sensor_dev;
	}

	return 0;
}
