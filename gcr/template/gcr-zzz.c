/*
 * gnome-keyring
 *
 * Copyright (C) 2010 Stefan Walter
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation; either version 2.1 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 * 02111-1307, USA.
 */

#include "config.h"

#include "gcr-zzz.h"

static void
gcr_zzz_base_init (gpointer gobject_class)
{
	static volatile gsize initialized = 0;
	if (g_once_init_enter (&initialized)) {
		/* Add properties and signals to the interface */


		g_once_init_leave (&initialized, 1);
	}
}

GType
gcr_zzz_get_type (void)
{
	static GType type = 0;
	if (!type) {
		static const GTypeInfo info = {
			sizeof (GcrZzzIface),
			gcr_zzz_base_init,  /* base init */
			NULL,               /* base finalize */
			NULL,               /* class_init */
			NULL,               /* class finalize */
			NULL,               /* class data */
			0,
			0,                  /* n_preallocs */
			NULL,               /* instance init */
		};
		type = g_type_register_static (G_TYPE_INTERFACE, "GcrZzzIface", &info, 0);
		g_type_interface_add_prerequisite (type, G_TYPE_OBJECT);
	}

	return type;
}