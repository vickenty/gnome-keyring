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

#ifndef __GCK_TRANSACTION_H__
#define __GCK_TRANSACTION_H__

#include <glib-object.h>

#include "gck-types.h"

#include "pkcs11/pkcs11.h"

#define GCK_TYPE_TRANSACTION               (gck_transaction_get_type ())
#define GCK_TRANSACTION(obj)               (G_TYPE_CHECK_INSTANCE_CAST ((obj), GCK_TYPE_TRANSACTION, GckTransaction))
#define GCK_TRANSACTION_CLASS(klass)       (G_TYPE_CHECK_CLASS_CAST ((klass), GCK_TYPE_TRANSACTION, GckTransactionClass))
#define GCK_IS_TRANSACTION(obj)            (G_TYPE_CHECK_INSTANCE_TYPE ((obj), GCK_TYPE_TRANSACTION))
#define GCK_IS_TRANSACTION_CLASS(klass)    (G_TYPE_CHECK_CLASS_TYPE ((klass), GCK_TYPE_TRANSACTION))
#define GCK_TRANSACTION_GET_CLASS(obj)     (G_TYPE_INSTANCE_GET_CLASS ((obj), GCK_TYPE_TRANSACTION, GckTransactionClass))

typedef struct _GckTransactionClass GckTransactionClass;
    
struct _GckTransactionClass {
	GObjectClass parent_class;
    
	/* signals --------------------------------------------------------- */
    
	gboolean (*complete) (GckTransaction *transaction);
};

GType                       gck_transaction_get_type               (void);

GckTransaction*             gck_transaction_new                    (void);

typedef gboolean            (*GckTransactionFunc)                  (GckTransaction *self,
                                                                    GObject *object,
                                                                    gpointer user_data);

void                        gck_transaction_add                    (GckTransaction *self,
                                                                    gpointer object,
                                                                    GckTransactionFunc callback,
                                                                    gpointer user_data);

void                        gck_transaction_fail                   (GckTransaction *self,
                                                                    CK_RV result);

void                        gck_transaction_complete               (GckTransaction *self);

gboolean                    gck_transaction_get_failed             (GckTransaction *self);

CK_RV                       gck_transaction_get_result             (GckTransaction *self);

gboolean                    gck_transaction_get_completed          (GckTransaction *self);

void                        gck_transaction_write_file             (GckTransaction *self,
                                                                    const gchar *filename,
                                                                    const guchar *data,
                                                                    gsize n_data);

void                        gck_transaction_remove_file            (GckTransaction *self,
                                                                    const gchar *filename);

CK_RV                       gck_transaction_complete_and_unref     (GckTransaction *self);

#endif /* __GCK_TRANSACTION_H__ */