/* -*- Mode: C; indent-tabs-mode: t; c-basic-offset: 8; tab-width: 8 -*- */
/* gck-data-der.c - parsing and serializing of common crypto DER structures 

   Copyright (C) 2007 Stefan Walter

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

#include "config.h"

#include "gck-crypto.h"
#include "gck-data-asn1.h"
#include "gck-data-der.h"
#include "gck-data-types.h"

#include <glib.h>
#include <gcrypt.h>
#include <libtasn1.h>

/* -----------------------------------------------------------------------------
 * QUARKS
 */

static GQuark OID_PKIX1_RSA;
static GQuark OID_PKIX1_DSA;

static GQuark OID_PBE_MD2_DES_CBC;
static GQuark OID_PBE_MD5_DES_CBC;
static GQuark OID_PBE_MD2_RC2_CBC;
static GQuark OID_PBE_MD5_RC2_CBC;
static GQuark OID_PBE_SHA1_DES_CBC;
static GQuark OID_PBE_SHA1_RC2_CBC;
static GQuark OID_PBES2;
static GQuark OID_PBKDF2;

static GQuark OID_DES_CBC;
static GQuark OID_DES_RC2_CBC;
static GQuark OID_DES_EDE3_CBC;
static GQuark OID_DES_RC5_CBC;

static GQuark OID_PKCS12_PBE_ARCFOUR_SHA1;
static GQuark OID_PKCS12_PBE_RC4_40_SHA1;
static GQuark OID_PKCS12_PBE_3DES_SHA1;
static GQuark OID_PKCS12_PBE_2DES_SHA1;
static GQuark OID_PKCS12_PBE_RC2_128_SHA1;
static GQuark OID_PKCS12_PBE_RC2_40_SHA1;

static void
init_quarks (void)
{
	static volatile gsize quarks_inited = 0;

	if (g_once_init_enter (&quarks_inited)) {

		#define QUARK(name, value) \
			name = g_quark_from_static_string(value)

		QUARK (OID_PKIX1_RSA, "1.2.840.113549.1.1.1");
		QUARK (OID_PKIX1_DSA, "1.2.840.10040.4.1");

		QUARK (OID_PBE_MD2_DES_CBC, "1.2.840.113549.1.5.1");
		QUARK (OID_PBE_MD5_DES_CBC, "1.2.840.113549.1.5.3");
		QUARK (OID_PBE_MD2_RC2_CBC, "1.2.840.113549.1.5.4");
		QUARK (OID_PBE_MD5_RC2_CBC, "1.2.840.113549.1.5.6");
		QUARK (OID_PBE_SHA1_DES_CBC, "1.2.840.113549.1.5.10");
		QUARK (OID_PBE_SHA1_RC2_CBC, "1.2.840.113549.1.5.11");
		
		QUARK (OID_PBES2, "1.2.840.113549.1.5.13");
		
		QUARK (OID_PBKDF2, "1.2.840.113549.1.5.12");
		
		QUARK (OID_DES_CBC, "1.3.14.3.2.7");
		QUARK (OID_DES_RC2_CBC, "1.2.840.113549.3.2");
		QUARK (OID_DES_EDE3_CBC, "1.2.840.113549.3.7");
		QUARK (OID_DES_RC5_CBC, "1.2.840.113549.3.9");
		
		QUARK (OID_PKCS12_PBE_ARCFOUR_SHA1, "1.2.840.113549.1.12.1.1");
		QUARK (OID_PKCS12_PBE_RC4_40_SHA1, "1.2.840.113549.1.12.1.2");
		QUARK (OID_PKCS12_PBE_3DES_SHA1, "1.2.840.113549.1.12.1.3");
		QUARK (OID_PKCS12_PBE_2DES_SHA1, "1.2.840.113549.1.12.1.4");
		QUARK (OID_PKCS12_PBE_RC2_128_SHA1, "1.2.840.113549.1.12.1.5");
		QUARK (OID_PKCS12_PBE_RC2_40_SHA1, "1.2.840.113549.1.12.1.6");
		
		#undef QUARK
		
		g_once_init_leave (&quarks_inited, 1);
	}
}

/* -----------------------------------------------------------------------------
 * KEY PARSING
 */

#define SEXP_PUBLIC_RSA  \
	"(public-key"    \
	"  (rsa"         \
	"    (n %m)"     \
	"    (e %m)))"

GckDataResult
gck_data_der_read_public_key_rsa (const guchar *data, gsize n_data, gcry_sexp_t *s_key)
{
	GckDataResult ret = GCK_DATA_UNRECOGNIZED;
	ASN1_TYPE asn = ASN1_TYPE_EMPTY;
	gcry_mpi_t n, e;
	int res;

	n = e = NULL;
	
	asn = gck_data_asn1_decode ("PK.RSAPublicKey", data, n_data);
	if (!asn)
		goto done;
		
	ret = GCK_DATA_FAILURE;
    
	if (!gck_data_asn1_read_mpi (asn, "modulus", &n) || 
	    !gck_data_asn1_read_mpi (asn, "publicExponent", &e))
		goto done;
		
	res = gcry_sexp_build (s_key, NULL, SEXP_PUBLIC_RSA, n, e);
	if (res)
		goto done;

	g_assert (*s_key);
	ret = GCK_DATA_SUCCESS;

done:
	if (asn)
		asn1_delete_structure (&asn);
	gcry_mpi_release (n);
	gcry_mpi_release (e);
	
	if (ret == GCK_DATA_FAILURE)
		g_message ("invalid RSA public key");
		
	return ret;
}

#define SEXP_PRIVATE_RSA  \
	"(private-key"   \
	"  (rsa"         \
	"    (n %m)"     \
	"    (e %m)"     \
	"    (d %m)"     \
	"    (p %m)"     \
	"    (q %m)"     \
	"    (u %m)))"

GckDataResult
gck_data_der_read_private_key_rsa (const guchar *data, gsize n_data, gcry_sexp_t *s_key)
{
	GckDataResult ret = GCK_DATA_UNRECOGNIZED;
	gcry_mpi_t n, e, d, p, q, u;
	gcry_mpi_t tmp;
	guint version;
	int res;
	ASN1_TYPE asn = ASN1_TYPE_EMPTY;

	n = e = d = p = q = u = NULL;
	
	asn = gck_data_asn1_decode ("PK.RSAPrivateKey", data, n_data);
	if (!asn)
		goto done;
		
	ret = GCK_DATA_FAILURE;
	
	if (!gck_data_asn1_read_uint (asn, "version", &version))
		goto done;
	
	/* We only support simple version */
	if (version != 0) {
		ret = GCK_DATA_UNRECOGNIZED;
		g_message ("unsupported version of RSA key: %u", version);
		goto done;
	}
    
	if (!gck_data_asn1_read_secure_mpi (asn, "modulus", &n) || 
	    !gck_data_asn1_read_secure_mpi (asn, "publicExponent", &e) ||
	    !gck_data_asn1_read_secure_mpi (asn, "privateExponent", &d) ||
	    !gck_data_asn1_read_secure_mpi (asn, "prime1", &p) ||
	    !gck_data_asn1_read_secure_mpi (asn, "prime2", &q) || 
	    !gck_data_asn1_read_secure_mpi (asn, "coefficient", &u))
		goto done;
		
	/* Fix up the incoming key so gcrypt likes it */    	
	if (gcry_mpi_cmp (p, q) > 0) {
		/* P shall be smaller then Q!  Swap primes.  iqmp becomes u.  */
		tmp = p;
		p = q;
		q = tmp;
	} else {
		/* U needs to be recomputed.  */
		gcry_mpi_invm (u, p, q);
	}

	res = gcry_sexp_build (s_key, NULL, SEXP_PRIVATE_RSA, n, e, d, p, q, u);
	if (res)
		goto done;

	g_assert (*s_key);
	ret = GCK_DATA_SUCCESS;

done:
	if (asn)
		asn1_delete_structure (&asn);
	gcry_mpi_release (n);
	gcry_mpi_release (e);
	gcry_mpi_release (d);
	gcry_mpi_release (p);
	gcry_mpi_release (q);
	gcry_mpi_release (u);
	
	if (ret == GCK_DATA_FAILURE)
		g_message ("invalid RSA key");
		
	return ret;
}

#define SEXP_PUBLIC_DSA  \
	"(public-key"   \
	"  (dsa"         \
	"    (p %m)"     \
	"    (q %m)"     \
	"    (g %m)"     \
	"    (y %m)))"

GckDataResult
gck_data_der_read_public_key_dsa (const guchar *data, gsize n_data, gcry_sexp_t *s_key)
{
	GckDataResult ret = GCK_DATA_UNRECOGNIZED;
	ASN1_TYPE asn = ASN1_TYPE_EMPTY;
	gcry_mpi_t p, q, g, y;
	int res;

	p = q = g = y = NULL;
	
	asn = gck_data_asn1_decode ("PK.DSAPublicKey", data, n_data);
	if (!asn)
		goto done;
	
	ret = GCK_DATA_FAILURE;
    
	if (!gck_data_asn1_read_mpi (asn, "p", &p) || 
	    !gck_data_asn1_read_mpi (asn, "q", &q) ||
	    !gck_data_asn1_read_mpi (asn, "g", &g) ||
	    !gck_data_asn1_read_mpi (asn, "Y", &y))
	    	goto done;

	res = gcry_sexp_build (s_key, NULL, SEXP_PUBLIC_DSA, p, q, g, y);
	if (res)
		goto done;
		
	g_assert (*s_key);
	ret = GCK_DATA_SUCCESS;
	
done:
	if (asn)
		asn1_delete_structure (&asn);
	gcry_mpi_release (p);
	gcry_mpi_release (q);
	gcry_mpi_release (g);
	gcry_mpi_release (y);
	
	if (ret == GCK_DATA_FAILURE) 
		g_message ("invalid public DSA key");
		
	return ret;	
}

GckDataResult
gck_data_der_read_public_key_dsa_parts (const guchar *keydata, gsize n_keydata,
                                        const guchar *params, gsize n_params,
                                        gcry_sexp_t *s_key)
{
	gcry_mpi_t p, q, g, y;
	GckDataResult ret = GCK_DATA_UNRECOGNIZED;
	ASN1_TYPE asn_params = ASN1_TYPE_EMPTY;
	ASN1_TYPE asn_key = ASN1_TYPE_EMPTY;
	int res;

	p = q = g = y = NULL;
	
	asn_params = gck_data_asn1_decode ("PK.DSAParameters", params, n_params);
	asn_key = gck_data_asn1_decode ("PK.DSAPublicPart", keydata, n_keydata);
	if (!asn_params || !asn_key)
		goto done;
	
	ret = GCK_DATA_FAILURE;
    
	if (!gck_data_asn1_read_mpi (asn_params, "p", &p) || 
	    !gck_data_asn1_read_mpi (asn_params, "q", &q) ||
	    !gck_data_asn1_read_mpi (asn_params, "g", &g))
	    	goto done;
	    	
	if (!gck_data_asn1_read_mpi (asn_key, "", &y))
		goto done;

	res = gcry_sexp_build (s_key, NULL, SEXP_PUBLIC_DSA, p, q, g, y);
	if (res)
		goto done;
		
	g_assert (*s_key);
	ret = GCK_DATA_SUCCESS;
	
done:
	if (asn_key)
		asn1_delete_structure (&asn_key);
	if (asn_params)
		asn1_delete_structure (&asn_params);
	gcry_mpi_release (p);
	gcry_mpi_release (q);
	gcry_mpi_release (g);
	gcry_mpi_release (y);
	
	if (ret == GCK_DATA_FAILURE) 
		g_message ("invalid DSA key");
		
	return ret;	
}

#define SEXP_PRIVATE_DSA  \
	"(private-key"   \
	"  (dsa"         \
	"    (p %m)"     \
	"    (q %m)"     \
	"    (g %m)"     \
	"    (y %m)"     \
	"    (x %m)))"

GckDataResult
gck_data_der_read_private_key_dsa (const guchar *data, gsize n_data, gcry_sexp_t *s_key)
{
	gcry_mpi_t p, q, g, y, x;
	GckDataResult ret = GCK_DATA_UNRECOGNIZED;
	int res;
	ASN1_TYPE asn;

	p = q = g = y = x = NULL;
	
	asn = gck_data_asn1_decode ("PK.DSAPrivateKey", data, n_data);
	if (!asn)
		goto done;
	
	ret = GCK_DATA_FAILURE;
    
	if (!gck_data_asn1_read_secure_mpi (asn, "p", &p) || 
	    !gck_data_asn1_read_secure_mpi (asn, "q", &q) ||
	    !gck_data_asn1_read_secure_mpi (asn, "g", &g) ||
	    !gck_data_asn1_read_secure_mpi (asn, "Y", &y) ||
	    !gck_data_asn1_read_secure_mpi (asn, "priv", &x))
		goto done;
		
	res = gcry_sexp_build (s_key, NULL, SEXP_PRIVATE_DSA, p, q, g, y, x);
	if (res)
		goto done;
		
	g_assert (*s_key);
	ret = GCK_DATA_SUCCESS;

done:
	if (asn)
		asn1_delete_structure (&asn);
	gcry_mpi_release (p);
	gcry_mpi_release (q);
	gcry_mpi_release (g);
	gcry_mpi_release (y);
	gcry_mpi_release (x);
	
	if (ret == GCK_DATA_FAILURE) 
		g_message ("invalid DSA key");
		
	return ret;
}

GckDataResult
gck_data_der_read_private_key_dsa_parts (const guchar *keydata, gsize n_keydata,
                                         const guchar *params, gsize n_params, 
                                         gcry_sexp_t *s_key)
{
	gcry_mpi_t p, q, g, y, x;
	GckDataResult ret = GCK_DATA_UNRECOGNIZED;
	int res;
	ASN1_TYPE asn_params = ASN1_TYPE_EMPTY;
	ASN1_TYPE asn_key = ASN1_TYPE_EMPTY;

	p = q = g = y = x = NULL;
	
	asn_params = gck_data_asn1_decode ("PK.DSAParameters", params, n_params);
	asn_key = gck_data_asn1_decode ("PK.DSAPrivatePart", keydata, n_keydata);
	if (!asn_params || !asn_key)
		goto done;
	
	ret = GCK_DATA_FAILURE;
    
	if (!gck_data_asn1_read_secure_mpi (asn_params, "p", &p) || 
	    !gck_data_asn1_read_secure_mpi (asn_params, "q", &q) ||
	    !gck_data_asn1_read_secure_mpi (asn_params, "g", &g))
	    	goto done;
	    	
	if (!gck_data_asn1_read_secure_mpi (asn_key, "", &x))
		goto done;

	/* Now we calculate y */
	y = gcry_mpi_snew (1024);
  	gcry_mpi_powm (y, g, x, p);

	res = gcry_sexp_build (s_key, NULL, SEXP_PRIVATE_DSA, p, q, g, y, x);
	if (res)
		goto done;
		
	g_assert (*s_key);
	ret = GCK_DATA_SUCCESS;
	
done:
	if (asn_key)
		asn1_delete_structure (&asn_key);
	if (asn_params)
		asn1_delete_structure (&asn_params);
	gcry_mpi_release (p);
	gcry_mpi_release (q);
	gcry_mpi_release (g);
	gcry_mpi_release (y);
	gcry_mpi_release (x);
	
	if (ret == GCK_DATA_FAILURE) 
		g_message ("invalid DSA key");
		
	return ret;	
}

GckDataResult  
gck_data_der_read_public_key (const guchar *data, gsize n_data, gcry_sexp_t *s_key)
{
	GckDataResult res;
	
	res = gck_data_der_read_public_key_rsa (data, n_data, s_key);
	if (res == GCK_DATA_UNRECOGNIZED)
		res = gck_data_der_read_public_key_dsa (data, n_data, s_key);
		
	return res;
}

GckDataResult
gck_data_der_read_public_key_info (const guchar* data, gsize n_data, gcry_sexp_t* s_key)
{
	GckDataResult ret = GCK_DATA_UNRECOGNIZED;
	GQuark oid;
	ASN1_TYPE asn;
	gsize n_key, n_params;
	const guchar *params;
	guchar *key = NULL;
	
	init_quarks ();

	asn = gck_data_asn1_decode ("PKIX1.SubjectPublicKeyInfo", data, n_data);
	if (!asn)
		goto done;
	
	ret = GCK_DATA_FAILURE;
    
	/* Figure out the algorithm */
	oid = gck_data_asn1_read_oid (asn, "algorithm.algorithm");
	if (!oid)
		goto done;
		
	/* A bit string so we cannot process in place */
	key = gck_data_asn1_read_value (asn, "subjectPublicKey", &n_key, NULL);
	if (!key)
		goto done;
	n_key /= 8;
		
	/* An RSA key is simple */
	if (oid == OID_PKIX1_RSA) {
		ret = gck_data_der_read_public_key_rsa (key, n_key, s_key);
		
	/* A DSA key paramaters are stored separately */
	} else if (oid == OID_PKIX1_DSA) {
		params = gck_data_asn1_read_element (asn, data, n_data, "algorithm.parameters", &n_params);
		if (!params)
			goto done;
		ret = gck_data_der_read_public_key_dsa_parts (key, n_key, params, n_params, s_key);
		
	} else {
		g_message ("unsupported key algorithm in certificate: %s", g_quark_to_string (oid));
		goto done;
	}
	
done:
	if (asn)
		asn1_delete_structure (&asn);
	
	g_free (key);
		
	if (ret == GCK_DATA_FAILURE)
		g_message ("invalid subject public-key info");
		
	return ret;
}

GckDataResult
gck_data_der_read_private_key (const guchar *data, gsize n_data, gcry_sexp_t *s_key)
{
	GckDataResult res;
	
	res = gck_data_der_read_private_key_rsa (data, n_data, s_key);
	if (res == GCK_DATA_UNRECOGNIZED)
		res = gck_data_der_read_private_key_dsa (data, n_data, s_key);
		
	return res;
}

guchar*
gck_data_der_write_public_key_rsa (gcry_sexp_t s_key, gsize *len)
{
	ASN1_TYPE asn = ASN1_TYPE_EMPTY;
	gcry_mpi_t n, e;
	guchar *result = NULL;
	int res;

	n = e = NULL;

	res = asn1_create_element (gck_data_asn1_get_pk_asn1type (), 
	                           "PK.RSAPublicKey", &asn);
	g_return_val_if_fail (res == ASN1_SUCCESS, NULL);

	if (!gck_crypto_sexp_extract_mpi (s_key, &n, "rsa", "n", NULL) || 
	    !gck_crypto_sexp_extract_mpi (s_key, &e, "rsa", "e", NULL))
	    	goto done;
	
	if (!gck_data_asn1_write_mpi (asn, "modulus", n) ||
	    !gck_data_asn1_write_mpi (asn, "publicExponent", e))
	    	goto done;

	result = gck_data_asn1_encode (asn, "", len, NULL);
	
done:
	if (asn)
		asn1_delete_structure (&asn);
	gcry_mpi_release (n);
	gcry_mpi_release (e);
	
	return result;
}

guchar*
gck_data_der_write_private_key_rsa (gcry_sexp_t s_key, gsize *n_key)
{
	ASN1_TYPE asn = ASN1_TYPE_EMPTY;
	gcry_mpi_t n, e, d, p, q, u, e1, e2, tmp;
	guchar *result = NULL;
	int res;

	n = e = d = p = q = u = e1 = e2 = tmp = NULL;

	res = asn1_create_element (gck_data_asn1_get_pk_asn1type (), 
	                           "PK.RSAPrivateKey", &asn);
	g_return_val_if_fail (res == ASN1_SUCCESS, NULL);

	if (!gck_crypto_sexp_extract_mpi (s_key, &n, "rsa", "n", NULL) || 
	    !gck_crypto_sexp_extract_mpi (s_key, &e, "rsa", "e", NULL) ||
	    !gck_crypto_sexp_extract_mpi (s_key, &d, "rsa", "d", NULL) ||
	    !gck_crypto_sexp_extract_mpi (s_key, &p, "rsa", "p", NULL) ||
	    !gck_crypto_sexp_extract_mpi (s_key, &q, "rsa", "q", NULL) ||
	    !gck_crypto_sexp_extract_mpi (s_key, &u, "rsa", "u", NULL))
		goto done;
	
	if (!gck_data_asn1_write_mpi (asn, "modulus", n) ||
	    !gck_data_asn1_write_mpi (asn, "publicExponent", e) || 
	    !gck_data_asn1_write_mpi (asn, "privateExponent", d) ||
	    !gck_data_asn1_write_mpi (asn, "prime1", p) ||
	    !gck_data_asn1_write_mpi (asn, "prime2", q) ||
	    !gck_data_asn1_write_mpi (asn, "coefficient", u))
		goto done;

	/* Calculate e1 and e2 */
	tmp = gcry_mpi_snew (1024);
	gcry_mpi_sub_ui (tmp, p, 1);
	e1 = gcry_mpi_snew (1024);
	gcry_mpi_mod (e1, d, tmp);
	gcry_mpi_sub_ui (tmp, q, 1);
	e2 = gcry_mpi_snew (1024);
	gcry_mpi_mod (e2, d, tmp);
	
	/* Write out calculated */
	if (!gck_data_asn1_write_mpi (asn, "exponent1", e1) ||
	    !gck_data_asn1_write_mpi (asn, "exponent2", e2))
		goto done;

	/* Write out the version */
	if (!gck_data_asn1_write_uint (asn, "version", 0))
		goto done;

	result = gck_data_asn1_encode (asn, "", n_key, NULL);
	
done:
	if (asn)
		asn1_delete_structure (&asn);
	gcry_mpi_release (n);
	gcry_mpi_release (e);
	gcry_mpi_release (d);
	gcry_mpi_release (p);
	gcry_mpi_release (q);
	gcry_mpi_release (u);
	
	gcry_mpi_release (tmp);
	gcry_mpi_release (e1);
	gcry_mpi_release (e2);
	
	return result;
}

guchar*
gck_data_der_write_public_key_dsa (gcry_sexp_t s_key, gsize *len)
{
	ASN1_TYPE asn = ASN1_TYPE_EMPTY;
	gcry_mpi_t p, q, g, y;
	guchar *result = NULL;
	int res;

	p = q = g = y = NULL;

	res = asn1_create_element (gck_data_asn1_get_pk_asn1type (), 
	                           "PK.DSAPublicKey", &asn);
	g_return_val_if_fail (res == ASN1_SUCCESS, NULL);

	if (!gck_crypto_sexp_extract_mpi (s_key, &p, "dsa", "p", NULL) || 
	    !gck_crypto_sexp_extract_mpi (s_key, &q, "dsa", "q", NULL) ||
	    !gck_crypto_sexp_extract_mpi (s_key, &g, "dsa", "g", NULL) ||
	    !gck_crypto_sexp_extract_mpi (s_key, &y, "dsa", "y", NULL))
	    	goto done;
	
	if (!gck_data_asn1_write_mpi (asn, "p", p) ||
	    !gck_data_asn1_write_mpi (asn, "q", q) ||
	    !gck_data_asn1_write_mpi (asn, "g", g) ||
	    !gck_data_asn1_write_mpi (asn, "Y", y))
	    	goto done;

	if (!gck_data_asn1_write_uint (asn, "version", 0))
		goto done; 
		
	result = gck_data_asn1_encode (asn, "", len, NULL);
	
done:
	if (asn)
		asn1_delete_structure (&asn);
	gcry_mpi_release (p);
	gcry_mpi_release (q);
	gcry_mpi_release (g);
	gcry_mpi_release (y);
	
	return result;
}

guchar*
gck_data_der_write_private_key_dsa_part (gcry_sexp_t skey, gsize *n_key)
{
	ASN1_TYPE asn = ASN1_TYPE_EMPTY;
	gcry_mpi_t x;
	guchar *result = NULL;
	int res;

	x = NULL;

	res = asn1_create_element (gck_data_asn1_get_pk_asn1type (), 
	                           "PK.DSAPrivatePart", &asn);
	g_return_val_if_fail (res == ASN1_SUCCESS, NULL);

	if (!gck_crypto_sexp_extract_mpi (skey, &x, "dsa", "x", NULL))
	    	goto done;
	
	if (!gck_data_asn1_write_mpi (asn, "", x))
	    	goto done;

	result = gck_data_asn1_encode (asn, "", n_key, NULL);
	
done:
	if (asn)
		asn1_delete_structure (&asn);
	gcry_mpi_release (x);
	
	return result;		
}

guchar*
gck_data_der_write_private_key_dsa_params (gcry_sexp_t skey, gsize *n_params)
{
	ASN1_TYPE asn = ASN1_TYPE_EMPTY;
	gcry_mpi_t p, q, g;
	guchar *result = NULL;
	int res;

	p = q = g = NULL;

	res = asn1_create_element (gck_data_asn1_get_pk_asn1type (), 
	                           "PK.DSAParameters", &asn);
	g_return_val_if_fail (res == ASN1_SUCCESS, NULL);

	if (!gck_crypto_sexp_extract_mpi (skey, &p, "dsa", "p", NULL) || 
	    !gck_crypto_sexp_extract_mpi (skey, &q, "dsa", "q", NULL) ||
	    !gck_crypto_sexp_extract_mpi (skey, &g, "dsa", "g", NULL))
	    	goto done;
	
	if (!gck_data_asn1_write_mpi (asn, "p", p) ||
	    !gck_data_asn1_write_mpi (asn, "q", q) ||
	    !gck_data_asn1_write_mpi (asn, "g", g))
	    	goto done;

	result = gck_data_asn1_encode (asn, "", n_params, NULL);
	
done:
	if (asn)
		asn1_delete_structure (&asn);
	gcry_mpi_release (p);
	gcry_mpi_release (q);
	gcry_mpi_release (g);
	
	return result;
}

guchar*
gck_data_der_write_private_key_dsa (gcry_sexp_t s_key, gsize *len)
{
	ASN1_TYPE asn = ASN1_TYPE_EMPTY;
	gcry_mpi_t p, q, g, y, x;
	guchar *result = NULL;
	int res;

	p = q = g = y = x = NULL;

	res = asn1_create_element (gck_data_asn1_get_pk_asn1type (), 
	                           "PK.DSAPrivateKey", &asn);
	g_return_val_if_fail (res == ASN1_SUCCESS, NULL);

	if (!gck_crypto_sexp_extract_mpi (s_key, &p, "dsa", "p", NULL) || 
	    !gck_crypto_sexp_extract_mpi (s_key, &q, "dsa", "q", NULL) ||
	    !gck_crypto_sexp_extract_mpi (s_key, &g, "dsa", "g", NULL) ||
	    !gck_crypto_sexp_extract_mpi (s_key, &y, "dsa", "y", NULL) ||
	    !gck_crypto_sexp_extract_mpi (s_key, &x, "dsa", "x", NULL))
	    	goto done;
	
	if (!gck_data_asn1_write_mpi (asn, "p", p) ||
	    !gck_data_asn1_write_mpi (asn, "q", q) ||
	    !gck_data_asn1_write_mpi (asn, "g", g) ||
	    !gck_data_asn1_write_mpi (asn, "Y", y) ||
	    !gck_data_asn1_write_mpi (asn, "priv", x))
	    	goto done;

	if (!gck_data_asn1_write_uint (asn, "version", 0))
		goto done; 
		
	result = gck_data_asn1_encode (asn, "", len, NULL);
	
done:
	if (asn)
		asn1_delete_structure (&asn);
	gcry_mpi_release (p);
	gcry_mpi_release (q);
	gcry_mpi_release (g);
	gcry_mpi_release (y);
	gcry_mpi_release (x);
	
	return result;
}

guchar*
gck_data_der_write_public_key (gcry_sexp_t s_key, gsize *len)
{
	gboolean is_priv;
	int algorithm;
	
	g_return_val_if_fail (s_key != NULL, NULL);
	
	if (!gck_crypto_sexp_parse_key (s_key, &algorithm, &is_priv, NULL))
		g_return_val_if_reached (NULL);
	
	g_return_val_if_fail (!is_priv, NULL);
		
	switch (algorithm) {
	case GCRY_PK_RSA:
		return gck_data_der_write_public_key_rsa (s_key, len);
	case GCRY_PK_DSA:
		return gck_data_der_write_public_key_dsa (s_key, len);
	default:
		g_return_val_if_reached (NULL);
	}
}

guchar*
gck_data_der_write_private_key (gcry_sexp_t s_key, gsize *len)
{
	gboolean is_priv;
	int algorithm;
	
	g_return_val_if_fail (s_key != NULL, NULL);
	
	if (!gck_crypto_sexp_parse_key (s_key, &algorithm, &is_priv, NULL))
		g_return_val_if_reached (NULL);
	
	g_return_val_if_fail (is_priv, NULL);
		
	switch (algorithm) {
	case GCRY_PK_RSA:
		return gck_data_der_write_private_key_rsa (s_key, len);
	case GCRY_PK_DSA:
		return gck_data_der_write_private_key_dsa (s_key, len);
	default:
		g_return_val_if_reached (NULL);
	}
}

/* -----------------------------------------------------------------------------
 * CERTIFICATES
 */
 
GckDataResult
gck_data_der_read_certificate (const guchar *data, gsize n_data, ASN1_TYPE *asn1)
{
	*asn1 = gck_data_asn1_decode ("PKIX1.Certificate", data, n_data);
	if (!*asn1)
		return GCK_DATA_UNRECOGNIZED;
	
	return GCK_DATA_SUCCESS;
}

GckDataResult
gck_data_der_read_basic_constraints (const guchar *data, gsize n_data, 
                                     gboolean *is_ca, gint *path_len)
{
	GckDataResult ret = GCK_DATA_UNRECOGNIZED;
	ASN1_TYPE asn;
	guint value;

	asn = gck_data_asn1_decode ("PKIX1.BasicConstraints", data, n_data);
	if (!asn)
		goto done;
	
	ret = GCK_DATA_FAILURE;
    
    	if (path_len) {
    		if (!gck_data_asn1_read_uint (asn, "pathLenConstraint", &value))
    			*path_len = -1;
    		else
    			*path_len = value;
    	}
    	
    	if (is_ca) {
    		if (!gck_data_asn1_read_boolean (asn, "cA", is_ca))
    			*is_ca = FALSE;
    	}
    	
	ret = GCK_DATA_SUCCESS;

done:
	if (asn)
		asn1_delete_structure (&asn);
	
	if (ret == GCK_DATA_FAILURE) 
		g_message ("invalid basic constraints");
		
	return ret;
}

#ifdef UNTESTED_CODE

GckDataResult
gck_data_der_read_key_usage (const guchar *data, gsize n_data, guint *key_usage)
{
	GckDataResult ret = GCK_DATA_UNRECOGNIZED;
	ASN1_TYPE asn;
	guchar buf[4];
	int res, len;
	
	asn = gck_data_asn1_decode ("PKIX1.KeyUsage", data, n_data);
	if (!asn)
		goto done;
		
	ret = GCK_DATA_FAILURE;

	memset (buf, 0, sizeof (buf));
	len = sizeof (buf);
  	res = asn1_read_value (asn, "", buf, &len);
  	if (res != ASN1_SUCCESS)
  		goto done;

	*key_usage = buf[0] | (buf[1] << 8);
	ret = GCK_DATA_SUCCESS;
	
done:
	if (asn)
		asn1_delete_structure (&asn);		
	return ret;
}

GckDataResult
gck_data_der_read_enhanced_usage (const guchar *data, gsize n_data, GQuark **usage_oids)
{
	GckDataResult ret = GCK_DATA_UNRECOGNIZED;
	ASN1_TYPE asn;
	gchar *part;
	GArray *array;
	GQuark oid;
	int i;
	
	asn = gck_data_asn1_decode ("PKIX1.ExtKeyUsageSyntax", data, n_data);
	if (!asn)
		goto done;
		
	ret = GCK_DATA_FAILURE;
	
	array = g_array_new (TRUE, TRUE, sizeof (GQuark));
	for (i = 0; TRUE; ++i) {
		part = g_strdup_printf ("?%d", i + 1);
		oid = gck_data_asn1_read_oid (asn, part);
		g_free (part);
		
		if (!oid) 
			break;
		
		g_array_append_val (array, oid);
	}
	
	*usage_oids = (GQuark*)g_array_free (array, FALSE);
	ret = GCK_DATA_SUCCESS;
	
done:
	if (asn)
		asn1_delete_structure (&asn);
	return ret;
}

guchar*
gck_data_der_write_certificate (ASN1_TYPE asn1, gsize *n_data)
{
	g_return_val_if_fail (asn1, NULL);
	g_return_val_if_fail (n_data, NULL);
	
	return gck_data_asn1_encode (asn1, "", n_data, NULL);
}

/* -----------------------------------------------------------------------------
 * CIPHER/KEY DESCRIPTIONS 
 */
 
GckDataResult
gck_data_der_read_cipher (GQuark oid_scheme, const gchar *password, 
                          const guchar *data, gsize n_data, gcry_cipher_hd_t *cih)
{
	GckDataResult ret = GCK_DATA_UNRECOGNIZED;
	
	g_return_val_if_fail (oid_scheme != 0, GCK_DATA_FAILURE);
	g_return_val_if_fail (cih != NULL, GCK_DATA_FAILURE);
	g_return_val_if_fail (data != NULL && n_data != 0, GCK_DATA_FAILURE);
	
	init_quarks ();
	
	/* PKCS#5 PBE */
	if (oid_scheme == OID_PBE_MD2_DES_CBC)
		ret = gck_data_der_read_cipher_pkcs5_pbe (GCRY_CIPHER_DES, GCRY_CIPHER_MODE_CBC,
		                                          GCRY_MD_MD2, password, data, n_data, cih);

	else if (oid_scheme == OID_PBE_MD2_RC2_CBC)
		/* RC2-64 has no implementation in libgcrypt */
		ret = GCK_DATA_UNRECOGNIZED;
	else if (oid_scheme == OID_PBE_MD5_DES_CBC)
		ret = gck_data_der_read_cipher_pkcs5_pbe (GCRY_CIPHER_DES, GCRY_CIPHER_MODE_CBC,
		                                          GCRY_MD_MD5, password, data, n_data, cih);
	else if (oid_scheme == OID_PBE_MD5_RC2_CBC)
		/* RC2-64 has no implementation in libgcrypt */
		ret = GCK_DATA_UNRECOGNIZED;
	else if (oid_scheme == OID_PBE_SHA1_DES_CBC)
		ret = gck_data_der_read_cipher_pkcs5_pbe (GCRY_CIPHER_DES, GCRY_CIPHER_MODE_CBC,
		                                          GCRY_MD_SHA1, password, data, n_data, cih);
	else if (oid_scheme == OID_PBE_SHA1_RC2_CBC)
		/* RC2-64 has no implementation in libgcrypt */
		ret = GCK_DATA_UNRECOGNIZED;

	
	/* PKCS#5 PBES2 */
	else if (oid_scheme == OID_PBES2)
		ret = gck_data_der_read_cipher_pkcs5_pbes2 (password, data, n_data, cih);

		
	/* PKCS#12 PBE */
	else if (oid_scheme == OID_PKCS12_PBE_ARCFOUR_SHA1)
		ret = gck_data_der_read_cipher_pkcs12_pbe (GCRY_CIPHER_ARCFOUR, GCRY_CIPHER_MODE_STREAM, 
                                                   password, data, n_data, cih);
	else if (oid_scheme == OID_PKCS12_PBE_RC4_40_SHA1)
		/* RC4-40 has no implementation in libgcrypt */;

	else if (oid_scheme == OID_PKCS12_PBE_3DES_SHA1)
		ret = gck_data_der_read_cipher_pkcs12_pbe (GCRY_CIPHER_3DES, GCRY_CIPHER_MODE_CBC, 
                                                   password, data, n_data, cih);
	else if (oid_scheme == OID_PKCS12_PBE_2DES_SHA1) 
		/* 2DES has no implementation in libgcrypt */;
		
	else if (oid_scheme == OID_PKCS12_PBE_RC2_128_SHA1)
		ret = gck_data_der_read_cipher_pkcs12_pbe (GCRY_CIPHER_RFC2268_128, GCRY_CIPHER_MODE_CBC, 
                                                   password, data, n_data, cih);

	else if (oid_scheme == OID_PKCS12_PBE_RC2_40_SHA1)
		ret = gck_data_der_read_cipher_pkcs12_pbe (GCRY_CIPHER_RFC2268_40, GCRY_CIPHER_MODE_CBC, 
                                                   password, data, n_data, cih);

	if (ret == GCK_DATA_UNRECOGNIZED)
    		g_message ("unsupported or unrecognized cipher oid: %s", g_quark_to_string (oid_scheme));
    	return ret;
}

GckDataResult
gck_data_der_read_cipher_pkcs5_pbe (int cipher_algo, int cipher_mode, 
                                    int hash_algo, const gchar *password, const guchar *data, 
                                    gsize n_data, gcry_cipher_hd_t *cih)
{
	ASN1_TYPE asn = ASN1_TYPE_EMPTY;
	gcry_error_t gcry;
	GckDataResult ret;
	const guchar *salt;
	gsize n_salt;
	gsize n_block, n_key;
	guint iterations;
	guchar *key = NULL;
	guchar *iv = NULL;

	g_return_val_if_fail (cipher_algo != 0 && cipher_mode != 0, GCK_DATA_FAILURE);
	g_return_val_if_fail (cih != NULL, GCK_DATA_FAILURE);
	g_return_val_if_fail (data != NULL && n_data != 0, GCK_DATA_FAILURE);

	*cih = NULL;	
	ret = GCK_DATA_UNRECOGNIZED;
	
	/* Check if we can use this algorithm */
	if (gcry_cipher_algo_info (cipher_algo, GCRYCTL_TEST_ALGO, NULL, 0) != 0 ||
	    gcry_md_test_algo (hash_algo) != 0)
		goto done;
	
	asn = gck_data_asn1_decode ("PKIX1.pkcs-5-PBE-params", data, n_data);
	if (!asn) 
		goto done;
		
	ret = GCK_DATA_FAILURE;
		
	salt = gck_data_asn1_read_content (asn, data, n_data, "salt", &n_salt);
	if (!salt)
		goto done;
	if (!gck_data_asn1_read_uint (asn, "iterationCount", &iterations))
		iterations = 1;
		
	n_key = gcry_cipher_get_algo_keylen (cipher_algo);
	g_return_val_if_fail (n_key > 0, GCK_DATA_FAILURE);
	n_block = gcry_cipher_get_algo_blklen (cipher_algo);
		
	if (!gck_crypto_symkey_generate_pbe (cipher_algo, hash_algo, password, salt,
	                                     n_salt, iterations, &key, n_block > 1 ? &iv : NULL))
		goto done;
		
	gcry = gcry_cipher_open (cih, cipher_algo, cipher_mode, 0);
	if (gcry != 0) {
		g_warning ("couldn't create cipher: %s", gcry_strerror (gcry));
		goto done;
	}
	
	if (iv) 
		gcry_cipher_setiv (*cih, iv, n_block);
	gcry_cipher_setkey (*cih, key, n_key);
	
	ret = GCK_DATA_SUCCESS;

done:
	gcry_free (iv);
	gcry_free (key);
	
	if (asn)
		asn1_delete_structure (&asn);
		
	return ret;
}

static gboolean
setup_pkcs5_rc2_params (const guchar *data, guchar n_data, gcry_cipher_hd_t cih)
{
	ASN1_TYPE asn = ASN1_TYPE_EMPTY;
	gcry_error_t gcry;
	const guchar *iv;
	gsize n_iv;
	guint version;
	
	g_assert (data);

	asn = gck_data_asn1_decode ("PKIX1.pkcs-5-rc2-CBC-params", data, n_data);
	if (!asn) 
		return GCK_DATA_UNRECOGNIZED;
		
	if (!gck_data_asn1_read_uint (asn, "rc2ParameterVersion", &version))
		return GCK_DATA_FAILURE;
	
	iv = gck_data_asn1_read_content (asn, data, n_data, "iv", &n_iv);
	asn1_delete_structure (&asn);

	if (!iv)
		return GCK_DATA_FAILURE;
		
	gcry = gcry_cipher_setiv (cih, iv, n_iv);
			
	if (gcry != 0) {
		g_message ("couldn't set %lu byte iv on cipher", (gulong)n_iv);
		return GCK_DATA_FAILURE;
	}
	
	return GCK_DATA_SUCCESS;
}

static gboolean
setup_pkcs5_des_params (const guchar *data, guchar n_data, gcry_cipher_hd_t cih)
{
	ASN1_TYPE asn = ASN1_TYPE_EMPTY;
	gcry_error_t gcry;
	const guchar *iv;
	gsize n_iv;
	
	g_assert (data);

	asn = gck_data_asn1_decode ("PKIX1.pkcs-5-des-EDE3-CBC-params", data, n_data);
	if (!asn)
		asn = gck_data_asn1_decode ("PKIX1.pkcs-5-des-CBC-params", data, n_data);
	if (!asn) 
		return GCK_DATA_UNRECOGNIZED;
	
	iv = gck_data_asn1_read_content (asn, data, n_data, "", &n_iv);
	asn1_delete_structure (&asn);

	if (!iv)
		return GCK_DATA_FAILURE;
		
	gcry = gcry_cipher_setiv (cih, iv, n_iv);
			
	if (gcry != 0) {
		g_message ("couldn't set %lu byte iv on cipher", (gulong)n_iv);
		return GCK_DATA_FAILURE;
	}
	
	return GCK_DATA_SUCCESS;
}

static GckDataResult
setup_pkcs5_pbkdf2_params (const gchar *password, const guchar *data, 
                           gsize n_data, int cipher_algo, gcry_cipher_hd_t cih)
{
	ASN1_TYPE asn = ASN1_TYPE_EMPTY;
	GckDataResult ret;
	gcry_error_t gcry;
	guchar *key = NULL; 
	const guchar *salt;
	gsize n_salt, n_key;
	guint iterations;
	
	g_assert (cipher_algo);
	g_assert (data);
	
	ret = GCK_DATA_UNRECOGNIZED;

	asn = gck_data_asn1_decode ("PKIX1.pkcs-5-PBKDF2-params", data, n_data);
	if (!asn)
		goto done;
		
	ret = GCK_DATA_FAILURE;
		
	if (!gck_data_asn1_read_uint (asn, "iterationCount", &iterations))
		iterations = 1;
	salt = gck_data_asn1_read_content (asn, data, n_data, "salt.specified", &n_salt);
	if (!salt)
		goto done;
				
	if (!gck_crypto_symkey_generate_pbkdf2 (cipher_algo, GCRY_MD_SHA1, password, 
	                                        salt, n_salt, iterations, &key, NULL))
		goto done;

	n_key = gcry_cipher_get_algo_keylen (cipher_algo);
	g_return_val_if_fail (n_key > 0, GCK_DATA_FAILURE);
	
	gcry = gcry_cipher_setkey (cih, key, n_key);
	if (gcry != 0) {
		g_message ("couldn't set %lu byte key on cipher", (gulong)n_key);
		goto done;
	}
	
	ret = GCK_DATA_SUCCESS;
	                                         
done:
	gcry_free (key);
	if (asn)
		asn1_delete_structure (&asn);
	return ret;
}

GckDataResult
gck_data_der_read_cipher_pkcs5_pbes2 (const gchar *password, const guchar *data, 
                                      gsize n_data, gcry_cipher_hd_t *cih)
{
	ASN1_TYPE asn = ASN1_TYPE_EMPTY;
	GckDataResult r, ret;
	GQuark key_deriv_algo, enc_oid;
	gcry_error_t gcry;
	int algo, mode;
	int beg, end, res;

	g_return_val_if_fail (cih != NULL, GCK_DATA_FAILURE);
	g_return_val_if_fail (data != NULL && n_data != 0, GCK_DATA_FAILURE);
	
	init_quarks ();
	
	*cih = NULL;
	ret = GCK_DATA_UNRECOGNIZED;
	
	asn = gck_data_asn1_decode ("PKIX1.pkcs-5-PBES2-params", data, n_data);
	if (!asn)
		goto done;
		
	res = GCK_DATA_FAILURE;
	algo = mode = 0;
	
	/* Read in all the encryption type */
	enc_oid = gck_data_asn1_read_oid (asn, "encryptionScheme.algorithm");
	if (!enc_oid)
		goto done;	
	if (enc_oid == OID_DES_EDE3_CBC)
		algo = GCRY_CIPHER_3DES;
	else if (enc_oid == OID_DES_CBC)
		algo = GCRY_CIPHER_DES;
	else if (enc_oid == OID_DES_RC2_CBC)
		algo = GCRY_CIPHER_RFC2268_128;
	else if (enc_oid == OID_DES_RC5_CBC)
		/* RC5 doesn't exist in libgcrypt */;
	
	/* Unsupported? */
	if (algo == 0 || gcry_cipher_algo_info (algo, GCRYCTL_TEST_ALGO, NULL, 0) != 0) {
		ret = GCK_DATA_UNRECOGNIZED;
		goto done;
	}

	/* Instantiate our cipher */
	gcry = gcry_cipher_open (cih, algo, GCRY_CIPHER_MODE_CBC, 0);
	if (gcry != 0) {
		g_warning ("couldn't create cipher: %s", gcry_cipher_algo_name (algo));
		goto done;
	}
		
	/* Read out the parameters */
	if (asn1_der_decoding_startEnd (asn, data, n_data, "encryptionScheme.parameters",
	                                &beg, &end) != ASN1_SUCCESS)
		goto done;
		
	switch (algo) {
	case GCRY_CIPHER_3DES:
	case GCRY_CIPHER_DES:
		r = setup_pkcs5_des_params (data + beg, end - beg + 1, *cih);
		break;
	case GCRY_CIPHER_RFC2268_128:
		r = setup_pkcs5_rc2_params (data + beg, end - beg + 1, *cih);
		break;
	default:
		/* Should have been caught on the oid check above */
		g_assert_not_reached ();
		r = GCK_DATA_UNRECOGNIZED;
		break;
	};

	if (r != GCK_DATA_SUCCESS) {
		ret = r;
		goto done;
	}

	/* Read out the key creation paramaters */
	key_deriv_algo = gck_data_asn1_read_oid (asn, "keyDerivationFunc.algorithm");
	if (!key_deriv_algo)
		goto done;
	if (key_deriv_algo != OID_PBKDF2) {
		g_message ("unsupported key derivation algorithm: %s", g_quark_to_string (key_deriv_algo));
		ret = GCK_DATA_UNRECOGNIZED;
		goto done;
	}

	if (asn1_der_decoding_startEnd (asn, data, n_data, "keyDerivationFunc.parameters",
	                                &beg, &end) != ASN1_SUCCESS)
		goto done;
	
	ret = setup_pkcs5_pbkdf2_params (password, data + beg, end - beg + 1, algo, *cih);

done:
	if (ret != GCK_DATA_SUCCESS && *cih) {
		gcry_cipher_close (*cih);
		*cih = NULL;
	}
	
	if (asn)
		asn1_delete_structure (&asn);
	
	return ret;
}

GckDataResult
gck_data_der_read_cipher_pkcs12_pbe (int cipher_algo, int cipher_mode, const gchar *password, 
                                     const guchar *data, gsize n_data, gcry_cipher_hd_t *cih)
{
	ASN1_TYPE asn = ASN1_TYPE_EMPTY;
	gcry_error_t gcry;
	GckDataResult ret;
	const guchar *salt;
	gsize n_salt;
	gsize n_block, n_key;
	guint iterations;
	guchar *key = NULL;
	guchar *iv = NULL;
	
	g_return_val_if_fail (cipher_algo != 0 && cipher_mode != 0, GCK_DATA_FAILURE);
	g_return_val_if_fail (cih != NULL, GCK_DATA_FAILURE);
	g_return_val_if_fail (data != NULL && n_data != 0, GCK_DATA_FAILURE);
	
	*cih = NULL;
	ret = GCK_DATA_UNRECOGNIZED;
	
	/* Check if we can use this algorithm */
	if (gcry_cipher_algo_info (cipher_algo, GCRYCTL_TEST_ALGO, NULL, 0) != 0)
		goto done;
	
	asn = gck_data_asn1_decode ("PKIX1.pkcs-12-PbeParams", data, n_data);
	if (!asn)
		goto done;

	ret = GCK_DATA_FAILURE;

	salt = gck_data_asn1_read_content (asn, data, n_data, "salt", &n_salt);
	if (!salt)
		goto done;
	if (!gck_data_asn1_read_uint (asn, "iterations", &iterations))
		goto done;
	
	n_block = gcry_cipher_get_algo_blklen (cipher_algo);
	n_key = gcry_cipher_get_algo_keylen (cipher_algo);
	
	/* Generate IV and key using salt read above */
	if (!gck_crypto_symkey_generate_pkcs12 (cipher_algo, GCRY_MD_SHA1, password,
	                                        salt, n_salt, iterations, &key, 
	                                        n_block > 1 ? &iv : NULL))
		goto done;
		
	gcry = gcry_cipher_open (cih, cipher_algo, cipher_mode, 0);
	if (gcry != 0) {
		g_warning ("couldn't create encryption cipher: %s", gcry_strerror (gcry));
		goto done;
	}
	
	if (iv) 
		gcry_cipher_setiv (*cih, iv, n_block);
	gcry_cipher_setkey (*cih, key, n_key);
	
	ret = GCK_DATA_SUCCESS;
	
done:
	if (ret != GCK_DATA_SUCCESS && *cih) {
		gcry_cipher_close (*cih);
		*cih = NULL;
	}
	
	gcry_free (iv);
	gcry_free (key);
	
	if (asn)
		asn1_delete_structure (&asn);
	
	return ret;
}

#endif /* UNTESTED_CODE */