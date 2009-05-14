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

#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <errno.h>

#include <string.h>
#include <sys/time.h>
#include <sys/types.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif /* HAVE_UNISTD_H */

#include <glib/gi18n.h>
#include <glib/gstdio.h>
#include <dbus/dbus-glib.h>
#include <dbus/dbus-glib-lowlevel.h>
#include <packagekit-glib/packagekit.h>

#include "egg-debug.h"
#include "egg-string.h"

#include "pk-network.h"
#include "pk-cache.h"
#include "pk-shared.h"
#include "pk-update-detail-list.h"
#include "pk-backend.h"
#include "pk-backend-internal.h"
#include "pk-engine.h"
#include "pk-transaction.h"
#include "pk-transaction-db.h"
#include "pk-transaction-list.h"
#include "pk-inhibit.h"
#include "pk-marshal.h"
#include "pk-notify.h"
#include "pk-file-monitor.h"
#include "pk-security.h"
#include "pk-conf.h"

static void     pk_engine_finalize	(GObject       *object);

#define PK_ENGINE_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), PK_TYPE_ENGINE, PkEnginePrivate))

/**
 * PK_ENGINE_STATE_CHANGED_PRIORITY_TIMEOUT:
 *
 * The timeout in seconds to wait when we get the StateHasChanged method.
 * We don't queue these transactions if one is already in progress.
 *
 * This should be used when a native tool has been used, and the update UI should
 * be updated to reflect reality.
 */
#define PK_ENGINE_STATE_CHANGED_PRIORITY_TIMEOUT		5 /* seconds */

/**
 * PK_ENGINE_STATE_CHANGED_NORMAL_TIMEOUT:
 *
 * The timeout in seconds to wait when we get the StateHasChanged method (for selected reasons).
 * We don't queue these transactions if one is already in progress.
 *
 * We probably don't want to be doing an update check at the busy time after a resume, or for
 * other non-critical reasons.
 */
#define PK_ENGINE_STATE_CHANGED_NORMAL_TIMEOUT		10*60 /* seconds */

struct PkEnginePrivate
{
	GTimer			*timer;
	gboolean		 notify_clients_of_upgrade;
	gboolean		 shutdown_as_soon_as_possible;
	PkTransactionList	*transaction_list;
	PkTransactionDb		*transaction_db;
	PkCache			*cache;
	PkUpdateDetailList	*update_detail_cache;
	PkBackend		*backend;
	PkInhibit		*inhibit;
	PkNetwork		*network;
	PkSecurity		*security;
	PkNotify		*notify;
	PkConf			*conf;
	PkFileMonitor		*file_monitor_conf;
	PkFileMonitor		*file_monitor_binary;
	PkBitfield		 actions;
	PkBitfield		 groups;
	PkBitfield		 filters;
	gchar			*mime_types;
	guint			 signal_state_priority_timeout;
	guint			 signal_state_normal_timeout;
};

enum {
	PK_ENGINE_LOCKED,
	PK_ENGINE_TRANSACTION_LIST_CHANGED,
	PK_ENGINE_REPO_LIST_CHANGED,
	PK_ENGINE_NETWORK_STATE_CHANGED,
	PK_ENGINE_RESTART_SCHEDULE,
	PK_ENGINE_UPDATES_CHANGED,
	PK_ENGINE_QUIT,
	PK_ENGINE_LAST_SIGNAL
};

static guint	     signals [PK_ENGINE_LAST_SIGNAL] = { 0 };

G_DEFINE_TYPE (PkEngine, pk_engine, G_TYPE_OBJECT)

/* prototype */
gboolean pk_engine_filter_check (const gchar *filter, GError **error);

/**
 * pk_engine_error_quark:
 * Return value: Our personal error quark.
 **/
GQuark
pk_engine_error_quark (void)
{
	static GQuark quark = 0;
	if (!quark)
		quark = g_quark_from_static_string ("pk_engine_error");
	return quark;
}

/**
 * pk_engine_error_get_type:
 **/
#define ENUM_ENTRY(NAME, DESC) { NAME, "" #NAME "", DESC }
GType
pk_engine_error_get_type (void)
{
	static GType etype = 0;

	if (etype == 0) {
		static const GEnumValue values[] =
		{
			ENUM_ENTRY (PK_ENGINE_ERROR_INVALID_STATE, "InvalidState"),
			ENUM_ENTRY (PK_ENGINE_ERROR_REFUSED_BY_POLICY, "RefusedByPolicy"),
			ENUM_ENTRY (PK_ENGINE_ERROR_CANNOT_SET_PROXY, "CannotSetProxy"),
			ENUM_ENTRY (PK_ENGINE_ERROR_NOT_SUPPORTED, "NotSupported"),
			{ 0, NULL, NULL }
		};
		etype = g_enum_register_static ("PkEngineError", values);
	}
	return etype;
}

/**
 * pk_engine_reset_timer:
 **/
static void
pk_engine_reset_timer (PkEngine *engine)
{
	egg_debug ("reset timer");
	g_timer_reset (engine->priv->timer);
}

/**
 * pk_engine_transaction_list_changed_cb:
 **/
static void
pk_engine_transaction_list_changed_cb (PkTransactionList *tlist, PkEngine *engine)
{
	gchar **transaction_list;

	g_return_if_fail (PK_IS_ENGINE (engine));

	transaction_list = pk_transaction_list_get_array (engine->priv->transaction_list);

	egg_debug ("emitting transaction-list-changed");
	g_signal_emit (engine, signals [PK_ENGINE_TRANSACTION_LIST_CHANGED], 0, transaction_list);
	pk_engine_reset_timer (engine);

	g_strfreev (transaction_list);
}

/**
 * pk_engine_inhibit_locked_cb:
 **/
static void
pk_engine_inhibit_locked_cb (PkInhibit *inhibit, gboolean is_locked, PkEngine *engine)
{
	g_return_if_fail (PK_IS_ENGINE (engine));
	egg_debug ("emitting locked %i", is_locked);
	g_signal_emit (engine, signals [PK_ENGINE_LOCKED], 0, is_locked);
}

/**
 * pk_engine_notify_repo_list_changed_cb:
 **/
static void
pk_engine_notify_repo_list_changed_cb (PkNotify *notify, PkEngine *engine)
{
	g_return_if_fail (PK_IS_ENGINE (engine));
	egg_debug ("emitting repo-list-changed");
	g_signal_emit (engine, signals [PK_ENGINE_REPO_LIST_CHANGED], 0);
}

/**
 * pk_engine_notify_updates_changed_cb:
 **/
static void
pk_engine_notify_updates_changed_cb (PkNotify *notify, PkEngine *engine)
{
	g_return_if_fail (PK_IS_ENGINE (engine));
	egg_debug ("emitting updates-changed");
	g_signal_emit (engine, signals [PK_ENGINE_UPDATES_CHANGED], 0);
}

/**
 * pk_engine_finished_cb:
 **/
static void
pk_engine_finished_cb (PkBackend *backend, PkExitEnum exit_enum, PkEngine *engine)
{
	g_return_if_fail (PK_IS_ENGINE (engine));

	/* daemon is busy */
	pk_engine_reset_timer (engine);
}

/**
 * pk_engine_get_tid:
 **/
void
pk_engine_get_tid (PkEngine *engine, DBusGMethodInvocation *context)
{
	gchar *new_tid;
	gboolean ret;
	gchar *sender = NULL;

	g_return_if_fail (PK_IS_ENGINE (engine));

	egg_debug ("GetTid method called");
	sender = dbus_g_method_get_sender (context);
	new_tid = pk_transaction_db_generate_id (engine->priv->transaction_db);

	ret = pk_transaction_list_create (engine->priv->transaction_list, new_tid, sender);
	egg_debug ("sending tid: '%s'", new_tid);

	/* reset the timer */
	pk_engine_reset_timer (engine);

	/* return TID */
	dbus_g_method_return (context, new_tid);
	g_free (new_tid);
	g_free (sender);
}

/**
 * pk_engine_get_network_state:
 **/
gboolean
pk_engine_get_network_state (PkEngine *engine, gchar **state, GError **error)
{
	PkNetworkEnum network;
	g_return_val_if_fail (PK_IS_ENGINE (engine), FALSE);
	/* get the network state */
	network = pk_network_get_network_state (engine->priv->network);
	*state = g_strdup (pk_network_enum_to_text (network));

	/* reset the timer */
	pk_engine_reset_timer (engine);

	return TRUE;
}

/**
 * pk_engine_get_daemon_state:
 **/
gboolean
pk_engine_get_daemon_state (PkEngine *engine, gchar **state, GError **error)
{
	g_return_val_if_fail (PK_IS_ENGINE (engine), FALSE);

	*state = pk_transaction_list_get_state (engine->priv->transaction_list);

	/* reset the timer */
	pk_engine_reset_timer (engine);

	return TRUE;
}

/**
 * pk_engine_get_transaction_list:
 **/
gboolean
pk_engine_get_transaction_list (PkEngine *engine, gchar ***transaction_list, GError **error)
{
	g_return_val_if_fail (engine != NULL, FALSE);
	g_return_val_if_fail (PK_IS_ENGINE (engine), FALSE);

	egg_debug ("GetTransactionList method called");
	*transaction_list = pk_transaction_list_get_array (engine->priv->transaction_list);

	/* reset the timer */
	pk_engine_reset_timer (engine);

	return TRUE;
}

/**
 * pk_engine_state_changed_cb:
 *
 * wait a little delay in case we get multiple requests or we need to setup state
 **/
static gboolean
pk_engine_state_changed_cb (gpointer data)
{
	PkNetworkEnum state;
	PkEngine *engine = PK_ENGINE (data);

	g_return_val_if_fail (PK_IS_ENGINE (engine), FALSE);

	/* if network is not up, then just reschedule */
	state = pk_network_get_network_state (engine->priv->network);
	if (state == PK_NETWORK_ENUM_OFFLINE) {
		/* wait another timeout of PK_ENGINE_STATE_CHANGED_x_TIMEOUT */
		return TRUE;
	}

	egg_debug ("unreffing updates cache as state may have changed");
	pk_cache_invalidate (engine->priv->cache);

	pk_notify_updates_changed (engine->priv->notify);

	/* reset, now valid */
	engine->priv->signal_state_priority_timeout = 0;
	engine->priv->signal_state_normal_timeout = 0;

	/* reset the timer */
	pk_engine_reset_timer (engine);

	return FALSE;
}

/**
 * pk_engine_state_has_changed:
 *
 * This should be called when tools like pup, pirut and yum-cli
 * have finished their transaction, and the update cache may not be valid.
 **/
gboolean
pk_engine_state_has_changed (PkEngine *engine, const gchar *reason, GError **error)
{
	gboolean is_priority = TRUE;

	g_return_val_if_fail (PK_IS_ENGINE (engine), FALSE);

	/* have we already scheduled priority? */
	if (engine->priv->signal_state_priority_timeout != 0) {
		g_set_error (error, PK_ENGINE_ERROR, PK_ENGINE_ERROR_INVALID_STATE,
			     "Already asked to refresh priority state less than %i seconds ago",
			     PK_ENGINE_STATE_CHANGED_PRIORITY_TIMEOUT);
		return FALSE;
	}

	/* don't bombard the user 10 seconds after resuming */
	if (egg_strequal (reason, "resume"))
		is_priority = FALSE;

	/* are we normal, and already scheduled normal? */
	if (!is_priority && engine->priv->signal_state_normal_timeout != 0) {
		g_set_error (error, PK_ENGINE_ERROR, PK_ENGINE_ERROR_INVALID_STATE,
			     "Already asked to refresh normal state less than %i seconds ago",
			     PK_ENGINE_STATE_CHANGED_NORMAL_TIMEOUT);
		return FALSE;
	}

	/* are we priority, and already scheduled normal? */
	if (is_priority && engine->priv->signal_state_normal_timeout != 0) {
		/* clear normal, as we are about to schedule a priority */
		g_source_remove (engine->priv->signal_state_normal_timeout);
		engine->priv->signal_state_normal_timeout = 0;	}

	/* wait a little delay in case we get multiple requests */
	if (is_priority)
		engine->priv->signal_state_priority_timeout = g_timeout_add_seconds (PK_ENGINE_STATE_CHANGED_PRIORITY_TIMEOUT,
										     pk_engine_state_changed_cb, engine);
	else
		engine->priv->signal_state_normal_timeout = g_timeout_add_seconds (PK_ENGINE_STATE_CHANGED_NORMAL_TIMEOUT,
										   pk_engine_state_changed_cb, engine);

	/* reset the timer */
	pk_engine_reset_timer (engine);

	return TRUE;
}

/**
 * pk_engine_get_actions:
 **/
gboolean
pk_engine_get_actions (PkEngine *engine, gchar **actions, GError **error)
{
	g_return_val_if_fail (PK_IS_ENGINE (engine), FALSE);
	*actions = pk_role_bitfield_to_text (engine->priv->actions);

	/* reset the timer */
	pk_engine_reset_timer (engine);

	return TRUE;
}

/**
 * pk_engine_get_groups:
 **/
gboolean
pk_engine_get_groups (PkEngine *engine, gchar **groups, GError **error)
{
	g_return_val_if_fail (PK_IS_ENGINE (engine), FALSE);
	*groups = pk_group_bitfield_to_text (engine->priv->groups);

	/* reset the timer */
	pk_engine_reset_timer (engine);

	return TRUE;
}

/**
 * pk_engine_get_mime_types:
 **/
gboolean
pk_engine_get_mime_types (PkEngine *engine, gchar **types, GError **error)
{
	g_return_val_if_fail (PK_IS_ENGINE (engine), FALSE);

	/* not compulsory for backends */
	if (engine->priv->mime_types == NULL) {
		g_set_error (error, PK_ENGINE_ERROR, PK_ENGINE_ERROR_NOT_SUPPORTED,
			     "Backend does not provide this information");
		return FALSE;
	}

	*types = g_strdup (engine->priv->mime_types);

	/* reset the timer */
	pk_engine_reset_timer (engine);

	return TRUE;
}

/**
 * pk_engine_get_filters:
 **/
gboolean
pk_engine_get_filters (PkEngine *engine, gchar **filters, GError **error)
{
	g_return_val_if_fail (PK_IS_ENGINE (engine), FALSE);
	*filters = pk_filter_bitfield_to_text (engine->priv->filters);

	/* reset the timer */
	pk_engine_reset_timer (engine);

	return TRUE;
}

/**
 * pk_engine_get_backend_detail:
 **/
gboolean
pk_engine_get_backend_detail (PkEngine *engine, gchar **name, gchar **author, GError **error)
{
	g_return_val_if_fail (PK_IS_ENGINE (engine), FALSE);

	egg_debug ("GetBackendDetail method called");
	pk_backend_get_backend_detail (engine->priv->backend, name, author);

	/* reset the timer */
	pk_engine_reset_timer (engine);

	return TRUE;
}

/**
 * pk_engine_get_time_since_action:
 *
 * @seconds: Number of seconds since the role was called, or zero is unknown
 **/
gboolean
pk_engine_get_time_since_action	(PkEngine *engine, const gchar *role_text, guint *seconds, GError **error)
{
	PkRoleEnum role;

	g_return_val_if_fail (PK_IS_ENGINE (engine), FALSE);

	role = pk_role_enum_from_text (role_text);
	*seconds = pk_transaction_db_action_time_since (engine->priv->transaction_db, role);

	/* reset the timer */
	pk_engine_reset_timer (engine);

	return TRUE;
}

/**
 * pk_engine_get_seconds_idle:
 **/
guint
pk_engine_get_seconds_idle (PkEngine *engine)
{
	guint idle;
	guint size;

	g_return_val_if_fail (PK_IS_ENGINE (engine), 0);

	/* check for transactions running - a transaction that takes a *long* time might not
	 * give sufficient percentage updates to not be marked as idle */
	size = pk_transaction_list_get_size (engine->priv->transaction_list);
	if (size != 0) {
		egg_debug ("engine idle zero as %i transactions in progress", size);
		return 0;
	}

	/* have we been updated? */
	if (engine->priv->notify_clients_of_upgrade) {
		egg_debug ("emitting restart-schedule because of binary change");
		g_signal_emit (engine, signals [PK_ENGINE_RESTART_SCHEDULE], 0);
		return G_MAXUINT;
	}

	/* do we need to shutdown quickly */
	if (engine->priv->shutdown_as_soon_as_possible) {
		egg_debug ("need to restart daemon asap");
		return G_MAXUINT;
	}

	idle = (guint) g_timer_elapsed (engine->priv->timer, NULL);
	return idle;
}

/**
 * pk_engine_suggest_daemon_quit:
 **/
gboolean
pk_engine_suggest_daemon_quit (PkEngine *engine, GError **error)
{
	guint size;

	g_return_val_if_fail (PK_IS_ENGINE (engine), FALSE);

	/* can we exit straight away */
	size = pk_transaction_list_get_size (engine->priv->transaction_list);
	if (size == 0) {
		egg_debug ("emitting quit");
		g_signal_emit (engine, signals [PK_ENGINE_QUIT], 0);
		return TRUE;
	}

	/* This will wait from 0..10 seconds, depending on the status of
	 * pk_main_timeout_check_cb() - usually it should be a few seconds
	 * after the last transaction */
	engine->priv->shutdown_as_soon_as_possible = TRUE;
	return TRUE;
}

/**
 * pk_engine_set_proxy:
 **/
void
pk_engine_set_proxy (PkEngine *engine, const gchar *proxy_http, const gchar *proxy_ftp, DBusGMethodInvocation *context)
{
	gboolean ret;
	GError *error;
	gchar *sender = NULL;
	gchar *error_detail = NULL;
	PkSecurityCaller *caller;

	g_return_if_fail (PK_IS_ENGINE (engine));

	egg_debug ("SetProxy method called: %s, %s", proxy_http, proxy_ftp);

	/* check if the action is allowed from this client - if not, set an error */
	sender = dbus_g_method_get_sender (context);

	/* get caller */
	caller = pk_security_caller_new_from_sender (engine->priv->security, sender);
	if (caller == NULL) {
		error = g_error_new (PK_ENGINE_ERROR, PK_ENGINE_ERROR_REFUSED_BY_POLICY,
				     "caller %s not found", sender);
		dbus_g_method_return_error (context, error);
		goto out;
	}

	/* use security model to get auth */
	ret = pk_security_action_is_allowed (engine->priv->security, caller, FALSE, PK_ROLE_ENUM_SET_PROXY_PRIVATE, &error_detail);
	if (!ret) {
		error = g_error_new (PK_ENGINE_ERROR, PK_ENGINE_ERROR_REFUSED_BY_POLICY, "%s", error_detail);
		dbus_g_method_return_error (context, error);
		goto out;
	}

	/* try to set the new proxy */
	ret = pk_backend_set_proxy (engine->priv->backend, proxy_http, proxy_ftp);
	if (!ret) {
		error = g_error_new (PK_ENGINE_ERROR, PK_ENGINE_ERROR_CANNOT_SET_PROXY, "%s", "setting the proxy failed");
		dbus_g_method_return_error (context, error);
		goto out;
	}

	/* all okay */
	dbus_g_method_return (context);

	/* reset the timer */
	pk_engine_reset_timer (engine);

out:
	if (caller != NULL)
		pk_security_caller_unref (caller);
	g_free (sender);
	g_free (error_detail);
}

/**
 * pk_engine_class_init:
 * @klass: The PkEngineClass
 **/
static void
pk_engine_class_init (PkEngineClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);

	object_class->finalize = pk_engine_finalize;

	/* set up signal that emits 'au' */
	signals [PK_ENGINE_LOCKED] =
		g_signal_new ("locked",
			      G_TYPE_FROM_CLASS (object_class), G_SIGNAL_RUN_LAST,
			      0, NULL, NULL, g_cclosure_marshal_VOID__BOOLEAN,
			      G_TYPE_NONE, 1, G_TYPE_BOOLEAN);
	signals [PK_ENGINE_TRANSACTION_LIST_CHANGED] =
		g_signal_new ("transaction-list-changed",
			      G_TYPE_FROM_CLASS (object_class), G_SIGNAL_RUN_LAST,
			      0, NULL, NULL, g_cclosure_marshal_VOID__BOXED,
			      G_TYPE_NONE, 1, G_TYPE_STRV);
	signals [PK_ENGINE_RESTART_SCHEDULE] =
		g_signal_new ("restart-schedule",
			      G_TYPE_FROM_CLASS (object_class), G_SIGNAL_RUN_LAST,
			      0, NULL, NULL, g_cclosure_marshal_VOID__VOID,
			      G_TYPE_NONE, 0);
	signals [PK_ENGINE_REPO_LIST_CHANGED] =
		g_signal_new ("repo-list-changed",
			      G_TYPE_FROM_CLASS (object_class), G_SIGNAL_RUN_LAST,
			      0, NULL, NULL, g_cclosure_marshal_VOID__VOID,
			      G_TYPE_NONE, 0);
	signals [PK_ENGINE_NETWORK_STATE_CHANGED] =
		g_signal_new ("network-state-changed",
			      G_TYPE_FROM_CLASS (object_class), G_SIGNAL_RUN_LAST,
			      0, NULL, NULL, g_cclosure_marshal_VOID__STRING,
			      G_TYPE_NONE, 1, G_TYPE_STRING);
	signals [PK_ENGINE_UPDATES_CHANGED] =
		g_signal_new ("updates-changed",
			      G_TYPE_FROM_CLASS (object_class), G_SIGNAL_RUN_LAST,
			      0, NULL, NULL, g_cclosure_marshal_VOID__VOID,
			      G_TYPE_NONE, 0);
	signals [PK_ENGINE_QUIT] =
		g_signal_new ("quit",
			      G_TYPE_FROM_CLASS (object_class), G_SIGNAL_RUN_LAST,
			      0, NULL, NULL, g_cclosure_marshal_VOID__VOID,
			      G_TYPE_NONE, 0);

	g_type_class_add_private (klass, sizeof (PkEnginePrivate));
}

/**
 * pk_engine_conf_file_changed_cb:
 *
 * A config file has changed, we need to reload the daemon
 **/
static void
pk_engine_conf_file_changed_cb (PkFileMonitor *file_monitor, PkEngine *engine)
{
	g_return_if_fail (PK_IS_ENGINE (engine));
	egg_debug ("setting shutdown_as_soon_as_possible TRUE");
	engine->priv->shutdown_as_soon_as_possible = TRUE;
}

/**
 * pk_engine_binary_file_changed_cb:
 **/
static void
pk_engine_binary_file_changed_cb (PkFileMonitor *file_monitor, PkEngine *engine)
{
	g_return_if_fail (PK_IS_ENGINE (engine));
	egg_debug ("setting notify_clients_of_upgrade TRUE");
	engine->priv->notify_clients_of_upgrade = TRUE;
}

/**
 * pk_engine_network_state_changed_cb:
 **/
static void
pk_engine_network_state_changed_cb (PkNetwork *network, PkNetworkEnum state, PkEngine *engine)
{
	const gchar *state_text;
	g_return_if_fail (PK_IS_ENGINE (engine));
	state_text = pk_network_enum_to_text (state);
	egg_debug ("emitting network-state-changed: %s", state_text);
	g_signal_emit (engine, signals [PK_ENGINE_NETWORK_STATE_CHANGED], 0, state_text);
}

/**
 * pk_engine_init:
 **/
static void
pk_engine_init (PkEngine *engine)
{
	DBusGConnection *connection;
	gboolean ret;
	gchar *filename;
	gchar *proxy_http;
	gchar *proxy_ftp;

	engine->priv = PK_ENGINE_GET_PRIVATE (engine);
	engine->priv->notify_clients_of_upgrade = FALSE;
	engine->priv->shutdown_as_soon_as_possible = FALSE;
	engine->priv->mime_types = NULL;

	/* use the config file */
	engine->priv->conf = pk_conf_new ();

	/* clear the download cache */
	filename = g_build_filename (LOCALSTATEDIR, "cache", "PackageKit", "downloads", NULL);
	egg_debug ("clearing download cache at %s", filename);
	pk_directory_remove_contents (filename);
	g_free (filename);

	/* setup the backend backend */
	engine->priv->backend = pk_backend_new ();
	g_signal_connect (engine->priv->backend, "finished",
			  G_CALLBACK (pk_engine_finished_cb), engine);

	/* lock database */
	ret = pk_backend_lock (engine->priv->backend);
	if (!ret)
		egg_error ("could not lock backend, you need to restart the daemon");

	/* we dont need this, just don't keep creating and destroying it */
	engine->priv->security = pk_security_new ();

	/* proxy the network state */
	engine->priv->network = pk_network_new ();
	g_signal_connect (engine->priv->network, "state-changed",
			  G_CALLBACK (pk_engine_network_state_changed_cb), engine);

	/* create a new backend so we can get the static stuff */
	engine->priv->actions = pk_backend_get_actions (engine->priv->backend);
	engine->priv->groups = pk_backend_get_groups (engine->priv->backend);
	engine->priv->filters = pk_backend_get_filters (engine->priv->backend);
	engine->priv->mime_types = pk_backend_get_mime_types (engine->priv->backend);

	engine->priv->timer = g_timer_new ();

	/* we save a cache of the latest update lists sowe can do cached responses */
	engine->priv->cache = pk_cache_new ();
	engine->priv->update_detail_cache = pk_update_detail_list_new ();

	/* we need to be able to clear this */
	engine->priv->signal_state_priority_timeout = 0;
	engine->priv->signal_state_normal_timeout = 0;

	/* get another connection */
	connection = dbus_g_bus_get (DBUS_BUS_SYSTEM, NULL);
	if (connection == NULL)
		egg_error ("no connection");

	/* add the interface */
	engine->priv->notify = pk_notify_new ();
	g_signal_connect (engine->priv->notify, "repo-list-changed",
			  G_CALLBACK (pk_engine_notify_repo_list_changed_cb), engine);
	g_signal_connect (engine->priv->notify, "updates-changed",
			  G_CALLBACK (pk_engine_notify_updates_changed_cb), engine);

	/* monitor the config file for changes */
	engine->priv->file_monitor_conf = pk_file_monitor_new ();
	filename = pk_conf_get_filename ();
	pk_file_monitor_set_file (engine->priv->file_monitor_conf, filename);
	g_signal_connect (engine->priv->file_monitor_conf, "file-changed",
			  G_CALLBACK (pk_engine_conf_file_changed_cb), engine);
	g_free (filename);

	/* monitor the binary file for changes */
	engine->priv->file_monitor_binary = pk_file_monitor_new ();
	pk_file_monitor_set_file (engine->priv->file_monitor_binary, SBINDIR "/packagekitd");
	g_signal_connect (engine->priv->file_monitor_binary, "file-changed",
			  G_CALLBACK (pk_engine_binary_file_changed_cb), engine);

	/* set the proxy */
	proxy_http = pk_conf_get_string (engine->priv->conf, "ProxyHTTP");
	proxy_ftp = pk_conf_get_string (engine->priv->conf, "ProxyFTP");
	pk_backend_set_proxy (engine->priv->backend, proxy_http, proxy_ftp);
	g_free (proxy_http);
	g_free (proxy_ftp);

	engine->priv->transaction_list = pk_transaction_list_new ();
	g_signal_connect (engine->priv->transaction_list, "changed",
			  G_CALLBACK (pk_engine_transaction_list_changed_cb), engine);

	engine->priv->inhibit = pk_inhibit_new ();
	g_signal_connect (engine->priv->inhibit, "locked",
			  G_CALLBACK (pk_engine_inhibit_locked_cb), engine);

	/* we use a trasaction db to store old transactions and to do rollbacks */
	engine->priv->transaction_db = pk_transaction_db_new ();
}

/**
 * pk_engine_finalize:
 * @object: The object to finalize
 **/
static void
pk_engine_finalize (GObject *object)
{
	PkEngine *engine;
	gboolean ret;

	g_return_if_fail (object != NULL);
	g_return_if_fail (PK_IS_ENGINE (object));

	engine = PK_ENGINE (object);

	g_return_if_fail (engine->priv != NULL);

	/* unlock if we locked this */
	ret = pk_backend_unlock (engine->priv->backend);
	if (!ret)
		egg_warning ("couldn't unlock the backend");

	/* if we set an state changed notifier, clear */
	if (engine->priv->signal_state_priority_timeout != 0) {
		g_source_remove (engine->priv->signal_state_priority_timeout);
		engine->priv->signal_state_priority_timeout = 0;
	}
	if (engine->priv->signal_state_normal_timeout != 0) {
		g_source_remove (engine->priv->signal_state_normal_timeout);
		engine->priv->signal_state_normal_timeout = 0;
	}

	/* compulsory gobjects */
	g_timer_destroy (engine->priv->timer);
	g_object_unref (engine->priv->file_monitor_conf);
	g_object_unref (engine->priv->file_monitor_binary);
	g_object_unref (engine->priv->inhibit);
	g_object_unref (engine->priv->transaction_list);
	g_object_unref (engine->priv->transaction_db);
	g_object_unref (engine->priv->network);
	g_object_unref (engine->priv->security);
	g_object_unref (engine->priv->notify);
	g_object_unref (engine->priv->backend);
	g_object_unref (engine->priv->cache);
	g_object_unref (engine->priv->update_detail_cache);
	g_object_unref (engine->priv->conf);
	g_free (engine->priv->mime_types);

	G_OBJECT_CLASS (pk_engine_parent_class)->finalize (object);
}

/**
 * pk_engine_new:
 *
 * Return value: a new PkEngine object.
 **/
PkEngine *
pk_engine_new (void)
{
	PkEngine *engine;
	engine = g_object_new (PK_TYPE_ENGINE, NULL);
	return PK_ENGINE (engine);
}

/***************************************************************************
 ***                          MAKE CHECK TESTS                           ***
 ***************************************************************************/
#ifdef EGG_TEST
#include "egg-test.h"

void
pk_engine_test (EggTest *test)
{
	gboolean ret;
	PkEngine *engine;
	PkBackend *backend;
	guint idle;
	gchar *actions;

	if (!egg_test_start (test, "PkEngine"))
		return;

	/* don't do these when doing make distcheck */
#ifndef PK_IS_DEVELOPER
	egg_test_end (test);
	return;
#endif

	/************************************************************/
	egg_test_title (test, "get a backend instance");
	backend = pk_backend_new ();
	egg_test_assert (test, backend != NULL);

	/* set the type, as we have no pk-main doing this for us */
	/************************************************************/
	egg_test_title (test, "set the backend name");
	ret = pk_backend_set_name (backend, "dummy");
	egg_test_assert (test, ret);

	/************************************************************/
	egg_test_title (test, "get an engine instance");
	engine = pk_engine_new ();
	egg_test_assert (test, engine != NULL);

	/************************************************************/
	egg_test_title (test, "get idle at startup");
	idle = pk_engine_get_seconds_idle (engine);
	if (idle < 1)
		egg_test_success (test, NULL);
	else
		egg_test_failed (test, "idle = %i", idle);

	/* wait 5 seconds */
	egg_test_loop_wait (test, 5000);

	/************************************************************/
	egg_test_title (test, "get idle at idle");
	idle = pk_engine_get_seconds_idle (engine);
	if (idle < 6 && idle > 4)
		egg_test_success (test, NULL);
	else
		egg_test_failed (test, "idle = %i", idle);

	/************************************************************/
	egg_test_title (test, "get idle after method");
	pk_engine_get_actions (engine, &actions, NULL);
	g_free (actions);
	idle = pk_engine_get_seconds_idle (engine);
	if (idle < 1)
		egg_test_success (test, NULL);
	else
		egg_test_failed (test, "idle = %i", idle);

	g_object_unref (backend);
	g_object_unref (engine);

	egg_test_end (test);
}
#endif

