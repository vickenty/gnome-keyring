/* -*- Mode: C; indent-tabs-mode: t; c-basic-offset: 8; tab-width: 8 -*- */
/* gkr-ask-daemon.c: Global ask functionality

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

#include "gkr-ask-daemon.h"
#include "gkr-ask-request.h"

#include "common/gkr-async.h"
#include "common/gkr-cleanup.h"

#include <glib.h>

static gboolean ask_daemon_inited = FALSE;

static GkrAsyncWait *wait_condition = NULL;
static GkrAskRequest *current_ask = NULL;

static GkrAskHook ask_daemon_hook = NULL;
static gpointer ask_daemon_hook_data = NULL;

static void 
ask_daemon_cleanup (gpointer unused)
{
	g_assert (ask_daemon_inited);

	if (current_ask)
		gkr_ask_request_cancel (current_ask);
	
	g_assert (wait_condition);
	gkr_async_wait_free (wait_condition);
	wait_condition = NULL;
	
	ask_daemon_inited = FALSE;
}

static void
ask_daemon_init (void)
{
	if (ask_daemon_inited)
		return;
	ask_daemon_inited = TRUE;
	
	wait_condition = gkr_async_wait_new ();
	
	gkr_cleanup_register (ask_daemon_cleanup, NULL);
}

void
gkr_ask_daemon_process (GkrAskRequest* ask)
{
	ask_daemon_init ();
	
	g_assert (GKR_IS_ASK_REQUEST (ask));
	g_assert (!gkr_ask_request_is_complete (ask));
	
	/* 
	 * Hand it off to the hook. This is used in the test harnesses
	 * to verify that a prompt or no prompt was run.
	 */
	if (ask_daemon_hook)
		(ask_daemon_hook) (ask, ask_daemon_hook_data);
	
	/* See if it'll complete without a prompt */
	if (gkr_ask_request_check (ask))
		goto done;
	
	if (gkr_async_is_stopping ()) {
		gkr_ask_request_cancel (ask);
		goto done;
	}
	
	/* Wait until no other asks are prompting */
	while (current_ask)
		gkr_async_wait (wait_condition);
	
	g_assert (ask_daemon_inited);
	
	g_object_ref (ask);
	current_ask = ask;
	
	if (!gkr_ask_request_check (ask))
		gkr_ask_request_prompt (ask);

	current_ask = NULL;
	g_object_unref (ask);
	
	g_assert (wait_condition);
	gkr_async_notify (wait_condition);
	
done:
	g_assert (gkr_ask_request_is_complete (ask));
}

void
gkr_ask_daemon_set_hook (GkrAskHook hook, gpointer data)
{
	ask_daemon_hook = hook;
	ask_daemon_hook_data = data;
}