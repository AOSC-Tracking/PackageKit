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

#ifndef __PK_POST_TRANS_H
#define __PK_POST_TRANS_H

#include <glib-object.h>

G_BEGIN_DECLS

#define PK_TYPE_POST_TRANS		(pk_post_trans_get_type ())
#define PK_POST_TRANS(o)		(G_TYPE_CHECK_INSTANCE_CAST ((o), PK_TYPE_POST_TRANS, PkPostTrans))
#define PK_POST_TRANS_CLASS(k)		(G_TYPE_CHECK_CLASS_CAST((k), PK_TYPE_POST_TRANS, PkPostTransClass))
#define PK_IS_POST_TRANS(o)		(G_TYPE_CHECK_INSTANCE_TYPE ((o), PK_TYPE_POST_TRANS))
#define PK_IS_POST_TRANS_CLASS(k)	(G_TYPE_CHECK_CLASS_TYPE ((k), PK_TYPE_POST_TRANS))
#define PK_POST_TRANS_GET_CLASS(o)	(G_TYPE_INSTANCE_GET_CLASS ((o), PK_TYPE_POST_TRANS, PkPostTransClass))

typedef struct PkPostTransPrivate PkPostTransPrivate;

typedef struct
{
	GObject		      parent;
	PkPostTransPrivate     *priv;
} PkPostTrans;

typedef struct
{
	GObjectClass	parent_class;
} PkPostTransClass;

GType		 pk_post_trans_get_type			(void) G_GNUC_CONST;
PkPostTrans	*pk_post_trans_new			(void);

gboolean	 pk_post_trans_clear_firmware_requests	(PkPostTrans	*post);
gboolean	 pk_post_trans_update_package_list	(PkPostTrans	*post);
gboolean	 pk_post_trans_import_desktop_files	(PkPostTrans	*post);
gboolean	 pk_post_trans_check_running_process	(PkPostTrans	*post,
							 gchar		**package_ids);
gboolean	 pk_post_trans_check_desktop_files	(PkPostTrans	*post,
							 gchar		**package_ids);

G_END_DECLS

#endif /* __PK_POST_TRANS_H */

