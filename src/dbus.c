/*-*- Mode: C; c-basic-offset: 8; indent-tabs-mode: nil -*-*/

/***
  This file is part of systemd.

  Copyright 2010 Lennart Poettering

  systemd is free software; you can redistribute it and/or modify it
  under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.

  systemd is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with systemd; If not, see <http://www.gnu.org/licenses/>.
***/

#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <errno.h>
#include <unistd.h>
#include <dbus/dbus.h>

#include "dbus.h"
#include "log.h"
#include "strv.h"
#include "cgroup.h"
#include "dbus-unit.h"
#include "dbus-job.h"
#include "dbus-manager.h"
#include "dbus-service.h"
#include "dbus-socket.h"
#include "dbus-target.h"
#include "dbus-device.h"
#include "dbus-mount.h"
#include "dbus-automount.h"
#include "dbus-snapshot.h"
#include "dbus-swap.h"
#include "dbus-timer.h"
#include "dbus-path.h"
#include "bus-errors.h"
#include "special.h"
#include "dbus-common.h"

#define CONNECTIONS_MAX 52

static const char bus_properties_interface[] = BUS_PROPERTIES_INTERFACE;
static const char bus_introspectable_interface[] = BUS_INTROSPECTABLE_INTERFACE;

const char *const bus_interface_table[] = {
        "org.freedesktop.DBus.Properties",     bus_properties_interface,
        "org.freedesktop.DBus.Introspectable", bus_introspectable_interface,
        "org.freedesktop.systemd1.Manager",    bus_manager_interface,
        "org.freedesktop.systemd1.Job",        bus_job_interface,
        "org.freedesktop.systemd1.Unit",       bus_unit_interface,
        "org.freedesktop.systemd1.Service",    bus_service_interface,
        "org.freedesktop.systemd1.Socket",     bus_socket_interface,
        "org.freedesktop.systemd1.Target",     bus_target_interface,
        "org.freedesktop.systemd1.Device",     bus_device_interface,
        "org.freedesktop.systemd1.Mount",      bus_mount_interface,
        "org.freedesktop.systemd1.Automount",  bus_automount_interface,
        "org.freedesktop.systemd1.Snapshot",   bus_snapshot_interface,
        "org.freedesktop.systemd1.Swap",       bus_swap_interface,
        "org.freedesktop.systemd1.Timer",      bus_timer_interface,
        "org.freedesktop.systemd1.Path",       bus_path_interface,
        NULL
};

static void bus_done_api(Manager *m);
static void bus_done_system(Manager *m);
static void bus_done_private(Manager *m);
static void shutdown_connection(Manager *m, DBusConnection *c);

static void bus_dispatch_status(DBusConnection *bus, DBusDispatchStatus status, void *data)  {
        Manager *m = data;

        assert(bus);
        assert(m);

        /* We maintain two sets, one for those connections where we
         * requested a dispatch, and another where we didn't. And then,
         * we move the connections between the two sets. */

        if (status == DBUS_DISPATCH_COMPLETE)
                set_move_one(m->bus_connections, m->bus_connections_for_dispatch, bus);
        else
                set_move_one(m->bus_connections_for_dispatch, m->bus_connections, bus);
}

void bus_watch_event(Manager *m, Watch *w, int events) {
        assert(m);
        assert(w);

        /* This is called by the event loop whenever there is
         * something happening on D-Bus' file handles. */

        if (!dbus_watch_get_enabled(w->data.bus_watch))
                return;

        dbus_watch_handle(w->data.bus_watch, bus_events_to_flags(events));
}

static dbus_bool_t bus_add_watch(DBusWatch *bus_watch, void *data) {
        Manager *m = data;
        Watch *w;
        struct epoll_event ev;

        assert(bus_watch);
        assert(m);

        if (!(w = new0(Watch, 1)))
                return FALSE;

        w->fd = dbus_watch_get_unix_fd(bus_watch);
        w->type = WATCH_DBUS_WATCH;
        w->data.bus_watch = bus_watch;

        zero(ev);
        ev.events = bus_flags_to_events(bus_watch);
        ev.data.ptr = w;

        if (epoll_ctl(m->epoll_fd, EPOLL_CTL_ADD, w->fd, &ev) < 0) {

                if (errno != EEXIST) {
                        free(w);
                        return FALSE;
                }

                /* Hmm, bloody D-Bus creates multiple watches on the
                 * same fd. epoll() does not like that. As a dirty
                 * hack we simply dup() the fd and hence get a second
                 * one we can safely add to the epoll(). */

                if ((w->fd = dup(w->fd)) < 0) {
                        free(w);
                        return FALSE;
                }

                if (epoll_ctl(m->epoll_fd, EPOLL_CTL_ADD, w->fd, &ev) < 0) {
                        close_nointr_nofail(w->fd);
                        free(w);
                        return FALSE;
                }

                w->fd_is_dupped = true;
        }

        dbus_watch_set_data(bus_watch, w, NULL);

        return TRUE;
}

static void bus_remove_watch(DBusWatch *bus_watch, void *data) {
        Manager *m = data;
        Watch *w;

        assert(bus_watch);
        assert(m);

        w = dbus_watch_get_data(bus_watch);
        if (!w)
                return;

        assert(w->type == WATCH_DBUS_WATCH);
        assert_se(epoll_ctl(m->epoll_fd, EPOLL_CTL_DEL, w->fd, NULL) >= 0);

        if (w->fd_is_dupped)
                close_nointr_nofail(w->fd);

        free(w);
}

static void bus_toggle_watch(DBusWatch *bus_watch, void *data) {
        Manager *m = data;
        Watch *w;
        struct epoll_event ev;

        assert(bus_watch);
        assert(m);

        w = dbus_watch_get_data(bus_watch);
        if (!w)
                return;

        assert(w->type == WATCH_DBUS_WATCH);

        zero(ev);
        ev.events = bus_flags_to_events(bus_watch);
        ev.data.ptr = w;

        assert_se(epoll_ctl(m->epoll_fd, EPOLL_CTL_MOD, w->fd, &ev) == 0);
}

static int bus_timeout_arm(Manager *m, Watch *w) {
        struct itimerspec its;

        assert(m);
        assert(w);

        zero(its);

        if (dbus_timeout_get_enabled(w->data.bus_timeout)) {
                timespec_store(&its.it_value, dbus_timeout_get_interval(w->data.bus_timeout) * USEC_PER_MSEC);
                its.it_interval = its.it_value;
        }

        if (timerfd_settime(w->fd, 0, &its, NULL) < 0)
                return -errno;

        return 0;
}

void bus_timeout_event(Manager *m, Watch *w, int events) {
        assert(m);
        assert(w);

        /* This is called by the event loop whenever there is
         * something happening on D-Bus' file handles. */

        if (!(dbus_timeout_get_enabled(w->data.bus_timeout)))
                return;

        dbus_timeout_handle(w->data.bus_timeout);
}

static dbus_bool_t bus_add_timeout(DBusTimeout *timeout, void *data) {
        Manager *m = data;
        Watch *w;
        struct epoll_event ev;

        assert(timeout);
        assert(m);

        if (!(w = new0(Watch, 1)))
                return FALSE;

        if ((w->fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK|TFD_CLOEXEC)) < 0)
                goto fail;

        w->type = WATCH_DBUS_TIMEOUT;
        w->data.bus_timeout = timeout;

        if (bus_timeout_arm(m, w) < 0)
                goto fail;

        zero(ev);
        ev.events = EPOLLIN;
        ev.data.ptr = w;

        if (epoll_ctl(m->epoll_fd, EPOLL_CTL_ADD, w->fd, &ev) < 0)
                goto fail;

        dbus_timeout_set_data(timeout, w, NULL);

        return TRUE;

fail:
        if (w->fd >= 0)
                close_nointr_nofail(w->fd);

        free(w);
        return FALSE;
}

static void bus_remove_timeout(DBusTimeout *timeout, void *data) {
        Manager *m = data;
        Watch *w;

        assert(timeout);
        assert(m);

        w = dbus_timeout_get_data(timeout);
        if (!w)
                return;

        assert(w->type == WATCH_DBUS_TIMEOUT);

        assert_se(epoll_ctl(m->epoll_fd, EPOLL_CTL_DEL, w->fd, NULL) >= 0);
        close_nointr_nofail(w->fd);
        free(w);
}

static void bus_toggle_timeout(DBusTimeout *timeout, void *data) {
        Manager *m = data;
        Watch *w;
        int r;

        assert(timeout);
        assert(m);

        w = dbus_timeout_get_data(timeout);
        if (!w)
                return;

        assert(w->type == WATCH_DBUS_TIMEOUT);

        if ((r = bus_timeout_arm(m, w)) < 0)
                log_error("Failed to rearm timer: %s", strerror(-r));
}

static DBusHandlerResult api_bus_message_filter(DBusConnection *connection, DBusMessage *message, void *data) {
        Manager *m = data;
        DBusError error;
        DBusMessage *reply = NULL;

        assert(connection);
        assert(message);
        assert(m);

        dbus_error_init(&error);

        if (dbus_message_get_type(message) == DBUS_MESSAGE_TYPE_METHOD_CALL ||
            dbus_message_get_type(message) == DBUS_MESSAGE_TYPE_SIGNAL)
                log_debug("Got D-Bus request: %s.%s() on %s",
                          dbus_message_get_interface(message),
                          dbus_message_get_member(message),
                          dbus_message_get_path(message));

        if (dbus_message_is_signal(message, DBUS_INTERFACE_LOCAL, "Disconnected")) {
                log_debug("API D-Bus connection terminated.");
                bus_done_api(m);

        } else if (dbus_message_is_signal(message, DBUS_INTERFACE_DBUS, "NameOwnerChanged")) {
                const char *name, *old_owner, *new_owner;

                if (!dbus_message_get_args(message, &error,
                                           DBUS_TYPE_STRING, &name,
                                           DBUS_TYPE_STRING, &old_owner,
                                           DBUS_TYPE_STRING, &new_owner,
                                           DBUS_TYPE_INVALID))
                        log_error("Failed to parse NameOwnerChanged message: %s", bus_error_message(&error));
                else  {
                        if (set_remove(BUS_CONNECTION_SUBSCRIBED(m, connection), (char*) name))
                                log_debug("Subscription client vanished: %s (left: %u)", name, set_size(BUS_CONNECTION_SUBSCRIBED(m, connection)));

                        if (old_owner[0] == 0)
                                old_owner = NULL;

                        if (new_owner[0] == 0)
                                new_owner = NULL;

                        manager_dispatch_bus_name_owner_changed(m, name, old_owner, new_owner);
                }
        } else if (dbus_message_is_signal(message, "org.freedesktop.systemd1.Activator", "ActivationRequest")) {
                const char *name;

                if (!dbus_message_get_args(message, &error,
                                           DBUS_TYPE_STRING, &name,
                                           DBUS_TYPE_INVALID))
                        log_error("Failed to parse ActivationRequest message: %s", bus_error_message(&error));
                else  {
                        int r;
                        Unit *u;

                        log_debug("Got D-Bus activation request for %s", name);

                        if (manager_unit_pending_inactive(m, SPECIAL_DBUS_SERVICE) ||
                            manager_unit_pending_inactive(m, SPECIAL_DBUS_SOCKET)) {
                                r = -EADDRNOTAVAIL;
                                dbus_set_error(&error, BUS_ERROR_SHUTTING_DOWN, "Refusing activation, D-Bus is shutting down.");
                        } else {
                                r = manager_load_unit(m, name, NULL, &error, &u);

                                if (r >= 0 && u->meta.refuse_manual_start)
                                        r = -EPERM;

                                if (r >= 0)
                                        r = manager_add_job(m, JOB_START, u, JOB_REPLACE, true, &error, NULL);
                        }

                        if (r < 0) {
                                const char *id, *text;

                                log_debug("D-Bus activation failed for %s: %s", name, strerror(-r));

                                if (!(reply = dbus_message_new_signal("/org/freedesktop/systemd1", "org.freedesktop.systemd1.Activator", "ActivationFailure")))
                                        goto oom;

                                id = error.name ? error.name : bus_errno_to_dbus(r);
                                text = bus_error(&error, r);

                                if (!dbus_message_set_destination(reply, DBUS_SERVICE_DBUS) ||
                                    !dbus_message_append_args(reply,
                                                              DBUS_TYPE_STRING, &name,
                                                              DBUS_TYPE_STRING, &id,
                                                              DBUS_TYPE_STRING, &text,
                                                              DBUS_TYPE_INVALID))
                                        goto oom;
                        }

                        /* On success we don't do anything, the service will be spawned now */
                }
        }

        dbus_error_free(&error);

        if (reply) {
                if (!dbus_connection_send(connection, reply, NULL))
                        goto oom;

                dbus_message_unref(reply);
        }

        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

oom:
        if (reply)
                dbus_message_unref(reply);

        dbus_error_free(&error);

        return DBUS_HANDLER_RESULT_NEED_MEMORY;
}

static DBusHandlerResult system_bus_message_filter(DBusConnection *connection, DBusMessage *message, void *data) {
        Manager *m = data;
        DBusError error;

        assert(connection);
        assert(message);
        assert(m);

        dbus_error_init(&error);

        if (m->api_bus != m->system_bus &&
            (dbus_message_get_type(message) == DBUS_MESSAGE_TYPE_METHOD_CALL ||
             dbus_message_get_type(message) == DBUS_MESSAGE_TYPE_SIGNAL))
                log_debug("Got D-Bus request on system bus: %s.%s() on %s",
                          dbus_message_get_interface(message),
                          dbus_message_get_member(message),
                          dbus_message_get_path(message));

        if (dbus_message_is_signal(message, DBUS_INTERFACE_LOCAL, "Disconnected")) {
                log_debug("System D-Bus connection terminated.");
                bus_done_system(m);

        } else if (m->running_as != MANAGER_SYSTEM &&
                   dbus_message_is_signal(message, "org.freedesktop.systemd1.Agent", "Released")) {

                const char *cgroup;

                if (!dbus_message_get_args(message, &error,
                                           DBUS_TYPE_STRING, &cgroup,
                                           DBUS_TYPE_INVALID))
                        log_error("Failed to parse Released message: %s", bus_error_message(&error));
                else
                        cgroup_notify_empty(m, cgroup);
        }

        dbus_error_free(&error);
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static DBusHandlerResult private_bus_message_filter(DBusConnection *connection, DBusMessage *message, void *data) {
        Manager *m = data;
        DBusError error;

        assert(connection);
        assert(message);
        assert(m);

        dbus_error_init(&error);

        if (dbus_message_get_type(message) == DBUS_MESSAGE_TYPE_METHOD_CALL ||
            dbus_message_get_type(message) == DBUS_MESSAGE_TYPE_SIGNAL)
                log_debug("Got D-Bus request: %s.%s() on %s",
                          dbus_message_get_interface(message),
                          dbus_message_get_member(message),
                          dbus_message_get_path(message));

        if (dbus_message_is_signal(message, DBUS_INTERFACE_LOCAL, "Disconnected"))
                shutdown_connection(m, connection);
        else if (m->running_as == MANAGER_SYSTEM &&
                 dbus_message_is_signal(message, "org.freedesktop.systemd1.Agent", "Released")) {

                const char *cgroup;

                if (!dbus_message_get_args(message, &error,
                                           DBUS_TYPE_STRING, &cgroup,
                                           DBUS_TYPE_INVALID))
                        log_error("Failed to parse Released message: %s", bus_error_message(&error));
                else
                        cgroup_notify_empty(m, cgroup);

                /* Forward the message to the system bus, so that user
                 * instances are notified as well */

                if (m->system_bus)
                        dbus_connection_send(m->system_bus, message, NULL);
        }

        dbus_error_free(&error);

        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

unsigned bus_dispatch(Manager *m) {
        DBusConnection *c;

        assert(m);

        if (m->queued_message) {
                /* If we cannot get rid of this message we won't
                 * dispatch any D-Bus messages, so that we won't end
                 * up wanting to queue another message. */

                if (m->queued_message_connection)
                        if (!dbus_connection_send(m->queued_message_connection, m->queued_message, NULL))
                                return 0;

                dbus_message_unref(m->queued_message);
                m->queued_message = NULL;
                m->queued_message_connection = NULL;
        }

        if ((c = set_first(m->bus_connections_for_dispatch))) {
                if (dbus_connection_dispatch(c) == DBUS_DISPATCH_COMPLETE)
                        set_move_one(m->bus_connections, m->bus_connections_for_dispatch, c);

                return 1;
        }

        return 0;
}

static void request_name_pending_cb(DBusPendingCall *pending, void *userdata) {
        DBusMessage *reply;
        DBusError error;

        dbus_error_init(&error);

        assert_se(reply = dbus_pending_call_steal_reply(pending));

        switch (dbus_message_get_type(reply)) {

        case DBUS_MESSAGE_TYPE_ERROR:

                assert_se(dbus_set_error_from_message(&error, reply));
                log_warning("RequestName() failed: %s", bus_error_message(&error));
                break;

        case DBUS_MESSAGE_TYPE_METHOD_RETURN: {
                uint32_t r;

                if (!dbus_message_get_args(reply,
                                           &error,
                                           DBUS_TYPE_UINT32, &r,
                                           DBUS_TYPE_INVALID)) {
                        log_error("Failed to parse RequestName() reply: %s", bus_error_message(&error));
                        break;
                }

                if (r == 1)
                        log_debug("Successfully acquired name.");
                else
                        log_error("Name already owned.");

                break;
        }

        default:
                assert_not_reached("Invalid reply message");
        }

        dbus_message_unref(reply);
        dbus_error_free(&error);
}

static int request_name(Manager *m) {
        const char *name = "org.freedesktop.systemd1";
        /* Allow replacing of our name, to ease implementation of
         * reexecution, where we keep the old connection open until
         * after the new connection is set up and the name installed
         * to allow clients to synchronously wait for reexecution to
         * finish */
        uint32_t flags = DBUS_NAME_FLAG_ALLOW_REPLACEMENT|DBUS_NAME_FLAG_REPLACE_EXISTING;
        DBusMessage *message = NULL;
        DBusPendingCall *pending = NULL;

        if (!(message = dbus_message_new_method_call(
                              DBUS_SERVICE_DBUS,
                              DBUS_PATH_DBUS,
                              DBUS_INTERFACE_DBUS,
                              "RequestName")))
                goto oom;

        if (!dbus_message_append_args(
                            message,
                            DBUS_TYPE_STRING, &name,
                            DBUS_TYPE_UINT32, &flags,
                            DBUS_TYPE_INVALID))
                goto oom;

        if (!dbus_connection_send_with_reply(m->api_bus, message, &pending, -1))
                goto oom;

        if (!dbus_pending_call_set_notify(pending, request_name_pending_cb, m, NULL))
                goto oom;

        dbus_message_unref(message);
        dbus_pending_call_unref(pending);

        /* We simple ask for the name and don't wait for it. Sooner or
         * later we'll have it. */

        return 0;

oom:
        if (pending) {
                dbus_pending_call_cancel(pending);
                dbus_pending_call_unref(pending);
        }

        if (message)
                dbus_message_unref(message);

        return -ENOMEM;
}

static void query_name_list_pending_cb(DBusPendingCall *pending, void *userdata) {
        DBusMessage *reply;
        DBusError error;
        Manager *m = userdata;

        assert(m);

        dbus_error_init(&error);

        assert_se(reply = dbus_pending_call_steal_reply(pending));

        switch (dbus_message_get_type(reply)) {

        case DBUS_MESSAGE_TYPE_ERROR:

                assert_se(dbus_set_error_from_message(&error, reply));
                log_warning("ListNames() failed: %s", bus_error_message(&error));
                break;

        case DBUS_MESSAGE_TYPE_METHOD_RETURN: {
                int r;
                char **l;

                if ((r = bus_parse_strv(reply, &l)) < 0)
                        log_warning("Failed to parse ListNames() reply: %s", strerror(-r));
                else {
                        char **t;

                        STRV_FOREACH(t, l)
                                /* This is a bit hacky, we say the
                                 * owner of the name is the name
                                 * itself, because we don't want the
                                 * extra traffic to figure out the
                                 * real owner. */
                                manager_dispatch_bus_name_owner_changed(m, *t, NULL, *t);

                        strv_free(l);
                }

                break;
        }

        default:
                assert_not_reached("Invalid reply message");
        }

        dbus_message_unref(reply);
        dbus_error_free(&error);
}

static int query_name_list(Manager *m) {
        DBusMessage *message = NULL;
        DBusPendingCall *pending = NULL;

        /* Asks for the currently installed bus names */

        if (!(message = dbus_message_new_method_call(
                              DBUS_SERVICE_DBUS,
                              DBUS_PATH_DBUS,
                              DBUS_INTERFACE_DBUS,
                              "ListNames")))
                goto oom;

        if (!dbus_connection_send_with_reply(m->api_bus, message, &pending, -1))
                goto oom;

        if (!dbus_pending_call_set_notify(pending, query_name_list_pending_cb, m, NULL))
                goto oom;

        dbus_message_unref(message);
        dbus_pending_call_unref(pending);

        /* We simple ask for the list and don't wait for it. Sooner or
         * later we'll get it. */

        return 0;

oom:
        if (pending) {
                dbus_pending_call_cancel(pending);
                dbus_pending_call_unref(pending);
        }

        if (message)
                dbus_message_unref(message);

        return -ENOMEM;
}

static int bus_setup_loop(Manager *m, DBusConnection *bus) {
        assert(m);
        assert(bus);

        dbus_connection_set_exit_on_disconnect(bus, FALSE);

        if (!dbus_connection_set_watch_functions(bus, bus_add_watch, bus_remove_watch, bus_toggle_watch, m, NULL) ||
            !dbus_connection_set_timeout_functions(bus, bus_add_timeout, bus_remove_timeout, bus_toggle_timeout, m, NULL)) {
                log_error("Not enough memory");
                return -ENOMEM;
        }

        if (set_put(m->bus_connections_for_dispatch, bus) < 0) {
                log_error("Not enough memory");
                return -ENOMEM;
        }

        dbus_connection_set_dispatch_status_function(bus, bus_dispatch_status, m, NULL);
        return 0;
}

static dbus_bool_t allow_only_same_user(DBusConnection *connection, unsigned long uid, void *data) {
        return uid == 0 || uid == geteuid();
}

static void bus_new_connection(
                DBusServer *server,
                DBusConnection *new_connection,
                void *data) {

        Manager *m = data;

        assert(m);

        if (set_size(m->bus_connections) >= CONNECTIONS_MAX) {
                log_error("Too many concurrent connections.");
                return;
        }

        dbus_connection_set_unix_user_function(new_connection, allow_only_same_user, NULL, NULL);

        if (bus_setup_loop(m, new_connection) < 0)
                return;

        if (!dbus_connection_register_object_path(new_connection, "/org/freedesktop/systemd1", &bus_manager_vtable, m) ||
            !dbus_connection_register_fallback(new_connection, "/org/freedesktop/systemd1/unit", &bus_unit_vtable, m) ||
            !dbus_connection_register_fallback(new_connection, "/org/freedesktop/systemd1/job", &bus_job_vtable, m) ||
            !dbus_connection_add_filter(new_connection, private_bus_message_filter, m, NULL)) {
                log_error("Not enough memory.");
                return;
        }

        log_debug("Accepted connection on private bus.");

        dbus_connection_ref(new_connection);
}

static int bus_init_system(Manager *m) {
        DBusError error;
        int r;

        assert(m);

        dbus_error_init(&error);

        if (m->system_bus)
                return 0;

        if (m->running_as == MANAGER_SYSTEM && m->api_bus)
                m->system_bus = m->api_bus;
        else {
                if (!(m->system_bus = dbus_bus_get_private(DBUS_BUS_SYSTEM, &error))) {
                        log_debug("Failed to get system D-Bus connection, retrying later: %s", bus_error_message(&error));
                        r = 0;
                        goto fail;
                }

                if ((r = bus_setup_loop(m, m->system_bus)) < 0)
                        goto fail;
        }

        if (!dbus_connection_add_filter(m->system_bus, system_bus_message_filter, m, NULL)) {
                log_error("Not enough memory");
                r = -ENOMEM;
                goto fail;
        }

        if (m->running_as != MANAGER_SYSTEM) {
                dbus_bus_add_match(m->system_bus,
                                   "type='signal',"
                                   "interface='org.freedesktop.systemd1.Agent',"
                                   "member='Released',"
                                   "path='/org/freedesktop/systemd1/agent'",
                                   &error);

                if (dbus_error_is_set(&error)) {
                        log_error("Failed to register match: %s", bus_error_message(&error));
                        r = -EIO;
                        goto fail;
                }
        }

        if (m->api_bus != m->system_bus) {
                char *id;
                log_debug("Successfully connected to system D-Bus bus %s as %s",
                         strnull((id = dbus_connection_get_server_id(m->system_bus))),
                         strnull(dbus_bus_get_unique_name(m->system_bus)));
                dbus_free(id);
        }

        return 0;

fail:
        bus_done_system(m);
        dbus_error_free(&error);

        return r;
}

static int bus_init_api(Manager *m) {
        DBusError error;
        int r;

        assert(m);

        dbus_error_init(&error);

        if (m->api_bus)
                return 0;

        if (m->running_as == MANAGER_SYSTEM && m->system_bus)
                m->api_bus = m->system_bus;
        else {
                if (!(m->api_bus = dbus_bus_get_private(m->running_as == MANAGER_USER ? DBUS_BUS_SESSION : DBUS_BUS_SYSTEM, &error))) {
                        log_debug("Failed to get API D-Bus connection, retrying later: %s", bus_error_message(&error));
                        r = 0;
                        goto fail;
                }

                if ((r = bus_setup_loop(m, m->api_bus)) < 0)
                        goto fail;
        }

        if (!dbus_connection_register_object_path(m->api_bus, "/org/freedesktop/systemd1", &bus_manager_vtable, m) ||
            !dbus_connection_register_fallback(m->api_bus, "/org/freedesktop/systemd1/unit", &bus_unit_vtable, m) ||
            !dbus_connection_register_fallback(m->api_bus, "/org/freedesktop/systemd1/job", &bus_job_vtable, m) ||
            !dbus_connection_add_filter(m->api_bus, api_bus_message_filter, m, NULL)) {
                log_error("Not enough memory");
                r = -ENOMEM;
                goto fail;
        }

        /* Get NameOwnerChange messages */
        dbus_bus_add_match(m->api_bus,
                           "type='signal',"
                           "sender='"DBUS_SERVICE_DBUS"',"
                           "interface='"DBUS_INTERFACE_DBUS"',"
                           "member='NameOwnerChanged',"
                           "path='"DBUS_PATH_DBUS"'",
                           &error);

        if (dbus_error_is_set(&error)) {
                log_error("Failed to register match: %s", bus_error_message(&error));
                r = -EIO;
                goto fail;
        }

        /* Get activation requests */
        dbus_bus_add_match(m->api_bus,
                           "type='signal',"
                           "sender='"DBUS_SERVICE_DBUS"',"
                           "interface='org.freedesktop.systemd1.Activator',"
                           "member='ActivationRequest',"
                           "path='"DBUS_PATH_DBUS"'",
                           &error);

        if (dbus_error_is_set(&error)) {
                log_error("Failed to register match: %s", bus_error_message(&error));
                r = -EIO;
                goto fail;
        }

        if ((r = request_name(m)) < 0)
                goto fail;

        if ((r = query_name_list(m)) < 0)
                goto fail;

        if (m->api_bus != m->system_bus) {
                char *id;
                log_debug("Successfully connected to API D-Bus bus %s as %s",
                         strnull((id = dbus_connection_get_server_id(m->api_bus))),
                         strnull(dbus_bus_get_unique_name(m->api_bus)));
                dbus_free(id);
        }

        return 0;

fail:
        bus_done_api(m);
        dbus_error_free(&error);

        return r;
}

static int bus_init_private(Manager *m) {
        DBusError error;
        int r;
        const char *const external_only[] = {
                "EXTERNAL",
                NULL
        };

        assert(m);

        dbus_error_init(&error);

        if (m->private_bus)
                return 0;

        if (m->running_as == MANAGER_SYSTEM) {

                /* We want the private bus only when running as init */
                if (getpid() != 1)
                        return 0;

                unlink("/run/systemd/private");
                m->private_bus = dbus_server_listen("unix:path=/run/systemd/private", &error);
        } else {
                const char *e;
                char *p;

                e = getenv("XDG_RUNTIME_DIR");
                if (!e)
                        return 0;

                if (asprintf(&p, "unix:path=%s/systemd/private", e) < 0) {
                        log_error("Not enough memory");
                        r = -ENOMEM;
                        goto fail;
                }

                mkdir_parents(p+10, 0755);
                unlink(p+10);
                m->private_bus = dbus_server_listen(p, &error);
                free(p);
        }

        if (!m->private_bus) {
                log_error("Failed to create private D-Bus server: %s", bus_error_message(&error));
                r = -EIO;
                goto fail;
        }

        if (!dbus_server_set_auth_mechanisms(m->private_bus, (const char**) external_only) ||
            !dbus_server_set_watch_functions(m->private_bus, bus_add_watch, bus_remove_watch, bus_toggle_watch, m, NULL) ||
            !dbus_server_set_timeout_functions(m->private_bus, bus_add_timeout, bus_remove_timeout, bus_toggle_timeout, m, NULL)) {
                log_error("Not enough memory");
                r = -ENOMEM;
                goto fail;
        }

        dbus_server_set_new_connection_function(m->private_bus, bus_new_connection, m, NULL);

        log_debug("Successfully created private D-Bus server.");

        return 0;

fail:
        bus_done_private(m);
        dbus_error_free(&error);

        return r;
}

int bus_init(Manager *m, bool try_bus_connect) {
        int r;

        if (set_ensure_allocated(&m->bus_connections, trivial_hash_func, trivial_compare_func) < 0 ||
            set_ensure_allocated(&m->bus_connections_for_dispatch, trivial_hash_func, trivial_compare_func) < 0) {
                log_error("Not enough memory");
                return -ENOMEM;
        }

        if (m->name_data_slot < 0)
                if (!dbus_pending_call_allocate_data_slot(&m->name_data_slot)) {
                        log_error("Not enough memory");
                        return -ENOMEM;
                }

        if (m->subscribed_data_slot < 0)
                if (!dbus_connection_allocate_data_slot(&m->subscribed_data_slot)) {
                        log_error("Not enough memory");
                        return -ENOMEM;
                }

        if (try_bus_connect) {
                if ((r = bus_init_system(m)) < 0 ||
                    (r = bus_init_api(m)) < 0)
                        return r;
        }

        if ((r = bus_init_private(m)) < 0)
                return r;

        return 0;
}

static void shutdown_connection(Manager *m, DBusConnection *c) {
        Set *s;
        Job *j;
        Iterator i;

        HASHMAP_FOREACH(j, m->jobs, i)
                if (j->bus == c) {
                        free(j->bus_client);
                        j->bus_client = NULL;

                        j->bus = NULL;
                }

        set_remove(m->bus_connections, c);
        set_remove(m->bus_connections_for_dispatch, c);

        if ((s = BUS_CONNECTION_SUBSCRIBED(m, c))) {
                char *t;

                while ((t = set_steal_first(s)))
                        free(t);

                set_free(s);
        }

        if (m->queued_message_connection == c) {
                m->queued_message_connection = NULL;

                if (m->queued_message) {
                        dbus_message_unref(m->queued_message);
                        m->queued_message = NULL;
                }
        }

        dbus_connection_set_dispatch_status_function(c, NULL, NULL, NULL);
        dbus_connection_flush(c);
        dbus_connection_close(c);
        dbus_connection_unref(c);
}

static void bus_done_api(Manager *m) {
        assert(m);

        if (m->api_bus) {
                if (m->system_bus == m->api_bus)
                        m->system_bus = NULL;

                shutdown_connection(m, m->api_bus);
                m->api_bus = NULL;
        }


       if (m->queued_message) {
               dbus_message_unref(m->queued_message);
               m->queued_message = NULL;
       }
}

static void bus_done_system(Manager *m) {
        assert(m);

        if (m->system_bus == m->api_bus)
                bus_done_api(m);

        if (m->system_bus) {
                shutdown_connection(m, m->system_bus);
                m->system_bus = NULL;
        }
}

static void bus_done_private(Manager *m) {

        if (m->private_bus) {
                dbus_server_disconnect(m->private_bus);
                dbus_server_unref(m->private_bus);
                m->private_bus = NULL;
        }
}

void bus_done(Manager *m) {
        DBusConnection *c;

        bus_done_api(m);
        bus_done_system(m);
        bus_done_private(m);

        while ((c = set_steal_first(m->bus_connections)))
                shutdown_connection(m, c);

        while ((c = set_steal_first(m->bus_connections_for_dispatch)))
                shutdown_connection(m, c);

        set_free(m->bus_connections);
        set_free(m->bus_connections_for_dispatch);

        if (m->name_data_slot >= 0)
               dbus_pending_call_free_data_slot(&m->name_data_slot);

        if (m->subscribed_data_slot >= 0)
                dbus_connection_free_data_slot(&m->subscribed_data_slot);
}

static void query_pid_pending_cb(DBusPendingCall *pending, void *userdata) {
        Manager *m = userdata;
        DBusMessage *reply;
        DBusError error;
        const char *name;

        dbus_error_init(&error);

        assert_se(name = BUS_PENDING_CALL_NAME(m, pending));
        assert_se(reply = dbus_pending_call_steal_reply(pending));

        switch (dbus_message_get_type(reply)) {

        case DBUS_MESSAGE_TYPE_ERROR:

                assert_se(dbus_set_error_from_message(&error, reply));
                log_warning("GetConnectionUnixProcessID() failed: %s", bus_error_message(&error));
                break;

        case DBUS_MESSAGE_TYPE_METHOD_RETURN: {
                uint32_t r;

                if (!dbus_message_get_args(reply,
                                           &error,
                                           DBUS_TYPE_UINT32, &r,
                                           DBUS_TYPE_INVALID)) {
                        log_error("Failed to parse GetConnectionUnixProcessID() reply: %s", bus_error_message(&error));
                        break;
                }

                manager_dispatch_bus_query_pid_done(m, name, (pid_t) r);
                break;
        }

        default:
                assert_not_reached("Invalid reply message");
        }

        dbus_message_unref(reply);
        dbus_error_free(&error);
}

int bus_query_pid(Manager *m, const char *name) {
        DBusMessage *message = NULL;
        DBusPendingCall *pending = NULL;
        char *n = NULL;

        assert(m);
        assert(name);

        if (!(message = dbus_message_new_method_call(
                              DBUS_SERVICE_DBUS,
                              DBUS_PATH_DBUS,
                              DBUS_INTERFACE_DBUS,
                              "GetConnectionUnixProcessID")))
                goto oom;

        if (!(dbus_message_append_args(
                              message,
                              DBUS_TYPE_STRING, &name,
                              DBUS_TYPE_INVALID)))
                goto oom;

        if (!dbus_connection_send_with_reply(m->api_bus, message, &pending, -1))
                goto oom;

        if (!(n = strdup(name)))
                goto oom;

        if (!dbus_pending_call_set_data(pending, m->name_data_slot, n, free))
                goto oom;

        n = NULL;

        if (!dbus_pending_call_set_notify(pending, query_pid_pending_cb, m, NULL))
                goto oom;

        dbus_message_unref(message);
        dbus_pending_call_unref(pending);

        return 0;

oom:
        free(n);

        if (pending) {
                dbus_pending_call_cancel(pending);
                dbus_pending_call_unref(pending);
        }

        if (message)
                dbus_message_unref(message);

        return -ENOMEM;
}

int bus_broadcast(Manager *m, DBusMessage *message) {
        bool oom = false;
        Iterator i;
        DBusConnection *c;

        assert(m);
        assert(message);

        SET_FOREACH(c, m->bus_connections_for_dispatch, i)
                if (c != m->system_bus || m->running_as == MANAGER_SYSTEM)
                        oom = !dbus_connection_send(c, message, NULL);

        SET_FOREACH(c, m->bus_connections, i)
                if (c != m->system_bus || m->running_as == MANAGER_SYSTEM)
                        oom = !dbus_connection_send(c, message, NULL);

        return oom ? -ENOMEM : 0;
}

bool bus_has_subscriber(Manager *m) {
        Iterator i;
        DBusConnection *c;

        assert(m);

        SET_FOREACH(c, m->bus_connections_for_dispatch, i)
                if (bus_connection_has_subscriber(m, c))
                        return true;

        SET_FOREACH(c, m->bus_connections, i)
                if (bus_connection_has_subscriber(m, c))
                        return true;

        return false;
}

bool bus_connection_has_subscriber(Manager *m, DBusConnection *c) {
        assert(m);
        assert(c);

        return !set_isempty(BUS_CONNECTION_SUBSCRIBED(m, c));
}

int bus_fdset_add_all(Manager *m, FDSet *fds) {
        Iterator i;
        DBusConnection *c;

        assert(m);
        assert(fds);

        /* When we are about to reexecute we add all D-Bus fds to the
         * set to pass over to the newly executed systemd. They won't
         * be used there however, except that they are closed at the
         * very end of deserialization, those making it possible for
         * clients to synchronously wait for systemd to reexec by
         * simply waiting for disconnection */

        SET_FOREACH(c, m->bus_connections_for_dispatch, i) {
                int fd;

                if (dbus_connection_get_unix_fd(c, &fd)) {
                        fd = fdset_put_dup(fds, fd);

                        if (fd < 0)
                                return fd;
                }
        }

        SET_FOREACH(c, m->bus_connections, i) {
                int fd;

                if (dbus_connection_get_unix_fd(c, &fd)) {
                        fd = fdset_put_dup(fds, fd);

                        if (fd < 0)
                                return fd;
                }
        }

        return 0;
}

void bus_broadcast_finished(
                Manager *m,
                usec_t kernel_usec,
                usec_t initrd_usec,
                usec_t userspace_usec,
                usec_t total_usec) {

        DBusMessage *message;

        assert(m);

        message = dbus_message_new_signal("/org/freedesktop/systemd1", "org.freedesktop.systemd1.Manager", "StartupFinished");
        if (!message) {
                log_error("Out of memory.");
                return;
        }

        assert_cc(sizeof(usec_t) == sizeof(uint64_t));
        if (!dbus_message_append_args(message,
                                      DBUS_TYPE_UINT64, &kernel_usec,
                                      DBUS_TYPE_UINT64, &initrd_usec,
                                      DBUS_TYPE_UINT64, &userspace_usec,
                                      DBUS_TYPE_UINT64, &total_usec,
                                      DBUS_TYPE_INVALID)) {
                log_error("Out of memory.");
                goto finish;
        }


        if (bus_broadcast(m, message) < 0) {
                log_error("Out of memory.");
                goto finish;
        }

finish:
        if (message)
                dbus_message_unref(message);
}
