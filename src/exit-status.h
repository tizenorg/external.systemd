/*-*- Mode: C; c-basic-offset: 8; indent-tabs-mode: nil -*-*/

#ifndef fooexitstatushfoo
#define fooexitstatushfoo

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

#include <stdbool.h>

typedef enum ExitStatus {
        /* EXIT_SUCCESS defined by libc */
        /* EXIT_FAILURE defined by libc */
        EXIT_INVALIDARGUMENT = 2,
        EXIT_NOTIMPLEMENTED = 3,
        EXIT_NOPERMISSION = 4,
        EXIT_NOTINSTALLED = 5,
        EXIT_NOTCONFIGURED = 6,
        EXIT_NOTRUNNING = 7,

        /* The LSB suggests that error codes >= 200 are "reserved". We
         * use them here under the assumption that they hence are
         * unused by init scripts.
         *
         * http://refspecs.freestandards.org/LSB_3.1.0/LSB-Core-generic/LSB-Core-generic/iniscrptact.html */

        EXIT_CHDIR = 200,
        EXIT_NICE,
        EXIT_FDS,
        EXIT_EXEC,
        EXIT_MEMORY,
        EXIT_LIMITS,
        EXIT_OOM_ADJUST,
        EXIT_SIGNAL_MASK,
        EXIT_STDIN,
        EXIT_STDOUT,
        EXIT_CHROOT,   /* 210 */
        EXIT_IOPRIO,
        EXIT_TIMERSLACK,
        EXIT_SECUREBITS,
        EXIT_SETSCHEDULER,
        EXIT_CPUAFFINITY,
        EXIT_GROUP,
        EXIT_USER,
        EXIT_CAPABILITIES,
        EXIT_CGROUP,
        EXIT_SETSID,   /* 220 */
        EXIT_CONFIRM,
        EXIT_STDERR,
        EXIT_TCPWRAP,
        EXIT_PAM,
        EXIT_NETWORK

} ExitStatus;

typedef enum ExitStatusLevel {
        EXIT_STATUS_MINIMAL,
        EXIT_STATUS_SYSTEMD,
        EXIT_STATUS_LSB,
        EXIT_STATUS_FULL = EXIT_STATUS_LSB
} ExitStatusLevel;

const char* exit_status_to_string(ExitStatus status, ExitStatusLevel level);

bool is_clean_exit(int code, int status);
bool is_clean_exit_lsb(int code, int status);

#endif
