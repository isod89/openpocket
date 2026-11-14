/* SPDX-License-Identifier: GPL-3.0-only
 * The chip's ADC for the host builds (tests/host/pins.h): each channel reads what the test put in h_adc. */
#pragma once
static int32_t h_adc[16];
static int32_t fm1_adc_read(uint32_t ch) { return ch < 16u ? h_adc[ch] : -1; }
