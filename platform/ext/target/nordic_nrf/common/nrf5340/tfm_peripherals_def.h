/*
 * Copyright (c) 2018-2019, Arm Limited. All rights reserved.
 * Copyright (c) 2020, Cypress Semiconductor Corporation. All rights reserved.
 * Copyright (c) 2020, Nordic Semiconductor ASA. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#ifndef __TFM_PERIPHERALS_DEF_H__
#define __TFM_PERIPHERALS_DEF_H__

#include <nrf.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TFM_TIMER0_IRQ    (TIMER0_IRQn)
#define TFM_TIMER1_IRQ    (TIMER1_IRQn)

struct platform_data_t;

extern struct platform_data_t tfm_peripheral_std_uart;
extern struct platform_data_t tfm_peripheral_timer0;

#define TFM_PERIPHERAL_STD_UART     (&tfm_peripheral_std_uart)
#define TFM_PERIPHERAL_TIMER0       (&tfm_peripheral_timer0)
#define TFM_PERIPHERAL_FPGA_IO      (0)

#ifdef PSA_API_TEST_IPC
#define FF_TEST_UART_IRQ         (EGU5_IRQn)
#define FF_TEST_UART_IRQ_Handler (pal_interrupt_handler)

extern struct platform_data_t tfm_peripheral_FF_TEST_NVMEM_REGION;
extern struct platform_data_t tfm_peripheral_FF_TEST_SERVER_PARTITION_MMIO;
extern struct platform_data_t tfm_peripheral_FF_TEST_DRIVER_PARTITION_MMIO;

#define FF_TEST_UART_REGION           (&tfm_peripheral_std_uart)
#define FF_TEST_WATCHDOG_REGION       (&tfm_peripheral_timer0)
#define FF_TEST_NVMEM_REGION          (&tfm_peripheral_FF_TEST_NVMEM_REGION)
#define FF_TEST_SERVER_PARTITION_MMIO (&tfm_peripheral_FF_TEST_SERVER_PARTITION_MMIO)
#define FF_TEST_DRIVER_PARTITION_MMIO (&tfm_peripheral_FF_TEST_DRIVER_PARTITION_MMIO)
#endif /* PSA_API_TEST_IPC */

#ifdef __cplusplus
}
#endif

#endif /* __TFM_PERIPHERALS_DEF_H__ */
