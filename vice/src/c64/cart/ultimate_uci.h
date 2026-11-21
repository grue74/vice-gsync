/*
 * ultimate_uci.h - Ultimate Command Interface (1541 Ultimate-II / Ultimate 64) emulation.
 *
 * This file is part of VICE, the Versatile Commodore Emulator.
 * See README for copyright notice.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 */

#ifndef VICE_ULTIMATE_UCI_H
#define VICE_ULTIMATE_UCI_H

int ultimate_uci_resources_init(void);
void ultimate_uci_resources_shutdown(void);
int ultimate_uci_cmdline_options_init(void);
int ultimate_uci_cart_enabled(void);
void ultimate_uci_reset(void);

#endif
