#ifndef __STM32F4XX_IT_H
#define __STM32F4XX_IT_H

#include "main.h"

void NMI_Handler(void);
void HardFault_Handler(void);
void MemManage_Handler(void);
void BusFault_Handler(void);
void UsageFault_Handler(void);
void DebugMon_Handler(void);
void TIM2_IRQHandler(void);
void USART2_IRQHandler(void);
void USART3_IRQHandler(void);
void DMA1_Stream1_IRQHandler(void);
void EXTI15_10_IRQHandler(void);

#endif
