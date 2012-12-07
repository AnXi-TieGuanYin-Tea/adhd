/* Copyright (c) 2012 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include <dbus/dbus.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <syslog.h>
#include <sys/select.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include "cras_system_state.h"
#include "cras_tm.h"

static void dbus_watch_callback(void *arg)
{
	DBusWatch *watch = (DBusWatch *)arg;
	int fd, r, flags;
	fd_set readfds, writefds;
	struct timeval timeout;

	fd = dbus_watch_get_unix_fd(watch);

	FD_ZERO(&readfds);
	FD_SET(fd, &readfds);

	FD_ZERO(&writefds);
	FD_SET(fd, &writefds);

	timeout.tv_sec = 0;
	timeout.tv_usec = 0;

	r = select(fd + 1, &readfds, &writefds, NULL, &timeout);
	if (r <= 0)
		return;

	flags = 0;
	if (FD_ISSET(fd, &readfds))
		flags |= DBUS_WATCH_READABLE;
	if (FD_ISSET(fd, &writefds))
		flags |= DBUS_WATCH_WRITABLE;

	if (!dbus_watch_handle(watch, flags))
		syslog(LOG_WARNING, "Failed to handle D-Bus watch.");
}

static dbus_bool_t dbus_watch_add(DBusWatch *watch, void *data)
{
	int r;

	if (dbus_watch_get_enabled(watch)) {
		r = cras_system_add_select_fd(dbus_watch_get_unix_fd(watch),
					      dbus_watch_callback,
					      watch);
		if (r != 0)
			return FALSE;
	}

	return TRUE;
}

static void dbus_watch_remove(DBusWatch *watch, void *data)
{
	cras_system_rm_select_fd(dbus_watch_get_unix_fd(watch));
}

static void dbus_watch_toggled(DBusWatch *watch, void *data)
{
	if (dbus_watch_get_enabled(watch)) {
		dbus_watch_add(watch, NULL);
	} else {
		dbus_watch_remove(watch, NULL);
	}
}


static void dbus_timeout_callback(struct cras_timer *t, void *data)
{
	struct DBusTimeout *timeout = data;

	if (!dbus_timeout_handle(timeout))
		syslog(LOG_WARNING, "Failed to handle D-Bus timeout.");
}

static dbus_bool_t dbus_timeout_add(DBusTimeout *timeout, void *arg)
{
	struct cras_tm *tm = cras_system_state_get_tm();
	struct cras_timer *t;

	if (dbus_timeout_get_enabled(timeout)) {
		t = cras_tm_create_timer(tm,
					 dbus_timeout_get_interval(timeout),
					 dbus_timeout_callback, timeout);
		if (t == NULL)
			return FALSE;

		dbus_timeout_set_data(timeout, t, NULL);
	}

	return TRUE;
}

static void dbus_timeout_remove(DBusTimeout *timeout, void *arg)
{
	struct cras_tm *tm = cras_system_state_get_tm();
	struct cras_timer *t = dbus_timeout_get_data(timeout);

	cras_tm_cancel_timer(tm, t);
}

static void dbus_timeout_toggled(DBusTimeout *timeout, void *arg)
{
	dbus_timeout_remove(timeout, NULL);
	dbus_timeout_add(timeout, NULL);
}


DBusConnection *cras_dbus_connect_system_bus()
{
	DBusError dbus_error;
	DBusConnection *conn;

	dbus_error_init(&dbus_error);

	conn = dbus_bus_get(DBUS_BUS_SYSTEM, &dbus_error);
	if (!conn) {
		syslog(LOG_WARNING, "Failed to connect to D-Bus: %s",
		       dbus_error.message);
		dbus_error_free(&dbus_error);
		return NULL;
	}

	if (!dbus_connection_set_watch_functions(conn,
						 dbus_watch_add,
						 dbus_watch_remove,
						 dbus_watch_toggled,
						 NULL,
						 NULL))
		goto error;
	if (!dbus_connection_set_timeout_functions(conn,
						   dbus_timeout_add,
						   dbus_timeout_remove,
						   dbus_timeout_toggled,
						   NULL,
						   NULL))
		goto error;

	return conn;

error:
	syslog(LOG_WARNING, "Failed to setup D-Bus connection.");
	dbus_connection_unref(conn);
	return NULL;
}

void cras_dbus_dispatch(DBusConnection *conn)
{
	while (dbus_connection_dispatch(conn)
		== DBUS_DISPATCH_DATA_REMAINS)
		;
}

void cras_dbus_disconnect_system_bus(DBusConnection *conn)
{
	dbus_connection_unref(conn);
}