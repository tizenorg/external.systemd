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
#include "dbus-unit.h"
#include "bus-errors.h"
#include "dbus-common.h"

const char bus_unit_interface[] _introspect_("Unit") = BUS_UNIT_INTERFACE;

#define INVALIDATING_PROPERTIES                 \
        "LoadState\0"                           \
        "ActiveState\0"                         \
        "SubState\0"                            \
        "InactiveExitTimestamp\0"               \
        "ActiveEnterTimestamp\0"                \
        "ActiveExitTimestamp\0"                 \
        "InactiveEnterTimestamp\0"              \
        "Job\0"                                 \
        "NeedDaemonReload\0"

int bus_unit_append_names(DBusMessageIter *i, const char *property, void *data) {
        char *t;
        Iterator j;
        DBusMessageIter sub;
        Unit *u = data;

        if (!dbus_message_iter_open_container(i, DBUS_TYPE_ARRAY, "s", &sub))
                return -ENOMEM;

        SET_FOREACH(t, u->meta.names, j)
                if (!dbus_message_iter_append_basic(&sub, DBUS_TYPE_STRING, &t))
                        return -ENOMEM;

        if (!dbus_message_iter_close_container(i, &sub))
                return -ENOMEM;

        return 0;
}

int bus_unit_append_following(DBusMessageIter *i, const char *property, void *data) {
        Unit *u = data, *f;
        const char *d;

        assert(i);
        assert(property);
        assert(u);

        f = unit_following(u);
        d = f ? f->meta.id : "";

        if (!dbus_message_iter_append_basic(i, DBUS_TYPE_STRING, &d))
                return -ENOMEM;

        return 0;
}

int bus_unit_append_dependencies(DBusMessageIter *i, const char *property, void *data) {
        Unit *u;
        Iterator j;
        DBusMessageIter sub;
        Set *s = data;

        if (!dbus_message_iter_open_container(i, DBUS_TYPE_ARRAY, "s", &sub))
                return -ENOMEM;

        SET_FOREACH(u, s, j)
                if (!dbus_message_iter_append_basic(&sub, DBUS_TYPE_STRING, &u->meta.id))
                        return -ENOMEM;

        if (!dbus_message_iter_close_container(i, &sub))
                return -ENOMEM;

        return 0;
}

int bus_unit_append_description(DBusMessageIter *i, const char *property, void *data) {
        Unit *u = data;
        const char *d;

        assert(i);
        assert(property);
        assert(u);

        d = unit_description(u);

        if (!dbus_message_iter_append_basic(i, DBUS_TYPE_STRING, &d))
                return -ENOMEM;

        return 0;
}

DEFINE_BUS_PROPERTY_APPEND_ENUM(bus_unit_append_load_state, unit_load_state, UnitLoadState);

int bus_unit_append_active_state(DBusMessageIter *i, const char *property, void *data) {
        Unit *u = data;
        const char *state;

        assert(i);
        assert(property);
        assert(u);

        state = unit_active_state_to_string(unit_active_state(u));

        if (!dbus_message_iter_append_basic(i, DBUS_TYPE_STRING, &state))
                return -ENOMEM;

        return 0;
}

int bus_unit_append_sub_state(DBusMessageIter *i, const char *property, void *data) {
        Unit *u = data;
        const char *state;

        assert(i);
        assert(property);
        assert(u);

        state = unit_sub_state_to_string(u);

        if (!dbus_message_iter_append_basic(i, DBUS_TYPE_STRING, &state))
                return -ENOMEM;

        return 0;
}

int bus_unit_append_file_state(DBusMessageIter *i, const char *property, void *data) {
        Unit *u = data;
        const char *state;

        assert(i);
        assert(property);
        assert(u);

        state = strempty(unit_file_state_to_string(unit_get_unit_file_state(u)));

        if (!dbus_message_iter_append_basic(i, DBUS_TYPE_STRING, &state))
                return -ENOMEM;

        return 0;
}

int bus_unit_append_can_start(DBusMessageIter *i, const char *property, void *data) {
        Unit *u = data;
        dbus_bool_t b;

        assert(i);
        assert(property);
        assert(u);

        b = unit_can_start(u) &&
                !u->meta.refuse_manual_start;

        if (!dbus_message_iter_append_basic(i, DBUS_TYPE_BOOLEAN, &b))
                return -ENOMEM;

        return 0;
}

int bus_unit_append_can_stop(DBusMessageIter *i, const char *property, void *data) {
        Unit *u = data;
        dbus_bool_t b;

        assert(i);
        assert(property);
        assert(u);

        /* On the lower levels we assume that every unit we can start
         * we can also stop */

        b = unit_can_start(u) &&
                !u->meta.refuse_manual_stop;

        if (!dbus_message_iter_append_basic(i, DBUS_TYPE_BOOLEAN, &b))
                return -ENOMEM;

        return 0;
}

int bus_unit_append_can_reload(DBusMessageIter *i, const char *property, void *data) {
        Unit *u = data;
        dbus_bool_t b;

        assert(i);
        assert(property);
        assert(u);

        b = unit_can_reload(u);

        if (!dbus_message_iter_append_basic(i, DBUS_TYPE_BOOLEAN, &b))
                return -ENOMEM;

        return 0;
}

int bus_unit_append_can_isolate(DBusMessageIter *i, const char *property, void *data) {
        Unit *u = data;
        dbus_bool_t b;

        assert(i);
        assert(property);
        assert(u);

        b = unit_can_isolate(u) &&
                !u->meta.refuse_manual_start;

        if (!dbus_message_iter_append_basic(i, DBUS_TYPE_BOOLEAN, &b))
                return -ENOMEM;

        return 0;
}

int bus_unit_append_job(DBusMessageIter *i, const char *property, void *data) {
        Unit *u = data;
        DBusMessageIter sub;
        char *p;

        assert(i);
        assert(property);
        assert(u);

        if (!dbus_message_iter_open_container(i, DBUS_TYPE_STRUCT, NULL, &sub))
                return -ENOMEM;

        if (u->meta.job) {

                if (!(p = job_dbus_path(u->meta.job)))
                        return -ENOMEM;

                if (!dbus_message_iter_append_basic(&sub, DBUS_TYPE_UINT32, &u->meta.job->id) ||
                    !dbus_message_iter_append_basic(&sub, DBUS_TYPE_OBJECT_PATH, &p)) {
                        free(p);
                        return -ENOMEM;
                }
        } else {
                uint32_t id = 0;

                /* No job, so let's fill in some placeholder
                 * data. Since we need to fill in a valid path we
                 * simple point to ourselves. */

                if (!(p = unit_dbus_path(u)))
                        return -ENOMEM;

                if (!dbus_message_iter_append_basic(&sub, DBUS_TYPE_UINT32, &id) ||
                    !dbus_message_iter_append_basic(&sub, DBUS_TYPE_OBJECT_PATH, &p)) {
                        free(p);
                        return -ENOMEM;
                }
        }

        free(p);

        if (!dbus_message_iter_close_container(i, &sub))
                return -ENOMEM;

        return 0;
}

int bus_unit_append_default_cgroup(DBusMessageIter *i, const char *property, void *data) {
        Unit *u = data;
        char *t;
        CGroupBonding *cgb;
        bool success;

        assert(i);
        assert(property);
        assert(u);

        if ((cgb = unit_get_default_cgroup(u))) {
                if (!(t = cgroup_bonding_to_string(cgb)))
                        return -ENOMEM;
        } else
                t = (char*) "";

        success = dbus_message_iter_append_basic(i, DBUS_TYPE_STRING, &t);

        if (cgb)
                free(t);

        return success ? 0 : -ENOMEM;
}

int bus_unit_append_cgroups(DBusMessageIter *i, const char *property, void *data) {
        Unit *u = data;
        CGroupBonding *cgb;
        DBusMessageIter sub;

        if (!dbus_message_iter_open_container(i, DBUS_TYPE_ARRAY, "s", &sub))
                return -ENOMEM;

        LIST_FOREACH(by_unit, cgb, u->meta.cgroup_bondings) {
                char *t;
                bool success;

                if (!(t = cgroup_bonding_to_string(cgb)))
                        return -ENOMEM;

                success = dbus_message_iter_append_basic(&sub, DBUS_TYPE_STRING, &t);
                free(t);

                if (!success)
                        return -ENOMEM;
        }

        if (!dbus_message_iter_close_container(i, &sub))
                return -ENOMEM;

        return 0;
}

int bus_unit_append_cgroup_attrs(DBusMessageIter *i, const char *property, void *data) {
        Unit *u = data;
        CGroupAttribute *a;
        DBusMessageIter sub, sub2;

        if (!dbus_message_iter_open_container(i, DBUS_TYPE_ARRAY, "(sss)", &sub))
                return -ENOMEM;

        LIST_FOREACH(by_unit, a, u->meta.cgroup_attributes) {
                char *v = NULL;
                bool success;

                if (a->map_callback)
                        a->map_callback(a->controller, a->name, a->value, &v);

                success =
                        dbus_message_iter_open_container(&sub, DBUS_TYPE_STRUCT, NULL, &sub2) &&
                        dbus_message_iter_append_basic(&sub2, DBUS_TYPE_STRING, &a->controller) &&
                        dbus_message_iter_append_basic(&sub2, DBUS_TYPE_STRING, &a->name) &&
                        dbus_message_iter_append_basic(&sub2, DBUS_TYPE_STRING, v ? &v : &a->value) &&
                        dbus_message_iter_close_container(&sub, &sub2);

                free(v);

                if (!success)
                        return -ENOMEM;
        }

        if (!dbus_message_iter_close_container(i, &sub))
                return -ENOMEM;

        return 0;
}

int bus_unit_append_need_daemon_reload(DBusMessageIter *i, const char *property, void *data) {
        Unit *u = data;
        dbus_bool_t b;

        assert(i);
        assert(property);
        assert(u);

        b = unit_need_daemon_reload(u);

        if (!dbus_message_iter_append_basic(i, DBUS_TYPE_BOOLEAN, &b))
                return -ENOMEM;

        return 0;
}

int bus_unit_append_load_error(DBusMessageIter *i, const char *property, void *data) {
        Unit *u = data;
        const char *name, *message;
        DBusMessageIter sub;

        assert(i);
        assert(property);
        assert(u);

        if (u->meta.load_error != 0) {
                name = bus_errno_to_dbus(u->meta.load_error);
                message = strempty(strerror(-u->meta.load_error));
        } else
                name = message = "";

        if (!dbus_message_iter_open_container(i, DBUS_TYPE_STRUCT, NULL, &sub) ||
            !dbus_message_iter_append_basic(&sub, DBUS_TYPE_STRING, &name) ||
            !dbus_message_iter_append_basic(&sub, DBUS_TYPE_STRING, &message) ||
            !dbus_message_iter_close_container(i, &sub))
                return -ENOMEM;

        return 0;
}

static DBusHandlerResult bus_unit_message_dispatch(Unit *u, DBusConnection *connection, DBusMessage *message) {
        DBusMessage *reply = NULL;
        Manager *m = u->meta.manager;
        DBusError error;
        JobType job_type = _JOB_TYPE_INVALID;
        char *path = NULL;
        bool reload_if_possible = false;

        dbus_error_init(&error);

        if (dbus_message_is_method_call(message, "org.freedesktop.systemd1.Unit", "Start"))
                job_type = JOB_START;
        else if (dbus_message_is_method_call(message, "org.freedesktop.systemd1.Unit", "Stop"))
                job_type = JOB_STOP;
        else if (dbus_message_is_method_call(message, "org.freedesktop.systemd1.Unit", "Reload"))
                job_type = JOB_RELOAD;
        else if (dbus_message_is_method_call(message, "org.freedesktop.systemd1.Unit", "Restart"))
                job_type = JOB_RESTART;
        else if (dbus_message_is_method_call(message, "org.freedesktop.systemd1.Unit", "TryRestart"))
                job_type = JOB_TRY_RESTART;
        else if (dbus_message_is_method_call(message, "org.freedesktop.systemd1.Unit", "ReloadOrRestart")) {
                reload_if_possible = true;
                job_type = JOB_RESTART;
        } else if (dbus_message_is_method_call(message, "org.freedesktop.systemd1.Unit", "ReloadOrTryRestart")) {
                reload_if_possible = true;
                job_type = JOB_TRY_RESTART;
        } else if (dbus_message_is_method_call(message, "org.freedesktop.systemd1.Unit", "Kill")) {
                const char *swho, *smode;
                int32_t signo;
                KillMode mode;
                KillWho who;
                int r;

                if (!dbus_message_get_args(
                                    message,
                                    &error,
                                    DBUS_TYPE_STRING, &swho,
                                    DBUS_TYPE_STRING, &smode,
                                    DBUS_TYPE_INT32, &signo,
                                    DBUS_TYPE_INVALID))
                        return bus_send_error_reply(connection, message, &error, -EINVAL);

                if (isempty(swho))
                        who = KILL_ALL;
                else {
                        who = kill_who_from_string(swho);
                        if (who < 0)
                                return bus_send_error_reply(connection, message, &error, -EINVAL);
                }

                if (isempty(smode))
                        mode = KILL_CONTROL_GROUP;
                else {
                        mode = kill_mode_from_string(smode);
                        if (mode < 0)
                                return bus_send_error_reply(connection, message, &error, -EINVAL);
                }

                if (signo <= 0 || signo >= _NSIG)
                        return bus_send_error_reply(connection, message, &error, -EINVAL);

                if ((r = unit_kill(u, who, mode, signo, &error)) < 0)
                        return bus_send_error_reply(connection, message, &error, r);

                if (!(reply = dbus_message_new_method_return(message)))
                        goto oom;

        } else if (dbus_message_is_method_call(message, "org.freedesktop.systemd1.Unit", "ResetFailed")) {

                unit_reset_failed(u);

                if (!(reply = dbus_message_new_method_return(message)))
                        goto oom;

        } else if (UNIT_VTABLE(u)->bus_message_handler)
                return UNIT_VTABLE(u)->bus_message_handler(u, connection, message);
        else
                return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

        if (job_type != _JOB_TYPE_INVALID) {
                const char *smode;
                JobMode mode;
                Job *j;
                int r;

                if ((job_type == JOB_START && u->meta.refuse_manual_start) ||
                    (job_type == JOB_STOP && u->meta.refuse_manual_stop) ||
                    ((job_type == JOB_RESTART || job_type == JOB_TRY_RESTART) &&
                     (u->meta.refuse_manual_start || u->meta.refuse_manual_stop))) {
                        dbus_set_error(&error, BUS_ERROR_ONLY_BY_DEPENDENCY, "Operation refused, may be requested by dependency only.");
                        return bus_send_error_reply(connection, message, &error, -EPERM);
                }

                if (!dbus_message_get_args(
                                    message,
                                    &error,
                                    DBUS_TYPE_STRING, &smode,
                                    DBUS_TYPE_INVALID))
                        return bus_send_error_reply(connection, message, &error, -EINVAL);

                if (reload_if_possible && unit_can_reload(u)) {
                        if (job_type == JOB_RESTART)
                                job_type = JOB_RELOAD_OR_START;
                        else if (job_type == JOB_TRY_RESTART)
                                job_type = JOB_RELOAD;
                }

                if ((mode = job_mode_from_string(smode)) == _JOB_MODE_INVALID) {
                        dbus_set_error(&error, BUS_ERROR_INVALID_JOB_MODE, "Job mode %s is invalid.", smode);
                        return bus_send_error_reply(connection, message, &error, -EINVAL);
                }

                if ((r = manager_add_job(m, job_type, u, mode, true, &error, &j)) < 0)
                        return bus_send_error_reply(connection, message, &error, r);

                if (!(reply = dbus_message_new_method_return(message)))
                        goto oom;

                if (!(path = job_dbus_path(j)))
                        goto oom;

                if (!dbus_message_append_args(
                                    reply,
                                    DBUS_TYPE_OBJECT_PATH, &path,
                                    DBUS_TYPE_INVALID))
                        goto oom;
        }

        if (reply) {
                if (!dbus_connection_send(connection, reply, NULL))
                        goto oom;

                dbus_message_unref(reply);
        }

        free(path);

        return DBUS_HANDLER_RESULT_HANDLED;

oom:
        free(path);

        if (reply)
                dbus_message_unref(reply);

        dbus_error_free(&error);

        return DBUS_HANDLER_RESULT_NEED_MEMORY;
}

static DBusHandlerResult bus_unit_message_handler(DBusConnection *connection, DBusMessage  *message, void *data) {
        Manager *m = data;
        Unit *u;
        int r;
        DBusMessage *reply;

        assert(connection);
        assert(message);
        assert(m);

        if (streq(dbus_message_get_path(message), "/org/freedesktop/systemd1/unit")) {
                /* Be nice to gdbus and return introspection data for our mid-level paths */

                if (dbus_message_is_method_call(message, "org.freedesktop.DBus.Introspectable", "Introspect")) {
                        char *introspection = NULL;
                        FILE *f;
                        Iterator i;
                        const char *k;
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

                        HASHMAP_FOREACH_KEY(u, k, m->units, i) {
                                char *p;

                                if (k != u->meta.id)
                                        continue;

                                if (!(p = bus_path_escape(k))) {
                                        fclose(f);
                                        free(introspection);
                                        goto oom;
                                }

                                fprintf(f, "<node name=\"%s\"/>", p);
                                free(p);
                        }

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

        if ((r = manager_get_unit_from_dbus_path(m, dbus_message_get_path(message), &u)) < 0) {

                if (r == -ENOMEM)
                        return DBUS_HANDLER_RESULT_NEED_MEMORY;

                if (r == -ENOENT) {
                        DBusError e;

                        dbus_error_init(&e);
                        dbus_set_error_const(&e, DBUS_ERROR_UNKNOWN_OBJECT, "Unknown unit");
                        return bus_send_error_reply(connection, message, &e, r);
                }

                return bus_send_error_reply(connection, message, NULL, r);
        }

        return bus_unit_message_dispatch(u, connection, message);

oom:
        if (reply)
                dbus_message_unref(reply);

        return DBUS_HANDLER_RESULT_NEED_MEMORY;
}

const DBusObjectPathVTable bus_unit_vtable = {
        .message_function = bus_unit_message_handler
};

void bus_unit_send_change_signal(Unit *u) {
        char *p = NULL;
        DBusMessage *m = NULL;

        assert(u);

        if (u->meta.in_dbus_queue) {
                LIST_REMOVE(Meta, dbus_queue, u->meta.manager->dbus_unit_queue, &u->meta);
                u->meta.in_dbus_queue = false;
        }

        if (!u->meta.id)
                return;

        if (!bus_has_subscriber(u->meta.manager)) {
                u->meta.sent_dbus_new_signal = true;
                return;
        }

        if (!(p = unit_dbus_path(u)))
                goto oom;

        if (u->meta.sent_dbus_new_signal) {
                /* Send a properties changed signal. First for the
                 * specific type, then for the generic unit. The
                 * clients may rely on this order to get atomic
                 * behaviour if needed. */

                if (UNIT_VTABLE(u)->bus_invalidating_properties) {

                        if (!(m = bus_properties_changed_new(p,
                                                             UNIT_VTABLE(u)->bus_interface,
                                                             UNIT_VTABLE(u)->bus_invalidating_properties)))
                                goto oom;

                        if (bus_broadcast(u->meta.manager, m) < 0)
                                goto oom;

                        dbus_message_unref(m);
                }

                if (!(m = bus_properties_changed_new(p, "org.freedesktop.systemd1.Unit", INVALIDATING_PROPERTIES)))
                        goto oom;

        } else {
                /* Send a new signal */

                if (!(m = dbus_message_new_signal("/org/freedesktop/systemd1", "org.freedesktop.systemd1.Manager", "UnitNew")))
                        goto oom;

                if (!dbus_message_append_args(m,
                                              DBUS_TYPE_STRING, &u->meta.id,
                                              DBUS_TYPE_OBJECT_PATH, &p,
                                              DBUS_TYPE_INVALID))
                        goto oom;
        }

        if (bus_broadcast(u->meta.manager, m) < 0)
                goto oom;

        free(p);
        dbus_message_unref(m);

        u->meta.sent_dbus_new_signal = true;

        return;

oom:
        free(p);

        if (m)
                dbus_message_unref(m);

        log_error("Failed to allocate unit change/new signal.");
}

void bus_unit_send_removed_signal(Unit *u) {
        char *p = NULL;
        DBusMessage *m = NULL;

        assert(u);

        if (!bus_has_subscriber(u->meta.manager))
                return;

        if (!u->meta.sent_dbus_new_signal)
                bus_unit_send_change_signal(u);

        if (!u->meta.id)
                return;

        if (!(p = unit_dbus_path(u)))
                goto oom;

        if (!(m = dbus_message_new_signal("/org/freedesktop/systemd1", "org.freedesktop.systemd1.Manager", "UnitRemoved")))
                goto oom;

        if (!dbus_message_append_args(m,
                                      DBUS_TYPE_STRING, &u->meta.id,
                                      DBUS_TYPE_OBJECT_PATH, &p,
                                      DBUS_TYPE_INVALID))
                goto oom;

        if (bus_broadcast(u->meta.manager, m) < 0)
                goto oom;

        free(p);
        dbus_message_unref(m);

        return;

oom:
        free(p);

        if (m)
                dbus_message_unref(m);

        log_error("Failed to allocate unit remove signal.");
}
