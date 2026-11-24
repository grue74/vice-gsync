/*
 * mainc64cpu.h - x64sc main CPU extra API.
 *
 * Written by
 *  Hannu Nuotio <hannu.nuotio@tut.fi>
 *
 * This file is part of VICE, the Versatile Commodore Emulator.
 * See README for copyright notice.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA
 *  02111-1307  USA.
 *
 */

#ifndef VICE_MAINC64CPU_H
#define VICE_MAINC64CPU_H

/* Mask: BA low */
#define MAINCPU_BA_LOW_VICII 1
#define MAINCPU_BA_LOW_REU   2
extern int maincpu_ba_low_flags;

/* CPU turbo (Ultimate 64 style), "TurboMode" resource values */
#define MAINCPU_TURBO_MODE_OFF          0   /**< always 1 MHz */
#define MAINCPU_TURBO_MODE_MANUAL       1   /**< settings from the resources, no registers */
#define MAINCPU_TURBO_MODE_REGISTERS    2   /**< like manual, $D031 (and $D07A/$D07B) change them */
#define MAINCPU_TURBO_MODE_ENABLE_BIT   3   /**< like registers, and $D030 bit 0 switches turbo on/off */

/* CPU cycles per system (1 MHz) cycle, and those done in the current one */
extern int maincpu_turbo_div;
extern int maincpu_turbo_cycle;

#endif
