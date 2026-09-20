/*
 * rp2350.h - RP2350 register map (the subset pTOS uses)
 *
 * Copyright (C) 2026 The pTOS development team
 *
 * This file is distributed under the GPL, version 2 or at your
 * option any later version.  See doc/license.txt for details.
 *
 * Addresses and bit positions are taken from the RP2350 datasheet (and
 * cross-checked against the register headers of the Raspberry Pi pico-sdk,
 * src/rp2350/hardware_regs).  This header is usable from C and assembler.
 */

#ifndef RP2350_H
#define RP2350_H

/* Memory */
#define RP2350_FLASH_BASE       0x10000000
#define RP2350_PSRAM_BASE       0x11000000      /* QMI CS1 window */
#define RP2350_SRAM_BASE        0x20000000
#define RP2350_SRAM_SIZE        (520UL * 1024)

/* APB peripherals */
#define RP2350_CLOCKS_BASE      0x40010000
#define RP2350_RESETS_BASE      0x40020000
#define RP2350_IO_BANK0_BASE    0x40028000
#define RP2350_PADS_BANK0_BASE  0x40038000
#define RP2350_XOSC_BASE        0x40048000
#define RP2350_PLL_SYS_BASE     0x40050000
#define RP2350_PLL_USB_BASE     0x40058000
#define RP2350_UART0_BASE       0x40070000
#define RP2350_SPI1_BASE        0x40088000
#define RP2350_TIMER0_BASE      0x400b0000
#define RP2350_WATCHDOG_BASE    0x400d8000
#define RP2350_TICKS_BASE       0x40108000

/* Single-cycle I/O: GPIO registers for GPIO0-31 and GPIO32-47 ("HI") */
#define RP2350_SIO_BASE         0xd0000000
#define RP2350_SIO_GPIO_IN          (RP2350_SIO_BASE + 0x04)
#define RP2350_SIO_GPIO_HI_IN       (RP2350_SIO_BASE + 0x08)
#define RP2350_SIO_GPIO_OUT_SET     (RP2350_SIO_BASE + 0x18)
#define RP2350_SIO_GPIO_HI_OUT_SET  (RP2350_SIO_BASE + 0x1c)
#define RP2350_SIO_GPIO_OUT_CLR     (RP2350_SIO_BASE + 0x20)
#define RP2350_SIO_GPIO_HI_OUT_CLR  (RP2350_SIO_BASE + 0x24)
#define RP2350_SIO_GPIO_OE_SET      (RP2350_SIO_BASE + 0x38)
#define RP2350_SIO_GPIO_HI_OE_SET   (RP2350_SIO_BASE + 0x3c)
#define RP2350_SIO_GPIO_OE_CLR      (RP2350_SIO_BASE + 0x40)
#define RP2350_SIO_GPIO_HI_OE_CLR   (RP2350_SIO_BASE + 0x44)

/* GPIO function select values */
#define RP2350_GPIO_FUNC_SPI    1
#define RP2350_GPIO_FUNC_UART   2
#define RP2350_GPIO_FUNC_SIO    5

/* Cortex-M33 private peripherals */
#define ARMV8M_SYST_CSR         0xe000e010      /* SysTick control and status */
#define ARMV8M_SYST_RVR         0xe000e014      /* SysTick reload value */
#define ARMV8M_SYST_CVR         0xe000e018      /* SysTick current value */
#define ARMV8M_NVIC_ISER0       0xe000e100
#define ARMV8M_NVIC_ICER0       0xe000e180
#define ARMV8M_NVIC_ICPR0       0xe000e280
#define ARMV8M_NVIC_IPR0        0xe000e400
#define ARMV8M_SCB_CPUID        0xe000ed00
#define ARMV8M_SCB_ICSR         0xe000ed04
#define ARMV8M_SCB_VTOR         0xe000ed08
#define ARMV8M_SCB_AIRCR        0xe000ed0c
#define ARMV8M_SCB_CCR          0xe000ed14
#define ARMV8M_SCB_SHPR2        0xe000ed1c      /* SVCall priority in bits 31:24 */
#define ARMV8M_SCB_SHPR3        0xe000ed20      /* PendSV 23:16, SysTick 31:24 */
#define ARMV8M_SCB_CPACR        0xe000ed88
#define ARMV8M_SCB_SHCSR        0xe000ed24
#define ARMV8M_SCB_CFSR         0xe000ed28
#define ARMV8M_SCB_HFSR         0xe000ed2c
#define ARMV8M_SCB_MMFAR        0xe000ed34
#define ARMV8M_SCB_BFAR         0xe000ed38

/* Atomic register access aliases (RP2350 datasheet 2.1.3) */
#define RP2350_REG_XOR          0x1000
#define RP2350_REG_SET          0x2000
#define RP2350_REG_CLR          0x3000

/* Number of external interrupt lines on the RP2350 NVIC */
#define RP2350_NUM_IRQS         52

/* Interrupt numbers */
#define RP2350_TIMER0_IRQ_0     0
#define RP2350_USBCTRL_IRQ      14
#define RP2350_IO_IRQ_BANK0     21
#define RP2350_UART0_IRQ        33

/* RESETS: reset bits */
#define RP2350_RESET_IO_BANK0   (1UL << 6)
#define RP2350_RESET_PADS_BANK0 (1UL << 9)
#define RP2350_RESET_PLL_SYS    (1UL << 14)
#define RP2350_RESET_PLL_USB    (1UL << 15)
#define RP2350_RESET_SPI1       (1UL << 19)
#define RP2350_RESET_TIMER0     (1UL << 23)
#define RP2350_RESET_UART0      (1UL << 26)
#define RP2350_RESET_USBCTRL    (1UL << 28)

/* Clock frequencies established by rp2350_clocks_init() */
#define RP2350_XOSC_HZ          12000000UL
#define RP2350_CLK_SYS_HZ       150000000UL
#define RP2350_CLK_PERI_HZ      RP2350_CLK_SYS_HZ

#ifndef ASM_SOURCE

#define RP2350_REG(addr)        (*(volatile ULONG *)(addr))

void rp2350_board_init(void);
void rp2350_gpio_set_function(int gpio, int func);
void rp2350_gpio_pull_up(int gpio);

#endif /* ASM_SOURCE */

#endif /* RP2350_H */
