/* 
 * gnome-keyring
 * 
 * Copyright (C) 2008 Stefan Walter
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

#ifndef __GCK_SSH_PUBLIC_KEY_H__
#define __GCK_SSH_PUBLIC_KEY_H__

#include <glib-object.h>

#include "gck/gck-public-xsa-key.h"

#define GCK_TYPE_SSH_PUBLIC_KEY               (gck_ssh_public_key_get_type ())
#define GCK_SSH_PUBLIC_KEY(obj)               (G_TYPE_CHECK_INSTANCE_CAST ((obj), GCK_TYPE_SSH_PUBLIC_KEY, GckSshPublicKey))
#define GCK_SSH_PUBLIC_KEY_CLASS(klass)       (G_TYPE_CHECK_CLASS_CAST ((klass), GCK_TYPE_SSH_PUBLIC_KEY, GckSshPublicKeyClass))
#define GCK_IS_SSH_PUBLIC_KEY(obj)            (G_TYPE_CHECK_INSTANCE_TYPE ((obj), GCK_TYPE_SSH_PUBLIC_KEY))
#define GCK_IS_SSH_PUBLIC_KEY_CLASS(klass)    (G_TYPE_CHECK_CLASS_TYPE ((klass), GCK_TYPE_SSH_PUBLIC_KEY))
#define GCK_SSH_PUBLIC_KEY_GET_CLASS(obj)     (G_TYPE_INSTANCE_GET_CLASS ((obj), GCK_TYPE_SSH_PUBLIC_KEY, GckSshPublicKeyClass))

typedef struct _GckSshPublicKey GckSshPublicKey;
typedef struct _GckSshPublicKeyClass GckSshPublicKeyClass;
    
struct _GckSshPublicKeyClass {
	GckPublicXsaKeyClass parent_class;
};

GType               gck_ssh_public_key_get_type               (void);

GckSshPublicKey*    gck_ssh_public_key_new                    (GckModule *self,
                                                               const gchar *unique);

const gchar*        gck_ssh_public_key_get_label              (GckSshPublicKey *key);

void                gck_ssh_public_key_set_label              (GckSshPublicKey *key,
                                                               const gchar *label);

#endif /* __GCK_SSH_PUBLIC_KEY_H__ */