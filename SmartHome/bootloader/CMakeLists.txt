cmake_minimum_required(VERSION 3.20)
project(bootloader_standalone C ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(ROOT_DIR ${CMAKE_CURRENT_LIST_DIR}/..)
set(HAL_DIR ${ROOT_DIR}/7_HAL/STM32F4xx_HAL_Driver)
set(CMSIS_DEVICE_DIR ${ROOT_DIR}/7_HAL/CMSIS/Device/ST/STM32F4xx)
set(CMSIS_CORE_DIR ${ROOT_DIR}/7_HAL/CMSIS/Include)
set(CORE_DIR ${ROOT_DIR}/8_Core)
set(BL_DIR ${ROOT_DIR}/bootloader)

set(BL_LINKER_SCRIPT ${BL_DIR}/ld/STM32F407_BOOTLOADER.ld)

add_executable(bootloader.elf
  ${BL_DIR}/bootloader_main.c
  ${BL_DIR}/bootloader_ota.c
  ${BL_DIR}/bootloader_uart.c
  ${BL_DIR}/bootloader_flash.c
  ${BL_DIR}/bootloader_metadata.c
  ${BL_DIR}/bootloader_it.c
  ${CORE_DIR}/system_stm32f4xx.c
  ${HAL_DIR}/Src/stm32f4xx_hal.c
  ${HAL_DIR}/Src/stm32f4xx_hal_cortex.c
  ${HAL_DIR}/Src/stm32f4xx_hal_dma.c
  ${HAL_DIR}/Src/stm32f4xx_hal_flash.c
  ${HAL_DIR}/Src/stm32f4xx_hal_flash_ex.c
  ${HAL_DIR}/Src/stm32f4xx_hal_gpio.c
  ${HAL_DIR}/Src/stm32f4xx_hal_pwr.c
  ${HAL_DIR}/Src/stm32f4xx_hal_rcc.c
  ${HAL_DIR}/Src/stm32f4xx_hal_rcc_ex.c
  ${HAL_DIR}/Src/stm32f4xx_hal_uart.c
  ${CMSIS_DEVICE_DIR}/Source/Templates/gcc/startup_stm32f407xx.s
)

target_compile_definitions(bootloader.elf PRIVATE
  STM32F407xx
  SH_MCU_F407
  USE_HAL_DRIVER
)

target_include_directories(bootloader.elf PRIVATE
  ${BL_DIR}
  ${CORE_DIR}
  ${HAL_DIR}/Inc
  ${CMSIS_CORE_DIR}
  ${CMSIS_DEVICE_DIR}/Include
)

target_compile_options(bootloader.elf PRIVATE
  -mcpu=cortex-m4
  -mthumb
  -mfpu=fpv4-sp-d16
  -mfloat-abi=hard
  -ffunction-sections
  -fdata-sections
  -Wall
  -Wextra
  -Wno-unused-parameter
)

target_link_options(bootloader.elf PRIVATE
  -mcpu=cortex-m4
  -mthumb
  -mfpu=fpv4-sp-d16
  -mfloat-abi=hard
  -specs=nano.specs
  -specs=nosys.specs
  -Wl,--gc-sections
  -Wl,-Map=${CMAKE_BINARY_DIR}/bootloader.map
  -T${BL_LINKER_SCRIPT}
)

find_program(CMAKE_OBJCOPY arm-none-eabi-objcopy)
if(CMAKE_OBJCOPY)
  add_custom_command(TARGET bootloader.elf POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O ihex $<TARGET_FILE:bootloader.elf> ${CMAKE_BINARY_DIR}/bootloader.hex
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:bootloader.elf> ${CMAKE_BINARY_DIR}/bootloader.bin
    COMMENT "Generating bootloader.hex and bootloader.bin"
  )
endif()
