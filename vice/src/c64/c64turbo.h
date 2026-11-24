/*
 * c64turbo.h - CPU turbo, Ultimate 64 style (x64 and x64sc).
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

#ifndef VICE_C64TURBO_H
#define VICE_C64TURBO_H

#include "types.h"

/*
 * The CPU does maincpu_turbo_div cycles per system cycle. The system clock
 * (maincpu_clk) still runs at the normal rate, and with it the VIC-II, CIAs,
 * SID, drives and everything else.
 *
 * As on the U64, internal memory (RAM, ROMs, colour RAM, cartridge ROM) is
 * accessed at full speed, the I/O chips through a 1 MHz bus: each access to
 * $D000-$D7FF or $DC00-$DFFF (when I/O is mapped in) waits for the start of
 * the next system cycle and takes the whole cycle.
 *
 * This file holds what x64 and x64sc share: the settings, the registers, the
 * IEC slowdown and the snapshot module. How the CPU cycles are counted is up
 * to each CPU core (c64cpusc.c/mainc64cpu.c for x64sc, c64cpu.c for x64).
 */

/* "TurboMode" resource values */
#define MAINCPU_TURBO_MODE_OFF          0   /**< always 1 MHz */
#define MAINCPU_TURBO_MODE_MANUAL       1   /**< settings from the resources, no registers */
#define MAINCPU_TURBO_MODE_REGISTERS    2   /**< like manual, $D031 (and $D07A/$D07B) change them */
#define MAINCPU_TURBO_MODE_ENABLE_BIT   3   /**< like registers, and $D030 bit 0 switches turbo on/off */

/* CPU cycles per system (1 MHz) cycle (1 = turbo off) */
extern int maincpu_turbo_div;

/* CPU cycles done in the current system cycle (0 .. maincpu_turbo_div - 1) */
extern int maincpu_turbo_cycle;

/* the CPU halts while the VIC-II holds BA low (badline timing) */
extern int c64turbo_badlines;

/* I/O accesses must go through the turbo (I/O bus timing, registers, IEC) */
extern int c64turbo_io_check;

int c64turbo_resources_init(void);
int c64turbo_cmdline_options_init(void);

/* CPU reset: back to the settings (and to 1 MHz in "enable bit" mode) */
void c64turbo_reset(void);

/* Is this an access to the I/O chips on the 1 MHz bus? */
int c64turbo_is_io(unsigned int addr);

#define C64TURBO_IO(addr) \
    (c64turbo_io_check && ((addr) & 0xf000) == 0xd000 && c64turbo_is_io(addr))

/* A read of a turbo register: returns 1 and sets *value if it is one */
int c64turbo_register_read(unsigned int addr, uint8_t *value);

/* A write to $D0xx: the turbo registers (they also reach the VIC-II) */
void c64turbo_register_store(unsigned int addr, uint8_t value);

/* After each I/O access: an IEC port access slows down for a while */
void c64turbo_iec_access(unsigned int addr);

struct snapshot_s;
int c64turbo_snapshot_write_module(struct snapshot_s *s);
int c64turbo_snapshot_read_module(struct snapshot_s *s);

#endif
