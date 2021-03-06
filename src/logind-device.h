/*-*- Mode: C; c-basic-offset: 8; indent-tabs-mode: nil -*-*/

#ifndef foologinddevicehfoo
#define foologinddevicehfoo

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

typedef struct Device Device;

#include "list.h"
#include "util.h"
#include "logind.h"
#include "logind-seat.h"

struct Device {
        Manager *manager;

        char *sysfs;
        Seat *seat;

        dual_timestamp timestamp;

        LIST_FIELDS(struct Device, devices);
};

Device* device_new(Manager *m, const char *sysfs);
void device_free(Device *d);
void device_attach(Device *d, Seat *s);
void device_detach(Device *d);

#endif
