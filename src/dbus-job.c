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

#include <errno.h>

#include "dbus.h"
#include "log.h"
#include "dbus-job.h"
#include "dbus-common.h"

#define BUS_JOB_INTERFACE                                             \
        " <interface name=\"org.freedesktop.systemd1.Job\">\n"        \
        "  <method name=\"Cancel\"/>\n"                               \
        "  <property name=\"Id\" type=\"u\" access=\"read\"/>\n"      \
        "  <property name=\"Unit\" type=\"(so)\" access=\"read\"/>\n" \
        "  <property name=\"JobType\" type=\"s\" access=\"read\"/>\n" \
        "  <property name=\"State\" type=\"s\" access=\"read\"/>\n"   \
        " </interface>\n"

#define INTROSPECTION                                                 \
        DBUS_INTROSPECT_1_0_XML_DOCTYPE_DECL_NODE                     \
        "<node>\n"                                                    \
        BUS_JOB_INTERFACE                                             \
        BUS_PROPERTIES_INTERFACE                                      \
        BUS_PEER_INTERFACE                                            \
        BUS_INTROSPECTABLE_INTERFACE                                  \
        "</node>\n"

const char bus_job_interface[] _introspect_("Job") = BUS_JOB_INTERFACE;

#define INTERFACES_LIST                              \
        BUS_GENERIC_INTERFACES_LIST                  \
        "org.freedesktop.systemd1.Job\0"

#define INVALIDATING_PROPERTIES                 \
        "State\0"

static DEFINE_BUS_PROPERTY_APPEND_ENUM(bus_job_append_state, job_state, JobState);
static DEFINE_BUS_PROPERTY_APPEND_ENUM(bus_job_append_type, job_type, JobType);

static int bus_job_append_unit(DBusMessageIter *i, const char *property, void *data) {
        Job *j = data;
        DBusMessageIter sub;
        char *p;

        assert(i);
        assert(property);
        assert(j);

        if (!dbus_message_iter_open_container(i, DBUS_TYPE_STRUCT, NULL, &sub))
                return -ENOMEM;

        if (!(p = unit_dbus_path(j->unit)))
                return -ENOMEM;

        if (!dbus_message_iter_append_basic(&sub, DBUS_TYPE_STRING, &j->unit->id) ||
            !dbus_message_iter_append_basic(&sub, DBUS_TYPE_OBJECT_PATH, &p)) {
                free(p);
                return -ENOMEM;
        }

        free(p);

        if (!dbus_message_iter_close_container(i, &sub))
                return -ENOMEM;

        return 0;
}

static const BusProperty bus_job_properties[] = {
        { "Id",      bus_property_append_uint32, "u", offsetof(Job, id)    },
        { "State",   bus_job_append_state,       "s", offsetof(Job, state) },
        { "JobType", bus_job_append_type,        "s", offsetof(Job, type)  },
        { "Unit",    bus_job_append_unit,     "(so)", 0 },
        { NULL, }
};

static DBusHandlerResult bus_job_message_dispatch(Job *j, DBusConnection *connection, DBusMessage *message) {
        DBusMessage *reply = NULL;

        if (dbus_message_is_method_call(message, "org.freedesktop.systemd1.Job", "Cancel")) {
                if (!(reply = dbus_message_new_method_return(message)))
                        goto oom;

                job_finish_and_invalidate(j, JOB_CANCELED);

        } else {
                const BusBoundProperties bps[] = {
                        { "org.freedesktop.systemd1.Job", bus_job_properties, j },
                        { NULL, }
                };
                return bus_default_message_handler(connection, message, INTROSPECTION, INTERFACES_LIST, bps);
        }

        if (reply) {
                if (!dbus_connection_send(connection, reply, NULL))
                        goto oom;

                dbus_message_unref(reply);
        }

        return DBUS_HANDLER_RESULT_HANDLED;

oom:
        if (reply)
                dbus_message_unref(reply);

        return DBUS_HANDLER_RESULT_NEED_MEMORY;
}

static DBusHandlerResult bus_job_message_handler(DBusConnection *connection, DBusMessage  *message, void *data) {
        Manager *m = data;
        Job *j;
        int r;
        DBusMessage *reply;

        assert(connection);
        assert(message);
        assert(m);

        if (streq(dbus_message_get_path(message), "/org/freedesktop/systemd1/job")) {
                /* Be nice to gdbus and return introspection data for our mid-level paths */

                if (dbus_message_is_method_call(message, "org.freedesktop.DBus.Introspectable", "Introspect")) {
                        char *introspection = NULL;
                        FILE *f;
                        Iterator i;
                        size_t size;

                        if (!(reply = dbus_message_new_method_return(message)))
                                goto oom;

                        /* We roll our own introspection code here, instead of
                         * relying on bus_default_message_handler() because we
                         * need to generate our introspection string
                         * dynamically. */

                        if (!(f = open_memstream(&introspection, &size)))
                                goto oom;

                        fputs(DBUS_INTROSPECT_1_0_XML_DOCTYPE_DECL_NODE
                              "<node>\n", f);

                        fputs(BUS_INTROSPECTABLE_INTERFACE, f);
                        fputs(BUS_PEER_INTERFACE, f);

                        HASHMAP_FOREACH(j, m->jobs, i)
                                fprintf(f, "<node name=\"job/%lu\"/>", (unsigned long) j->id);

                        fputs("</node>\n", f);

                        if (ferror(f)) {
                                fclose(f);
                                free(introspection);
                                goto oom;
                        }

                        fclose(f);

                        if (!introspection)
                                goto oom;

                        if (!dbus_message_append_args(reply, DBUS_TYPE_STRING, &introspection, DBUS_TYPE_INVALID)) {
                                free(introspection);
                                goto oom;
                        }

                        free(introspection);

                        if (!dbus_connection_send(connection, reply, NULL))
                                goto oom;

                        dbus_message_unref(reply);

                        return DBUS_HANDLER_RESULT_HANDLED;
                }

                return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
        }

        if ((r = manager_get_job_from_dbus_path(m, dbus_message_get_path(message), &j)) < 0) {

                if (r == -ENOMEM)
                        return DBUS_HANDLER_RESULT_NEED_MEMORY;

                if (r == -ENOENT) {
                        DBusError e;

                        dbus_error_init(&e);
                        dbus_set_error_const(&e, DBUS_ERROR_UNKNOWN_OBJECT, "Unknown job");
                        return bus_send_error_reply(connection, message, &e, r);
                }

                return bus_send_error_reply(connection, message, NULL, r);
        }

        return bus_job_message_dispatch(j, connection, message);

oom:
        if (reply)
                dbus_message_unref(reply);

        return DBUS_HANDLER_RESULT_NEED_MEMORY;
}

const DBusObjectPathVTable bus_job_vtable = {
        .message_function = bus_job_message_handler
};

static int job_send_message(Job *j, DBusMessage *m) {
        int r;

        assert(j);
        assert(m);

        if (bus_has_subscriber(j->manager)) {
                if ((r = bus_broadcast(j->manager, m)) < 0)
                        return r;

        } else  if (j->bus_client) {
                /* If nobody is subscribed, we just send the message
                 * to the client which created the job */

                assert(j->bus);

                if (!dbus_message_set_destination(m, j->bus_client))
                        return -ENOMEM;

                if (!dbus_connection_send(j->bus, m, NULL))
                        return -ENOMEM;
        }

        return 0;
}

void bus_job_send_change_signal(Job *j) {
        char *p = NULL;
        DBusMessage *m = NULL;

        assert(j);

        if (j->in_dbus_queue) {
                LIST_REMOVE(Job, dbus_queue, j->manager->dbus_job_queue, j);
                j->in_dbus_queue = false;
        }

        if (!bus_has_subscriber(j->manager) && !j->bus_client) {
                j->sent_dbus_new_signal = true;
                return;
        }

        if (!(p = job_dbus_path(j)))
                goto oom;

        if (j->sent_dbus_new_signal) {
                /* Send a properties changed signal */

                if (!(m = bus_properties_changed_new(p, "org.freedesktop.systemd1.Job", INVALIDATING_PROPERTIES)))
                        goto oom;

        } else {
                /* Send a new signal */

                if (!(m = dbus_message_new_signal("/org/freedesktop/systemd1", "org.freedesktop.systemd1.Manager", "JobNew")))
                        goto oom;

                if (!dbus_message_append_args(m,
                                              DBUS_TYPE_UINT32, &j->id,
                                              DBUS_TYPE_OBJECT_PATH, &p,
                                              DBUS_TYPE_INVALID))
                        goto oom;
        }

        if (job_send_message(j, m) < 0)
                goto oom;

        free(p);
        dbus_message_unref(m);

        j->sent_dbus_new_signal = true;

        return;

oom:
        free(p);

        if (m)
                dbus_message_unref(m);

        log_error("Failed to allocate job change signal.");
}

void bus_job_send_removed_signal(Job *j) {
        char *p = NULL;
        DBusMessage *m = NULL;
        const char *r;

        assert(j);

        if (!bus_has_subscriber(j->manager) && !j->bus_client)
                return;

        if (!j->sent_dbus_new_signal)
                bus_job_send_change_signal(j);

        if (!(p = job_dbus_path(j)))
                goto oom;

        if (!(m = dbus_message_new_signal("/org/freedesktop/systemd1", "org.freedesktop.systemd1.Manager", "JobRemoved")))
                goto oom;

        r = job_result_to_string(j->result);

        if (!dbus_message_append_args(m,
                                      DBUS_TYPE_UINT32, &j->id,
                                      DBUS_TYPE_OBJECT_PATH, &p,
                                      DBUS_TYPE_STRING, &r,
                                      DBUS_TYPE_INVALID))
                goto oom;

        if (job_send_message(j, m) < 0)
                goto oom;

        free(p);
        dbus_message_unref(m);

        return;

oom:
        free(p);

        if (m)
                dbus_message_unref(m);

        log_error("Failed to allocate job remove signal.");
}
