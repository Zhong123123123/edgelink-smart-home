#include "stm32f4xx_hal.h"
#include "bootloader_uart.h"

void SysTick_Handler(void)
{
    HAL_IncTick();
}

void USART3_IRQHandler(void)
{
    bl_uart_irq_handler();
}

void DMA1_Stream1_IRQHandler(void)
{
    bl_uart_dma_irq_handler();
}
