/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2008 Richard Hughes <richard@hughsie.com>
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
#include <errno.h>
#include <signal.h>

#include <string.h>
#include <sys/types.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif /* HAVE_UNISTD_H */

#include <sys/wait.h>
#include <fcntl.h>
#include <sys/stat.h>

#include <glib/gstdio.h>
#include <glib/gi18n.h>
#include <dbus/dbus-glib.h>
#include <dbus/dbus-glib-lowlevel.h>
#include <packagekit-glib/packagekit.h>

#include "egg-debug.h"
#include "egg-string.h"
#include "egg-dbus-monitor.h"

#include "pk-transaction.h"
#include "pk-transaction-list.h"
#include "pk-transaction-db.h"
#include "pk-marshal.h"
#include "pk-backend.h"
#include "pk-backend-internal.h"
#include "pk-inhibit.h"
#include "pk-update-detail-list.h"
#include "pk-conf.h"
#include "pk-shared.h"
#include "pk-cache.h"
#include "pk-notify.h"
#include "pk-security.h"
#include "pk-post-trans.h"

static void     pk_transaction_class_init	(PkTransactionClass *klass);
static void     pk_transaction_init		(PkTransaction      *transaction);
static void     pk_transaction_finalize		(GObject	    *object);

#define PK_TRANSACTION_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), PK_TYPE_TRANSACTION, PkTransactionPrivate))
#define PK_TRANSACTION_UPDATES_CHANGED_TIMEOUT	100 /* ms */

static void pk_transaction_status_changed_cb (PkBackend *backend, PkStatusEnum status, PkTransaction *transaction);
static void pk_transaction_progress_changed_cb (PkBackend *backend, guint percentage, guint subpercentage, guint elapsed, guint remaining, PkTransaction *transaction);

struct PkTransactionPrivate
{
	PkRoleEnum		 role;
	PkStatusEnum		 status;
	guint			 percentage;
	guint			 subpercentage;
	guint			 elapsed;
	guint			 remaining;
	gboolean		 finished;
	gboolean		 running;
	gboolean		 has_been_run;
	gboolean		 allow_cancel;
	gboolean		 emit_eula_required;
	gboolean		 emit_signature_required;
	gchar			*locale;
	EggDbusMonitor		*monitor;
	PkBackend		*backend;
	PkInhibit		*inhibit;
	PkCache			*cache;
	PkConf			*conf;
	PkUpdateDetailList	*update_detail_list;
	PkNotify		*notify;
	PkSecurity		*security;
	PkPostTrans		*post_trans;

	/* needed for gui coldplugging */
	gchar			*last_package_id;
	gchar			*dbus_name;
	gchar			*tid;
	PkPackageList		*package_list;
	PkTransactionList	*transaction_list;
	PkTransactionDb		*transaction_db;

	/* cached */
	gboolean		 cached_force;
	gboolean		 cached_allow_deps;
	gboolean		 cached_autoremove;
	gboolean		 cached_enabled;
	gboolean		 cached_trusted;
	gchar			*cached_package_id;
	gchar			**cached_package_ids;
	gchar			*cached_transaction_id;
	gchar			*cached_full_path;
	gchar			**cached_full_paths;
	PkBitfield		 cached_filters;
	gchar			*cached_search;
	gchar			*cached_repo_id;
	gchar			*cached_key_id;
	gchar			*cached_parameter;
	gchar			*cached_value;
	gchar			*cached_directory;
	gchar			*cached_cat_id;
	PkProvidesEnum		 cached_provides;

	guint			 signal_allow_cancel;
	guint			 signal_details;
	guint			 signal_error_code;
	guint			 signal_files;
	guint			 signal_distro_upgrade;
	guint			 signal_finished;
	guint			 signal_message;
	guint			 signal_package;
	guint			 signal_progress_changed;
	guint			 signal_repo_detail;
	guint			 signal_repo_signature_required;
	guint			 signal_eula_required;
	guint			 signal_require_restart;
	guint			 signal_status_changed;
	guint			 signal_update_detail;
	guint			 signal_category;
};

enum {
	PK_TRANSACTION_ALLOW_CANCEL,
	PK_TRANSACTION_CALLER_ACTIVE_CHANGED,
	PK_TRANSACTION_DETAILS,
	PK_TRANSACTION_ERROR_CODE,
	PK_TRANSACTION_DISTRO_UPGRADE,
	PK_TRANSACTION_FILES,
	PK_TRANSACTION_FINISHED,
	PK_TRANSACTION_MESSAGE,
	PK_TRANSACTION_PACKAGE,
	PK_TRANSACTION_PROGRESS_CHANGED,
	PK_TRANSACTION_REPO_DETAIL,
	PK_TRANSACTION_REPO_SIGNATURE_REQUIRED,
	PK_TRANSACTION_EULA_REQUIRED,
	PK_TRANSACTION_REQUIRE_RESTART,
	PK_TRANSACTION_STATUS_CHANGED,
	PK_TRANSACTION_TRANSACTION,
	PK_TRANSACTION_UPDATE_DETAIL,
	PK_TRANSACTION_CATEGORY,
	PK_TRANSACTION_DESTROY,
	PK_TRANSACTION_LAST_SIGNAL
};

static guint	     signals [PK_TRANSACTION_LAST_SIGNAL] = { 0 };

G_DEFINE_TYPE (PkTransaction, pk_transaction, G_TYPE_OBJECT)

/**
 * pk_transaction_error_quark:
 * Return value: Our personal error quark.
 **/
GQuark
pk_transaction_error_quark (void)
{
	static GQuark quark = 0;
	if (!quark)
		quark = g_quark_from_static_string ("pk_transaction_error");
	return quark;
}

/**
 * pk_transaction_error_get_type:
 **/
#define ENUM_ENTRY(NAME, DESC) { NAME, "" #NAME "", DESC }
GType
pk_transaction_error_get_type (void)
{
	static GType etype = 0;

	if (etype == 0) {
		static const GEnumValue values[] =
		{
			ENUM_ENTRY (PK_TRANSACTION_ERROR_DENIED, "PermissionDenied"),
			ENUM_ENTRY (PK_TRANSACTION_ERROR_NOT_RUNNING, "NotRunning"),
			ENUM_ENTRY (PK_TRANSACTION_ERROR_NO_ROLE, "NoRole"),
			ENUM_ENTRY (PK_TRANSACTION_ERROR_CANNOT_CANCEL, "CannotCancel"),
			ENUM_ENTRY (PK_TRANSACTION_ERROR_NOT_SUPPORTED, "NotSupported"),
			ENUM_ENTRY (PK_TRANSACTION_ERROR_NO_SUCH_TRANSACTION, "NoSuchTransaction"),
			ENUM_ENTRY (PK_TRANSACTION_ERROR_NO_SUCH_FILE, "NoSuchFile"),
			ENUM_ENTRY (PK_TRANSACTION_ERROR_NO_SUCH_DIRECTORY, "NoSuchDirectory"),
			ENUM_ENTRY (PK_TRANSACTION_ERROR_TRANSACTION_EXISTS_WITH_ROLE, "TransactionExistsWithRole"),
			ENUM_ENTRY (PK_TRANSACTION_ERROR_REFUSED_BY_POLICY, "RefusedByPolicy"),
			ENUM_ENTRY (PK_TRANSACTION_ERROR_PACKAGE_ID_INVALID, "PackageIdInvalid"),
			ENUM_ENTRY (PK_TRANSACTION_ERROR_SEARCH_INVALID, "SearchInvalid"),
			ENUM_ENTRY (PK_TRANSACTION_ERROR_FILTER_INVALID, "FilterInvalid"),
			ENUM_ENTRY (PK_TRANSACTION_ERROR_INPUT_INVALID, "InputInvalid"),
			ENUM_ENTRY (PK_TRANSACTION_ERROR_INVALID_STATE, "InvalidState"),
			ENUM_ENTRY (PK_TRANSACTION_ERROR_INITIALIZE_FAILED, "InitializeFailed"),
			ENUM_ENTRY (PK_TRANSACTION_ERROR_COMMIT_FAILED, "CommitFailed"),
			ENUM_ENTRY (PK_TRANSACTION_ERROR_PACK_INVALID, "PackInvalid"),
			ENUM_ENTRY (PK_TRANSACTION_ERROR_INVALID_PROVIDE, "InvalidProvide"),
			{ 0, NULL, NULL }
		};
		etype = g_enum_register_static ("PkTransactionError", values);
	}
	return etype;
}

/**
 * pk_transaction_get_runtime:
 *
 * Returns time running in ms
 */
guint
pk_transaction_get_runtime (PkTransaction *transaction)
{
	g_return_val_if_fail (PK_IS_TRANSACTION (transaction), 0);
	g_return_val_if_fail (transaction->priv->tid != NULL, 0);
	return pk_backend_get_runtime (transaction->priv->backend);
}

/**
 * pk_transaction_set_dbus_name:
 */
gboolean
pk_transaction_set_dbus_name (PkTransaction *transaction, const gchar *dbus_name)
{
	g_return_val_if_fail (PK_IS_TRANSACTION (transaction), FALSE);
	g_return_val_if_fail (transaction->priv->tid != NULL, FALSE);
	g_return_val_if_fail (dbus_name != NULL, FALSE);

	if (transaction->priv->dbus_name != NULL) {
		egg_warning ("you can't assign more than once!");
		return FALSE;
	}
	transaction->priv->dbus_name = g_strdup (dbus_name);
	egg_debug ("assigning %s to %p", dbus_name, transaction);
	egg_dbus_monitor_assign (transaction->priv->monitor, EGG_DBUS_MONITOR_SYSTEM, dbus_name);
	return TRUE;
}

/**
 * pk_transaction_set_role:
 * We should only set this when we are creating a manual cache
 **/
static gboolean
pk_transaction_set_role (PkTransaction *transaction, PkRoleEnum role)
{
	g_return_val_if_fail (PK_IS_TRANSACTION (transaction), FALSE);
	g_return_val_if_fail (transaction->priv->tid != NULL, FALSE);

	/* save this */
	transaction->priv->role = role;
	return TRUE;
}

/**
 * pk_transaction_get_text:
 **/
gchar *
pk_transaction_get_text (PkTransaction *transaction)
{
	PkPackageId *id;
	gchar *text = NULL;
	const gchar *data;

	g_return_val_if_fail (PK_IS_TRANSACTION (transaction), NULL);
	g_return_val_if_fail (transaction->priv->tid != NULL, NULL);

	if (transaction->priv->cached_package_id != NULL) {
		data = transaction->priv->cached_package_id;
		/* is a package id? */
		if (pk_package_id_check (data)) {
			id = pk_package_id_new_from_string (data);
			text = g_strdup (id->name);
			pk_package_id_free (id);
		} else {
			text = g_strdup (data);
		}
	} else if (transaction->priv->cached_package_ids != NULL) {
		data = transaction->priv->cached_package_ids[0];
		/* is a package id? */
		if (pk_package_id_check (data)) {
			/* FIXME: join all with ';' */
			id = pk_package_id_new_from_string (data);
			text = g_strdup (id->name);
			pk_package_id_free (id);
		} else {
			text = g_strdup (data);
		}
	} else if (transaction->priv->cached_search != NULL) {
		text = g_strdup (transaction->priv->cached_search);
	}

	return text;
}

/**
 * pk_transaction_finish_invalidate_caches:
 **/
static gboolean
pk_transaction_finish_invalidate_caches (PkTransaction *transaction)
{
	const gchar *c_tid;

	g_return_val_if_fail (PK_IS_TRANSACTION (transaction), FALSE);

	c_tid = pk_backend_get_current_tid (transaction->priv->backend);
	if (c_tid == NULL) {
		egg_warning ("could not get current tid from backend");
		return FALSE;
	}

	egg_debug ("invalidating caches");

	/* copy this into the cache if we are getting updates */
	if (transaction->priv->role == PK_ROLE_ENUM_GET_UPDATES)
		pk_cache_set_updates (transaction->priv->cache, transaction->priv->package_list);

	/* we unref the update cache if it exists */
	if (transaction->priv->role == PK_ROLE_ENUM_UPDATE_SYSTEM ||
	    transaction->priv->role == PK_ROLE_ENUM_UPDATE_PACKAGES)
		pk_cache_invalidate (transaction->priv->cache);

	/* this has to be done as different repos might have different updates */
	if (transaction->priv->role == PK_ROLE_ENUM_REPO_ENABLE ||
	    transaction->priv->role == PK_ROLE_ENUM_REPO_SET_DATA)
		pk_cache_invalidate (transaction->priv->cache);

	/* could the update list have changed? */
	if (transaction->priv->role == PK_ROLE_ENUM_UPDATE_SYSTEM ||
	    transaction->priv->role == PK_ROLE_ENUM_UPDATE_PACKAGES ||
	    transaction->priv->role == PK_ROLE_ENUM_REPO_ENABLE ||
	    transaction->priv->role == PK_ROLE_ENUM_REPO_SET_DATA ||
	    transaction->priv->role == PK_ROLE_ENUM_REFRESH_CACHE) {
		/* this needs to be done after a small delay */
		pk_notify_wait_updates_changed (transaction->priv->notify,
						PK_TRANSACTION_UPDATES_CHANGED_TIMEOUT);
	}
	return TRUE;
}

/**
 * pk_transaction_progress_changed_emit:
 **/
static void
pk_transaction_progress_changed_emit (PkTransaction *transaction, guint percentage, guint subpercentage, guint elapsed, guint remaining)
{
	g_return_if_fail (PK_IS_TRANSACTION (transaction));

	/* save so we can do GetProgress on a queued or finished transaction */
	transaction->priv->percentage = percentage;
	transaction->priv->subpercentage = subpercentage;
	transaction->priv->elapsed = elapsed;
	transaction->priv->remaining = remaining;

	egg_debug ("emitting percentage-changed %i, %i, %i, %i", percentage, subpercentage, elapsed, remaining);
	g_signal_emit (transaction, signals [PK_TRANSACTION_PROGRESS_CHANGED], 0, percentage, subpercentage, elapsed, remaining);
}

/**
 * pk_transaction_allow_cancel_emit:
 **/
static void
pk_transaction_allow_cancel_emit (PkTransaction *transaction, gboolean allow_cancel)
{
	g_return_if_fail (PK_IS_TRANSACTION (transaction));

	transaction->priv->allow_cancel = allow_cancel;

	/* remove or add the hal inhibit */
	if (allow_cancel)
		pk_inhibit_remove (transaction->priv->inhibit, transaction);
	else
		pk_inhibit_add (transaction->priv->inhibit, transaction);

	egg_debug ("emitting allow-cancel %i", allow_cancel);
	g_signal_emit (transaction, signals [PK_TRANSACTION_ALLOW_CANCEL], 0, allow_cancel);
}

/**
 * pk_transaction_status_changed_emit:
 **/
static void
pk_transaction_status_changed_emit (PkTransaction *transaction, PkStatusEnum status)
{
	const gchar *status_text;

	g_return_if_fail (PK_IS_TRANSACTION (transaction));
	g_return_if_fail (transaction->priv->tid != NULL);

	transaction->priv->status = status;
	status_text = pk_status_enum_to_text (status);

	egg_debug ("emitting status-changed '%s'", status_text);
	g_signal_emit (transaction, signals [PK_TRANSACTION_STATUS_CHANGED], 0, status_text);
}

/**
 * pk_transaction_finished_emit:
 **/
static void
pk_transaction_finished_emit (PkTransaction *transaction, PkExitEnum exit, guint time)
{
	const gchar *exit_text;
	exit_text = pk_exit_enum_to_text (exit);
	egg_debug ("emitting finished '%s', %i", exit_text, time);
	g_signal_emit (transaction, signals [PK_TRANSACTION_FINISHED], 0, exit_text, time);
}

/**
 * pk_transaction_allow_cancel_cb:
 **/
static void
pk_transaction_allow_cancel_cb (PkBackend *backend, gboolean allow_cancel, PkTransaction *transaction)
{
	g_return_if_fail (PK_IS_TRANSACTION (transaction));
	g_return_if_fail (transaction->priv->tid != NULL);

	pk_transaction_allow_cancel_emit (transaction, allow_cancel);
}

/**
 * pk_transaction_caller_active_changed_cb:
 **/
static void
pk_transaction_caller_active_changed_cb (EggDbusMonitor *egg_dbus_monitor, gboolean is_active, PkTransaction *transaction)
{
	g_return_if_fail (PK_IS_TRANSACTION (transaction));
	g_return_if_fail (transaction->priv->tid != NULL);

	if (is_active == FALSE) {
		egg_debug ("client disconnected....");
		g_signal_emit (transaction, signals [PK_TRANSACTION_CALLER_ACTIVE_CHANGED], 0, FALSE);
	}
}

/**
 * pk_transaction_details_cb:
 **/
static void
pk_transaction_details_cb (PkBackend *backend, PkDetailsObj *obj, PkTransaction *transaction)
{
	const gchar *group_text;
	gchar *package_id;

	g_return_if_fail (PK_IS_TRANSACTION (transaction));
	g_return_if_fail (transaction->priv->tid != NULL);

	group_text = pk_group_enum_to_text (obj->group);
	package_id = pk_package_id_to_string (obj->id);
	g_signal_emit (transaction, signals [PK_TRANSACTION_DETAILS], 0, package_id,
		       obj->license, group_text, obj->description, obj->url, obj->size);
	g_free (package_id);
}

/**
 * pk_transaction_error_code_cb:
 **/
static void
pk_transaction_error_code_cb (PkBackend *backend, PkErrorCodeEnum code,
			      const gchar *details, PkTransaction *transaction)
{
	const gchar *code_text;

	g_return_if_fail (PK_IS_TRANSACTION (transaction));
	g_return_if_fail (transaction->priv->tid != NULL);

	if (code == PK_ERROR_ENUM_UNKNOWN) {
		pk_backend_message (transaction->priv->backend, PK_MESSAGE_ENUM_BACKEND_ERROR,
				    "%s emitted 'unknown error' rather than a specific error "
				    "- this is a backend problem and should be fixed!", pk_role_enum_to_text (transaction->priv->role));
	}

	code_text = pk_error_enum_to_text (code);
	egg_debug ("emitting error-code %s, '%s'", code_text, details);
	g_signal_emit (transaction, signals [PK_TRANSACTION_ERROR_CODE], 0, code_text, details);
}

/**
 * pk_transaction_files_cb:
 **/
static void
pk_transaction_files_cb (PkBackend *backend, const gchar *package_id,
			 const gchar *filelist, PkTransaction *transaction)
{
	g_return_if_fail (PK_IS_TRANSACTION (transaction));
	g_return_if_fail (transaction->priv->tid != NULL);

	egg_debug ("emitting files %s, %s", package_id, filelist);
	g_signal_emit (transaction, signals [PK_TRANSACTION_FILES], 0, package_id, filelist);
}

/**
 * pk_transaction_category_cb:
 **/
static void
pk_transaction_category_cb (PkBackend *backend, const gchar *parent_id, const gchar *cat_id,
			 const gchar *name, const gchar *summary, const gchar *icon,
			 PkTransaction *transaction)
{
	g_return_if_fail (PK_IS_TRANSACTION (transaction));
	g_return_if_fail (transaction->priv->tid != NULL);

	egg_debug ("emitting category %s, %s, %s, %s, %s ", parent_id, cat_id, name, summary, icon);
	g_signal_emit (transaction, signals [PK_TRANSACTION_CATEGORY], 0, parent_id, cat_id, name, summary, icon);
}

/**
 * pk_transaction_distro_upgrade_cb:
 **/
static void
pk_transaction_distro_upgrade_cb (PkBackend *backend, PkDistroUpgradeEnum type,
				  const gchar *name, const gchar *summary, PkTransaction *transaction)
{
	const gchar *type_text;

	g_return_if_fail (PK_IS_TRANSACTION (transaction));
	g_return_if_fail (transaction->priv->tid != NULL);

	type_text = pk_distro_upgrade_enum_to_text (type);
	egg_debug ("emitting distro-upgrade %s, %s, %s", type_text, name, summary);
	g_signal_emit (transaction, signals [PK_TRANSACTION_DISTRO_UPGRADE], 0, type_text, name, summary);
}

/**
 * pk_transaction_finished_cb:
 **/
static void
pk_transaction_finished_cb (PkBackend *backend, PkExitEnum exit, PkTransaction *transaction)
{
	gboolean ret;
	guint time;
	gchar *packages;
	gchar **package_ids;
	guint i, length;
	PkPackageList *list;
	const PkPackageObj *obj;

	g_return_if_fail (PK_IS_TRANSACTION (transaction));
	g_return_if_fail (transaction->priv->tid != NULL);

	/* have we already been marked as finished? */
	if (transaction->priv->finished) {
		egg_warning ("Already finished");
		return;
	}

	/* disconnect these straight away, as the PkTransaction object takes time to timeout */
	g_signal_handler_disconnect (transaction->priv->backend, transaction->priv->signal_details);
	g_signal_handler_disconnect (transaction->priv->backend, transaction->priv->signal_error_code);
	g_signal_handler_disconnect (transaction->priv->backend, transaction->priv->signal_files);
	g_signal_handler_disconnect (transaction->priv->backend, transaction->priv->signal_distro_upgrade);
	g_signal_handler_disconnect (transaction->priv->backend, transaction->priv->signal_finished);
	g_signal_handler_disconnect (transaction->priv->backend, transaction->priv->signal_package);
	g_signal_handler_disconnect (transaction->priv->backend, transaction->priv->signal_repo_detail);
	g_signal_handler_disconnect (transaction->priv->backend, transaction->priv->signal_repo_signature_required);
	g_signal_handler_disconnect (transaction->priv->backend, transaction->priv->signal_eula_required);
	g_signal_handler_disconnect (transaction->priv->backend, transaction->priv->signal_update_detail);
	g_signal_handler_disconnect (transaction->priv->backend, transaction->priv->signal_category);

	/* check for session restarts */
	if (exit == PK_EXIT_ENUM_SUCCESS &&
	    (transaction->priv->role == PK_ROLE_ENUM_UPDATE_SYSTEM ||
	     transaction->priv->role == PK_ROLE_ENUM_UPDATE_PACKAGES)) {

		/* check updated packages file lists and running processes */
		ret = pk_conf_get_bool (transaction->priv->conf, "UpdateCheckProcesses");
		if (ret) {

			/* filter on UPDATING */
			list = pk_package_list_new ();
			length = pk_package_list_get_size (transaction->priv->package_list);
			for (i=0; i<length; i++) {
				obj = pk_package_list_get_obj (transaction->priv->package_list, i);
				if (obj->info == PK_INFO_ENUM_UPDATING)
					pk_obj_list_add (PK_OBJ_LIST(list), obj);
			}

			/* process file lists on these packages */
			if (PK_OBJ_LIST(list)->len > 0) {
				package_ids = pk_package_list_to_strv (list);
				pk_post_trans_check_process_filelists (transaction->priv->post_trans, package_ids);
				g_strfreev (package_ids);
			}
			g_object_unref (list);
		}
	}

	/* do some optional extra actions when we've finished refreshing the cache */
	if (exit == PK_EXIT_ENUM_SUCCESS &&
	    transaction->priv->role == PK_ROLE_ENUM_REFRESH_CACHE) {

		/* generate the package list */
		ret = pk_conf_get_bool (transaction->priv->conf, "RefreshCacheUpdatePackageList");
		if (ret)
			pk_post_trans_update_package_list (transaction->priv->post_trans);

		/* refresh the desktop icon cache */
		ret = pk_conf_get_bool (transaction->priv->conf, "RefreshCacheScanDesktopFiles");
		if (ret)
			pk_post_trans_import_desktop_files (transaction->priv->post_trans);

		/* clear the firmware requests directory */
		pk_post_trans_clear_firmware_requests (transaction->priv->post_trans);
	}

	/* signals we are allowed to send from a post transaction */
	g_signal_handler_disconnect (transaction->priv->backend, transaction->priv->signal_allow_cancel);
	g_signal_handler_disconnect (transaction->priv->backend, transaction->priv->signal_message);
	g_signal_handler_disconnect (transaction->priv->backend, transaction->priv->signal_status_changed);
	g_signal_handler_disconnect (transaction->priv->backend, transaction->priv->signal_progress_changed);
	g_signal_handler_disconnect (transaction->priv->backend, transaction->priv->signal_require_restart);

	/* if we did not send this, ensure the GUI has the right state */
	if (transaction->priv->allow_cancel)
		pk_transaction_allow_cancel_emit (transaction, FALSE);

	/* we should get no more from the backend with this tid */
	transaction->priv->finished = TRUE;

	/* mark not running */
	transaction->priv->running = FALSE;

	/* if we did ::repo-signature-required or ::eula-required, change the error code */
	if (transaction->priv->emit_signature_required)
		exit = PK_EXIT_ENUM_KEY_REQUIRED;
	else if (transaction->priv->emit_eula_required)
		exit = PK_EXIT_ENUM_EULA_REQUIRED;

	/* invalidate some caches if we succeeded*/
	if (exit == PK_EXIT_ENUM_SUCCESS)
		pk_transaction_finish_invalidate_caches (transaction);

	/* find the length of time we have been running */
	time = pk_transaction_get_runtime (transaction);
	egg_debug ("backend was running for %i ms", time);

	/* add to the database if we are going to log it */
	if (transaction->priv->role == PK_ROLE_ENUM_UPDATE_SYSTEM ||
	    transaction->priv->role == PK_ROLE_ENUM_UPDATE_PACKAGES ||
	    transaction->priv->role == PK_ROLE_ENUM_INSTALL_PACKAGES ||
	    transaction->priv->role == PK_ROLE_ENUM_REMOVE_PACKAGES) {
		packages = pk_obj_list_to_string (PK_OBJ_LIST(transaction->priv->package_list));
		if (!egg_strzero (packages))
			pk_transaction_db_set_data (transaction->priv->transaction_db, transaction->priv->tid, packages);
		g_free (packages);
	}

	/* the repo list will have changed */
	if (transaction->priv->role == PK_ROLE_ENUM_REPO_ENABLE ||
	    transaction->priv->role == PK_ROLE_ENUM_REPO_SET_DATA) {
		pk_notify_repo_list_changed (transaction->priv->notify);
	}

	/* only reset the time if we succeeded */
	if (exit == PK_EXIT_ENUM_SUCCESS)
		pk_transaction_db_action_time_reset (transaction->priv->transaction_db, transaction->priv->role);

	/* did we finish okay? */
	if (exit == PK_EXIT_ENUM_SUCCESS)
		pk_transaction_db_set_finished (transaction->priv->transaction_db, transaction->priv->tid, TRUE, time);
	else
		pk_transaction_db_set_finished (transaction->priv->transaction_db, transaction->priv->tid, FALSE, time);

	/* remove any inhibit */
	pk_inhibit_remove (transaction->priv->inhibit, transaction);

	/* we emit last, as other backends will be running very soon after us, and we don't want to be notified */
	pk_transaction_finished_emit (transaction, exit, time);
}

/**
 * pk_transaction_message_cb:
 **/
static void
pk_transaction_message_cb (PkBackend *backend, PkMessageEnum message, const gchar *details, PkTransaction *transaction)
{
	const gchar *message_text;

	g_return_if_fail (PK_IS_TRANSACTION (transaction));
	g_return_if_fail (transaction->priv->tid != NULL);

#ifndef PK_IS_DEVELOPER
	if (message == PK_MESSAGE_ENUM_BACKEND_ERROR ||
	    message == PK_MESSAGE_ENUM_DAEMON_ERROR) {
		egg_warning ("ignoring message: %s", details);
		return;
	}
#endif

	message_text = pk_message_enum_to_text (message);
	egg_debug ("emitting message %s, '%s'", message_text, details);
	g_signal_emit (transaction, signals [PK_TRANSACTION_MESSAGE], 0, message_text, details);
}

/**
 * pk_transaction_package_cb:
 **/
static void
pk_transaction_package_cb (PkBackend *backend, const PkPackageObj *obj, PkTransaction *transaction)
{
	const gchar *info_text;
	const gchar *role_text;

	g_return_if_fail (PK_IS_TRANSACTION (transaction));
	g_return_if_fail (transaction->priv->tid != NULL);

	/* have we already been marked as finished? */
	if (transaction->priv->finished) {
		egg_warning ("Already finished");
		return;
	}

	/* we need this in warnings */
	role_text = pk_role_enum_to_text (transaction->priv->role);

	/* check the backend is doing the right thing */
	if (transaction->priv->role == PK_ROLE_ENUM_UPDATE_SYSTEM ||
	    transaction->priv->role == PK_ROLE_ENUM_INSTALL_PACKAGES ||
	    transaction->priv->role == PK_ROLE_ENUM_UPDATE_PACKAGES) {
		if (obj->info == PK_INFO_ENUM_INSTALLED) {
			pk_backend_message (transaction->priv->backend, PK_MESSAGE_ENUM_BACKEND_ERROR,
					    "%s emitted 'installed' rather than 'installing' "
					    "- you need to do the package *before* you do the action", role_text);
			return;
		}
	}

	/* check we are respecting the filters */
	if (pk_bitfield_contain (transaction->priv->cached_filters, PK_FILTER_ENUM_NOT_INSTALLED)) {
		if (obj->info == PK_INFO_ENUM_INSTALLED) {
			pk_backend_message (transaction->priv->backend, PK_MESSAGE_ENUM_BACKEND_ERROR,
					    "%s emitted package that was installed when "
					    "the ~installed filter is in place", role_text);
			return;
		}
	}
	if (pk_bitfield_contain (transaction->priv->cached_filters, PK_FILTER_ENUM_INSTALLED)) {
		if (obj->info == PK_INFO_ENUM_AVAILABLE) {
			pk_backend_message (transaction->priv->backend, PK_MESSAGE_ENUM_BACKEND_ERROR,
					    "%s emitted package that was ~installed when "
					    "the installed filter is in place", role_text);
			return;
		}
	}

	/* add to package cache even if we already got a result */
	info_text = pk_info_enum_to_text (obj->info);
	pk_obj_list_add (PK_OBJ_LIST(transaction->priv->package_list), obj);

	/* emit */
	g_free (transaction->priv->last_package_id);
	transaction->priv->last_package_id = pk_package_id_to_string (obj->id);
	g_signal_emit (transaction, signals [PK_TRANSACTION_PACKAGE], 0, info_text,
		       transaction->priv->last_package_id, obj->summary);
}

/**
 * pk_transaction_progress_changed_cb:
 **/
static void
pk_transaction_progress_changed_cb (PkBackend *backend, guint percentage, guint subpercentage,
				    guint elapsed, guint remaining, PkTransaction *transaction)
{
	g_return_if_fail (PK_IS_TRANSACTION (transaction));
	g_return_if_fail (transaction->priv->tid != NULL);

	pk_transaction_progress_changed_emit (transaction, percentage, subpercentage, elapsed, remaining);
}

/**
 * pk_transaction_repo_detail_cb:
 **/
static void
pk_transaction_repo_detail_cb (PkBackend *backend, const gchar *repo_id,
			       const gchar *description, gboolean enabled, PkTransaction *transaction)
{
	g_return_if_fail (PK_IS_TRANSACTION (transaction));
	g_return_if_fail (transaction->priv->tid != NULL);

	egg_debug ("emitting repo-detail %s, %s, %i", repo_id, description, enabled);
	g_signal_emit (transaction, signals [PK_TRANSACTION_REPO_DETAIL], 0, repo_id, description, enabled);
}

/**
 * pk_transaction_repo_signature_required_cb:
 **/
static void
pk_transaction_repo_signature_required_cb (PkBackend *backend, const gchar *package_id,
					   const gchar *repository_name, const gchar *key_url,
					   const gchar *key_userid, const gchar *key_id,
					   const gchar *key_fingerprint, const gchar *key_timestamp,
					   PkSigTypeEnum type, PkTransaction *transaction)
{
	const gchar *type_text;

	g_return_if_fail (PK_IS_TRANSACTION (transaction));
	g_return_if_fail (transaction->priv->tid != NULL);

	type_text = pk_sig_type_enum_to_text (type);

	egg_debug ("emitting repo_signature_required %s, %s, %s, %s, %s, %s, %s, %s",
		  package_id, repository_name, key_url, key_userid, key_id,
		  key_fingerprint, key_timestamp, type_text);
	g_signal_emit (transaction, signals [PK_TRANSACTION_REPO_SIGNATURE_REQUIRED], 0,
		       package_id, repository_name, key_url, key_userid, key_id,
		       key_fingerprint, key_timestamp, type_text);

	/* we should mark this transaction so that we finish with a special code */
	transaction->priv->emit_signature_required = TRUE;
}

/**
 * pk_transaction_eula_required_cb:
 **/
static void
pk_transaction_eula_required_cb (PkBackend *backend, const gchar *eula_id, const gchar *package_id,
				 const gchar *vendor_name, const gchar *license_agreement,
				 PkTransaction *transaction)
{
	g_return_if_fail (PK_IS_TRANSACTION (transaction));
	g_return_if_fail (transaction->priv->tid != NULL);

	egg_debug ("emitting eula-required %s, %s, %s, %s",
		  eula_id, package_id, vendor_name, license_agreement);
	g_signal_emit (transaction, signals [PK_TRANSACTION_EULA_REQUIRED], 0,
		       eula_id, package_id, vendor_name, license_agreement);

	/* we should mark this transaction so that we finish with a special code */
	transaction->priv->emit_eula_required = TRUE;
}

/**
 * pk_transaction_require_restart_cb:
 **/
static void
pk_transaction_require_restart_cb (PkBackend *backend, PkRestartEnum restart, const gchar *details, PkTransaction *transaction)
{
	const gchar *restart_text;

	g_return_if_fail (PK_IS_TRANSACTION (transaction));
	g_return_if_fail (transaction->priv->tid != NULL);

	restart_text = pk_restart_enum_to_text (restart);
	egg_debug ("emitting require-restart %s, '%s'", restart_text, details);
	g_signal_emit (transaction, signals [PK_TRANSACTION_REQUIRE_RESTART], 0, restart_text, details);
}

/**
 * pk_transaction_status_changed_cb:
 **/
static void
pk_transaction_status_changed_cb (PkBackend *backend, PkStatusEnum status, PkTransaction *transaction)
{
	g_return_if_fail (PK_IS_TRANSACTION (transaction));
	g_return_if_fail (transaction->priv->tid != NULL);

	/* have we already been marked as finished? */
	if (transaction->priv->finished) {
		egg_warning ("Already finished, so can't proxy status %s", pk_status_enum_to_text (status));
		return;
	}

	pk_transaction_status_changed_emit (transaction, status);
}

/**
 * pk_transaction_transaction_cb:
 **/
static void
pk_transaction_transaction_cb (PkTransactionDb *tdb, const gchar *old_tid, const gchar *timespec,
			       gboolean succeeded, PkRoleEnum role, guint duration,
			       const gchar *data, PkTransaction *transaction)
{
	const gchar *role_text;

	g_return_if_fail (PK_IS_TRANSACTION (transaction));
	g_return_if_fail (transaction->priv->tid != NULL);

	role_text = pk_role_enum_to_text (role);
	egg_debug ("emitting transaction %s, %s, %i, %s, %i, %s", old_tid, timespec, succeeded, role_text, duration, data);
	g_signal_emit (transaction, signals [PK_TRANSACTION_TRANSACTION], 0, old_tid, timespec, succeeded, role_text, duration, data);
}

/**
 * pk_transaction_update_detail_cb:
 **/
static void
pk_transaction_update_detail_cb (PkBackend *backend, const PkUpdateDetailObj *detail, PkTransaction *transaction)
{
	const gchar *restart_text;
	const gchar *state_text;
	gchar *package_id;
	gchar *issued;
	gchar *updated;

	g_return_if_fail (PK_IS_TRANSACTION (transaction));
	g_return_if_fail (transaction->priv->tid != NULL);

	/* add, if not already added? */
	pk_update_detail_list_add_obj (transaction->priv->update_detail_list, detail);

	restart_text = pk_restart_enum_to_text (detail->restart);
	package_id = pk_package_id_to_string (detail->id);
	state_text = pk_update_state_enum_to_text (detail->state);
	issued = pk_iso8601_from_date (detail->issued);
	updated = pk_iso8601_from_date (detail->updated);

	g_signal_emit (transaction, signals [PK_TRANSACTION_UPDATE_DETAIL], 0,
		       package_id, detail->updates, detail->obsoletes, detail->vendor_url,
		       detail->bugzilla_url, detail->cve_url, restart_text, detail->update_text,
		       detail->changelog, state_text, issued, updated);

	g_free (issued);
	g_free (updated);
	g_free (package_id);
}

/**
 * pk_transaction_set_running:
 */
G_GNUC_WARN_UNUSED_RESULT static gboolean
pk_transaction_set_running (PkTransaction *transaction)
{
	PkBackendDesc *desc;
	PkStore *store;
	PkTransactionPrivate *priv = PK_TRANSACTION_GET_PRIVATE (transaction);
	g_return_val_if_fail (PK_IS_TRANSACTION (transaction), FALSE);
	g_return_val_if_fail (transaction->priv->tid != NULL, FALSE);

	/* prepare for use; the transaction list ensures this is safe */
	pk_backend_reset (transaction->priv->backend);

	/* assign */
	pk_backend_set_current_tid (priv->backend, priv->tid);

	/* if we didn't set a locale for this transaction, we would reuse the
	 * last set locale in the backend, or NULL if it was not ever set.
	 * in this case use the C locale */
	if (priv->locale == NULL)
		pk_backend_set_locale (priv->backend, "C");
	else
		pk_backend_set_locale (priv->backend, priv->locale);

	/* set the role */
	pk_backend_set_role (priv->backend, priv->role);

	/* we are no longer waiting, we are setting up */
	pk_backend_set_status (priv->backend, PK_STATUS_ENUM_SETUP);

	/* connect up the signals */
	transaction->priv->signal_allow_cancel =
		g_signal_connect (transaction->priv->backend, "allow-cancel",
				  G_CALLBACK (pk_transaction_allow_cancel_cb), transaction);
	transaction->priv->signal_details =
		g_signal_connect (transaction->priv->backend, "details",
				  G_CALLBACK (pk_transaction_details_cb), transaction);
	transaction->priv->signal_error_code =
		g_signal_connect (transaction->priv->backend, "error-code",
				  G_CALLBACK (pk_transaction_error_code_cb), transaction);
	transaction->priv->signal_files =
		g_signal_connect (transaction->priv->backend, "files",
				  G_CALLBACK (pk_transaction_files_cb), transaction);
	transaction->priv->signal_distro_upgrade =
		g_signal_connect (transaction->priv->backend, "distro-upgrade",
				  G_CALLBACK (pk_transaction_distro_upgrade_cb), transaction);
	transaction->priv->signal_finished =
		g_signal_connect (transaction->priv->backend, "finished",
				  G_CALLBACK (pk_transaction_finished_cb), transaction);
	transaction->priv->signal_message =
		g_signal_connect (transaction->priv->backend, "message",
				  G_CALLBACK (pk_transaction_message_cb), transaction);
	transaction->priv->signal_package =
		g_signal_connect (transaction->priv->backend, "package",
				  G_CALLBACK (pk_transaction_package_cb), transaction);
	transaction->priv->signal_progress_changed =
		g_signal_connect (transaction->priv->backend, "progress-changed",
				  G_CALLBACK (pk_transaction_progress_changed_cb), transaction);
	transaction->priv->signal_repo_detail =
		g_signal_connect (transaction->priv->backend, "repo-detail",
				  G_CALLBACK (pk_transaction_repo_detail_cb), transaction);
	transaction->priv->signal_repo_signature_required =
		g_signal_connect (transaction->priv->backend, "repo-signature-required",
				  G_CALLBACK (pk_transaction_repo_signature_required_cb), transaction);
	transaction->priv->signal_eula_required =
		g_signal_connect (transaction->priv->backend, "eula-required",
				  G_CALLBACK (pk_transaction_eula_required_cb), transaction);
	transaction->priv->signal_require_restart =
		g_signal_connect (transaction->priv->backend, "require-restart",
				  G_CALLBACK (pk_transaction_require_restart_cb), transaction);
	transaction->priv->signal_status_changed =
		g_signal_connect (transaction->priv->backend, "status-changed",
				  G_CALLBACK (pk_transaction_status_changed_cb), transaction);
	transaction->priv->signal_update_detail =
		g_signal_connect (transaction->priv->backend, "update-detail",
				  G_CALLBACK (pk_transaction_update_detail_cb), transaction);
	transaction->priv->signal_category =
		g_signal_connect (transaction->priv->backend, "category",
				  G_CALLBACK (pk_transaction_category_cb), transaction);

	/* mark running */
	transaction->priv->running = TRUE;
	transaction->priv->has_been_run = TRUE;
	transaction->priv->allow_cancel = FALSE;

	/* set all possible arguments for backend */
	store = pk_backend_get_store (priv->backend);
	pk_store_set_bool (store, "force", priv->cached_force);
	pk_store_set_bool (store, "allow_deps", priv->cached_allow_deps);
	pk_store_set_bool (store, "autoremove", priv->cached_autoremove);
	pk_store_set_bool (store, "enabled", priv->cached_enabled);
	pk_store_set_bool (store, "trusted", priv->cached_trusted);
	pk_store_set_uint (store, "filters", priv->cached_filters);
	pk_store_set_uint (store, "provides", priv->cached_provides);
	pk_store_set_strv (store, "package_ids", priv->cached_package_ids);
	pk_store_set_strv (store, "full_paths", priv->cached_full_paths);
	pk_store_set_string (store, "package_id", priv->cached_package_id);
	pk_store_set_string (store, "transaction_id", priv->cached_transaction_id);
	pk_store_set_string (store, "full_path", priv->cached_full_path);
	pk_store_set_string (store, "search", priv->cached_search);
	pk_store_set_string (store, "repo_id", priv->cached_repo_id);
	pk_store_set_string (store, "key_id", priv->cached_key_id);
	pk_store_set_string (store, "parameter", priv->cached_parameter);
	pk_store_set_string (store, "value", priv->cached_value);
	pk_store_set_string (store, "directory", priv->cached_directory);

	/* lets reduce pointer dereferences... */
	desc = priv->backend->desc;

	/* do the correct action with the cached parameters */
	if (priv->role == PK_ROLE_ENUM_GET_DEPENDS)
		desc->get_depends (priv->backend, priv->cached_filters, priv->cached_package_ids, priv->cached_force);
	else if (priv->role == PK_ROLE_ENUM_GET_UPDATE_DETAIL)
		desc->get_update_detail (priv->backend, priv->cached_package_ids);
	else if (priv->role == PK_ROLE_ENUM_RESOLVE)
		desc->resolve (priv->backend, priv->cached_filters, priv->cached_package_ids);
	else if (priv->role == PK_ROLE_ENUM_ROLLBACK)
		desc->rollback (priv->backend, priv->cached_transaction_id);
	else if (priv->role == PK_ROLE_ENUM_DOWNLOAD_PACKAGES)
		desc->download_packages (priv->backend, priv->cached_package_ids, priv->cached_directory);
	else if (priv->role == PK_ROLE_ENUM_GET_DETAILS)
		desc->get_details (priv->backend, priv->cached_package_ids);
	else if (priv->role == PK_ROLE_ENUM_GET_DISTRO_UPGRADES)
		desc->get_distro_upgrades (priv->backend);
	else if (priv->role == PK_ROLE_ENUM_GET_FILES)
		desc->get_files (priv->backend, priv->cached_package_ids);
	else if (priv->role == PK_ROLE_ENUM_GET_REQUIRES)
		desc->get_requires (priv->backend, priv->cached_filters, priv->cached_package_ids, priv->cached_force);
	else if (priv->role == PK_ROLE_ENUM_WHAT_PROVIDES)
		desc->what_provides (priv->backend, priv->cached_filters, priv->cached_provides, priv->cached_search);
	else if (priv->role == PK_ROLE_ENUM_GET_UPDATES)
		desc->get_updates (priv->backend, priv->cached_filters);
	else if (priv->role == PK_ROLE_ENUM_GET_PACKAGES)
		desc->get_packages (priv->backend, priv->cached_filters);
	else if (priv->role == PK_ROLE_ENUM_SEARCH_DETAILS)
		desc->search_details (priv->backend, priv->cached_filters, priv->cached_search);
	else if (priv->role == PK_ROLE_ENUM_SEARCH_FILE)
		desc->search_file (priv->backend, priv->cached_filters, priv->cached_search);
	else if (priv->role == PK_ROLE_ENUM_SEARCH_GROUP)
		desc->search_group (priv->backend, priv->cached_filters, priv->cached_search);
	else if (priv->role == PK_ROLE_ENUM_SEARCH_NAME)
		desc->search_name (priv->backend,priv->cached_filters,priv->cached_search);
	else if (priv->role == PK_ROLE_ENUM_INSTALL_PACKAGES)
		desc->install_packages (priv->backend, priv->cached_package_ids);
	else if (priv->role == PK_ROLE_ENUM_INSTALL_FILES)
		desc->install_files (priv->backend, priv->cached_trusted, priv->cached_full_paths);
	else if (priv->role == PK_ROLE_ENUM_INSTALL_SIGNATURE)
		desc->install_signature (priv->backend, PK_SIGTYPE_ENUM_GPG, priv->cached_key_id, priv->cached_package_id);
	else if (priv->role == PK_ROLE_ENUM_REFRESH_CACHE)
		desc->refresh_cache (priv->backend,  priv->cached_force);
	else if (priv->role == PK_ROLE_ENUM_REMOVE_PACKAGES)
		desc->remove_packages (priv->backend, priv->cached_package_ids, priv->cached_allow_deps, priv->cached_autoremove);
	else if (priv->role == PK_ROLE_ENUM_UPDATE_PACKAGES)
		desc->update_packages (priv->backend, priv->cached_package_ids);
	else if (priv->role == PK_ROLE_ENUM_UPDATE_SYSTEM)
		desc->update_system (priv->backend);
	else if (priv->role == PK_ROLE_ENUM_GET_CATEGORIES)
		desc->get_categories (priv->backend);
	else if (priv->role == PK_ROLE_ENUM_GET_REPO_LIST)
		desc->get_repo_list (priv->backend, priv->cached_filters);
	else if (priv->role == PK_ROLE_ENUM_REPO_ENABLE)
		desc->repo_enable (priv->backend, priv->cached_repo_id, priv->cached_enabled);
	else if (priv->role == PK_ROLE_ENUM_REPO_SET_DATA)
		desc->repo_set_data (priv->backend, priv->cached_repo_id, priv->cached_parameter, priv->cached_value);
	else {
		egg_error ("failed to run as role not assigned");
		return FALSE;
	}
	return TRUE;
}

/**
 * pk_transaction_run:
 */
gboolean
pk_transaction_run (PkTransaction *transaction)
{
	gboolean ret;
	g_return_val_if_fail (PK_IS_TRANSACTION (transaction), FALSE);
	g_return_val_if_fail (transaction->priv->tid != NULL, FALSE);

	ret = pk_transaction_set_running (transaction);
	return ret;
}

/**
 * pk_transaction_get_tid:
 */
const gchar *
pk_transaction_get_tid (PkTransaction *transaction)
{
	g_return_val_if_fail (PK_IS_TRANSACTION (transaction), NULL);
	g_return_val_if_fail (transaction->priv->tid != NULL, NULL);

	return transaction->priv->tid;
}

/**
 * pk_transaction_set_tid:
 */
gboolean
pk_transaction_set_tid (PkTransaction *transaction, const gchar *tid)
{
	g_return_val_if_fail (PK_IS_TRANSACTION (transaction), FALSE);
	g_return_val_if_fail (tid != NULL, FALSE);
	g_return_val_if_fail (transaction->priv->tid == NULL, FALSE);

	if (transaction->priv->tid != NULL) {
		egg_warning ("changing a tid -- why?");
		return FALSE;
	}
	g_free (transaction->priv->tid);
	transaction->priv->tid = g_strdup (tid);
	return TRUE;
}

/**
 * pk_transaction_commit:
 **/
G_GNUC_WARN_UNUSED_RESULT static gboolean
pk_transaction_commit (PkTransaction *transaction)
{
	gboolean ret;

	g_return_val_if_fail (PK_IS_TRANSACTION (transaction), FALSE);
	g_return_val_if_fail (transaction->priv->tid != NULL, FALSE);

	/* commit, so it appears in the JobList */
	ret = pk_transaction_list_commit (transaction->priv->transaction_list,
					  transaction->priv->tid);
	if (!ret) {
		pk_transaction_list_remove (transaction->priv->transaction_list,
					    transaction->priv->tid);
		egg_warning ("failed to commit (job not run?)");
		return FALSE;
	}

	/* only save into the database for useful stuff */
	if (transaction->priv->role == PK_ROLE_ENUM_UPDATE_SYSTEM ||
	    transaction->priv->role == PK_ROLE_ENUM_REMOVE_PACKAGES ||
	    transaction->priv->role == PK_ROLE_ENUM_INSTALL_PACKAGES ||
	    transaction->priv->role == PK_ROLE_ENUM_UPDATE_PACKAGES) {
		/* add to database */
		pk_transaction_db_add (transaction->priv->transaction_db, transaction->priv->tid);

		/* save role in the database */
		pk_transaction_db_set_role (transaction->priv->transaction_db, transaction->priv->tid, transaction->priv->role);
	}
	return TRUE;
}

/**
 * pk_transaction_finished_idle_cb:
 **/
static gboolean
pk_transaction_finished_idle_cb (PkTransaction *transaction)
{
	pk_transaction_finished_emit (transaction, PK_EXIT_ENUM_SUCCESS, 0);
	return FALSE;
}

/**
 * pk_transaction_search_check:
 **/
static gboolean
pk_transaction_search_check (const gchar *search, GError **error)
{
	guint size;
	gboolean ret;

	/* limit to a 1k chunk */
	size = egg_strlen (search, 1024);

	if (search == NULL) {
		*error = g_error_new (PK_TRANSACTION_ERROR, PK_TRANSACTION_ERROR_SEARCH_INVALID,
				     "Search is null. This isn't supposed to happen...");
		return FALSE;
	}
	if (size == 0) {
		*error = g_error_new (PK_TRANSACTION_ERROR, PK_TRANSACTION_ERROR_SEARCH_INVALID,
				     "Search string zero length");
		return FALSE;
	}
	if (strstr (search, "*") != NULL) {
		*error = g_error_new (PK_TRANSACTION_ERROR, PK_TRANSACTION_ERROR_SEARCH_INVALID,
				     "Invalid search containing '*'");
		return FALSE;
	}
	if (strstr (search, "?") != NULL) {
		*error = g_error_new (PK_TRANSACTION_ERROR, PK_TRANSACTION_ERROR_SEARCH_INVALID,
				     "Invalid search containing '?'");
		return FALSE;
	}
	if (size < 2) {
		*error = g_error_new (PK_TRANSACTION_ERROR, PK_TRANSACTION_ERROR_SEARCH_INVALID,
				     "The search string length is too small");
		return FALSE;
	}
	if (size == 1024) {
		*error = g_error_new (PK_TRANSACTION_ERROR, PK_TRANSACTION_ERROR_SEARCH_INVALID,
				     "The search string length is too large");
		return FALSE;
	}
	ret = pk_strvalidate (search);
	if (!ret) {
		*error = g_error_new (PK_TRANSACTION_ERROR, PK_TRANSACTION_ERROR_INPUT_INVALID,
				     "Invalid search term");
		return FALSE;
	}
	return TRUE;
}

/**
 * pk_transaction_filter_check:
 **/
gboolean
pk_transaction_filter_check (const gchar *filter, GError **error)
{
	gchar **sections;
	guint i;
	guint length;
	gboolean ret = FALSE;

	g_return_val_if_fail (error != NULL, FALSE);

	/* is zero? */
	if (egg_strzero (filter)) {
		*error = g_error_new (PK_TRANSACTION_ERROR, PK_TRANSACTION_ERROR_INPUT_INVALID,
				     "filter zero length");
		return FALSE;
	}

	/* check for invalid input */
	ret = pk_strvalidate (filter);
	if (!ret) {
		*error = g_error_new (PK_TRANSACTION_ERROR, PK_TRANSACTION_ERROR_INPUT_INVALID,
				     "Invalid filter term: %s", filter);
		return FALSE;
	}

	/* split by delimeter ';' */
	sections = g_strsplit (filter, ";", 0);
	length = g_strv_length (sections);
	for (i=0; i<length; i++) {
		/* only one wrong part is enough to fail the filter */
		if (egg_strzero (sections[i])) {
			*error = g_error_new (PK_TRANSACTION_ERROR, PK_TRANSACTION_ERROR_INPUT_INVALID,
					     "Single empty section of filter: %s", filter);
			goto out;
		}
		if (pk_filter_enum_from_text (sections[i]) == PK_FILTER_ENUM_UNKNOWN) {
			*error = g_error_new (PK_TRANSACTION_ERROR, PK_TRANSACTION_ERROR_INPUT_INVALID,
					     "Unknown filter part: %s", sections[i]);
			goto out;
		}
	}
	ret = TRUE;
out:
	g_strfreev (sections);
	return ret;
}

/**
 * pk_transaction_action_is_allowed:
 *
 * Only valid from an async caller, which is fine, as we won't prompt the user
 * when not async.
 **/
static gboolean
pk_transaction_action_is_allowed (PkTransaction *transaction, gboolean trusted, PkRoleEnum role, GError **error)
{
	gboolean ret;
	gchar *error_detail;

	g_return_val_if_fail (transaction->priv->dbus_name != NULL, FALSE);

	/* use security model to get auth */
	ret = pk_security_action_is_allowed (transaction->priv->security, transaction->priv->dbus_name, trusted, role, &error_detail);
	if (!ret) {
		*error = g_error_new (PK_TRANSACTION_ERROR, PK_TRANSACTION_ERROR_REFUSED_BY_POLICY, "%s", error_detail);
		return FALSE;
	}
	return TRUE;
}

/**
 * pk_transaction_priv_get_role:
 **/
PkRoleEnum
pk_transaction_priv_get_role (PkTransaction *transaction)
{
	g_return_val_if_fail (PK_IS_TRANSACTION (transaction), FALSE);
	return transaction->priv->role;
}

/**
 * pk_transaction_accept_eula:
 *
 * This should be called when a eula_id needs to be added into an internal db.
 **/
void
pk_transaction_accept_eula (PkTransaction *transaction, const gchar *eula_id, DBusGMethodInvocation *context)
{
	gboolean ret;
	GError *error;
	gchar *sender;

	g_return_if_fail (PK_IS_TRANSACTION (transaction));
	g_return_if_fail (transaction->priv->tid != NULL);

	/* check for sanity */
	ret = pk_strvalidate (eula_id);
	if (!ret) {
		error = g_error_new (PK_TRANSACTION_ERROR, PK_TRANSACTION_ERROR_INPUT_INVALID,
				     "Invalid input passed to daemon");
		dbus_g_method_return_error (context, error);
		return;
	}

	/* set the dbus name, so we can get the disconnect */
	if (context != NULL) {
		/* not set inside the test suite */
		sender = dbus_g_method_get_sender (context);
		pk_transaction_set_dbus_name (transaction, sender);
		g_free (sender);
	}

	/* check if the action is allowed from this client - if not, set an error */
	ret = pk_transaction_action_is_allowed (transaction, FALSE, PK_ROLE_ENUM_ACCEPT_EULA, &error);
	if (!ret) {
		dbus_g_method_return_error (context, error);
		return;
	}

	egg_debug ("AcceptEula method called: %s", eula_id);
	ret = pk_backend_accept_eula (transaction->priv->backend, eula_id);
	if (!ret) {
		error = g_error_new (PK_TRANSACTION_ERROR, PK_TRANSACTION_ERROR_INPUT_INVALID,
				     "EULA failed to be added");
		dbus_g_method_return_error (context, error);
		return;
	}

	/* we are done */
	g_idle_add ((GSourceFunc) pk_transaction_finished_idle_cb, transaction);

	/* not set inside the test suite */
	if (context != NULL)
		dbus_g_method_return (context);
}

/**
 * pk_transaction_cancel:
 **/
gboolean
pk_transaction_cancel (PkTransaction *transaction, GError **error)
{
	g_return_val_if_fail (PK_IS_TRANSACTION (transaction), FALSE);
	g_return_val_if_fail (transaction->priv->tid != NULL, FALSE);

	egg_debug ("Cancel method called on %s", transaction->priv->tid);

	/* if it's never been run, just remove this transaction from the list */
	if (!transaction->priv->has_been_run) {
		pk_transaction_progress_changed_emit (transaction, 100, 100, 0, 0);
		pk_transaction_allow_cancel_emit (transaction, FALSE);
		pk_transaction_status_changed_emit (transaction, PK_STATUS_ENUM_FINISHED);
		pk_transaction_finished_emit (transaction, PK_EXIT_ENUM_CANCELLED, 0);
		pk_transaction_list_remove (transaction->priv->transaction_list, transaction->priv->tid);
		return TRUE;
	}

	/* if it's finished, cancelling will have no action */
	if (transaction->priv->finished) {
		egg_debug ("No point trying to cancel a finished transaction, ignoring");
		return TRUE;
	}

	/* not implemented yet */
	if (transaction->priv->backend->desc->cancel == NULL) {
		egg_debug ("Not implemented yet: Cancel");
		g_set_error (error, PK_TRANSACTION_ERROR, PK_TRANSACTION_ERROR_NOT_SUPPORTED,
			     "Operation not yet supported by backend");
		return FALSE;
	}

	/* check to see if we have an action */
	if (transaction->priv->role == PK_ROLE_ENUM_UNKNOWN) {
		g_set_error (error, PK_TRANSACTION_ERROR, PK_TRANSACTION_ERROR_NO_ROLE, "No role");
		return FALSE;
	}

	/* check if it's safe to kill */
	if (transaction->priv->allow_cancel == FALSE) {
		g_set_error (error, PK_TRANSACTION_ERROR, PK_TRANSACTION_ERROR_CANNOT_CANCEL,
			     "Tried to cancel a transaction that is not safe to kill");
		return FALSE;
	}

	/* set the state, as cancelling might take a few seconds */
	pk_backend_set_status (transaction->priv->backend, PK_STATUS_ENUM_CANCEL);

	/* we don't want to cancel twice */
	pk_backend_set_allow_cancel (transaction->priv->backend, FALSE);

	/* we need ::finished to not return success or failed */
	pk_backend_set_exit_code (transaction->priv->backend, PK_EXIT_ENUM_CANCELLED);

	/* actually run the method */
	transaction->priv->backend->desc->cancel (transaction->priv->backend);
	return TRUE;
}

/**
 * pk_transaction_download_packages:
 **/
void
pk_transaction_download_packages (PkTransaction *transaction, gchar **package_ids, DBusGMethodInvocation *context)
{
	gboolean ret;
	GError *error;
	gchar *package_ids_temp;
	gchar *directory;
	gint retval;
	gchar *sender;

	g_return_if_fail (PK_IS_TRANSACTION (transaction));
	g_return_if_fail (transaction->priv->tid != NULL);

	egg_debug ("DownloadPackages method called: %s", package_ids[0]);

	/* not implemented yet */
	if (transaction->priv->backend->desc->download_packages == NULL) {
	        error = g_error_new (PK_TRANSACTION_ERROR, PK_TRANSACTION_ERROR_NOT_SUPPORTED,
	                             "Operation not yet supported by backend");
		pk_transaction_list_remove (transaction->priv->transaction_list,
					    transaction->priv->tid);
	        dbus_g_method_return_error (context, error);
	        return;
	}

	/* check package_ids */
	ret = pk_package_ids_check (package_ids);
	if (!ret) {
	        package_ids_temp = pk_package_ids_to_text (package_ids);
	        error = g_error_new (PK_TRANSACTION_ERROR, PK_TRANSACTION_ERROR_PACKAGE_ID_INVALID,
	                             "The package id's '%s' are not valid", package_ids_temp);
	        g_free (package_ids_temp);
	        dbus_g_method_return_error (context, error);
	        return;
	}

	/* create cache directory */
	directory = g_build_filename (LOCALSTATEDIR, "cache", "PackageKit",
				     "downloads", transaction->priv->tid, NULL);
	/* rwxrwxr-x */
	retval = g_mkdir (directory, 0775);
	if (retval != 0) {
	        error = g_error_new (PK_TRANSACTION_ERROR, PK_TRANSACTION_ERROR_DENIED,
	                             "cannot create %s", directory);
	        dbus_g_method_return_error (context, error);
	        return;
	}

	/* set the dbus name, so we can get the disconnect */
	if (context != NULL) {
		/* not set inside the test suite */
		sender = dbus_g_method_get_sender (context);
		pk_transaction_set_dbus_name (transaction, sender);
		g_free (sender);
	}

	/* save so we can run later */
	transaction->priv->cached_package_ids = g_strdupv (package_ids);
	transaction->priv->cached_directory = g_strdup (directory);
	pk_transaction_set_role (transaction, PK_ROLE_ENUM_DOWNLOAD_PACKAGES);

	/* try to commit this */
	ret = pk_transaction_commit (transaction);
	if (!ret) {
	        error = g_error_new (PK_TRANSACTION_ERROR, PK_TRANSACTION_ERROR_COMMIT_FAILED,
	                             "Could not commit to a transaction object");
		pk_transaction_list_remove (transaction->priv->transaction_list,
					    transaction->priv->tid);
	        return;
	}

	g_free (directory);
	/* not set inside the test suite */
	if (context != NULL)
		dbus_g_method_return (context);
}

/**
 * pk_transaction_get_allow_cancel:
 **/
gboolean
pk_transaction_get_allow_cancel (PkTransaction *transaction, gboolean *allow_cancel, GError **error)
{
	g_return_val_if_fail (PK_IS_TRANSACTION (transaction), FALSE);
	g_return_val_if_fail (transaction->priv->tid != NULL, FALSE);

	egg_debug ("GetAllowCancel method called");
	*allow_cancel = transaction->priv->allow_cancel;
	return TRUE;
}

/**
 * pk_transaction_get_categories:
 **/
void
pk_transaction_get_categories (PkTransaction *transaction, DBusGMethodInvocation *context)
{
	gboolean ret;
	GError *error;
	gchar *sender;

	g_return_if_fail (PK_IS_TRANSACTION (transaction));
	g_return_if_fail (transaction->priv->tid != NULL);

	egg_debug ("GetCategories method called");

	/* not implemented yet */
	if (transaction->priv->backend->desc->get_categories == NULL) {
		error = g_error_new (PK_TRANSACTION_ERROR, PK_TRANSACTION_ERROR_NOT_SUPPORTED,
				     "Operation not yet supported by backend");
		pk_transaction_list_remove (transaction->priv->transaction_list,
					    transaction->priv->tid);
		dbus_g_method_return_error (context, error);
		return;
	}

	/* set the dbus name, so we can get the disconnect */
	if (context != NULL) {
		/* not set inside the test suite */
		sender = dbus_g_method_get_sender (context);
		pk_transaction_set_dbus_name (transaction, sender);
		g_free (sender);
	}

	/* are we already performing an update? */
	if (pk_transaction_list_role_present (transaction->priv->transaction_list, PK_ROLE_ENUM_GET_CATEGORIES)) {
		error = g_error_new (PK_TRANSACTION_ERROR, PK_TRANSACTION_ERROR_TRANSACTION_EXISTS_WITH_ROLE,
				     "Already performing get categories");
		dbus_g_method_return_error (context, error);
		return;
	}

	pk_transaction_set_role (transaction, PK_ROLE_ENUM_GET_CATEGORIES);

	/* try to commit this */
	ret = pk_transaction_commit (transaction);
	if (!ret) {
		error = g_error_new (PK_TRANSACTION_ERROR, PK_TRANSACTION_ERROR_COMMIT_FAILED,
				     "Could not commit to a transaction object");
		dbus_g_method_return_error (context, error);
		return;
	}

	dbus_g_method_return (context);
}

/**
 * pk_transaction_get_depends:
 **/
void
pk_transaction_get_depends (PkTransaction *transaction, const gchar *filter, gchar **package_ids,
			    gboolean recursive, DBusGMethodInvocation *context)
{
	gboolean ret;
	GError *error;
	gchar *package_ids_temp;
	gchar *sender;

	g_return_if_fail (PK_IS_TRANSACTION (transaction));
	g_return_if_fail (transaction->priv->tid != NULL);

	package_ids_temp = pk_package_ids_to_text (package_ids);
	egg_debug ("GetDepends method called: %s (recursive %i)", package_ids_temp, recursive);
	g_free (package_ids_temp);

	/* not implemented yet */
	if (transaction->priv->backend->desc->get_depends == NULL) {
		egg_debug ("Not implemented yet: GetDepends");
		error = g_error_new (PK_TRANSACTION_ERROR, PK_TRANSACTION_ERROR_NOT_SUPPORTED,
				     "Operation not yet supported by backend");
		dbus_g_method_return_error (context, error);
		return;
	}

	/* check the filter */
	ret = pk_transaction_filter_check (filter, &error);
	if (!ret) {
		dbus_g_method_return_error (context, error);
		return;
	}

	/* check package_ids */
	ret = pk_package_ids_check (package_ids);
	if (!ret) {
		package_ids_temp = pk_package_ids_to_text (package_ids);
		error = g_error_new (PK_TRANSACTION_ERROR, PK_TRANSACTION_ERROR_PACKAGE_ID_INVALID,
				     "The package id's '%s' are not valid", package_ids_temp);
		g_free (package_ids_temp);
		dbus_g_method_return_error (context, error);
		return;
	}

	/* set the dbus name, so we can get the disconnect */
	if (context != NULL) {
		/* not set inside the test suite */
		sender = dbus_g_method_get_sender (context);
		pk_transaction_set_dbus_name (transaction, sender);
		g_free (sender);
	}

	/* save so we can run later */
	transaction->priv->cached_filters = pk_filter_bitfield_from_text (filter);
	transaction->priv->cached_package_ids = g_strdupv (package_ids);
	transaction->priv->cached_force = recursive;
	pk_transaction_set_role (transaction, PK_ROLE_ENUM_GET_DEPENDS);

	/* try to commit this */
	ret = pk_transaction_commit (transaction);
	if (!ret) {
		error = g_error_new (PK_TRANSACTION_ERROR, PK_TRANSACTION_ERROR_COMMIT_FAILED,
				     "Could not commit to a transaction object");
		dbus_g_method_return_error (context, error);
		return;
	}

	/* not set inside the test suite */
	if (context != NULL)
		dbus_g_method_return (context);
}

/**
 * pk_transaction_get_details:
 **/
void
pk_transaction_get_details (PkTransaction *transaction, gchar **package_ids, DBusGMethodInvocation *context)
{
	gboolean ret;
	GError *error;
	gchar *package_ids_temp;
	gchar *sender;

	g_return_if_fail (PK_IS_TRANSACTION (transaction));
	g_return_if_fail (transaction->priv->tid != NULL);

	package_ids_temp = pk_package_ids_to_text (package_ids);
	egg_debug ("GetDetails method called: %s", package_ids_temp);
	g_free (package_ids_temp);

	/* not implemented yet */
	if (transaction->priv->backend->desc->get_details == NULL) {
		egg_debug ("Not implemented yet: GetDetails");
		error = g_error_new (PK_TRANSACTION_ERROR, PK_TRANSACTION_ERROR_NOT_SUPPORTED,
				     "Operation not yet supported by backend");
		dbus_g_method_return_error (context, error);
		return;
	}

	/* check package_ids */
	ret = pk_package_ids_check (package_ids);
	if (!ret) {
		package_ids_temp = pk_package_ids_to_text (package_ids);
		error = g_error_new (PK_TRANSACTION_ERROR, PK_TRANSACTION_ERROR_PACKAGE_ID_INVALID,
				     "The package id's '%s' are not valid", package_ids_temp);
		g_free (package_ids_temp);
		dbus_g_method_return_error (context, error);
		return;
	}

	/* set the dbus name, so we can get the disconnect */
	if (context != NULL) {
		/* not set inside the test suite */
		sender = dbus_g_method_get_sender (context);
		pk_transaction_set_dbus_name (transaction, sender);
		g_free (sender);
	}

	/* save so we can run later */
	transaction->priv->cached_package_ids = g_strdupv (package_ids);
	pk_transaction_set_role (transaction, PK_ROLE_ENUM_GET_DETAILS);

	/* try to commit this */
	ret = pk_transaction_commit (transaction);
	if (!ret) {
		error = g_error_new (PK_TRANSACTION_ERROR, PK_TRANSACTION_ERROR_COMMIT_FAILED,
				     "Could not commit to a transaction object");
		dbus_g_method_return_error (context, error);
		return;
	}

	/* not set inside the test suite */
	if (context != NULL)
		dbus_g_method_return (context);
}

/**
 * pk_transaction_get_distro_upgrades:
 **/
void
pk_transaction_get_distro_upgrades (PkTransaction *transaction, DBusGMethodInvocation *context)
{
	gboolean ret;
	GError *error;
	gchar *sender;

	g_return_if_fail (PK_IS_TRANSACTION (transaction));
	g_return_if_fail (transaction->priv->tid != NULL);

	egg_debug ("GetDistroUpgrades method called");

	/* not implemented yet */
	if (transaction->priv->backend->desc->get_distro_upgrades == NULL) {
		egg_debug ("Not implemented yet: GetDistroUpgrades");
		error = g_error_new (PK_TRANSACTION_ERROR, PK_TRANSACTION_ERROR_NOT_SUPPORTED,
				     "Operation not yet supported by backend");
		dbus_g_method_return_error (context, error);
		return;
	}

	/* set the dbus name, so we can get the disconnect */
	if (context != NULL) {
		/* not set inside the test suite */
		sender = dbus_g_method_get_sender (context);
		pk_transaction_set_dbus_name (transaction, sender);
		g_free (sender);
	}

	/* save so we can run later */
	pk_transaction_set_role (transaction, PK_ROLE_ENUM_GET_DISTRO_UPGRADES);

	/* try to commit this */
	ret = pk_transaction_commit (transaction);
	if (!ret) {
		error = g_error_new (PK_TRANSACTION_ERROR, PK_TRANSACTION_ERROR_COMMIT_FAILED,
				     "Could not commit to a transaction object");
		dbus_g_method_return_error (context, error);
		return;
	}

	if (context != NULL) {
		/* not set inside the test suite */
		dbus_g_method_return (context);
	}
}

/**
 * pk_transaction_get_files:
 **/
void
pk_transaction_get_files (PkTransaction *transaction, gchar **package_ids, DBusGMethodInvocation *context)
{
	gboolean ret;
	GError *error;
	gchar *package_ids_temp;
	gchar *sender;

	g_return_if_fail (PK_IS_TRANSACTION (transaction));
	g_return_if_fail (transaction->priv->tid != NULL);

	package_ids_temp = pk_package_ids_to_text (package_ids);
	egg_debug ("GetFiles method called: %s", package_ids_temp);
	g_free (package_ids_temp);

	/* not implemented yet */
	if (transaction->priv->backend->desc->get_files == NULL) {
		egg_debug ("Not implemented yet: GetFiles");
		error = g_error_new (PK_TRANSACTION_ERROR, PK_TRANSACTION_ERROR_NOT_SUPPORTED,
				     "Operation not yet supported by backend");
		dbus_g_method_return_error (context, error);
		return;
	}

	/* check package_ids */
	ret = pk_package_ids_check (package_ids);
	if (!ret) {
		package_ids_temp = pk_package_ids_to_text (package_ids);
		error = g_error_new (PK_TRANSACTION_ERROR, PK_TRANSACTION_ERROR_PACKAGE_ID_INVALID,
				     "The package id's '%s' are not valid", package_ids_temp);
		g_free (package_ids_temp);
		dbus_g_method_return_error (context, error);
		return;
	}

	/* set the dbus name, so we can get the disconnect */
	if (context != NULL) {
		/* not set inside the test suite */
		sender = dbus_g_method_get_sender (context);
		pk_transaction_set_dbus_name (transaction, sender);
		g_free (sender);
	}

	/* save so we can run later */
	transaction->priv->cached_package_ids = g_strdupv (package_ids);
	pk_transaction_set_role (transaction, PK_ROLE_ENUM_GET_FILES);

	/* try to commit this */
	ret = pk_transaction_commit (transaction);
	if (!ret) {
		error = g_error_new (PK_TRANSACTION_ERROR, PK_TRANSACTION_ERROR_COMMIT_FAILED,
				     "Could not commit to a transaction object");
		dbus_g_method_return_error (context, error);
		return;
	}

	/* not set inside the test suite */
	if (context != NULL)
		dbus_g_method_return (context);
}

/**
 * pk_transaction_get_packages:
 **/
void
pk_transaction_get_packages (PkTransaction *transaction, const gchar *filter, DBusGMethodInvocation *context)
{
	gboolean ret;
	GError *error;
	gchar *sender;

	g_return_if_fail (PK_IS_TRANSACTION (transaction));
	g_return_if_fail (transaction->priv->tid != NULL);

	egg_debug ("GetPackages method called: %s", filter);

	/* not implemented yet */
	if (transaction->priv->backend->desc->get_packages == NULL) {
		egg_debug ("Not implemented yet: GetPackages");
		error = g_error_new (PK_TRANSACTION_ERROR, PK_TRANSACTION_ERROR_NOT_SUPPORTED,
				     "Operation not yet supported by backend");
		dbus_g_method_return_error (context, error);
		return;
	}

	/* check the filter */
	ret = pk_transaction_filter_check (filter, &error);
	if (!ret) {
		dbus_g_method_return_error (context, error);
		return;
	}

	/* set the dbus name, so we can get the disconnect */
	if (context != NULL) {
		/* not set inside the test suite */
		sender = dbus_g_method_get_sender (context);
		pk_transaction_set_dbus_name (transaction, sender);
		g_free (sender);
	}

	/* save so we can run later */
	transaction->priv->cached_filters = pk_filter_bitfield_from_text (filter);
	pk_transaction_set_role (transaction, PK_ROLE_ENUM_GET_PACKAGES);

	/* try to commit this */
	ret = pk_transaction_commit (transaction);
	if (!ret) {
		error = g_error_new (PK_TRANSACTION_ERROR, PK_TRANSACTION_ERROR_COMMIT_FAILED,
				     "Could not commit to a transaction object");
		dbus_g_method_return_error (context, error);
		return;
	}

	/* not set inside the test suite */
	if (context != NULL)
		dbus_g_method_return (context);
}

/**
 * pk_transaction_get_old_transactions:
 **/
gboolean
pk_transaction_get_old_transactions (PkTransaction *transaction, guint number, GError **error)
{
	g_return_val_if_fail (PK_IS_TRANSACTION (transaction), FALSE);
	g_return_val_if_fail (transaction->priv->tid != NULL, FALSE);

	egg_debug ("GetOldTransactions method called");

	pk_transaction_db_get_list (transaction->priv->transaction_db, number);
	g_idle_add ((GSourceFunc) pk_transaction_finished_idle_cb, transaction);

	return TRUE;
}

/**
 * pk_transaction_get_package_last:
 **/
gboolean
pk_transaction_get_package_last (PkTransaction *transaction, gchar **package_id, GError **error)
{
	g_return_val_if_fail (PK_IS_TRANSACTION (transaction), FALSE);
	g_return_val_if_fail (transaction->priv->tid != NULL, FALSE);

	egg_debug ("GetPackageLast method called");

	if (transaction->priv->last_package_id == NULL) {
		g_set_error (error, PK_TRANSACTION_ERROR, PK_TRANSACTION_ERROR_INVALID_STATE,
			     "No package data available");
		return FALSE;
	}
	*package_id = g_strdup (transaction->priv->last_package_id);
	return TRUE;
}

/**
 * pk_transaction_get_progress:
 **/
gboolean
pk_transaction_get_progress (PkTransaction *transaction,
			     guint *percentage, guint *subpercentage,
			     guint *elapsed, guint *remaining, GError **error)
{
	g_return_val_if_fail (PK_IS_TRANSACTION (transaction), FALSE);
	g_return_val_if_fail (transaction->priv->tid != NULL, FALSE);

	egg_debug ("GetProgress method called, using cached values");
	*percentage = transaction->priv->percentage;
	*subpercentage = transaction->priv->subpercentage;
	*elapsed = transaction->priv->elapsed;
	*remaining = transaction->priv->remaining;

	return TRUE;
}

/**
 * pk_transaction_get_repo_list:
 **/
void
pk_transaction_get_repo_list (PkTransaction *transaction, const gchar *filter, DBusGMethodInvocation *context)
{
	gboolean ret;
	GError *error;
	gchar *sender;

	g_return_if_fail (PK_IS_TRANSACTION (transaction));
	g_return_if_fail (transaction->priv->tid != NULL);

	egg_debug ("GetRepoList method called");

	/* not implemented yet */
	if (transaction->priv->backend->desc->get_repo_list == NULL) {
		egg_debug ("Not implemented yet: GetRepoList");
		error = g_error_new (PK_TRANSACTION_ERROR, PK_TRANSACTION_ERROR_NOT_SUPPORTED,
				     "Operation not yet supported by backend");
		dbus_g_method_return_error (context, error);
		return;
	}

	/* check the filter */
	ret = pk_transaction_filter_check (filter, &error);
	if (!ret) {
		dbus_g_method_return_error (context, error);
		return;
	}

	/* set the dbus name, so we can get the disconnect */
	if (context != NULL) {
		/* not set inside the test suite */
		sender = dbus_g_method_get_sender (context);
		pk_transaction_set_dbus_name (transaction, sender);
		g_free (sender);
	}

	/* save so we can run later */
	transaction->priv->cached_filters = pk_filter_bitfield_from_text (filter);
	pk_transaction_set_role (transaction, PK_ROLE_ENUM_GET_REPO_LIST);

	/* try to commit this */
	ret = pk_transaction_commit (transaction);
	if (!ret) {
		error = g_error_new (PK_TRANSACTION_ERROR, PK_TRANSACTION_ERROR_COMMIT_FAILED,
				     "Could not commit to a transaction object");
		dbus_g_method_return_error (context, error);
		return;
	}

	/* not set inside the test suite */
	if (context != NULL)
		dbus_g_method_return (context);
}

/**
 * pk_transaction_get_requires:
 **/
void
pk_transaction_get_requires (PkTransaction *transaction, const gchar *filter, gchar **package_ids,
			     gboolean recursive, DBusGMethodInvocation *context)
{
	gboolean ret;
	GError *error;
	gchar *package_ids_temp;
	gchar *sender;

	g_return_if_fail (PK_IS_TRANSACTION (transaction));
	g_return_if_fail (transaction->priv->tid != NULL);

	package_ids_temp = pk_package_ids_to_text (package_ids);
	egg_debug ("GetRequires method called: %s (recursive %i)", package_ids_temp, recursive);
	g_free (package_ids_temp);

	/* not implemented yet */
	if (transaction->priv->backend->desc->get_requires == NULL) {
		egg_debug ("Not implemented yet: GetRequires");
		error = g_error_new (PK_TRANSACTION_ERROR, PK_TRANSACTION_ERROR_NOT_SUPPORTED,
				     "Operation not yet supported by backend");
		dbus_g_method_return_error (context, error);
		return;
	}

	/* check the filter */
	ret = pk_transaction_filter_check (filter, &error);
	if (!ret) {
		dbus_g_method_return_error (context, error);
		return;
	}

	/* check package_ids */
	ret = pk_package_ids_check (package_ids);
	if (!ret) {
		package_ids_temp = pk_package_ids_to_text (package_ids);
		error = g_error_new (PK_TRANSACTION_ERROR, PK_TRANSACTION_ERROR_PACKAGE_ID_INVALID,
				     "The package id's '%s' are not valid", package_ids_temp);
		g_free (package_ids_temp);
		dbus_g_method_return_error (context, error);
		return;
	}

	/* set the dbus name, so we can get the disconnect */
	if (context != NULL) {
		/* not set inside the test suite */
		sender = dbus_g_method_get_sender (context);
		pk_transaction_set_dbus_name (transaction, sender);
		g_free (sender);
	}

	/* save so we can run later */
	transaction->priv->cached_filters = pk_filter_bitfield_from_text (filter);
	transaction->priv->cached_package_ids = g_strdupv (package_ids);
	transaction->priv->cached_force = recursive;
	pk_transaction_set_role (transaction, PK_ROLE_ENUM_GET_REQUIRES);

	/* try to commit this */
	ret = pk_transaction_commit (transaction);
	if (!ret) {
		error = g_error_new (PK_TRANSACTION_ERROR, PK_TRANSACTION_ERROR_COMMIT_FAILED,
				     "Could not commit to a transaction object");
		dbus_g_method_return_error (context, error);
		return;
	}

	/* not set inside the test suite */
	if (context != NULL)
		dbus_g_method_return (context);
}

/**
 * pk_transaction_get_role:
 **/
gboolean
pk_transaction_get_role (PkTransaction *transaction,
			 const gchar **role, const gchar **text, GError **error)
{
	gchar *text_temp;

	g_return_val_if_fail (PK_IS_TRANSACTION (transaction), FALSE);

	egg_debug ("GetRole method called");

	/* we might not have this set yet */
	if (transaction->priv->tid == NULL) {
		g_set_error (error, PK_TRANSACTION_ERROR, PK_TRANSACTION_ERROR_NO_SUCH_TRANSACTION, "Role not set");
		return FALSE;
	}

	text_temp = pk_transaction_get_text (transaction);
	*role = g_strdup (pk_role_enum_to_text (transaction->priv->role));
	*text = g_strdup (text_temp);
	g_free (text_temp);
	return TRUE;
}

/**
 * pk_transaction_get_status:
 **/
gboolean
pk_transaction_get_status (PkTransaction *transaction, const gchar **status, GError **error)
{
	g_return_val_if_fail (PK_IS_TRANSACTION (transaction), FALSE);
	g_return_val_if_fail (transaction->priv->tid != NULL, FALSE);

	egg_debug ("GetStatus method called");

	*status = g_strdup (pk_status_enum_to_text (transaction->priv->status));
	return TRUE;
}

/**
 * pk_transaction_get_update_detail:
 **/
void
pk_transaction_get_update_detail (PkTransaction *transaction, gchar **package_ids,
				  DBusGMethodInvocation *context)
{
	gboolean ret;
	GError *error;
	gchar *package_ids_temp;
	gchar *package_id;
	gchar **package_ids_new;
	const PkUpdateDetailObj *detail;
	PkPackageId *id;
	GPtrArray *array;
	guint i;
	guint len;
	gchar *sender;

	g_return_if_fail (PK_IS_TRANSACTION (transaction));
	g_return_if_fail (transaction->priv->tid != NULL);

	package_ids_temp = pk_package_ids_to_text (package_ids);
	egg_debug ("GetUpdateDetail method called: %s", package_ids_temp);
	g_free (package_ids_temp);

	/* need to split the package_ids into new and cached */
	array = g_ptr_array_new ();

	/* not implemented yet */
	if (transaction->priv->backend->desc->get_update_detail == NULL) {
		egg_debug ("Not implemented yet: GetUpdateDetail");
		error = g_error_new (PK_TRANSACTION_ERROR, PK_TRANSACTION_ERROR_NOT_SUPPORTED,
				     "Operation not yet supported by backend");
		dbus_g_method_return_error (context, error);
		return;
	}

	/* check package_ids */
	ret = pk_package_ids_check (package_ids);
	if (!ret) {
		package_ids_temp = pk_package_ids_to_text (package_ids);
		error = g_error_new (PK_TRANSACTION_ERROR, PK_TRANSACTION_ERROR_PACKAGE_ID_INVALID,
				     "The package id's '%s' are not valid", package_ids_temp);
		g_free (package_ids_temp);
		dbus_g_method_return_error (context, error);
		return;
	}

	/* set the dbus name, so we can get the disconnect */
	if (context != NULL) {
		/* not set inside the test suite */
		sender = dbus_g_method_get_sender (context);
		pk_transaction_set_dbus_name (transaction, sender);
		g_free (sender);
	}

	/* save so we can run later */
	transaction->priv->cached_package_ids = g_strdupv (package_ids);
	pk_transaction_set_role (transaction, PK_ROLE_ENUM_GET_UPDATE_DETAIL);

	/* try and reuse cache */
	len = g_strv_length (package_ids);
	for (i=0; i<len; i++) {
		id = pk_package_id_new_from_string (package_ids[i]);
		detail = pk_update_detail_list_get_obj (transaction->priv->update_detail_list, id);
		pk_package_id_free (id);
		if (detail != NULL) {
			gchar *issued;
			gchar *updated;
			const gchar *state_text;
			package_id = pk_package_id_to_string (detail->id);
			issued = pk_iso8601_from_date (detail->issued);
			updated = pk_iso8601_from_date (detail->updated);
			state_text = pk_update_state_enum_to_text (detail->state);

			/* emulate the backend */
			g_signal_emit (transaction, signals [PK_TRANSACTION_UPDATE_DETAIL], 0,
				       package_id, detail->updates, detail->obsoletes,
				       detail->vendor_url, detail->bugzilla_url, detail->cve_url,
				       pk_restart_enum_to_text (detail->restart), detail->update_text,
				       detail->changelog, state_text, issued, updated);
			g_free (issued);
			g_free (updated);
			g_free (package_id);
		} else {
			egg_debug ("not got %s", package_ids[i]);
			g_ptr_array_add (array, g_strdup (package_ids[i]));
		}
	}

	/* if we have nothing to do, i.e. everything was in the cache */
	if (array->len == 0) {
		/* we are done */
		g_idle_add ((GSourceFunc) pk_transaction_finished_idle_cb, transaction);
		goto out;
	}

	/* get the new list */
	egg_debug ("%i more to process", array->len);
	package_ids_new = pk_ptr_array_to_strv (array);

	/* alter list */
	g_strfreev (transaction->priv->cached_package_ids);
	transaction->priv->cached_package_ids = package_ids_new;

	/* try to commit this */
	ret = pk_transaction_commit (transaction);
	if (!ret) {
		error = g_error_new (PK_TRANSACTION_ERROR, PK_TRANSACTION_ERROR_COMMIT_FAILED,
				     "Could not commit to a transaction object");
		dbus_g_method_return_error (context, error);
		return;
	}

out:
	g_ptr_array_foreach (array, (GFunc) g_free, NULL);
	g_ptr_array_free (array, TRUE);
	/* not set inside the test suite */
	if (context != NULL)
		dbus_g_method_return (context);
}

/**
 * pk_transaction_get_updates:
 **/
void
pk_transaction_get_updates (PkTransaction *transaction, const gchar *filter, DBusGMethodInvocation *context)
{
	gboolean ret;
	GError *error;
	PkPackageList *updates_cache;
	gchar *package_id;
	gchar *sender;

	g_return_if_fail (PK_IS_TRANSACTION (transaction));
	g_return_if_fail (transaction->priv->tid != NULL);

	egg_debug ("GetUpdates method called");

	/* not implemented yet */
	if (transaction->priv->backend->desc->get_updates == NULL) {
		egg_debug ("Not implemented yet: GetUpdates");
		error = g_error_new (PK_TRANSACTION_ERROR, PK_TRANSACTION_ERROR_NOT_SUPPORTED,
				     "Operation not yet supported by backend");
		dbus_g_method_return_error (context, error);
		return;
	}

	/* check the filter */
	ret = pk_transaction_filter_check (filter, &error);
	if (!ret) {
		dbus_g_method_return_error (context, error);
		return;
	}

	/* set the dbus name, so we can get the disconnect */
	if (context != NULL) {
		/* not set inside the test suite */
		sender = dbus_g_method_get_sender (context);
		pk_transaction_set_dbus_name (transaction, sender);
		g_free (sender);
	}

	/* save so we can run later */
	transaction->priv->cached_filters = pk_filter_bitfield_from_text (filter);
	pk_transaction_set_role (transaction, PK_ROLE_ENUM_GET_UPDATES);

	/* try and reuse cache */
	updates_cache = pk_cache_get_updates (transaction->priv->cache);
	if (updates_cache != NULL) {
		const PkPackageObj *obj;
		const gchar *info_text;
		guint i;
		guint length;

		length = pk_package_list_get_size (updates_cache);
		egg_debug ("we have cached data (%i) we should use!", length);

		/* emulate the backend */
		for (i=0; i<length; i++) {
			obj = pk_package_list_get_obj (updates_cache, i);
			info_text = pk_info_enum_to_text (obj->info);
			package_id = pk_package_id_to_string (obj->id);
			g_signal_emit (transaction, signals [PK_TRANSACTION_PACKAGE], 0,
				       info_text, package_id, obj->summary);
			g_free (package_id);
		}

		/* we are done */
		g_idle_add ((GSourceFunc) pk_transaction_finished_idle_cb, transaction);

		/* not set inside the test suite */
		if (context != NULL)
			dbus_g_method_return (context);
		return;
	}

	/* try to commit this */
	ret = pk_transaction_commit (transaction);
	if (!ret) {
		error = g_error_new (PK_TRANSACTION_ERROR, PK_TRANSACTION_ERROR_COMMIT_FAILED,
				     "Could not commit to a transaction object");
		dbus_g_method_return_error (context, error);
		return;
	}

	/* not set inside the test suite */
	if (context != NULL)
		dbus_g_method_return (context);
}

/**
 * pk_transaction_install_files:
 **/
void
pk_transaction_install_files (PkTransaction *transaction, gboolean trusted,
			      gchar **full_paths, DBusGMethodInvocation *context)
{
	gchar *full_paths_temp;
	gboolean ret;
	GError *error;
	GError *error_local = NULL;
	PkServicePack *service_pack;
	gchar *sender;
	guint length;
	guint i;

	g_return_if_fail (PK_IS_TRANSACTION (transaction));
	g_return_if_fail (transaction->priv->tid != NULL);

	full_paths_temp = pk_package_ids_to_text (full_paths);
	egg_debug ("InstallFiles method called: %s (trusted %i)", full_paths_temp, trusted);
	g_free (full_paths_temp);

	/* not implemented yet */
	if (transaction->priv->backend->desc->install_files == NULL) {
		error = g_error_new (PK_TRANSACTION_ERROR, PK_TRANSACTION_ERROR_NOT_SUPPORTED,
				     "Operation not yet supported by backend");
		pk_transaction_list_remove (transaction->priv->transaction_list,
					    transaction->priv->tid);
		dbus_g_method_return_error (context, error);
		return;
	}

	/* check all files exists and are valid */
	length = g_strv_length (full_paths);

	for (i=0; i<length; i++) {
		/* exists */
		ret = g_file_test (full_paths[i], G_FILE_TEST_EXISTS);
		if (!ret) {
			error = g_error_new (PK_TRANSACTION_ERROR, PK_TRANSACTION_ERROR_NO_SUCH_FILE,
					     "No such file %s", full_paths[i]);
			dbus_g_method_return_error (context, error);
			return;
		}
		/* valid */
		if (g_str_has_suffix (full_paths[i], ".servicepack")) {
			service_pack = pk_service_pack_new ();
			pk_service_pack_set_filename (service_pack, full_paths[i]);
			ret = pk_service_pack_check_valid (service_pack, &error_local);
			g_object_unref (service_pack);
			if (!ret) {
				error = g_error_new (PK_TRANSACTION_ERROR, PK_TRANSACTION_ERROR_PACK_INVALID, "%s", error_local->message);
				dbus_g_method_return_error (context, error);
				g_error_free (error_local);
				return;
			}
		}
	}

	/* set the dbus name, so we can get the disconnect */
	if (context != NULL) {
		/* not set inside the test suite */
		sender = dbus_g_method_get_sender (context);
		pk_transaction_set_dbus_name (transaction, sender);
		g_free (sender);
	}

	/* check if the action is allowed from this client - if not, set an error */
	ret = pk_transaction_action_is_allowed (transaction, trusted, PK_ROLE_ENUM_INSTALL_FILES, &error);
	if (!ret) {
		dbus_g_method_return_error (context, error);
		return;
	}

	/* save so we can run later */
	transaction->priv->cached_trusted = trusted;
	transaction->priv->cached_full_paths = g_strdupv (full_paths);
	pk_transaction_set_role (transaction, PK_ROLE_ENUM_INSTALL_FILES);

	/* try to commit this */
	ret = pk_transaction_commit (transaction);
	if (!ret) {
		error = g_error_new (PK_TRANSACTION_ERROR, PK_TRANSACTION_ERROR_COMMIT_FAILED,
				     "Could not commit to a transaction object");
		dbus_g_method_return_error (context, error);
		return;
	}

	/* not set inside the test suite */
	if (context != NULL)
		dbus_g_method_return (context);
	return;
}

/**
 * pk_transaction_install_packages:
 **/
void
pk_transaction_install_packages (PkTransaction *transaction, gchar **package_ids,
				 DBusGMethodInvocation *context)
{
	gboolean ret;
	GError *error;
	gchar *sender;
	gchar *package_ids_temp;

	g_return_if_fail (PK_IS_TRANSACTION (transaction));
	g_return_if_fail (transaction->priv->tid != NULL);

	package_ids_temp = pk_package_ids_to_text (package_ids);
	egg_debug ("InstallPackages method called: %s", package_ids_temp);
	g_free (package_ids_temp);

	/* not implemented yet */
	if (transaction->priv->backend->desc->install_packages == NULL) {
		error = g_error_new (PK_TRANSACTION_ERROR, PK_TRANSACTION_ERROR_NOT_SUPPORTED,
				     "Operation not yet supported by backend");
		pk_transaction_list_remove (transaction->priv->transaction_list,
					    transaction->priv->tid);
		dbus_g_method_return_error (context, error);
		return;
	}

	/* check package_ids */
	ret = pk_package_ids_check (package_ids);
	if (!ret) {
		package_ids_temp = pk_package_ids_to_text (package_ids);
		error = g_error_new (PK_TRANSACTION_ERROR, PK_TRANSACTION_ERROR_PACKAGE_ID_INVALID,
				     "The package id's '%s' are not valid", package_ids_temp);
		g_free (package_ids_temp);
		dbus_g_method_return_error (context, error);
		return;
	}

	/* set the dbus name, so we can get the disconnect */
	if (context != NULL) {
		/* not set inside the test suite */
		sender = dbus_g_method_get_sender (context);
		pk_transaction_set_dbus_name (transaction, sender);
		g_free (sender);
	}

	/* check if the action is allowed from this client - if not, set an error */
	ret = pk_transaction_action_is_allowed (transaction, FALSE, PK_ROLE_ENUM_INSTALL_PACKAGES, &error);
	if (!ret) {
		dbus_g_method_return_error (context, error);
		return;
	}

	/* save so we can run later */
	transaction->priv->cached_package_ids = g_strdupv (package_ids);
	pk_transaction_set_role (transaction, PK_ROLE_ENUM_INSTALL_PACKAGES);

	/* try to commit this */
	ret = pk_transaction_commit (transaction);
	if (!ret) {
		error = g_error_new (PK_TRANSACTION_ERROR, PK_TRANSACTION_ERROR_COMMIT_FAILED,
				     "Could not commit to a transaction object");
		dbus_g_method_return_error (context, error);
		return;
	}

	/* not set inside the test suite */
	if (context != NULL)
		dbus_g_method_return (context);
}

/**
 * pk_transaction_install_signature:
 **/
void
pk_transaction_install_signature (PkTransaction *transaction, const gchar *sig_type,
				  const gchar *key_id, const gchar *package_id,
				  DBusGMethodInvocation *context)
{
	gboolean ret;
	GError *error;
	gchar *sender;

	g_return_if_fail (PK_IS_TRANSACTION (transaction));
	g_return_if_fail (transaction->priv->tid != NULL);

	egg_debug ("method called: %s, %s", key_id, package_id);

	/* not implemented yet */
	if (transaction->priv->backend->desc->install_signature == NULL) {
		error = g_error_new (PK_TRANSACTION_ERROR, PK_TRANSACTION_ERROR_NOT_SUPPORTED,
				     "Operation not yet supported by backend");
		pk_transaction_list_remove (transaction->priv->transaction_list,
					    transaction->priv->tid);
		dbus_g_method_return_error (context, error);
		return;
	}

	/* check for sanity */
	ret = pk_strvalidate (key_id);
	if (!ret) {
		error = g_error_new (PK_TRANSACTION_ERROR, PK_TRANSACTION_ERROR_INPUT_INVALID,
				     "Invalid input passed to daemon");
		dbus_g_method_return_error (context, error);
		return;
	}

	/* check package_id */
	ret = pk_package_id_check (package_id);
	if (!ret) {
		error = g_error_new (PK_TRANSACTION_ERROR, PK_TRANSACTION_ERROR_PACKAGE_ID_INVALID,
				     "The package id '%s' is not valid", package_id);
		dbus_g_method_return_error (context, error);
		return;
	}

	/* set the dbus name, so we can get the disconnect */
	if (context != NULL) {
		/* not set inside the test suite */
		sender = dbus_g_method_get_sender (context);
		pk_transaction_set_dbus_name (transaction, sender);
		g_free (sender);
	}

	/* check if the action is allowed from this client - if not, set an error */
	ret = pk_transaction_action_is_allowed (transaction, FALSE, PK_ROLE_ENUM_INSTALL_SIGNATURE, &error);
	if (!ret) {
		dbus_g_method_return_error (context, error);
		return;
	}

	/* save so we can run later */
	transaction->priv->cached_package_id = g_strdup (package_id);
	transaction->priv->cached_key_id = g_strdup (key_id);
	pk_transaction_set_role (transaction, PK_ROLE_ENUM_INSTALL_SIGNATURE);

	/* try to commit this */
	ret = pk_transaction_commit (transaction);
	if (!ret) {
		error = g_error_new (PK_TRANSACTION_ERROR, PK_TRANSACTION_ERROR_COMMIT_FAILED,
				     "Could not commit to a transaction object");
		dbus_g_method_return_error (context, error);
		return;
	}

	/* not set inside the test suite */
	if (context != NULL)
		dbus_g_method_return (context);
}

/**
 * pk_transaction_is_caller_active:
 **/
gboolean
pk_transaction_is_caller_active (PkTransaction *transaction, gboolean *is_active, GError **error)
{
	g_return_val_if_fail (PK_IS_TRANSACTION (transaction), FALSE);
	g_return_val_if_fail (transaction->priv->tid != NULL, FALSE);

	egg_debug ("is caller active");

	*is_active = egg_dbus_monitor_is_connected (transaction->priv->monitor);
	return TRUE;
}

/**
 * pk_transaction_refresh_cache:
 **/
void
pk_transaction_refresh_cache (PkTransaction *transaction, gboolean force, DBusGMethodInvocation *context)
{
	gboolean ret;
	GError *error;
	gchar *sender;

	g_return_if_fail (PK_IS_TRANSACTION (transaction));
	g_return_if_fail (transaction->priv->tid != NULL);

	egg_debug ("RefreshCache method called: %i", force);

	/* not implemented yet */
	if (transaction->priv->backend->desc->refresh_cache == NULL) {
		error = g_error_new (PK_TRANSACTION_ERROR, PK_TRANSACTION_ERROR_NOT_SUPPORTED,
			     "Operation not yet supported by backend");
		pk_transaction_list_remove (transaction->priv->transaction_list,
					    transaction->priv->tid);
		dbus_g_method_return_error (context, error);
		return;
	}

	/* set the dbus name, so we can get the disconnect */
	if (context != NULL) {
		/* not set inside the test suite */
		sender = dbus_g_method_get_sender (context);
		pk_transaction_set_dbus_name (transaction, sender);
		g_free (sender);
	}

	/* check if the action is allowed from this client - if not, set an error */
	ret = pk_transaction_action_is_allowed (transaction, FALSE, PK_ROLE_ENUM_REFRESH_CACHE, &error);
	if (!ret) {
		dbus_g_method_return_error (context, error);
		return;
	}

	/* we unref the update cache if it exists */
	pk_cache_invalidate (transaction->priv->cache);

	/* save so we can run later */
	transaction->priv->cached_force = force;
	pk_transaction_set_role (transaction, PK_ROLE_ENUM_REFRESH_CACHE);

	/* try to commit this */
	ret = pk_transaction_commit (transaction);
	if (!ret) {
		error = g_error_new (PK_TRANSACTION_ERROR, PK_TRANSACTION_ERROR_COMMIT_FAILED,
				     "Could not commit to a transaction object");
		dbus_g_method_return_error (context, error);
		return;
	}

	/* not set inside the test suite */
	if (context != NULL)
		dbus_g_method_return (context);
}

/**
 * pk_transaction_remove_packages:
 **/
void
pk_transaction_remove_packages (PkTransaction *transaction, gchar **package_ids,
			        gboolean allow_deps, gboolean autoremove,
			        DBusGMethodInvocation *context)
{
	gboolean ret;
	GError *error;
	gchar *sender;
	gchar *package_ids_temp;

	g_return_if_fail (PK_IS_TRANSACTION (transaction));
	g_return_if_fail (transaction->priv->tid != NULL);

	package_ids_temp = pk_package_ids_to_text (package_ids);
	egg_debug ("RemovePackages method called: %s, %i, %i", package_ids_temp, allow_deps, autoremove);
	g_free (package_ids_temp);

	/* not implemented yet */
	if (transaction->priv->backend->desc->remove_packages == NULL) {
		error = g_error_new (PK_TRANSACTION_ERROR, PK_TRANSACTION_ERROR_NOT_SUPPORTED,
				     "Operation not yet supported by backend");
		pk_transaction_list_remove (transaction->priv->transaction_list,
					    transaction->priv->tid);
		dbus_g_method_return_error (context, error);
		return;
	}

	/* check package_ids */
	ret = pk_package_ids_check (package_ids);
	if (!ret) {
		package_ids_temp = pk_package_ids_to_text (package_ids);
		error = g_error_new (PK_TRANSACTION_ERROR, PK_TRANSACTION_ERROR_PACKAGE_ID_INVALID,
				     "The package id's '%s' are not valid", package_ids_temp);
		g_free (package_ids_temp);
		dbus_g_method_return_error (context, error);
		return;
	}

	/* set the dbus name, so we can get the disconnect */
	if (context != NULL) {
		/* not set inside the test suite */
		sender = dbus_g_method_get_sender (context);
		pk_transaction_set_dbus_name (transaction, sender);
		g_free (sender);
	}

	/* check if the action is allowed from this client - if not, set an error */
	ret = pk_transaction_action_is_allowed (transaction, FALSE, PK_ROLE_ENUM_REMOVE_PACKAGES, &error);
	if (!ret) {
		dbus_g_method_return_error (context, error);
		return;
	}

	/* save so we can run later */
	transaction->priv->cached_allow_deps = allow_deps;
	transaction->priv->cached_package_ids = g_strdupv (package_ids);
	pk_transaction_set_role (transaction, PK_ROLE_ENUM_REMOVE_PACKAGES);

	/* try to commit this */
	ret = pk_transaction_commit (transaction);
	if (!ret) {
		error = g_error_new (PK_TRANSACTION_ERROR, PK_TRANSACTION_ERROR_COMMIT_FAILED,
				     "Could not commit to a transaction object");
		dbus_g_method_return_error (context, error);
		return;
	}

	/* not set inside the test suite */
	if (context != NULL)
		dbus_g_method_return (context);
}

/**
 * pk_transaction_repo_enable:
 **/
void
pk_transaction_repo_enable (PkTransaction *transaction, const gchar *repo_id, gboolean enabled,
			    DBusGMethodInvocation *context)
{
	gboolean ret;
	GError *error;
	gchar *sender;

	g_return_if_fail (PK_IS_TRANSACTION (transaction));
	g_return_if_fail (transaction->priv->tid != NULL);

	egg_debug ("RepoEnable method called: %s, %i", repo_id, enabled);

	/* not implemented yet */
	if (transaction->priv->backend->desc->repo_enable == NULL) {
		error = g_error_new (PK_TRANSACTION_ERROR, PK_TRANSACTION_ERROR_NOT_SUPPORTED,
				     "Operation not yet supported by backend");
		pk_transaction_list_remove (transaction->priv->transaction_list,
					    transaction->priv->tid);
		dbus_g_method_return_error (context, error);
		return;
	}

	/* check for sanity */
	ret = pk_strvalidate (repo_id);
	if (!ret) {
		error = g_error_new (PK_TRANSACTION_ERROR, PK_TRANSACTION_ERROR_INPUT_INVALID,
				     "Invalid input passed to daemon");
		dbus_g_method_return_error (context, error);
		return;
	}

	/* set the dbus name, so we can get the disconnect */
	if (context != NULL) {
		/* not set inside the test suite */
		sender = dbus_g_method_get_sender (context);
		pk_transaction_set_dbus_name (transaction, sender);
		g_free (sender);
	}

	/* check if the action is allowed from this client - if not, set an error */
	ret = pk_transaction_action_is_allowed (transaction, FALSE, PK_ROLE_ENUM_REPO_ENABLE, &error);
	if (!ret) {
		dbus_g_method_return_error (context, error);
		return;
	}

	/* save so we can run later */
	transaction->priv->cached_repo_id = g_strdup (repo_id);
	transaction->priv->cached_enabled = enabled;
	pk_transaction_set_role (transaction, PK_ROLE_ENUM_REPO_ENABLE);

	/* try to commit this */
	ret = pk_transaction_commit (transaction);
	if (!ret) {
		error = g_error_new (PK_TRANSACTION_ERROR, PK_TRANSACTION_ERROR_COMMIT_FAILED,
				     "Could not commit to a transaction object");
		dbus_g_method_return_error (context, error);
		return;
	}

	/* not set inside the test suite */
	if (context != NULL)
		dbus_g_method_return (context);
}

/**
 * pk_transaction_repo_set_data:
 **/
void
pk_transaction_repo_set_data (PkTransaction *transaction, const gchar *repo_id,
			      const gchar *parameter, const gchar *value,
		              DBusGMethodInvocation *context)
{
	gboolean ret;
	GError *error;
	gchar *sender;

	g_return_if_fail (PK_IS_TRANSACTION (transaction));
	g_return_if_fail (transaction->priv->tid != NULL);

	egg_debug ("RepoSetData method called: %s, %s, %s", repo_id, parameter, value);

	/* not implemented yet */
	if (transaction->priv->backend->desc->repo_set_data == NULL) {
		error = g_error_new (PK_TRANSACTION_ERROR, PK_TRANSACTION_ERROR_NOT_SUPPORTED,
				     "Operation not yet supported by backend");
		pk_transaction_list_remove (transaction->priv->transaction_list,
					    transaction->priv->tid);
		dbus_g_method_return_error (context, error);
		return;
	}

	/* check for sanity */
	ret = pk_strvalidate (repo_id);
	if (!ret) {
		error = g_error_new (PK_TRANSACTION_ERROR, PK_TRANSACTION_ERROR_INPUT_INVALID,
				     "Invalid input passed to daemon");
		dbus_g_method_return_error (context, error);
		return;
	}

	/* set the dbus name, so we can get the disconnect */
	if (context != NULL) {
		/* not set inside the test suite */
		sender = dbus_g_method_get_sender (context);
		pk_transaction_set_dbus_name (transaction, sender);
		g_free (sender);
	}

	/* check if the action is allowed from this client - if not, set an error */
	ret = pk_transaction_action_is_allowed (transaction, FALSE, PK_ROLE_ENUM_REPO_SET_DATA, &error);
	if (!ret) {
		dbus_g_method_return_error (context, error);
		return;
	}

	/* save so we can run later */
	transaction->priv->cached_repo_id = g_strdup (repo_id);
	transaction->priv->cached_parameter = g_strdup (parameter);
	transaction->priv->cached_value = g_strdup (value);
	pk_transaction_set_role (transaction, PK_ROLE_ENUM_REPO_SET_DATA);

	/* try to commit this */
	ret = pk_transaction_commit (transaction);
	if (!ret) {
		error = g_error_new (PK_TRANSACTION_ERROR, PK_TRANSACTION_ERROR_COMMIT_FAILED,
				     "Could not commit to a transaction object");
		dbus_g_method_return_error (context, error);
		return;
	}

	/* not set inside the test suite */
	if (context != NULL)
		dbus_g_method_return (context);
}

/**
 * pk_transaction_resolve:
 **/
void
pk_transaction_resolve (PkTransaction *transaction, const gchar *filter,
			gchar **packages, DBusGMethodInvocation *context)
{
	gboolean ret;
	GError *error;
	gchar *packages_temp;
	guint i;
	guint length;
	gchar *sender;

	g_return_if_fail (PK_IS_TRANSACTION (transaction));
	g_return_if_fail (transaction->priv->tid != NULL);

	packages_temp = pk_package_ids_to_text (packages);
	egg_debug ("Resolve method called: %s, %s", filter, packages_temp);
	g_free (packages_temp);

	/* not implemented yet */
	if (transaction->priv->backend->desc->resolve == NULL) {
		egg_debug ("Not implemented yet: Resolve");
		error = g_error_new (PK_TRANSACTION_ERROR, PK_TRANSACTION_ERROR_NOT_SUPPORTED,
				     "Operation not yet supported by backend");
		dbus_g_method_return_error (context, error);
		return;
	}

	/* check the filter */
	ret = pk_transaction_filter_check (filter, &error);
	if (!ret) {
		dbus_g_method_return_error (context, error);
		return;
	}

	/* check for sanity */
	length = g_strv_length (packages);
	for (i=0; i<length; i++) {
		ret = pk_strvalidate (packages[i]);
		if (!ret) {
			error = g_error_new (PK_TRANSACTION_ERROR, PK_TRANSACTION_ERROR_INPUT_INVALID,
					     "Invalid input passed to daemon");
			dbus_g_method_return_error (context, error);
			return;
		}
	}

	/* set the dbus name, so we can get the disconnect */
	if (context != NULL) {
		/* not set inside the test suite */
		sender = dbus_g_method_get_sender (context);
		pk_transaction_set_dbus_name (transaction, sender);
		g_free (sender);
	}

	/* save so we can run later */
	transaction->priv->cached_package_ids = g_strdupv (packages);
	transaction->priv->cached_filters = pk_filter_bitfield_from_text (filter);
	pk_transaction_set_role (transaction, PK_ROLE_ENUM_RESOLVE);

	/* try to commit this */
	ret = pk_transaction_commit (transaction);
	if (!ret) {
		error = g_error_new (PK_TRANSACTION_ERROR, PK_TRANSACTION_ERROR_COMMIT_FAILED,
				     "Could not commit to a transaction object");
		dbus_g_method_return_error (context, error);
		return;
	}

	/* not set inside the test suite */
	if (context != NULL)
		dbus_g_method_return (context);
}

/**
 * pk_transaction_rollback:
 **/
void
pk_transaction_rollback (PkTransaction *transaction, const gchar *transaction_id,
			 DBusGMethodInvocation *context)
{
	gboolean ret;
	GError *error;
	gchar *sender;

	g_return_if_fail (PK_IS_TRANSACTION (transaction));
	g_return_if_fail (transaction->priv->tid != NULL);

	egg_debug ("Rollback method called: %s", transaction_id);

	/* not implemented yet */
	if (transaction->priv->backend->desc->rollback == NULL) {
		error = g_error_new (PK_TRANSACTION_ERROR, PK_TRANSACTION_ERROR_NOT_SUPPORTED,
				     "Operation not yet supported by backend");
		pk_transaction_list_remove (transaction->priv->transaction_list,
					    transaction->priv->tid);
		dbus_g_method_return_error (context, error);
		return;
	}

	/* check for sanity */
	ret = pk_strvalidate (transaction_id);
	if (!ret) {
		error = g_error_new (PK_TRANSACTION_ERROR, PK_TRANSACTION_ERROR_INPUT_INVALID,
				     "Invalid input passed to daemon");
		dbus_g_method_return_error (context, error);
		return;
	}

	/* set the dbus name, so we can get the disconnect */
	if (context != NULL) {
		/* not set inside the test suite */
		sender = dbus_g_method_get_sender (context);
		pk_transaction_set_dbus_name (transaction, sender);
		g_free (sender);
	}

	/* check if the action is allowed from this client - if not, set an error */
	ret = pk_transaction_action_is_allowed (transaction, FALSE, PK_ROLE_ENUM_ROLLBACK, &error);
	if (!ret) {
		dbus_g_method_return_error (context, error);
		return;
	}

	/* save so we can run later */
	transaction->priv->cached_transaction_id = g_strdup (transaction_id);
	pk_transaction_set_role (transaction, PK_ROLE_ENUM_ROLLBACK);

	/* try to commit this */
	ret = pk_transaction_commit (transaction);
	if (!ret) {
		error = g_error_new (PK_TRANSACTION_ERROR, PK_TRANSACTION_ERROR_COMMIT_FAILED,
				     "Could not commit to a transaction object");
		dbus_g_method_return_error (context, error);
		return;
	}

	/* not set inside the test suite */
	if (context != NULL)
		dbus_g_method_return (context);
}

/**
 * pk_transaction_search_details:
 **/
void
pk_transaction_search_details (PkTransaction *transaction, const gchar *filter,
			       const gchar *search, DBusGMethodInvocation *context)
{
	gboolean ret;
	GError *error;
	gchar *sender;

	g_return_if_fail (PK_IS_TRANSACTION (transaction));
	g_return_if_fail (transaction->priv->tid != NULL);

	egg_debug ("SearchDetails method called: %s, %s", filter, search);

	/* not implemented yet */
	if (transaction->priv->backend->desc->search_details == NULL) {
		egg_debug ("Not implemented yet: SearchDetails");
		error = g_error_new (PK_TRANSACTION_ERROR, PK_TRANSACTION_ERROR_NOT_SUPPORTED,
				     "Operation not yet supported by backend");
		dbus_g_method_return_error (context, error);
		return;
	}

	/* check the search term */
	ret = pk_transaction_search_check (search, &error);
	if (!ret) {
		dbus_g_method_return_error (context, error);
		return;
	}

	/* check the filter */
	ret = pk_transaction_filter_check (filter, &error);
	if (!ret) {
		dbus_g_method_return_error (context, error);
		return;
	}

	/* set the dbus name, so we can get the disconnect */
	if (context != NULL) {
		/* not set inside the test suite */
		sender = dbus_g_method_get_sender (context);
		pk_transaction_set_dbus_name (transaction, sender);
		g_free (sender);
	}

	/* save so we can run later */
	transaction->priv->cached_filters = pk_filter_bitfield_from_text (filter);
	transaction->priv->cached_search = g_strdup (search);
	pk_transaction_set_role (transaction, PK_ROLE_ENUM_SEARCH_DETAILS);

	/* try to commit this */
	ret = pk_transaction_commit (transaction);
	if (!ret) {
		error = g_error_new (PK_TRANSACTION_ERROR, PK_TRANSACTION_ERROR_COMMIT_FAILED,
				     "Could not commit to a transaction object");
		dbus_g_method_return_error (context, error);
		return;
	}

	/* not set inside the test suite */
	if (context != NULL)
		dbus_g_method_return (context);
}

/**
 * pk_transaction_search_file:
 **/
void
pk_transaction_search_file (PkTransaction *transaction, const gchar *filter,
		   	    const gchar *search, DBusGMethodInvocation *context)
{
	gboolean ret;
	GError *error;
	gchar *sender;

	g_return_if_fail (PK_IS_TRANSACTION (transaction));
	g_return_if_fail (transaction->priv->tid != NULL);

	egg_debug ("SearchFile method called: %s, %s", filter, search);

	/* not implemented yet */
	if (transaction->priv->backend->desc->search_file == NULL) {
		egg_debug ("Not implemented yet: SearchFile");
		error = g_error_new (PK_TRANSACTION_ERROR, PK_TRANSACTION_ERROR_NOT_SUPPORTED,
				     "Operation not yet supported by backend");
		dbus_g_method_return_error (context, error);
		return;
	}

	/* check the search term */
	ret = pk_transaction_search_check (search, &error);
	if (!ret) {
		dbus_g_method_return_error (context, error);
		return;
	}

	/* check the filter */
	ret = pk_transaction_filter_check (filter, &error);
	if (!ret) {
		dbus_g_method_return_error (context, error);
		return;
	}

	/* set the dbus name, so we can get the disconnect */
	if (context != NULL) {
		/* not set inside the test suite */
		sender = dbus_g_method_get_sender (context);
		pk_transaction_set_dbus_name (transaction, sender);
		g_free (sender);
	}

	/* save so we can run later */
	transaction->priv->cached_filters = pk_filter_bitfield_from_text (filter);
	transaction->priv->cached_search = g_strdup (search);
	pk_transaction_set_role (transaction, PK_ROLE_ENUM_SEARCH_FILE);

	/* try to commit this */
	ret = pk_transaction_commit (transaction);
	if (!ret) {
		error = g_error_new (PK_TRANSACTION_ERROR, PK_TRANSACTION_ERROR_COMMIT_FAILED,
				     "Could not commit to a transaction object");
		dbus_g_method_return_error (context, error);
		return;
	}

	/* not set inside the test suite */
	if (context != NULL)
		dbus_g_method_return (context);
}

/**
 * pk_transaction_search_group:
 **/
void
pk_transaction_search_group (PkTransaction *transaction, const gchar *filter,
			     const gchar *search, DBusGMethodInvocation *context)
{
	gboolean ret;
	GError *error;
	gchar *sender;

	g_return_if_fail (PK_IS_TRANSACTION (transaction));
	g_return_if_fail (transaction->priv->tid != NULL);

	egg_debug ("SearchGroup method called: %s, %s", filter, search);

	/* not implemented yet */
	if (transaction->priv->backend->desc->search_group == NULL) {
		egg_debug ("Not implemented yet: SearchGroup");
		error = g_error_new (PK_TRANSACTION_ERROR, PK_TRANSACTION_ERROR_NOT_SUPPORTED,
				     "Operation not yet supported by backend");
		dbus_g_method_return_error (context, error);
		return;
	}

	/* check the search term */
	ret = pk_transaction_search_check (search, &error);
	if (!ret) {
		dbus_g_method_return_error (context, error);
		return;
	}

	/* check the filter */
	ret = pk_transaction_filter_check (filter, &error);
	if (!ret) {
		dbus_g_method_return_error (context, error);
		return;
	}

	/* set the dbus name, so we can get the disconnect */
	if (context != NULL) {
		/* not set inside the test suite */
		sender = dbus_g_method_get_sender (context);
		pk_transaction_set_dbus_name (transaction, sender);
		g_free (sender);
	}

	/* save so we can run later */
	transaction->priv->cached_filters = pk_filter_bitfield_from_text (filter);
	transaction->priv->cached_search = g_strdup (search);
	pk_transaction_set_role (transaction, PK_ROLE_ENUM_SEARCH_GROUP);

	/* try to commit this */
	ret = pk_transaction_commit (transaction);
	if (!ret) {
		error = g_error_new (PK_TRANSACTION_ERROR, PK_TRANSACTION_ERROR_COMMIT_FAILED,
				     "Could not commit to a transaction object");
		dbus_g_method_return_error (context, error);
		return;
	}

	/* not set inside the test suite */
	if (context != NULL)
		dbus_g_method_return (context);
}

/**
 * pk_transaction_search_name:
 **/
void
pk_transaction_search_name (PkTransaction *transaction, const gchar *filter,
		  	    const gchar *search, DBusGMethodInvocation *context)
{
	gboolean ret;
	GError *error;
	gchar *sender;

	g_return_if_fail (PK_IS_TRANSACTION (transaction));
	g_return_if_fail (transaction->priv->tid != NULL);

	egg_debug ("SearchName method called: %s, %s", filter, search);

	/* not implemented yet */
	if (transaction->priv->backend->desc->search_name == NULL) {
		egg_debug ("Not implemented yet: SearchName");
		error = g_error_new (PK_TRANSACTION_ERROR, PK_TRANSACTION_ERROR_NOT_SUPPORTED,
				     "Operation not yet supported by backend");
		dbus_g_method_return_error (context, error);
		return;
	}

	/* check the search term */
	ret = pk_transaction_search_check (search, &error);
	if (!ret) {
		dbus_g_method_return_error (context, error);
		return;
	}

	/* check the filter */
	ret = pk_transaction_filter_check (filter, &error);
	if (!ret) {
		dbus_g_method_return_error (context, error);
		return;
	}

	/* set the dbus name, so we can get the disconnect */
	if (context != NULL) {
		/* not set inside the test suite */
		sender = dbus_g_method_get_sender (context);
		pk_transaction_set_dbus_name (transaction, sender);
		g_free (sender);
	}

	/* save so we can run later */
	transaction->priv->cached_filters = pk_filter_bitfield_from_text (filter);
	transaction->priv->cached_search = g_strdup (search);
	pk_transaction_set_role (transaction, PK_ROLE_ENUM_SEARCH_NAME);

	/* try to commit this */
	ret = pk_transaction_commit (transaction);
	if (!ret) {
		error = g_error_new (PK_TRANSACTION_ERROR, PK_TRANSACTION_ERROR_COMMIT_FAILED,
				     "Could not commit to a transaction object");
		dbus_g_method_return_error (context, error);
		return;
	}

	/* not set inside the test suite */
	if (context != NULL)
		dbus_g_method_return (context);
}

/**
 * pk_transaction_set_locale:
 */
gboolean
pk_transaction_set_locale (PkTransaction *transaction, const gchar *code, GError **error)
{
	g_return_val_if_fail (PK_IS_TRANSACTION (transaction), FALSE);
	g_return_val_if_fail (transaction->priv->tid != NULL, FALSE);

	/* already set? */
	if (transaction->priv->locale != NULL) {
		egg_warning ("Already set locale");
		g_set_error (error, PK_TRANSACTION_ERROR, PK_TRANSACTION_ERROR_NOT_SUPPORTED,
			     "Already set locale to %s", transaction->priv->locale);
		return FALSE;
	}

	/* save so we can pass to the backend */
	transaction->priv->locale = g_strdup (code);
	return TRUE;
}

/**
 * pk_transaction_update_packages:
 **/
void
pk_transaction_update_packages (PkTransaction *transaction, gchar **package_ids, DBusGMethodInvocation *context)
{
	gboolean ret;
	GError *error;
	gchar *sender;
	gchar *package_ids_temp;

	g_return_if_fail (PK_IS_TRANSACTION (transaction));
	g_return_if_fail (transaction->priv->tid != NULL);

	package_ids_temp = pk_package_ids_to_text (package_ids);
	egg_debug ("UpdatePackages method called: %s", package_ids_temp);
	g_free (package_ids_temp);

	/* not implemented yet */
	if (transaction->priv->backend->desc->update_packages == NULL) {
		error = g_error_new (PK_TRANSACTION_ERROR, PK_TRANSACTION_ERROR_NOT_SUPPORTED,
				     "Operation not yet supported by backend");
		pk_transaction_list_remove (transaction->priv->transaction_list,
					    transaction->priv->tid);
		dbus_g_method_return_error (context, error);
		return;
	}

	/* check package_ids */
	ret = pk_package_ids_check (package_ids);
	if (!ret) {
		package_ids_temp = pk_package_ids_to_text (package_ids);
		error = g_error_new (PK_TRANSACTION_ERROR, PK_TRANSACTION_ERROR_PACKAGE_ID_INVALID,
				     "The package id's '%s' are not valid", package_ids_temp);
		g_free (package_ids_temp);
		dbus_g_method_return_error (context, error);
		return;
	}

	/* set the dbus name, so we can get the disconnect */
	if (context != NULL) {
		/* not set inside the test suite */
		sender = dbus_g_method_get_sender (context);
		pk_transaction_set_dbus_name (transaction, sender);
		g_free (sender);
	}

	/* check if the action is allowed from this client - if not, set an error */
	ret = pk_transaction_action_is_allowed (transaction, FALSE, PK_ROLE_ENUM_UPDATE_PACKAGES, &error);
	if (!ret) {
		dbus_g_method_return_error (context, error);
		return;
	}

	/* save so we can run later */
	transaction->priv->cached_package_ids = g_strdupv (package_ids);
	pk_transaction_set_role (transaction, PK_ROLE_ENUM_UPDATE_PACKAGES);

	/* try to commit this */
	ret = pk_transaction_commit (transaction);
	if (!ret) {
		error = g_error_new (PK_TRANSACTION_ERROR, PK_TRANSACTION_ERROR_COMMIT_FAILED,
				     "Could not commit to a transaction object");
		dbus_g_method_return_error (context, error);
		return;
	}

	/* not set inside the test suite */
	if (context != NULL)
		dbus_g_method_return (context);
}

/**
 * pk_transaction_update_system:
 **/
void
pk_transaction_update_system (PkTransaction *transaction, DBusGMethodInvocation *context)
{
	gboolean ret;
	GError *error;
	gchar *sender;

	g_return_if_fail (PK_IS_TRANSACTION (transaction));
	g_return_if_fail (transaction->priv->tid != NULL);

	egg_debug ("UpdateSystem method called");

	/* not implemented yet */
	if (transaction->priv->backend->desc->update_system == NULL) {
		error = g_error_new (PK_TRANSACTION_ERROR, PK_TRANSACTION_ERROR_NOT_SUPPORTED,
				     "Operation not yet supported by backend");
		pk_transaction_list_remove (transaction->priv->transaction_list,
					    transaction->priv->tid);
		dbus_g_method_return_error (context, error);
		return;
	}

	/* set the dbus name, so we can get the disconnect */
	if (context != NULL) {
		/* not set inside the test suite */
		sender = dbus_g_method_get_sender (context);
		pk_transaction_set_dbus_name (transaction, sender);
		g_free (sender);
	}

	/* check if the action is allowed from this client - if not, set an error */
	ret = pk_transaction_action_is_allowed (transaction, FALSE, PK_ROLE_ENUM_UPDATE_SYSTEM, &error);
	if (!ret) {
		dbus_g_method_return_error (context, error);
		return;
	}

	/* are we already performing an update? */
	if (pk_transaction_list_role_present (transaction->priv->transaction_list, PK_ROLE_ENUM_UPDATE_SYSTEM)) {
		error = g_error_new (PK_TRANSACTION_ERROR, PK_TRANSACTION_ERROR_TRANSACTION_EXISTS_WITH_ROLE,
				     "Already performing system update");
		dbus_g_method_return_error (context, error);
		return;
	}

	pk_transaction_set_role (transaction, PK_ROLE_ENUM_UPDATE_SYSTEM);

	/* try to commit this */
	ret = pk_transaction_commit (transaction);
	if (!ret) {
		error = g_error_new (PK_TRANSACTION_ERROR, PK_TRANSACTION_ERROR_COMMIT_FAILED,
				     "Could not commit to a transaction object");
		dbus_g_method_return_error (context, error);
		return;
	}

	/* not set inside the test suite */
	if (context != NULL)
		dbus_g_method_return (context);
}

/**
 * pk_transaction_what_provides:
 **/
void
pk_transaction_what_provides (PkTransaction *transaction, const gchar *filter, const gchar *type,
			      const gchar *search, DBusGMethodInvocation *context)
{
	gboolean ret;
	PkProvidesEnum provides;
	GError *error;
	gchar *sender;

	g_return_if_fail (PK_IS_TRANSACTION (transaction));
	g_return_if_fail (transaction->priv->tid != NULL);

	egg_debug ("WhatProvides method called: %s, %s", type, search);

	/* not implemented yet */
	if (transaction->priv->backend->desc->what_provides == NULL) {
		egg_debug ("Not implemented yet: WhatProvides");
		error = g_error_new (PK_TRANSACTION_ERROR, PK_TRANSACTION_ERROR_NOT_SUPPORTED,
				     "Operation not yet supported by backend");
		dbus_g_method_return_error (context, error);
		return;
	}

	/* check the search term */
	ret = pk_transaction_search_check (search, &error);
	if (!ret) {
		dbus_g_method_return_error (context, error);
		return;
	}

	/* check the filter */
	ret = pk_transaction_filter_check (filter, &error);
	if (!ret) {
		dbus_g_method_return_error (context, error);
		return;
	}

	provides = pk_provides_enum_from_text (type);
	if (provides == PK_PROVIDES_ENUM_UNKNOWN) {
		error = g_error_new (PK_TRANSACTION_ERROR, PK_TRANSACTION_ERROR_INVALID_PROVIDE,
				     "provide type '%s' not found", type);
		dbus_g_method_return_error (context, error);
		return;
	}

	/* set the dbus name, so we can get the disconnect */
	if (context != NULL) {
		/* not set inside the test suite */
		sender = dbus_g_method_get_sender (context);
		pk_transaction_set_dbus_name (transaction, sender);
		g_free (sender);
	}

	/* save so we can run later */
	transaction->priv->cached_filters = pk_filter_bitfield_from_text (filter);
	transaction->priv->cached_search = g_strdup (search);
	transaction->priv->cached_provides = provides;
	pk_transaction_set_role (transaction, PK_ROLE_ENUM_WHAT_PROVIDES);

	/* try to commit this */
	ret = pk_transaction_commit (transaction);
	if (!ret) {
		error = g_error_new (PK_TRANSACTION_ERROR, PK_TRANSACTION_ERROR_COMMIT_FAILED,
				     "Could not commit to a transaction object");
		dbus_g_method_return_error (context, error);
		return;
	}

	/* not set inside the test suite */
	if (context != NULL)
		dbus_g_method_return (context);
}

/**
 * pk_transaction_class_init:
 * @klass: The PkTransactionClass
 **/
static void
pk_transaction_class_init (PkTransactionClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	object_class->finalize = pk_transaction_finalize;

	signals [PK_TRANSACTION_ALLOW_CANCEL] =
		g_signal_new ("allow-cancel",
			      G_TYPE_FROM_CLASS (object_class), G_SIGNAL_RUN_LAST,
			      0, NULL, NULL, g_cclosure_marshal_VOID__BOOLEAN,
			      G_TYPE_NONE, 1, G_TYPE_BOOLEAN);
	signals [PK_TRANSACTION_CALLER_ACTIVE_CHANGED] =
		g_signal_new ("caller-active-changed",
			      G_TYPE_FROM_CLASS (object_class), G_SIGNAL_RUN_LAST,
			      0, NULL, NULL, g_cclosure_marshal_VOID__BOOLEAN,
			      G_TYPE_NONE, 1, G_TYPE_BOOLEAN);
	signals [PK_TRANSACTION_DETAILS] =
		g_signal_new ("details",
			      G_TYPE_FROM_CLASS (object_class), G_SIGNAL_RUN_LAST,
			      0, NULL, NULL, pk_marshal_VOID__STRING_STRING_STRING_STRING_STRING_UINT64,
			      G_TYPE_NONE, 6, G_TYPE_STRING,
			      G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_UINT64);
	signals [PK_TRANSACTION_ERROR_CODE] =
		g_signal_new ("error-code",
			      G_TYPE_FROM_CLASS (object_class), G_SIGNAL_RUN_LAST,
			      0, NULL, NULL, pk_marshal_VOID__STRING_STRING,
			      G_TYPE_NONE, 2, G_TYPE_STRING, G_TYPE_STRING);
	signals [PK_TRANSACTION_FILES] =
		g_signal_new ("files",
			      G_TYPE_FROM_CLASS (object_class), G_SIGNAL_RUN_LAST,
			      0, NULL, NULL, pk_marshal_VOID__STRING_STRING,
			      G_TYPE_NONE, 2, G_TYPE_STRING, G_TYPE_STRING);
	signals [PK_TRANSACTION_CATEGORY] =
		g_signal_new ("category",
			      G_TYPE_FROM_CLASS (object_class), G_SIGNAL_RUN_LAST,
			      0, NULL, NULL, pk_marshal_VOID__STRING_STRING_STRING_STRING_STRING,
			      G_TYPE_NONE, 5, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
	signals [PK_TRANSACTION_DISTRO_UPGRADE] =
		g_signal_new ("distro-upgrade",
			      G_TYPE_FROM_CLASS (object_class), G_SIGNAL_RUN_LAST,
			      0, NULL, NULL, pk_marshal_VOID__STRING_STRING_STRING,
			      G_TYPE_NONE, 3, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
	signals [PK_TRANSACTION_FINISHED] =
		g_signal_new ("finished",
			      G_TYPE_FROM_CLASS (object_class), G_SIGNAL_RUN_LAST,
			      0, NULL, NULL, pk_marshal_VOID__STRING_UINT,
			      G_TYPE_NONE, 2, G_TYPE_STRING, G_TYPE_UINT);
	signals [PK_TRANSACTION_MESSAGE] =
		g_signal_new ("message",
			      G_TYPE_FROM_CLASS (object_class), G_SIGNAL_RUN_LAST,
			      0, NULL, NULL, pk_marshal_VOID__STRING_STRING,
			      G_TYPE_NONE, 2, G_TYPE_STRING, G_TYPE_STRING);
	signals [PK_TRANSACTION_PACKAGE] =
		g_signal_new ("package",
			      G_TYPE_FROM_CLASS (object_class), G_SIGNAL_RUN_LAST,
			      0, NULL, NULL, pk_marshal_VOID__STRING_STRING_STRING,
			      G_TYPE_NONE, 3, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
	signals [PK_TRANSACTION_PROGRESS_CHANGED] =
		g_signal_new ("progress-changed",
			      G_TYPE_FROM_CLASS (object_class), G_SIGNAL_RUN_LAST,
			      0, NULL, NULL, pk_marshal_VOID__UINT_UINT_UINT_UINT,
			      G_TYPE_NONE, 4, G_TYPE_UINT, G_TYPE_UINT, G_TYPE_UINT, G_TYPE_UINT);
	signals [PK_TRANSACTION_REPO_DETAIL] =
		g_signal_new ("repo-detail",
			      G_TYPE_FROM_CLASS (object_class), G_SIGNAL_RUN_LAST,
			      0, NULL, NULL, pk_marshal_VOID__STRING_STRING_BOOL,
			      G_TYPE_NONE, 3, G_TYPE_STRING,
			      G_TYPE_STRING, G_TYPE_BOOLEAN);
	signals [PK_TRANSACTION_REPO_SIGNATURE_REQUIRED] =
		g_signal_new ("repo-signature-required",
			      G_TYPE_FROM_CLASS (object_class), G_SIGNAL_RUN_LAST,
			      0, NULL, NULL, pk_marshal_VOID__STRING_STRING_STRING_STRING_STRING_STRING_STRING_STRING,
			      G_TYPE_NONE, 8, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
			      G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
	signals [PK_TRANSACTION_EULA_REQUIRED] =
		g_signal_new ("eula-required",
			      G_TYPE_FROM_CLASS (object_class), G_SIGNAL_RUN_LAST,
			      0, NULL, NULL, pk_marshal_VOID__STRING_STRING_STRING_STRING,
			      G_TYPE_NONE, 4, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
	signals [PK_TRANSACTION_REQUIRE_RESTART] =
		g_signal_new ("require-restart",
			      G_TYPE_FROM_CLASS (object_class), G_SIGNAL_RUN_LAST,
			      0, NULL, NULL, pk_marshal_VOID__STRING_STRING,
			      G_TYPE_NONE, 2, G_TYPE_STRING, G_TYPE_STRING);
	signals [PK_TRANSACTION_STATUS_CHANGED] =
		g_signal_new ("status-changed",
			      G_TYPE_FROM_CLASS (object_class), G_SIGNAL_RUN_LAST,
			      0, NULL, NULL, g_cclosure_marshal_VOID__STRING,
			      G_TYPE_NONE, 1, G_TYPE_STRING);
	signals [PK_TRANSACTION_TRANSACTION] =
		g_signal_new ("transaction",
			      G_TYPE_FROM_CLASS (object_class), G_SIGNAL_RUN_LAST,
			      0, NULL, NULL, pk_marshal_VOID__STRING_STRING_BOOL_STRING_UINT_STRING,
			      G_TYPE_NONE, 6, G_TYPE_STRING,
			      G_TYPE_STRING, G_TYPE_BOOLEAN, G_TYPE_STRING, G_TYPE_UINT, G_TYPE_STRING);
	signals [PK_TRANSACTION_UPDATE_DETAIL] =
		g_signal_new ("update-detail",
			      G_TYPE_FROM_CLASS (object_class), G_SIGNAL_RUN_LAST,
			      0, NULL, NULL, pk_marshal_VOID__STRING_STRING_STRING_STRING_STRING_STRING_STRING_STRING_STRING_STRING_STRING_STRING,
			      G_TYPE_NONE, 12, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
			      G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
			      G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
	signals [PK_TRANSACTION_DESTROY] =
		g_signal_new ("destroy",
			      G_TYPE_FROM_CLASS (object_class), G_SIGNAL_RUN_LAST,
			      0, NULL, NULL, g_cclosure_marshal_VOID__VOID, G_TYPE_NONE, 0);

	g_type_class_add_private (klass, sizeof (PkTransactionPrivate));
}

/**
 * pk_transaction_init:
 * @transaction: This class instance
 **/
static void
pk_transaction_init (PkTransaction *transaction)
{
	transaction->priv = PK_TRANSACTION_GET_PRIVATE (transaction);
	transaction->priv->finished = FALSE;
	transaction->priv->running = FALSE;
	transaction->priv->has_been_run = FALSE;
	transaction->priv->allow_cancel = TRUE;
	transaction->priv->emit_eula_required = FALSE;
	transaction->priv->emit_signature_required = FALSE;
	transaction->priv->dbus_name = NULL;
	transaction->priv->cached_enabled = FALSE;
	transaction->priv->cached_key_id = NULL;
	transaction->priv->cached_package_id = NULL;
	transaction->priv->cached_package_ids = NULL;
	transaction->priv->cached_transaction_id = NULL;
	transaction->priv->cached_full_path = NULL;
	transaction->priv->cached_full_paths = NULL;
	transaction->priv->cached_filters = PK_FILTER_ENUM_NONE;
	transaction->priv->cached_search = NULL;
	transaction->priv->cached_repo_id = NULL;
	transaction->priv->cached_parameter = NULL;
	transaction->priv->cached_value = NULL;
	transaction->priv->last_package_id = NULL;
	transaction->priv->tid = NULL;
	transaction->priv->locale = NULL;
	transaction->priv->role = PK_ROLE_ENUM_UNKNOWN;
	transaction->priv->status = PK_STATUS_ENUM_WAIT;
	transaction->priv->percentage = PK_BACKEND_PERCENTAGE_INVALID;
	transaction->priv->subpercentage = PK_BACKEND_PERCENTAGE_INVALID;
	transaction->priv->elapsed = 0;
	transaction->priv->remaining = 0;
	transaction->priv->backend = pk_backend_new ();
	transaction->priv->security = pk_security_new ();
	transaction->priv->cache = pk_cache_new ();
	transaction->priv->conf = pk_conf_new ();
	transaction->priv->update_detail_list = pk_update_detail_list_new ();
	transaction->priv->notify = pk_notify_new ();
	transaction->priv->inhibit = pk_inhibit_new ();
	transaction->priv->package_list = pk_package_list_new ();
	transaction->priv->transaction_list = pk_transaction_list_new ();
	transaction->priv->post_trans = pk_post_trans_new ();

	transaction->priv->transaction_db = pk_transaction_db_new ();
	g_signal_connect (transaction->priv->transaction_db, "transaction",
			  G_CALLBACK (pk_transaction_transaction_cb), transaction);

	transaction->priv->monitor = egg_dbus_monitor_new ();
	g_signal_connect (transaction->priv->monitor, "connection-changed",
			  G_CALLBACK (pk_transaction_caller_active_changed_cb), transaction);
}

/**
 * pk_transaction_finalize:
 * @object: The object to finalize
 **/
static void
pk_transaction_finalize (GObject *object)
{
	PkTransaction *transaction;

	g_return_if_fail (PK_IS_TRANSACTION (object));

	transaction = PK_TRANSACTION (object);
	g_return_if_fail (transaction->priv != NULL);

	/* send signal to clients that we are about to be destroyed */
	egg_debug ("emitting destroy %s", transaction->priv->tid);
	g_signal_emit (transaction, signals [PK_TRANSACTION_DESTROY], 0);

	g_free (transaction->priv->last_package_id);
	g_free (transaction->priv->dbus_name);
	g_free (transaction->priv->locale);
	g_free (transaction->priv->cached_package_id);
	g_free (transaction->priv->cached_key_id);
	g_strfreev (transaction->priv->cached_package_ids);
	g_free (transaction->priv->cached_transaction_id);
	g_free (transaction->priv->cached_directory);
	g_free (transaction->priv->cached_search);
	g_free (transaction->priv->cached_repo_id);
	g_free (transaction->priv->cached_parameter);
	g_free (transaction->priv->cached_value);
	g_free (transaction->priv->tid);

	/* remove any inhibit, it's okay to call this function when it's not needed */
	pk_inhibit_remove (transaction->priv->inhibit, transaction);
	g_object_unref (transaction->priv->conf);
	g_object_unref (transaction->priv->cache);
	g_object_unref (transaction->priv->update_detail_list);
	g_object_unref (transaction->priv->inhibit);
	g_object_unref (transaction->priv->backend);
	g_object_unref (transaction->priv->monitor);
	g_object_unref (transaction->priv->package_list);
	g_object_unref (transaction->priv->transaction_list);
	g_object_unref (transaction->priv->transaction_db);
	g_object_unref (transaction->priv->security);
	g_object_unref (transaction->priv->notify);
	g_object_unref (transaction->priv->post_trans);

	G_OBJECT_CLASS (pk_transaction_parent_class)->finalize (object);
}

/**
 * pk_transaction_new:
 *
 * Return value: a new PkTransaction object.
 **/
PkTransaction *
pk_transaction_new (void)
{
	PkTransaction *transaction;
	transaction = g_object_new (PK_TYPE_TRANSACTION, NULL);
	return PK_TRANSACTION (transaction);
}

/***************************************************************************
 ***                          MAKE CHECK TESTS                           ***
 ***************************************************************************/
#ifdef EGG_TEST
#include "egg-test.h"

void
egg_test_transaction (EggTest *test)
{
	PkTransaction *transaction = NULL;
	gboolean ret;
	const gchar *temp;
	GError *error = NULL;

	if (!egg_test_start (test, "PkTransaction"))
		return;

	/************************************************************/
	egg_test_title (test, "get PkTransaction object");
	transaction = pk_transaction_new ();
	egg_test_assert (test, transaction != NULL);

	/************************************************************
	 ****************          FILTERS         ******************
	 ************************************************************/
	temp = NULL;
	egg_test_title (test, "test a fail filter (null)");
	ret = pk_transaction_filter_check (temp, &error);
	egg_test_assert (test, !ret);
	g_clear_error (&error);

	/************************************************************/
	temp = "";
	egg_test_title (test, "test a fail filter ()");
	ret = pk_transaction_filter_check (temp, &error);
	egg_test_assert (test, !ret);
	g_clear_error (&error);

	/************************************************************/
	temp = ";";
	egg_test_title (test, "test a fail filter (;)");
	ret = pk_transaction_filter_check (temp, &error);
	egg_test_assert (test, !ret);
	g_clear_error (&error);

	/************************************************************/
	temp = "moo";
	egg_test_title (test, "test a fail filter (invalid)");
	ret = pk_transaction_filter_check (temp, &error);
	egg_test_assert (test, !ret);

	g_clear_error (&error);

	/************************************************************/
	temp = "moo;foo";
	egg_test_title (test, "test a fail filter (invalid, multiple)");
	ret = pk_transaction_filter_check (temp, &error);
	egg_test_assert (test, !ret);
	g_clear_error (&error);

	/************************************************************/
	temp = "gui;;";
	egg_test_title (test, "test a fail filter (valid then zero length)");
	ret = pk_transaction_filter_check (temp, &error);
	egg_test_assert (test, !ret);
	g_clear_error (&error);

	/************************************************************/
	temp = "none";
	egg_test_title (test, "test a pass filter (none)");
	ret = pk_transaction_filter_check (temp, &error);
	egg_test_assert (test, ret);
	g_clear_error (&error);

	/************************************************************/
	temp = "gui";
	egg_test_title (test, "test a pass filter (single)");
	ret = pk_transaction_filter_check (temp, &error);
	egg_test_assert (test, ret);
	g_clear_error (&error);

	/************************************************************/
	temp = "devel;~gui";
	egg_test_title (test, "test a pass filter (multiple)");
	ret = pk_transaction_filter_check (temp, &error);
	egg_test_assert (test, ret);
	g_clear_error (&error);

	/************************************************************/
	temp = "~gui;~installed";
	egg_test_title (test, "test a pass filter (multiple2)");
	ret = pk_transaction_filter_check (temp, &error);
	egg_test_assert (test, ret);
	g_clear_error (&error);

	g_object_unref (transaction);

	egg_test_end (test);
}
#endif

