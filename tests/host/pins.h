/* SPDX-License-Identifier: GPL-3.0-only
 * The chip's ports as plain variables, for the host builds that run the base firmware's controls
 * (firmware/base/controls.h, input.h): tests/test_base_input.c and the apps' simulator, tests/appsim.c. Included before
 * base/controls.h, with -Itests/host so that controls.h finds the ADC of fm1_adc.h here.
 */
#pragma once
#include <stdint.h>
static uint32_t h_pa_in, h_pb_in, h_pc_in, h_ph_in, h_reg, h_bat_chg;
#define PA_IN h_pa_in
#define PB_IN h_pb_in
#define PC_IN h_pc_in
#define PH_IN h_ph_in
#define PA_DIR h_reg                            /* (the set-up registers: written, never read back) */
#define PA_DIE h_reg
#define PA_PU h_reg
#define PA_PD h_reg
#define PB_DIE h_reg
#define PB_PU h_reg
#define PC_DIR h_reg
#define PC_DIE h_reg
#define PC_PU h_reg
#define PC_PD h_reg
#define PC_OUT h_reg
#define PH_DIE h_reg
#define PH_PU h_reg
#define PH_DIR h_reg
#define PC7 (1u << 7)
#define BAT_CHG h_bat_chg
