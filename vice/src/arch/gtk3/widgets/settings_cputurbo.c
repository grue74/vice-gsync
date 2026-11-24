/** \file   settings_cputurbo.c
 * \brief   Settings widget for the x64/x64sc CPU turbo (Ultimate 64 style)
 */

/*
 * $VICERES TurboMode           x64 x64sc
 * $VICERES TurboSpeed          x64 x64sc
 * $VICERES TurboBadlines       x64 x64sc
 * $VICERES TurboIECSlowdown    x64 x64sc
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

#include "vice.h"
#include <gtk/gtk.h>

#include "vice_gtk3.h"

#include "settings_cputurbo.h"


/** \brief  Turbo control modes (values as in mainc64cpu.h) */
static const vice_gtk3_radiogroup_entry_t turbo_modes[] = {
    { "Off (always 1 MHz)",                         0 },
    { "Manual (settings below)",                    1 },
    { "U64 turbo registers ($D031)",                2 },
    { "Turbo enable bit ($D030 bit 0, C128 style)", 3 },
    VICE_GTK3_RADIOGROUP_ENTRY_LIST_END
};

/** \brief  CPU speeds of the Ultimate 64 */
static const vice_gtk3_combo_entry_int_t turbo_speeds[] = {
    { "1 MHz",   1 }, { "2 MHz",   2 }, { "3 MHz",   3 }, { "4 MHz",   4 },
    { "5 MHz",   5 }, { "6 MHz",   6 }, { "8 MHz",   8 }, { "10 MHz", 10 },
    { "12 MHz", 12 }, { "14 MHz", 14 }, { "16 MHz", 16 }, { "20 MHz", 20 },
    { "24 MHz", 24 }, { "32 MHz", 32 }, { "40 MHz", 40 }, { "48 MHz", 48 },
    { NULL, -1 }
};

/** \brief  Create left-aligned label
 *
 * \param[in]   text    text for the label (Pango markup)
 *
 * \return  GtkLabel
 */
static GtkWidget *create_label(const char *text)
{
    GtkWidget *label = gtk_label_new(NULL);

    gtk_label_set_markup(GTK_LABEL(label), text);
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0);
    return label;
}

/** \brief  Create widget for the CPU turbo settings
 *
 * \param[in]   parent  parent widget (ignored)
 *
 * \return  GtkGrid
 */
GtkWidget *settings_cputurbo_widget_create(GtkWidget *parent)
{
    GtkWidget *grid;
    GtkWidget *label;
    GtkWidget *mode;
    GtkWidget *speed;
    GtkWidget *badlines;
    GtkWidget *iec;
    int row = 0;

    grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 16);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);

    label = create_label("<b>CPU turbo (Ultimate 64 style)</b>");
    gtk_grid_attach(GTK_GRID(grid), label, 0, row++, 2, 1);

    label = create_label("The CPU runs faster, the VIC-II, SID, CIAs and drives at normal speed."
                         " I/O accesses take a 1 MHz cycle each.");
    gtk_grid_attach(GTK_GRID(grid), label, 0, row++, 2, 1);

    label = create_label("Turbo control");
    gtk_widget_set_valign(label, GTK_ALIGN_START);
    mode = vice_gtk3_resource_radiogroup_new("TurboMode", turbo_modes, GTK_ORIENTATION_VERTICAL);
    gtk_grid_attach(GTK_GRID(grid), label, 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), mode,  1, row++, 1, 1);

    label = create_label("CPU speed");
    speed = vice_gtk3_resource_combo_int_new("TurboSpeed", turbo_speeds);
    gtk_widget_set_halign(speed, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(grid), label, 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), speed, 1, row++, 1, 1);

    badlines = vice_gtk3_resource_check_button_new("TurboBadlines",
            "Badline timing (the CPU halts during VIC-II DMA, as on a real C64)");
    gtk_grid_attach(GTK_GRID(grid), badlines, 1, row++, 1, 1);

    iec = vice_gtk3_resource_check_button_new("TurboIECSlowdown",
            "1 MHz during IEC bus transfers (KERNAL and fast loaders, not on a real U64)");
    gtk_grid_attach(GTK_GRID(grid), iec, 1, row++, 1, 1);

    label = create_label("<small>The speed and badline timing are the starting values for the registers;"
                         " a reset restores them. In \"enable bit\" mode the machine starts at 1 MHz"
                         " until a program sets $D030 bit 0.</small>");
    gtk_grid_attach(GTK_GRID(grid), label, 0, row++, 2, 1);

    gtk_widget_show_all(grid);
    return grid;
}
