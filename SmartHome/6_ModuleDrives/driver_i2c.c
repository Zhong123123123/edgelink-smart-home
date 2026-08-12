#include "driver_i2c.h"

static I2C_HandleTypeDef g_hi2c1;

static void Driver_I2C_EnableGpioClock(GPIO_TypeDef *port)
{
	if (port == GPIOA)
	{
		BOARD_ENABLE_GPIOA_CLK();
	}
	else if (port == GPIOB)
	{
		BOARD_ENABLE_GPIOB_CLK();
	}
	else if (port == GPIOC)
	{
		BOARD_ENABLE_GPIOC_CLK();
	}
	else if (port == GPIOD)
	{
		BOARD_ENABLE_GPIOD_CLK();
	}
	else if (port == GPIOE)
	{
		BOARD_ENABLE_GPIOE_CLK();
	}
	else if (port == GPIOF)
	{
		BOARD_ENABLE_GPIOF_CLK();
	}
	else if (port == GPIOG)
	{
		BOARD_ENABLE_GPIOG_CLK();
	}
}

I2C_HandleTypeDef *Driver_I2C_GetHandle(void)
{
	return &g_hi2c1;
}

int Driver_I2C_Init(void)
{
	g_hi2c1.Instance = BOARD_OLED_I2C_INSTANCE;
	g_hi2c1.Init.ClockSpeed = 400000U;
	g_hi2c1.Init.DutyCycle = I2C_DUTYCYCLE_2;
	g_hi2c1.Init.OwnAddress1 = 0U;
	g_hi2c1.Init.AddressingMode = I2C_ADDRESSINGMODE_7BIT;
	g_hi2c1.Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
	g_hi2c1.Init.OwnAddress2 = 0U;
	g_hi2c1.Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
	g_hi2c1.Init.NoStretchMode = I2C_NOSTRETCH_DISABLE;

	if (HAL_I2C_Init(&g_hi2c1) != HAL_OK)
	{
		return -1;
	}

	return 0;
}

void HAL_I2C_MspInit(I2C_HandleTypeDef *hi2c)
{
	GPIO_InitTypeDef GPIO_InitStruct = {0};

	if (hi2c->Instance != BOARD_OLED_I2C_INSTANCE)
	{
		return;
	}

	Driver_I2C_EnableGpioClock(BOARD_OLED_I2C_SCL_PORT);
	Driver_I2C_EnableGpioClock(BOARD_OLED_I2C_SDA_PORT);
	BOARD_ENABLE_OLED_I2C_CLK();

	GPIO_InitStruct.Mode = GPIO_MODE_AF_OD;
	GPIO_InitStruct.Pull = GPIO_PULLUP;
	GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
	GPIO_InitStruct.Alternate = BOARD_OLED_I2C_AF;

	if (BOARD_OLED_I2C_SCL_PORT == BOARD_OLED_I2C_SDA_PORT)
	{
		GPIO_InitStruct.Pin = BOARD_OLED_I2C_SCL_PIN | BOARD_OLED_I2C_SDA_PIN;
		HAL_GPIO_Init(BOARD_OLED_I2C_SCL_PORT, &GPIO_InitStruct);
	}
	else
	{
		GPIO_InitStruct.Pin = BOARD_OLED_I2C_SCL_PIN;
		HAL_GPIO_Init(BOARD_OLED_I2C_SCL_PORT, &GPIO_InitStruct);
		GPIO_InitStruct.Pin = BOARD_OLED_I2C_SDA_PIN;
		HAL_GPIO_Init(BOARD_OLED_I2C_SDA_PORT, &GPIO_InitStruct);
	}
}

int Driver_I2C_Write(uint16_t dev_addr, const uint8_t *data, uint16_t len, uint32_t timeout)
{
	if (data == 0 || len == 0U)
	{
		return -1;
	}

	if (HAL_I2C_Master_Transmit(&g_hi2c1, dev_addr, (uint8_t *)data, len, timeout) != HAL_OK)
	{
		return -1;
	}

	return 0;
}
