/*
 *  linux/include/asm-mips/mach-jz4750l/serial.h
 *
 *  Ingenic's JZ4750L common include.
 *
 *  Copyright (C) 2008 Ingenic Semiconductor Inc.
 *
 *  Author: <cwjia@ingenic.cn>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#ifndef __ASM_BOARD_SERIAL_H__
#define __ASM_BOARD_SERIAL_H__

#ifndef CONFIG_SERIAL_MANY_PORTS
#undef RS_TABLE_SIZE
#define RS_TABLE_SIZE  1
#endif

#define JZ_BASE_BAUD	(12000000/16)

#define JZ_SERIAL_PORT_DEFNS \
	{ .baud_base = JZ_BASE_BAUD, .irq = IRQ_UART0, \
	  .flags = STD_COM_FLAGS, .iomem_base = (u8 *)UART0_BASE, \
	  .iomem_reg_shift = 2, .io_type = SERIAL_IO_MEM },

#endif /* __ASM_BORAD_SERIAL_H__ */
