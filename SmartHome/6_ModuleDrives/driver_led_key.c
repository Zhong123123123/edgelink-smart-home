#include "driver_led_key.h"
#include "driver_buffer.h"

static RingBuffer KeyBuffer;
volatile static uint32_t KeyTrigerTime = 0;
volatile static uint8_t KeyPending = 0;

static void board_enable_gpio_clock(GPIO_TypeDef *port)
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

int Driver_LED_Init(void)
{
	GPIO_InitTypeDef GPIO_InitStruct = {0};

	board_enable_gpio_clock(LED_PORT);

	GPIO_InitStruct.Pin = LED_PIN;
	GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
	GPIO_InitStruct.Pull = GPIO_PULLUP;
	GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;

	HAL_GPIO_Init(LED_PORT, &GPIO_InitStruct);
	return 0;
}

int Driver_LED_WriteStatus(uint8_t status)
{
	LED(status);
	return 0;
}

int Driver_Beep_Init(void)
{
	GPIO_InitTypeDef GPIO_InitStruct = {0};

	board_enable_gpio_clock(BEEP_PORT);

	GPIO_InitStruct.Pin = BEEP_PIN;
	GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
	GPIO_InitStruct.Pull = GPIO_NOPULL;
	GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;

	HAL_GPIO_Init(BEEP_PORT, &GPIO_InitStruct);
	BEEP(0U);
	return 0;
}

int Driver_Beep_WriteStatus(uint8_t status)
{
	BEEP(status);
	return 0;
}

int Driver_Key_Init(void)
{
	GPIO_InitTypeDef GPIO_InitStruct = {0};

	Driver_Buffer_Init(&KeyBuffer, sizeof(KeyEvent) << 4);
	Driver_Buffer_Clean(&KeyBuffer);
	board_enable_gpio_clock(KEY_PORT);

	GPIO_InitStruct.Pin = KEY_PIN;
	GPIO_InitStruct.Mode = GPIO_MODE_IT_RISING_FALLING;
	GPIO_InitStruct.Pull = GPIO_PULLUP;
	HAL_GPIO_Init(KEY_PORT, &GPIO_InitStruct);

	HAL_NVIC_SetPriority(BOARD_KEY_EXTI_IRQn, 0, 0);
	HAL_NVIC_EnableIRQ(BOARD_KEY_EXTI_IRQn);
	return 0;
}

void BOARD_KEY_IRQ_HANDLER(void)
{
	HAL_GPIO_EXTI_IRQHandler(KEY_PIN);
}

void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
	if (GPIO_Pin != KEY_PIN)
	{
		return;
	}
	KeyTrigerTime = HAL_GetTick() + 50;
	KeyPending = 1U;
}

int Driver_Key_Read(uint8_t *buf, uint16_t len)
{
	if (len == 0 || len < sizeof(KeyEvent) || (len % sizeof(KeyEvent) != 0))
	{
		return -1;
	}
	if (buf == NULL)
	{
		return -1;
	}

	if (Driver_Buffer_ReadBytes(&KeyBuffer, buf, len))
	{
		return 0;
	}
	return -1;
}

void KeyShakeProcess_Callback(void)
{
	KeyEvent nKeyEvent = {0};
	static uint32_t press_time = 0;
	static uint32_t release_time = 0;
	uint32_t tick = HAL_GetTick();

	if (KeyPending && ((int32_t)(tick - KeyTrigerTime) >= 0))
	{
		KeyPending = 0U;
		if (KEY_STATUE() == 0)
		{
			press_time = tick;
		}
		else
		{
			release_time = tick;
		}
		if (press_time != 0 && release_time != 0)
		{
			nKeyEvent.num = 1;
			nKeyEvent.time = release_time - press_time;
			release_time = 0;
			press_time = 0;
			if (Driver_Buffer_WriteBytes(&KeyBuffer, (uint8_t *)&nKeyEvent, sizeof(KeyEvent)) != sizeof(KeyEvent))
			{
				Driver_Buffer_Clean(&KeyBuffer);
				Driver_Buffer_WriteBytes(&KeyBuffer, (uint8_t *)&nKeyEvent, sizeof(KeyEvent));
			}
		}
	}
}
