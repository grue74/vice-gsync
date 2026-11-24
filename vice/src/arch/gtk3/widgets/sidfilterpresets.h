/** \file   sidfilterpresets.h
 * \brief   Saved SID filter settings ("My settings") - header
 */

/*
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

#ifndef VICE_SIDFILTERPRESETS_H
#define VICE_SIDFILTERPRESETS_H

#include <gtk/gtk.h>

/* Create the saved-settings controls (list + Delete, name + Save, status).
 * on_loaded is called after a preset was loaded, so the host can show the new
 * values on its sliders. It may be NULL. */
GtkWidget *sid_filter_presets_widget_create(void (*on_loaded)(void));

#endif
