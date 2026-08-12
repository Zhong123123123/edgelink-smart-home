#ifndef __DRIVER_DHT11_H
#define __DRIVER_DHT11_H

#include <stdint.h>

typedef struct
{
	float temperature;
	float humidity;
	uint16_t light;
} DHT11Data;

typedef DHT11Data DHT22Data;

int Driver_DHT11_Init(void);
int Driver_DHT11_Read(DHT11Data *out_data);
int Driver_DHT22_Init(void);
int Driver_DHT22_Read(DHT22Data *out_data);

#endif
