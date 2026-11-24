/** \file   vsidexport.c
 * \brief   Export VSID tunes to MP3, FLAC, Ogg Vorbis or WAV files
 *
 * Plays a subtune (or all of them) from the start and records it through
 * VICE's sound recording devices, for the HVSC song length or a fixed time,
 * with an optional fade-out. In warp mode the export runs silently and much
 * faster than real time (the recording device keeps getting every sample).
 * The current SID engine and filter settings apply, so the file sounds like
 * the player.
 */

/*
 * $VICERES SoundRecordDeviceName   vsid
 * $VICERES SoundRecordDeviceArg    vsid
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
#include <string.h>

#include "filechooserhelpers.h"
#include "lib.h"
#include "machine.h"
#include "mainlock.h"
#include "psid.h"
#include "resources.h"
#include "sound.h"
#include "ui.h"
#include "vice_gtk3.h"
#include "vsidcontrolwidget.h"
#include "vsidstate.h"
#include "vsidtuneinfowidget.h"
#include "vsync.h"

#include "vsidexport.h"


/** \brief  An output format: a sound recording device */
typedef struct export_format_s {
    const char *name;       /**< shown in the combo box */
    const char *device;     /**< SoundRecordDeviceName */
    const char *ext;        /**< file name extension */
} export_format_t;

/** \brief  Formats in this build */
static const export_format_t formats[] = {
#ifdef USE_LAMEMP3
    { "MP3 (128 kbit/s)", "mp3", ".mp3" },
#endif
#ifdef USE_FLAC
    { "FLAC (lossless)", "flac", ".flac" },
#endif
#ifdef USE_VORBIS
    { "Ogg Vorbis", "ogg", ".ogg" },
#endif
    { "WAV (uncompressed)", "wav", ".wav" },
    { NULL, NULL, NULL }
};

#define MAX_TUNES 256

/** \brief  Dialog widgets and the state of a running export */
static struct {
    GtkWidget *dialog;
    GtkWidget *inputs;          /* grid of settings, insensitive while exporting */
    GtkWidget *entry;
    GtkWidget *format;
    GtkWidget *all_tunes;
    GtkWidget *use_db;
    GtkWidget *fixed_seconds;
    GtkWidget *fade_seconds;
    GtkWidget *fast;
    GtkWidget *progress;
    GtkWidget *status;
    GtkWidget *start;

    gboolean running;
    guint timer;
    int tunes[MAX_TUNES];
    int ntunes;
    int index;
    double length;              /* seconds of the subtune being recorded */
    double done_seconds;        /* seconds of the subtunes already exported */
    double total_seconds;       /* seconds of all subtunes to export */
    char *base;                 /* file name without extension (UTF-8) */
    const export_format_t *fmt;
    double fixed;
    double fade;
    gboolean db;
    int old_warp;
} ex;


/** \brief  Length of a subtune in seconds: HVSC song length, or the fixed time */
static double tune_length(int tune)
{
    long *lengths = NULL;
    int count = vsid_tune_info_widget_get_song_lengths(&lengths);

    if (ex.db && lengths != NULL && tune >= 1 && tune <= count && lengths[tune - 1] > 0) {
        return lengths[tune - 1] / 1000.0;
    }
    return ex.fixed;
}

static const export_format_t *current_format(void)
{
    int i = gtk_combo_box_get_active(GTK_COMBO_BOX(ex.format));

    if (i < 0) {
        i = 0;
    }
    return &formats[i];
}

/** \brief  File name without a known extension (g_free() it) */
static gchar *strip_extension(const char *name)
{
    gchar *base = g_strdup(name);
    int i;

    for (i = 0; formats[i].name != NULL; i++) {
        if (g_str_has_suffix(base, formats[i].ext)) {
            base[strlen(base) - strlen(formats[i].ext)] = '\0';
            break;
        }
    }
    return base;
}

static void set_inputs_sensitive(gboolean sensitive)
{
    if (ex.dialog != NULL) {
        gtk_widget_set_sensitive(ex.inputs, sensitive);
        gtk_button_set_label(GTK_BUTTON(ex.start), sensitive ? "Export" : "Cancel");
    }
}

/* The dialog may have been closed while the export goes on: these check */
static void set_status(const char *text)
{
    if (ex.dialog != NULL) {
        gtk_label_set_text(GTK_LABEL(ex.status), text);
    }
}

static void set_progress(double fraction)
{
    if (ex.dialog != NULL) {
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(ex.progress), fraction);
    }
}

/** \brief  Start playing and recording ex.tunes[ex.index] */
static void start_tune(void)
{
    int tune = ex.tunes[ex.index];
    gchar *filename;
    gchar *filename_locale;
    char *psid = NULL;
    vsid_state_t *state;
    char text[160];

    ex.length = tune_length(tune);
    if (ex.ntunes > 1) {
        filename = g_strdup_printf("%s-%02d%s", ex.base, tune, ex.fmt->ext);
    } else {
        filename = g_strdup_printf("%s%s", ex.base, ex.fmt->ext);
    }
    filename_locale = file_chooser_convert_to_locale(filename);

    /* record exactly the length, with the fade-out */
    sound_record_limit(ex.length, ex.fade);
    resources_set_string("SoundRecordDeviceArg", filename_locale);
    resources_set_string("SoundRecordDeviceName", ex.fmt->device);

    /* play the subtune from its start (as the player's own controls do) */
    state = vsid_state_lock();
    if (state->tune_current > 0) {
        state->tune_previous = state->tune_current;
    }
    state->tune_current = tune;
    if (state->psid_filename != NULL) {
        psid = lib_strdup(state->psid_filename);
    }
    vsid_state_unlock();
    if (psid != NULL) {
        psid_load_file(psid);
        lib_free(psid);
    }
    machine_play_psid(tune);
    machine_trigger_reset(MACHINE_RESET_MODE_RESET_CPU);
    ui_pause_disable();
    vsid_control_widget_set_state(VSID_PLAYING);

    g_snprintf(text, sizeof text, "Subtune %d (%d of %d), %d:%02d: %s",
               tune, ex.index + 1, ex.ntunes,
               (int)ex.length / 60, (int)ex.length % 60, filename);
    set_status(text);

    g_free(filename_locale);
    g_free(filename);
}

/** \brief  Stop the recording that is running */
static void stop_recording(void)
{
    resources_set_string("SoundRecordDeviceName", "");
    sound_record_limit(0.0, 0.0);
}

/** \brief  End the export: restore the speed, update the dialog */
static void finish(const char *message)
{
    if (ex.timer != 0) {
        g_source_remove(ex.timer);
        ex.timer = 0;
    }
    ex.running = FALSE;
    vsync_set_warp_mode(ex.old_warp);
    if (ex.base != NULL) {
        g_free(ex.base);
        ex.base = NULL;
    }
    set_inputs_sensitive(TRUE);
    set_status(message);
}

/** \brief  Timer: progress, next subtune, end */
static gboolean on_tick(gpointer data)
{
    const char *dev = NULL;

    if (!ex.running) {
        return G_SOURCE_REMOVE;
    }

    mainlock_obtain();

    if (sound_record_limit_reached()) {
        stop_recording();
        ex.done_seconds += ex.length;
        ex.index++;
        if (ex.index < ex.ntunes) {
            start_tune();
        } else {
            char text[64];
            g_snprintf(text, sizeof text, "Done: %d file%s written.",
                       ex.ntunes, ex.ntunes == 1 ? "" : "s");
            ex.timer = 0;   /* this source ends by returning FALSE */
            finish(text);
            set_progress(1.0);
            mainlock_release();
            return G_SOURCE_REMOVE;
        }
    } else {
        /* the device closes itself (resource cleared) when it can't write */
        resources_get_string("SoundRecordDeviceName", &dev);
        if (dev == NULL || *dev == '\0') {
            sound_record_limit(0.0, 0.0);
            ex.timer = 0;
            finish("Could not write the file: check the folder and the name (see the log).");
            mainlock_release();
            return G_SOURCE_REMOVE;
        }
    }

    if (ex.total_seconds > 0.0) {
        double f = (ex.done_seconds + sound_record_limit_written()) / ex.total_seconds;
        set_progress(f > 1.0 ? 1.0 : f);
    }
    mainlock_release();
    return G_SOURCE_CONTINUE;
}

/** \brief  Export / Cancel button */
static void on_start_clicked(GtkWidget *button, gpointer data)
{
    vsid_state_t *state;
    int current;
    int count;
    int i;
    const char *name;

    if (ex.running) {
        stop_recording();
        finish("Cancelled. The file of the current subtune is incomplete.");
        return;
    }

    name = gtk_entry_get_text(GTK_ENTRY(ex.entry));
    if (name == NULL || *name == '\0') {
        gtk_label_set_text(GTK_LABEL(ex.status), "Enter a file name first.");
        return;
    }

    state = vsid_state_lock();
    current = state->tune_current > 0 ? state->tune_current : state->tune_default;
    count = state->tune_count;
    vsid_state_unlock();
    if (count < 1) {
        gtk_label_set_text(GTK_LABEL(ex.status), "Load a tune first.");
        return;
    }

    ex.fmt = current_format();
    ex.base = strip_extension(name);
    ex.db = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ex.use_db));
    ex.fixed = gtk_spin_button_get_value(GTK_SPIN_BUTTON(ex.fixed_seconds));
    ex.fade = gtk_spin_button_get_value(GTK_SPIN_BUTTON(ex.fade_seconds));

    ex.ntunes = 0;
    if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ex.all_tunes))) {
        for (i = 1; i <= count && ex.ntunes < MAX_TUNES; i++) {
            ex.tunes[ex.ntunes++] = i;
        }
    } else {
        ex.tunes[ex.ntunes++] = current;
    }
    ex.total_seconds = 0.0;
    for (i = 0; i < ex.ntunes; i++) {
        ex.total_seconds += tune_length(ex.tunes[i]);
    }
    ex.done_seconds = 0.0;
    ex.index = 0;

    ex.old_warp = vsync_get_warp_mode();
    vsync_set_warp_mode(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ex.fast)) ? 1 : 0);

    ex.running = TRUE;
    set_inputs_sensitive(FALSE);
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(ex.progress), 0.0);
    start_tune();
    ex.timer = g_timeout_add(50, on_tick, NULL);
}

/** \brief  New format: change the extension of the file name */
static void on_format_changed(GtkWidget *combo, gpointer data)
{
    gchar *base = strip_extension(gtk_entry_get_text(GTK_ENTRY(ex.entry)));
    gchar *name = g_strdup_printf("%s%s", base, current_format()->ext);

    gtk_entry_set_text(GTK_ENTRY(ex.entry), name);
    g_free(name);
    g_free(base);
}

/** \brief  Browse button: choose the file */
static void on_browse_clicked(GtkWidget *button, gpointer data)
{
    GtkWidget *chooser;
    gchar *current = g_strdup(gtk_entry_get_text(GTK_ENTRY(ex.entry)));
    gint response;

    chooser = gtk_file_chooser_dialog_new("Export to",
                                          GTK_WINDOW(ex.dialog),
                                          GTK_FILE_CHOOSER_ACTION_SAVE,
                                          "_Cancel", GTK_RESPONSE_CANCEL,
                                          "_Save", GTK_RESPONSE_ACCEPT,
                                          NULL);
    gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(chooser), TRUE);
    if (current != NULL && *current != '\0') {
        gchar *dir = g_path_get_dirname(current);
        gchar *file = g_path_get_basename(current);
        gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(chooser), dir);
        gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(chooser), file);
        g_free(dir);
        g_free(file);
    }

    mainlock_release();
    response = gtk_dialog_run(GTK_DIALOG(chooser));
    mainlock_obtain();

    if (response == GTK_RESPONSE_ACCEPT) {
        gchar *name = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(chooser));
        if (name != NULL) {
            gtk_entry_set_text(GTK_ENTRY(ex.entry), name);
            g_free(name);
            on_format_changed(NULL, NULL);
        }
    }
    gtk_widget_destroy(chooser);
    g_free(current);
}

/** \brief  Dialog closed: a running export goes on in the background (the
 *          timer finishes it and restores the speed)
 */
static void on_dialog_destroy(GtkWidget *dialog, gpointer data)
{
    ex.dialog = NULL;
}

static void on_close_clicked(GtkWidget *button, gpointer data)
{
    gtk_widget_destroy(ex.dialog);
}

static GtkWidget *label_left(const char *text)
{
    GtkWidget *label = gtk_label_new(text);
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    return label;
}

void vsid_export_dialog_show(void)
{
    GtkWidget *content;
    GtkWidget *grid;
    GtkWidget *box;
    GtkWidget *button;
    GtkWidget *current_tune;
    GtkWidget *fixed_radio;
    vsid_state_t *state;
    long *lengths = NULL;
    int nlengths;
    int current;
    int count;
    int row = 0;
    int i;
    gchar *text;
    char *psid = NULL;

    if (ex.dialog != NULL) {
        gtk_window_present(GTK_WINDOW(ex.dialog));
        return;
    }

    state = vsid_state_lock();
    current = state->tune_current > 0 ? state->tune_current : state->tune_default;
    count = state->tune_count;
    if (state->psid_filename != NULL) {
        psid = lib_strdup(state->psid_filename);
    }
    vsid_state_unlock();
    nlengths = vsid_tune_info_widget_get_song_lengths(&lengths);

    ex.dialog = gtk_dialog_new();
    gtk_window_set_title(GTK_WINDOW(ex.dialog), "Export tune");
    gtk_window_set_transient_for(GTK_WINDOW(ex.dialog), ui_get_active_window());
    gtk_window_set_resizable(GTK_WINDOW(ex.dialog), FALSE);
    content = gtk_dialog_get_content_area(GTK_DIALOG(ex.dialog));

    grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_widget_set_margin_start(grid, 16);
    gtk_widget_set_margin_end(grid, 16);
    gtk_widget_set_margin_top(grid, 16);
    ex.inputs = grid;

    /* file */
    gtk_grid_attach(GTK_GRID(grid), label_left("File"), 0, row, 1, 1);
    box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    ex.entry = gtk_entry_new();
    gtk_entry_set_width_chars(GTK_ENTRY(ex.entry), 48);
    gtk_widget_set_hexpand(ex.entry, TRUE);
    if (psid != NULL) {
        gchar *base = g_strdup(psid);
        gchar *dot = strrchr(base, '.');
        gchar *sep = strrchr(base, G_DIR_SEPARATOR);
        if (dot != NULL && (sep == NULL || dot > sep)) {
            *dot = '\0';
        }
        text = g_strdup_printf("%s%s", base, formats[0].ext);
        gtk_entry_set_text(GTK_ENTRY(ex.entry), text);
        g_free(text);
        g_free(base);
        lib_free(psid);
    }
    button = gtk_button_new_with_label("Browse...");
    g_signal_connect(button, "clicked", G_CALLBACK(on_browse_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(box), ex.entry, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), button, FALSE, FALSE, 0);
    gtk_grid_attach(GTK_GRID(grid), box, 1, row++, 1, 1);

    /* format */
    gtk_grid_attach(GTK_GRID(grid), label_left("Format"), 0, row, 1, 1);
    ex.format = gtk_combo_box_text_new();
    for (i = 0; formats[i].name != NULL; i++) {
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(ex.format), formats[i].name);
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(ex.format), 0);
    g_signal_connect(ex.format, "changed", G_CALLBACK(on_format_changed), NULL);
    gtk_widget_set_halign(ex.format, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(grid), ex.format, 1, row++, 1, 1);

    /* subtunes */
    gtk_grid_attach(GTK_GRID(grid), label_left("Subtunes"), 0, row, 1, 1);
    box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    text = g_strdup_printf("Current (%d)", current);
    current_tune = gtk_radio_button_new_with_label(NULL, text);
    g_free(text);
    text = g_strdup_printf("All %d (file name gets -01, -02, ...)", count);
    ex.all_tunes = gtk_radio_button_new_with_label_from_widget(GTK_RADIO_BUTTON(current_tune), text);
    g_free(text);
    gtk_widget_set_sensitive(ex.all_tunes, count > 1);
    gtk_box_pack_start(GTK_BOX(box), current_tune, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), ex.all_tunes, FALSE, FALSE, 0);
    gtk_grid_attach(GTK_GRID(grid), box, 1, row++, 1, 1);

    /* length */
    gtk_grid_attach(GTK_GRID(grid), label_left("Length"), 0, row, 1, 1);
    box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    if (lengths != NULL && current >= 1 && current <= nlengths && lengths[current - 1] > 0) {
        long ms = lengths[current - 1];
        text = g_strdup_printf("Song length database (%ld:%02ld)", ms / 60000, (ms / 1000) % 60);
    } else {
        text = g_strdup("Song length database (not found for this tune)");
    }
    ex.use_db = gtk_radio_button_new_with_label(NULL, text);
    g_free(text);
    fixed_radio = gtk_radio_button_new_with_label_from_widget(GTK_RADIO_BUTTON(ex.use_db), "Fixed:");
    ex.fixed_seconds = gtk_spin_button_new_with_range(1, 3600, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(ex.fixed_seconds), 180);
    gtk_box_pack_start(GTK_BOX(box), ex.use_db, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), fixed_radio, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), ex.fixed_seconds, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), gtk_label_new("s"), FALSE, FALSE, 0);
    if (lengths == NULL) {
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(fixed_radio), TRUE);
    }
    gtk_grid_attach(GTK_GRID(grid), box, 1, row++, 1, 1);
    gtk_grid_attach(GTK_GRID(grid),
                    label_left("(subtunes without a database entry use the fixed length)"),
                    1, row++, 1, 1);

    /* fade-out */
    gtk_grid_attach(GTK_GRID(grid), label_left("Fade-out"), 0, row, 1, 1);
    box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    ex.fade_seconds = gtk_spin_button_new_with_range(0, 30, 0.5);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(ex.fade_seconds), 3);
    gtk_box_pack_start(GTK_BOX(box), ex.fade_seconds, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), gtk_label_new("s at the end (0 = none)"), FALSE, FALSE, 0);
    gtk_grid_attach(GTK_GRID(grid), box, 1, row++, 1, 1);

    /* speed */
    ex.fast = gtk_check_button_new_with_label(
            "Fast (warp: silent, much faster than real time)");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ex.fast), TRUE);
    gtk_grid_attach(GTK_GRID(grid), ex.fast, 1, row++, 1, 1);

    gtk_grid_attach(GTK_GRID(grid),
                    label_left("The current SID engine and filter settings apply."),
                    1, row++, 1, 1);

    gtk_box_pack_start(GTK_BOX(content), grid, FALSE, FALSE, 0);

    /* progress */
    box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_margin_start(box, 16);
    gtk_widget_set_margin_end(box, 16);
    gtk_widget_set_margin_top(box, 12);
    ex.progress = gtk_progress_bar_new();
    ex.status = label_left("");
    gtk_label_set_ellipsize(GTK_LABEL(ex.status), PANGO_ELLIPSIZE_MIDDLE);
    gtk_box_pack_start(GTK_BOX(box), ex.progress, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), ex.status, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(content), box, FALSE, FALSE, 0);

    /* buttons */
    box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(box, GTK_ALIGN_END);
    gtk_widget_set_margin_start(box, 16);
    gtk_widget_set_margin_end(box, 16);
    gtk_widget_set_margin_top(box, 12);
    gtk_widget_set_margin_bottom(box, 16);
    ex.start = gtk_button_new_with_label("Export");
    g_signal_connect(ex.start, "clicked", G_CALLBACK(on_start_clicked), NULL);
    button = gtk_button_new_with_label("Close");
    g_signal_connect(button, "clicked", G_CALLBACK(on_close_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(box), ex.start, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(content), box, FALSE, FALSE, 0);

    g_signal_connect_unlocked(ex.dialog, "destroy", G_CALLBACK(on_dialog_destroy), NULL);
    gtk_widget_show_all(ex.dialog);
}
