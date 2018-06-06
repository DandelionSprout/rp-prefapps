/*
Copyright (c) 2018 Raspberry Pi (Trading) Ltd.
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    * Neither the name of the copyright holder nor the
      names of its contributors may be used to endorse or promote products
      derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <string.h>
#include <math.h>
#include <ctype.h>
#include <stdlib.h>

#include <glib.h>
#include <glib/gi18n.h>
#include <glib/gstdio.h>
#include <gtk/gtk.h>
#include <gdk/gdkx.h>

#define I_KNOW_THE_PACKAGEKIT_GLIB2_API_IS_SUBJECT_TO_CHANGE
#include <packagekit-glib2/packagekit.h>

#include <X11/Xlib.h>

#include <libintl.h>

/* Columns in packages and categories list stores */

#define PACK_ICON           0
#define PACK_CELL_TEXT      1
#define PACK_INSTALLED      2
#define PACK_CATEGORY       3
#define PACK_PACKAGE_NAME   4
#define PACK_PACKAGE_ID     5
#define PACK_CELL_NAME      6
#define PACK_CELL_DESC      7
#define PACK_SIZE           8
#define PACK_DESCRIPTION    9
#define PACK_SUMMARY        10

#define CAT_ICON            0
#define CAT_NAME            1

/* Controls */

static GtkWidget *main_dlg, *msg_dlg, *msg_msg, *msg_pb, *msg_btn;;
static GtkWidget *cat_tv, *pack_tv;
static GtkWidget *cancel_btn, *install_btn, *info_btn;

GtkListStore *categories, *packages;
GtkTreeModel *fpackages;

guint inst, uninst;
gchar **pnames, **pinst, **puninst;

static gboolean ok_clicked (GtkButton *button, gpointer data);
static void message (char *msg, int wait, int prog);
static char *name_from_id (const gchar *id);
static void progress (PkProgress *progress, PkProgressType *type, gpointer data);
static void details_done (PkTask *task, GAsyncResult *res, gpointer data);
static void refresh_cache_done (PkTask *task, GAsyncResult *res, gpointer data);
static void resolve_done (PkTask *task, GAsyncResult *res, gpointer data);
static const char *cat_icon_name (char *category);
static gboolean read_data_file (gpointer data);
static gboolean match_category (GtkTreeModel *model, GtkTreeIter *iter, gpointer data);
static void package_selected (GtkTreeView *tv, gpointer ptr);
static void category_selected (GtkTreeView *tv, gpointer ptr);
static void install_toggled (GtkCellRendererToggle *cell, gchar *path, gpointer user_data);
static void cancel (GtkButton* btn, gpointer ptr);
static void remove_done (PkTask *task, GAsyncResult *res, gpointer data);
static void info (GtkButton* btn, gpointer ptr);
static void install_done (PkTask *task, GAsyncResult *res, gpointer data);
static void install (GtkButton* btn, gpointer ptr);


static gboolean ok_clicked (GtkButton *button, gpointer data)
{
    gtk_widget_destroy (GTK_WIDGET (msg_dlg));
    msg_dlg = NULL;
    gtk_main_quit ();
    return FALSE;
}

static void message (char *msg, int wait, int prog)
{
    if (!msg_dlg)
    {
        GtkBuilder *builder;
        GtkWidget *wid;
        GdkColor col;

        builder = gtk_builder_new ();
        gtk_builder_add_from_file (builder, PACKAGE_DATA_DIR "/rp_prefapps.ui", NULL);

        msg_dlg = (GtkWidget *) gtk_builder_get_object (builder, "msg");
        gtk_window_set_modal (GTK_WINDOW (msg_dlg), TRUE);
        gtk_window_set_transient_for (GTK_WINDOW (msg_dlg), GTK_WINDOW (main_dlg));
        gtk_window_set_position (GTK_WINDOW (msg_dlg), GTK_WIN_POS_CENTER_ON_PARENT);
        gtk_window_set_destroy_with_parent (GTK_WINDOW (msg_dlg), TRUE);
        gtk_window_set_default_size (GTK_WINDOW (msg_dlg), 340, 100);

        wid = (GtkWidget *) gtk_builder_get_object (builder, "msg_eb");
        gdk_color_parse ("#FFFFFF", &col);
        gtk_widget_modify_bg (wid, GTK_STATE_NORMAL, &col);

        msg_msg = (GtkWidget *) gtk_builder_get_object (builder, "msg_lbl");
        msg_pb = (GtkWidget *) gtk_builder_get_object (builder, "msg_pb");
        msg_btn = (GtkWidget *) gtk_builder_get_object (builder, "msg_btn");

        gtk_label_set_text (GTK_LABEL (msg_msg), msg);

        gtk_widget_show_all (msg_dlg);
        g_object_unref (builder);
    }
    else gtk_label_set_text (GTK_LABEL (msg_msg), msg);

    if (wait)
    {
        g_signal_connect (msg_btn, "clicked", G_CALLBACK (ok_clicked), NULL);
        gtk_widget_set_visible (msg_pb, FALSE);
        gtk_widget_set_visible (msg_btn, TRUE);
    }
    else
    {
        gtk_widget_set_visible (msg_btn, FALSE);
        gtk_widget_set_visible (msg_pb, TRUE);
        if (prog == -1) gtk_progress_bar_pulse (GTK_PROGRESS_BAR (msg_pb));
        else
        {
            float progress = prog / 100.0;
            gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (msg_pb), progress);
        }
    }
}

static char *name_from_id (const gchar *id)
{
    GtkTreeIter iter;
    gboolean valid;
    gchar *tid, *name;

    valid = gtk_tree_model_get_iter_first (GTK_TREE_MODEL (packages), &iter);
    while (valid)
    {
        gtk_tree_model_get (GTK_TREE_MODEL (packages), &iter, PACK_PACKAGE_ID, &tid, PACK_CELL_NAME, &name, -1);
        if (!g_strcmp0 (id, tid)) return name;
        valid = gtk_tree_model_iter_next (GTK_TREE_MODEL (packages), &iter);
    }
    return NULL;
}

static void progress (PkProgress *progress, PkProgressType *type, gpointer data)
{
    char *buf, *name;
    int role = pk_progress_get_role (progress);
    int status = pk_progress_get_status (progress);

    //printf ("progress %d %d %d %d %s\n", role, type, status, pk_progress_get_percentage (progress), pk_progress_get_package_id (progress));

    if (msg_dlg)
    {
        switch (role)
        {
            case PK_ROLE_ENUM_REFRESH_CACHE :       if (status == PK_STATUS_ENUM_LOADING_CACHE)
                                                        message (_("Updating package data - please wait..."), 0, pk_progress_get_percentage (progress));
                                                    else
                                                        gtk_progress_bar_pulse (GTK_PROGRESS_BAR (msg_pb));
                                                    break;

            case PK_ROLE_ENUM_RESOLVE :             if (status == PK_STATUS_ENUM_LOADING_CACHE)
                                                        message (_("Reading package status - please wait..."), 0, pk_progress_get_percentage (progress));
                                                    else
                                                        gtk_progress_bar_pulse (GTK_PROGRESS_BAR (msg_pb));
                                                    break;

            case PK_ROLE_ENUM_GET_DETAILS :         if (status == PK_STATUS_ENUM_LOADING_CACHE)
                                                        message (_("Reading package details - please wait..."), 0, pk_progress_get_percentage (progress));
                                                    else
                                                        gtk_progress_bar_pulse (GTK_PROGRESS_BAR (msg_pb));
                                                    break;

            case PK_ROLE_ENUM_INSTALL_PACKAGES :    if (status == PK_STATUS_ENUM_DOWNLOAD || status == PK_STATUS_ENUM_INSTALL)
                                                    {
                                                        name = name_from_id (pk_progress_get_package_id (progress));
                                                        buf = g_strdup_printf (_("%s %s - please wait..."), status == PK_STATUS_ENUM_INSTALL ? _("Installing") : _("Downloading"),
                                                            name ? name : _("packages"));
                                                        message (buf, 0, pk_progress_get_percentage (progress));
                                                    }
                                                    else
                                                        gtk_progress_bar_pulse (GTK_PROGRESS_BAR (msg_pb));
                                                    break;

            case PK_ROLE_ENUM_REMOVE_PACKAGES :     if (status == PK_STATUS_ENUM_REMOVE)
                                                    {
                                                        name = name_from_id (pk_progress_get_package_id (progress));
                                                        if (name)
                                                        {
                                                            buf = g_strdup_printf (_("Removing %s - please wait..."), name);
                                                            message (buf, 0, pk_progress_get_percentage (progress));
                                                        }
                                                        else
                                                            message (_("Removing packages - please wait..."), 0, pk_progress_get_percentage (progress));
                                                   }
                                                    else
                                                        gtk_progress_bar_pulse (GTK_PROGRESS_BAR (msg_pb));
                                                    break;
        }
    }
}

static unsigned long get_size (const char *pkg_id)
{
    char *pkg, *ver, *buf;
    pkg = g_strdup (pkg_id);
    strtok (pkg, ";");
    ver = strtok (NULL, ";");
    buf = g_strdup_printf ("apt-cache show %s=%s | grep ^Size |  cut -d ' ' -f 2", pkg, ver);
    system (buf);
    g_free (buf);
    g_free (pkg);
}

static void details_done (PkTask *task, GAsyncResult *res, gpointer data)
{
    PkResults *results;
    PkError *pkerror;
    PkDetails *item;
    GError *error = NULL;
    GPtrArray *array;
    GtkTreeIter iter;
    gboolean valid;
    gchar *buf, *name, *desc;
    const gchar *package_id;
    guint64 siz;
    float skb;
    int i, dp;

    results = pk_task_generic_finish (task, res, &error);
    if (error != NULL)
    {
        buf = g_strdup_printf (_("Error reading package details - %s"), error->message);
        message (buf, 1, -1);
        g_free (buf);
        return;
    }

    pkerror = pk_results_get_error_code (results);
    if (pkerror != NULL)
    {
        buf = g_strdup_printf (_("Error reading package details - %s"), pk_error_get_details (pkerror));
        message (buf, 1, -1);
        g_free (buf);
        return;
    }

    array = pk_results_get_details_array (results);

    for (i = 0; i < array->len; i++)
    {
        item = g_ptr_array_index (array, i);
        package_id = pk_details_get_package_id (item);

        valid = gtk_tree_model_get_iter_first (GTK_TREE_MODEL (packages), &iter);
        while (valid)
        {
            gtk_tree_model_get (GTK_TREE_MODEL (packages), &iter, PACK_PACKAGE_NAME, &buf, PACK_CELL_NAME, &name, PACK_CELL_DESC, &desc, -1);
            if (!strncmp (buf, package_id, strlen (buf)))
            {
                siz = pk_details_get_size (item);
                gtk_list_store_set (packages, &iter, PACK_SIZE, siz, -1);
                skb = siz;
                skb /= 1048576.0;
                if (skb >= 100) dp = 0;
                else if (skb >= 10) dp = 1;
                else dp = 2;

                g_free (buf);
                buf = g_strdup_printf (_("<b>%s</b>\n%s\n%s size : %.*f MB"), name, desc, strstr (package_id, ";installed:") ? _("Installed") : _("Download"), dp, skb);
                gtk_list_store_set (packages, &iter, PACK_CELL_TEXT, buf, PACK_DESCRIPTION, pk_details_get_description (item), PACK_SUMMARY, pk_details_get_summary (item), -1);
                g_free (buf);
                g_free (name);
                g_free (desc);
                break;
            }
            g_free (buf);
            g_free (name);
            g_free (desc);
            valid = gtk_tree_model_iter_next (GTK_TREE_MODEL (packages), &iter);
        }
    }

    // data now all loaded - show the main window
    gtk_tree_view_set_model (GTK_TREE_VIEW (cat_tv), GTK_TREE_MODEL (categories));
    gtk_tree_model_get_iter_first (GTK_TREE_MODEL (categories), &iter);
    gtk_tree_selection_select_iter (gtk_tree_view_get_selection (GTK_TREE_VIEW (cat_tv)), &iter);

    // set up category filter for package list
    fpackages = gtk_tree_model_filter_new (GTK_TREE_MODEL (packages), NULL);
    gtk_tree_model_filter_set_visible_func (GTK_TREE_MODEL_FILTER (fpackages), (GtkTreeModelFilterVisibleFunc) match_category, NULL, NULL);
    gtk_tree_view_set_model (GTK_TREE_VIEW (pack_tv), GTK_TREE_MODEL (fpackages));
    category_selected (NULL, NULL);

    gtk_widget_set_sensitive (cancel_btn, TRUE);
    gtk_widget_set_sensitive (install_btn, TRUE);

    gtk_widget_destroy (GTK_WIDGET (msg_dlg));
    msg_dlg = NULL;
}

static void resolve_done (PkTask *task, GAsyncResult *res, gpointer data)
{
    PkResults *results;
    PkError *pkerror;
    PkPackage *item;
    PkPackageSack *sack;
    PkInfoEnum info;
    GError *error = NULL;
    GPtrArray *array;
    GtkTreeIter iter;
    gboolean valid, inst;
    gchar *buf, *package_id, *summary;
    int i;

    results = pk_task_generic_finish (task, res, &error);
    if (error != NULL)
    {
        buf = g_strdup_printf (_("Error reading package status - %s"), error->message);
        message (buf, 1, -1);
        g_free (buf);
        return;
    }

    pkerror = pk_results_get_error_code (results);
    if (pkerror != NULL)
    {
        buf = g_strdup_printf (_("Error reading package status - %s"), pk_error_get_details (pkerror));
        message (buf, 1, -1);
        g_free (buf);
        return;
    }

    array = pk_results_get_package_array (results);

    for (i = 0; i < array->len; i++)
    {
        item = g_ptr_array_index (array, i);
        g_object_get (item, "info", &info, "package-id", &package_id, "summary", &summary, NULL);

        valid = gtk_tree_model_get_iter_first (GTK_TREE_MODEL (packages), &iter);
        while (valid)
        {
            gtk_tree_model_get (GTK_TREE_MODEL (packages), &iter, PACK_INSTALLED, &inst, PACK_PACKAGE_NAME, &buf, -1);

            // only update the package id string if no installed version of this package has already been found...
            if (!strncmp (buf, package_id, strlen (buf)) && !inst)
            {
                gtk_list_store_set (packages, &iter, PACK_PACKAGE_ID, package_id, -1);

                // never toggle installed flag from installed to uninstalled - only one version is ever installed...
                if (info == PK_INFO_ENUM_INSTALLED) gtk_list_store_set (packages, &iter, PACK_INSTALLED, TRUE, -1);
                break;
            }
            valid = gtk_tree_model_iter_next (GTK_TREE_MODEL (packages), &iter);
            g_free (buf);
        }
    }

    message (_("Reading package details - please wait..."), 0 , -1);

    sack = pk_results_get_package_sack (results);
    pk_client_get_details_async (PK_CLIENT (task), pk_package_sack_get_ids (sack), NULL, (PkProgressCallback) progress, NULL, (GAsyncReadyCallback) details_done, NULL);
}

static void refresh_cache_done (PkTask *task, GAsyncResult *res, gpointer data)
{
    PkResults *results;
    PkError *pkerror;
    GError *error = NULL;
    gchar *buf;

    results = pk_task_generic_finish (task, res, &error);
    if (error != NULL)
    {
        buf = g_strdup_printf (_("Error updating package data - %s"), error->message);
        message (buf, 1, -1);
        g_free (buf);
        return;
    }

    pkerror = pk_results_get_error_code (results);
    if (pkerror != NULL)
    {
        buf = g_strdup_printf (_("Error updating package data - %s"), pk_error_get_details (pkerror));
        message (buf, 1, -1);
        g_free (buf);
        return;
    }

    message (_("Reading package status - please wait..."), 0 , -1);

    pk_client_resolve_async (PK_CLIENT (task), 0, pnames, NULL, (PkProgressCallback) progress, NULL, (GAsyncReadyCallback) resolve_done, NULL);
}

static const char *cat_icon_name (char *category)
{
    if (!g_strcmp0 (category, _("Programming")))
        return "applications-development";

    if (!g_strcmp0 (category, _("Office")))
        return "applications-office";

    if (!g_strcmp0 (category, _("Internet")))
        return "applications-internet";

    if (!g_strcmp0 (category, _("Games")))
        return "applications-games";

    if (!g_strcmp0 (category, _("Other")))
        return "applications-other";

    if (!g_strcmp0 (category, _("Accessories")))
        return "applications-accessories";

    if (!g_strcmp0 (category, _("Sound & Video")))
        return "applications-multimedia";

    if (!g_strcmp0 (category, _("System Tools")))
        return "applications-system";

    if (!g_strcmp0 (category, _("Engineering")))
        return "applications-engineering";

    if (!g_strcmp0 (category, _("Education")))
        return "applications-science";

    if (!g_strcmp0 (category, _("Graphics")))
        return "applications-graphics";

    if (!g_strcmp0 (category, _("Science & Maths")))
        return "applications-science";

    if (!g_strcmp0 (category, _("Preferences")))
        return "preferences-desktop";

    return NULL;
}

static gboolean read_data_file (gpointer data)
{
    GtkTreeIter entry, cat_entry;
    GdkPixbuf *icon;
    GKeyFile *kf;
    PkTask *task;
    gchar **groups;
    gchar *buf, *cat, *name, *desc, *iname, *loc;
    gboolean new;
    gchar *pname;
    int pcount = 0;

    loc = setlocale (0, "");
    strtok (loc, "_. ");
    buf = g_strdup_printf ("%s/prefapps_%s.conf", PACKAGE_DATA_DIR, loc);

    kf = g_key_file_new ();
    if (g_key_file_load_from_file (kf, buf, G_KEY_FILE_NONE, NULL) ||
        g_key_file_load_from_file (kf, PACKAGE_DATA_DIR "/prefapps.conf", G_KEY_FILE_NONE, NULL))
    {
        g_free (buf);
        gtk_list_store_append (GTK_LIST_STORE (categories), &cat_entry);
        icon = gtk_icon_theme_load_icon (gtk_icon_theme_get_default (), "rpi", 32, 0, NULL);
        gtk_list_store_set (categories, &cat_entry, CAT_ICON, icon, CAT_NAME, _("All Programs"), -1);
        if (icon) g_object_unref (icon);
        groups = g_key_file_get_groups (kf, NULL);

        while (*groups)
        {
            cat = g_key_file_get_value (kf, *groups, "category", NULL);
            name = g_key_file_get_value (kf, *groups, "name", NULL);
            desc = g_key_file_get_value (kf, *groups, "description", NULL);
            iname = g_key_file_get_value (kf, *groups, "icon", NULL);

            // create array of package names
            pnames = realloc (pnames, (pcount + 2) * sizeof (gchar *));
            pnames[pcount++] = g_key_file_get_value (kf, *groups, "package", NULL);
            pnames[pcount] = NULL;

            // add unique entries to category list
            new = TRUE;
            gtk_tree_model_get_iter_first (GTK_TREE_MODEL (categories), &cat_entry);
            while (gtk_tree_model_iter_next (GTK_TREE_MODEL (categories), &cat_entry))
            {
                gtk_tree_model_get (GTK_TREE_MODEL (categories), &cat_entry, CAT_NAME, &buf, -1);
                if (!g_strcmp0 (cat, buf))
                {
                    new = FALSE;
                    g_free (buf);
                    break;
                }
                g_free (buf);
            } 

            if (new)
            {
                icon = gtk_icon_theme_load_icon (gtk_icon_theme_get_default (), cat_icon_name (cat), 32, 0, NULL);
                gtk_list_store_append (categories, &cat_entry);
                gtk_list_store_set (categories, &cat_entry, CAT_ICON, icon, CAT_NAME, cat, -1);
                if (icon) g_object_unref (icon);
            }

            // create the entry for the packages list
            icon = gtk_icon_theme_load_icon (gtk_icon_theme_get_default (), iname, 32, 0, NULL);
            if (!icon) icon = gtk_icon_theme_load_icon (gtk_icon_theme_get_default (), "application-x-executable", 32, 0, NULL);
            gtk_list_store_append (packages, &entry);
            gtk_list_store_set (packages, &entry, PACK_ICON, icon, PACK_INSTALLED, FALSE, PACK_CATEGORY, cat, PACK_PACKAGE_NAME, pnames[pcount - 1], PACK_PACKAGE_ID, "none", PACK_CELL_NAME, name, PACK_CELL_DESC, desc, -1);
            if (icon) g_object_unref (icon);

            g_free (cat);
            g_free (name);
            g_free (desc);
            g_free (iname);

            groups++;
        }
    }
    else
    {
        // handle no data file here...
        g_free (buf);
        message (_("Unable to open package data file"), 1 , -1);
        return FALSE;
    }

    message (_("Updating package data - please wait..."), 0 , -1);

    task = pk_task_new ();
    pk_client_refresh_cache_async (PK_CLIENT (task), TRUE, NULL, (PkProgressCallback) progress, NULL, (GAsyncReadyCallback) refresh_cache_done, NULL);
    return FALSE;
}

static gboolean match_category (GtkTreeModel *model, GtkTreeIter *iter, gpointer data)
{
    GtkTreeModel *cmodel;
    GtkTreeIter citer;
    GtkTreeSelection *sel;
    char *str, *cat;
    gboolean res;

    // get the current category selection from the category box
    sel = gtk_tree_view_get_selection (GTK_TREE_VIEW (cat_tv));
    if (sel && gtk_tree_selection_get_selected (sel, &cmodel, &citer))
    {
        gtk_tree_model_get (cmodel, &citer, CAT_NAME, &cat, -1);
    }
    else cat = g_strdup (_("All Programs"));

    // first make sure the package has a package ID - ignore if not
    gtk_tree_model_get (model, iter, PACK_PACKAGE_ID, &str, -1);
    if (!g_strcmp0 (str, "none")) res = FALSE;
    else
    {
        // check that category matches
        if (!g_strcmp0 (cat, _("All Programs"))) res = TRUE;
        else
        {
            g_free (str);
            gtk_tree_model_get (model, iter, PACK_CATEGORY, &str, -1);
            if (!g_strcmp0 (str, cat)) res = TRUE;
            else res = FALSE;
        }
    }
    g_free (str);
    g_free (cat);
    return res;
}

static void package_selected (GtkTreeView *tv, gpointer ptr)
{
    GtkTreeModel *model;
    GtkTreeSelection *sel;
    GList *rows;

    model = gtk_tree_view_get_model (GTK_TREE_VIEW (pack_tv));
    sel = gtk_tree_view_get_selection (GTK_TREE_VIEW (pack_tv));
    rows = gtk_tree_selection_get_selected_rows (sel, &model);
    if (rows && rows->data) gtk_widget_set_sensitive (info_btn, TRUE);
    else gtk_widget_set_sensitive (info_btn, FALSE);
}

static void category_selected (GtkTreeView *tv, gpointer ptr)
{
    gtk_tree_model_filter_refilter (GTK_TREE_MODEL_FILTER (fpackages));
    package_selected (NULL, NULL);
}

static void install_toggled (GtkCellRendererToggle *cell, gchar *path, gpointer user_data)
{
    GtkTreeIter iter, citer;
    GtkTreeModel *model, *cmodel;
    gboolean val;
    gchar *name, *desc, *id, *buf, *state;
    guint64 siz;
    float skb;
    int dp;

    model = gtk_tree_view_get_model (GTK_TREE_VIEW (pack_tv));
    gtk_tree_model_get_iter_from_string (model, &iter, path);

    cmodel = gtk_tree_model_filter_get_model (GTK_TREE_MODEL_FILTER (model));
    gtk_tree_model_filter_convert_iter_to_child_iter (GTK_TREE_MODEL_FILTER (model), &citer, &iter);

    gtk_tree_model_get (cmodel, &citer, PACK_INSTALLED, &val, PACK_CELL_NAME, &name, PACK_CELL_DESC, &desc, PACK_SIZE, &siz, PACK_PACKAGE_ID, &id, -1);

    if (!strstr (id, ";installed:") && !val) state = g_strdup ("   <b>Will be installed</b>");
    else if (strstr (id, ";installed:") && val) state = g_strdup ("   <b>Will be removed</b>");
    else state = g_strdup ("");

    skb = siz;
    skb /= 1048576.0;
    if (skb >= 100) dp = 0;
    else if (skb >= 10) dp = 1;
    else dp = 2;

    buf = g_strdup_printf (_("<b>%s</b>\n%s\n%s size : %.*f MB%s"), name, desc, strstr (id, ";installed:") ? _("Installed") : _("Download"), dp, skb, state);
    gtk_list_store_set (GTK_LIST_STORE (cmodel), &citer, PACK_INSTALLED, 1 - val, PACK_CELL_TEXT, buf, -1);
    g_free (buf);
    g_free (state);
    g_free (name);
    g_free (desc);
    g_free (id);
}

static void cancel (GtkButton* btn, gpointer ptr)
{
    gtk_main_quit ();
}

static void remove_done (PkTask *task, GAsyncResult *res, gpointer data)
{
    PkResults *results;
    PkError *pkerror;
    GError *error = NULL;
    gchar *buf;

    results = pk_task_generic_finish (task, res, &error);
    if (error != NULL)
    {
        buf = g_strdup_printf (_("Error removing packages - %s"), error->message);
        message (buf, 1, -1);
        g_free (buf);
        return;
    }

    pkerror = pk_results_get_error_code (results);
    if (pkerror != NULL)
    {
        buf = g_strdup_printf (_("Error removing packages - %s"), pk_error_get_details (pkerror));
        message (buf, 1, -1);
        g_free (buf);
        return;
    }

    if (inst)
        message (_("Installation and removal complete"), 1, -1);
    else
        message (_("Removal complete"), 1, -1);
}

static void info (GtkButton* btn, gpointer ptr)
{
    GtkTreeIter iter, citer;
    GtkTreeModel *model, *cmodel;
    GtkTreeSelection *sel;
    GList *rows;
    GtkWidget *dlg, *img;
    GdkPixbuf *pix;
    gchar *sum = NULL, *desc = NULL, *name = NULL;

    model = gtk_tree_view_get_model (GTK_TREE_VIEW (pack_tv));
    sel = gtk_tree_view_get_selection (GTK_TREE_VIEW (pack_tv));
    rows = gtk_tree_selection_get_selected_rows (sel, &model);
    if (rows && rows->data)
    {
        gtk_tree_model_get_iter (model, &iter, (GtkTreePath *) rows->data);

        cmodel = gtk_tree_model_filter_get_model (GTK_TREE_MODEL_FILTER (model));
        gtk_tree_model_filter_convert_iter_to_child_iter (GTK_TREE_MODEL_FILTER (model), &citer, &iter);

        gtk_tree_model_get (cmodel, &citer, PACK_CELL_NAME, &name, PACK_DESCRIPTION, &desc, PACK_SUMMARY, &sum, PACK_ICON, &pix, -1);
        if (!desc) desc = g_strdup (_("No additional information available for this package."));
        dlg = gtk_message_dialog_new (GTK_WINDOW (main_dlg), GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT, GTK_MESSAGE_OTHER, GTK_BUTTONS_OK, "%s\n\n%s", sum, desc);
        img = gtk_image_new_from_pixbuf (pix);
        gtk_message_dialog_set_image (GTK_MESSAGE_DIALOG (dlg), img);
        gtk_window_set_title (GTK_WINDOW (dlg), name);
        gtk_window_set_type_hint (GTK_WINDOW (dlg), GDK_WINDOW_TYPE_HINT_MENU);
        gtk_widget_show_all (dlg);
        gtk_dialog_run (GTK_DIALOG (dlg));
        gtk_widget_destroy (dlg);
        g_free (sum);
        g_free (desc);
        g_free (name);
    }
}

static void install_done (PkTask *task, GAsyncResult *res, gpointer data)
{
    PkResults *results;
    PkError *pkerror;
    GError *error = NULL;
    gchar *buf;

    results = pk_task_generic_finish (task, res, &error);
    if (error != NULL)
    {
        buf = g_strdup_printf (_("Error installing packages - %s"), error->message);
        message (buf, 1, -1);
        g_free (buf);
        return;
    }

    pkerror = pk_results_get_error_code (results);
    if (pkerror != NULL)
    {
        buf = g_strdup_printf (_("Error installing packages - %s"), pk_error_get_details (pkerror));
        message (buf, 1, -1);
        g_free (buf);
        return;
    }

    if (uninst)
    {
        message (_("Removing packages - please wait..."), 0 , -1);

        pk_task_remove_packages_async (task, puninst, TRUE, TRUE, NULL, (PkProgressCallback) progress, NULL, (GAsyncReadyCallback) remove_done, NULL);
    }
    else message (_("Installation complete"), 1, -1);
}

static void install (GtkButton* btn, gpointer ptr)
{
    PkTask *task;
    GtkTreeIter iter;
    gboolean valid, state;
    gchar *id;

    gtk_widget_set_sensitive (info_btn, FALSE);
    gtk_widget_set_sensitive (cancel_btn, FALSE);
    gtk_widget_set_sensitive (install_btn, FALSE);

    inst = 0;
    uninst = 0;
    pinst = malloc (sizeof (gchar *));
    pinst[inst] = NULL;
    pinst = malloc (sizeof (gchar *));
    pinst[uninst] = NULL;

    valid = gtk_tree_model_get_iter_first (GTK_TREE_MODEL (packages), &iter);
    while (valid)
    {
        gtk_tree_model_get (GTK_TREE_MODEL (packages), &iter, PACK_INSTALLED, &state, PACK_PACKAGE_ID, &id, -1);
        if (!strstr (id, ";installed:"))
        {
            if (state)
            {
                // needs install
                pinst = realloc (pinst, (inst + 2) * sizeof (gchar *));
                pinst[inst++] = g_strdup (id);
                pinst[inst] = NULL;
            }
        }
        else
        {
            if (!state)
            {
                // needs uninstall
                puninst = realloc (puninst, (uninst + 2) * sizeof (gchar *));
                puninst[uninst++] = g_strdup (id);
                puninst[uninst] = NULL;
            }
        }
        g_free (id);
        valid = gtk_tree_model_iter_next (GTK_TREE_MODEL (packages), &iter);
    }

    if (inst)
    {
        message (_("Installing packages - please wait..."), 0 , -1);

        task = pk_task_new ();
        pk_task_install_packages_async (task, pinst, NULL, (PkProgressCallback) progress, NULL, (GAsyncReadyCallback) install_done, NULL);
    }
    else if (uninst)
    {
        message (_("Removing packages - please wait..."), 0 , -1);

        task = pk_task_new ();
        pk_task_remove_packages_async (task, puninst, TRUE, TRUE, NULL, (PkProgressCallback) progress, NULL, (GAsyncReadyCallback) remove_done, NULL);
    }
    else gtk_main_quit ();
}

/* The dialog... */

int main (int argc, char *argv[])
{
    GtkBuilder *builder;
    GtkWidget *wid;
    GtkCellRenderer *crp, *crt, *crb;
    int res;

#ifdef ENABLE_NLS
    setlocale (LC_ALL, "");
    bindtextdomain ( GETTEXT_PACKAGE, PACKAGE_LOCALE_DIR );
    bind_textdomain_codeset ( GETTEXT_PACKAGE, "UTF-8" );
    textdomain ( GETTEXT_PACKAGE );
#endif

    // GTK setup
    gdk_threads_init ();
    gdk_threads_enter ();
    gtk_init (&argc, &argv);
    gtk_icon_theme_prepend_search_path (gtk_icon_theme_get_default(), PACKAGE_DATA_DIR);

    // build the UI
    builder = gtk_builder_new ();
    gtk_builder_add_from_file (builder, PACKAGE_DATA_DIR "/rp_prefapps.ui", NULL);

    main_dlg = (GtkWidget *) gtk_builder_get_object (builder, "main_window");
    cat_tv = (GtkWidget *) gtk_builder_get_object (builder, "treeview_cat");
    pack_tv = (GtkWidget *) gtk_builder_get_object (builder, "treeview_prog");
    info_btn = (GtkWidget *) gtk_builder_get_object (builder, "button_info");
    cancel_btn = (GtkWidget *) gtk_builder_get_object (builder, "button_cancel");
    install_btn = (GtkWidget *) gtk_builder_get_object (builder, "button_ok");

    // create list stores
    categories = gtk_list_store_new (2, GDK_TYPE_PIXBUF, G_TYPE_STRING);
    packages = gtk_list_store_new (11, GDK_TYPE_PIXBUF, G_TYPE_STRING, G_TYPE_BOOLEAN, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_UINT64, G_TYPE_STRING, G_TYPE_STRING);

    // set up tree views
    crp = gtk_cell_renderer_pixbuf_new ();
    crt = gtk_cell_renderer_text_new ();
    crb = gtk_cell_renderer_toggle_new ();

    gtk_tree_view_insert_column_with_attributes (GTK_TREE_VIEW (cat_tv), 0, "Icon", crp, "pixbuf", CAT_ICON, NULL);
    gtk_tree_view_insert_column_with_attributes (GTK_TREE_VIEW (cat_tv), 1, "Category", crt, "text", CAT_NAME, NULL);

    gtk_widget_set_size_request (cat_tv, 160, -1);
    gtk_tree_view_set_headers_visible (GTK_TREE_VIEW (cat_tv), FALSE);

    gtk_tree_view_insert_column_with_attributes (GTK_TREE_VIEW (pack_tv), 0, "", crp, "pixbuf", PACK_ICON, NULL);
    gtk_tree_view_insert_column_with_attributes (GTK_TREE_VIEW (pack_tv), 1, "Description", crt, "markup", PACK_CELL_TEXT, NULL);
    gtk_tree_view_insert_column_with_attributes (GTK_TREE_VIEW (pack_tv), 2, "Install", crb, "active", PACK_INSTALLED, NULL);

    gtk_tree_view_column_set_sizing (gtk_tree_view_get_column (GTK_TREE_VIEW (pack_tv), 0), GTK_TREE_VIEW_COLUMN_FIXED);
    gtk_tree_view_column_set_fixed_width (gtk_tree_view_get_column (GTK_TREE_VIEW (pack_tv), 0), 45);
    gtk_tree_view_column_set_expand (gtk_tree_view_get_column (GTK_TREE_VIEW (pack_tv), 1), TRUE);
    g_object_set (crt, "wrap-mode", PANGO_WRAP_WORD, "wrap-width", 320, NULL);
    gtk_tree_view_column_set_sizing (gtk_tree_view_get_column (GTK_TREE_VIEW (pack_tv), 2), GTK_TREE_VIEW_COLUMN_FIXED);
    gtk_tree_view_column_set_fixed_width (gtk_tree_view_get_column (GTK_TREE_VIEW (pack_tv), 2), 45);

    g_signal_connect (cat_tv, "cursor-changed", G_CALLBACK (category_selected), NULL);
    g_signal_connect (crb, "toggled", G_CALLBACK (install_toggled), NULL);
    g_signal_connect (cancel_btn, "clicked", G_CALLBACK (cancel), NULL);
    g_signal_connect (install_btn, "clicked", G_CALLBACK (install), NULL);
    g_signal_connect (info_btn, "clicked", G_CALLBACK (info), NULL);
    g_signal_connect (main_dlg, "delete_event", G_CALLBACK (cancel), NULL);
    g_signal_connect (pack_tv, "cursor-changed", G_CALLBACK (package_selected), NULL);

    gtk_widget_set_sensitive (info_btn, FALSE);
    gtk_widget_set_sensitive (cancel_btn, FALSE);
    gtk_widget_set_sensitive (install_btn, FALSE);

    gtk_window_set_default_size (GTK_WINDOW (main_dlg), 640, 400);
    gtk_widget_show_all (main_dlg);

    // load the data file and check with backend
    g_idle_add (read_data_file, NULL);
    gtk_main ();

    g_object_unref (builder);
    gtk_widget_destroy (main_dlg);
    gdk_threads_leave ();
    return 0;
}


