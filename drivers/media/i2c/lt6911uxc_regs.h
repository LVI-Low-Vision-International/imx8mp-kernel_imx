/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * lt6911uxc_regs.h - Lontium LT6911UXC HDMI-to-MIPI-CSI2 bridge register definitions
 *
 * Adapted from lt6911uxc_regs_zhaw.h:
 * Copyright (c) 2020, Alexey Gromov <groo@zhaw.ch>
 */

#ifndef __LT6911UXC_REGS_H__
#define __LT6911UXC_REGS_H__

/* Register bank select - always written to 0xFF */
#define LT6911UXC_SW_BANK		0xFF

/* I2C control */
#define LT6911UXC_ENABLE_I2C		0x80EE
#define LT6911UXC_DISABLE_WD		0x8010

/* ---- Resolution / timing registers ---- */
/* All H_* registers hold half the actual pixel count; multiply by 2 for 4-lane */
#define LT6911UXC_H_TOTAL_0P5		0x867C	/* horizontal half total pixels */
#define LT6911UXC_H_ACTIVE_0P5		0x8680	/* horizontal half active pixels */
#define LT6911UXC_H_FP_0P5		0x8678	/* horizontal half front porch */
#define LT6911UXC_H_BP_0P5		0x8676	/* horizontal half back porch */
#define LT6911UXC_H_SW_0P5		0x8672	/* hsync half length */
#define LT6911UXC_V_TOTAL		0x867A	/* vertical total lines */
#define LT6911UXC_V_ACTIVE		0x867E	/* vertical active lines */
#define LT6911UXC_V_BP			0x8674	/* vertical back porch lines */
#define LT6911UXC_V_FP			0x8675	/* vertical front porch lines */
#define LT6911UXC_V_SW			0x8671	/* vsync length lines */

/* Sync polarity */
#define LT6911UXC_SYNC_POL		0x8670
#define LT6911UXC_MASK_VSYNC_POL	BIT(1)
#define LT6911UXC_MASK_HSYNC_POL	BIT(0)

/* ---- Frequency meter (pixel clock) ---- */
#define LT6911UXC_MASK_FMI_FREQ2	0x0F
#define LT6911UXC_FM1_FREQ_IN2		0x8548
#define LT6911UXC_FM1_FREQ_IN1		0x8549
#define LT6911UXC_FM1_FREQ_IN0		0x854A
#define LT6911UXC_AD_HALF_PCLK		0x8540

/* ---- MIPI TX ---- */
#define LT6911UXC_MIPI_TX_CTRL		0x811D
#define LT6911UXC_MIPI_TX_ENABLE	0xFB
#define LT6911UXC_MIPI_TX_DISABLE	0x00
#define LT6911UXC_MIPI_LANES		0x86A2
#define LT6911UXC_MIPI_CLK_MODE		0xD468

/* ---- Audio ---- */
#define LT6911UXC_AUDIO_SR		0xB0AB

/* ---- Interrupts ---- */
#define LT6911UXC_INT_HDMI		0x86A3
#define LT6911UXC_INT_HDMI_STABLE	0x55
#define LT6911UXC_INT_HDMI_DISCONNECT	0x88

#define LT6911UXC_INT_AUDIO		0x86A5
#define LT6911UXC_INT_AUDIO_DISCONNECT	0x88
#define LT6911UXC_INT_AUDIO_SR_HIGH	0x55
#define LT6911UXC_INT_AUDIO_SR_LOW	0xAA

#endif /* __LT6911UXC_REGS_H__ */
