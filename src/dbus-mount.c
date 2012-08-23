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

#include "dbus-unit.h"
#include "dbus-mount.h"
#include "dbus-execute.h"
#include "dbus-common.h"

#define BUS_MOUNT_INTERFACE                                             \
        " <interface name=\"org.freedesktop.systemd1.Mount\">\n"        \
        "  <property name=\"Where\" type=\"s\" access=\"read\"/>\n"     \
        "  <property name=\"What\" type=\"s\" access=\"read\"/>\n"      \
        "  <property name=\"Options\" type=\"s\" access=\"read\"/>\n"   \
        "  <property name=\"Type\" type=\"s\" access=\"read\"/>\n"      \
        "  <property name=\"TimeoutUSec\" type=\"t\" access=\"read\"/>\n" \
        BUS_EXEC_COMMAND_INTERFACE("ExecMount")                         \
        BUS_EXEC_COMMAND_INTERFACE("ExecUnmount")                       \
        BUS_EXEC_COMMAND_INTERFACE("ExecRemount")                       \
        BUS_EXEC_CONTEXT_INTERFACE                                      \
        "  <property name=\"ControlPID\" type=\"u\" access=\"read\"/>\n" \
        "  <property name=\"DirectoryMode\" type=\"u\" access=\"read\"/>\n" \
        " </interface>\n"

#define INTROSPECTION                                                   \
        DBUS_INTROSPECT_1_0_XML_DOCTYPE_DECL_NODE                       \
        "<node>\n"                                                      \
        BUS_UNIT_INTERFACE                                              \
        BUS_MOUNT_INTERFACE                                             \
        BUS_PROPERTIES_INTERFACE                                        \
        BUS_PEER_INTERFACE                                              \
        BUS_INTROSPECTABLE_INTERFACE                                    \
        "</node>\n"

#define INTERFACES_LIST                              \
        BUS_UNIT_INTERFACES_LIST                     \
        "org.freedesktop.systemd1.Mount\0"

const char bus_mount_interface[] _introspect_("Mount") = BUS_MOUNT_INTERFACE;

const char bus_mount_invalidating_properties[] =
        "What\0"
        "Options\0"
        "Type\0"
        "ExecMount\0"
        "ExecUnmount\0"
        "ExecRemount\0"
        "ControlPID\0";

static int bus_mount_append_what(DBusMessageIter *i, const char *property, void *data) {
        Mount *m = data;
        const char *d;

        assert(i);
        assert(property);
        assert(m);

        if (m->from_proc_self_mountinfo && m->parameters_proc_self_mountinfo.what)
                d = m->parameters_proc_self_mountinfo.what;
        else if (m->from_fragment && m->parameters_fragment.what)
                d = m->parameters_fragment.what;
        else if (m->from_etc_fstab && m->parameters_etc_fstab.what)
                d = m->parameters_etc_fstab.what;
        else
                d = "";

        if (!dbus_message_iter_append_basic(i, DBUS_TYPE_STRING, &d))
                return -ENOMEM;

        return 0;
}

static int bus_mount_append_options(DBusMessageIter *i, const char *property, void *data) {
        Mount *m = data;
        const char *d;

        assert(i);
        assert(property);
        assert(m);

        if (m->from_proc_self_mountinfo && m->parameters_proc_self_mountinfo.options)
                d = m->parameters_proc_self_mountinfo.options;
        else if (m->from_fragment && m->parameters_fragment.options)
                d = m->parameters_fragment.options;
        else if (m->from_etc_fstab && m->parameters_etc_fstab.options)
                d = m->parameters_etc_fstab.options;
        else
                d = "";

        if (!dbus_message_iter_append_basic(i, DBUS_TYPE_STRING, &d))
                return -ENOMEM;

        return 0;
}

static int bus_mount_append_type(DBusMessageIter *i, const char *property, void *data) {
        Mount *m = data;
        const char *d;

        assert(i);
        assert(property);
        assert(m);

        if (m->from_proc_self_mountinfo && m->parameters_proc_self_mountinfo.fstype)
                d = m->parameters_proc_self_mountinfo.fstype;
        else if (m->from_fragment && m->parameters_fragment.fstype)
                d = m->parameters_fragment.fstype;
        else if (m->from_etc_fstab && m->parameters_etc_fstab.fstype)
                d = m->parameters_etc_fstab.fstype;
        else
                d = "";

        if (!dbus_message_iter_append_basic(i, DBUS_TYPE_STRING, &d))
                return -ENOMEM;

        return 0;
}

DBusHandlerResult bus_mount_message_handler(Unit *u, DBusConnection *c, DBusMessage *message) {

        const BusProperty properties[] = {
                BUS_UNIT_PROPERTIES,
                { "org.freedesktop.systemd1.Mount", "Where",         bus_property_append_string, "s", u->mount.where           },
                { "org.freedesktop.systemd1.Mount", "What",          bus_mount_append_what,      "s", u                        },
                { "org.freedesktop.systemd1.Mount", "Options",       bus_mount_append_options,   "s", u                        },
                { "org.freedesktop.systemd1.Mount", "Type",          bus_mount_append_type,      "s", u                        },
                { "org.freedesktop.systemd1.Mount", "TimeoutUSec",   bus_property_append_usec,   "t", &u->mount.timeout_usec   },
                BUS_EXEC_COMMAND_PROPERTY("org.freedesktop.systemd1.Mount", u->mount.exec_command+MOUNT_EXEC_MOUNT,   "ExecMount"),
                BUS_EXEC_COMMAND_PROPERTY("org.freedesktop.systemd1.Mount", u->mount.exec_command+MOUNT_EXEC_UNMOUNT, "ExecUnmount"),
                BUS_EXEC_COMMAND_PROPERTY("org.freedesktop.systemd1.Mount", u->mount.exec_command+MOUNT_EXEC_REMOUNT, "ExecRemount"),
                BUS_EXEC_CONTEXT_PROPERTIES("org.freedesktop.systemd1.Mount", u->mount.exec_context),
                { "org.freedesktop.systemd1.Mount", "ControlPID",    bus_property_append_pid,    "u", &u->mount.control_pid    },
                { "org.freedesktop.systemd1.Mount", "DirectoryMode", bus_property_append_mode,   "u", &u->mount.directory_mode },
                { NULL, NULL, NULL, NULL, NULL }
        };

        return bus_default_message_handler(c, message, INTROSPECTION, INTERFACES_LIST, properties);
}
