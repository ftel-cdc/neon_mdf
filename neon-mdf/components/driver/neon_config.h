/**
 * @file neon_config.h
 * @brief Build-time module switches for Neon MDF.
 * @details Optional sdkconfig integration; public module headers remain independently includable.
 */
#ifndef __NEON_CONFIG_H__
#define __NEON_CONFIG_H__

#if defined(__has_include)
#if __has_include("sdkconfig.h")
#include "sdkconfig.h"
#define NEON_CONFIG_HAS_SDKCONFIG 1
#endif
#endif
#ifndef NEON_CONFIG_HAS_SDKCONFIG
#define NEON_CONFIG_HAS_SDKCONFIG 0
#endif

#ifndef NEON_GPIO_MODULE_ENABLED
#if !defined(FX1)
#define NEON_GPIO_MODULE_ENABLED 1
#else
#define NEON_GPIO_MODULE_ENABLED 0
#endif
#endif

#ifndef NEON_FLASH_MODULE_ENABLED
#if !defined(FX1)
#define NEON_FLASH_MODULE_ENABLED 1
#else
#define NEON_FLASH_MODULE_ENABLED 0
#endif
#endif

#ifndef NEON_CRM_MODULE_ENABLED
#if defined(FU2)
#define NEON_CRM_MODULE_ENABLED 1
#else
#define NEON_CRM_MODULE_ENABLED 0
#endif
#endif

#ifndef NEON_SYSCSR_MODULE_ENABLED
#if defined(FU2)
#define NEON_SYSCSR_MODULE_ENABLED 1
#else
#define NEON_SYSCSR_MODULE_ENABLED 0
#endif
#endif

#ifndef NEON_UART_MODULE_ENABLED
#if (defined(FU2) || defined(STM32F103CB)) && (!NEON_CONFIG_HAS_SDKCONFIG || defined(CONFIG_NEON_MDF_DRIVER_UART))
#define NEON_UART_MODULE_ENABLED 1
#else
#define NEON_UART_MODULE_ENABLED 0
#endif
#endif

#ifndef NEON_I2C_MODULE_ENABLED
#if defined(FU2) && (!NEON_CONFIG_HAS_SDKCONFIG || defined(CONFIG_NEON_MDF_DRIVER_I2C))
#define NEON_I2C_MODULE_ENABLED 1
#else
#define NEON_I2C_MODULE_ENABLED 0
#endif
#endif

#ifndef NEON_CRC_MODULE_ENABLED
#if defined(FU2) && (!NEON_CONFIG_HAS_SDKCONFIG || defined(CONFIG_NEON_MDF_DRIVER_CRC))
#define NEON_CRC_MODULE_ENABLED 1
#else
#define NEON_CRC_MODULE_ENABLED 0
#endif
#endif

#ifndef NEON_EFUSE_MODULE_ENABLED
#if 0
#define NEON_EFUSE_MODULE_ENABLED 1
#else
#define NEON_EFUSE_MODULE_ENABLED 0
#endif
#endif

#ifndef NEON_ADC_MODULE_ENABLED
#if defined(FU2) && (!NEON_CONFIG_HAS_SDKCONFIG || defined(CONFIG_NEON_MDF_DRIVER_ADC))
#define NEON_ADC_MODULE_ENABLED 1
#else
#define NEON_ADC_MODULE_ENABLED 0
#endif
#endif

#ifndef NEON_DMA_MODULE_ENABLED
#if defined(FU2) && (!NEON_CONFIG_HAS_SDKCONFIG || defined(CONFIG_NEON_MDF_DRIVER_DMA))
#define NEON_DMA_MODULE_ENABLED 1
#else
#define NEON_DMA_MODULE_ENABLED 0
#endif
#endif

#ifndef NEON_SPI_MODULE_ENABLED
#if defined(FU2) && (!NEON_CONFIG_HAS_SDKCONFIG || defined(CONFIG_NEON_MDF_DRIVER_SPI))
#define NEON_SPI_MODULE_ENABLED 1
#else
#define NEON_SPI_MODULE_ENABLED 0
#endif
#endif

#ifndef NEON_PWM_MODULE_ENABLED
#if defined(FU2) && (!NEON_CONFIG_HAS_SDKCONFIG || defined(CONFIG_NEON_MDF_DRIVER_PWM))
#define NEON_PWM_MODULE_ENABLED 1
#else
#define NEON_PWM_MODULE_ENABLED 0
#endif
#endif

#ifndef NEON_TIM_MODULE_ENABLED
#if defined(FU2) && (!NEON_CONFIG_HAS_SDKCONFIG || defined(CONFIG_NEON_MDF_DRIVER_TIM))
#define NEON_TIM_MODULE_ENABLED 1
#else
#define NEON_TIM_MODULE_ENABLED 0
#endif
#endif

#ifndef NEON_RTC_MODULE_ENABLED
#if defined(FU2) && (!NEON_CONFIG_HAS_SDKCONFIG || defined(CONFIG_NEON_MDF_DRIVER_RTC))
#define NEON_RTC_MODULE_ENABLED 1
#else
#define NEON_RTC_MODULE_ENABLED 0
#endif
#endif

#ifndef NEON_TRNG_MODULE_ENABLED
#if defined(FU2) && (!NEON_CONFIG_HAS_SDKCONFIG || defined(CONFIG_NEON_MDF_DRIVER_TRNG))
#define NEON_TRNG_MODULE_ENABLED 1
#else
#define NEON_TRNG_MODULE_ENABLED 0
#endif
#endif

#ifndef NEON_AES_MODULE_ENABLED
#if defined(FU2) && (!NEON_CONFIG_HAS_SDKCONFIG || defined(CONFIG_NEON_MDF_DRIVER_AES))
#define NEON_AES_MODULE_ENABLED 1
#else
#define NEON_AES_MODULE_ENABLED 0
#endif
#endif

#ifndef NEON_QSPI_MODULE_ENABLED
#if defined(FU2) && (!NEON_CONFIG_HAS_SDKCONFIG || defined(CONFIG_NEON_MDF_DRIVER_QSPI))
#define NEON_QSPI_MODULE_ENABLED 1
#else
#define NEON_QSPI_MODULE_ENABLED 0
#endif
#endif

#ifndef NEON_WDT_MODULE_ENABLED
#if defined(FU2) && (!NEON_CONFIG_HAS_SDKCONFIG || defined(CONFIG_NEON_MDF_DRIVER_WDT))
#define NEON_WDT_MODULE_ENABLED 1
#else
#define NEON_WDT_MODULE_ENABLED 0
#endif
#endif

#ifndef NEON_ETH_MODULE_ENABLED
#if defined(FX1) && (!NEON_CONFIG_HAS_SDKCONFIG || defined(CONFIG_NEON_MDF_DRIVER_ETH))
#define NEON_ETH_MODULE_ENABLED 1
#else
#define NEON_ETH_MODULE_ENABLED 0
#endif
#endif

#endif /* __NEON_CONFIG_H__ */
