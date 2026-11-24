/*
 * residfp.h - reSIDfp interface code.
 *
 * Written by
 *  Teemu Rantanen <tvr@cs.hut.fi>
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

#ifndef VICE_RESIDFP_H
#define VICE_RESIDFP_H

#include "types.h"

extern sid_engine_t residfp_hooks;

/* Set the filter capacitor deviations (0.1 % units) of all reSIDfp chips.
   Takes effect at the next sound buffer, also while playing. */
void residfp_set_filter_caps(int c1, int c2);

/* Set the 6581 cutoff DAC 2R/R ratio (x1000) of all reSIDfp chips, live. */
void residfp_set_dac_ratio(int ratio);

/* Set the 6581 snake W/L scale (x1000) of all reSIDfp chips, live. */
void residfp_set_snake_scale(int scale);

/* Set the resonance strength (x1000, both models) and the 6581 op-amp gain
   (x1000). These rebuild tables shared by all chips (~50 ms each), so they
   are applied once the value has stayed the same for a moment. */
void residfp_set_resonance_strength(int strength);
void residfp_set_opamp_gain(int gain);

#endif
