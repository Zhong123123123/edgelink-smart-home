#ifndef __BOARD_PROFILE_H
#define __BOARD_PROFILE_H

/*
 * MCU profile selection:
 * - Default: STM32F103 (keeps current project behavior unchanged)
 * - For migration: define SH_MCU_F407 in project preprocessor macros.
 */
#if !defined(SH_MCU_F103) && !defined(SH_MCU_F407)
#define SH_MCU_F103 1
#endif

#if defined(SH_MCU_F407) && defined(SH_MCU_F103)
#error "Only one MCU profile can be enabled: SH_MCU_F103 or SH_MCU_F407"
#endif

#if defined(SH_MCU_F407)
#include "stm32f4xx_hal.h"
#else
#include "stm32f1xx_hal.h"
#endif

#endif
