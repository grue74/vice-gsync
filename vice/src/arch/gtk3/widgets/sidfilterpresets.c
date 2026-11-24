/** \file   sidfilterpresets.c
 * \brief   Saved SID filter settings ("My settings")
 *
 * Every filter-related resource, saved under a name to sidfilter-presets.txt
 * in the user configuration directory, one per line:
 *     name<TAB>Resource=value;Resource=value;...
 * The SID engine and model are not part of a preset. Used by the VSID SID
 * panel and the Settings > Audio > SID page; both see the same presets.
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "archdep.h"
#include "lib.h"
#include "log.h"
#include "resources.h"
#include "util.h"
#include "vice_gtk3.h"

#include "sidfilterpresets.h"


/** \brief  Resources stored in a preset (those not in this build are skipped) */
static const char *preset_resources[] = {
    "SidFilters",
    "SidResidPassband", "SidResidGain", "SidResidFilterBias",
    "SidResid8580Passband", "SidResid8580Gain", "SidResid8580FilterBias",
    "SidResid6581FilterCurve", "SidResid6581FilterRange", "SidResid8580FilterCurve",
    "SidResid6581OldCaps", "SidResidCombinedWaveformStrength",
    "SidResidFilterC1", "SidResidFilterC2", "SidResid6581DacRatio",
    "SidResid6581Snake", "SidResidResonanceStrength", "SidResid6581OpampGain",
    "SidResid6581ResonanceQ",
    NULL
};

#define MAX_PRESETS 64

static char *preset_names[MAX_PRESETS];
static char *preset_values[MAX_PRESETS];
static int preset_count = 0;

/** \brief  Name of the preset loaded or saved last (shown selected) */
static char *preset_current = NULL;

/** \brief  Path of the preset file (lib_free() it) */
static char *preset_path(void)
{
    return util_join_paths(archdep_user_config_path(), "sidfilter-presets.txt", NULL);
}

static void presets_clear(void)
{
    int i;

    for (i = 0; i < preset_count; i++) {
        lib_free(preset_names[i]);
        lib_free(preset_values[i]);
    }
    preset_count = 0;
}

/** \brief  Read the preset file */
static void presets_read(void)
{
    char *path = preset_path();
    FILE *fp;
    char line[2048];

    presets_clear();
    fp = fopen(path, "r");
    lib_free(path);
    if (fp == NULL) {
        return;
    }
    while (preset_count < MAX_PRESETS && fgets(line, sizeof line, fp) != NULL) {
        char *tab = strchr(line, '\t');
        size_t len = strlen(line);

        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[--len] = '\0';
        }
        if (tab == NULL || tab == line) {
            continue;
        }
        *tab = '\0';
        preset_names[preset_count] = lib_strdup(line);
        preset_values[preset_count] = lib_strdup(tab + 1);
        preset_count++;
    }
    fclose(fp);
}

/** \brief  Write the preset file
 *
 * \return  TRUE on success
 */
static gboolean presets_write(void)
{
    char *path = preset_path();
    FILE *fp = fopen(path, "w");
    int i;

    if (fp == NULL) {
        log_error(LOG_DEFAULT, "Cannot write SID filter presets to %s", path);
        lib_free(path);
        return FALSE;
    }
    lib_free(path);
    for (i = 0; i < preset_count; i++) {
        fprintf(fp, "%s\t%s\n", preset_names[i], preset_values[i]);
    }
    fclose(fp);
    return TRUE;
}

/** \brief  Current values of the preset resources as "Res=val;Res=val" */
static char *presets_capture(void)
{
    GString *str = g_string_new(NULL);
    char *result;
    int i;

    for (i = 0; preset_resources[i] != NULL; i++) {
        int value;

        if (resources_query_type(preset_resources[i]) != RES_INTEGER) {
            continue;
        }
        if (resources_get_int(preset_resources[i], &value) == 0) {
            g_string_append_printf(str, "%s%s=%d", str->len ? ";" : "", preset_resources[i], value);
        }
    }
    result = lib_strdup(str->str);
    g_string_free(str, TRUE);
    return result;
}

/** \brief  Set the resources of a preset ("Res=val;Res=val")
 *
 * Resources the preset doesn't name (saved before they existed) are set to
 * their defaults, so a preset always gives the same sound.
 */
static void presets_apply(const char *values)
{
    gchar **pairs = g_strsplit(values, ";", -1);
    int i;

    for (i = 0; preset_resources[i] != NULL; i++) {
        size_t len = strlen(preset_resources[i]);
        const char *p = values;
        int named = 0;
        int def;

        while (p != NULL && *p != '\0') {
            if (strncmp(p, preset_resources[i], len) == 0 && p[len] == '=') {
                named = 1;
                break;
            }
            p = strchr(p, ';');
            if (p != NULL) {
                p++;
            }
        }
        if (!named && resources_query_type(preset_resources[i]) == RES_INTEGER
                && resources_get_default_value(preset_resources[i], &def) == 0) {
            resources_set_int(preset_resources[i], def);
        }
    }

    for (i = 0; pairs[i] != NULL; i++) {
        gchar *eq = strchr(pairs[i], '=');
        int k;

        if (eq == NULL) {
            continue;
        }
        *eq = '\0';
        /* only the resources a preset may set */
        for (k = 0; preset_resources[k] != NULL; k++) {
            if (strcmp(preset_resources[k], pairs[i]) == 0) {
                if (resources_query_type(pairs[i]) == RES_INTEGER) {
                    resources_set_int(pairs[i], atoi(eq + 1));
                }
                break;
            }
        }
    }
    g_strfreev(pairs);
}

static int presets_find(const char *name)
{
    int i;

    for (i = 0; i < preset_count; i++) {
        if (strcmp(preset_names[i], name) == 0) {
            return i;
        }
    }
    return -1;
}

static void set_current_preset(const char *name)
{
    if (preset_current != NULL) {
        lib_free(preset_current);
    }
    preset_current = (name != NULL) ? lib_strdup(name) : NULL;
}


/* ---- widget -------------------------------------------------------------- */

typedef void (*loaded_cb_t)(void);

static GtkWidget *get_child(GtkWidget *grid, const char *key)
{
    return GTK_WIDGET(g_object_get_data(G_OBJECT(grid), key));
}

static void set_status(GtkWidget *grid, const char *text)
{
    gtk_label_set_text(GTK_LABEL(get_child(grid, "status")), text);
}

static void on_preset_changed(GtkComboBox *combo, gpointer grid);

/** \brief  Fill the list from the preset file, selecting the current one */
static void refill_combo(GtkWidget *grid)
{
    GtkWidget *combo = get_child(grid, "combo");
    gulong handler = GPOINTER_TO_SIZE(g_object_get_data(G_OBJECT(grid), "handler"));
    char id[16];
    int i;

    presets_read();
    g_signal_handler_block(combo, handler);
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(combo));
    for (i = 0; i < preset_count; i++) {
        g_snprintf(id, sizeof id, "%d", i);
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(combo), id, preset_names[i]);
        if (preset_current != NULL && strcmp(preset_current, preset_names[i]) == 0) {
            gtk_combo_box_set_active_id(GTK_COMBO_BOX(combo), id);
        }
    }
    g_signal_handler_unblock(combo, handler);
    gtk_widget_set_tooltip_text(combo, preset_count > 0
            ? "Choose saved filter settings to load them"
            : "No saved filter settings yet: type a name below and press Save");
}

/** \brief  A preset was chosen in the list: load it */
static void on_preset_changed(GtkComboBox *combo, gpointer grid)
{
    gchar *name = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(combo));
    loaded_cb_t cb;
    int i;

    if (name == NULL) {
        return;
    }
    presets_read();
    i = presets_find(name);
    if (i >= 0) {
        presets_apply(preset_values[i]);
        set_current_preset(name);
        gtk_entry_set_text(GTK_ENTRY(get_child(grid, "entry")), name);
        set_status(grid, "Loaded.");
        cb = (loaded_cb_t)g_object_get_data(G_OBJECT(grid), "loaded");
        if (cb != NULL) {
            cb();
        }
    }
    g_free(name);
}

/** \brief  Save the current settings under the name in the entry */
static void on_preset_save(GtkWidget *widget, gpointer grid)
{
    gchar *name = g_strstrip(g_strdup(gtk_entry_get_text(GTK_ENTRY(get_child(grid, "entry")))));
    char *values;
    char *p;
    int i;

    /* a name is one line without tabs */
    for (p = name; *p != '\0'; p++) {
        if (*p == '\t' || *p == '\n' || *p == '\r') {
            *p = ' ';
        }
    }
    presets_read();
    if (*name == '\0') {
        g_free(name);
        name = g_strdup_printf("My filter %d", preset_count + 1);
    }

    values = presets_capture();
    i = presets_find(name);
    if (i >= 0) {
        lib_free(preset_values[i]);
        preset_values[i] = values;
    } else if (preset_count < MAX_PRESETS) {
        preset_names[preset_count] = lib_strdup(name);
        preset_values[preset_count] = values;
        preset_count++;
    } else {
        lib_free(values);
        set_status(grid, "No room for more presets; delete one first.");
        g_free(name);
        return;
    }
    if (presets_write()) {
        set_current_preset(name);
        gtk_entry_set_text(GTK_ENTRY(get_child(grid, "entry")), name);
        refill_combo(grid);
        set_status(grid, "Saved.");
    } else {
        set_status(grid, "Could not write the preset file (see the log).");
    }
    g_free(name);
}

/** \brief  Delete the preset selected in the list */
static void on_preset_delete(GtkWidget *button, gpointer grid)
{
    gchar *name = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(get_child(grid, "combo")));
    int i;

    if (name == NULL) {
        set_status(grid, "Choose a preset to delete.");
        return;
    }
    presets_read();
    i = presets_find(name);
    if (i >= 0) {
        lib_free(preset_names[i]);
        lib_free(preset_values[i]);
        for (; i < preset_count - 1; i++) {
            preset_names[i] = preset_names[i + 1];
            preset_values[i] = preset_values[i + 1];
        }
        preset_count--;
        presets_write();
    }
    set_current_preset(NULL);
    refill_combo(grid);
    set_status(grid, "Deleted.");
    g_free(name);
}

GtkWidget *sid_filter_presets_widget_create(void (*on_loaded)(void))
{
    GtkWidget *grid;
    GtkWidget *combo;
    GtkWidget *entry;
    GtkWidget *button;
    GtkWidget *status;
    gulong handler;

    grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 4);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 2);
    gtk_widget_set_hexpand(grid, TRUE);
    g_object_set_data(G_OBJECT(grid), "loaded", (gpointer)on_loaded);

    combo = gtk_combo_box_text_new();
    gtk_widget_set_hexpand(combo, TRUE);
    g_object_set_data(G_OBJECT(grid), "combo", combo);
    button = gtk_button_new_with_label("Delete");
    gtk_widget_set_tooltip_text(button, "Delete the chosen settings");
    g_signal_connect(button, "clicked", G_CALLBACK(on_preset_delete), grid);
    gtk_grid_attach(GTK_GRID(grid), combo, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), button, 1, 0, 1, 1);

    entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry), "Name for the current settings");
    gtk_widget_set_hexpand(entry, TRUE);
    if (preset_current != NULL) {
        gtk_entry_set_text(GTK_ENTRY(entry), preset_current);
    }
    g_object_set_data(G_OBJECT(grid), "entry", entry);
    button = gtk_button_new_with_label("Save");
    gtk_widget_set_tooltip_text(button,
            "Save all filter settings (sliders, caps, DAC kink, snake, resonance,"
            " filter on/off) under this name. The same name overwrites.");
    g_signal_connect(button, "clicked", G_CALLBACK(on_preset_save), grid);
    g_signal_connect(entry, "activate", G_CALLBACK(on_preset_save), grid);
    gtk_grid_attach(GTK_GRID(grid), entry, 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), button, 1, 1, 1, 1);

    status = gtk_label_new(NULL);
    gtk_widget_set_halign(status, GTK_ALIGN_START);
    g_object_set_data(G_OBJECT(grid), "status", status);
    gtk_grid_attach(GTK_GRID(grid), status, 0, 2, 2, 1);

    handler = g_signal_connect(combo, "changed", G_CALLBACK(on_preset_changed), grid);
    g_object_set_data(G_OBJECT(grid), "handler", GSIZE_TO_POINTER(handler));
    refill_combo(grid);

    gtk_widget_show_all(grid);
    return grid;
}
