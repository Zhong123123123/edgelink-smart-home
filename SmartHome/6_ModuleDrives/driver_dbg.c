#include "driver_dbg.h"
#include <stdio.h>
#include "board_pins.h"

static uint8_t g_dbg_inited = 0U;

int Driver_DBG_Init(void)
{
	if (g_dbg_inited != 0U)
	{
		return 0;
	}
	/* If USART is already enabled by early boot trace, avoid re-init. */
	if ((BOARD_DBG_UART_INSTANCE->CR1 & USART_CR1_UE) != 0U)
	{
		g_dbg_inited = 1U;
		return 0;
	}

	/* Keep DBGOUT init lightweight and deterministic:
	 * only configure TX path on USART1 to avoid re-init side effects.
	 */
	BOARD_ENABLE_DBG_UART_CLK();
	if (BOARD_DBG_TX_PORT == GPIOA || BOARD_DBG_RX_PORT == GPIOA)
	{
		BOARD_ENABLE_GPIOA_CLK();
	}
	if (BOARD_DBG_TX_PORT == GPIOB || BOARD_DBG_RX_PORT == GPIOB)
	{
		BOARD_ENABLE_GPIOB_CLK();
	}
	Board_UART_GPIO_Init_TX(BOARD_DBG_TX_PORT, BOARD_DBG_TX_PIN, BOARD_DBG_UART_AF);

	BOARD_DBG_UART_INSTANCE->CR1 = 0U;
	BOARD_DBG_UART_INSTANCE->CR2 = 0U;
	BOARD_DBG_UART_INSTANCE->CR3 = 0U;
	BOARD_DBG_UART_INSTANCE->BRR =
		(uint16_t)((84000000UL + (BOARD_UART_BAUD_DEFAULT / 2UL)) / BOARD_UART_BAUD_DEFAULT);
	BOARD_DBG_UART_INSTANCE->CR1 = USART_CR1_TE | USART_CR1_UE;

	g_dbg_inited = 1U;
	return 0;
}

struct __FILE
{
	int handle;
};

FILE __stdout;
int fputc(int ch, FILE *f)
{
	(void)f;
	while ((BOARD_DBG_UART_INSTANCE->SR & USART_SR_TXE) == 0U)
	{
	}
	BOARD_DBG_UART_INSTANCE->DR = (uint16_t)(uint8_t)ch;
	return ch;
}




