#include "driver_light.h"

#include "board_pins.h"

static ADC_HandleTypeDef g_light_adc;
static uint8_t g_light_adc_ready;

int Driver_Light_Init(void)
{
	GPIO_InitTypeDef GPIO_InitStruct = {0};

	BOARD_ENABLE_GPIOF_CLK();
	BOARD_ENABLE_LIGHT_ADC_CLK();

	GPIO_InitStruct.Pin = BOARD_LIGHT_ADC_PIN;
	GPIO_InitStruct.Mode = GPIO_MODE_ANALOG;
	GPIO_InitStruct.Pull = GPIO_NOPULL;
	HAL_GPIO_Init(BOARD_LIGHT_ADC_PORT, &GPIO_InitStruct);

	g_light_adc.Instance = BOARD_LIGHT_ADC_INSTANCE;
	g_light_adc.Init.ClockPrescaler = ADC_CLOCKPRESCALER_PCLK_DIV4;
	g_light_adc.Init.Resolution = ADC_RESOLUTION12b;
	g_light_adc.Init.ScanConvMode = DISABLE;
	g_light_adc.Init.ContinuousConvMode = DISABLE;
	g_light_adc.Init.DiscontinuousConvMode = DISABLE;
	g_light_adc.Init.ExternalTrigConvEdge = ADC_EXTERNALTRIGCONVEDGE_NONE;
	g_light_adc.Init.ExternalTrigConv = ADC_SOFTWARE_START;
	g_light_adc.Init.DataAlign = ADC_DATAALIGN_RIGHT;
	g_light_adc.Init.NbrOfConversion = 1;
	g_light_adc.Init.DMAContinuousRequests = DISABLE;
	g_light_adc.Init.EOCSelection = DISABLE;

	if (HAL_ADC_Init(&g_light_adc) != HAL_OK)
	{
		g_light_adc_ready = 0U;
		return -1;
	}

	g_light_adc_ready = 1U;
	return 0;
}

static uint16_t light_read_raw_once(void)
{
	ADC_ChannelConfTypeDef channel = {0};

	channel.Channel = BOARD_LIGHT_ADC_CHANNEL;
	channel.Rank = 1;
	channel.SamplingTime = ADC_SAMPLETIME_480CYCLES;

	if (HAL_ADC_ConfigChannel(&g_light_adc, &channel) != HAL_OK)
	{
		return 0U;
	}
	if (HAL_ADC_Start(&g_light_adc) != HAL_OK)
	{
		return 0U;
	}
	if (HAL_ADC_PollForConversion(&g_light_adc, 10U) != HAL_OK)
	{
		(void)HAL_ADC_Stop(&g_light_adc);
		return 0U;
	}

	return (uint16_t)HAL_ADC_GetValue(&g_light_adc);
}

uint16_t Driver_Light_Read(void)
{
	uint32_t sum = 0U;
	uint32_t scaled;
	uint8_t i;

	if (!g_light_adc_ready)
	{
		if (Driver_Light_Init() != 0)
		{
			return 0U;
		}
	}

	for (i = 0U; i < 10U; i++)
	{
		sum += light_read_raw_once();
		HAL_Delay(1U);
	}

	scaled = (sum / 10U) / 40U;
	if (scaled > 100U)
	{
		scaled = 100U;
	}

	return (uint16_t)(100U - scaled);
}
