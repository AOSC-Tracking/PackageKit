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
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <gio/gio.h>
#include <pk-backend.h>
#include <pk-backend-spawn.h>
#include <string.h>
#include <zif.h>

#define PREUPGRADE_BINARY			"/usr/bin/preupgrade"
#define YUM_REPOS_DIRECTORY			"/etc/yum.repos.d"
#define YUM_BACKEND_LOCKING_RETRIES		10
#define YUM_BACKEND_LOCKING_DELAY		2 /* seconds */

typedef struct {
	PkBackendSpawn	*spawn;
	GFileMonitor	*monitor;
	GCancellable	*cancellable;
	ZifDownload	*download;
	ZifConfig	*config;
	ZifStoreLocal	*store_local;
	ZifRepos	*repos;
	ZifGroups	*groups;
	ZifCompletion	*completion;
	ZifLock		*lock;
} PkBackendYumPrivate;

static PkBackendYumPrivate *priv;

/**
 * backend_stderr_cb:
 */
static gboolean
backend_stderr_cb (PkBackend *backend, const gchar *output)
{
	/* unsigned rpm, this will be picked up by yum and and exception will be thrown */
	if (strstr (output, "NOKEY") != NULL)
		return FALSE;
	if (strstr (output, "GPG") != NULL)
		return FALSE;
	if (strstr (output, "DeprecationWarning") != NULL)
		return FALSE;
	return TRUE;
}

/**
 * backend_stdout_cb:
 */
static gboolean
backend_stdout_cb (PkBackend *backend, const gchar *output)
{
	return TRUE;
}

/**
 * backend_yum_repos_changed_cb:
 **/
static void
backend_yum_repos_changed_cb (GFileMonitor *monitor_, GFile *file, GFile *other_file, GFileMonitorEvent event_type, PkBackend *backend)
{
	pk_backend_repo_list_changed (backend);
}

static void
backend_completion_percentage_changed_cb (ZifCompletion *completion, guint percentage, PkBackend *backend)
{
	pk_backend_set_percentage (backend, percentage);
}

static void
backend_completion_subpercentage_changed_cb (ZifCompletion *completion, guint subpercentage, PkBackend *backend)
{
	pk_backend_set_sub_percentage (backend, subpercentage);
}

/**
 * backend_get_lock:
 */
static gboolean
backend_get_lock (PkBackend *backend)
{
	guint i;
	guint pid;
	gboolean ret = FALSE;
	GError *error = NULL;

	for (i=0; i<YUM_BACKEND_LOCKING_RETRIES; i++) {

		/* try to lock */
		ret = zif_lock_set_locked (priv->lock, &pid, &error);
		if (ret)
			break;

		/* we're now waiting */
		pk_backend_set_status (backend, PK_STATUS_ENUM_WAITING_FOR_LOCK);

		/* now wait */
		egg_debug ("Failed to lock on try %i of %i, already locked by PID %i (sleeping for %i seconds): %s\n",
			   i+1, YUM_BACKEND_LOCKING_RETRIES, pid, YUM_BACKEND_LOCKING_DELAY, error->message);
		g_clear_error (&error);
		g_usleep (YUM_BACKEND_LOCKING_DELAY * G_USEC_PER_SEC);
	}

	/* we failed */
	if (!ret)
		pk_backend_error_code (backend, PK_ERROR_ENUM_CANNOT_GET_LOCK, "failed to get lock, held by PID: %i", pid);

	return ret;
}

/**
 * backend_unlock:
 */
static gboolean
backend_unlock (PkBackend *backend)
{
	gboolean ret;
	GError *error = NULL;

	/* try to unlock */
	ret = zif_lock_set_unlocked (priv->lock, &error);
	if (!ret) {
		egg_warning ("failed to unlock: %s", error->message);
		g_error_free (error);
	}
	return ret;
}

/**
 * backend_initialize:
 * This should only be run once per backend load, i.e. not every transaction
 */
static void
backend_initialize (PkBackend *backend)
{
	gboolean ret;
	GFile *file;
	GError *error = NULL;

	/* create private area */
	priv = g_new0 (PkBackendYumPrivate, 1);

	egg_debug ("backend: initialize");
	priv->spawn = pk_backend_spawn_new ();
	pk_backend_spawn_set_filter_stderr (priv->spawn, backend_stderr_cb);
	pk_backend_spawn_set_filter_stdout (priv->spawn, backend_stdout_cb);
	pk_backend_spawn_set_name (priv->spawn, "yum");
	pk_backend_spawn_set_allow_sigkill (priv->spawn, FALSE);

	/* setup a file monitor on the repos directory */
	file = g_file_new_for_path (YUM_REPOS_DIRECTORY);
	priv->monitor = g_file_monitor_directory (file, G_FILE_MONITOR_NONE, NULL, &error);
	if (priv->monitor != NULL) {
		g_signal_connect (priv->monitor, "changed", G_CALLBACK (backend_yum_repos_changed_cb), backend);
	} else {
		egg_warning ("failed to setup monitor: %s", error->message);
		g_error_free (error);
	}

	/* init rpm */
	zif_init ();

	/* TODO: hook up errors */
	priv->cancellable = g_cancellable_new ();

	/* ZifCompletion */
	priv->completion = zif_completion_new ();
	g_signal_connect (priv->completion, "percentage-changed", G_CALLBACK (backend_completion_percentage_changed_cb), backend);
	g_signal_connect (priv->completion, "subpercentage-changed", G_CALLBACK (backend_completion_subpercentage_changed_cb), backend);

	/* ZifConfig */
	priv->config = zif_config_new ();
	ret = zif_config_set_filename (priv->config, "/etc/yum.conf", &error);
	if (!ret) {
		pk_backend_error_code (backend, PK_ERROR_ENUM_FAILED_CONFIG_PARSING, "failed to set config: %s", error->message);
		g_error_free (error);
		goto out;
	}

	/* ZifDownload */
	priv->download = zif_download_new ();

	/* ZifLock */
	priv->lock = zif_lock_new ();

	/* ZifStoreLocal */
	priv->store_local = zif_store_local_new ();
	ret = zif_store_local_set_prefix (priv->store_local, "/", &error);
	if (!ret) {
		pk_backend_error_code (backend, PK_ERROR_ENUM_INTERNAL_ERROR, "failed to set prefix: %s", error->message);
		g_error_free (error);
		goto out;
	}

	/* ZifRepos */
	priv->repos = zif_repos_new ();
	ret = zif_repos_set_repos_dir (priv->repos, "/etc/yum.repos.d", &error);
	if (!ret) {
		pk_backend_error_code (backend, PK_ERROR_ENUM_REPO_CONFIGURATION_ERROR, "failed to set repos dir: %s", error->message);
		g_error_free (error);
		goto out;
	}

	/* ZifGroups */
	priv->groups = zif_groups_new ();
	ret = zif_groups_set_mapping_file (priv->groups, "/usr/share/PackageKit/helpers/yum/yum-comps-groups.conf", &error);
	if (!ret) {
		pk_backend_error_code (backend, PK_ERROR_ENUM_GROUP_LIST_INVALID, "failed to set mapping file: %s", error->message);
		g_error_free (error);
		goto out;
	}
out:
	g_object_unref (file);
}

/**
 * backend_destroy:
 * This should only be run once per backend load, i.e. not every transaction
 */
static void
backend_destroy (PkBackend *backend)
{
	egg_debug ("backend: destroy");
	g_object_unref (priv->spawn);
	if (priv->monitor != NULL)
		g_object_unref (priv->monitor);
	if (priv->config != NULL)
		g_object_unref (priv->config);
	if (priv->download != NULL)
		g_object_unref (priv->download);
	if (priv->completion != NULL)
		g_object_unref (priv->completion);
	if (priv->repos != NULL)
		g_object_unref (priv->repos);
	if (priv->groups != NULL)
		g_object_unref (priv->groups);
	if (priv->store_local != NULL)
		g_object_unref (priv->store_local);
	if (priv->lock != NULL)
		g_object_unref (priv->lock);
	g_free (priv);
}

/**
 * backend_get_groups:
 */
static PkBitfield
backend_get_groups (PkBackend *backend)
{
	GError *error = NULL;
	PkBitfield groups;

	/* get the dynamic group list */
	groups = zif_groups_get_groups (priv->groups, &error);
	if (groups == 0) {
		pk_backend_error_code (backend, PK_ERROR_ENUM_GROUP_LIST_INVALID, "failed to get the list of groups: %s", error->message);
		g_error_free (error);
		goto out;
	}

	/* add the virtual groups */
	pk_bitfield_add (groups, PK_GROUP_ENUM_COLLECTIONS);
	pk_bitfield_add (groups, PK_GROUP_ENUM_NEWEST);
out:
	return groups;
}

/**
 * backend_get_filters:
 */
static PkBitfield
backend_get_filters (PkBackend *backend)
{
	return pk_bitfield_from_enums (
		PK_FILTER_ENUM_GUI,
		PK_FILTER_ENUM_INSTALLED,
		PK_FILTER_ENUM_DEVELOPMENT,
		PK_FILTER_ENUM_BASENAME,
		PK_FILTER_ENUM_FREE,
		PK_FILTER_ENUM_NEWEST,
		PK_FILTER_ENUM_ARCH,
		-1);
}

/**
 * backend_get_roles:
 */
static PkBitfield
backend_get_roles (PkBackend *backend)
{
	PkBitfield roles;
	roles = pk_bitfield_from_enums (
		PK_ROLE_ENUM_CANCEL,
		PK_ROLE_ENUM_GET_DEPENDS,
		PK_ROLE_ENUM_GET_DETAILS,
		PK_ROLE_ENUM_GET_FILES,
		PK_ROLE_ENUM_GET_REQUIRES,
		PK_ROLE_ENUM_GET_PACKAGES,
		PK_ROLE_ENUM_WHAT_PROVIDES,
		PK_ROLE_ENUM_GET_UPDATES,
		PK_ROLE_ENUM_GET_UPDATE_DETAIL,
		PK_ROLE_ENUM_INSTALL_PACKAGES,
		PK_ROLE_ENUM_INSTALL_FILES,
		PK_ROLE_ENUM_INSTALL_SIGNATURE,
		PK_ROLE_ENUM_REFRESH_CACHE,
		PK_ROLE_ENUM_REMOVE_PACKAGES,
		PK_ROLE_ENUM_DOWNLOAD_PACKAGES,
		PK_ROLE_ENUM_RESOLVE,
		PK_ROLE_ENUM_SEARCH_DETAILS,
		PK_ROLE_ENUM_SEARCH_FILE,
		PK_ROLE_ENUM_SEARCH_GROUP,
		PK_ROLE_ENUM_SEARCH_NAME,
		PK_ROLE_ENUM_UPDATE_PACKAGES,
		PK_ROLE_ENUM_UPDATE_SYSTEM,
		PK_ROLE_ENUM_GET_REPO_LIST,
		PK_ROLE_ENUM_REPO_ENABLE,
		PK_ROLE_ENUM_REPO_SET_DATA,
		PK_ROLE_ENUM_GET_CATEGORIES,
		PK_ROLE_ENUM_SIMULATE_INSTALL_FILES,
		PK_ROLE_ENUM_SIMULATE_INSTALL_PACKAGES,
		PK_ROLE_ENUM_SIMULATE_UPDATE_PACKAGES,
		PK_ROLE_ENUM_SIMULATE_REMOVE_PACKAGES,
		-1);

	/* only add GetDistroUpgrades if the binary is present */
	if (g_file_test (PREUPGRADE_BINARY, G_FILE_TEST_EXISTS))
		pk_bitfield_add (roles, PK_ROLE_ENUM_GET_DISTRO_UPGRADES);

	return roles;
}

/**
 * backend_get_mime_types:
 */
static gchar *
backend_get_mime_types (PkBackend *backend)
{
	return g_strdup ("application/x-rpm;application/x-servicepack");
}

/**
 * pk_backend_cancel:
 */
static void
backend_cancel (PkBackend *backend)
{
	/* this feels bad... */
	pk_backend_spawn_kill (priv->spawn);
}

/**
 * backend_download_packages:
 */
static void
backend_download_packages (PkBackend *backend, gchar **package_ids, const gchar *directory)
{
	gchar *package_ids_temp;

	/* send the complete list as stdin */
	package_ids_temp = pk_package_ids_to_string (package_ids);
	pk_backend_spawn_helper (priv->spawn, "yumBackend.py", "download-packages", directory, package_ids_temp, NULL);
	g_free (package_ids_temp);
}

/**
 * backend_get_depends:
 */
static void
backend_get_depends (PkBackend *backend, PkBitfield filters, gchar **package_ids, gboolean recursive)
{
	gchar *filters_text;
	gchar *package_ids_temp;
	package_ids_temp = pk_package_ids_to_string (package_ids);
	filters_text = pk_filter_bitfield_to_string (filters);
	pk_backend_spawn_helper (priv->spawn, "yumBackend.py", "get-depends", filters_text, package_ids_temp, pk_backend_bool_to_string (recursive), NULL);
	g_free (filters_text);
	g_free (package_ids_temp);
}

/**
 * backend_get_details:
 */
static void
backend_get_details (PkBackend *backend, gchar **package_ids)
{
	gchar *package_ids_temp;
	package_ids_temp = pk_package_ids_to_string (package_ids);
	pk_backend_spawn_helper (priv->spawn, "yumBackend.py", "get-details", package_ids_temp, NULL);
	g_free (package_ids_temp);
}

/**
  * backend_get_distro_upgrades_thread:
  */
static gboolean
backend_get_distro_upgrades_thread (PkBackend *backend)
{
	gboolean ret;
	gchar *distro_id = NULL;
	gchar *filename = NULL;
	gchar **groups = NULL;
	gchar *name = NULL;
	gchar *proxy = NULL;
	gchar **split = NULL;
	guint i;
	guint last_version = 0;
	guint newest = G_MAXUINT;
	guint version;
	GError *error = NULL;
	GKeyFile *file = NULL;
	ZifCompletion *child;

	/* download, then parse */
	zif_completion_reset (priv->completion);
	zif_completion_set_number_steps (priv->completion, 2);

	/* set proxy */
	proxy = pk_backend_get_proxy_http (backend);
	ret = zif_download_set_proxy (priv->download, proxy, &error);
	if (!ret) {
		pk_backend_error_code (backend, PK_ERROR_ENUM_TRANSACTION_ERROR, "failed to set proxy: %s", error->message);
		g_error_free (error);
		goto out;
	}

	/* download new file */
	filename = g_build_filename ("/var/cache/PackageKit", "releases.txt", NULL);
	child = zif_completion_get_child (priv->completion);
	pk_backend_set_status (backend, PK_STATUS_ENUM_DOWNLOAD_UPDATEINFO);
	ret = zif_download_file (priv->download, "http://mirrors.fedoraproject.org/releases.txt", filename, NULL, child, &error);
	if (!ret) {
		pk_backend_error_code (backend, PK_ERROR_ENUM_TRANSACTION_ERROR, "failed to download %s: %s", filename, error->message);
		g_error_free (error);
		goto out;
	}
	zif_completion_done (priv->completion);

	file = g_key_file_new ();
	ret = g_key_file_load_from_file (file, filename, G_KEY_FILE_NONE, &error);
	if (!ret) {
		pk_backend_error_code (backend, PK_ERROR_ENUM_TRANSACTION_ERROR, "failed to open %s: %s", filename, error->message);
		g_error_free (error);
		goto out;
	}

	/* get all entries */
	groups = g_key_file_get_groups (file, NULL);
	for (i=0; groups[i] != NULL; i++) {
		/* we only care about stable versions */
		if (!g_key_file_get_boolean (file, groups[i], "stable", NULL))
			goto out;
		version = g_key_file_get_integer (file, groups[i], "version", NULL);
		egg_debug ("%s is update to version %i", groups[i], version);
		if (version > last_version) {
			newest = i;
			last_version = version;
		}
	}

	/* nothing found */
	if (newest == G_MAXUINT) {
		pk_backend_error_code (backend, PK_ERROR_ENUM_FAILED_CONFIG_PARSING, "could not get latest distro data");
		goto out;
	}

	/* are we already on the latest version */
	version = zif_config_get_uint (priv->config, "releasever", &error);
	if (version == G_MAXUINT) {
		pk_backend_error_code (backend, PK_ERROR_ENUM_FAILED_CONFIG_PARSING, "could not get distro present version");
		goto out;
	}

	/* all okay, nothing to show */
	if (version >= last_version)
		goto out;

	/* if we have an upgrade candidate then pass back data to daemon */
	split = g_strsplit (groups[newest], " ", -1);
	name = g_ascii_strdown (split[0], -1);
	distro_id = g_strdup_printf ("%s-%s", name, split[1]);
	pk_backend_distro_upgrade (backend, PK_DISTRO_UPGRADE_ENUM_STABLE, distro_id, groups[newest]);

	/* we're done */
	zif_completion_done (priv->completion);
out:
	pk_backend_finished (backend);
	g_free (distro_id);
	g_free (filename);
	g_free (name);
	g_free (proxy);
	if (file != NULL)
		g_key_file_free (file);
	g_strfreev (groups);
	g_strfreev (split);
	return TRUE;
}

/**
 * backend_get_distro_upgrades:
 */
static void
backend_get_distro_upgrades (PkBackend *backend)
{
	pk_backend_thread_create (backend, backend_get_distro_upgrades_thread);
}

/**
 * backend_get_files:
 */
static void
backend_get_files (PkBackend *backend, gchar **package_ids)
{
	gchar *package_ids_temp;
	package_ids_temp = pk_package_ids_to_string (package_ids);
	pk_backend_spawn_helper (priv->spawn,  "yumBackend.py", "get-files", package_ids_temp, NULL);
	g_free (package_ids_temp);
}

/**
 * backend_get_requires:
 */
static void
backend_get_requires (PkBackend *backend, PkBitfield filters, gchar **package_ids, gboolean recursive)
{
	gchar *package_ids_temp;
	gchar *filters_text;
	package_ids_temp = pk_package_ids_to_string (package_ids);
	filters_text = pk_filter_bitfield_to_string (filters);
	pk_backend_spawn_helper (priv->spawn, "yumBackend.py", "get-requires", filters_text, package_ids_temp, pk_backend_bool_to_string (recursive), NULL);
	g_free (filters_text);
	g_free (package_ids_temp);
}

/**
 * backend_get_updates:
 */
static void
backend_get_updates (PkBackend *backend, PkBitfield filters)
{
	gchar *filters_text;
	filters_text = pk_filter_bitfield_to_string (filters);
	pk_backend_spawn_helper (priv->spawn,  "yumBackend.py", "get-updates", filters_text, NULL);
	g_free (filters_text);
}

/**
 * backend_get_packages:
 */
static void
backend_get_packages (PkBackend *backend, PkBitfield filters)
{
	gchar *filters_text;
	filters_text = pk_filter_bitfield_to_string (filters);
	pk_backend_spawn_helper (priv->spawn, "yumBackend.py", "get-packages", filters_text, NULL);
	g_free (filters_text);
}

/**
 * backend_get_update_detail:
 */
static void
backend_get_update_detail (PkBackend *backend, gchar **package_ids)
{
	gchar *package_ids_temp;
	package_ids_temp = pk_package_ids_to_string (package_ids);
	pk_backend_spawn_helper (priv->spawn, "yumBackend.py", "get-update-detail", package_ids_temp, NULL);
	g_free (package_ids_temp);
}

/**
 * backend_install_packages:
 */
static void
backend_install_packages (PkBackend *backend, gboolean only_trusted, gchar **package_ids)
{
	gchar *package_ids_temp;

	/* send the complete list as stdin */
	package_ids_temp = pk_package_ids_to_string (package_ids);
	pk_backend_spawn_helper (priv->spawn, "yumBackend.py", "install-packages", pk_backend_bool_to_string (only_trusted), package_ids_temp, NULL);
	g_free (package_ids_temp);
}

/**
 * backend_simulate_remove_packages:
 */
static void
backend_simulate_remove_packages (PkBackend *backend, gchar **package_ids, gboolean autoremove)
{
	gchar *package_ids_temp;

	/* send the complete list as stdin */
	package_ids_temp = pk_package_ids_to_string (package_ids);
	pk_backend_spawn_helper (priv->spawn, "yumBackend.py", "simulate-remove-packages", package_ids_temp, NULL);
	g_free (package_ids_temp);
}

/**
 * backend_simulate_update_packages:
 */
static void
backend_simulate_update_packages (PkBackend *backend, gchar **package_ids)
{
	gchar *package_ids_temp;

	/* send the complete list as stdin */
	package_ids_temp = pk_package_ids_to_string (package_ids);
	pk_backend_spawn_helper (priv->spawn, "yumBackend.py", "simulate-update-packages", package_ids_temp, NULL);
	g_free (package_ids_temp);
}

/**
 * backend_simulate_install_packages:
 */
static void
backend_simulate_install_packages (PkBackend *backend, gchar **package_ids)
{
	gchar *package_ids_temp;

	/* send the complete list as stdin */
	package_ids_temp = pk_package_ids_to_string (package_ids);
	pk_backend_spawn_helper (priv->spawn, "yumBackend.py", "simulate-install-packages", package_ids_temp, NULL);
	g_free (package_ids_temp);
}

/**
 * backend_install_files:
 */
static void
backend_install_files (PkBackend *backend, gboolean only_trusted, gchar **full_paths)
{
	gchar *package_ids_temp;

	/* send the complete list as stdin */
	package_ids_temp = g_strjoinv (PK_BACKEND_SPAWN_FILENAME_DELIM, full_paths);
	pk_backend_spawn_helper (priv->spawn, "yumBackend.py", "install-files", pk_backend_bool_to_string (only_trusted), package_ids_temp, NULL);
	g_free (package_ids_temp);
}

/**
 * backend_install_signature:
 */
static void
backend_install_signature (PkBackend *backend, PkSigTypeEnum type,
			   const gchar *key_id, const gchar *package_id)
{
	const gchar *type_text;

	type_text = pk_sig_type_enum_to_string (type);
	pk_backend_spawn_helper (priv->spawn, "yumBackend.py", "install-signature", type_text, key_id, package_id, NULL);
}

/**
 * backend_refresh_cache:
 */
static void
backend_refresh_cache (PkBackend *backend, gboolean force)
{
	/* check network state */
	if (!pk_backend_is_online (backend)) {
		pk_backend_error_code (backend, PK_ERROR_ENUM_NO_NETWORK, "Cannot refresh cache whilst offline");
		pk_backend_finished (backend);
		return;
	}

	pk_backend_spawn_helper (priv->spawn, "yumBackend.py", "refresh-cache", pk_backend_bool_to_string (force), NULL);
}

/**
 * pk_backend_remove_packages:
 */
static void
backend_remove_packages (PkBackend *backend, gchar **package_ids, gboolean allow_deps, gboolean autoremove)
{
	gchar *package_ids_temp;

	/* send the complete list as stdin */
	package_ids_temp = pk_package_ids_to_string (package_ids);
	pk_backend_spawn_helper (priv->spawn, "yumBackend.py", "remove-packages", pk_backend_bool_to_string (allow_deps), pk_backend_bool_to_string (autoremove), package_ids_temp, NULL);
	g_free (package_ids_temp);
}

/**
 * pk_backend_search_details:
 */
static void
backend_search_details (PkBackend *backend, PkBitfield filters, gchar **values)
{
	gchar *filters_text;
	gchar *search;
	filters_text = pk_filter_bitfield_to_string (filters);
	search = g_strjoinv ("&", values);
	pk_backend_spawn_helper (priv->spawn, "yumBackend.py", "search-details", filters_text, search, NULL);
	g_free (filters_text);
	g_free (search);
}

/**
 * pk_backend_search_files:
 */
static void
backend_search_files (PkBackend *backend, PkBitfield filters, gchar **values)
{
	gchar *filters_text;
	gchar *search;
	filters_text = pk_filter_bitfield_to_string (filters);
	search = g_strjoinv ("&", values);
	pk_backend_spawn_helper (priv->spawn, "yumBackend.py", "search-file", filters_text, search, NULL);
	g_free (filters_text);
	g_free (search);
}

/**
 * pk_backend_search_groups:
 */
static void
backend_search_groups (PkBackend *backend, PkBitfield filters, gchar **values)
{
	gchar *filters_text;
	gchar *search;
	filters_text = pk_filter_bitfield_to_string (filters);
	search = g_strjoinv ("&", values);
	pk_backend_spawn_helper (priv->spawn, "yumBackend.py", "search-group", filters_text, search, NULL);
	g_free (filters_text);
	g_free (search);
}

/**
 * pk_backend_search_names:
 */
static void
backend_search_names (PkBackend *backend, PkBitfield filters, gchar **values)
{
	gchar *filters_text;
	gchar *search;
	filters_text = pk_filter_bitfield_to_string (filters);
	search = g_strjoinv ("&", values);
	pk_backend_spawn_helper (priv->spawn, "yumBackend.py", "search-name", filters_text, search, NULL);
	g_free (filters_text);
	g_free (search);
}

/**
 * pk_backend_update_packages:
 */
static void
backend_update_packages (PkBackend *backend, gboolean only_trusted, gchar **package_ids)
{
	gchar *package_ids_temp;

	/* send the complete list as stdin */
	package_ids_temp = pk_package_ids_to_string (package_ids);
	pk_backend_spawn_helper (priv->spawn, "yumBackend.py", "update-packages", pk_backend_bool_to_string (only_trusted), package_ids_temp, NULL);
	g_free (package_ids_temp);
}

/**
 * pk_backend_update_system:
 */
static void
backend_update_system (PkBackend *backend, gboolean only_trusted)
{
	pk_backend_spawn_helper (priv->spawn, "yumBackend.py", "update-system", pk_backend_bool_to_string (only_trusted), NULL);
}

/**
 * pk_backend_resolve:
 */
static void
backend_resolve (PkBackend *backend, PkBitfield filters, gchar **package_ids)
{
	gchar *filters_text;
	gchar *package_ids_temp;
	filters_text = pk_filter_bitfield_to_string (filters);
	package_ids_temp = pk_package_ids_to_string (package_ids);
	pk_backend_spawn_helper (priv->spawn, "yumBackend.py", "resolve", filters_text, package_ids_temp, NULL);
	g_free (filters_text);
	g_free (package_ids_temp);
}

/**
 * backend_get_repo_list_thread:
 */
static gboolean
backend_get_repo_list_thread (PkBackend *backend)
{
	gboolean ret;
	PkBitfield filters = (PkBitfield) pk_backend_get_uint (backend, "filters");
	guint i;
	GPtrArray *array = NULL;
	ZifStoreRemote *store;
	ZifCompletion *completion_local;
	const gchar *repo_id;
	const gchar *name;
	gboolean enabled;
	gboolean devel;
	GError *error = NULL;

	/* get lock */
	ret = backend_get_lock (backend);
	if (!ret) {
		egg_warning ("failed to get lock");
		goto out;
	}

	pk_backend_set_status (backend, PK_STATUS_ENUM_QUERY);

	/* setup completion */
	zif_completion_reset (priv->completion);
	zif_completion_set_number_steps (priv->completion, 2);

	completion_local = zif_completion_get_child (priv->completion);
	array = zif_repos_get_stores (priv->repos, priv->cancellable, completion_local, &error);
	if (array == NULL) {
		pk_backend_error_code (backend, PK_ERROR_ENUM_REPO_NOT_FOUND, "failed to find repos: %s", error->message);
		g_error_free (error);
		goto out;
	}

	/* none? */
	if (array->len == 0) {
		pk_backend_error_code (backend, PK_ERROR_ENUM_REPO_NOT_FOUND, "failed to find any repos");
		goto out;
	}

	/* this section done */
	zif_completion_done (priv->completion);

	/* setup completion */
	completion_local = zif_completion_get_child (priv->completion);
	zif_completion_set_number_steps (completion_local, array->len);

	/* looks at each store */
	for (i=0; i<array->len; i++) {
		store = g_ptr_array_index (array, i);
		if (pk_bitfield_contain (filters, PK_FILTER_ENUM_NOT_DEVELOPMENT)) {
			/* TODO: completion */
			devel = zif_store_remote_is_devel (store, priv->cancellable, NULL, NULL);
			if (devel)
				continue;
		}
		repo_id = zif_store_get_id (ZIF_STORE (store));
		/* TODO: completion */
		name = zif_store_remote_get_name (store, priv->cancellable, NULL, NULL);
		/* TODO: completion */
		enabled = zif_store_remote_get_enabled (store, priv->cancellable, NULL, NULL);
		pk_backend_repo_detail (backend, repo_id, name, enabled);

		/* this section done */
		zif_completion_done (completion_local);
	}

	/* this section done */
	zif_completion_done (priv->completion);
out:
	backend_unlock (backend);
	pk_backend_finished (backend);
	if (array != NULL)
		g_ptr_array_unref (array);
	return TRUE;
}

/**
 * pk_backend_get_repo_list:
 */
static void
backend_get_repo_list (PkBackend *backend, PkBitfield filters)
{
	pk_backend_thread_create (backend, backend_get_repo_list_thread);
}

/**
 * backend_repo_enable_thread:
 */
static gboolean
backend_repo_enable_thread (PkBackend *backend)
{
	ZifStoreRemote *repo = NULL;
	gboolean ret;
	GError *error = NULL;
	gchar *warning = NULL;
	gboolean enabled = pk_backend_get_bool (backend, "enabled");
	const gchar *repo_id = pk_backend_get_string (backend, "repo_id");

	/* get lock */
	ret = backend_get_lock (backend);
	if (!ret) {
		egg_warning ("failed to get lock");
		goto out;
	}

	pk_backend_set_status (backend, PK_STATUS_ENUM_QUERY);

	/* find the right repo */
	repo = zif_repos_get_store (priv->repos, repo_id, priv->cancellable, priv->completion, &error);
	if (repo == NULL) {
		pk_backend_error_code (backend, PK_ERROR_ENUM_REPO_NOT_FOUND, "failed to find repo: %s", error->message);
		g_error_free (error);
		goto out;
	}

	/* set the state */
	ret = zif_store_remote_set_enabled (repo, enabled, &error);
	if (!ret) {
		pk_backend_error_code (backend, PK_ERROR_ENUM_CANNOT_DISABLE_REPOSITORY, "failed to set enable: %s", error->message);
		g_error_free (error);
		goto out;
	}

	/* warn if rawhide */
	if (g_strstr_len (repo_id, -1, "rawhide") != NULL) {
		warning = g_strdup_printf ("These packages are untested and still under development."
					   "This repository is used for development of new releases.\n\n"
					   "This repository can see significant daily turnover and major "
					   "functionality changes which cause unexpected problems with "
					   "other development packages.\n"
					   "Please use these packages if you want to work with the "
					   "Fedora developers by testing these new development packages.\n\n"
					   "If this is not correct, please disable the %s software source.", repo_id);
		pk_backend_message (backend, PK_MESSAGE_ENUM_REPO_FOR_DEVELOPERS_ONLY, warning);
	}
out:
	backend_unlock (backend);
	pk_backend_finished (backend);
	g_free (warning);
	if (repo != NULL)
		g_object_unref (repo);
	return TRUE;
}

/**
 * pk_backend_repo_enable:
 */
static void
backend_repo_enable (PkBackend *backend, const gchar *repo_id, gboolean enabled)
{
	pk_backend_thread_create (backend, backend_repo_enable_thread);
}

/**
 * pk_backend_repo_set_data:
 */
static void
backend_repo_set_data (PkBackend *backend, const gchar *repo_id, const gchar *parameter, const gchar *value)
{
	/* no operation */
	pk_backend_finished (backend);
}

/**
 * backend_what_provides:
 */
static void
backend_what_provides (PkBackend *backend, PkBitfield filters, PkProvidesEnum provides, gchar **values)
{
	gchar *filters_text;
	gchar *search;
	const gchar *provides_text;
	provides_text = pk_provides_enum_to_string (provides);
	filters_text = pk_filter_bitfield_to_string (filters);
	search = g_strjoinv ("&", values);
	pk_backend_spawn_helper (priv->spawn, "yumBackend.py", "what-provides", filters_text, provides_text, search, NULL);
	g_free (filters_text);
	g_free (search);
}

/**
 * pk_backend_get_categories:
 */
static void
backend_get_categories (PkBackend *backend)
{
	pk_backend_spawn_helper (priv->spawn, "yumBackend.py", "get-categories", NULL);
}

/**
 * backend_simulate_install_files:
 */
static void
backend_simulate_install_files (PkBackend *backend, gchar **full_paths)
{
	gchar *package_ids_temp;

	/* send the complete list as stdin */
	package_ids_temp = g_strjoinv (PK_BACKEND_SPAWN_FILENAME_DELIM, full_paths);
	pk_backend_spawn_helper (priv->spawn, "yumBackend.py", "simulate-install-files", package_ids_temp, NULL);
	g_free (package_ids_temp);
}

PK_BACKEND_OPTIONS (
	"YUM",					/* description */
	"Tim Lauridsen <timlau@fedoraproject.org>, Richard Hughes <richard@hughsie.com>",	/* author */
	backend_initialize,			/* initalize */
	backend_destroy,			/* destroy */
	backend_get_groups,			/* get_groups */
	backend_get_filters,			/* get_filters */
	backend_get_roles,			/* get_roles */
	backend_get_mime_types,			/* get_mime_types */
	backend_cancel,				/* cancel */
	backend_download_packages,		/* download_packages */
	backend_get_categories,			/* get_categories */
	backend_get_depends,			/* get_depends */
	backend_get_details,			/* get_details */
	backend_get_distro_upgrades,		/* get_distro_upgrades */
	backend_get_files,			/* get_files */
	backend_get_packages,			/* get_packages */
	backend_get_repo_list,			/* get_repo_list */
	backend_get_requires,			/* get_requires */
	backend_get_update_detail,		/* get_update_detail */
	backend_get_updates,			/* get_updates */
	backend_install_files,			/* install_files */
	backend_install_packages,		/* install_packages */
	backend_install_signature,		/* install_signature */
	backend_refresh_cache,			/* refresh_cache */
	backend_remove_packages,		/* remove_packages */
	backend_repo_enable,			/* repo_enable */
	backend_repo_set_data,			/* repo_set_data */
	backend_resolve,			/* resolve */
	NULL,					/* rollback */
	backend_search_details,			/* search_details */
	backend_search_files,			/* search_files */
	backend_search_groups,			/* search_groups */
	backend_search_names,			/* search_names */
	backend_update_packages,		/* update_packages */
	backend_update_system,			/* update_system */
	backend_what_provides,			/* what_provides */
	backend_simulate_install_files,		/* simulate_install_files */
	backend_simulate_install_packages,	/* simulate_install_packages */
	backend_simulate_remove_packages,	/* simulate_remove_packages */
	backend_simulate_update_packages	/* simulate_update_packages */
);

