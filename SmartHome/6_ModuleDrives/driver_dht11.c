#include "driver_dht11.h"

#include "board_pins.h"
#include "driver_light.h"

#define DHT22_MIN_READ_INTERVAL_MS 2000U
#define DHT22_START_LOW_US         1200U
#define DHT22_START_RELEASE_US     35U
#define DHT22_BIT_ONE_THRESHOLD_US 45U
#define DHT22_TIMEOUT_US           120U

static uint32_t g_last_read_tick;
static uint8_t g_has_cache;
static DHT22Data g_cached_data;

static void dht22_delay_us(uint32_t us)
{
	uint32_t start;
	uint32_t ticks;

	if ((CoreDebug->DEMCR & CoreDebug_DEMCR_TRCENA_Msk) == 0U)
	{
		CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
	}
	if ((DWT->CTRL & DWT_CTRL_CYCCNTENA_Msk) == 0U)
	{
		DWT->CYCCNT = 0U;
		DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
	}

	start = DWT->CYCCNT;
	ticks = (SystemCoreClock / 1000000U) * us;
	while ((DWT->CYCCNT - start) < ticks)
	{
	}
}

static uint8_t dht22_wait_level(GPIO_PinState level, uint32_t timeout_us)
{
	uint32_t start = DWT->CYCCNT;
	uint32_t timeout_ticks = (SystemCoreClock / 1000000U) * timeout_us;

	while (HAL_GPIO_ReadPin(BOARD_DHT11_PORT, BOARD_DHT11_PIN) != level)
	{
		if ((DWT->CYCCNT - start) > timeout_ticks)
		{
			return 0U;
		}
	}
	return 1U;
}

static uint32_t dht22_measure_level_us(GPIO_PinState level, uint32_t timeout_us)
{
	uint32_t start;
	uint32_t elapsed;
	uint32_t timeout_ticks = (SystemCoreClock / 1000000U) * timeout_us;

	start = DWT->CYCCNT;
	while (HAL_GPIO_ReadPin(BOARD_DHT11_PORT, BOARD_DHT11_PIN) == level)
	{
		elapsed = DWT->CYCCNT - start;
		if (elapsed > timeout_ticks)
		{
			return 0U;
		}
	}

	return (DWT->CYCCNT - start) / (SystemCoreClock / 1000000U);
}

static void dht22_set_output(GPIO_PinState level)
{
	GPIO_InitTypeDef GPIO_InitStruct = {0};

	GPIO_InitStruct.Pin = BOARD_DHT11_PIN;
	GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_OD;
	GPIO_InitStruct.Pull = GPIO_PULLUP;
	GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
	HAL_GPIO_Init(BOARD_DHT11_PORT, &GPIO_InitStruct);
	HAL_GPIO_WritePin(BOARD_DHT11_PORT, BOARD_DHT11_PIN, level);
}

static void dht22_set_input(void)
{
	GPIO_InitTypeDef GPIO_InitStruct = {0};

	GPIO_InitStruct.Pin = BOARD_DHT11_PIN;
	GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
	GPIO_InitStruct.Pull = GPIO_PULLUP;
	GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
	HAL_GPIO_Init(BOARD_DHT11_PORT, &GPIO_InitStruct);
}

static int dht22_read_raw(uint8_t data[5])
{
	uint8_t i;
	uint8_t bit;
	uint32_t high_us;

	for (i = 0; i < 5U; i++)
	{
		data[i] = 0U;
	}

	dht22_set_output(GPIO_PIN_RESET);
	dht22_delay_us(DHT22_START_LOW_US);
	HAL_GPIO_WritePin(BOARD_DHT11_PORT, BOARD_DHT11_PIN, GPIO_PIN_SET);
	dht22_delay_us(DHT22_START_RELEASE_US);
	dht22_set_input();

	if (!dht22_wait_level(GPIO_PIN_RESET, DHT22_TIMEOUT_US))
	{
		return -1;
	}
	if (dht22_measure_level_us(GPIO_PIN_RESET, DHT22_TIMEOUT_US) == 0U)
	{
		return -1;
	}
	if (dht22_measure_level_us(GPIO_PIN_SET, DHT22_TIMEOUT_US) == 0U)
	{
		return -1;
	}

	for (i = 0; i < 5U; i++)
	{
		for (bit = 0; bit < 8U; bit++)
		{
			if (dht22_measure_level_us(GPIO_PIN_RESET, DHT22_TIMEOUT_US) == 0U)
			{
				return -1;
			}
			high_us = dht22_measure_level_us(GPIO_PIN_SET, DHT22_TIMEOUT_US);
			if (high_us == 0U)
			{
				return -1;
			}
			data[i] <<= 1;
			if (high_us > DHT22_BIT_ONE_THRESHOLD_US)
			{
				data[i] |= 1U;
			}
		}
	}

	if (((uint8_t)(data[0] + data[1] + data[2] + data[3])) != data[4])
	{
		return -1;
	}

	return 0;
}

int Driver_DHT22_Init(void)
{
	BOARD_ENABLE_GPIOG_CLK();
	dht22_set_input();
	(void)Driver_Light_Init();
	g_last_read_tick = 0U;
	g_has_cache = 0U;
	return 0;
}

int Driver_DHT22_Read(DHT22Data *out_data)
{
	uint8_t data[5];
	uint16_t raw_humidity;
	uint16_t raw_temperature;
	uint32_t now = HAL_GetTick();

	if (out_data == 0)
	{
		return -1;
	}

	if (g_has_cache && ((now - g_last_read_tick) < DHT22_MIN_READ_INTERVAL_MS))
	{
		*out_data = g_cached_data;
		return 0;
	}

	if (dht22_read_raw(data) != 0)
	{
		return -1;
	}

	raw_humidity = ((uint16_t)data[0] << 8) | data[1];
	raw_temperature = ((uint16_t)data[2] << 8) | data[3];

	out_data->humidity = (float)raw_humidity / 10.0f;
	if ((raw_temperature & 0x8000U) != 0U)
	{
		raw_temperature &= 0x7FFFU;
		out_data->temperature = -((float)raw_temperature / 10.0f);
	}
	else
	{
		out_data->temperature = (float)raw_temperature / 10.0f;
	}
	out_data->light = Driver_Light_Read();

	g_cached_data = *out_data;
	g_has_cache = 1U;
	g_last_read_tick = now;

	return 0;
}

int Driver_DHT11_Init(void)
{
	return Driver_DHT22_Init();
}

int Driver_DHT11_Read(DHT11Data *out_data)
{
	return Driver_DHT22_Read(out_data);
}
