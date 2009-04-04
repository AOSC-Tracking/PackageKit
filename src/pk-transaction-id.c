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
#include <string.h>
#include <sys/types.h>
#include <glib/gi18n.h>

#include "egg-debug.h"
#include "egg-string.h"

#include "pk-transaction-id.h"
#define PK_TRANSACTION_ID_COUNT_FILE		LOCALSTATEDIR "/lib/PackageKit/job_count.dat"

/**
 * pk_transaction_id_get_random_hex_string:
 **/
static gchar *
pk_transaction_id_get_random_hex_string (guint length)
{
	GRand *gen;
	gint32 num;
	gchar *string;
	guint i;

	gen = g_rand_new ();

	/* allocate a string with the correct size */
	string = g_strnfill (length, 'x');
	for (i=0; i<length; i++) {
		num = g_rand_int_range (gen, (gint32) 'a', (gint32) 'f');
		/* assign a random number as a char */
		string[i] = (gchar) num;
	}
	g_rand_free (gen);
	return string;
}

/**
 * pk_transaction_id_load_job_count:
 **/
static guint
pk_transaction_id_load_job_count (void)
{
	gboolean ret;
	gchar *contents;
	guint job_count;
	ret = g_file_get_contents (PK_TRANSACTION_ID_COUNT_FILE, &contents, NULL, NULL);
	if (ret == FALSE) {
		egg_warning ("failed to get last job");
		return FALSE;
	}

	/* convert */
	ret = egg_strtouint (contents, &job_count);
	if (ret == FALSE) {
		egg_warning ("failed to convert");
	}

	/* check we got a sane number */
	if (job_count > 10240) {
		egg_warning ("invalid job count!");
		job_count = 0;
	}

	egg_debug ("job=%i", job_count);
	g_free (contents);
	return job_count;
}

/**
 * pk_transaction_id_save_job_count:
 **/
static gboolean
pk_transaction_id_save_job_count (guint job_count)
{
	gboolean ret;
	gchar *contents;

	egg_debug ("saving %i", job_count);
	contents = g_strdup_printf ("%i", job_count);
	ret = g_file_set_contents (PK_TRANSACTION_ID_COUNT_FILE, contents, -1, NULL);
	g_free (contents);
	if (ret == FALSE) {
		egg_warning ("failed to set last job");
		return FALSE;
	}
	return TRUE;
}

/**
 * pk_transaction_id_equal:
 **/
gboolean
pk_transaction_id_equal (const gchar *tid1, const gchar *tid2)
{
	/* TODO, ignore the data part */
	return egg_strequal (tid1, tid2);
}

/**
 * pk_transaction_id_generate:
 **/
gchar *
pk_transaction_id_generate (void)
{
	gchar *rand_str;
	gchar *tid;
	guint job_count;

	/* load from file */
	job_count = pk_transaction_id_load_job_count ();
	rand_str = pk_transaction_id_get_random_hex_string (8);
	job_count++;

	/* save the new value */
	pk_transaction_id_save_job_count (job_count);

	/* make the tid */
	tid = g_strdup_printf ("/%i_%s_data", job_count, rand_str);

	g_free (rand_str);
	return tid;
}

/***************************************************************************
 ***                          MAKE CHECK TESTS                           ***
 ***************************************************************************/
#ifdef EGG_TEST
#include "egg-test.h"

void
egg_test_transaction_id (EggTest *test)
{
	gchar *tid;
	gboolean ret;

	if (!egg_test_start (test, "PkTransactionId"))
		return;

	/************************************************************
	 ****************          IDENT           ******************
	 ************************************************************/
	egg_test_title (test, "get an tid object");
	tid = pk_transaction_id_generate ();
	egg_test_assert (test, tid != NULL);
	g_free (tid);

	/************************************************************/
	egg_test_title (test, "tid equal pass (same)");
	ret = pk_transaction_id_equal ("/34_1234def_r23", "/34_1234def_r23");
	egg_test_assert (test, ret);

	egg_test_end (test);
}
#endif

