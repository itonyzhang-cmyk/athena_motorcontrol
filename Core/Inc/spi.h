/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    spi.h
  * @brief   This file contains all the function prototypes for
  *          the spi.c file
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2022 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __SPI_H__
#define __SPI_H__

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "main.h"

#ifdef STM32F446
/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

extern SPI_HandleTypeDef hspi1;

extern SPI_HandleTypeDef hspi3;

/* USER CODE BEGIN Private defines */

/* USER CODE END Private defines */

void MX_SPI1_Init(void);
void MX_SPI3_Init(void);

/* USER CODE BEGIN Prototypes */

/* USER CODE END Prototypes */

#else
#define SPI_SET_NSS_HIGH(x)          gpio_bit_set(x)
#define SPI_SET_NSS_LOW(x)           gpio_bit_reset(x)
#define SPI_TRANSFER_POLL_LIMIT      2048U

typedef enum {
  SPI_TRANSFER_OK = 0,
  SPI_TRANSFER_TBE_TIMEOUT = -1,
  SPI_TRANSFER_RBNE_TIMEOUT = -2,
  SPI_TRANSFER_BUSY_TIMEOUT = -3
} spi_transfer_status_t;

/* At 120 MHz this is comfortably longer than the AS5047P's 350 ns minimum
 * chip-select setup/hold time, without depending on SysTick from an ISR. */
static inline void spi_chip_select_delay(void)
{
  for (volatile uint32_t i = 0U; i < 64U; i++) {
    __asm volatile ("nop");
  }
}

static inline int spi_transmit_receive_timeout(uint32_t spi_periph,
                                               const uint16_t *tx_data,
                                               uint16_t *rx_data,
                                               uint32_t poll_limit)
{
  uint32_t remaining = poll_limit;

  while (RESET == spi_i2s_flag_get(spi_periph, SPI_FLAG_TBE)) {
    if (remaining-- == 0U) {
      return SPI_TRANSFER_TBE_TIMEOUT;
    }
  }

  spi_i2s_data_transmit(spi_periph, *tx_data);

  remaining = poll_limit;
  while (RESET == spi_i2s_flag_get(spi_periph, SPI_FLAG_RBNE)) {
    if (remaining-- == 0U) {
      return SPI_TRANSFER_RBNE_TIMEOUT;
    }
  }

  *rx_data = spi_i2s_data_receive(spi_periph);

  remaining = poll_limit;
  while (SET == spi_i2s_flag_get(spi_periph, SPI_FLAG_TRANS)) {
    if (remaining-- == 0U) {
      return SPI_TRANSFER_BUSY_TIMEOUT;
    }
  }

  return SPI_TRANSFER_OK;
}

static inline int spi_transmit_receive(uint32_t spi_periph,
                                       const uint16_t *tx_data,
                                       uint16_t *rx_data)
{
  return spi_transmit_receive_timeout(spi_periph, tx_data, rx_data,
                                      SPI_TRANSFER_POLL_LIMIT);
}

void MX_SPI1_Init(void);
void MX_SPI2_Init(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* __SPI_H__ */
