/*-*- Mode: C; c-basic-offset: 8; indent-tabs-mode: nil -*-*/

/***
  This file is part of systemd.

  Copyright 2011 Lennart Poettering

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
#include <string.h>
#include <unistd.h>
#include <pwd.h>

#include "logind.h"
#include "dbus-common.h"
#include "strv.h"
#include "polkit.h"
#include "special.h"

#define BUS_MANAGER_INTERFACE                                           \
        " <interface name=\"org.freedesktop.login1.Manager\">\n"        \
        "  <method name=\"GetSession\">\n"                              \
        "   <arg name=\"id\" type=\"s\" direction=\"in\"/>\n"           \
        "   <arg name=\"session\" type=\"o\" direction=\"out\"/>\n"     \
        "  </method>\n"                                                 \
        "  <method name=\"GetSessionByPID\">\n"                         \
        "   <arg name=\"pid\" type=\"u\" direction=\"in\"/>\n"          \
        "   <arg name=\"session\" type=\"o\" direction=\"out\"/>\n"     \
        "  </method>\n"                                                 \
        "  <method name=\"GetUser\">\n"                                 \
        "   <arg name=\"uid\" type=\"u\" direction=\"in\"/>\n"          \
        "   <arg name=\"user\" type=\"o\" direction=\"out\"/>\n"        \
        "  </method>\n"                                                 \
        "  <method name=\"GetSeat\">\n"                                 \
        "   <arg name=\"id\" type=\"s\" direction=\"in\"/>\n"           \
        "   <arg name=\"seat\" type=\"o\" direction=\"out\"/>\n"        \
        "  </method>\n"                                                 \
        "  <method name=\"ListSessions\">\n"                            \
        "   <arg name=\"sessions\" type=\"a(susso)\" direction=\"out\"/>\n" \
        "  </method>\n"                                                 \
        "  <method name=\"ListUsers\">\n"                               \
        "   <arg name=\"users\" type=\"a(uso)\" direction=\"out\"/>\n"  \
        "  </method>\n"                                                 \
        "  <method name=\"ListSeats\">\n"                               \
        "   <arg name=\"seats\" type=\"a(so)\" direction=\"out\"/>\n"   \
        "  </method>\n"                                                 \
        "  <method name=\"CreateSession\">\n"                           \
        "   <arg name=\"uid\" type=\"u\" direction=\"in\"/>\n"          \
        "   <arg name=\"leader\" type=\"u\" direction=\"in\"/>\n"       \
        "   <arg name=\"sevice\" type=\"s\" direction=\"in\"/>\n"       \
        "   <arg name=\"type\" type=\"s\" direction=\"in\"/>\n"         \
        "   <arg name=\"class\" type=\"s\" direction=\"in\"/>\n"        \
        "   <arg name=\"seat\" type=\"s\" direction=\"in\"/>\n"         \
        "   <arg name=\"vtnr\" type=\"u\" direction=\"in\"/>\n"         \
        "   <arg name=\"tty\" type=\"s\" direction=\"in\"/>\n"          \
        "   <arg name=\"display\" type=\"s\" direction=\"in\"/>\n"      \
        "   <arg name=\"remote\" type=\"b\" direction=\"in\"/>\n"       \
        "   <arg name=\"remote_user\" type=\"s\" direction=\"in\"/>\n"  \
        "   <arg name=\"remote_host\" type=\"s\" direction=\"in\"/>\n"  \
        "   <arg name=\"controllers\" type=\"as\" direction=\"in\"/>\n" \
        "   <arg name=\"reset_controllers\" type=\"as\" direction=\"in\"/>\n" \
        "   <arg name=\"kill_processes\" type=\"b\" direction=\"in\"/>\n" \
        "   <arg name=\"id\" type=\"s\" direction=\"out\"/>\n"          \
        "   <arg name=\"path\" type=\"o\" direction=\"out\"/>\n"        \
        "   <arg name=\"runtime_path\" type=\"o\" direction=\"out\"/>\n" \
        "   <arg name=\"fd\" type=\"h\" direction=\"out\"/>\n"          \
        "   <arg name=\"seat\" type=\"s\" direction=\"out\"/>\n"        \
        "   <arg name=\"vtnr\" type=\"u\" direction=\"out\"/>\n"        \
        "  </method>\n"                                                 \
        "  <method name=\"ActivateSession\">\n"                         \
        "   <arg name=\"id\" type=\"s\" direction=\"in\"/>\n"           \
        "  </method>\n"                                                 \
        "  <method name=\"ActivateSessionOnSeat\">\n"                   \
        "   <arg name=\"id\" type=\"s\" direction=\"in\"/>\n"           \
        "   <arg name=\"seat\" type=\"s\" direction=\"in\"/>\n"         \
        "  </method>\n"                                                 \
        "  <method name=\"LockSession\">\n"                             \
        "   <arg name=\"id\" type=\"s\" direction=\"in\"/>\n"           \
        "  </method>\n"                                                 \
        "  <method name=\"UnlockSession\">\n"                           \
        "   <arg name=\"id\" type=\"s\" direction=\"in\"/>\n"           \
        "  </method>\n"                                                 \
        "  <method name=\"KillSession\">\n"                             \
        "   <arg name=\"id\" type=\"s\" direction=\"in\"/>\n"           \
        "   <arg name=\"who\" type=\"s\" direction=\"in\"/>\n"          \
        "   <arg name=\"signal\" type=\"s\" direction=\"in\"/>\n"       \
        "  </method>\n"                                                 \
        "  <method name=\"KillUser\">\n"                                \
        "   <arg name=\"uid\" type=\"u\" direction=\"in\"/>\n"          \
        "   <arg name=\"signal\" type=\"s\" direction=\"in\"/>\n"       \
        "  </method>\n"                                                 \
        "  <method name=\"TerminateSession\">\n"                        \
        "   <arg name=\"id\" type=\"s\" direction=\"in\"/>\n"           \
        "  </method>\n"                                                 \
        "  <method name=\"TerminateUser\">\n"                           \
        "   <arg name=\"uid\" type=\"u\" direction=\"in\"/>\n"          \
        "  </method>\n"                                                 \
        "  <method name=\"TerminateSeat\">\n"                           \
        "   <arg name=\"id\" type=\"s\" direction=\"in\"/>\n"           \
        "  </method>\n"                                                 \
        "  <method name=\"SetUserLinger\">\n"                           \
        "   <arg name=\"uid\" type=\"u\" direction=\"in\"/>\n"          \
        "   <arg name=\"b\" type=\"b\" direction=\"in\"/>\n"            \
        "   <arg name=\"interactive\" type=\"b\" direction=\"in\"/>\n"  \
        "  </method>\n"                                                 \
        "  <method name=\"AttachDevice\">\n"                            \
        "   <arg name=\"seat\" type=\"s\" direction=\"in\"/>\n"         \
        "   <arg name=\"sysfs\" type=\"s\" direction=\"in\"/>\n"        \
        "   <arg name=\"interactive\" type=\"b\" direction=\"in\"/>\n"  \
        "  </method>\n"                                                 \
        "  <method name=\"FlushDevices\">\n"                            \
        "   <arg name=\"interactive\" type=\"b\" direction=\"in\"/>\n"  \
        "  </method>\n"                                                 \
        "  <method name=\"PowerOff\">\n"                                \
        "   <arg name=\"interactive\" type=\"b\" direction=\"in\"/>\n"  \
        "  </method>\n"                                                 \
        "  <method name=\"Reboot\">\n"                                  \
        "   <arg name=\"interactive\" type=\"b\" direction=\"in\"/>\n"  \
        "  </method>\n"                                                 \
        "  <method name=\"CanPowerOff\">\n"                             \
        "   <arg name=\"result\" type=\"s\" direction=\"out\"/>\n"      \
        "  </method>\n"                                                 \
        "  <method name=\"CanReboot\">\n"                               \
        "   <arg name=\"result\" type=\"s\" direction=\"out\"/>\n"      \
        "  </method>\n"                                                 \
        "  <signal name=\"SessionNew\">\n"                              \
        "   <arg name=\"id\" type=\"s\"/>\n"                            \
        "   <arg name=\"path\" type=\"o\"/>\n"                          \
        "  </signal>\n"                                                 \
        "  <signal name=\"SessionRemoved\">\n"                          \
        "   <arg name=\"id\" type=\"s\"/>\n"                            \
        "   <arg name=\"path\" type=\"o\"/>\n"                          \
        "  </signal>\n"                                                 \
        "  <signal name=\"UserNew\">\n"                                 \
        "   <arg name=\"uid\" type=\"u\"/>\n"                           \
        "   <arg name=\"path\" type=\"o\"/>\n"                          \
        "  </signal>\n"                                                 \
        "  <signal name=\"UserRemoved\">\n"                             \
        "   <arg name=\"uid\" type=\"u\"/>\n"                           \
        "   <arg name=\"path\" type=\"o\"/>\n"                          \
        "  </signal>\n"                                                 \
        "  <signal name=\"SeatNew\">\n"                                 \
        "   <arg name=\"id\" type=\"s\"/>\n"                            \
        "   <arg name=\"path\" type=\"o\"/>\n"                          \
        "  </signal>\n"                                                 \
        "  <signal name=\"SeatRemoved\">\n"                             \
        "   <arg name=\"id\" type=\"s\"/>\n"                            \
        "   <arg name=\"path\" type=\"o\"/>\n"                          \
        "  </signal>\n"                                                 \
        "  <property name=\"ControlGroupHierarchy\" type=\"s\" access=\"read\"/>\n" \
        "  <property name=\"Controllers\" type=\"as\" access=\"read\"/>\n" \
        "  <property name=\"ResetControllers\" type=\"as\" access=\"read\"/>\n" \
        "  <property name=\"NAutoVTs\" type=\"u\" access=\"read\"/>\n" \
        "  <property name=\"KillOnlyUsers\" type=\"as\" access=\"read\"/>\n" \
        "  <property name=\"KillExcludeUsers\" type=\"as\" access=\"read\"/>\n" \
        "  <property name=\"KillUserProcesses\" type=\"b\" access=\"read\"/>\n" \
        "  <property name=\"IdleHint\" type=\"b\" access=\"read\"/>\n"  \
        "  <property name=\"IdleSinceHint\" type=\"t\" access=\"read\"/>\n" \
        "  <property name=\"IdleSinceHintMonotonic\" type=\"t\" access=\"read\"/>\n" \
        " </interface>\n"

#define INTROSPECTION_BEGIN                                             \
        DBUS_INTROSPECT_1_0_XML_DOCTYPE_DECL_NODE                       \
        "<node>\n"                                                      \
        BUS_MANAGER_INTERFACE                                           \
        BUS_PROPERTIES_INTERFACE                                        \
        BUS_PEER_INTERFACE                                              \
        BUS_INTROSPECTABLE_INTERFACE

#define INTROSPECTION_END                                               \
        "</node>\n"

#define INTERFACES_LIST                              \
        BUS_GENERIC_INTERFACES_LIST                  \
        "org.freedesktop.login1.Manager\0"

static int bus_manager_append_idle_hint(DBusMessageIter *i, const char *property, void *data) {
        Manager *m = data;
        dbus_bool_t b;

        assert(i);
        assert(property);
        assert(m);

        b = manager_get_idle_hint(m, NULL) > 0;
        if (!dbus_message_iter_append_basic(i, DBUS_TYPE_BOOLEAN, &b))
                return -ENOMEM;

        return 0;
}

static int bus_manager_append_idle_hint_since(DBusMessageIter *i, const char *property, void *data) {
        Manager *m = data;
        dual_timestamp t;
        uint64_t u;

        assert(i);
        assert(property);
        assert(m);

        manager_get_idle_hint(m, &t);
        u = streq(property, "IdleSinceHint") ? t.realtime : t.monotonic;

        if (!dbus_message_iter_append_basic(i, DBUS_TYPE_UINT64, &u))
                return -ENOMEM;

        return 0;
}

static int bus_manager_create_session(Manager *m, DBusMessage *message, DBusMessage **_reply) {
        Session *session = NULL;
        User *user = NULL;
        const char *type, *class, *seat, *tty, *display, *remote_user, *remote_host, *service;
        uint32_t uid, leader, audit_id = 0;
        dbus_bool_t remote, kill_processes;
        char **controllers = NULL, **reset_controllers = NULL;
        SessionType t;
        SessionClass c;
        Seat *s;
        DBusMessageIter iter;
        int r;
        char *id = NULL, *p;
        uint32_t vtnr = 0;
        int fifo_fd = -1;
        DBusMessage *reply = NULL;
        bool b;

        assert(m);
        assert(message);
        assert(_reply);

        if (!dbus_message_iter_init(message, &iter) ||
            dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_UINT32)
                return -EINVAL;

        dbus_message_iter_get_basic(&iter, &uid);

        if (!dbus_message_iter_next(&iter) ||
            dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_UINT32)
                return -EINVAL;

        dbus_message_iter_get_basic(&iter, &leader);

        if (leader <= 0 ||
            !dbus_message_iter_next(&iter) ||
            dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_STRING)
                return -EINVAL;

        dbus_message_iter_get_basic(&iter, &service);

        if (!dbus_message_iter_next(&iter) ||
            dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_STRING)
                return -EINVAL;

        dbus_message_iter_get_basic(&iter, &type);
        t = session_type_from_string(type);

        if (t < 0 ||
            !dbus_message_iter_next(&iter) ||
            dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_STRING)
                return -EINVAL;

        dbus_message_iter_get_basic(&iter, &class);
        if (isempty(class))
                c = SESSION_USER;
        else
                c = session_class_from_string(class);

        if (c < 0 ||
            !dbus_message_iter_next(&iter) ||
            dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_STRING)
                return -EINVAL;

        dbus_message_iter_get_basic(&iter, &seat);

        if (isempty(seat))
                s = NULL;
        else {
                s = hashmap_get(m->seats, seat);
                if (!s)
                        return -ENOENT;
        }

        if (!dbus_message_iter_next(&iter) ||
            dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_UINT32)
                return -EINVAL;

        dbus_message_iter_get_basic(&iter, &vtnr);

        if (!dbus_message_iter_next(&iter) ||
            dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_STRING)
                return -EINVAL;

        dbus_message_iter_get_basic(&iter, &tty);

        if (tty_is_vc(tty)) {
                int v;

                if (!s)
                        s = m->vtconsole;
                else if (s != m->vtconsole)
                        return -EINVAL;

                v = vtnr_from_tty(tty);

                if (v <= 0)
                        return v < 0 ? v : -EINVAL;

                if (vtnr <= 0)
                        vtnr = (uint32_t) v;
                else if (vtnr != (uint32_t) v)
                        return -EINVAL;

        } else if (!isempty(tty) && s && seat_is_vtconsole(s))
                return -EINVAL;

        if (s) {
                if (seat_can_multi_session(s)) {
                        if (vtnr <= 0 || vtnr > 63)
                                return -EINVAL;
                } else {
                        if (vtnr > 0)
                                return -EINVAL;
                }
        }

        if (!dbus_message_iter_next(&iter) ||
            dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_STRING)
                return -EINVAL;

        dbus_message_iter_get_basic(&iter, &display);

        if (!dbus_message_iter_next(&iter) ||
            dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_BOOLEAN)
                return -EINVAL;

        dbus_message_iter_get_basic(&iter, &remote);

        if (!dbus_message_iter_next(&iter) ||
            dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_STRING)
                return -EINVAL;

        dbus_message_iter_get_basic(&iter, &remote_user);

        if (!dbus_message_iter_next(&iter) ||
            dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_STRING)
                return -EINVAL;

        dbus_message_iter_get_basic(&iter, &remote_host);

        if (!dbus_message_iter_next(&iter) ||
            dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_ARRAY ||
            dbus_message_iter_get_element_type(&iter) != DBUS_TYPE_STRING)
                return -EINVAL;

        r = bus_parse_strv_iter(&iter, &controllers);
        if (r < 0)
                return -EINVAL;

        if (strv_contains(controllers, "systemd") ||
            !dbus_message_iter_next(&iter) ||
            dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_ARRAY ||
            dbus_message_iter_get_element_type(&iter) != DBUS_TYPE_STRING) {
                r = -EINVAL;
                goto fail;
        }

        r = bus_parse_strv_iter(&iter, &reset_controllers);
        if (r < 0)
                goto fail;

        if (strv_contains(reset_controllers, "systemd") ||
            !dbus_message_iter_next(&iter) ||
            dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_BOOLEAN) {
                r = -EINVAL;
                goto fail;
        }

        dbus_message_iter_get_basic(&iter, &kill_processes);

        r = manager_add_user_by_uid(m, uid, &user);
        if (r < 0)
                goto fail;

        audit_session_from_pid(leader, &audit_id);

        if (audit_id > 0) {
                asprintf(&id, "%lu", (unsigned long) audit_id);

                if (!id) {
                        r = -ENOMEM;
                        goto fail;
                }

                session = hashmap_get(m->sessions, id);

                if (session) {
                        free(id);

                        fifo_fd = session_create_fifo(session);
                        if (fifo_fd < 0) {
                                r = fifo_fd;
                                goto fail;
                        }

                        /* Session already exists, client is probably
                         * something like "su" which changes uid but
                         * is still the same audit session */

                        reply = dbus_message_new_method_return(message);
                        if (!reply) {
                                r = -ENOMEM;
                                goto fail;
                        }

                        p = session_bus_path(session);
                        if (!p) {
                                r = -ENOMEM;
                                goto fail;
                        }

                        seat = session->seat ? session->seat->id : "";
                        vtnr = session->vtnr;
                        b = dbus_message_append_args(
                                        reply,
                                        DBUS_TYPE_STRING, &session->id,
                                        DBUS_TYPE_OBJECT_PATH, &p,
                                        DBUS_TYPE_STRING, &session->user->runtime_path,
                                        DBUS_TYPE_UNIX_FD, &fifo_fd,
                                        DBUS_TYPE_STRING, &seat,
                                        DBUS_TYPE_UINT32, &vtnr,
                                        DBUS_TYPE_INVALID);
                        free(p);

                        if (!b) {
                                r = -ENOMEM;
                                goto fail;
                        }

                        close_nointr_nofail(fifo_fd);
                        *_reply = reply;

                        strv_free(controllers);
                        strv_free(reset_controllers);

                        return 0;
                }

        } else {
                do {
                        free(id);
                        asprintf(&id, "c%lu", ++m->session_counter);

                        if (!id) {
                                r = -ENOMEM;
                                goto fail;
                        }

                } while (hashmap_get(m->sessions, id));
        }

        r = manager_add_session(m, user, id, &session);
        free(id);
        if (r < 0)
                goto fail;

        session->leader = leader;
        session->audit_id = audit_id;
        session->type = t;
        session->class = c;
        session->remote = remote;
        session->controllers = controllers;
        session->reset_controllers = reset_controllers;
        session->kill_processes = kill_processes;
        session->vtnr = vtnr;

        controllers = reset_controllers = NULL;

        if (!isempty(tty)) {
                session->tty = strdup(tty);
                if (!session->tty) {
                        r = -ENOMEM;
                        goto fail;
                }
        }

        if (!isempty(display)) {
                session->display = strdup(display);
                if (!session->display) {
                        r = -ENOMEM;
                        goto fail;
                }
        }

        if (!isempty(remote_user)) {
                session->remote_user = strdup(remote_user);
                if (!session->remote_user) {
                        r = -ENOMEM;
                        goto fail;
                }
        }

        if (!isempty(remote_host)) {
                session->remote_host = strdup(remote_host);
                if (!session->remote_host) {
                        r = -ENOMEM;
                        goto fail;
                }
        }

        if (!isempty(service)) {
                session->service = strdup(service);
                if (!session->service) {
                        r = -ENOMEM;
                        goto fail;
                }
        }

        fifo_fd = session_create_fifo(session);
        if (fifo_fd < 0) {
                r = fifo_fd;
                goto fail;
        }

        if (s) {
                r = seat_attach_session(s, session);
                if (r < 0)
                        goto fail;
        }

        r = session_start(session);
        if (r < 0)
                goto fail;

        reply = dbus_message_new_method_return(message);
        if (!reply) {
                r = -ENOMEM;
                goto fail;
        }

        p = session_bus_path(session);
        if (!p) {
                r = -ENOMEM;
                goto fail;
        }

        seat = s ? s->id : "";
        b = dbus_message_append_args(
                        reply,
                        DBUS_TYPE_STRING, &session->id,
                        DBUS_TYPE_OBJECT_PATH, &p,
                        DBUS_TYPE_STRING, &session->user->runtime_path,
                        DBUS_TYPE_UNIX_FD, &fifo_fd,
                        DBUS_TYPE_STRING, &seat,
                        DBUS_TYPE_UINT32, &vtnr,
                        DBUS_TYPE_INVALID);
        free(p);

        if (!b) {
                r = -ENOMEM;
                goto fail;
        }

        close_nointr_nofail(fifo_fd);
        *_reply = reply;

        return 0;

fail:
        strv_free(controllers);
        strv_free(reset_controllers);

        if (session)
                session_add_to_gc_queue(session);

        if (user)
                user_add_to_gc_queue(user);

        if (fifo_fd >= 0)
                close_nointr_nofail(fifo_fd);

        if (reply)
                dbus_message_unref(reply);

        return r;
}

static int trigger_device(Manager *m, struct udev_device *d) {
        struct udev_enumerate *e;
        struct udev_list_entry *first, *item;
        int r;

        assert(m);

        e = udev_enumerate_new(m->udev);
        if (!e) {
                r = -ENOMEM;
                goto finish;
        }

        if (d) {
                if (udev_enumerate_add_match_parent(e, d) < 0) {
                        r = -EIO;
                        goto finish;
                }
        }

        if (udev_enumerate_scan_devices(e) < 0) {
                r = -EIO;
                goto finish;
        }

        first = udev_enumerate_get_list_entry(e);
        udev_list_entry_foreach(item, first) {
                char *t;
                const char *p;

                p = udev_list_entry_get_name(item);

                t = strappend(p, "/uevent");
                if (!t) {
                        r = -ENOMEM;
                        goto finish;
                }

                write_one_line_file(t, "change");
                free(t);
        }

        r = 0;

finish:
        if (e)
                udev_enumerate_unref(e);

        return r;
}

static int attach_device(Manager *m, const char *seat, const char *sysfs) {
        struct udev_device *d;
        char *rule = NULL, *file = NULL;
        const char *id_for_seat;
        int r;

        assert(m);
        assert(seat);
        assert(sysfs);

        d = udev_device_new_from_syspath(m->udev, sysfs);
        if (!d)
                return -ENODEV;

        if (!udev_device_has_tag(d, "seat")) {
                r = -ENODEV;
                goto finish;
        }

        id_for_seat = udev_device_get_property_value(d, "ID_FOR_SEAT");
        if (!id_for_seat) {
                r = -ENODEV;
                goto finish;
        }

        if (asprintf(&file, "/etc/udev/rules.d/72-seat-%s.rules", id_for_seat) < 0) {
                r = -ENOMEM;
                goto finish;
        }

        if (asprintf(&rule, "TAG==\"seat\", ENV{ID_FOR_SEAT}==\"%s\", ENV{ID_SEAT}=\"%s\"", id_for_seat, seat) < 0) {
                r = -ENOMEM;
                goto finish;
        }

        mkdir_p("/etc/udev/rules.d", 0755);
        r = write_one_line_file_atomic(file, rule);
        if (r < 0)
                goto finish;

        r = trigger_device(m, d);

finish:
        free(rule);
        free(file);

        if (d)
                udev_device_unref(d);

        return r;
}

static int flush_devices(Manager *m) {
        DIR *d;

        assert(m);

        d = opendir("/etc/udev/rules.d");
        if (!d) {
                if (errno != ENOENT)
                        log_warning("Failed to open /etc/udev/rules.d: %m");
        } else {
                struct dirent *de;

                while ((de = readdir(d))) {

                        if (!dirent_is_file(de))
                                continue;

                        if (!startswith(de->d_name, "72-seat-"))
                                continue;

                        if (!endswith(de->d_name, ".rules"))
                                continue;

                        if (unlinkat(dirfd(d), de->d_name, 0) < 0)
                                log_warning("Failed to unlink %s: %m", de->d_name);
                }

                closedir(d);
        }

        return trigger_device(m, NULL);
}

static int have_multiple_sessions(
                DBusConnection *connection,
                Manager *m,
                DBusMessage *message,
                DBusError *error) {

        Session *s;

        assert(m);

        if (hashmap_size(m->sessions) > 1)
                return true;

        /* Hmm, there's only one session, but let's make sure it
         * actually belongs to the user who is asking. If not, better
         * be safe than sorry. */

        s = hashmap_first(m->sessions);
        if (s) {
                unsigned long ul;

                ul = dbus_bus_get_unix_user(connection, dbus_message_get_sender(message), error);
                if (ul == (unsigned long) -1)
                        return -EIO;

                return s->user->uid != ul;
        }

        return false;
}

static const BusProperty bus_login_manager_properties[] = {
        { "ControlGroupHierarchy",  bus_property_append_string,         "s",  offsetof(Manager, cgroup_path),        true },
        { "Controllers",            bus_property_append_strv,           "as", offsetof(Manager, controllers),        true },
        { "ResetControllers",       bus_property_append_strv,           "as", offsetof(Manager, reset_controllers),  true },
        { "NAutoVTs",               bus_property_append_unsigned,       "u",  offsetof(Manager, n_autovts)           },
        { "KillOnlyUsers",          bus_property_append_strv,           "as", offsetof(Manager, kill_only_users),    true },
        { "KillExcludeUsers",       bus_property_append_strv,           "as", offsetof(Manager, kill_exclude_users), true },
        { "KillUserProcesses",      bus_property_append_bool,           "b",  offsetof(Manager, kill_user_processes) },
        { "IdleHint",               bus_manager_append_idle_hint,       "b",  0 },
        { "IdleSinceHint",          bus_manager_append_idle_hint_since, "t",  0 },
        { "IdleSinceHintMonotonic", bus_manager_append_idle_hint_since, "t",  0 },
        { NULL, }
};

static DBusHandlerResult manager_message_handler(
                DBusConnection *connection,
                DBusMessage *message,
                void *userdata) {

        Manager *m = userdata;

        DBusError error;
        DBusMessage *reply = NULL;
        int r;

        assert(connection);
        assert(message);
        assert(m);

        dbus_error_init(&error);

        if (dbus_message_is_method_call(message, "org.freedesktop.login1.Manager", "GetSession")) {
                const char *name;
                char *p;
                Session *session;
                bool b;

                if (!dbus_message_get_args(
                                    message,
                                    &error,
                                    DBUS_TYPE_STRING, &name,
                                    DBUS_TYPE_INVALID))
                        return bus_send_error_reply(connection, message, &error, -EINVAL);

                session = hashmap_get(m->sessions, name);
                if (!session)
                        return bus_send_error_reply(connection, message, &error, -ENOENT);

                reply = dbus_message_new_method_return(message);
                if (!reply)
                        goto oom;

                p = session_bus_path(session);
                if (!p)
                        goto oom;

                b = dbus_message_append_args(
                                reply,
                                DBUS_TYPE_OBJECT_PATH, &p,
                                DBUS_TYPE_INVALID);
                free(p);

                if (!b)
                        goto oom;

        } else if (dbus_message_is_method_call(message, "org.freedesktop.login1.Manager", "GetSessionByPID")) {
                uint32_t pid;
                char *p;
                Session *session;
                bool b;

                if (!dbus_message_get_args(
                                    message,
                                    &error,
                                    DBUS_TYPE_UINT32, &pid,
                                    DBUS_TYPE_INVALID))
                        return bus_send_error_reply(connection, message, &error, -EINVAL);

                r = manager_get_session_by_pid(m, pid, &session);
                if (r <= 0)
                        return bus_send_error_reply(connection, message, NULL, r < 0 ? r : -ENOENT);

                reply = dbus_message_new_method_return(message);
                if (!reply)
                        goto oom;

                p = session_bus_path(session);
                if (!p)
                        goto oom;

                b = dbus_message_append_args(
                                reply,
                                DBUS_TYPE_OBJECT_PATH, &p,
                                DBUS_TYPE_INVALID);
                free(p);

                if (!b)
                        goto oom;

        } else if (dbus_message_is_method_call(message, "org.freedesktop.login1.Manager", "GetUser")) {
                uint32_t uid;
                char *p;
                User *user;
                bool b;

                if (!dbus_message_get_args(
                                    message,
                                    &error,
                                    DBUS_TYPE_UINT32, &uid,
                                    DBUS_TYPE_INVALID))
                        return bus_send_error_reply(connection, message, &error, -EINVAL);

                user = hashmap_get(m->users, ULONG_TO_PTR((unsigned long) uid));
                if (!user)
                        return bus_send_error_reply(connection, message, &error, -ENOENT);

                reply = dbus_message_new_method_return(message);
                if (!reply)
                        goto oom;

                p = user_bus_path(user);
                if (!p)
                        goto oom;

                b = dbus_message_append_args(
                                reply,
                                DBUS_TYPE_OBJECT_PATH, &p,
                                DBUS_TYPE_INVALID);
                free(p);

                if (!b)
                        goto oom;

        } else if (dbus_message_is_method_call(message, "org.freedesktop.login1.Manager", "GetSeat")) {
                const char *name;
                char *p;
                Seat *seat;
                bool b;

                if (!dbus_message_get_args(
                                    message,
                                    &error,
                                    DBUS_TYPE_STRING, &name,
                                    DBUS_TYPE_INVALID))
                        return bus_send_error_reply(connection, message, &error, -EINVAL);

                seat = hashmap_get(m->seats, name);
                if (!seat)
                        return bus_send_error_reply(connection, message, &error, -ENOENT);

                reply = dbus_message_new_method_return(message);
                if (!reply)
                        goto oom;

                p = seat_bus_path(seat);
                if (!p)
                        goto oom;

                b = dbus_message_append_args(
                                reply,
                                DBUS_TYPE_OBJECT_PATH, &p,
                                DBUS_TYPE_INVALID);
                free(p);

                if (!b)
                        goto oom;

        } else if (dbus_message_is_method_call(message, "org.freedesktop.login1.Manager", "ListSessions")) {
                char *p;
                Session *session;
                Iterator i;
                DBusMessageIter iter, sub;
                const char *empty = "";

                reply = dbus_message_new_method_return(message);
                if (!reply)
                        goto oom;

                dbus_message_iter_init_append(reply, &iter);

                if (!dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "(susso)", &sub))
                        goto oom;

                HASHMAP_FOREACH(session, m->sessions, i) {
                        DBusMessageIter sub2;
                        uint32_t uid;

                        if (!dbus_message_iter_open_container(&sub, DBUS_TYPE_STRUCT, NULL, &sub2))
                                goto oom;

                        uid = session->user->uid;

                        p = session_bus_path(session);
                        if (!p)
                                goto oom;

                        if (!dbus_message_iter_append_basic(&sub2, DBUS_TYPE_STRING, &session->id) ||
                            !dbus_message_iter_append_basic(&sub2, DBUS_TYPE_UINT32, &uid) ||
                            !dbus_message_iter_append_basic(&sub2, DBUS_TYPE_STRING, &session->user->name) ||
                            !dbus_message_iter_append_basic(&sub2, DBUS_TYPE_STRING, session->seat ? (const char**) &session->seat->id : &empty) ||
                            !dbus_message_iter_append_basic(&sub2, DBUS_TYPE_OBJECT_PATH, &p)) {
                                free(p);
                                goto oom;
                        }

                        free(p);

                        if (!dbus_message_iter_close_container(&sub, &sub2))
                                goto oom;
                }

                if (!dbus_message_iter_close_container(&iter, &sub))
                        goto oom;

        } else if (dbus_message_is_method_call(message, "org.freedesktop.login1.Manager", "ListUsers")) {
                char *p;
                User *user;
                Iterator i;
                DBusMessageIter iter, sub;

                reply = dbus_message_new_method_return(message);
                if (!reply)
                        goto oom;

                dbus_message_iter_init_append(reply, &iter);

                if (!dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "(uso)", &sub))
                        goto oom;

                HASHMAP_FOREACH(user, m->users, i) {
                        DBusMessageIter sub2;
                        uint32_t uid;

                        if (!dbus_message_iter_open_container(&sub, DBUS_TYPE_STRUCT, NULL, &sub2))
                                goto oom;

                        uid = user->uid;

                        p = user_bus_path(user);
                        if (!p)
                                goto oom;

                        if (!dbus_message_iter_append_basic(&sub2, DBUS_TYPE_UINT32, &uid) ||
                            !dbus_message_iter_append_basic(&sub2, DBUS_TYPE_STRING, &user->name) ||
                            !dbus_message_iter_append_basic(&sub2, DBUS_TYPE_OBJECT_PATH, &p)) {
                                free(p);
                                goto oom;
                        }

                        free(p);

                        if (!dbus_message_iter_close_container(&sub, &sub2))
                                goto oom;
                }

                if (!dbus_message_iter_close_container(&iter, &sub))
                        goto oom;

        } else if (dbus_message_is_method_call(message, "org.freedesktop.login1.Manager", "ListSeats")) {
                char *p;
                Seat *seat;
                Iterator i;
                DBusMessageIter iter, sub;

                reply = dbus_message_new_method_return(message);
                if (!reply)
                        goto oom;

                dbus_message_iter_init_append(reply, &iter);

                if (!dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "(so)", &sub))
                        goto oom;

                HASHMAP_FOREACH(seat, m->seats, i) {
                        DBusMessageIter sub2;

                        if (!dbus_message_iter_open_container(&sub, DBUS_TYPE_STRUCT, NULL, &sub2))
                                goto oom;

                        p = seat_bus_path(seat);
                        if (!p)
                                goto oom;

                        if (!dbus_message_iter_append_basic(&sub2, DBUS_TYPE_STRING, &seat->id) ||
                            !dbus_message_iter_append_basic(&sub2, DBUS_TYPE_OBJECT_PATH, &p)) {
                                free(p);
                                goto oom;
                        }

                        free(p);

                        if (!dbus_message_iter_close_container(&sub, &sub2))
                                goto oom;
                }

                if (!dbus_message_iter_close_container(&iter, &sub))
                        goto oom;

        } else if (dbus_message_is_method_call(message, "org.freedesktop.login1.Manager", "CreateSession")) {

                r = bus_manager_create_session(m, message, &reply);

                /* Don't delay the work on OOM here, since it might be
                 * triggered by a low RLIMIT_NOFILE here (since we
                 * send a dupped fd to the client), and we'd rather
                 * see this fail quickly then be retried later */

                if (r < 0)
                        return bus_send_error_reply(connection, message, &error, r);

        } else if (dbus_message_is_method_call(message, "org.freedesktop.login1.Manager", "ActivateSession")) {
                const char *name;
                Session *session;

                if (!dbus_message_get_args(
                                    message,
                                    &error,
                                    DBUS_TYPE_STRING, &name,
                                    DBUS_TYPE_INVALID))
                        return bus_send_error_reply(connection, message, &error, -EINVAL);

                session = hashmap_get(m->sessions, name);
                if (!session)
                        return bus_send_error_reply(connection, message, &error, -ENOENT);

                r = session_activate(session);
                if (r < 0)
                        return bus_send_error_reply(connection, message, NULL, r);

                reply = dbus_message_new_method_return(message);
                if (!reply)
                        goto oom;

        } else if (dbus_message_is_method_call(message, "org.freedesktop.login1.Manager", "ActivateSessionOnSeat")) {
                const char *session_name, *seat_name;
                Session *session;
                Seat *seat;

                /* Same as ActivateSession() but refuses to work if
                 * the seat doesn't match */

                if (!dbus_message_get_args(
                                    message,
                                    &error,
                                    DBUS_TYPE_STRING, &session_name,
                                    DBUS_TYPE_STRING, &seat_name,
                                    DBUS_TYPE_INVALID))
                        return bus_send_error_reply(connection, message, &error, -EINVAL);

                session = hashmap_get(m->sessions, session_name);
                if (!session)
                        return bus_send_error_reply(connection, message, &error, -ENOENT);

                seat = hashmap_get(m->seats, seat_name);
                if (!seat)
                        return bus_send_error_reply(connection, message, &error, -ENOENT);

                if (session->seat != seat)
                        return bus_send_error_reply(connection, message, &error, -EINVAL);

                r = session_activate(session);
                if (r < 0)
                        return bus_send_error_reply(connection, message, NULL, r);

                reply = dbus_message_new_method_return(message);
                if (!reply)
                        goto oom;

        } else if (dbus_message_is_method_call(message, "org.freedesktop.login1.Manager", "LockSession") ||
                   dbus_message_is_method_call(message, "org.freedesktop.login1.Manager", "UnlockSession")) {
                const char *name;
                Session *session;

                if (!dbus_message_get_args(
                                    message,
                                    &error,
                                    DBUS_TYPE_STRING, &name,
                                    DBUS_TYPE_INVALID))
                        return bus_send_error_reply(connection, message, &error, -EINVAL);

                session = hashmap_get(m->sessions, name);
                if (!session)
                        return bus_send_error_reply(connection, message, &error, -ENOENT);

                if (session_send_lock(session, streq(dbus_message_get_member(message), "LockSession")) < 0)
                        goto oom;

                reply = dbus_message_new_method_return(message);
                if (!reply)
                        goto oom;

        } else if (dbus_message_is_method_call(message, "org.freedesktop.login1.Manager", "KillSession")) {
                const char *swho;
                int32_t signo;
                KillWho who;
                const char *name;
                Session *session;

                if (!dbus_message_get_args(
                                    message,
                                    &error,
                                    DBUS_TYPE_STRING, &name,
                                    DBUS_TYPE_STRING, &swho,
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

                if (signo <= 0 || signo >= _NSIG)
                        return bus_send_error_reply(connection, message, &error, -EINVAL);

                session = hashmap_get(m->sessions, name);
                if (!session)
                        return bus_send_error_reply(connection, message, &error, -ENOENT);

                r = session_kill(session, who, signo);
                if (r < 0)
                        return bus_send_error_reply(connection, message, NULL, r);

                reply = dbus_message_new_method_return(message);
                if (!reply)
                        goto oom;

        } else if (dbus_message_is_method_call(message, "org.freedesktop.login1.Manager", "KillUser")) {
                uint32_t uid;
                User *user;
                int32_t signo;

                if (!dbus_message_get_args(
                                    message,
                                    &error,
                                    DBUS_TYPE_UINT32, &uid,
                                    DBUS_TYPE_INT32, &signo,
                                    DBUS_TYPE_INVALID))
                        return bus_send_error_reply(connection, message, &error, -EINVAL);

                if (signo <= 0 || signo >= _NSIG)
                        return bus_send_error_reply(connection, message, &error, -EINVAL);

                user = hashmap_get(m->users, ULONG_TO_PTR((unsigned long) uid));
                if (!user)
                        return bus_send_error_reply(connection, message, &error, -ENOENT);

                r = user_kill(user, signo);
                if (r < 0)
                        return bus_send_error_reply(connection, message, NULL, r);

                reply = dbus_message_new_method_return(message);
                if (!reply)
                        goto oom;

        } else if (dbus_message_is_method_call(message, "org.freedesktop.login1.Manager", "TerminateSession")) {
                const char *name;
                Session *session;

                if (!dbus_message_get_args(
                                    message,
                                    &error,
                                    DBUS_TYPE_STRING, &name,
                                    DBUS_TYPE_INVALID))
                        return bus_send_error_reply(connection, message, &error, -EINVAL);

                session = hashmap_get(m->sessions, name);
                if (!session)
                        return bus_send_error_reply(connection, message, &error, -ENOENT);

                r = session_stop(session);
                if (r < 0)
                        return bus_send_error_reply(connection, message, NULL, r);

                reply = dbus_message_new_method_return(message);
                if (!reply)
                        goto oom;

        } else if (dbus_message_is_method_call(message, "org.freedesktop.login1.Manager", "TerminateUser")) {
                uint32_t uid;
                User *user;

                if (!dbus_message_get_args(
                                    message,
                                    &error,
                                    DBUS_TYPE_UINT32, &uid,
                                    DBUS_TYPE_INVALID))
                        return bus_send_error_reply(connection, message, &error, -EINVAL);

                user = hashmap_get(m->users, ULONG_TO_PTR((unsigned long) uid));
                if (!user)
                        return bus_send_error_reply(connection, message, &error, -ENOENT);

                r = user_stop(user);
                if (r < 0)
                        return bus_send_error_reply(connection, message, NULL, r);

                reply = dbus_message_new_method_return(message);
                if (!reply)
                        goto oom;

        } else if (dbus_message_is_method_call(message, "org.freedesktop.login1.Manager", "TerminateSeat")) {
                const char *name;
                Seat *seat;

                if (!dbus_message_get_args(
                                    message,
                                    &error,
                                    DBUS_TYPE_STRING, &name,
                                    DBUS_TYPE_INVALID))
                        return bus_send_error_reply(connection, message, &error, -EINVAL);

                seat = hashmap_get(m->seats, name);
                if (!seat)
                        return bus_send_error_reply(connection, message, &error, -ENOENT);

                r = seat_stop_sessions(seat);
                if (r < 0)
                        return bus_send_error_reply(connection, message, NULL, r);

                reply = dbus_message_new_method_return(message);
                if (!reply)
                        goto oom;

        } else if (dbus_message_is_method_call(message, "org.freedesktop.login1.Manager", "SetUserLinger")) {
                uint32_t uid;
                struct passwd *pw;
                dbus_bool_t b, interactive;
                char *path;

                if (!dbus_message_get_args(
                                    message,
                                    &error,
                                    DBUS_TYPE_UINT32, &uid,
                                    DBUS_TYPE_BOOLEAN, &b,
                                    DBUS_TYPE_BOOLEAN, &interactive,
                                    DBUS_TYPE_INVALID))
                        return bus_send_error_reply(connection, message, &error, -EINVAL);

                errno = 0;
                pw = getpwuid(uid);
                if (!pw)
                        return bus_send_error_reply(connection, message, NULL, errno ? -errno : -EINVAL);

                r = verify_polkit(connection, message, "org.freedesktop.login1.set-user-linger", interactive, NULL, &error);
                if (r < 0)
                        return bus_send_error_reply(connection, message, &error, r);

                mkdir_p("/var/lib/systemd", 0755);

                r = safe_mkdir("/var/lib/systemd/linger", 0755, 0, 0);
                if (r < 0)
                        return bus_send_error_reply(connection, message, &error, r);

                path = strappend("/var/lib/systemd/linger/", pw->pw_name);
                if (!path)
                        goto oom;

                if (b) {
                        User *u;

                        r = touch(path);
                        free(path);

                        if (r < 0)
                                return bus_send_error_reply(connection, message, &error, r);

                        if (manager_add_user_by_uid(m, uid, &u) >= 0)
                                user_start(u);

                } else {
                        User *u;

                        r = unlink(path);
                        free(path);

                        if (r < 0 && errno != ENOENT)
                                return bus_send_error_reply(connection, message, &error, -errno);

                        u = hashmap_get(m->users, ULONG_TO_PTR((unsigned long) uid));
                        if (u)
                                user_add_to_gc_queue(u);
                }

                reply = dbus_message_new_method_return(message);
                if (!reply)
                        goto oom;

        } else if (dbus_message_is_method_call(message, "org.freedesktop.login1.Manager", "AttachDevice")) {
                const char *sysfs, *seat;
                dbus_bool_t interactive;

                if (!dbus_message_get_args(
                                    message,
                                    &error,
                                    DBUS_TYPE_STRING, &seat,
                                    DBUS_TYPE_STRING, &sysfs,
                                    DBUS_TYPE_BOOLEAN, &interactive,
                                    DBUS_TYPE_INVALID))
                        return bus_send_error_reply(connection, message, &error, -EINVAL);

                if (!path_startswith(sysfs, "/sys") || !seat_name_is_valid(seat))
                        return bus_send_error_reply(connection, message, NULL, -EINVAL);

                r = verify_polkit(connection, message, "org.freedesktop.login1.attach-device", interactive, NULL, &error);
                if (r < 0)
                        return bus_send_error_reply(connection, message, &error, r);

                r = attach_device(m, seat, sysfs);
                if (r < 0)
                        return bus_send_error_reply(connection, message, NULL, -EINVAL);

                reply = dbus_message_new_method_return(message);
                if (!reply)
                        goto oom;


        } else if (dbus_message_is_method_call(message, "org.freedesktop.login1.Manager", "FlushDevices")) {
                dbus_bool_t interactive;

                if (!dbus_message_get_args(
                                    message,
                                    &error,
                                    DBUS_TYPE_BOOLEAN, &interactive,
                                    DBUS_TYPE_INVALID))
                        return bus_send_error_reply(connection, message, &error, -EINVAL);

                r = verify_polkit(connection, message, "org.freedesktop.login1.flush-devices", interactive, NULL, &error);
                if (r < 0)
                        return bus_send_error_reply(connection, message, &error, r);

                r = flush_devices(m);
                if (r < 0)
                        return bus_send_error_reply(connection, message, NULL, -EINVAL);

                reply = dbus_message_new_method_return(message);
                if (!reply)
                        goto oom;

        } else if (dbus_message_is_method_call(message, "org.freedesktop.login1.Manager", "PowerOff") ||
                   dbus_message_is_method_call(message, "org.freedesktop.login1.Manager", "Reboot")) {
                dbus_bool_t interactive;
                bool multiple_sessions;
                DBusMessage *forward, *freply;
                const char *name;
                const char *mode = "replace";
                const char *action;

                if (!dbus_message_get_args(
                                    message,
                                    &error,
                                    DBUS_TYPE_BOOLEAN, &interactive,
                                    DBUS_TYPE_INVALID))
                        return bus_send_error_reply(connection, message, &error, -EINVAL);

                r = have_multiple_sessions(connection, m, message, &error);
                if (r < 0)
                        return bus_send_error_reply(connection, message, &error, r);

                multiple_sessions = r > 0;

                if (streq(dbus_message_get_member(message), "PowerOff")) {
                        if (multiple_sessions)
                                action = "org.freedesktop.login1.power-off-multiple-sessions";
                        else
                                action = "org.freedesktop.login1.power-off";

                        name = SPECIAL_POWEROFF_TARGET;
                } else {
                        if (multiple_sessions)
                                action = "org.freedesktop.login1.reboot-multiple-sessions";
                        else
                                action = "org.freedesktop.login1.reboot";

                        name = SPECIAL_REBOOT_TARGET;
                }

                r = verify_polkit(connection, message, action, interactive, NULL, &error);
                if (r < 0)
                        return bus_send_error_reply(connection, message, &error, r);

                forward = dbus_message_new_method_call(
                              "org.freedesktop.systemd1",
                              "/org/freedesktop/systemd1",
                              "org.freedesktop.systemd1.Manager",
                              "StartUnit");
                if (!forward)
                        return bus_send_error_reply(connection, message, NULL, -ENOMEM);

                if (!dbus_message_append_args(forward,
                                              DBUS_TYPE_STRING, &name,
                                              DBUS_TYPE_STRING, &mode,
                                              DBUS_TYPE_INVALID)) {
                        dbus_message_unref(forward);
                        return bus_send_error_reply(connection, message, NULL, -ENOMEM);
                }

                freply = dbus_connection_send_with_reply_and_block(connection, forward, -1, &error);
                dbus_message_unref(forward);

                if (!freply)
                        return bus_send_error_reply(connection, message, &error, -EIO);

                dbus_message_unref(freply);

                reply = dbus_message_new_method_return(message);
                if (!reply)
                        goto oom;

        } else if (dbus_message_is_method_call(message, "org.freedesktop.login1.Manager", "CanPowerOff") ||
                   dbus_message_is_method_call(message, "org.freedesktop.login1.Manager", "CanReboot")) {

                bool multiple_sessions, challenge, b;
                const char *t, *action;

                r = have_multiple_sessions(connection, m, message, &error);
                if (r < 0)
                        return bus_send_error_reply(connection, message, &error, r);

                multiple_sessions = r > 0;

                if (streq(dbus_message_get_member(message), "CanPowerOff")) {
                        if (multiple_sessions)
                                action = "org.freedesktop.login1.power-off-multiple-sessions";
                        else
                                action = "org.freedesktop.login1.power-off";

                } else {
                        if (multiple_sessions)
                                action = "org.freedesktop.login1.reboot-multiple-sessions";
                        else
                                action = "org.freedesktop.login1.reboot";
                }

                r = verify_polkit(connection, message, action, false, &challenge, &error);
                if (r < 0)
                        return bus_send_error_reply(connection, message, &error, r);

                reply = dbus_message_new_method_return(message);
                if (!reply)
                        goto oom;

                t =     r > 0 ?     "yes" :
                        challenge ? "challenge" :
                                    "no";

                b = dbus_message_append_args(
                                reply,
                                DBUS_TYPE_STRING, &t,
                                DBUS_TYPE_INVALID);
                if (!b)
                        goto oom;

        } else if (dbus_message_is_method_call(message, "org.freedesktop.DBus.Introspectable", "Introspect")) {
                char *introspection = NULL;
                FILE *f;
                Iterator i;
                Session *session;
                Seat *seat;
                User *user;
                size_t size;
                char *p;

                if (!(reply = dbus_message_new_method_return(message)))
                        goto oom;

                /* We roll our own introspection code here, instead of
                 * relying on bus_default_message_handler() because we
                 * need to generate our introspection string
                 * dynamically. */

                if (!(f = open_memstream(&introspection, &size)))
                        goto oom;

                fputs(INTROSPECTION_BEGIN, f);

                HASHMAP_FOREACH(seat, m->seats, i) {
                        p = bus_path_escape(seat->id);

                        if (p) {
                                fprintf(f, "<node name=\"seat/%s\"/>", p);
                                free(p);
                        }
                }

                HASHMAP_FOREACH(user, m->users, i)
                        fprintf(f, "<node name=\"user/%llu\"/>", (unsigned long long) user->uid);

                HASHMAP_FOREACH(session, m->sessions, i) {
                        p = bus_path_escape(session->id);

                        if (p) {
                                fprintf(f, "<node name=\"session/%s\"/>", p);
                                free(p);
                        }
                }

                fputs(INTROSPECTION_END, f);

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
        } else {
                const BusBoundProperties bps[] = {
                        { "org.freedesktop.login1.Manager", bus_login_manager_properties, m },
                        { NULL, }
                };
                return bus_default_message_handler(connection, message, NULL, INTERFACES_LIST, bps);
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

        dbus_error_free(&error);

        return DBUS_HANDLER_RESULT_NEED_MEMORY;
}

const DBusObjectPathVTable bus_manager_vtable = {
        .message_function = manager_message_handler
};

DBusHandlerResult bus_message_filter(
                DBusConnection *connection,
                DBusMessage *message,
                void *userdata) {

        Manager *m = userdata;
        DBusError error;

        assert(m);
        assert(connection);
        assert(message);

        dbus_error_init(&error);

        if (dbus_message_is_signal(message, "org.freedesktop.systemd1.Agent", "Released")) {
                const char *cgroup;

                if (!dbus_message_get_args(message, &error,
                                           DBUS_TYPE_STRING, &cgroup,
                                           DBUS_TYPE_INVALID))
                        log_error("Failed to parse Released message: %s", bus_error_message(&error));
                else
                        manager_cgroup_notify_empty(m, cgroup);
        }

        dbus_error_free(&error);

        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

int manager_send_changed(Manager *manager, const char *properties) {
        DBusMessage *m;
        int r = -ENOMEM;

        assert(manager);

        m = bus_properties_changed_new("/org/freedesktop/login1", "org.freedesktop.login1.Manager", properties);
        if (!m)
                goto finish;

        if (!dbus_connection_send(manager->bus, m, NULL))
                goto finish;

        r = 0;

finish:
        if (m)
                dbus_message_unref(m);

        return r;
}
