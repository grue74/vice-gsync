/** \file   vsidmixerwidget.c
 * \brief   GTK3 mixer widget for VSID
 *
 * Needs some way of switching between SID model to display the proper values.
 *
 * \author  Bas Wassink <b.wassink@ziggo.nl>
 */

/*
 * $VICERES SidResidPassband        vsid
 * $VICERES SidResidGain            vsid
 * $VICERES SidResidFilterBias      vsid
 * $VICERES SidResid8580Passband    vsid
 * $VICERES SidResid8580Gain        vsid
 * $VICERES SidResid8580FilterBias  vsid
 * $VICERES SidResid6581FilterCurve vsid
 * $VICERES SidResid6581FilterRange vsid
 * $VICERES SidResid8580FilterCurve vsid
 * $VICERES SidResidCombinedWaveformStrength vsid
 * $VICERES SidResidFilterC1        vsid
 * $VICERES SidResidFilterC2        vsid
 * $VICERES SidResid6581OldCaps     vsid
 * $VICERES SidResid6581DacRatio    vsid
 * $VICERES SidResid6581Snake       vsid
 * $VICERES SidResidResonanceStrength vsid
 * $VICERES SidResid6581OpampGain   vsid
 * $VICERES SidResid6581ResonanceQ  vsid
 * $VICERES SidResid6581WaveOffset  vsid
 * $VICERES SidFilters              vsid
 * $VICERES SidEngine               vsid
 * $VICERES SidModel                vsid
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
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <gtk/gtk.h>

#include "vice_gtk3.h"
#include "archdep.h"
#include "debug.h"
#include "util.h"
#include "machine.h"
#include "lib.h"
#include "log.h"
#include "resources.h"
#include "sid.h"

#include "sidfilterpresets.h"
#include "sidcurvewidget.h"
#include "vsidmixerwidget.h"


#if defined(HAVE_RESID) || defined(HAVE_RESID_DTV) || defined(HAVE_RESIDFP)

/*
 * SID panel of the VSID main window: engine and model, filter on/off, all
 * filter sliders of the current engine and model, and (reSIDfp) the filter
 * capacitors C1/C2 with their presets. The panel is rebuilt when the engine or
 * the model changes, so it only shows what applies.
 */

/** \brief  CSS for the scales: less vertical space, fixed width value */
#define SLIDER_CSS \
    "scale {\n" \
    "  margin-top:    -2px;\n" \
    "  margin-bottom: -2px;\n" \
    "}\n" \
    "scale value {\n" \
    "  min-width: 5em;\n" \
    "}"

/** \brief  CSS for the labels: less vertical space */
#define LABEL_CSS \
   "label {\n" \
   "  margin-top:    -2px;\n" \
   "  margin-bottom: -2px;\n" \
   "}"

/** \brief  Most sliders on the panel at once */
#define MAX_SCALES  12

/** \brief  CSS provider for a label */
static GtkCssProvider *label_css_provider;
/** \brief  CSS provider for a scale  */
static GtkCssProvider *scale_css_provider;

/** \brief  The panel (a GtkGrid) */
static GtkWidget *main_grid;

/** \brief  Sliders on the panel now, for "Reset to defaults" */
static GtkWidget *scale_widgets[MAX_SCALES];
static int scale_count = 0;

#ifdef HAVE_RESIDFP
/** \brief  C1 and C2 sliders, for the presets */
static GtkWidget *cap_scales[2];
#endif

/** \brief  Engine and model the panel was built for */
static int shown_engine = -1;
static int shown_model = -1;

/** \brief  A rebuild is queued */
static guint rebuild_source = 0;

static void rebuild(void);

/** \brief  Information on a slider */
typedef struct mixer_scale_s {
    const char *label;          /**< label in front of the scale */
    const char *resource_name;  /**< resource name */
    const char *display_format; /**< format string for the value */
    gint        resource_low;   /**< resource value lower bound */
    gint        resource_high;  /**< resource value upper bound */
    gdouble     display_low;    /**< displayed value lower bound */
    gdouble     display_high;   /**< displayed value upper bound */
    gdouble     display_step;   /**< stepping for the scale */
    const char *tooltip;        /**< tooltip, or NULL */
} mixer_scale_t;

#ifdef HAVE_RESID
static const mixer_scale_t scales_6581[] = {
    { "Passband", "SidResidPassband", "%3.0f%%",
      RESID_6581_PASSBAND_MIN, RESID_6581_PASSBAND_MAX,
      RESID_6581_PASSBAND_MIN, RESID_6581_PASSBAND_MAX, 1.0, NULL },
    { "Gain", "SidResidGain", "%3.0f%%",
      RESID_6581_FILTER_GAIN_MIN, RESID_6581_FILTER_GAIN_MAX,
      RESID_6581_FILTER_GAIN_MIN, RESID_6581_FILTER_GAIN_MAX, 1.0, NULL },
    { "Filter bias", "SidResidFilterBias", "%+1.2fmV",
      RESID_6581_FILTER_BIAS_MIN, RESID_6581_FILTER_BIAS_MAX,
      (RESID_6581_FILTER_BIAS_MIN / RESID_6581_FILTER_BIAS_ONE),
      (RESID_6581_FILTER_BIAS_MAX / RESID_6581_FILTER_BIAS_ONE), 0.01, NULL },
    { NULL, NULL, NULL, 0, 0, 0, 0, 0, NULL }
};

static const mixer_scale_t scales_8580[] = {
    { "Passband", "SidResid8580Passband", "%3.0f%%",
      RESID_8580_PASSBAND_MIN, RESID_8580_PASSBAND_MAX,
      RESID_8580_PASSBAND_MIN, RESID_8580_PASSBAND_MAX, 1.0, NULL },
    { "Gain", "SidResid8580Gain", "%3.0f%%",
      RESID_8580_FILTER_GAIN_MIN, RESID_8580_FILTER_GAIN_MAX,
      RESID_8580_FILTER_GAIN_MIN, RESID_8580_FILTER_GAIN_MAX, 1.0, NULL },
    { "Filter bias", "SidResid8580FilterBias", "%+1.2fmV",
      RESID_8580_FILTER_BIAS_MIN, RESID_8580_FILTER_BIAS_MAX,
      (RESID_8580_FILTER_BIAS_MIN / RESID_8580_FILTER_BIAS_ONE),
      (RESID_8580_FILTER_BIAS_MAX / RESID_8580_FILTER_BIAS_ONE), 0.01, NULL },
    { NULL, NULL, NULL, 0, 0, 0, 0, 0, NULL }
};
#endif

#ifdef HAVE_RESIDFP
static const mixer_scale_t fp_scales_6581[] = {
    { "Filter curve", "SidResid6581FilterCurve", "%1.2f",
      RESIDFP_6581_FILTER_CURVE_MIN, RESIDFP_6581_FILTER_CURVE_MAX,
      RESIDFP_6581_FILTER_CURVE_MIN / RESIDFP_6581_FILTER_CURVE_ONE,
      RESIDFP_6581_FILTER_CURVE_MAX / RESIDFP_6581_FILTER_CURVE_ONE, 0.01,
      "Shifts the 6581 cutoff curve (chips differ a lot)" },
    { "Filter range", "SidResid6581FilterRange", "%1.2f",
      RESIDFP_6581_FILTER_RANGE_MIN, RESIDFP_6581_FILTER_RANGE_MAX,
      RESIDFP_6581_FILTER_RANGE_MIN / RESIDFP_6581_FILTER_RANGE_ONE,
      RESIDFP_6581_FILTER_RANGE_MAX / RESIDFP_6581_FILTER_RANGE_ONE, 0.01,
      "Widens or narrows the 6581 cutoff range" },
    { "DAC kink (2R/R)", "SidResid6581DacRatio", "%1.3f",
      RESIDFP_6581_DAC_RATIO_MIN, RESIDFP_6581_DAC_RATIO_MAX,
      (double)RESIDFP_6581_DAC_RATIO_MIN / RESIDFP_6581_DAC_RATIO_ONE,
      (double)RESIDFP_6581_DAC_RATIO_MAX / RESIDFP_6581_DAC_RATIO_ONE, 0.005,
      "2R/R ratio of the 6581 cutoff DAC. 2.000 is an ideal, smooth DAC;"
      " higher values deepen the cutoff drops at FC 256, 512 and 1024"
      " (2.200 is the reSIDfp default). Applies while playing." },
    { "Snake resistor", "SidResid6581Snake", "\xc3\x97%1.2f",
      RESIDFP_6581_SNAKE_MIN, RESIDFP_6581_SNAKE_MAX,
      (double)RESIDFP_6581_SNAKE_MIN / RESIDFP_6581_SNAKE_ONE,
      (double)RESIDFP_6581_SNAKE_MAX / RESIDFP_6581_SNAKE_ONE, 0.01,
      "Width of the 6581 snake transistor (the fixed resistor beside the cutoff"
      " transistor), relative to nominal. It sets the floor of the cutoff curve:"
      " higher = lower resistance = higher minimum cutoff. The top of the curve"
      " barely moves. Applies while playing." },
    { "Op-amp gain", "SidResid6581OpampGain", "\xc3\x97%1.2f",
      RESIDFP_6581_OPAMP_GAIN_MIN, RESIDFP_6581_OPAMP_GAIN_MAX,
      (double)RESIDFP_6581_OPAMP_GAIN_MIN / RESIDFP_6581_OPAMP_GAIN_ONE,
      (double)RESIDFP_6581_OPAMP_GAIN_MAX / RESIDFP_6581_OPAMP_GAIN_ONE, 0.01,
      "Gain of the 6581's NMOS op-amps around their working point. Their low"
      " gain makes the filter lossy and limits the resonance (even at resonance"
      " 15). Higher = stronger resonance peak and a different distortion, but"
      " also a higher cutoff (about x2 at 2.00): compensate with Filter range."
      " Applied when the slider stops moving." },
    { "Resonance Q", "SidResid6581ResonanceQ", "\xc3\x97%1.2f",
      RESIDFP_6581_RESONANCE_Q_MIN, RESIDFP_6581_RESONANCE_Q_MAX,
      (double)RESIDFP_6581_RESONANCE_Q_MIN / RESIDFP_6581_RESONANCE_Q_ONE,
      (double)RESIDFP_6581_RESONANCE_Q_MAX / RESIDFP_6581_RESONANCE_Q_ONE, 0.01,
      "Scales the 6581 filter Q at every resonance value, 15 included, without"
      " moving the cutoff or raising the bandpass level. Resonance strength has"
      " no effect at resonance 15; this does. Applies while playing." },
    { "Wave offset", "SidResid6581WaveOffset", "%.0f",
      RESIDFP_6581_WAVE_OFFSET_MIN, RESIDFP_6581_WAVE_OFFSET_MAX,
      RESIDFP_6581_WAVE_OFFSET_MIN, RESIDFP_6581_WAVE_OFFSET_MAX, 1,
      "Zero level of the 6581 waveform DAC (raw DAC value, 896 default):"
      " the DC level of the voices going into the filter and mixer. Moves"
      " the filter working point, so the filter distortion changes, and"
      " sets how loud volume register writes (digis, clicks) are."
      " Applies while playing." },
    { NULL, NULL, NULL, 0, 0, 0, 0, 0, NULL }
};

static const mixer_scale_t fp_scales_8580[] = {
    { "Filter curve", "SidResid8580FilterCurve", "%1.2f",
      RESIDFP_8580_FILTER_CURVE_MIN, RESIDFP_8580_FILTER_CURVE_MAX,
      RESIDFP_8580_FILTER_CURVE_MIN / RESIDFP_8580_FILTER_CURVE_ONE,
      RESIDFP_8580_FILTER_CURVE_MAX / RESIDFP_8580_FILTER_CURVE_ONE, 0.01,
      "Shifts the 8580 cutoff curve" },
    { NULL, NULL, NULL, 0, 0, 0, 0, 0, NULL }
};

/** \brief  Resonance strength, both models */
static const mixer_scale_t fp_scales_resonance[] = {
    { "Resonance strength", "SidResidResonanceStrength", "\xc3\x97%1.2f",
      RESIDFP_RESONANCE_STRENGTH_MIN, RESIDFP_RESONANCE_STRENGTH_MAX,
      (double)RESIDFP_RESONANCE_STRENGTH_MIN / RESIDFP_RESONANCE_STRENGTH_ONE,
      (double)RESIDFP_RESONANCE_STRENGTH_MAX / RESIDFP_RESONANCE_STRENGTH_ONE, 0.01,
      "Scales the resonance ladder: Q is multiplied by this (x2.00 = double Q,"
      " x0.50 = half). On the 8580 it controls Q fully; on the 6581 it works"
      " below resonance 15 (use Op-amp gain for the top). Applied when the"
      " slider stops moving." },
    { NULL, NULL, NULL, 0, 0, 0, 0, 0, NULL }
};

/** \brief  Filter capacitors, both models */
static const mixer_scale_t fp_scales_caps[] = {
    { "Filter cap C1", "SidResidFilterC1", "%+5.1f%%",
      RESIDFP_FILTER_CAP_MIN, RESIDFP_FILTER_CAP_MAX,
      RESIDFP_FILTER_CAP_MIN / RESIDFP_FILTER_CAP_ONE,
      RESIDFP_FILTER_CAP_MAX / RESIDFP_FILTER_CAP_ONE, 0.1,
      "CAP1 (lowpass integrator), deviation from nominal. Real boards use"
      " 10 % parts. C1\xc3\x97" "C2 moves the cutoff. Applies while playing." },
    { "Filter cap C2", "SidResidFilterC2", "%+5.1f%%",
      RESIDFP_FILTER_CAP_MIN, RESIDFP_FILTER_CAP_MAX,
      RESIDFP_FILTER_CAP_MIN / RESIDFP_FILTER_CAP_ONE,
      RESIDFP_FILTER_CAP_MAX / RESIDFP_FILTER_CAP_ONE, 0.1,
      "CAP2 (bandpass integrator), deviation from nominal. C2 larger than C1"
      " gives more resonance, smaller gives less. Applies while playing." },
    { NULL, NULL, NULL, 0, 0, 0, 0, 0, NULL }
};

/** \brief  Combined waveform strength (SidResidCombinedWaveformStrength) */
static const vice_gtk3_combo_entry_int_t combined_strengths[] = {
    { "Weak",    0 },
    { "Average", 1 },
    { "Strong",  2 },
    VICE_GTK3_COMBO_ENTRY_INT_LIST_END
};
#endif

/** \brief  Handler for the 'destroy' event of the panel: unref CSS providers */
static void on_destroy(GtkWidget *self, gpointer data)
{
    if (rebuild_source != 0) {
        g_source_remove(rebuild_source);
        rebuild_source = 0;
    }
    if (label_css_provider != NULL) {
        g_object_unref(label_css_provider);
        label_css_provider = NULL;
    }
    if (scale_css_provider != NULL) {
        g_object_unref(scale_css_provider);
        scale_css_provider = NULL;
    }
    main_grid = NULL;
}

/** \brief  "Reset to defaults": all sliders on the panel to factory values */
static void on_reset_clicked(GtkWidget *widget, gpointer data)
{
    int i;

    for (i = 0; i < scale_count; i++) {
        vice_gtk3_resource_scale_custom_factory(scale_widgets[i]);
    }
}

/** \brief  Create a left-aligned label using Pango markup */
static GtkWidget *create_label(const char *markup)
{
    GtkWidget *label;

    label = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(label), markup);
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    vice_gtk3_css_provider_add(label, label_css_provider);
    return label;
}

/** \brief  Add a row of sliders to the panel
 *
 * \return  next free row
 */
static int add_scales(GtkWidget *grid, int row, const mixer_scale_t *scales)
{
    int i;

    for (i = 0; scales[i].label != NULL && scale_count < MAX_SCALES; i++) {
        GtkWidget *label;
        GtkWidget *scale;

        label = create_label(scales[i].label);
        scale = vice_gtk3_resource_scale_custom_new(scales[i].resource_name,
                                                    GTK_ORIENTATION_HORIZONTAL,
                                                    scales[i].resource_low,
                                                    scales[i].resource_high,
                                                    scales[i].display_low,
                                                    scales[i].display_high,
                                                    scales[i].display_step,
                                                    scales[i].display_format);
        gtk_widget_set_hexpand(scale, TRUE);
        gtk_widget_set_halign(scale, GTK_ALIGN_FILL);
        gtk_scale_set_value_pos(GTK_SCALE(scale), GTK_POS_RIGHT);
        gtk_scale_set_draw_value(GTK_SCALE(scale), TRUE);
        vice_gtk3_css_provider_add(scale, scale_css_provider);
        if (scales[i].tooltip != NULL) {
            gtk_widget_set_tooltip_text(scale, scales[i].tooltip);
            gtk_widget_set_tooltip_text(label, scales[i].tooltip);
        }
        gtk_grid_attach(GTK_GRID(grid), label, 0, row, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), scale, 1, row, 1, 1);
        scale_widgets[scale_count++] = scale;
        row++;
    }
    return row;
}

/** \brief  Engine/model combo box changed */
static void on_engine_model_changed(GtkComboBox *combo, gpointer data)
{
    const char *id = gtk_combo_box_get_active_id(combo);
    int value;
    int engine;
    int model;
    int cur_engine = 0;
    int cur_model = 0;

    if (id == NULL) {
        return;
    }
    value = atoi(id);
    engine = (value >> 8) & 0xff;
    model = value & 0xff;
    resources_get_int("SidEngine", &cur_engine);
    resources_get_int("SidModel", &cur_model);
    if (engine != cur_engine || model != cur_model) {
        sid_set_engine_model(engine, model);
        vsid_mixer_widget_update();
    }
}

/** \brief  Create the engine/model combo box */
static GtkWidget *create_engine_model_combo(void)
{
    GtkWidget *combo = gtk_combo_box_text_new();
    sid_engine_model_t **list = sid_get_engine_model_list();
    int engine = 0;
    int model = 0;
    char id[16];
    int i;

    resources_get_int("SidEngine", &engine);
    resources_get_int("SidModel", &model);
    for (i = 0; list[i] != NULL; i++) {
        g_snprintf(id, sizeof id, "%d", list[i]->value);
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(combo), id, list[i]->name);
    }
    g_snprintf(id, sizeof id, "%d", (engine << 8) | model);
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(combo), id);
    gtk_widget_set_hexpand(combo, TRUE);
    g_signal_connect(combo, "changed", G_CALLBACK(on_engine_model_changed), NULL);
    return combo;
}

#ifdef HAVE_RESIDFP
/** \brief  One random 10 % part: normal, tolerance at 2 sigma, parts outside
 *          the tolerance rejected
 *
 * \return  deviation in 0.1 %
 */
static int random_part(void)
{
    const double tolerance = 10.0;
    const double sigma = tolerance / 2.0;
    double z;

    do {
        double u = g_random_double();
        double v = g_random_double();
        if (u < 1e-12) {
            u = 1e-12;
        }
        z = sqrt(-2.0 * log(u)) * cos(2.0 * G_PI * v) * sigma;
    } while (fabs(z) > tolerance);

    return (int)lround(z * RESIDFP_FILTER_CAP_ONE);
}

/** \brief  Capacitor presets: 0 random board, 1 matched pair, 2 most
 *          resonance, 3 least resonance
 */
static void on_cap_preset(GtkWidget *button, gpointer data)
{
    int c1 = 0;
    int c2 = 0;

    switch (GPOINTER_TO_INT(data)) {
        case 0:
            c1 = random_part();
            c2 = random_part();
            break;
        case 2:
            c1 = -100;
            c2 = 100;
            break;
        case 3:
            c1 = 100;
            c2 = -100;
            break;
        default:
            break;
    }
    if (cap_scales[0] != NULL && cap_scales[1] != NULL) {
        vice_gtk3_resource_scale_custom_set(cap_scales[0], c1);
        vice_gtk3_resource_scale_custom_set(cap_scales[1], c2);
    }
}

/** \brief  Create the 2x2 grid of capacitor preset buttons */
static GtkWidget *create_cap_presets(void)
{
    static const char *names[4] = {
        "Random board", "Matched pair", "Most resonance", "Least resonance"
    };
    static const char *tips[4] = {
        "Two random 10 % capacitors, as on a real board",
        "C1 and C2 both nominal",
        "C1 -10 %, C2 +10 %",
        "C1 +10 %, C2 -10 %"
    };
    GtkWidget *grid = gtk_grid_new();
    int i;

    gtk_grid_set_column_spacing(GTK_GRID(grid), 4);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 4);
    gtk_grid_set_column_homogeneous(GTK_GRID(grid), TRUE);
    for (i = 0; i < 4; i++) {
        GtkWidget *button = gtk_button_new_with_label(names[i]);
        gtk_widget_set_tooltip_text(button, tips[i]);
        g_signal_connect(button, "clicked", G_CALLBACK(on_cap_preset), GINT_TO_POINTER(i));
        gtk_grid_attach(GTK_GRID(grid), button, i % 2, i / 2, 1, 1);
    }
    return grid;
}
#endif


static void queue_rebuild(void);

/** \brief  Opens the export dialog; set by VSID itself (see
 *          vsid_mixer_widget_set_export_handler), since this file is also
 *          linked into the other emulators, which have no PSID player
 */
static void (*export_handler)(void) = NULL;

void vsid_mixer_widget_set_export_handler(void (*handler)(void))
{
    export_handler = handler;
}

/** \brief  "Export tune..." button */
static void on_export_clicked(GtkWidget *button, gpointer data)
{
    if (export_handler != NULL) {
        export_handler();
    }
}

/** \brief  Fill the panel for the current engine and model */
static void populate(GtkWidget *grid)
{
    GtkWidget *label;
    GtkWidget *widget;
    int engine = 0;
    int model = 0;
    int is_8580;
    int row = 0;

    resources_get_int("SidEngine", &engine);
    resources_get_int("SidModel", &model);
    shown_engine = engine;
    shown_model = model;
    is_8580 = (model == SID_MODEL_8580 || model == SID_MODEL_8580D);
    scale_count = 0;
#ifdef HAVE_RESIDFP
    cap_scales[0] = NULL;
    cap_scales[1] = NULL;
#endif

    label = create_label("<b>SID</b>");
    gtk_grid_attach(GTK_GRID(grid), label, 0, row, 1, 1);
    if (export_handler != NULL) {
        widget = gtk_button_new_with_label("Export tune...");
        gtk_widget_set_tooltip_text(widget,
                "Save the tune (or all its subtunes) as MP3, FLAC, Ogg Vorbis or WAV,"
                " with the current SID and filter settings");
        gtk_widget_set_halign(widget, GTK_ALIGN_END);
        g_signal_connect(widget, "clicked", G_CALLBACK(on_export_clicked), NULL);
        gtk_grid_attach(GTK_GRID(grid), widget, 1, row, 1, 1);
    }
    row++;

    gtk_grid_attach(GTK_GRID(grid), create_label("Engine"), 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), create_engine_model_combo(), 1, row++, 1, 1);

    widget = vice_gtk3_resource_check_button_new("SidFilters", "Filter emulation");
    gtk_grid_attach(GTK_GRID(grid), widget, 1, row++, 1, 1);

#ifdef HAVE_RESID
    if (engine == SID_ENGINE_RESID) {
        row = add_scales(grid, row, is_8580 ? scales_8580 : scales_6581);
#ifndef HAVE_NEW_8580_FILTER
        if (is_8580) {
            int i;
            /* the old 8580 filter has no settings */
            for (i = 0; i < scale_count; i++) {
                gtk_widget_set_sensitive(scale_widgets[i], FALSE);
            }
        }
#endif
    }
#endif
#ifdef HAVE_RESIDFP
    if (engine == SID_ENGINE_RESIDFP) {
        row = add_scales(grid, row, is_8580 ? fp_scales_8580 : fp_scales_6581);

        widget = vice_gtk3_resource_combo_int_new("SidResidCombinedWaveformStrength",
                                                  combined_strengths);
        gtk_widget_set_tooltip_text(widget, "How strongly combined waveforms come through");
        gtk_grid_attach(GTK_GRID(grid), create_label("Combined waves"), 0, row, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), widget, 1, row++, 1, 1);

        if (!is_8580) {
            widget = vice_gtk3_resource_check_button_new("SidResid6581OldCaps",
                                                         "Old 2200 pF filter capacitors");
            gtk_grid_attach(GTK_GRID(grid), widget, 1, row++, 1, 1);
        }

        row = add_scales(grid, row, fp_scales_resonance);

        {
            int first = scale_count;
            row = add_scales(grid, row, fp_scales_caps);
            cap_scales[0] = scale_widgets[first];
            cap_scales[1] = scale_widgets[first + 1];
        }
        widget = create_cap_presets();
        gtk_grid_attach(GTK_GRID(grid), widget, 1, row++, 1, 1);

        if (!is_8580) {
            gtk_grid_attach(GTK_GRID(grid), sid_curve_widget_create(), 0, row++, 2, 1);
        }
    }
#endif

    /* saved filter settings (shared with Settings > Audio > SID) */
    widget = create_label("My settings");
    gtk_widget_set_valign(widget, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(grid), widget, 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), sid_filter_presets_widget_create(queue_rebuild), 1, row++, 1, 1);

    if (scale_count > 0) {
        widget = gtk_button_new_with_label("Reset sliders to defaults");
        g_signal_connect(G_OBJECT(widget), "clicked", G_CALLBACK(on_reset_clicked), NULL);
        gtk_widget_set_margin_top(widget, 4);
        gtk_grid_attach(GTK_GRID(grid), widget, 0, row++, 2, 1);
    }

    gtk_widget_show_all(grid);
}

/** \brief  Remove everything from the panel and fill it again */
static void rebuild(void)
{
    GList *children;
    GList *iter;

    if (main_grid == NULL) {
        return;
    }
    children = gtk_container_get_children(GTK_CONTAINER(main_grid));
    for (iter = children; iter != NULL; iter = g_list_next(iter)) {
        gtk_widget_destroy(GTK_WIDGET(iter->data));
    }
    g_list_free(children);
    populate(main_grid);
}

/** \brief  Idle callback: rebuild outside of the signal handler that asked */
static gboolean rebuild_idle(gpointer data)
{
    rebuild_source = 0;
    rebuild();
    return G_SOURCE_REMOVE;
}

/** \brief  Rebuild the panel from the main loop (the caller may be one of
 *          the widgets the rebuild destroys)
 */
static void queue_rebuild(void)
{
    if (main_grid != NULL && rebuild_source == 0) {
        rebuild_source = g_idle_add(rebuild_idle, NULL);
    }
}

/** \brief  Create VSID SID panel
 *
 * \return  GtkGrid
 */
GtkWidget *vsid_mixer_widget_create(void)
{
    GtkWidget *grid;

    label_css_provider = vice_gtk3_css_provider_new(LABEL_CSS);
    scale_css_provider = vice_gtk3_css_provider_new(SLIDER_CSS);

    grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 2);
    gtk_widget_set_hexpand(grid, TRUE);
    main_grid = grid;

    populate(grid);

    g_signal_connect_unlocked(G_OBJECT(grid), "destroy", G_CALLBACK(on_destroy), NULL);
    return grid;
}

/** \brief  Update the panel if the SID engine or model changed
 *
 * Called on tune load, after the settings dialog changed the engine/model,
 * and by the panel's own engine selector. The rebuild runs from the main loop,
 * since the caller may be a widget the rebuild destroys.
 */
void vsid_mixer_widget_update(void)
{
    int engine = 0;
    int model = 0;

    if (main_grid == NULL) {
        return;
    }
    resources_get_int("SidEngine", &engine);
    resources_get_int("SidModel", &model);
    if ((engine != shown_engine || model != shown_model) && rebuild_source == 0) {
        rebuild_source = g_idle_add(rebuild_idle, NULL);
    }
}

#else   /* no reSID engine */

GtkWidget *vsid_mixer_widget_create(void)
{
    GtkWidget *grid;
    grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    gtk_widget_set_hexpand(grid, TRUE);

    return grid;
}

void vsid_mixer_widget_update(void)
{
}

#endif
