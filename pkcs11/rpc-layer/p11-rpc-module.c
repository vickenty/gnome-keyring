/* -*- Mode: C; indent-tabs-mode: t; c-basic-offset: 8; tab-width: 8 -*- */
/* gkr-pkcs11-rpc-module.c - a PKCS#11 module which communicates with another process

   Copyright (C) 2008, Stefan Walter

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

#include "p11-rpc.h"
#include "p11-rpc-private.h"

#include "pkcs11/pkcs11.h"

#include <sys/types.h>
#include <sys/param.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <stdlib.h>
#include <limits.h>
#include <ctype.h>
#include <stdint.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* -------------------------------------------------------------------
 * GLOBALS / DEFINES
 */

/* Various mutexes */
static pthread_mutex_t init_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Our per thread key */
static pthread_key_t pkcs11_per_thread = 0;

/* Whether we've been initialized, and on what process id it happened */
static int pkcs11_initialized = 0;
static pid_t pkcs11_initialized_pid = 0;

/* The socket to connect to */
static char pkcs11_socket_path[MAXPATHLEN] = { 0, };

/* The error used by us when parsing of rpc message fails */
#define PARSE_ERROR   CKR_DEVICE_ERROR

/* -----------------------------------------------------------------------------
 * LOGGING and DEBUGGING
 */

#if DEBUG_OUTPUT
#define debug(x) p11_rpc_debug x
#else
#define debug(x)	
#endif 
#define warning(x) p11_rpc_warn x

#define return_val_if_fail(x, v) \
	if (!(x)) { p11_rpc_warn ("'%s' not true at %s", #x, __func__); return v; } 

/* -----------------------------------------------------------------------------
 * CALL SESSION
 */

enum CallStatus {
	CALL_INVALID,
	CALL_READY,
	CALL_PREP,
	CALL_TRANSIT,
	CALL_PARSE
};

typedef struct _CallState {
	int socket;                     /* The connection we're sending on */
	P11RpcMessage *req;          /* The current request */
	P11RpcMessage *resp;         /* The current response */
	int call_status;
} CallState;

/* Allocator for call session buffers */
static void*
call_allocator (void* p, unsigned long sz)
{
	void* res = realloc (p, (size_t)sz);
	if (!res && sz)
		warning (("memory allocation of %lu bytes failed", sz));
	return res;	
}

static CK_RV
call_connect (CallState *cs)
{
	struct sockaddr_un addr;
	int sock;

	assert (cs);
	assert (cs->socket == -1);
	assert (cs->call_status == CALL_INVALID);
	assert (pkcs11_socket_path[0]);
	
	debug (("connecting to: %s", pkcs11_socket_path));
		
	addr.sun_family = AF_UNIX;
	strncpy (addr.sun_path, pkcs11_socket_path, sizeof (addr.sun_path));
	
	sock = socket (AF_UNIX, SOCK_STREAM, 0);
	if (sock < 0) {
		warning (("couldn't open socket: %s", strerror (errno)));
		return CKR_DEVICE_ERROR;
	}

	/* close on exec */
	if (fcntl (sock, F_SETFD, 1) == -1) {
		close (sock);
		warning (("couldn't secure socket: %s", strerror (errno)));
		return CKR_DEVICE_ERROR;
	}

	if (connect (sock, (struct sockaddr*) &addr, sizeof (addr)) < 0) {
		close (sock);
		warning (("couldn't connect to: %s: %s", pkcs11_socket_path, strerror (errno)));
		return CKR_DEVICE_ERROR;
	}

	if (!p11_rpc_write_credentials (sock)) {
		close (sock);
		warning (("couldn't send socket credentials: %s", strerror (errno)));
		return CKR_DEVICE_ERROR;
	}

	cs->socket = sock;
	cs->call_status = CALL_READY;
	debug (("connected socket"));
	
	return CKR_OK;
}

static void
call_disconnect (CallState *cs)
{
	assert (cs);
	
	if (cs->socket != -1) {
		debug (("disconnected socket"));
		close (cs->socket);
		cs->socket = -1;
	}
}

static void
call_destroy (void *value)
{
	CallState *cs = value;
	
	if (value) {
		call_disconnect (cs);
		assert (cs->socket == -1);

		p11_rpc_message_free (cs->req);
		p11_rpc_message_free (cs->resp);
		
		free (cs);
		
		debug (("destroyed state"));
	}
}

static CK_RV
call_create (void)
{
	CallState *cs;
	
	assert (pkcs11_per_thread);
	
	cs = calloc(1, sizeof (CallState));
	if (!cs)
		return CKR_HOST_MEMORY;
	cs->socket = -1;
	cs->call_status = CALL_INVALID;
	
	assert (!pthread_getspecific (pkcs11_per_thread));
	pthread_setspecific (pkcs11_per_thread, cs);
	
	return CKR_OK;
}

static CK_RV
call_lookup (CallState **ret)
{
	CallState *cs;
	CK_RV rv;
	
	assert (ret);
	assert (pkcs11_per_thread);
	
	cs = pthread_getspecific (pkcs11_per_thread);
	if (cs == NULL) {
		rv = call_create ();
		if (rv != CKR_OK)
			return rv;

		cs = pthread_getspecific (pkcs11_per_thread);
		assert (cs);
	}

	if (cs->call_status == CALL_INVALID) {
		rv = call_connect (cs);
		if (rv != CKR_OK)
			return rv;
	}
	
	assert (cs->call_status == CALL_READY);
	assert (cs->socket != -1);
	*ret = cs;
	return CKR_OK;
}

/* Perform the initial setup for a new call. */
static CK_RV
call_prepare (CallState *cs, int call_id)
{
	assert (cs);
	assert (cs->call_status == CALL_READY);

	/* Allocate a new request if we've lost the old one */
	if (!cs->req) {
		cs->req = p11_rpc_message_new (call_allocator);
		if (!cs->req) {
			warning (("cannot allocate request buffer: out of memory"));
			return CKR_HOST_MEMORY;
		}
	}
	
	/* Put in the Call ID and signature */
	p11_rpc_message_reset (cs->req);
	if (!p11_rpc_message_prep (cs->req, call_id, P11_RPC_REQUEST))
		return CKR_HOST_MEMORY;
	
	debug (("prepared call: %d", call_id));

	/* Ready to fill in arguments */
	cs->call_status = CALL_PREP;
	return CKR_OK;
}

/* Write all data to session socket.  */
static CK_RV
call_write (CallState *cs, unsigned char* data, size_t len)
{
	int fd, r;

	assert (cs);
	assert (data);
	assert (len > 0);

	while (len > 0) {
	
		fd = cs->socket;
		if (fd == -1) {
			warning (("couldn't send data: socket has been closed"));
			return CKR_DEVICE_ERROR;
		}
		
		r = write (fd, data, len);
		
		if (r == -1) {
			if (errno == EPIPE) {
				warning (("couldn't send data: daemon closed connection"));
				call_disconnect (cs);
				return CKR_DEVICE_ERROR;
			} else if (errno != EAGAIN && errno != EINTR) {
				warning (("couldn't send data: %s", strerror (errno)));
				return CKR_DEVICE_ERROR;
			}
		} else {
			debug (("wrote %d bytes", r));
			data += r;
			len -= r;
		}
	}
	
	return CKR_OK;
}

/* Read a certain amount of data from session socket. */
static CK_RV
call_read (CallState *cs, unsigned char* data, size_t len)
{
	int fd, r;

	assert (cs);
	assert (data);
	assert (len > 0);

	while (len > 0) {
	
		fd = cs->socket;
		if (fd == -1) {
			warning (("couldn't receive data: session socket has been closed"));
			return CKR_DEVICE_ERROR;
		}
		
		r = read (fd, data, len);
		
		if (r == 0) {
			warning (("couldn't receive data: daemon closed connection"));
			call_disconnect (cs);
			return CKR_DEVICE_ERROR;
		} else if (r == -1) {
			if (errno != EAGAIN && errno != EINTR) {
				warning (("couldn't receive data: %s", strerror (errno)));
				return CKR_DEVICE_ERROR;
			}
		} else {
			debug (("read %d bytes", r));
			data += r;
			len -= r;
		}
	}
	
	return CKR_OK;
}

/* 
 * Used by call_session_do_call() to actually send the message to the daemon.
 * Note how we unlock and relock the session during the call. 
 */
static CK_RV
call_send_recv (CallState *cs)
{
	P11RpcMessage *req, *resp;
	unsigned char buf[4];
	uint32_t len;
	CK_RV ret;
	
	assert (cs);
	assert (cs->req);
	assert (cs->call_status == CALL_PREP);
	
	cs->call_status = CALL_TRANSIT;
	
	/* Setup the response buffer properly */
	if (!cs->resp) {
		/* TODO: Do secrets or passwords ever flow through here? */
		cs->resp = p11_rpc_message_new (call_allocator);
		if (!cs->resp) {
			warning (("couldn't allocate response buffer: out of memory"));
			return CKR_HOST_MEMORY;
		}
	}
	p11_rpc_message_reset (cs->resp);
	
	/* 
	 * Now as an additional check to make sure nothing nasty will
	 * happen while we are unlocked, we remove the request and 
	 * response from the session during the action.
	 */
	req = cs->req;
	resp = cs->resp;
	cs->req = cs->resp = NULL;

	/* Send the number of bytes, and then the data */
	gkr_buffer_encode_uint32 (buf, req->buffer.len);
	ret = call_write (cs, buf, 4);
	if (ret != CKR_OK)
		goto cleanup;
	ret = call_write (cs, req->buffer.buf, req->buffer.len);
	if (ret != CKR_OK)
		goto cleanup;
	
	/* Now read out the number of bytes, and then the data */
	ret = call_read (cs, buf, 4);
	if (ret != CKR_OK) 
		goto cleanup;
	len = gkr_buffer_decode_uint32 (buf);
	if (!gkr_buffer_reserve (&resp->buffer, len + resp->buffer.len)) {
		warning (("couldn't allocate %u byte response area: out of memory", len));
		ret = CKR_HOST_MEMORY;
		goto cleanup;
	}
	ret = call_read (cs, resp->buffer.buf, len);
	if (ret != CKR_OK)
		goto cleanup;
	
	gkr_buffer_add_empty (&resp->buffer, len);
	if (!p11_rpc_message_parse (resp, P11_RPC_RESPONSE))
		goto cleanup;
	
	debug (("received response from daemon"));
	
cleanup:
	/* Make sure nobody else used this thread while unlocked */
	assert (cs->call_status == CALL_TRANSIT);
	assert (cs->resp == NULL);
	cs->resp = resp;
	assert (cs->req == NULL);
	cs->req = req;
	
	return ret;
}

/* 
 * At this point the request is ready. So we validate it, and we send it to 
 * the daemon for a response. 
 */
static CK_RV
call_run (CallState *cs)
{
	CK_RV ret = CKR_OK;
	CK_ULONG ckerr;

	assert (cs);
	assert (cs->req);
	assert (cs->call_status == CALL_PREP);
	assert (cs->socket != -1);

	/* Did building the call fail? */
	if (p11_rpc_message_buffer_error (cs->req)) {
		warning (("couldn't allocate request area: out of memory"));
		return CKR_HOST_MEMORY;
	}

	/* Make sure that the signature is valid */
	assert (p11_rpc_message_is_verified (cs->req));

	/* Do the dialog with daemon */
	ret = call_send_recv (cs);
	
	cs->call_status = CALL_PARSE;
	
	if (ret != CKR_OK)
		return ret;

	/* If it's an error code then return it */
	if (cs->resp->call_id == P11_RPC_CALL_ERROR) {

		if (!p11_rpc_message_read_ulong (cs->resp, &ckerr)) {
			warning (("invalid error response from gnome-keyring-daemon: too short"));
			return CKR_DEVICE_ERROR;
		}

		if (ckerr <= CKR_OK) {
			warning (("invalid error response from gnome-keyring-daemon: bad error code"));
			return CKR_DEVICE_ERROR;
		}

		/* An error code from the daemon */
		return (CK_RV)ckerr;
	}
	
	/* Make sure daemon answered the right call */
	if (cs->req->call_id != cs->resp->call_id) {
		warning (("invalid response from gnome-keyring-daemon: call mismatch"));
		return CKR_DEVICE_ERROR;
	}

	assert (!p11_rpc_message_buffer_error (cs->resp));
	debug (("parsing response values"));

	return CKR_OK;
}

static CK_RV
call_done (CallState *cs, CK_RV ret)
{
	assert (cs);
	assert (cs->call_status > CALL_INVALID);

	if (cs->call_status == CALL_PARSE && cs->req && cs->resp) {

		/* Check for parsing errors that were not caught elsewhere */
		if (ret == CKR_OK) {

			if (p11_rpc_message_buffer_error (cs->resp)) {
				warning (("invalid response from gnome-keyring-daemon: bad argument data"));
				return CKR_GENERAL_ERROR;
			}

			/* Double check that the signature matched our decoding */
			assert (p11_rpc_message_is_verified (cs->resp));
		} 
	}

	/* Some cleanup */
	if (cs->socket == -1)
		cs->call_status = CALL_INVALID;
	else
		cs->call_status = CALL_READY;

	return ret;
}

/* -----------------------------------------------------------------------------
 * MODULE SPECIFIC PROTOCOL CODE
 */

static CK_RV
proto_read_attribute_array (P11RpcMessage *msg, CK_ATTRIBUTE_PTR arr, CK_ULONG len)
{
	uint32_t i, num, val;
	CK_ATTRIBUTE_PTR attr;
	const unsigned char *attrval;
	size_t attrlen;
	unsigned char validity;
	CK_RV ret;

	assert (len);
	assert (msg);

	/* Make sure this is in the right order */
	assert (!msg->signature || p11_rpc_message_verify_part (msg, "aA"));
	
	/* Get the number of items. We need this value to be correct */
	if (!gkr_buffer_get_uint32 (&msg->buffer, msg->parsed, &msg->parsed, &num))
		return PARSE_ERROR; 

	if (len != num) {
		
		/*
		 * This should never happen in normal operation. It denotes a goof up 
		 * on the other side of our RPC. We should be indicating the exact number
		 * of attributes to the other side. And it should respond with the same
		 * number.
		 */

		warning (("received an attribute array with wrong number of attributes"));
		return PARSE_ERROR;
	}

	ret = CKR_OK;

	/* We need to go ahead and read everything in all cases */
	for (i = 0; i < num; ++i) {
	
		/* The attribute type */
		gkr_buffer_get_uint32 (&msg->buffer, msg->parsed,
		                       &msg->parsed, &val);

		/* Attribute validity */
		gkr_buffer_get_byte (&msg->buffer, msg->parsed,
		                     &msg->parsed, &validity);

		/* And the data itself */
		if (validity)
			gkr_buffer_get_byte_array (&msg->buffer, msg->parsed,
			                           &msg->parsed, &attrval, &attrlen);
			
		/* Don't act on this data unless no errors */
		if (gkr_buffer_has_error (&msg->buffer))
			break;

		/* Try and stuff it in the output data */
		if (arr) {
			attr = &(arr[i]);
			attr->type = val;

			if (validity) {
				/* Just requesting the attribute size */
				if (!attr->pValue) {
					attr->ulValueLen = attrlen;

				/* Wants attribute data, but too small */
				} else if (attr->ulValueLen < attrlen) {
					attr->ulValueLen = attrlen;
					ret = CKR_BUFFER_TOO_SMALL;

				/* Wants attribute data, value is null */
				} else if (attrval == NULL) {
					attr->ulValueLen = 0;

				/* Wants attribute data, enough space */
				} else {
					attr->ulValueLen = attrlen;
					memcpy (attr->pValue, attrval, attrlen);
				}

			/* Not a valid attribute */
			} else {
				attr->ulValueLen = ((CK_ULONG)-1);
			}
		}
	}

	return gkr_buffer_has_error (&msg->buffer) ? PARSE_ERROR : ret;	
}

static CK_RV
proto_read_byte_array (P11RpcMessage *msg, CK_BYTE_PTR arr,
                       CK_ULONG_PTR len, CK_ULONG max)
{
	const unsigned char *val;
	unsigned char valid;
	size_t vlen;

	assert (len);
	assert (msg);

	/* Make sure this is in the right order */
	assert (!msg->signature || p11_rpc_message_verify_part (msg, "ay"));

	/* A single byte which determines whether valid or not */
	if (!gkr_buffer_get_byte (&msg->buffer, msg->parsed, &msg->parsed, &valid))
		return PARSE_ERROR;
	
	/* If not valid, then just the length is encoded */
	if (!valid) {
		if (!gkr_buffer_get_uint32 (&msg->buffer, msg->parsed, &msg->parsed, &vlen))
			return PARSE_ERROR;
		
		if (arr) {
			
			/*
			 * This should never happen in normal operation. It denotes a goof up 
			 * on the other side of our RPC. We should be sending an empty buffer 
			 * only in the case where there's no array to be filled, which is what 
			 * indicates the other side to reply with an invalid array.
			 */

			warning (("received an invalid array, but caller expected filled"));
			return PARSE_ERROR;
		}
		
		/* Just return the length */
		*len = vlen;
		return CKR_OK;
	} 

	/* Get the actual bytes */
	if (!gkr_buffer_get_byte_array (&msg->buffer, msg->parsed, &msg->parsed, &val, &vlen))
		return PARSE_ERROR; 

	*len = vlen;

	/* Just asking us for size */
	if (!arr)
		return CKR_OK;

	if (max < vlen)
		return CKR_BUFFER_TOO_SMALL;

	/* Enough space, yay */
	memcpy (arr, val, vlen);
	return CKR_OK;
}

static CK_RV
proto_read_ulong_array (P11RpcMessage *msg, CK_ULONG_PTR arr,
                        CK_ULONG_PTR len, CK_ULONG max)
{
	uint32_t i, num, val;
	unsigned char valid;

	assert (len);
	assert (msg);

	/* Make sure this is in the right order */
	assert (!msg->signature || p11_rpc_message_verify_part (msg, "au"));

	/* A single byte which determines whether valid or not */
	if (!gkr_buffer_get_byte (&msg->buffer, msg->parsed, &msg->parsed, &valid))
		return PARSE_ERROR;

	/* Get the number of items. */
	if (!gkr_buffer_get_uint32 (&msg->buffer, msg->parsed, &msg->parsed, &num))
		return PARSE_ERROR;

	*len = num;

	if (!valid) {

		if (arr) {
			
			/*
			 * This should never happen in normal operation. It denotes a goof up 
			 * on the other side of our RPC. We should be sending an empty buffer 
			 * only in the case where there's no array to be filled, which is what 
			 * indicates the other side to reply with an invalid array.
			 */

			warning (("received an invalid array, but caller expected filled"));
			return PARSE_ERROR;
		}

		return CKR_OK;
	}

	if (max < num)
		return CKR_BUFFER_TOO_SMALL;

	/* We need to go ahead and read everything in all cases */
	for (i = 0; i < num; ++i) {
		gkr_buffer_get_uint32 (&msg->buffer, msg->parsed, &msg->parsed, &val);
		if (arr)
			arr[i] = val;
	}

	return gkr_buffer_has_error (&msg->buffer) ? PARSE_ERROR : CKR_OK;
}

static CK_RV
proto_write_mechanism (P11RpcMessage *msg, CK_MECHANISM_PTR mech)
{
	assert (msg);
	assert (mech);

	/* Make sure this is in the right order */
	assert (!msg->signature || p11_rpc_message_verify_part (msg, "M"));
	
	/* The mechanism type */
	gkr_buffer_add_uint32 (&msg->buffer, mech->mechanism);
	
	/*
	 * PKCS#11 mechanism parameters are not easy to serialize. They're 
	 * completely different for so many mechanisms, they contain 
	 * pointers to arbitrary memory, and many callers don't initialize
	 * them completely or properly. 
	 * 
	 * We only support certain mechanisms. 
	 * 
	 * Also callers do yucky things like leaving parts of the structure
	 * pointing to garbage if they don't think it's going to be used.
	 */

	if (p11_rpc_mechanism_has_no_parameters (mech->mechanism))
		gkr_buffer_add_byte_array (&msg->buffer, NULL, 0);
	else if (p11_rpc_mechanism_has_sane_parameters (mech->mechanism))
		gkr_buffer_add_byte_array (&msg->buffer, mech->pParameter, 
		                           mech->ulParameterLen);
	else
		return CKR_MECHANISM_INVALID; 

	return gkr_buffer_has_error (&msg->buffer) ? CKR_HOST_MEMORY : CKR_OK;
}

static CK_RV
proto_read_info (P11RpcMessage *msg, CK_INFO_PTR info)
{
	assert (msg);
	assert (info);

	if (!p11_rpc_message_read_version (msg, &info->cryptokiVersion) || 
	    !p11_rpc_message_read_space_string (msg, info->manufacturerID, 32) ||
	    !p11_rpc_message_read_ulong (msg, &info->flags) ||
	    !p11_rpc_message_read_space_string (msg, info->libraryDescription, 32) ||
	    !p11_rpc_message_read_version (msg, &info->libraryVersion))
		return PARSE_ERROR;

	return CKR_OK;
}

static CK_RV
proto_read_slot_info (P11RpcMessage *msg, CK_SLOT_INFO_PTR info)
{
	assert (msg);
	assert (info);

	if (!p11_rpc_message_read_space_string (msg, info->slotDescription, 64) ||
	    !p11_rpc_message_read_space_string (msg, info->manufacturerID, 32) ||
	    !p11_rpc_message_read_ulong (msg, &info->flags) ||
	    !p11_rpc_message_read_version (msg, &info->hardwareVersion) ||
	    !p11_rpc_message_read_version (msg, &info->firmwareVersion))
		return PARSE_ERROR;

	return CKR_OK;
}

static CK_RV
proto_read_token_info (P11RpcMessage *msg, CK_TOKEN_INFO_PTR info)
{
	assert (msg);
	assert (info);

	if (!p11_rpc_message_read_space_string (msg, info->label, 32) ||
	    !p11_rpc_message_read_space_string (msg, info->manufacturerID, 32) ||
	    !p11_rpc_message_read_space_string (msg, info->model, 16) ||
	    !p11_rpc_message_read_space_string (msg, info->serialNumber, 16) ||
	    !p11_rpc_message_read_ulong (msg, &info->flags) ||
	    !p11_rpc_message_read_ulong (msg, &info->ulMaxSessionCount) ||
	    !p11_rpc_message_read_ulong (msg, &info->ulSessionCount) ||
	    !p11_rpc_message_read_ulong (msg, &info->ulMaxRwSessionCount) ||
	    !p11_rpc_message_read_ulong (msg, &info->ulRwSessionCount) ||
	    !p11_rpc_message_read_ulong (msg, &info->ulMaxPinLen) ||
	    !p11_rpc_message_read_ulong (msg, &info->ulMinPinLen) ||
	    !p11_rpc_message_read_ulong (msg, &info->ulTotalPublicMemory) ||
	    !p11_rpc_message_read_ulong (msg, &info->ulFreePublicMemory) ||
	    !p11_rpc_message_read_ulong (msg, &info->ulTotalPrivateMemory) ||
	    !p11_rpc_message_read_ulong (msg, &info->ulFreePrivateMemory) ||
	    !p11_rpc_message_read_version (msg, &info->hardwareVersion) ||
	    !p11_rpc_message_read_version (msg, &info->firmwareVersion) ||
	    !p11_rpc_message_read_space_string (msg, info->utcTime, 16))
		return PARSE_ERROR;
	
	return CKR_OK;
}

static CK_RV
proto_read_mechanism_info (P11RpcMessage *msg, CK_MECHANISM_INFO_PTR info)
{
	assert (msg);
	assert (info);
	
	if (!p11_rpc_message_read_ulong (msg, &info->ulMinKeySize) ||
	    !p11_rpc_message_read_ulong (msg, &info->ulMaxKeySize) ||
	    !p11_rpc_message_read_ulong (msg, &info->flags))
		return PARSE_ERROR;
	
	return CKR_OK;
}

static CK_RV
proto_read_sesssion_info (P11RpcMessage *msg, CK_SESSION_INFO_PTR info)
{
	assert (msg);
	assert (info);

	if (!p11_rpc_message_read_ulong (msg, &info->slotID) ||
	    !p11_rpc_message_read_ulong (msg, &info->state) ||
	    !p11_rpc_message_read_ulong (msg, &info->flags) ||
	    !p11_rpc_message_read_ulong (msg, &info->ulDeviceError))
		return PARSE_ERROR;

	return CKR_OK;
}

/* -------------------------------------------------------------------
 * CALL MACROS 
 */

#define BEGIN_CALL(call_id) \
	debug ((#call_id ": enter")); \
	return_val_if_fail (pkcs11_initialized, CKR_CRYPTOKI_NOT_INITIALIZED); \
	{  \
		CallState *_cs; \
		CK_RV _ret = CKR_OK; \
		_ret = call_lookup (&_cs); \
		if (_ret != CKR_OK) return _ret; \
		_ret = call_prepare (_cs, P11_RPC_CALL_##call_id); \
		if (_ret != CKR_OK) goto _cleanup;

#define PROCESS_CALL \
		_ret = call_run (_cs); \
		if (_ret != CKR_OK) goto _cleanup;

#define RETURN(ret) \
		_ret = ret; \
		goto _cleanup;
		
#define END_CALL \
	_cleanup: \
		_ret = call_done (_cs, _ret); \
		debug (("ret: %d", _ret)); \
		return _ret; \
	} 

#define IN_BYTE(val) \
	if (!p11_rpc_message_write_byte (_cs->req, val)) \
		{ _ret = CKR_HOST_MEMORY; goto _cleanup; }

#define IN_ULONG(val) \
	if (!p11_rpc_message_write_ulong (_cs->req, val)) \
		{ _ret = CKR_HOST_MEMORY; goto _cleanup; }
	
#define IN_STRING(val) \
	if (!p11_rpc_message_write_zero_string (_cs->req, val)) \
		{ _ret = CKR_HOST_MEMORY; goto _cleanup; }

#define IN_BYTE_BUFFER(arr, len) \
	if (len == NULL) \
		{ _ret = CKR_ARGUMENTS_BAD; goto _cleanup; } \
	if (!p11_rpc_message_write_byte_buffer (_cs->req, arr ? *len : 0)) \
		{ _ret = CKR_HOST_MEMORY; goto _cleanup; }
	
#define IN_BYTE_ARRAY(arr, len) \
	if (len != 0 && arr == NULL) \
		_ret = CKR_ARGUMENTS_BAD; \
	if (!p11_rpc_message_write_byte_array (_cs->req, arr, len)) \
		{ _ret = CKR_HOST_MEMORY; goto _cleanup; }

#define IN_ULONG_BUFFER(arr, len) \
	if (len == NULL) \
		{ _ret = CKR_ARGUMENTS_BAD; goto _cleanup; } \
	if (!p11_rpc_message_write_ulong_buffer (_cs->req, arr ? *len : 0)) \
		{ _ret = CKR_HOST_MEMORY; goto _cleanup; }
	
#define IN_ULONG_ARRAY(arr, len) \
	if (len != 0 && arr == NULL) \
		_ret = CKR_ARGUMENTS_BAD; \
	if (!p11_rpc_message_write_ulong_array (_cs->req, arr, len)) \
		{ _ret = CKR_HOST_MEMORY; goto _cleanup; }

#define IN_ATTRIBUTE_BUFFER(arr, num) \
	if (num != 0 && arr == NULL) \
		{ _ret = CKR_ARGUMENTS_BAD; goto _cleanup; } \
	if (!p11_rpc_message_write_attribute_buffer (_cs->req, (arr), (num))) \
		{ _ret = CKR_HOST_MEMORY; goto _cleanup; }

#define IN_ATTRIBUTE_ARRAY(arr, num) \
	if (num != 0 && arr == NULL) \
		{ _ret = CKR_ARGUMENTS_BAD; goto _cleanup; } \
	if (!p11_rpc_message_write_attribute_array (_cs->req, (arr), (num))) \
		{ _ret = CKR_HOST_MEMORY; goto _cleanup; }

#define IN_MECHANISM_TYPE(val) \
	if(!p11_rpc_mechanism_is_supported (val)) \
		{ _ret = CKR_MECHANISM_INVALID; goto _cleanup; } \
	if (!p11_rpc_message_write_ulong (_cs->req, val)) \
		{ _ret = CKR_HOST_MEMORY; goto _cleanup; }

#define IN_MECHANISM(val) \
	if (val == NULL) \
		_ret = CKR_ARGUMENTS_BAD; \
	_ret = proto_write_mechanism (_cs->req, val); \
	if (_ret != CKR_OK) goto _cleanup;



#define OUT_ULONG(val) \
	if (val == NULL) \
		_ret = CKR_ARGUMENTS_BAD; \
	if (_ret == CKR_OK && !p11_rpc_message_read_ulong (_cs->resp, val)) \
		_ret = PARSE_ERROR;

#define OUT_RETURN_CODE() { \
	CK_RV r; \
	_ret = p11_rpc_message_read_ulong (_cs->resp, (_ret == CKR_OK) ? &r : NULL); \
	if (_ret == CKR_OK) _ret = r; \
	if (_ret != CKR_OK) goto _cleanup; \
	}

#define OUT_BYTE_ARRAY(arr, len)  \
	if (len == NULL) \
		_ret = CKR_ARGUMENTS_BAD; \
	if (_ret == CKR_OK) \
		_ret = proto_read_byte_array (_cs->resp, (arr), (len), *(len)); 

#define OUT_ULONG_ARRAY(a, len) \
	if (len == NULL) \
		_ret = CKR_ARGUMENTS_BAD; \
	if (_ret == CKR_OK) \
		_ret = proto_read_ulong_array (_cs->resp, (a), (len), *(len)); 

#define OUT_ATTRIBUTE_ARRAY(arr, num) \
	if (_ret == CKR_OK) \
		_ret = proto_read_attribute_array (_cs->resp, (arr), (num));

#define OUT_INFO(info) \
	if (info == NULL) \
		_ret = CKR_ARGUMENTS_BAD; \
	if (_ret == CKR_OK) \
		_ret = proto_read_info (_cs->resp, info); 

#define OUT_SLOT_INFO(info) \
	if (info == NULL) \
		_ret = CKR_ARGUMENTS_BAD; \
	if (_ret == CKR_OK) \
		_ret = proto_read_slot_info (_cs->resp, info);

#define OUT_TOKEN_INFO(info) \
	if (info == NULL) \
		_ret = CKR_ARGUMENTS_BAD; \
	if (_ret == CKR_OK) \
		_ret = proto_read_token_info (_cs->resp, info);

#define OUT_SESSION_INFO(info) \
	if (info == NULL) \
		_ret = CKR_ARGUMENTS_BAD; \
	if (_ret == CKR_OK) \
		_ret = proto_read_sesssion_info (_cs->resp, info); 

#define OUT_MECHANISM_TYPE_ARRAY(arr, len) \
	if (len == NULL) \
		_ret = CKR_ARGUMENTS_BAD; \
	if (_ret == CKR_OK) \
		_ret = proto_read_ulong_array (_cs->resp, (arr), (len), *(len)); \
	if (_ret == CKR_OK && arr) \
		p11_rpc_mechanism_list_purge (arr, len);
	
#define OUT_MECHANISM_INFO(info) \
	if (info == NULL) \
		_ret = CKR_ARGUMENTS_BAD; \
	if (_ret == CKR_OK) \
		_ret = proto_read_mechanism_info (_cs->resp, info); 
	

/* -------------------------------------------------------------------
 * INITIALIZATION and 'GLOBAL' CALLS
 */

static CK_RV
rpc_C_Initialize (CK_VOID_PTR init_args)
{
	CK_C_INITIALIZE_ARGS_PTR args = NULL;
	CK_RV ret = CKR_OK;
	const char *path;
	CallState *cs;
	pid_t pid;
	int err;
	
	debug (("C_Initialize: enter"));

#ifdef _DEBUG 
	P11_RPC_CHECK_CALLS();
#endif

	pthread_mutex_lock (&init_mutex);

		if (init_args != NULL) {
			int supplied_ok;

			/* pReserved must be NULL */
			args = init_args;

			/* ALL supplied function pointers need to have the value either NULL or non-NULL. */
			supplied_ok = (args->CreateMutex == NULL && args->DestroyMutex == NULL &&
			               args->LockMutex == NULL && args->UnlockMutex == NULL) ||
			              (args->CreateMutex != NULL && args->DestroyMutex != NULL &&
			               args->LockMutex != NULL && args->UnlockMutex != NULL);
			if (!supplied_ok) {
				warning (("invalid set of mutex calls supplied"));
				ret = CKR_ARGUMENTS_BAD;
				goto done;
			}

			/*
			 * When the CKF_OS_LOCKING_OK flag isn't set return an error.  
			 * We must be able to use our pthread functionality.
			 */
			if (!(args->flags & CKF_OS_LOCKING_OK)) {
				warning (("can't do without os locking"));
				ret = CKR_CANT_LOCK;
				goto done;
			}
		}
	
		pid = getpid ();
		if (pkcs11_initialized) {

			/* This process has called C_Initialize already */
			if (pid == pkcs11_initialized_pid) {
				warning (("C_Initialize called twice for same process"));
				ret = CKR_CRYPTOKI_ALREADY_INITIALIZED;
				goto done;
			}
		}
			
		/* Create the necessary per thread key */
		if (pkcs11_per_thread == 0) {
			err = pthread_key_create (&pkcs11_per_thread, call_destroy);
			if (err != 0) {
				ret = CKR_GENERAL_ERROR;
				goto done;
			}
		}

		path = p11_rpc_module_init (args);
		if (!path || !path[0]) {
			warning (("missing pkcs11 socket path in environment"));
			ret = CKR_GENERAL_ERROR;
			goto done;
		}
		
		/* Make a copy of the socket path */
		snprintf (pkcs11_socket_path, sizeof (pkcs11_socket_path), "%s", path);
			
		/* Call through and initialize the daemon */
		ret = call_lookup (&cs);
		if (ret == CKR_OK) { 
			ret = call_prepare (cs, P11_RPC_CALL_C_Initialize);
			if (ret == CKR_OK)
				if (!p11_rpc_message_write_byte_array (cs->req, P11_RPC_HANDSHAKE, P11_RPC_HANDSHAKE_LEN))
					ret = CKR_HOST_MEMORY;
			if (ret == CKR_OK) {
				ret = call_run (cs);
				if (ret == CKR_CRYPTOKI_ALREADY_INITIALIZED)
					ret = CKR_OK;
			}
			call_done (cs, ret);
		}

done:
	/* Mark us as officially initialized */
	if (ret == CKR_OK) {
		pkcs11_initialized = 1;
		pkcs11_initialized_pid = pid;
	} else if (ret != CKR_CRYPTOKI_ALREADY_INITIALIZED) {
		pkcs11_initialized = 0;
		pkcs11_initialized_pid = 0;
		pkcs11_socket_path[0] = 0;
	}
			
	pthread_mutex_unlock (&init_mutex);

	debug (("C_Initialize: %d", ret));
	return ret;
}

static CK_RV
rpc_C_Finalize (CK_VOID_PTR reserved)
{
	CallState *cs;
	CK_RV ret;
	
	debug (("C_Finalize: enter"));
	return_val_if_fail (pkcs11_initialized, CKR_CRYPTOKI_NOT_INITIALIZED);
	return_val_if_fail (!reserved, CKR_ARGUMENTS_BAD);

	pthread_mutex_lock (&init_mutex);

		ret = call_lookup (&cs);
		if (ret == CKR_OK) { 
			ret = call_prepare (cs, P11_RPC_CALL_C_Finalize);
			if (ret == CKR_OK) {
				ret = call_run (cs);
			}
			call_done (cs, ret);
		}
		
		if (ret != CKR_OK)
			warning (("finalizing the daemon returned an error: %d", ret));
		
		/* This should stop all other calls in */
		pkcs11_initialized = 0;
		pkcs11_initialized_pid = 0;
		pkcs11_socket_path[0] = 0;
		
	pthread_mutex_unlock (&init_mutex);
	
	debug (("C_Finalize: %d", CKR_OK));
	return CKR_OK;
}

static CK_RV
rpc_C_GetInfo (CK_INFO_PTR info)
{
	return_val_if_fail (info, CKR_ARGUMENTS_BAD);

	BEGIN_CALL (C_GetInfo);
	PROCESS_CALL;
		OUT_INFO (info);
	END_CALL;
}

static CK_RV
rpc_C_GetFunctionList (CK_FUNCTION_LIST_PTR_PTR list)
{
	/* This would be a strange call to receive */
	return C_GetFunctionList (list);
}

static CK_RV
rpc_C_GetSlotList (CK_BBOOL token_present, CK_SLOT_ID_PTR slot_list, CK_ULONG_PTR count)
{
	return_val_if_fail (count, CKR_ARGUMENTS_BAD);

	BEGIN_CALL (C_GetSlotList);
		IN_BYTE (token_present);
		IN_ULONG_BUFFER (slot_list, count);
	PROCESS_CALL;
		OUT_ULONG_ARRAY (slot_list, count);
	END_CALL;
}

static CK_RV
rpc_C_GetSlotInfo (CK_SLOT_ID id, CK_SLOT_INFO_PTR info)
{
	return_val_if_fail (info, CKR_ARGUMENTS_BAD);

	BEGIN_CALL (C_GetSlotInfo);
		IN_ULONG (id);
	PROCESS_CALL;
		OUT_SLOT_INFO (info);
	END_CALL;
}

static CK_RV
rpc_C_GetTokenInfo (CK_SLOT_ID id, CK_TOKEN_INFO_PTR info)
{
	return_val_if_fail (info, CKR_ARGUMENTS_BAD);

	BEGIN_CALL (C_GetTokenInfo);
		IN_ULONG (id);
	PROCESS_CALL;
		OUT_TOKEN_INFO (info);
	END_CALL;
}

static CK_RV
rpc_C_GetMechanismList (CK_SLOT_ID id, CK_MECHANISM_TYPE_PTR mechanism_list,
                        CK_ULONG_PTR count)
{
	return_val_if_fail (count, CKR_ARGUMENTS_BAD);

	BEGIN_CALL (C_GetMechanismList);
		IN_ULONG (id);
		IN_ULONG_BUFFER (mechanism_list, count);
	PROCESS_CALL;
		OUT_MECHANISM_TYPE_ARRAY (mechanism_list, count);
	END_CALL;

}

static CK_RV
rpc_C_GetMechanismInfo (CK_SLOT_ID id, CK_MECHANISM_TYPE type, 
                        CK_MECHANISM_INFO_PTR info)
{
	return_val_if_fail (info, CKR_ARGUMENTS_BAD);

	BEGIN_CALL (C_GetMechanismInfo);
		IN_ULONG (id);
		IN_MECHANISM_TYPE (type);
	PROCESS_CALL;
		OUT_MECHANISM_INFO (info);
	END_CALL;
}

static CK_RV
rpc_C_InitToken (CK_SLOT_ID id, CK_UTF8CHAR_PTR pin, CK_ULONG pin_len, 
                 CK_UTF8CHAR_PTR label)
{
	BEGIN_CALL (C_InitToken);
		IN_ULONG (id);
		IN_BYTE_ARRAY (pin, pin_len);
		IN_STRING (label);
	PROCESS_CALL;
	END_CALL;
}

static CK_RV
rpc_C_WaitForSlotEvent (CK_FLAGS flags, CK_SLOT_ID_PTR slot, CK_VOID_PTR reserved)
{
	return_val_if_fail (slot, CKR_ARGUMENTS_BAD);

	BEGIN_CALL (C_WaitForSlotEvent);
		IN_ULONG (flags);
	PROCESS_CALL;
		OUT_ULONG (slot);
	END_CALL;
}

static CK_RV
rpc_C_OpenSession (CK_SLOT_ID id, CK_FLAGS flags, CK_VOID_PTR user_data,
                   CK_NOTIFY callback, CK_SESSION_HANDLE_PTR session)
{
	return_val_if_fail (session, CKR_ARGUMENTS_BAD);

	BEGIN_CALL (C_OpenSession);
		IN_ULONG (id);
		IN_ULONG (flags);
	PROCESS_CALL;
		OUT_ULONG (session);
	END_CALL;
}

static CK_RV
rpc_C_CloseSession (CK_SESSION_HANDLE session)
{
	BEGIN_CALL (C_CloseSession);
		IN_ULONG (session);
	PROCESS_CALL;
	END_CALL;
}

static CK_RV
rpc_C_CloseAllSessions (CK_SLOT_ID id)
{
	BEGIN_CALL (C_CloseAllSessions);
		IN_ULONG (id);
	PROCESS_CALL;
	END_CALL;
}

static CK_RV
rpc_C_GetFunctionStatus (CK_SESSION_HANDLE session)
{
	BEGIN_CALL (C_GetFunctionStatus);
		IN_ULONG (session);
	PROCESS_CALL;
	END_CALL;
}

static CK_RV
rpc_C_CancelFunction (CK_SESSION_HANDLE session)
{
	BEGIN_CALL (C_CancelFunction);
		IN_ULONG (session);
	PROCESS_CALL;
	END_CALL;
}

static CK_RV
rpc_C_GetSessionInfo(CK_SESSION_HANDLE session, CK_SESSION_INFO_PTR info)
{
	return_val_if_fail (info, CKR_ARGUMENTS_BAD);

	BEGIN_CALL (C_GetSessionInfo);
		IN_ULONG (session);
	PROCESS_CALL;
		OUT_SESSION_INFO (info);
	END_CALL;
}

static CK_RV
rpc_C_InitPIN (CK_SESSION_HANDLE session, CK_UTF8CHAR_PTR pin, 
               CK_ULONG pin_len)
{
	BEGIN_CALL (C_InitPIN);
		IN_ULONG (session);
		IN_BYTE_ARRAY (pin, pin_len);
	PROCESS_CALL;
	END_CALL;
}

static CK_RV
rpc_C_SetPIN (CK_SESSION_HANDLE session, CK_UTF8CHAR_PTR old_pin,
              CK_ULONG old_pin_len, CK_UTF8CHAR_PTR new_pin, CK_ULONG new_pin_len)
{
	BEGIN_CALL (C_SetPIN);
		IN_ULONG (session);
		IN_BYTE_ARRAY (old_pin, old_pin_len);
		IN_BYTE_ARRAY (new_pin, old_pin_len);
	PROCESS_CALL;
	END_CALL;
}

static CK_RV
rpc_C_GetOperationState (CK_SESSION_HANDLE session, CK_BYTE_PTR operation_state,
                         CK_ULONG_PTR operation_state_len)
{
	return_val_if_fail (operation_state_len, CKR_ARGUMENTS_BAD);

	BEGIN_CALL (C_GetOperationState);
		IN_ULONG (session);
		IN_BYTE_BUFFER (operation_state, operation_state_len);
	PROCESS_CALL;
		OUT_BYTE_ARRAY (operation_state, operation_state_len);
	END_CALL;
}

static CK_RV
rpc_C_SetOperationState (CK_SESSION_HANDLE session, CK_BYTE_PTR operation_state,
                         CK_ULONG operation_state_len, CK_OBJECT_HANDLE encryption_key,
                         CK_OBJECT_HANDLE authentication_key)
{
	BEGIN_CALL (C_SetOperationState);
		IN_ULONG (session);
		IN_BYTE_ARRAY (operation_state, operation_state_len);
		IN_ULONG (encryption_key);
		IN_ULONG (authentication_key);
	PROCESS_CALL;
	END_CALL;
}

static CK_RV
rpc_C_Login (CK_SESSION_HANDLE session, CK_USER_TYPE user_type,
             CK_UTF8CHAR_PTR pin, CK_ULONG pin_len)
{
	BEGIN_CALL (C_Login);
		IN_ULONG (session);
		IN_ULONG (user_type);
		IN_BYTE_ARRAY (pin, pin_len);
	PROCESS_CALL;
	END_CALL;
}

static CK_RV
rpc_C_Logout (CK_SESSION_HANDLE session)
{
	BEGIN_CALL (C_Logout);
		IN_ULONG (session);
	PROCESS_CALL;
	END_CALL;
}

static CK_RV
rpc_C_CreateObject (CK_SESSION_HANDLE session, CK_ATTRIBUTE_PTR template,
                    CK_ULONG count, CK_OBJECT_HANDLE_PTR new_object)
{
	return_val_if_fail (new_object, CKR_ARGUMENTS_BAD);

	BEGIN_CALL (C_CreateObject);
		IN_ULONG (session);
		IN_ATTRIBUTE_ARRAY (template, count);
	PROCESS_CALL;
		OUT_ULONG (new_object);
	END_CALL;
}

static CK_RV
rpc_C_CopyObject (CK_SESSION_HANDLE session, CK_OBJECT_HANDLE object,
                  CK_ATTRIBUTE_PTR template, CK_ULONG count,
                  CK_OBJECT_HANDLE_PTR new_object)
{
	return_val_if_fail (new_object, CKR_ARGUMENTS_BAD);

	BEGIN_CALL (C_CopyObject);
		IN_ULONG (session);
		IN_ULONG (object);
		IN_ATTRIBUTE_ARRAY (template, count);
	PROCESS_CALL;
		OUT_ULONG (new_object);
	END_CALL;
}


static CK_RV
rpc_C_DestroyObject (CK_SESSION_HANDLE session, CK_OBJECT_HANDLE object)
{
	BEGIN_CALL (C_DestroyObject);
		IN_ULONG (session);
		IN_ULONG (object);
	PROCESS_CALL;
	END_CALL;
}

static CK_RV
rpc_C_GetObjectSize (CK_SESSION_HANDLE session, CK_OBJECT_HANDLE object,
                     CK_ULONG_PTR size)
{
	return_val_if_fail (size, CKR_ARGUMENTS_BAD);

	BEGIN_CALL (C_GetObjectSize);
		IN_ULONG (session);
		IN_ULONG (object);
	PROCESS_CALL;
		OUT_ULONG (size);
	END_CALL;
}

static CK_RV
rpc_C_GetAttributeValue (CK_SESSION_HANDLE session, CK_OBJECT_HANDLE object,
                         CK_ATTRIBUTE_PTR template, CK_ULONG count)
{
	BEGIN_CALL (C_GetAttributeValue);
		IN_ULONG (session);
		IN_ULONG (object);
		IN_ATTRIBUTE_BUFFER (template, count);
	PROCESS_CALL;
		OUT_ATTRIBUTE_ARRAY (template, count);
		OUT_RETURN_CODE ();
	END_CALL;
}

static CK_RV
rpc_C_SetAttributeValue (CK_SESSION_HANDLE session, CK_OBJECT_HANDLE object,
                         CK_ATTRIBUTE_PTR template, CK_ULONG count)
{
	BEGIN_CALL (C_SetAttributeValue);
		IN_ULONG (session);
		IN_ULONG (object);
		IN_ATTRIBUTE_ARRAY (template, count);
	PROCESS_CALL;
	END_CALL;
}

static CK_RV
rpc_C_FindObjectsInit (CK_SESSION_HANDLE session, CK_ATTRIBUTE_PTR template,
                       CK_ULONG count)
{
	BEGIN_CALL (C_FindObjectsInit);
		IN_ULONG (session);
		IN_ATTRIBUTE_ARRAY (template, count);
	PROCESS_CALL;
	END_CALL;
}

static CK_RV
rpc_C_FindObjects (CK_SESSION_HANDLE session, CK_OBJECT_HANDLE_PTR objects,
                   CK_ULONG max_count, CK_ULONG_PTR count)
{
	return_val_if_fail (count, CKR_ARGUMENTS_BAD);

	BEGIN_CALL (C_FindObjects);
		IN_ULONG (session);
		IN_ULONG_BUFFER (objects, &max_count);
	PROCESS_CALL;
		*count = max_count;
		OUT_ULONG_ARRAY (objects, count);
	END_CALL;
}

static CK_RV
rpc_C_FindObjectsFinal (CK_SESSION_HANDLE session)
{
	BEGIN_CALL (C_FindObjectsFinal);
		IN_ULONG (session);
	PROCESS_CALL;
	END_CALL;
}

static CK_RV
rpc_C_EncryptInit (CK_SESSION_HANDLE session, CK_MECHANISM_PTR mechanism,
                   CK_OBJECT_HANDLE key)
{
	BEGIN_CALL (C_EncryptInit);
		IN_ULONG (session);
		IN_MECHANISM (mechanism);
		IN_ULONG (key);
	PROCESS_CALL;
	END_CALL;
}

static CK_RV
rpc_C_Encrypt (CK_SESSION_HANDLE session, CK_BYTE_PTR data, CK_ULONG data_len,
               CK_BYTE_PTR encrypted_data, CK_ULONG_PTR encrypted_data_len)
{
	return_val_if_fail (encrypted_data_len, CKR_ARGUMENTS_BAD);

	BEGIN_CALL (C_Encrypt);
		IN_ULONG (session);
		IN_BYTE_ARRAY (data, data_len);
		IN_BYTE_BUFFER (encrypted_data, encrypted_data_len);
	PROCESS_CALL;
		OUT_BYTE_ARRAY (encrypted_data, encrypted_data_len);
	END_CALL;
}

static CK_RV
rpc_C_EncryptUpdate (CK_SESSION_HANDLE session, CK_BYTE_PTR part,
                     CK_ULONG part_len, CK_BYTE_PTR encrypted_part,
                     CK_ULONG_PTR encrypted_part_len)
{
	return_val_if_fail (encrypted_part_len, CKR_ARGUMENTS_BAD);

	BEGIN_CALL (C_EncryptUpdate);
		IN_ULONG (session);
		IN_BYTE_ARRAY (part, part_len);
		IN_BYTE_BUFFER (encrypted_part, encrypted_part_len);
	PROCESS_CALL;
		OUT_BYTE_ARRAY (encrypted_part, encrypted_part_len);
	END_CALL;
}

static CK_RV
rpc_C_EncryptFinal (CK_SESSION_HANDLE session, CK_BYTE_PTR last_part,
                    CK_ULONG_PTR last_part_len)
{
	return_val_if_fail (last_part_len, CKR_ARGUMENTS_BAD);

	BEGIN_CALL (C_EncryptFinal);
		IN_ULONG (session);
		IN_BYTE_BUFFER (last_part, last_part_len);
	PROCESS_CALL;
		OUT_BYTE_ARRAY (last_part, last_part_len);
	END_CALL;
}

static CK_RV
rpc_C_DecryptInit (CK_SESSION_HANDLE session, CK_MECHANISM_PTR mechanism,
                   CK_OBJECT_HANDLE key)
{
	BEGIN_CALL (C_DecryptInit);
		IN_ULONG (session);
		IN_MECHANISM (mechanism);
		IN_ULONG (key);
	PROCESS_CALL;
	END_CALL;
}

static CK_RV
rpc_C_Decrypt (CK_SESSION_HANDLE session, CK_BYTE_PTR enc_data,
               CK_ULONG enc_data_len, CK_BYTE_PTR data, CK_ULONG_PTR data_len)
{
	return_val_if_fail (data_len, CKR_ARGUMENTS_BAD);

	BEGIN_CALL (C_Decrypt);
		IN_ULONG (session);
		IN_BYTE_ARRAY (enc_data, enc_data_len);
		IN_BYTE_BUFFER (data, data_len);
	PROCESS_CALL;
		OUT_BYTE_ARRAY (data, data_len);
	END_CALL;
}

static CK_RV
rpc_C_DecryptUpdate (CK_SESSION_HANDLE session, CK_BYTE_PTR enc_part,
                     CK_ULONG enc_part_len, CK_BYTE_PTR part, CK_ULONG_PTR part_len)
{
	return_val_if_fail (part_len, CKR_ARGUMENTS_BAD);

	BEGIN_CALL (C_DecryptUpdate);
		IN_ULONG (session);
		IN_BYTE_ARRAY (enc_part, enc_part_len);
		IN_BYTE_BUFFER (part, part_len);
	PROCESS_CALL;
		OUT_BYTE_ARRAY (part, part_len);
	END_CALL;
}

static CK_RV
rpc_C_DecryptFinal (CK_SESSION_HANDLE session, CK_BYTE_PTR last_part,
                    CK_ULONG_PTR last_part_len)
{
	return_val_if_fail (last_part_len, CKR_ARGUMENTS_BAD);

	BEGIN_CALL (C_DecryptFinal);
		IN_ULONG (session);
		IN_BYTE_BUFFER (last_part, last_part_len);
	PROCESS_CALL;
		OUT_BYTE_ARRAY (last_part, last_part_len);
	END_CALL;
}

static CK_RV
rpc_C_DigestInit (CK_SESSION_HANDLE session, CK_MECHANISM_PTR mechanism)
{
	BEGIN_CALL (C_DigestInit);
		IN_ULONG (session);
		IN_MECHANISM (mechanism);
	PROCESS_CALL;
	END_CALL;
}

static CK_RV
rpc_C_Digest (CK_SESSION_HANDLE session, CK_BYTE_PTR data, CK_ULONG data_len,
              CK_BYTE_PTR digest, CK_ULONG_PTR digest_len)
{
	return_val_if_fail (digest_len, CKR_ARGUMENTS_BAD);

	BEGIN_CALL (C_Digest);
		IN_ULONG (session);
		IN_BYTE_ARRAY (data, data_len);
		IN_BYTE_BUFFER (digest, digest_len);
	PROCESS_CALL;
		OUT_BYTE_ARRAY (digest, digest_len);
	END_CALL;
}

static CK_RV
rpc_C_DigestUpdate (CK_SESSION_HANDLE session, CK_BYTE_PTR part, CK_ULONG part_len)
{
	BEGIN_CALL (C_DigestUpdate);
		IN_ULONG (session);
		IN_BYTE_ARRAY (part, part_len);
	PROCESS_CALL;
	END_CALL;
}

static CK_RV
rpc_C_DigestKey (CK_SESSION_HANDLE session, CK_OBJECT_HANDLE key)
{
	BEGIN_CALL (C_DigestKey);
		IN_ULONG (session);
		IN_ULONG (key);
	PROCESS_CALL;
	END_CALL;
}

static CK_RV
rpc_C_DigestFinal (CK_SESSION_HANDLE session, CK_BYTE_PTR digest,
                   CK_ULONG_PTR digest_len)
{
	return_val_if_fail (digest_len, CKR_ARGUMENTS_BAD);

	BEGIN_CALL (C_DigestFinal);
		IN_ULONG (session);
		IN_BYTE_BUFFER (digest, digest_len);
	PROCESS_CALL;
		OUT_BYTE_ARRAY (digest, digest_len);
	END_CALL;
}

static CK_RV
rpc_C_SignInit (CK_SESSION_HANDLE session, CK_MECHANISM_PTR mechanism,
                CK_OBJECT_HANDLE key)
{
	BEGIN_CALL (C_SignInit);
		IN_ULONG (session);
		IN_MECHANISM (mechanism);
		IN_ULONG (key);
	PROCESS_CALL;
	END_CALL;
}

static CK_RV
rpc_C_Sign (CK_SESSION_HANDLE session, CK_BYTE_PTR data, CK_ULONG data_len,
            CK_BYTE_PTR signature, CK_ULONG_PTR signature_len)
{
	return_val_if_fail (signature_len, CKR_ARGUMENTS_BAD);

	BEGIN_CALL (C_Sign);
		IN_ULONG (session);
		IN_BYTE_ARRAY (data, data_len);
		IN_BYTE_BUFFER (signature, signature_len);
	PROCESS_CALL;
		OUT_BYTE_ARRAY (signature, signature_len);
	END_CALL;
}

static CK_RV
rpc_C_SignUpdate (CK_SESSION_HANDLE session, CK_BYTE_PTR part, CK_ULONG part_len)
{
	return_val_if_fail (part_len, CKR_ARGUMENTS_BAD);

	BEGIN_CALL (C_SignUpdate);
		IN_ULONG (session);
		IN_BYTE_ARRAY (part, part_len);
	PROCESS_CALL;
	END_CALL;
}

static CK_RV
rpc_C_SignFinal (CK_SESSION_HANDLE session, CK_BYTE_PTR signature,
                 CK_ULONG_PTR signature_len)
{
	return_val_if_fail (signature_len, CKR_ARGUMENTS_BAD);

	BEGIN_CALL (C_SignFinal);
		IN_ULONG (session);
		IN_BYTE_BUFFER (signature, signature_len);
	PROCESS_CALL;
		OUT_BYTE_ARRAY (signature, signature_len);
	END_CALL;
}

static CK_RV
rpc_C_SignRecoverInit (CK_SESSION_HANDLE session, CK_MECHANISM_PTR mechanism,
                       CK_OBJECT_HANDLE key)
{
	BEGIN_CALL (C_SignRecoverInit);
		IN_ULONG (session);
		IN_MECHANISM (mechanism);
		IN_ULONG (key);
	PROCESS_CALL;
	END_CALL;
}

static CK_RV
rpc_C_SignRecover (CK_SESSION_HANDLE session, CK_BYTE_PTR data, CK_ULONG data_len, 
                   CK_BYTE_PTR signature, CK_ULONG_PTR signature_len)
{
	return_val_if_fail (signature_len, CKR_ARGUMENTS_BAD);

	BEGIN_CALL (C_SignRecover);
		IN_ULONG (session);
		IN_BYTE_ARRAY (data, data_len);
		IN_BYTE_BUFFER (signature, signature_len);
	PROCESS_CALL;
		OUT_BYTE_ARRAY (signature, signature_len);
	END_CALL;
}

static CK_RV
rpc_C_VerifyInit (CK_SESSION_HANDLE session, CK_MECHANISM_PTR mechanism,
                  CK_OBJECT_HANDLE key)
{
	BEGIN_CALL (C_VerifyInit);
		IN_ULONG (session);
		IN_MECHANISM (mechanism);
		IN_ULONG (key);
	PROCESS_CALL;
	END_CALL;
}

static CK_RV
rpc_C_Verify (CK_SESSION_HANDLE session, CK_BYTE_PTR data, CK_ULONG data_len,
              CK_BYTE_PTR signature, CK_ULONG signature_len)
{
	BEGIN_CALL (C_Verify);
		IN_ULONG (session);
		IN_BYTE_ARRAY (data, data_len);
		IN_BYTE_ARRAY (signature, signature_len);
	PROCESS_CALL;
	END_CALL;
}

static CK_RV
rpc_C_VerifyUpdate (CK_SESSION_HANDLE session, CK_BYTE_PTR part, CK_ULONG part_len)
{
	BEGIN_CALL (C_VerifyUpdate);
		IN_ULONG (session);
		IN_BYTE_ARRAY (part, part_len);
	PROCESS_CALL;
	END_CALL;
}

static CK_RV
rpc_C_VerifyFinal (CK_SESSION_HANDLE session, CK_BYTE_PTR signature,
                   CK_ULONG signature_len)
{
	BEGIN_CALL (C_VerifyFinal);
		IN_ULONG (session);
		IN_BYTE_ARRAY (signature, signature_len);
	PROCESS_CALL;
	END_CALL;
}

static CK_RV
rpc_C_VerifyRecoverInit (CK_SESSION_HANDLE session, CK_MECHANISM_PTR mechanism,
                         CK_OBJECT_HANDLE key)
{
	BEGIN_CALL (C_VerifyRecoverInit);
		IN_ULONG (session);
		IN_MECHANISM (mechanism);
		IN_ULONG (key);
	PROCESS_CALL;
	END_CALL;
}

static CK_RV
rpc_C_VerifyRecover (CK_SESSION_HANDLE session, CK_BYTE_PTR signature,
                     CK_ULONG signature_len, CK_BYTE_PTR data, CK_ULONG_PTR data_len)
{
	return_val_if_fail (data_len, CKR_ARGUMENTS_BAD);

	BEGIN_CALL (C_VerifyRecover);
		IN_ULONG (session);
		IN_BYTE_ARRAY (signature, signature_len);
		IN_BYTE_BUFFER (data, data_len);
	PROCESS_CALL;
		OUT_BYTE_ARRAY (data, data_len);
	END_CALL;
}

static CK_RV
rpc_C_DigestEncryptUpdate (CK_SESSION_HANDLE session, CK_BYTE_PTR part,
                           CK_ULONG part_len, CK_BYTE_PTR enc_part,
                           CK_ULONG_PTR enc_part_len)
{
	return_val_if_fail (enc_part_len, CKR_ARGUMENTS_BAD);

	BEGIN_CALL (C_DigestEncryptUpdate);
		IN_ULONG (session);
		IN_BYTE_ARRAY (part, part_len);
		IN_BYTE_BUFFER (enc_part, enc_part_len);
	PROCESS_CALL;
		OUT_BYTE_ARRAY (enc_part, enc_part_len);
	END_CALL;
}

static CK_RV
rpc_C_DecryptDigestUpdate (CK_SESSION_HANDLE session, CK_BYTE_PTR enc_part,
                           CK_ULONG enc_part_len, CK_BYTE_PTR part, 
                           CK_ULONG_PTR part_len)
{
	return_val_if_fail (part_len, CKR_ARGUMENTS_BAD);

	BEGIN_CALL (C_DecryptDigestUpdate);
		IN_ULONG (session);
		IN_BYTE_ARRAY (enc_part, enc_part_len);
		IN_BYTE_BUFFER (part, part_len);
	PROCESS_CALL;
		OUT_BYTE_ARRAY (part, part_len);
	END_CALL;
}

static CK_RV
rpc_C_SignEncryptUpdate (CK_SESSION_HANDLE session, CK_BYTE_PTR part,
                         CK_ULONG part_len, CK_BYTE_PTR enc_part,
                         CK_ULONG_PTR enc_part_len)
{
	return_val_if_fail (enc_part_len, CKR_ARGUMENTS_BAD);

	BEGIN_CALL (C_SignEncryptUpdate);
		IN_ULONG (session);
		IN_BYTE_ARRAY (part, part_len);
		IN_BYTE_BUFFER (enc_part, enc_part_len);
	PROCESS_CALL;
		OUT_BYTE_ARRAY (enc_part, enc_part_len);
	END_CALL;
}

static CK_RV
rpc_C_DecryptVerifyUpdate (CK_SESSION_HANDLE session, CK_BYTE_PTR enc_part,
                           CK_ULONG enc_part_len, CK_BYTE_PTR part, 
                           CK_ULONG_PTR part_len)
{
	return_val_if_fail (part_len, CKR_ARGUMENTS_BAD);

	BEGIN_CALL (C_DecryptVerifyUpdate);
		IN_ULONG (session);
		IN_BYTE_ARRAY (enc_part, enc_part_len);
		IN_BYTE_BUFFER (part, part_len);
	PROCESS_CALL;
		OUT_BYTE_ARRAY (part, part_len);
	END_CALL;
}

static CK_RV
rpc_C_GenerateKey (CK_SESSION_HANDLE session, CK_MECHANISM_PTR mechanism,
                   CK_ATTRIBUTE_PTR template, CK_ULONG count, 
                   CK_OBJECT_HANDLE_PTR key)
{
	BEGIN_CALL (C_GenerateKey);
		IN_ULONG (session);
		IN_MECHANISM (mechanism);
		IN_ATTRIBUTE_ARRAY (template, count);
	PROCESS_CALL;
		OUT_ULONG (key);
	END_CALL;
}

static CK_RV
rpc_C_GenerateKeyPair (CK_SESSION_HANDLE session, CK_MECHANISM_PTR mechanism,
                       CK_ATTRIBUTE_PTR pub_template, CK_ULONG pub_count,
                       CK_ATTRIBUTE_PTR priv_template, CK_ULONG priv_count,
                       CK_OBJECT_HANDLE_PTR pub_key, CK_OBJECT_HANDLE_PTR priv_key)
{
	BEGIN_CALL (C_GenerateKeyPair);
		IN_ULONG (session);
		IN_MECHANISM (mechanism);
		IN_ATTRIBUTE_ARRAY (pub_template, pub_count);
		IN_ATTRIBUTE_ARRAY (priv_template, priv_count);
	PROCESS_CALL;
		OUT_ULONG (pub_key);
		OUT_ULONG (priv_key);
	END_CALL;
}

static CK_RV
rpc_C_WrapKey (CK_SESSION_HANDLE session, CK_MECHANISM_PTR mechanism,
               CK_OBJECT_HANDLE wrapping_key, CK_OBJECT_HANDLE key,
               CK_BYTE_PTR wrapped_key, CK_ULONG_PTR wrapped_key_len)
{
	return_val_if_fail (wrapped_key_len, CKR_ARGUMENTS_BAD);
	
	BEGIN_CALL (C_WrapKey);
		IN_ULONG (session);
		IN_MECHANISM (mechanism);
		IN_ULONG (wrapping_key);
		IN_ULONG (key);
		IN_BYTE_BUFFER (wrapped_key, wrapped_key_len);
	PROCESS_CALL;
		OUT_BYTE_ARRAY (wrapped_key, wrapped_key_len);
	END_CALL;
}

static CK_RV
rpc_C_UnwrapKey (CK_SESSION_HANDLE session, CK_MECHANISM_PTR mechanism,
                 CK_OBJECT_HANDLE unwrapping_key, CK_BYTE_PTR wrapped_key,
                 CK_ULONG wrapped_key_len, CK_ATTRIBUTE_PTR template,
                 CK_ULONG count, CK_OBJECT_HANDLE_PTR key)
{
	BEGIN_CALL (C_UnwrapKey);
		IN_ULONG (session);
		IN_MECHANISM (mechanism);
		IN_ULONG (unwrapping_key);
		IN_BYTE_ARRAY (wrapped_key, wrapped_key_len);
		IN_ATTRIBUTE_ARRAY (template, count);
	PROCESS_CALL;
		OUT_ULONG (key);
	END_CALL;
}

static CK_RV
rpc_C_DeriveKey (CK_SESSION_HANDLE session, CK_MECHANISM_PTR mechanism,
                 CK_OBJECT_HANDLE base_key, CK_ATTRIBUTE_PTR template,
                 CK_ULONG count, CK_OBJECT_HANDLE_PTR key)
{
	BEGIN_CALL (C_DeriveKey);
		IN_ULONG (session);
		IN_MECHANISM (mechanism);
		IN_ULONG (base_key);
		IN_ATTRIBUTE_ARRAY (template, count);
	PROCESS_CALL;
		OUT_ULONG (key);
	END_CALL;
}

static CK_RV
rpc_C_SeedRandom (CK_SESSION_HANDLE session, CK_BYTE_PTR seed, CK_ULONG seed_len)
{
	BEGIN_CALL (C_SeedRandom);
		IN_ULONG (session);
		IN_BYTE_ARRAY (seed, seed_len);
	PROCESS_CALL;
	END_CALL;
}

static CK_RV
rpc_C_GenerateRandom (CK_SESSION_HANDLE session, CK_BYTE_PTR random_data,
                      CK_ULONG random_len)
{
	BEGIN_CALL (C_GenerateRandom);
		IN_ULONG (session);
		IN_BYTE_BUFFER (random_data, &random_len);
	PROCESS_CALL;
		OUT_BYTE_ARRAY (random_data, &random_len);
	END_CALL;
}

/* --------------------------------------------------------------------
 * MODULE ENTRY POINT
 */

/* 
 * PKCS#11 is broken here. It states that Unix compilers automatically byte 
 * pack structures. This is wrong. GCC on Linux aligns to 4 by default. 
 * 
 * This results in incompatibilities. Where this structure's first version
 * members take up too much or too little space depending on how this module
 * is compiled.
 */

static CK_FUNCTION_LIST functionList = {
	{ CRYPTOKI_VERSION_MAJOR, CRYPTOKI_VERSION_MINOR },  /* version */
	rpc_C_Initialize,
	rpc_C_Finalize,
	rpc_C_GetInfo,
	rpc_C_GetFunctionList,
	rpc_C_GetSlotList,
	rpc_C_GetSlotInfo,
	rpc_C_GetTokenInfo,
	rpc_C_GetMechanismList,
	rpc_C_GetMechanismInfo,
	rpc_C_InitToken,
	rpc_C_InitPIN,
	rpc_C_SetPIN,
	rpc_C_OpenSession,
	rpc_C_CloseSession,
	rpc_C_CloseAllSessions,
	rpc_C_GetSessionInfo,
	rpc_C_GetOperationState,
	rpc_C_SetOperationState,
	rpc_C_Login,
	rpc_C_Logout,
	rpc_C_CreateObject,
	rpc_C_CopyObject,
	rpc_C_DestroyObject,
	rpc_C_GetObjectSize,
	rpc_C_GetAttributeValue,
	rpc_C_SetAttributeValue,
	rpc_C_FindObjectsInit,
	rpc_C_FindObjects,
	rpc_C_FindObjectsFinal,
	rpc_C_EncryptInit,
	rpc_C_Encrypt,
	rpc_C_EncryptUpdate,
	rpc_C_EncryptFinal,
	rpc_C_DecryptInit,
	rpc_C_Decrypt,
	rpc_C_DecryptUpdate,
	rpc_C_DecryptFinal,
	rpc_C_DigestInit,
	rpc_C_Digest,
	rpc_C_DigestUpdate,
	rpc_C_DigestKey,
	rpc_C_DigestFinal,
	rpc_C_SignInit,
	rpc_C_Sign,
	rpc_C_SignUpdate,
	rpc_C_SignFinal,
	rpc_C_SignRecoverInit,
	rpc_C_SignRecover,
	rpc_C_VerifyInit,
	rpc_C_Verify,
	rpc_C_VerifyUpdate,
	rpc_C_VerifyFinal,
	rpc_C_VerifyRecoverInit,
	rpc_C_VerifyRecover,
	rpc_C_DigestEncryptUpdate,
	rpc_C_DecryptDigestUpdate,
	rpc_C_SignEncryptUpdate,
	rpc_C_DecryptVerifyUpdate,
	rpc_C_GenerateKey,
	rpc_C_GenerateKeyPair,
	rpc_C_WrapKey,
	rpc_C_UnwrapKey,
	rpc_C_DeriveKey,
	rpc_C_SeedRandom,
	rpc_C_GenerateRandom,
	rpc_C_GetFunctionStatus,
	rpc_C_CancelFunction,
	rpc_C_WaitForSlotEvent
};

CK_RV
C_GetFunctionList (CK_FUNCTION_LIST_PTR_PTR list)
{
	return_val_if_fail (list, CKR_ARGUMENTS_BAD);

	*list = &functionList;
	return CKR_OK;
}