/*
 * c64turbo.c - CPU turbo, Ultimate 64 style (x64 and x64sc).
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

#include <stdio.h>

#include "alarm.h"
#include "c64meminit.h"
#include "c64turbo.h"
#include "cmdline.h"
#include "dma.h"
#include "maincpu.h"
#include "mem.h"
#include "resources.h"
#include "snapshot.h"
#include "types.h"

int maincpu_turbo_div = 1;
int maincpu_turbo_cycle = 0;
int c64turbo_badlines = 1;
int c64turbo_io_check = 0;

/* U64 speed index ($D031 bits 0-3) to MHz */
static const int turbo_speeds[16] = {
    1, 2, 3, 4, 5, 6, 8, 10, 12, 14, 16, 20, 24, 32, 40, 48
};

/* Resources */
static int turbo_mode_res = MAINCPU_TURBO_MODE_OFF;
static int turbo_speed_res = 4;             /* MHz */
static int turbo_badlines_res = 1;          /* badline timing */
static int turbo_iec_slowdown_res = 1;      /* 1 MHz for a while after an IEC access */

/* Runtime state */
static int turbo_enabled = 0;           /* on/off ($D030 bit 0, $D07A/$D07B) */
static uint8_t turbo_d031 = 0;          /* speed index (bits 0-3), badlines off (bit 7) */
static int turbo_iec_slow = 0;          /* slowed down by an IEC access */
static alarm_t *turbo_iec_alarm = NULL;

/*
 * System cycles at 1 MHz after an access to the IEC port ($DD00/$DD01).
 * Not something the U64 does: the KERNAL serial routines and fast loaders
 * time the bus with CPU loops, which a faster CPU breaks. Polling $DD00
 * keeps the CPU at 1 MHz for the whole transfer.
 */
#define TURBO_IEC_SLOW_CYCLES   1000

/* Index of a speed in turbo_speeds, or -1 */
static int turbo_speed_index(int mhz)
{
    int i;

    for (i = 0; i < 16; i++) {
        if (turbo_speeds[i] == mhz) {
            return i;
        }
    }
    return -1;
}

/* Work out the CPU speed from the settings and the runtime state */
static void turbo_apply(void)
{
    int on = turbo_mode_res != MAINCPU_TURBO_MODE_OFF && turbo_enabled;
    int div = 1;
    int badlines = 1;

    if (on && !turbo_iec_slow) {
        if (turbo_mode_res == MAINCPU_TURBO_MODE_MANUAL) {
            div = turbo_speed_res;
            badlines = turbo_badlines_res;
        } else {
            div = turbo_speeds[turbo_d031 & 0x0f];
            badlines = !(turbo_d031 & 0x80);
        }
    }

    maincpu_turbo_div = div;
    c64turbo_badlines = (div > 1) ? badlines : 1;
    if (div == 1) {
        maincpu_turbo_cycle = 0;
    } else if (maincpu_turbo_cycle >= div) {
        maincpu_turbo_cycle = div - 1;
    }

    /* x64: the VIC-II steals the cycles of badlines and sprites through DMA */
    dma_maincpu_steal_disabled = !c64turbo_badlines;

    c64turbo_io_check = div > 1
                     || turbo_mode_res >= MAINCPU_TURBO_MODE_REGISTERS
                     || (on && turbo_iec_slowdown_res);
}

/* (Re)load the runtime state from the resources */
static void turbo_load(int keep_enabled)
{
    if (!keep_enabled) {
        /* in "enable bit" mode the machine starts at 1 MHz, like a C128 */
        turbo_enabled = turbo_mode_res == MAINCPU_TURBO_MODE_MANUAL
                     || turbo_mode_res == MAINCPU_TURBO_MODE_REGISTERS;
    }
    turbo_d031 = (uint8_t)(turbo_speed_index(turbo_speed_res) | (turbo_badlines_res ? 0x00 : 0x80));
    turbo_apply();
}

static void turbo_iec_alarm_handler(CLOCK offset, void *data)
{
    alarm_unset(turbo_iec_alarm);
    turbo_iec_slow = 0;
    turbo_apply();
}

static void turbo_iec_stop(void)
{
    if (turbo_iec_alarm) {
        alarm_unset(turbo_iec_alarm);
    }
    turbo_iec_slow = 0;
}

void c64turbo_reset(void)
{
    turbo_iec_stop();
    maincpu_turbo_cycle = 0;
    turbo_load(0);
}

/* An access to $DD00/$DD01 (or a mirror): stay at 1 MHz for a while */
void c64turbo_iec_access(unsigned int addr)
{
    if ((addr & 0xff0e) == 0xdd00
        && turbo_iec_slowdown_res
        && turbo_enabled
        && turbo_mode_res != MAINCPU_TURBO_MODE_OFF) {
        if (!turbo_iec_alarm) {
            turbo_iec_alarm = alarm_new(maincpu_alarm_context, "CPUTurboIEC", turbo_iec_alarm_handler, NULL);
        }
        alarm_set(turbo_iec_alarm, maincpu_clk + TURBO_IEC_SLOW_CYCLES);
        if (!turbo_iec_slow) {
            turbo_iec_slow = 1;
            turbo_apply();
        }
    }
}

int c64turbo_is_io(unsigned int addr)
{
    if ((addr & 0xf000) != 0xd000) {
        return 0;
    }
    if ((addr & 0x0c00) == 0x0800) {
        return 0; /* $D800-$DBFF colour RAM, internal */
    }
    return c64meminit_io_config[mem_get_current_bank_config()] != 0;
}

int c64turbo_register_read(unsigned int addr, uint8_t *value)
{
    if (addr == 0xd031 && turbo_mode_res >= MAINCPU_TURBO_MODE_REGISTERS) {
        *value = turbo_d031;
        return 1;
    }
    if (addr == 0xd030 && turbo_mode_res == MAINCPU_TURBO_MODE_ENABLE_BIT) {
        *value = (uint8_t)(0xfe | turbo_enabled);
        return 1;
    }
    return 0;
}

void c64turbo_register_store(unsigned int addr, uint8_t value)
{
    if (turbo_mode_res < MAINCPU_TURBO_MODE_REGISTERS) {
        return;
    }
    switch (addr) {
        case 0xd030:
            if (turbo_mode_res == MAINCPU_TURBO_MODE_ENABLE_BIT) {
                turbo_enabled = value & 1;
                turbo_apply();
            }
            break;
        case 0xd031:
            turbo_d031 = value;
            turbo_apply();
            break;
        case 0xd07a:    /* SuperCPU compatible: normal speed */
            turbo_enabled = 0;
            turbo_apply();
            break;
        case 0xd07b:    /* SuperCPU compatible: turbo */
            turbo_enabled = 1;
            turbo_apply();
            break;
        default:
            break;
    }
}

/* ------------------------------------------------------------------------- */

static int set_turbo_mode(int val, void *param)
{
    int old = turbo_mode_res;

    if (val < MAINCPU_TURBO_MODE_OFF || val > MAINCPU_TURBO_MODE_ENABLE_BIT) {
        return -1;
    }
    turbo_mode_res = val;
    if (val == MAINCPU_TURBO_MODE_OFF) {
        turbo_iec_stop();
    }
    turbo_load(old == val);
    return 0;
}

static int set_turbo_speed(int val, void *param)
{
    if (turbo_speed_index(val) < 0) {
        return -1;
    }
    turbo_speed_res = val;
    turbo_load(1);
    return 0;
}

static int set_turbo_badlines(int val, void *param)
{
    turbo_badlines_res = val ? 1 : 0;
    turbo_load(1);
    return 0;
}

static int set_turbo_iec_slowdown(int val, void *param)
{
    turbo_iec_slowdown_res = val ? 1 : 0;
    if (!turbo_iec_slowdown_res) {
        turbo_iec_stop();
    }
    turbo_apply();
    return 0;
}

static const resource_int_t resources_int[] = {
    { "TurboMode", MAINCPU_TURBO_MODE_OFF, RES_EVENT_SAME, NULL,
      &turbo_mode_res, set_turbo_mode, NULL },
    { "TurboSpeed", 4, RES_EVENT_SAME, NULL,
      &turbo_speed_res, set_turbo_speed, NULL },
    { "TurboBadlines", 1, RES_EVENT_SAME, NULL,
      &turbo_badlines_res, set_turbo_badlines, NULL },
    { "TurboIECSlowdown", 1, RES_EVENT_SAME, NULL,
      &turbo_iec_slowdown_res, set_turbo_iec_slowdown, NULL },
    RESOURCE_INT_LIST_END
};

int c64turbo_resources_init(void)
{
    return resources_register_int(resources_int);
}

static const cmdline_option_t cmdline_options[] =
{
    { "-turbomode", SET_RESOURCE, CMDLINE_ATTRIB_NEED_ARGS,
      NULL, NULL, "TurboMode", NULL,
      "<Mode>", "Set CPU turbo mode (Ultimate 64 style): (0: Off, 1: Manual, 2: U64 turbo registers ($D031), 3: Turbo enable bit ($D030))" },
    { "-turbospeed", SET_RESOURCE, CMDLINE_ATTRIB_NEED_ARGS,
      NULL, NULL, "TurboSpeed", NULL,
      "<MHz>", "Set CPU turbo speed: 1, 2, 3, 4, 5, 6, 8, 10, 12, 14, 16, 20, 24, 32, 40 or 48 MHz" },
    { "-turbobadlines", SET_RESOURCE, CMDLINE_ATTRIB_NONE,
      NULL, NULL, "TurboBadlines", (resource_value_t)1,
      NULL, "Enable badline timing in turbo mode (the CPU halts during VIC-II DMA)" },
    { "+turbobadlines", SET_RESOURCE, CMDLINE_ATTRIB_NONE,
      NULL, NULL, "TurboBadlines", (resource_value_t)0,
      NULL, "Disable badline timing in turbo mode" },
    { "-turboiecslowdown", SET_RESOURCE, CMDLINE_ATTRIB_NONE,
      NULL, NULL, "TurboIECSlowdown", (resource_value_t)1,
      NULL, "Run at 1 MHz for a while after each IEC bus ($DD00) access in turbo mode" },
    { "+turboiecslowdown", SET_RESOURCE, CMDLINE_ATTRIB_NONE,
      NULL, NULL, "TurboIECSlowdown", (resource_value_t)0,
      NULL, "Do not slow down on IEC bus accesses in turbo mode" },
    CMDLINE_LIST_END
};

int c64turbo_cmdline_options_init(void)
{
    return cmdline_register_options(cmdline_options);
}

/* ------------------------------------------------------------------------- */

/*
 * CPU turbo state, in a module of its own so that older snapshots (without
 * it) still load, with the turbo set from the current settings.
 *
 * type  | name     | description
 * -----------------------------
 * BYTE  | mode     | TurboMode
 * BYTE  | speed    | TurboSpeed (MHz)
 * BYTE  | badlines | TurboBadlines
 * BYTE  | enabled  | turbo on/off ($D030 bit 0, $D07A/$D07B)
 * BYTE  | d031     | $D031 register
 * DWORD | cycle    | CPU cycles done in the current system cycle
 */
static const char snap_module_name[] = "C64TURBO";
#define SNAP_MAJOR 1
#define SNAP_MINOR 0

int c64turbo_snapshot_write_module(snapshot_t *s)
{
    snapshot_module_t *m;

    m = snapshot_module_create(s, snap_module_name, SNAP_MAJOR, SNAP_MINOR);
    if (m == NULL) {
        return -1;
    }

    if (0
        || SMW_B(m, (uint8_t)turbo_mode_res) < 0
        || SMW_B(m, (uint8_t)turbo_speed_res) < 0
        || SMW_B(m, (uint8_t)turbo_badlines_res) < 0
        || SMW_B(m, (uint8_t)turbo_enabled) < 0
        || SMW_B(m, turbo_d031) < 0
        || SMW_DW(m, (uint32_t)maincpu_turbo_cycle) < 0) {
        snapshot_module_close(m);
        return -1;
    }

    return snapshot_module_close(m);
}

int c64turbo_snapshot_read_module(snapshot_t *s)
{
    uint8_t major, minor;
    uint8_t mode, speed, badlines, enabled, d031;
    uint32_t cycle;
    int error = snapshot_get_error();
    snapshot_module_t *m;

    turbo_iec_stop();

    m = snapshot_module_open(s, snap_module_name, &major, &minor);
    if (m == NULL) {
        /* older snapshot: the turbo as after a reset */
        snapshot_set_error(error);
        maincpu_turbo_cycle = 0;
        turbo_load(0);
        return 0;
    }

    if (snapshot_version_is_bigger(major, minor, SNAP_MAJOR, SNAP_MINOR)) {
        snapshot_set_error(SNAPSHOT_MODULE_HIGHER_VERSION);
        snapshot_module_close(m);
        return -1;
    }

    if (0
        || SMR_B(m, &mode) < 0
        || SMR_B(m, &speed) < 0
        || SMR_B(m, &badlines) < 0
        || SMR_B(m, &enabled) < 0
        || SMR_B(m, &d031) < 0
        || SMR_DW(m, &cycle) < 0) {
        snapshot_module_close(m);
        return -1;
    }

    resources_set_int("TurboMode", mode);
    resources_set_int("TurboSpeed", speed);
    resources_set_int("TurboBadlines", badlines);
    turbo_enabled = enabled ? 1 : 0;
    turbo_d031 = d031;
    maincpu_turbo_cycle = (int)cycle;
    turbo_apply();

    return snapshot_module_close(m);
}
