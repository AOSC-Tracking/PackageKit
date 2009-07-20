/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2007-2008 Richard Hughes <richard@hughsie.com>
 *
 * Licensed under the GNU General Public License Version 2
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#include "config.h"

#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <locale.h>
#include <glib.h>
#include <glib/gi18n.h>
#include <dbus/dbus-glib.h>
#include <packagekit-glib/packagekit.h>

#include "egg-debug.h"

#include "pk-tools-common.h"

static PkControl *control = NULL;
static gboolean verbose = FALSE;

/**
 * pk_monitor_task_list_changed_cb:
 **/
static void
pk_monitor_task_list_changed_cb (PkTaskList *tlist, gpointer data)
{
	guint i;
	PkTaskListItem *item;
	guint length;
	gchar *state;

	length = pk_task_list_get_size (tlist);
	g_print ("Tasks:\n");
	if (length == 0) {
		g_print ("[none]\n");
		return;
	}
	for (i=0; i<length; i++) {
		item = pk_task_list_get_item (tlist, i);
		g_print ("#%i\t%s\t%s (%s)\t%s\n", i+1, item->tid, pk_role_enum_to_text (item->role),
			 pk_status_enum_to_text (item->status), item->text);
	}

	/* only print state when verbose */
	if (verbose) {
		state = pk_control_get_daemon_state (control, NULL);
		g_print ("%s", state);
		g_free (state);
	}
}

/**
 * pk_monitor_repo_list_changed_cb:
 **/
static void
pk_monitor_repo_list_changed_cb (PkControl *_control, gpointer data)
{
	g_print ("repo-list-changed\n");
}

/**
 * pk_monitor_updates_changed_cb:
 **/
static void
pk_monitor_updates_changed_cb (PkControl *_control, gpointer data)
{
	g_print ("updates-changed\n");
}

/**
 * pk_connection_changed_cb:
 **/
static void
pk_connection_changed_cb (PkConnection *pconnection, gboolean connected, gpointer data)
{
	egg_debug ("connected=%i", connected);
}

/**
 * pk_monitor_locked_cb:
 **/
static void
pk_monitor_locked_cb (PkControl *_control, gboolean is_locked, gpointer data)
{
	if (is_locked)
		g_print ("locked\n");
	else
		g_print ("unlocked\n");
}

/**
 * main:
 **/
int
main (int argc, char *argv[])
{
	PkTaskList *tlist;
	gboolean ret;
	GMainLoop *loop;
	PkConnection *pconnection;
	gboolean connected;
	gboolean program_version = FALSE;
	gchar *state;
	GOptionContext *context;
	gint retval = PK_EXIT_CODE_SUCCESS;

	const GOptionEntry options[] = {
		{ "verbose", 'v', 0, G_OPTION_ARG_NONE, &verbose,
			_("Show extra debugging information"), NULL },
		{ "version", '\0', 0, G_OPTION_ARG_NONE, &program_version,
			_("Show the program version and exit"), NULL},
		{ NULL}
	};

	setlocale (LC_ALL, "");
	bindtextdomain (GETTEXT_PACKAGE, PACKAGE_LOCALE_DIR);
	bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
	textdomain (GETTEXT_PACKAGE);

	if (! g_thread_supported ())
		g_thread_init (NULL);
	dbus_g_thread_init ();
	g_type_init ();

	context = g_option_context_new (NULL);
	/* TRANSLATORS: this is a program that monitors PackageKit */
	g_option_context_set_summary (context, _("PackageKit Monitor"));
	g_option_context_add_main_entries (context, options, NULL);
	g_option_context_parse (context, &argc, &argv, NULL);
	g_option_context_free (context);

	if (program_version) {
		g_print (VERSION "\n");
		goto out;
	}

	egg_debug_init (verbose);

	loop = g_main_loop_new (NULL, FALSE);

	pconnection = pk_connection_new ();
	g_signal_connect (pconnection, "connection-changed",
			  G_CALLBACK (pk_connection_changed_cb), loop);
	connected = pk_connection_valid (pconnection);
	egg_debug ("connected=%i", connected);

	control = pk_control_new ();
	g_signal_connect (control, "locked",
			  G_CALLBACK (pk_monitor_locked_cb), NULL);
	g_signal_connect (control, "repo-list-changed",
			  G_CALLBACK (pk_monitor_repo_list_changed_cb), NULL);
	g_signal_connect (control, "updates-changed",
			  G_CALLBACK (pk_monitor_updates_changed_cb), NULL);

	tlist = pk_task_list_new ();
	g_signal_connect (tlist, "changed",
			  G_CALLBACK (pk_monitor_task_list_changed_cb), NULL);
	g_signal_connect (tlist, "status-changed",
			  G_CALLBACK (pk_monitor_task_list_changed_cb), NULL);

	egg_debug ("refreshing task list");
	ret = pk_task_list_refresh (tlist);
	if (!ret) {
		g_print ("%s\n", _("Cannot show the list of transactions"));
		retval = PK_EXIT_CODE_FAILED;
		goto out;
	}
	pk_task_list_print (tlist);

	/* only print state when verbose */
	if (verbose) {
		state = pk_control_get_daemon_state (control, NULL);
		g_print ("%s\n", state);
		g_free (state);
	}

	/* spin */
	g_main_loop_run (loop);

	g_object_unref (control);
	g_object_unref (tlist);
	g_object_unref (pconnection);
out:
	return retval;
}
