/**
 * @file neon.h
 * @brief Optional entry header for enabled Neon MDF driver modules.
 */
#ifndef __NEON_H__
#define __NEON_H__
#include "neon_types.h"
#include "neon_config.h"

#if NEON_GPIO_MODULE_ENABLED == 1
#include "gpio/neon_gpio.h"
#endif

#if NEON_FLASH_MODULE_ENABLED == 1
#include "flash/neon_flash.h"
#endif

#if NEON_CRM_MODULE_ENABLED == 1
#include "crm/neon_crm.h"
#endif

#if NEON_SYSCSR_MODULE_ENABLED == 1
#include "syscsr/neon_syscsr.h"
#endif

#if NEON_UART_MODULE_ENABLED == 1
#include "uart/neon_uart.h"
#endif

#if NEON_I2C_MODULE_ENABLED == 1
#include "i2c/neon_i2c.h"
#endif

#if NEON_CRC_MODULE_ENABLED == 1
#include "crc/neon_crc.h"
#endif

#if NEON_EFUSE_MODULE_ENABLED == 1
#include "efuse/neon_efuse.h"
#endif

#if NEON_ADC_MODULE_ENABLED == 1
#include "adc/neon_adc.h"
#endif

#if NEON_DMA_MODULE_ENABLED == 1
#include "dma/neon_dma.h"
#endif

#if NEON_SPI_MODULE_ENABLED == 1
#include "spi/neon_spi.h"
#endif

#if NEON_PWM_MODULE_ENABLED == 1
#include "pwm/neon_pwm.h"
#endif

#if NEON_TIM_MODULE_ENABLED == 1
#include "tim/neon_tim.h"
#endif

#if NEON_RTC_MODULE_ENABLED == 1
#include "rtc/neon_rtc.h"
#endif

#if NEON_TRNG_MODULE_ENABLED == 1
#include "trng/neon_trng.h"
#endif

#if NEON_AES_MODULE_ENABLED == 1
#include "aes/neon_aes.h"
#endif

#if NEON_QSPI_MODULE_ENABLED == 1
#include "qspi/neon_qspi.h"
#endif

#if NEON_WDT_MODULE_ENABLED == 1
#include "wdt/neon_wdt.h"
#endif

#if NEON_ETH_MODULE_ENABLED == 1
#include "eth/neon_eth.h"
#endif

#endif /* __NEON_H__ */
