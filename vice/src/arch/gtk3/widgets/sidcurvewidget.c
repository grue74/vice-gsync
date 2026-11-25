/** \file   sidcurvewidget.c
 * \brief   reSIDfp 6581 cutoff curve graph with draggable shape knots
 *
 * Shows the cutoff frequency against the FC register value for the playing
 * reSIDfp 6581 (computed on the sound side, see residfp_get_6581_curve()):
 * the curve with the current settings, and faintly the same settings without
 * the user shape. The nine knots of the shape (resources
 * SidResid6581CurveK0..K8, cutoff DAC offsets in mV at FC 0, 256, ..., 2047)
 * can be dragged up and down; a double click puts a knot back to 0.
 */

/*
 * $VICERES SidResid6581CurveK0    all
 * $VICERES SidResid6581CurveK1    all
 * $VICERES SidResid6581CurveK2    all
 * $VICERES SidResid6581CurveK3    all
 * $VICERES SidResid6581CurveK4    all
 * $VICERES SidResid6581CurveK5    all
 * $VICERES SidResid6581CurveK6    all
 * $VICERES SidResid6581CurveK7    all
 * $VICERES SidResid6581CurveK8    all
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
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 *  02111-1307  USA.
 *
 */

#include "vice.h"

#include <gtk/gtk.h>
#include <math.h>
#include <stdio.h>

#include "lib.h"
#include "resources.h"
#include "sid.h"

#include "sidcurvewidget.h"

#ifdef HAVE_RESIDFP

#include "sid/residfp.h"

#define KNOTS       RESIDFP_6581_CURVE_KNOTS
#define POINTS      RESIDFP_CURVE_POINTS

/* plot area margins (pixels) */
#define MARGIN_LEFT     44
#define MARGIN_RIGHT    10
#define MARGIN_TOP      8
#define MARGIN_BOTTOM   18

/* fixed frequency range, so that the graph doesn't jump while dragging */
#define HZ_MIN      50.0
#define HZ_MAX      25000.0

/* knot drag: mV per pixel */
#define MV_PER_PIXEL 2.0

/* knot grab distance (pixels) */
#define GRAB 9.0

/* The reSIDfp 6581 default curve (all filter settings at their defaults, PAL
   clock), as the same estimate computes it; drawn as a fixed reference. */
static const struct { int fc; double hz; } default_curve[] = {
    { 0, 361 }, { 32, 394 }, { 64, 431 }, { 96, 478 }, { 128, 524 }, { 160, 591 },
    { 192, 664 }, { 224, 752 }, { 255, 856 }, { 256, 809 }, { 288, 913 }, { 320, 1109 },
    { 352, 1323 }, { 384, 1554 }, { 416, 1843 }, { 448, 2137 }, { 480, 2474 }, { 511, 2827 },
    { 512, 2526 }, { 544, 2858 }, { 576, 3196 }, { 608, 3563 }, { 640, 3873 }, { 672, 4242 },
    { 704, 4650 }, { 736, 5039 }, { 767, 5443 }, { 768, 5243 }, { 800, 5623 }, { 832, 6035 },
    { 864, 6429 }, { 896, 6772 }, { 928, 7167 }, { 960, 7568 }, { 992, 7959 }, { 1023, 8391 },
    { 1024, 7656 }, { 1056, 8063 }, { 1088, 8484 }, { 1120, 8876 }, { 1152, 9173 }, { 1184, 9567 },
    { 1216, 9934 }, { 1248, 10350 }, { 1279, 10770 }, { 1280, 10576 }, { 1312, 10954 }, { 1344, 11361 },
    { 1376, 11729 }, { 1408, 12052 }, { 1440, 12416 }, { 1472, 12755 }, { 1504, 13130 }, { 1535, 13494 },
    { 1536, 13177 }, { 1568, 13532 }, { 1600, 13833 }, { 1632, 14222 }, { 1664, 14481 }, { 1696, 14820 },
    { 1728, 15127 }, { 1760, 15433 }, { 1791, 15785 }, { 1792, 15667 }, { 1824, 15972 }, { 1856, 16217 },
    { 1888, 16539 }, { 1920, 16782 }, { 1952, 17041 }, { 1984, 17310 }, { 2016, 17545 }, { 2047, 17832 },
};

typedef struct curve_state_s {
    GtkWidget *area;
    GtkWidget *info;
    guint timer;
    unsigned int serial;
    int count;
    double hz[POINTS];
    double ref[POINTS];
    int drag;           /* knot being dragged, -1 none */
    int hover;          /* knot under the pointer, -1 none */
    double drag_y0;
    int drag_mv0;
    double drag_y;      /* current pointer y while dragging */
} curve_state_t;


static int knot_fc(int k)
{
    int fc = k * 2048 / (KNOTS - 1);
    return fc > 2047 ? 2047 : fc;
}

static int get_knot(int k)
{
    int v = 0;
    resources_get_int_sprintf("SidResid6581CurveK%d", &v, k);
    return v;
}

static void set_knot(int k, int v)
{
    if (v < RESIDFP_6581_CURVE_KNOT_MIN) {
        v = RESIDFP_6581_CURVE_KNOT_MIN;
    } else if (v > RESIDFP_6581_CURVE_KNOT_MAX) {
        v = RESIDFP_6581_CURVE_KNOT_MAX;
    }
    resources_set_int_sprintf("SidResid6581CurveK%d", v, k);
}

/* plot geometry */
static void plot_rect(GtkWidget *w, double *x0, double *y0, double *pw, double *ph)
{
    *x0 = MARGIN_LEFT;
    *y0 = MARGIN_TOP;
    *pw = gtk_widget_get_allocated_width(w) - MARGIN_LEFT - MARGIN_RIGHT;
    *ph = gtk_widget_get_allocated_height(w) - MARGIN_TOP - MARGIN_BOTTOM;
}

static double fc_to_x(GtkWidget *w, double fc)
{
    double x0, y0, pw, ph;
    plot_rect(w, &x0, &y0, &pw, &ph);
    return x0 + fc / 2047.0 * pw;
}

static double x_to_fc(GtkWidget *w, double x)
{
    double x0, y0, pw, ph;
    double fc;
    plot_rect(w, &x0, &y0, &pw, &ph);
    fc = (x - x0) / pw * 2047.0;
    return fc < 0 ? 0 : (fc > 2047 ? 2047 : fc);
}

static double hz_to_y(GtkWidget *w, double hz)
{
    double x0, y0, pw, ph;
    double t;
    plot_rect(w, &x0, &y0, &pw, &ph);
    if (hz < HZ_MIN) {
        hz = HZ_MIN;
    }
    if (hz > HZ_MAX) {
        hz = HZ_MAX;
    }
    t = (log10(hz) - log10(HZ_MIN)) / (log10(HZ_MAX) - log10(HZ_MIN));
    return y0 + ph * (1.0 - t);
}

/* cutoff at any FC from a computed curve (log interpolation between the
   points, which are not evenly spaced); 0 = unknown */
static double curve_at(const double *c, int count, double fc)
{
    int i;
    double f;
    double f0, f1;

    if (count < 2) {
        return 0.0;
    }
    for (i = 0; i < count - 2 && residfp_curve_fc(i + 1) <= fc; i++) {
        /* find the segment */
    }
    f0 = residfp_curve_fc(i);
    f1 = residfp_curve_fc(i + 1);
    f = (f1 > f0) ? (fc - f0) / (f1 - f0) : 0.0;
    if (f < 0.0) {
        f = 0.0;
    } else if (f > 1.0) {
        f = 1.0;
    }
    if (c[i] <= 0.0 || c[i + 1] <= 0.0) {
        return f < 0.5 ? c[i] : c[i + 1];
    }
    return exp(log(c[i]) * (1.0 - f) + log(c[i + 1]) * f);
}

static void format_hz(char *buf, size_t size, double hz)
{
    if (hz <= 0.0) {
        g_snprintf(buf, size, "?");
    } else if (hz >= 1000.0) {
        g_snprintf(buf, size, "%.1f kHz", hz / 1000.0);
    } else {
        g_snprintf(buf, size, "%.0f Hz", hz);
    }
}

/* knot handle position */
static void knot_pos(curve_state_t *st, int k, double *x, double *y)
{
    GtkWidget *w = st->area;
    *x = fc_to_x(w, knot_fc(k));
    if (st->drag == k) {
        *y = st->drag_y;
    } else {
        *y = hz_to_y(w, curve_at(st->hz, st->count, knot_fc(k)));
    }
}

static int knot_at(curve_state_t *st, double x, double y)
{
    int k;
    int best = -1;
    double bd = GRAB * GRAB;

    if (st->count == 0) {
        return -1;
    }
    for (k = 0; k < KNOTS; k++) {
        double kx, ky, d;
        knot_pos(st, k, &kx, &ky);
        d = (kx - x) * (kx - x) + (ky - y) * (ky - y);
        if (d <= bd) {
            bd = d;
            best = k;
        }
    }
    return best;
}

/* the default curve at any FC (log interpolation) */
static double default_at(double fc)
{
    int i;
    int n = (int)G_N_ELEMENTS(default_curve);
    double f;

    for (i = 0; i < n - 2 && default_curve[i + 1].fc <= fc; i++) {
        /* find the segment */
    }
    f = (fc - default_curve[i].fc) / (double)(default_curve[i + 1].fc - default_curve[i].fc);
    if (f < 0.0) {
        f = 0.0;
    } else if (f > 1.0) {
        f = 1.0;
    }
    return exp(log(default_curve[i].hz) * (1.0 - f) + log(default_curve[i + 1].hz) * f);
}

static void update_info(curve_state_t *st, double x)
{
    char text[256];
    char a[32], b[32], c[32];
    int k = st->drag >= 0 ? st->drag : st->hover;

    if (st->count == 0) {
        gtk_label_set_text(GTK_LABEL(st->info), "");
        return;
    }
    if (k >= 0) {
        format_hz(a, sizeof a, curve_at(st->hz, st->count, knot_fc(k)));
        g_snprintf(text, sizeof text,
                   "Knot at FC %d: %+d mV, cutoff %s. Drag up/down; double click = 0.",
                   knot_fc(k), get_knot(k), a);
    } else if (x >= 0) {
        double fc = x_to_fc(st->area, x);
        format_hz(a, sizeof a, curve_at(st->hz, st->count, fc));
        format_hz(b, sizeof b, curve_at(st->ref, st->count, fc));
        format_hz(c, sizeof c, default_at(fc));
        g_snprintf(text, sizeof text, "FC %d: cutoff %s (without the knots %s, default %s)",
                   (int)(fc + 0.5), a, b, c);
    } else {
        g_snprintf(text, sizeof text,
                   "Cutoff at full level (approx.). Drag the dots to shape the curve.");
    }
    gtk_label_set_text(GTK_LABEL(st->info), text);
}

static void set_source(cairo_t *cr, const GdkRGBA *c, double alpha)
{
    cairo_set_source_rgba(cr, c->red, c->green, c->blue, c->alpha * alpha);
}

static void draw_curve(cairo_t *cr, GtkWidget *w, const double *c, int count)
{
    int i;
    int pen = 0;

    for (i = 0; i < count; i++) {
        double x, y;
        if (c[i] <= 0.0) {
            pen = 0;
            continue;
        }
        x = fc_to_x(w, residfp_curve_fc(i));
        y = hz_to_y(w, c[i]);
        if (pen) {
            cairo_line_to(cr, x, y);
        } else {
            cairo_move_to(cr, x, y);
            pen = 1;
        }
    }
    cairo_stroke(cr);
}

static gboolean on_draw(GtkWidget *w, cairo_t *cr, gpointer data)
{
    curve_state_t *st = data;
    GtkStyleContext *style = gtk_widget_get_style_context(w);
    GdkRGBA fg;
    GdkRGBA accent;
    double x0, y0, pw, ph;
    static const double grid_hz[] = { 100, 200, 500, 1000, 2000, 5000, 10000, 20000 };
    static const char *grid_label[] = { "100", "200", "500", "1k", "2k", "5k", "10k", "20k" };
    int i;
    char buf[32];

    gtk_style_context_get_color(style, gtk_style_context_get_state(style), &fg);
    if (!gtk_style_context_lookup_color(style, "theme_selected_bg_color", &accent)) {
        gdk_rgba_parse(&accent, "#2a78d6");
    }
    accent.alpha = 1.0;
    plot_rect(w, &x0, &y0, &pw, &ph);

    cairo_set_font_size(cr, 10.0);

    /* frame and grid */
    cairo_set_line_width(cr, 1.0);
    set_source(cr, &fg, 0.35);
    cairo_rectangle(cr, x0 + 0.5, y0 + 0.5, pw - 1, ph - 1);
    cairo_stroke(cr);
    for (i = 0; i < (int)G_N_ELEMENTS(grid_hz); i++) {
        double y = floor(hz_to_y(w, grid_hz[i])) + 0.5;
        cairo_text_extents_t ext;
        set_source(cr, &fg, 0.12);
        cairo_move_to(cr, x0, y);
        cairo_line_to(cr, x0 + pw, y);
        cairo_stroke(cr);
        set_source(cr, &fg, 0.7);
        cairo_text_extents(cr, grid_label[i], &ext);
        cairo_move_to(cr, x0 - 6 - ext.width, y + ext.height / 2);
        cairo_show_text(cr, grid_label[i]);
    }
    for (i = 0; i < KNOTS; i++) {
        double x = floor(fc_to_x(w, knot_fc(i))) + 0.5;
        cairo_text_extents_t ext;
        set_source(cr, &fg, 0.12);
        cairo_move_to(cr, x, y0);
        cairo_line_to(cr, x, y0 + ph);
        cairo_stroke(cr);
        if (i % 2 == 0) {
            g_snprintf(buf, sizeof buf, "%d", knot_fc(i));
            set_source(cr, &fg, 0.7);
            cairo_text_extents(cr, buf, &ext);
            cairo_move_to(cr, x - ext.width / 2, y0 + ph + 13);
            cairo_show_text(cr, buf);
        }
    }

    /* the reSIDfp default curve: fixed, grey */
    cairo_save(cr);
    cairo_rectangle(cr, x0, y0, pw, ph);
    cairo_clip(cr);
    cairo_set_line_width(cr, 1.5);
    set_source(cr, &fg, 0.4);
    for (i = 0; i < (int)G_N_ELEMENTS(default_curve); i++) {
        double x = fc_to_x(w, default_curve[i].fc);
        double y = hz_to_y(w, default_curve[i].hz);
        if (i == 0) {
            cairo_move_to(cr, x, y);
        } else {
            cairo_line_to(cr, x, y);
        }
    }
    cairo_stroke(cr);
    cairo_restore(cr);

    /* legend, top left (the curves rise to the right) */
    {
        static const double dash[] = { 4.0, 3.0 };
        double lx = x0 + 8, ly = y0 + 12;
        int n = 0;
        const char *names[3] = { "reSIDfp default", "current", "current, knots at 0" };

        for (n = 0; n < (st->count > 0 ? 3 : 1); n++) {
            cairo_set_line_width(cr, n == 1 ? 2.0 : 1.5);
            if (n == 0) {
                set_source(cr, &fg, 0.4);
            } else {
                set_source(cr, &accent, n == 1 ? 1.0 : 0.55);
            }
            if (n == 2) {
                cairo_set_dash(cr, dash, 2, 0);
            }
            cairo_move_to(cr, lx, ly + n * 13 - 3.5);
            cairo_line_to(cr, lx + 18, ly + n * 13 - 3.5);
            cairo_stroke(cr);
            cairo_set_dash(cr, NULL, 0, 0);
            set_source(cr, &fg, 0.75);
            cairo_move_to(cr, lx + 24, ly + n * 13);
            cairo_show_text(cr, names[n]);
        }
    }

    if (st->count == 0) {
        const char *msg = "Current curve shown while a reSIDfp 6581 is playing";
        cairo_text_extents_t ext;
        cairo_set_font_size(cr, 12.0);
        cairo_text_extents(cr, msg, &ext);
        set_source(cr, &fg, 0.7);
        cairo_move_to(cr, x0 + (pw - ext.width) / 2, y0 + ph / 2);
        cairo_show_text(cr, msg);
        return FALSE;
    }

    cairo_save(cr);
    cairo_rectangle(cr, x0, y0, pw, ph);
    cairo_clip(cr);

    /* without the shape: faint, dashed */
    {
        static const double dash[] = { 4.0, 3.0 };
        cairo_set_dash(cr, dash, 2, 0);
        cairo_set_line_width(cr, 1.5);
        set_source(cr, &accent, 0.55);
        draw_curve(cr, w, st->ref, st->count);
        cairo_set_dash(cr, NULL, 0, 0);
    }
    /* with the shape */
    cairo_set_line_width(cr, 2.0);
    set_source(cr, &accent, 1.0);
    draw_curve(cr, w, st->hz, st->count);

    /* knots: filled when moved from 0 */
    for (i = 0; i < KNOTS; i++) {
        double kx, ky;
        double r = (i == st->drag || i == st->hover) ? 6.0 : 4.5;
        knot_pos(st, i, &kx, &ky);
        cairo_new_path(cr);
        cairo_arc(cr, kx, ky, r, 0, 2 * G_PI);
        if (get_knot(i) != 0) {
            set_source(cr, &accent, 1.0);
            cairo_fill(cr);
        } else {
            set_source(cr, &accent, 1.0);
            cairo_set_line_width(cr, 1.5);
            cairo_stroke(cr);
        }
    }
    cairo_restore(cr);
    return FALSE;
}

static gboolean on_button_press(GtkWidget *w, GdkEventButton *ev, gpointer data)
{
    curve_state_t *st = data;
    int k;

    if (ev->button != 1) {
        return FALSE;
    }
    k = knot_at(st, ev->x, ev->y);
    if (k < 0) {
        return FALSE;
    }
    if (ev->type == GDK_2BUTTON_PRESS) {
        set_knot(k, 0);
        st->drag = -1;
        gtk_widget_queue_draw(w);
        update_info(st, ev->x);
        return TRUE;
    }
    if (ev->type == GDK_BUTTON_PRESS) {
        double kx, ky;
        knot_pos(st, k, &kx, &ky);
        st->drag = k;
        st->drag_y0 = ev->y;
        st->drag_y = ky;
        st->drag_mv0 = get_knot(k);
        /* keep the handle where it was grabbed relative to the pointer */
        st->drag_y0 = ev->y;
        st->drag_y = ev->y;
        gtk_widget_queue_draw(w);
        update_info(st, ev->x);
        return TRUE;
    }
    return FALSE;
}

static gboolean on_button_release(GtkWidget *w, GdkEventButton *ev, gpointer data)
{
    curve_state_t *st = data;

    if (ev->button == 1 && st->drag >= 0) {
        st->drag = -1;
        gtk_widget_queue_draw(w);
        update_info(st, ev->x);
        return TRUE;
    }
    return FALSE;
}

static gboolean on_motion(GtkWidget *w, GdkEventMotion *ev, gpointer data)
{
    curve_state_t *st = data;

    if (st->drag >= 0) {
        int mv = st->drag_mv0 + (int)lround((st->drag_y0 - ev->y) * MV_PER_PIXEL);
        if (mv < RESIDFP_6581_CURVE_KNOT_MIN) {
            mv = RESIDFP_6581_CURVE_KNOT_MIN;
        } else if (mv > RESIDFP_6581_CURVE_KNOT_MAX) {
            mv = RESIDFP_6581_CURVE_KNOT_MAX;
        }
        st->drag_y = ev->y;
        if (mv != get_knot(st->drag)) {
            set_knot(st->drag, mv);
        }
        gtk_widget_queue_draw(w);
    } else {
        int k = knot_at(st, ev->x, ev->y);
        if (k != st->hover) {
            st->hover = k;
            gtk_widget_queue_draw(w);
        }
    }
    update_info(st, ev->x);
    return TRUE;
}

static gboolean on_leave(GtkWidget *w, GdkEventCrossing *ev, gpointer data)
{
    curve_state_t *st = data;

    if (st->drag < 0) {
        st->hover = -1;
        gtk_widget_queue_draw(w);
        update_info(st, -1);
    }
    return FALSE;
}

/* poll for a new curve from the sound side */
static gboolean on_timer(gpointer data)
{
    curve_state_t *st = data;
    unsigned int serial = 0;
    double hz[POINTS], ref[POINTS];
    int n = residfp_get_6581_curve(hz, ref, &serial);

    if (serial != st->serial || n != st->count) {
        int i;
        st->serial = serial;
        st->count = n;
        for (i = 0; i < n; i++) {
            st->hz[i] = hz[i];
            st->ref[i] = ref[i];
        }
        gtk_widget_queue_draw(st->area);
        if (st->drag < 0 && st->hover < 0) {
            update_info(st, -1);
        }
    }
    return G_SOURCE_CONTINUE;
}

static void on_flat_clicked(GtkWidget *button, gpointer data)
{
    curve_state_t *st = data;
    int k;

    for (k = 0; k < KNOTS; k++) {
        set_knot(k, 0);
    }
    gtk_widget_queue_draw(st->area);
}

static void on_destroy(GtkWidget *w, gpointer data)
{
    curve_state_t *st = data;

    if (st->timer != 0) {
        g_source_remove(st->timer);
    }
    lib_free(st);
}

GtkWidget *sid_curve_widget_create(void)
{
    GtkWidget *grid;
    GtkWidget *title;
    GtkWidget *button;
    curve_state_t *st = lib_calloc(1, sizeof *st);

    st->drag = -1;
    st->hover = -1;
    st->serial = (unsigned int)-1;

    grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 2);
    gtk_widget_set_margin_top(grid, 8);

    title = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(title), "<b>6581 cutoff curve</b>");
    gtk_widget_set_halign(title, GTK_ALIGN_START);
    gtk_widget_set_hexpand(title, TRUE);
    gtk_grid_attach(GTK_GRID(grid), title, 0, 0, 1, 1);

    button = gtk_button_new_with_label("Flat");
    gtk_widget_set_tooltip_text(button, "Put all knots back to 0 (the plain model curve)");
    g_signal_connect(button, "clicked", G_CALLBACK(on_flat_clicked), st);
    gtk_grid_attach(GTK_GRID(grid), button, 1, 0, 1, 1);

    st->area = gtk_drawing_area_new();
    gtk_widget_set_size_request(st->area, 360, 190);
    gtk_widget_set_hexpand(st->area, TRUE);
    gtk_widget_add_events(st->area, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK
                          | GDK_POINTER_MOTION_MASK | GDK_LEAVE_NOTIFY_MASK);
    gtk_widget_set_tooltip_text(st->area,
            "Cutoff frequency against the FC register value, for the playing"
            " reSIDfp 6581 with the current settings (approximate, at full"
            " signal level; low in the curve the 6581 cutoff also depends on"
            " the signal level). The dots move the cutoff DAC voltage at FC 0,"
            " 256, ..., 2047, linear in between. Applies while playing.");
    g_signal_connect(st->area, "draw", G_CALLBACK(on_draw), st);
    g_signal_connect(st->area, "button-press-event", G_CALLBACK(on_button_press), st);
    g_signal_connect(st->area, "button-release-event", G_CALLBACK(on_button_release), st);
    g_signal_connect(st->area, "motion-notify-event", G_CALLBACK(on_motion), st);
    g_signal_connect(st->area, "leave-notify-event", G_CALLBACK(on_leave), st);
    gtk_grid_attach(GTK_GRID(grid), st->area, 0, 1, 2, 1);

    st->info = gtk_label_new("");
    gtk_widget_set_halign(st->info, GTK_ALIGN_START);
    gtk_label_set_ellipsize(GTK_LABEL(st->info), PANGO_ELLIPSIZE_END);
    gtk_grid_attach(GTK_GRID(grid), st->info, 0, 2, 2, 1);

    st->timer = g_timeout_add(150, on_timer, st);
    g_signal_connect(grid, "destroy", G_CALLBACK(on_destroy), st);
    on_timer(st);
    update_info(st, -1);
    return grid;
}

#else

GtkWidget *sid_curve_widget_create(void)
{
    return gtk_grid_new();
}

#endif
