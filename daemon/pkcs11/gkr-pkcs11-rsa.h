/* -*- Mode: C; indent-tabs-mode: t; c-basic-offset: 8; tab-width: 8 -*- */
/* gkr-pkcs11-rsa.h - RSA mechanism code for PKCS#11

   Copyright (C) 2007, Stefan Walter

   The Gnome Keyring Library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Library General Public License as
   published by the Free Software Foundation; either version 2 of the
   License, or (at your option) any later version.

   The Gnome Keyring Library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Library General Public License for more details.

   You should have received a copy of the GNU Library General Public
   License along with the Gnome Library; see the file COPYING.LIB.  If not,
   write to the Free Software Foundation, Inc., 59 Temple Place - Suite 330,
   Boston, MA 02111-1307, USA.

   Author: Stef Walter <stef@memberwebs.com>
*/

#ifndef GKRPKCS11RSA_H_
#define GKRPKCS11RSA_H_

#include "common/gkr-crypto.h"

#include "pk/gkr-pk-object.h"

#include "pkcs11/pkcs11.h"

CK_RV                 gkr_pkcs11_rsa_encrypt                (GkrPkObject *key, GkrCryptoPadding padfunc,
                                                             const guchar *plain, gsize n_plain, 
                                                             guchar **encrypted, gsize *n_encrypted);

CK_RV                 gkr_pkcs11_rsa_decrypt                (GkrPkObject *key, GkrCryptoPadding padfunc,
                                                             const guchar *encrypted, gsize n_encrypted, 
                                                             guchar **plain, gsize *n_plain);

CK_RV                 gkr_pkcs11_rsa_sign                  (GkrPkObject *key, GkrCryptoPadding padfunc,
                                                            const guchar *data, gsize n_data, 
                                                            guchar **signature, gsize *n_signature);

CK_RV                 gkr_pkcs11_rsa_verify                 (GkrPkObject *key, GkrCryptoPadding padfunc,
                                                             const guchar *data, gsize n_data, 
                                                             const guchar *signature, gsize n_signature);

#endif /*GKRPKCS11RSA_H_*/