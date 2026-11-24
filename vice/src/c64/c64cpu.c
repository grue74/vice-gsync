/*
 * c64cpu.c - Emulation of the main 6510 processor used for x64
 *
 * Written by groepaz <groepaz@gmx.net>
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

#include "vice.h"

#include "c64turbo.h"
#include "maincpu.h"
#include "mem.h"

#include "cpmcart.h"

#ifdef FEATURE_CPUMEMHISTORY
#include "monitor.h"
#include "c64pla.h"
#endif

/* ------------------------------------------------------------------------- */

/* MACHINE_STUFF should define/undef

 - NEED_REG_PC
 - TRACE

 The following are optional:

 - PAGE_ZERO
 - PAGE_ONE
 - STORE_IND
 - LOAD_IND
 - DMA_FUNC
 - DMA_ON_RESET
 - CHECK_AND_RUN_ALTERNATE_CPU

*/

#ifdef FEATURE_CPUMEMHISTORY
/* FIXME do proper ROM/RAM/IO tests */

/* FIXME: the following functions should handle IO/RAM/ROM and -dummy accesses
 * for the memmap feature - see mainc64cpu.c */

inline static void memmap_mem_update(unsigned int addr, int write)
{
    unsigned int type = MEMMAP_RAM_R;

    if (write) {
        if ((addr >= 0xd000) && (addr <= 0xdfff)) {
            type = MEMMAP_I_O_W;
        } else {
            type = MEMMAP_RAM_W;
        }
    } else {
        switch (addr >> 12) {
            case 0xa:
            case 0xb:
            case 0xe:
            case 0xf:
                if (pport.data_read & (1 << ((addr >> 14) & 1))) {
                    type = MEMMAP_ROM_R;
                } else {
                    type = MEMMAP_RAM_R;
                }
                break;
            case 0xd:
                type = MEMMAP_I_O_R;
                break;
            default:
                type = MEMMAP_RAM_R;
                break;
        }
        if (memmap_state & MEMMAP_STATE_OPCODE) {
            /* HACK: transform R to X */
            type >>= 2;
            memmap_state &= ~(MEMMAP_STATE_OPCODE);
        } else if (memmap_state & MEMMAP_STATE_INSTR) {
            /* ignore operand reads */
            type = 0;
        }
    }
    monitor_memmap_store(addr, type);
}

static void memmap_mem_store(unsigned int addr, unsigned int value)
{
    memmap_mem_update(addr, 1);
    (*_mem_write_tab_ptr[(addr) >> 8])((uint16_t)(addr), (uint8_t)(value));
}

static uint8_t memmap_mem_read(unsigned int addr)
{
    memmap_mem_update(addr, 0);
    return (*_mem_read_tab_ptr[(addr) >> 8])((uint16_t)(addr));
}

/* FIXME: is this really only needed for the MMU-Translate stuff? */
#if USE_MMU_TRANSLATE
static void memmap_mark_read(unsigned int addr)
{
    memmap_mem_update(addr, 0);
}
#endif

static void memmap_mem_store_dummy(unsigned int addr, unsigned int value)
{
    memmap_mem_update(addr, 1);
    (*_mem_write_tab_ptr_dummy[(addr) >> 8])((uint16_t)(addr), (uint8_t)(value));
}

static uint8_t memmap_mem_read_dummy(unsigned int addr)
{
    memmap_mem_update(addr, 0);
    return (*_mem_read_tab_ptr_dummy[(addr) >> 8])((uint16_t)(addr));
}
#endif

/* ------------------------------------------------------------------------- */
/*
 * CPU turbo, Ultimate 64 style (settings, registers, snapshot: c64turbo.c)
 *
 * The x64 CPU core adds the cycles of an instruction in steps (CLK_ADD), not
 * one by one as x64sc does. With the turbo on, those are CPU cycles: they go
 * into maincpu_turbo_cycle, and every maincpu_turbo_div of them advance the
 * system clock by one. This is not cycle exact (interrupts are noticed up to
 * a couple of system cycles late), but it is close, and x64 is fast.
 *
 * An I/O access starts at a system cycle of its own and takes all of it.
 * Badlines and sprites stop the CPU through DMA (dma_maincpu_steal_cycles),
 * which the turbo turns off when badline timing is off.
 */

inline static void c64cpu_clock_add(CLOCK *clock, int amount)
{
    int div = maincpu_turbo_div;
    int cycle;

    if (div == 1) {
        *clock += amount;
        return;
    }

    cycle = maincpu_turbo_cycle + amount;
    if (cycle >= div) {
        *clock += (CLOCK)(cycle / div);
        cycle %= div;
    } else if (cycle < 0) {
        int back = (-cycle + div - 1) / div;    /* rounded down, not to zero */

        *clock -= (CLOCK)back;
        cycle += back * div;
    }
    maincpu_turbo_cycle = cycle;
}

#define CLK_ADD(clock, amount) c64cpu_clock_add(&(clock), (amount))

/* ROM traps and JAM: the opcode fetch (2 CPU cycles) is done again */
#define REWIND_FETCH_OPCODE(clock) c64cpu_clock_add(&(clock), -2)

#define CPU_ADDITIONAL_RESET() c64turbo_reset()

/* Start of an I/O access: wait for a system cycle of its own */
inline static void turbo_bus_begin(void)
{
    if (maincpu_turbo_div > 1 && maincpu_turbo_cycle > 0) {
        maincpu_clk++;
        maincpu_turbo_cycle = 0;
    }
}

/* End of an I/O access: the CPU cycle after it ends the system cycle */
inline static void turbo_bus_end(void)
{
    if (maincpu_turbo_div > 1) {
        maincpu_turbo_cycle = maincpu_turbo_div - 1;
    }
}

static uint8_t turbo_io_read(unsigned int addr, read_func_ptr_t *tab)
{
    uint8_t value;

    turbo_bus_begin();
    if (!c64turbo_register_read(addr, &value)) {
        value = tab[addr >> 8]((uint16_t)addr);
    }
    c64turbo_iec_access(addr);
    turbo_bus_end();

    return value;
}

static void turbo_io_store(unsigned int addr, uint8_t value, store_func_ptr_t *tab)
{
    turbo_bus_begin();
    tab[addr >> 8]((uint16_t)addr, value);
    c64turbo_register_store(addr, value);
    c64turbo_iec_access(addr);
    turbo_bus_end();
}

inline static uint8_t c64cpu_load(unsigned int addr)
{
    if (C64TURBO_IO(addr)) {
#ifdef FEATURE_CPUMEMHISTORY
        memmap_mem_update(addr, 0);
#endif
        return turbo_io_read(addr, _mem_read_tab_ptr);
    }
#ifdef FEATURE_CPUMEMHISTORY
    return memmap_mem_read(addr);
#else
    return (*_mem_read_tab_ptr[addr >> 8])((uint16_t)addr);
#endif
}

inline static uint8_t c64cpu_load_dummy(unsigned int addr)
{
    if (C64TURBO_IO(addr)) {
#ifdef FEATURE_CPUMEMHISTORY
        memmap_mem_update(addr, 0);
#endif
        return turbo_io_read(addr, _mem_read_tab_ptr_dummy);
    }
#ifdef FEATURE_CPUMEMHISTORY
    return memmap_mem_read_dummy(addr);
#else
    return (*_mem_read_tab_ptr_dummy[addr >> 8])((uint16_t)addr);
#endif
}

inline static void c64cpu_store(unsigned int addr, uint8_t value)
{
    if (C64TURBO_IO(addr)) {
#ifdef FEATURE_CPUMEMHISTORY
        memmap_mem_update(addr, 1);
#endif
        turbo_io_store(addr, value, _mem_write_tab_ptr);
        return;
    }
#ifdef FEATURE_CPUMEMHISTORY
    memmap_mem_store(addr, value);
#else
    (*_mem_write_tab_ptr[addr >> 8])((uint16_t)addr, value);
#endif
}

inline static void c64cpu_store_dummy(unsigned int addr, uint8_t value)
{
    if (C64TURBO_IO(addr)) {
#ifdef FEATURE_CPUMEMHISTORY
        memmap_mem_update(addr, 1);
#endif
        turbo_io_store(addr, value, _mem_write_tab_ptr_dummy);
        return;
    }
#ifdef FEATURE_CPUMEMHISTORY
    memmap_mem_store_dummy(addr, value);
#else
    (*_mem_write_tab_ptr_dummy[addr >> 8])((uint16_t)addr, value);
#endif
}

#define LOAD(addr)                  c64cpu_load((unsigned int)(addr))
#define LOAD_DUMMY(addr)            c64cpu_load_dummy((unsigned int)(addr))
#define STORE(addr, value)          c64cpu_store((unsigned int)(addr), (uint8_t)(value))
#define STORE_DUMMY(addr, value)    c64cpu_store_dummy((unsigned int)(addr), (uint8_t)(value))

/* ------------------------------------------------------------------------- */

static void check_and_run_alternate_cpu(void)
{
    cpmcart_check_and_run_z80();
}

#define CHECK_AND_RUN_ALTERNATE_CPU check_and_run_alternate_cpu();

#define HAVE_Z80_REGS

#include "../maincpu.c"
