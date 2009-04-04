/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2007 Richard Hughes <richard@hughsie.com>
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

#ifndef __PK_SECURITY_H
#define __PK_SECURITY_H

#include <glib-object.h>
#include <packagekit-glib/packagekit.h>

G_BEGIN_DECLS

#define PK_TYPE_SECURITY		(pk_security_get_type ())
#define PK_SECURITY(o)			(G_TYPE_CHECK_INSTANCE_CAST ((o), PK_TYPE_SECURITY, PkSecurity))
#define PK_SECURITY_CLASS(k)		(G_TYPE_CHECK_CLASS_CAST((k), PK_TYPE_SECURITY, PkSecurityClass))
#define PK_IS_SECURITY(o)		(G_TYPE_CHECK_INSTANCE_TYPE ((o), PK_TYPE_SECURITY))
#define PK_IS_SECURITY_CLASS(k)		(G_TYPE_CHECK_CLASS_TYPE ((k), PK_TYPE_SECURITY))
#define PK_SECURITY_GET_CLASS(o)	(G_TYPE_INSTANCE_GET_CLASS ((o), PK_TYPE_SECURITY, PkSecurityClass))

/* not actually a role, but treated as one */
#define PK_ROLE_ENUM_SET_PROXY_PRIVATE		1 << 31

typedef struct PkSecurityPrivate PkSecurityPrivate;

typedef struct
{
	GObject		      parent;
	PkSecurityPrivate     *priv;
} PkSecurity;

typedef struct
{
	GObjectClass	parent_class;
} PkSecurityClass;

typedef struct PkSecurityCaller_ PkSecurityCaller;

GType		 pk_security_get_type		(void) G_GNUC_CONST;
PkSecurity	*pk_security_new		(void);

PkSecurityCaller *pk_security_caller_new_from_sender	(PkSecurity		*security,
							 const gchar		*sender);
void		 pk_security_caller_unref		(PkSecurityCaller	*caller);
guint		 pk_security_get_uid			(PkSecurity		*security,
							 PkSecurityCaller	*caller);
gchar 		*pk_security_get_cmdline		(PkSecurity		*security,
							 PkSecurityCaller	*caller);
gboolean	 pk_security_action_is_allowed		(PkSecurity		*security,
							 PkSecurityCaller	*caller,
							 gboolean		 trusted,
							 PkRoleEnum		 role,
							 gchar			**error_detail)
							 G_GNUC_WARN_UNUSED_RESULT;

G_END_DECLS

#endif /* __PK_SECURITY_H */
