/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2013 Richard Hughes <richard@hughsie.com>
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

#include <stdlib.h>
#include <glib.h>

#include "hif-package.h"
#include "hif-package-md.h"

/**
 * hif_package_get_filename:
 **/
const gchar *
hif_package_get_filename (GHashTable *fixme, HyPackage pkg)
{
	const gchar *filename;
	filename = hif_package_md_get_data (fixme,
					    pkg,
					    "downloaded-filename");
	return filename;
}

/**
 * hif_package_set_filename:
 **/
void
hif_package_set_filename (GHashTable *fixme, HyPackage pkg, const gchar *filename)
{
	hif_package_md_set_data (fixme,
				 pkg,
				 "downloaded-filename",
				 g_strdup (filename),
				 g_free);
}
