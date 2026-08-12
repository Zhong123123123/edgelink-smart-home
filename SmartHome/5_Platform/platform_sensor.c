#include "platform_sensor.h"

#include "driver_dht11.h"

int platform_sensor_init(struct SensorDev *dev)
{
	if (dev == 0)
	{
		return -1;
	}

	return Driver_DHT11_Init();
}

int platform_sensor_read(struct SensorDev *dev, SensorData *out_data)
{
	DHT11Data data;

	if (dev == 0 || out_data == 0)
	{
		return -1;
	}

	if (Driver_DHT11_Read(&data) != 0)
	{
		return -1;
	}

	out_data->temperature = data.temperature;
	out_data->humidity = data.humidity;
	out_data->light = data.light;

	return 0;
}
