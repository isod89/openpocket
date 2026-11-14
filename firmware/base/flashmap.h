/* SPDX-License-Identifier: GPL-3.0-only
 * The base firmware (docs/BASE-FIRMWARE.md): the flash an app other than OpenPocket may keep its own data in.
 *   0x04000..0xA3000   the firmware (an update rewrites it)
 *   0xC0000..0xE0000   OpenPocket's NAM models         } never an app's: they must be there, untouched, when
 *   0xF0000..0xFC000   OpenPocket's presets and IRs    } OpenPocket is installed again
 *   0xE0000..0xE5000   an update's staging area
 *   0xE5000..0xEF000   an app's own data (base/store.h), ten 4 KiB sectors
 * An app that uses base/store.h defines, before px_usb.h:   #define OP_FLASH_OWN(off, n) BS_FLASH_OWN(off, n)
 */
#pragma once
#define BS_LO 0xE5000u
#define BS_HI 0xEF000u
#define BS_FLASH_OWN(off, n) FL_IN(off, n, BS_LO, BS_HI)
