/*
 * events.c:  Event handles
 *
 * Author:
 *	Dick Porter (dick@ximian.com)
 *
 * (C) 2002 Ximian, Inc.
 */

#include <config.h>
#include <glib.h>
#include <pthread.h>
#include <string.h>

#include <mono/io-layer/wapi.h>
#include <mono/io-layer/wapi-private.h>
#include <mono/io-layer/handles-private.h>
#include <mono/io-layer/event-private.h>
#include <mono/io-layer/io-trace.h>
#include <mono/utils/mono-once.h>
#include <mono/utils/mono-logger-internals.h>

static void event_signal(gpointer handle);
static gboolean event_own (gpointer handle);

static void namedevent_signal (gpointer handle);
static gboolean namedevent_own (gpointer handle);

struct _WapiHandleOps _wapi_event_ops = {
	NULL,			/* close */
	event_signal,		/* signal */
	event_own,		/* own */
	NULL,			/* is_owned */
	NULL,			/* special_wait */
	NULL			/* prewait */
};

struct _WapiHandleOps _wapi_namedevent_ops = {
	NULL,			/* close */
	namedevent_signal,	/* signal */
	namedevent_own,		/* own */
	NULL,			/* is_owned */
};

static gboolean event_pulse (gpointer handle);
static gboolean event_reset (gpointer handle);
static gboolean event_set (gpointer handle);

static gboolean namedevent_pulse (gpointer handle);
static gboolean namedevent_reset (gpointer handle);
static gboolean namedevent_set (gpointer handle);

static struct
{
	gboolean (*pulse)(gpointer handle);
	gboolean (*reset)(gpointer handle);
	gboolean (*set)(gpointer handle);
} event_ops[WAPI_HANDLE_COUNT] = {
		{NULL},
		{NULL},
		{NULL},
		{NULL},
		{NULL},
		{NULL},
		{event_pulse, event_reset, event_set},
		{NULL},
		{NULL},
		{NULL},
		{NULL},
		{NULL},
		{NULL},
		{namedevent_pulse, namedevent_reset, namedevent_set},
};

void _wapi_event_details (gpointer handle_info)
{
	struct _WapiHandle_event *event = (struct _WapiHandle_event *)handle_info;
	
	g_print ("manual: %s", event->manual?"TRUE":"FALSE");
}

static mono_once_t event_ops_once=MONO_ONCE_INIT;

static void event_ops_init (void)
{
	_wapi_handle_register_capabilities (WAPI_HANDLE_EVENT,
		(WapiHandleCapability)(WAPI_HANDLE_CAP_WAIT | WAPI_HANDLE_CAP_SIGNAL));
	_wapi_handle_register_capabilities (WAPI_HANDLE_NAMEDEVENT,
		(WapiHandleCapability)(WAPI_HANDLE_CAP_WAIT | WAPI_HANDLE_CAP_SIGNAL));
}

static void event_signal(gpointer handle)
{
	SetEvent(handle);
}

static gboolean event_own (gpointer handle)
{
	struct _WapiHandle_event *event_handle;
	gboolean ok;
	
	ok=_wapi_lookup_handle (handle, WAPI_HANDLE_EVENT,
				(gpointer *)&event_handle);
	if(ok==FALSE) {
		g_warning ("%s: error looking up event handle %p", __func__,
			   handle);
		return (FALSE);
	}
	
	MONO_TRACE (G_LOG_LEVEL_DEBUG, MONO_TRACE_IO_LAYER, "%s: owning event handle %p", __func__, handle);

	if(event_handle->manual==FALSE) {
		g_assert (event_handle->set_count > 0);
		
		if (--event_handle->set_count == 0) {
			_wapi_handle_set_signal_state (handle, FALSE, FALSE);
		}
	}

	return(TRUE);
}

static void namedevent_signal (gpointer handle)
{
	SetEvent (handle);
}

/* NB, always called with the shared handle lock held */
static gboolean namedevent_own (gpointer handle)
{
	struct _WapiHandle_namedevent *namedevent_handle;
	gboolean ok;
	
	MONO_TRACE (G_LOG_LEVEL_DEBUG, MONO_TRACE_IO_LAYER, "%s: owning named event handle %p", __func__, handle);

	ok = _wapi_lookup_handle (handle, WAPI_HANDLE_NAMEDEVENT,
				  (gpointer *)&namedevent_handle);
	if (ok == FALSE) {
		g_warning ("%s: error looking up named event handle %p",
			   __func__, handle);
		return(FALSE);
	}
	
	if (namedevent_handle->manual == FALSE) {
		g_assert (namedevent_handle->set_count > 0);
		
		if (--namedevent_handle->set_count == 0) {
			_wapi_handle_set_signal_state (handle, FALSE, FALSE);
		}
	}
	
	return (TRUE);
}
static gpointer event_create (WapiSecurityAttributes *security G_GNUC_UNUSED,
			      gboolean manual, gboolean initial)
{
	struct _WapiHandle_event event_handle = {0};
	gpointer handle;
	int thr_ret;
	
	/* Need to blow away any old errors here, because code tests
	 * for ERROR_ALREADY_EXISTS on success (!) to see if an event
	 * was freshly created
	 */
	SetLastError (ERROR_SUCCESS);

	MONO_TRACE (G_LOG_LEVEL_DEBUG, MONO_TRACE_IO_LAYER, "%s: Creating unnamed event", __func__);
	
	event_handle.manual = manual;
	event_handle.set_count = 0;

	if (initial == TRUE) {
		if (manual == FALSE) {
			event_handle.set_count = 1;
		}
	}
	
	handle = _wapi_handle_new (WAPI_HANDLE_EVENT, &event_handle);
	if (handle == _WAPI_HANDLE_INVALID) {
		g_warning ("%s: error creating event handle", __func__);
		SetLastError (ERROR_GEN_FAILURE);
		return(NULL);
	}

	thr_ret = _wapi_handle_lock_handle (handle);
	g_assert (thr_ret == 0);
	
	if (initial == TRUE) {
		_wapi_handle_set_signal_state (handle, TRUE, FALSE);
	}
	
	MONO_TRACE (G_LOG_LEVEL_DEBUG, MONO_TRACE_IO_LAYER, "%s: created new event handle %p", __func__, handle);

	thr_ret = _wapi_handle_unlock_handle (handle);
	g_assert (thr_ret == 0);

	return(handle);
}

static gpointer namedevent_create (WapiSecurityAttributes *security G_GNUC_UNUSED,
				   gboolean manual, gboolean initial,
				   const gunichar2 *name G_GNUC_UNUSED)
{
	struct _WapiHandle_namedevent namedevent_handle = {{{0}}, 0};
	gpointer handle;
	gchar *utf8_name;
	int thr_ret;
	
	/* w32 seems to guarantee that opening named objects can't
	 * race each other
	 */
	thr_ret = _wapi_namespace_lock ();
	g_assert (thr_ret == 0);

	/* Need to blow away any old errors here, because code tests
	 * for ERROR_ALREADY_EXISTS on success (!) to see if an event
	 * was freshly created
	 */
	SetLastError (ERROR_SUCCESS);
	
	utf8_name = g_utf16_to_utf8 (name, -1, NULL, NULL, NULL);
	
	MONO_TRACE (G_LOG_LEVEL_DEBUG, MONO_TRACE_IO_LAYER, "%s: Creating named event [%s]", __func__, utf8_name);
	
	handle = _wapi_search_handle_namespace (WAPI_HANDLE_NAMEDEVENT,
						utf8_name);
	if (handle == _WAPI_HANDLE_INVALID) {
		/* The name has already been used for a different
		 * object.
		 */
		SetLastError (ERROR_INVALID_HANDLE);
		goto cleanup;
	} else if (handle) {
		/* Not an error, but this is how the caller is
		 * informed that the event wasn't freshly created
		 */
		SetLastError (ERROR_ALREADY_EXISTS);
	} else {
		/* A new named event, so create both the private and
		 * shared parts
		 */
	
		strncpy (&namedevent_handle.sharedns.name [0], utf8_name, MAX_PATH);
		namedevent_handle.sharedns.name [MAX_PATH] = '\0';

		namedevent_handle.manual = manual;
		namedevent_handle.set_count = 0;
		
		if (initial == TRUE) {
			if (manual == FALSE) {
				namedevent_handle.set_count = 1;
			}
		}
		
		handle = _wapi_handle_new (WAPI_HANDLE_NAMEDEVENT,
					   &namedevent_handle);

		if (handle == _WAPI_HANDLE_INVALID) {
			g_warning ("%s: error creating event handle", __func__);
			SetLastError (ERROR_GEN_FAILURE);
			goto cleanup;
		}

		/* Set the initial state, as this is a completely new
		 * handle
		 */
		thr_ret = _wapi_handle_lock_handle (handle);
		g_assert (thr_ret == 0);
	
		if (initial == TRUE) {
			_wapi_handle_set_signal_state (handle, TRUE, FALSE);
		}

		thr_ret = _wapi_handle_unlock_handle (handle);
		g_assert (thr_ret == 0);
	}

	MONO_TRACE (G_LOG_LEVEL_DEBUG, MONO_TRACE_IO_LAYER, "%s: returning event handle %p", __func__, handle);

cleanup:
	g_free (utf8_name);

	_wapi_namespace_unlock (NULL);
	
	return handle;

}


/**
 * CreateEvent:
 * @security: Ignored for now.
 * @manual: Specifies whether the new event handle has manual or auto
 * reset behaviour.
 * @initial: Specifies whether the new event handle is initially
 * signalled or not.
 * @name:Pointer to a string specifying the name of this name, or
 * %NULL.  Currently ignored.
 *
 * Creates a new event handle.
 *
 * An event handle is signalled with SetEvent().  If the new handle is
 * a manual reset event handle, it remains signalled until it is reset
 * with ResetEvent().  An auto reset event remains signalled until a
 * single thread has waited for it, at which time the event handle is
 * automatically reset to unsignalled.
 *
 * Return value: A new handle, or %NULL on error.
 */
gpointer CreateEvent(WapiSecurityAttributes *security G_GNUC_UNUSED,
		     gboolean manual, gboolean initial,
		     const gunichar2 *name G_GNUC_UNUSED)
{
	mono_once (&event_ops_once, event_ops_init);

	if (name == NULL) {
		return(event_create (security, manual, initial));
	} else {
		return(namedevent_create (security, manual, initial, name));
	}
}

static gboolean event_pulse (gpointer handle)
{
	struct _WapiHandle_event *event_handle;
	gboolean ok;
	int thr_ret;
	
	ok = _wapi_lookup_handle (handle, WAPI_HANDLE_EVENT,
				  (gpointer *)&event_handle);
	if (ok == FALSE) {
		g_warning ("%s: error looking up event handle %p", __func__,
			   handle);
		return(FALSE);
	}
	
	thr_ret = _wapi_handle_unlock_handle (handle);
	g_assert (thr_ret == 0);

	MONO_TRACE (G_LOG_LEVEL_DEBUG, MONO_TRACE_IO_LAYER, "%s: Pulsing event handle %p", __func__, handle);

	if (event_handle->manual == TRUE) {
		_wapi_handle_set_signal_state (handle, TRUE, TRUE);
	} else {
		event_handle->set_count = 1;
		_wapi_handle_set_signal_state (handle, TRUE, FALSE);
	}

	thr_ret = _wapi_handle_unlock_handle (handle);
	g_assert (thr_ret == 0);
	
	if (event_handle->manual == TRUE) {
		/* For a manual-reset event, we're about to try and
		 * get the handle lock again, so give other threads a
		 * chance
		 */
		sched_yield ();

		/* Reset the handle signal state */
		/* I'm not sure whether or not we need a barrier here
		 * to make sure that all threads waiting on the event
		 * have proceeded.  Currently we rely on broadcasting
		 * a condition.
		 */
		MONO_TRACE (G_LOG_LEVEL_DEBUG, MONO_TRACE_IO_LAYER, "%s: Obtained write lock on event handle %p",
			   __func__, handle);

		thr_ret = _wapi_handle_lock_handle (handle);
		g_assert (thr_ret == 0);
		
		_wapi_handle_set_signal_state (handle, FALSE, FALSE);

		thr_ret = _wapi_handle_unlock_handle (handle);
		g_assert (thr_ret == 0);
	}

	return(TRUE);
}

static gboolean namedevent_pulse (gpointer handle)
{
	struct _WapiHandle_namedevent *namedevent_handle;
	gboolean ok;
	int thr_ret;
	
	ok = _wapi_lookup_handle (handle, WAPI_HANDLE_NAMEDEVENT,
				  (gpointer *)&namedevent_handle);
	if (ok == FALSE) {
		g_warning ("%s: error looking up named event handle %p",
			   __func__, handle);
		return(FALSE);
	}
	
	thr_ret = _wapi_handle_unlock_handle (handle);
	g_assert (thr_ret == 0);

	MONO_TRACE (G_LOG_LEVEL_DEBUG, MONO_TRACE_IO_LAYER, "%s: Pulsing named event handle %p", __func__, handle);

	if (namedevent_handle->manual == TRUE) {
		_wapi_handle_set_signal_state (handle, TRUE, TRUE);
	} else {
		namedevent_handle->set_count = 1;
		_wapi_handle_set_signal_state (handle, TRUE, FALSE);
	}

	thr_ret = _wapi_handle_unlock_handle (handle);
	g_assert (thr_ret == 0);
	
	if (namedevent_handle->manual == TRUE) {
		/* For a manual-reset event, we're about to try and
		 * get the handle lock again, so give other processes
		 * a chance
		 */
		_wapi_handle_spin (200);

		/* Reset the handle signal state */
		/* I'm not sure whether or not we need a barrier here
		 * to make sure that all threads waiting on the event
		 * have proceeded.  Currently we rely on waiting for
		 * twice the shared handle poll interval.
		 */
		MONO_TRACE (G_LOG_LEVEL_DEBUG, MONO_TRACE_IO_LAYER, "%s: Obtained write lock on event handle %p",
			   __func__, handle);

		thr_ret = _wapi_handle_lock_handle (handle);
		g_assert (thr_ret == 0);
		
		_wapi_handle_set_signal_state (handle, FALSE, FALSE);

		thr_ret = _wapi_handle_unlock_handle (handle);
		g_assert (thr_ret == 0);
	}

	return(TRUE);
}

/**
 * PulseEvent:
 * @handle: The event handle.
 *
 * Sets the event handle @handle to the signalled state, and then
 * resets it to unsignalled after informing any waiting threads.
 *
 * If @handle is a manual reset event, all waiting threads that can be
 * released immediately are released.  @handle is then reset.  If
 * @handle is an auto reset event, one waiting thread is released even
 * if multiple threads are waiting.
 *
 * Return value: %TRUE on success, %FALSE otherwise.  (Currently only
 * ever returns %TRUE).
 */
gboolean PulseEvent(gpointer handle)
{
	WapiHandleType type;
	
	if (handle == NULL) {
		SetLastError (ERROR_INVALID_HANDLE);
		return(FALSE);
	}
	
	type = _wapi_handle_type (handle);
	
	if (event_ops[type].pulse == NULL) {
		SetLastError (ERROR_INVALID_HANDLE);
		return(FALSE);
	}
	
	return(event_ops[type].pulse (handle));
}

static gboolean event_reset (gpointer handle)
{
	struct _WapiHandle_event *event_handle;
	gboolean ok;
	int thr_ret;
	
	ok = _wapi_lookup_handle (handle, WAPI_HANDLE_EVENT,
				  (gpointer *)&event_handle);
	if (ok == FALSE) {
		g_warning ("%s: error looking up event handle %p",
			   __func__, handle);
		return(FALSE);
	}

	MONO_TRACE (G_LOG_LEVEL_DEBUG, MONO_TRACE_IO_LAYER, "%s: Resetting event handle %p", __func__, handle);

	thr_ret = _wapi_handle_lock_handle (handle);
	g_assert (thr_ret == 0);
	
	if (_wapi_handle_issignalled (handle) == FALSE) {
		MONO_TRACE (G_LOG_LEVEL_DEBUG, MONO_TRACE_IO_LAYER, "%s: No need to reset event handle %p", __func__,
			   handle);
	} else {
		MONO_TRACE (G_LOG_LEVEL_DEBUG, MONO_TRACE_IO_LAYER, "%s: Obtained write lock on event handle %p",
			   __func__, handle);

		_wapi_handle_set_signal_state (handle, FALSE, FALSE);
	}
	
	event_handle->set_count = 0;
	
	thr_ret = _wapi_handle_unlock_handle (handle);
	g_assert (thr_ret == 0);
	
	return(TRUE);
}

static gboolean namedevent_reset (gpointer handle)
{
	struct _WapiHandle_namedevent *namedevent_handle;
	gboolean ok;
	int thr_ret;
	
	ok = _wapi_lookup_handle (handle, WAPI_HANDLE_NAMEDEVENT,
				  (gpointer *)&namedevent_handle);
	if (ok == FALSE) {
		g_warning ("%s: error looking up named event handle %p",
			   __func__, handle);
		return(FALSE);
	}

	MONO_TRACE (G_LOG_LEVEL_DEBUG, MONO_TRACE_IO_LAYER, "%s: Resetting named event handle %p", __func__, handle);

	thr_ret = _wapi_handle_lock_handle (handle);
	g_assert (thr_ret == 0);
	
	if (_wapi_handle_issignalled (handle) == FALSE) {
		MONO_TRACE (G_LOG_LEVEL_DEBUG, MONO_TRACE_IO_LAYER, "%s: No need to reset named event handle %p",
			   __func__, handle);
	} else {
		MONO_TRACE (G_LOG_LEVEL_DEBUG, MONO_TRACE_IO_LAYER, "%s: Obtained write lock on named event handle %p",
			   __func__, handle);

		_wapi_handle_set_signal_state (handle, FALSE, FALSE);
	}
	
	namedevent_handle->set_count = 0;
	
	thr_ret = _wapi_handle_unlock_handle (handle);
	g_assert (thr_ret == 0);
	
	return(TRUE);
}

/**
 * ResetEvent:
 * @handle: The event handle.
 *
 * Resets the event handle @handle to the unsignalled state.
 *
 * Return value: %TRUE on success, %FALSE otherwise.  (Currently only
 * ever returns %TRUE).
 */
gboolean ResetEvent(gpointer handle)
{
	WapiHandleType type;
	
	if (handle == NULL) {
		SetLastError (ERROR_INVALID_HANDLE);
		return(FALSE);
	}
	
	type = _wapi_handle_type (handle);
	
	if (event_ops[type].reset == NULL) {
		SetLastError (ERROR_INVALID_HANDLE);
		return(FALSE);
	}
	
	return(event_ops[type].reset (handle));
}

static gboolean event_set (gpointer handle)
{
	struct _WapiHandle_event *event_handle;
	gboolean ok;
	int thr_ret;
	
	ok = _wapi_lookup_handle (handle, WAPI_HANDLE_EVENT,
				  (gpointer *)&event_handle);
	if (ok == FALSE) {
		g_warning ("%s: error looking up event handle %p", __func__,
			   handle);
		return(FALSE);
	}
	
	thr_ret = _wapi_handle_lock_handle (handle);
	g_assert (thr_ret == 0);

	MONO_TRACE (G_LOG_LEVEL_DEBUG, MONO_TRACE_IO_LAYER, "%s: Setting event handle %p", __func__, handle);

	if (event_handle->manual == TRUE) {
		_wapi_handle_set_signal_state (handle, TRUE, TRUE);
	} else {
		event_handle->set_count = 1;
		_wapi_handle_set_signal_state (handle, TRUE, FALSE);
	}

	thr_ret = _wapi_handle_unlock_handle (handle);
	g_assert (thr_ret == 0);
	
	return(TRUE);
}

static gboolean namedevent_set (gpointer handle)
{
	struct _WapiHandle_namedevent *namedevent_handle;
	gboolean ok;
	int thr_ret;
	
	ok = _wapi_lookup_handle (handle, WAPI_HANDLE_NAMEDEVENT,
				  (gpointer *)&namedevent_handle);
	if (ok == FALSE) {
		g_warning ("%s: error looking up named event handle %p",
			   __func__, handle);
		return(FALSE);
	}
	
	thr_ret = _wapi_handle_lock_handle (handle);
	g_assert (thr_ret == 0);

	MONO_TRACE (G_LOG_LEVEL_DEBUG, MONO_TRACE_IO_LAYER, "%s: Setting named event handle %p", __func__, handle);

	if (namedevent_handle->manual == TRUE) {
		_wapi_handle_set_signal_state (handle, TRUE, TRUE);
	} else {
		namedevent_handle->set_count = 1;
		_wapi_handle_set_signal_state (handle, TRUE, TRUE);
	}

	thr_ret = _wapi_handle_unlock_handle (handle);
	g_assert (thr_ret == 0);

	return(TRUE);
}

/**
 * SetEvent:
 * @handle: The event handle
 *
 * Sets the event handle @handle to the signalled state.
 *
 * If @handle is a manual reset event, it remains signalled until it
 * is reset with ResetEvent().  An auto reset event remains signalled
 * until a single thread has waited for it, at which time @handle is
 * automatically reset to unsignalled.
 *
 * Return value: %TRUE on success, %FALSE otherwise.  (Currently only
 * ever returns %TRUE).
 */
gboolean SetEvent(gpointer handle)
{
	WapiHandleType type;
	
	if (handle == NULL) {
		SetLastError (ERROR_INVALID_HANDLE);
		return(FALSE);
	}
	
	type = _wapi_handle_type (handle);
	
	if (event_ops[type].set == NULL) {
		SetLastError (ERROR_INVALID_HANDLE);
		return(FALSE);
	}
	
	return(event_ops[type].set (handle));
}

gpointer OpenEvent (guint32 access G_GNUC_UNUSED, gboolean inherit G_GNUC_UNUSED, const gunichar2 *name)
{
	gpointer handle;
	gchar *utf8_name;
	int thr_ret;
	
	mono_once (&event_ops_once, event_ops_init);

	/* w32 seems to guarantee that opening named objects can't
	 * race each other
	 */
	thr_ret = _wapi_namespace_lock ();
	g_assert (thr_ret == 0);

	utf8_name = g_utf16_to_utf8 (name, -1, NULL, NULL, NULL);
	
	MONO_TRACE (G_LOG_LEVEL_DEBUG, MONO_TRACE_IO_LAYER, "%s: Opening named event [%s]", __func__, utf8_name);
	
	handle = _wapi_search_handle_namespace (WAPI_HANDLE_NAMEDEVENT,
						utf8_name);
	if (handle == _WAPI_HANDLE_INVALID) {
		/* The name has already been used for a different
		 * object.
		 */
		SetLastError (ERROR_INVALID_HANDLE);
		goto cleanup;
	} else if (!handle) {
		/* This name doesn't exist */
		SetLastError (ERROR_FILE_NOT_FOUND);	/* yes, really */
		goto cleanup;
	}

	MONO_TRACE (G_LOG_LEVEL_DEBUG, MONO_TRACE_IO_LAYER, "%s: returning named event handle %p", __func__, handle);

cleanup:
	g_free (utf8_name);

	_wapi_namespace_unlock (NULL);
	
	return handle;

}
