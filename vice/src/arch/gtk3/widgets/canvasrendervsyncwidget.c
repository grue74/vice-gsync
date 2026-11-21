/** \file   canvasrendervsyncwidget.c
 * \brief   Widget to select how the GL output is synced
 *
 * \author  Bas Wassink <b.wassink@ziggo.nl>
 * \author  groepaz <groepaz@gmx.net>
 */

/*
 * $VICERES CrtcVSync      xpet xcbm2
 * $VICERES TEDVSync       xplus4
 * $VICERES VDCVSync       x128
 * $VICERES VICVSync       xvic
 * $VICERES VICIIVSync     x64 x64sc xscpu64 xdtv64 x128 xcbm5x0
 * $VICERES CrtcCRTBeamSim  xpet xcbm2
 * $VICERES TEDCRTBeamSim   xplus4
 * $VICERES VDCCRTBeamSim   x128
 * $VICERES VICCRTBeamSim   xvic
 * $VICERES VICIICRTBeamSim x64 x64sc xscpu64 xdtv64 x128 xcbm5x0
 * $VICERES CrtcCRTBeamPersistence  xpet xcbm2
 * $VICERES TEDCRTBeamPersistence   xplus4
 * $VICERES VDCCRTBeamPersistence   x128
 * $VICERES VICCRTBeamPersistence   xvic
 * $VICERES VICIICRTBeamPersistence x64 x64sc xscpu64 xdtv64 x128 xcbm5x0
 * $VICERES CrtcCRTBeamDecay  xpet xcbm2
 * $VICERES TEDCRTBeamDecay   xplus4
 * $VICERES VDCCRTBeamDecay   x128
 * $VICERES VICCRTBeamDecay   xvic
 * $VICERES VICIICRTBeamDecay x64 x64sc xscpu64 xdtv64 x128 xcbm5x0
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

#include "canvasrendervsyncwidget.h"


/** \brief  Create widget to select the Gtk vsync method
 *
 * \param[in]   chip    video chip prefix
 *
 * \return  GtkGrid
 */
GtkWidget *canvas_render_vsync_widget_create(const char *chip)
{
    GtkWidget *grid;
    GtkWidget *label;
    GtkWidget *vsync_widget;

    grid = gtk_grid_new();

    label = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(label), "<b>Synchronization</b>");
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    gtk_widget_set_margin_bottom(label, 8);
    gtk_grid_attach(GTK_GRID(grid), label, 0, 0, 1, 1);

    vsync_widget = vice_gtk3_resource_check_button_new_sprintf("%sVSync",
                                                               "VSync",
                                                               chip);
    gtk_grid_attach(GTK_GRID(grid), vsync_widget, 0, 1, 1, 1);

#ifdef WINDOWS_COMPILE
    /* only the DirectX renderer implements this so far */
    {
        GtkWidget *beam_widget;
        GtkWidget *persist_label;
        GtkWidget *persist_widget;
        GtkWidget *decay_label;
        GtkWidget *decay_widget;

        beam_widget = vice_gtk3_resource_check_button_new_sprintf(
                "%sCRTBeamSim",
                "CRT beam simulation (rolling scan, needs VSync and a 120 Hz+ or VRR display)",
                chip);
        gtk_widget_set_tooltip_text(beam_widget,
                "Shows each frame as a rolling CRT-like scan over several display refreshes. "
                "Sharper motion than a held image, with a 50/60 Hz flicker like a real CRT.");
        gtk_grid_attach(GTK_GRID(grid), beam_widget, 0, 2, 1, 1);

        persist_label = gtk_label_new("CRT persistence (%): lower = sharper motion, more flicker");
        gtk_widget_set_halign(persist_label, GTK_ALIGN_START);
        gtk_widget_set_margin_start(persist_label, 16);
        gtk_grid_attach(GTK_GRID(grid), persist_label, 0, 3, 1, 1);

        persist_widget = vice_gtk3_resource_scale_int_new_sprintf(
                "%sCRTBeamPersistence",
                GTK_ORIENTATION_HORIZONTAL,
                0, 100, 5,
                chip);
        gtk_widget_set_hexpand(persist_widget, TRUE);
        gtk_widget_set_margin_start(persist_widget, 16);
        gtk_widget_set_tooltip_text(persist_widget,
                "How much of each frame every pixel stays lit at least. "
                "0 = shortest flash (sharpest motion, strongest flicker), "
                "100 = held for the whole frame (no flicker, like normal display).");
        gtk_grid_attach(GTK_GRID(grid), persist_widget, 0, 4, 1, 1);

        decay_label = gtk_label_new("CRT phosphor decay (%): afterglow that fades out behind the beam");
        gtk_widget_set_halign(decay_label, GTK_ALIGN_START);
        gtk_widget_set_margin_start(decay_label, 16);
        gtk_grid_attach(GTK_GRID(grid), decay_label, 0, 5, 1, 1);

        decay_widget = vice_gtk3_resource_scale_int_new_sprintf(
                "%sCRTBeamDecay",
                GTK_ORIENTATION_HORIZONTAL,
                0, 100, 5,
                chip);
        gtk_widget_set_hexpand(decay_widget, TRUE);
        gtk_widget_set_margin_start(decay_widget, 16);
        gtk_widget_set_tooltip_text(decay_widget,
                "Like a slow phosphor: after the beam has passed, each pixel fades out "
                "instead of going dark at once. The fade time is given in percent of a frame. "
                "0 = no afterglow; higher values soften the flicker but leave a trail behind moving objects.");
        gtk_grid_attach(GTK_GRID(grid), decay_widget, 0, 6, 1, 1);
    }
#endif

    gtk_widget_show_all(grid);
    return grid;
}
