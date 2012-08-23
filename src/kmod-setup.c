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

#include <sys/wait.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

#include "macro.h"
#include "execute.h"

#include "kmod-setup.h"

static const char * const kmod_table[] = {
        "autofs4", "/sys/class/misc/autofs",
        "ipv6",    "/sys/module/ipv6",
        "unix",    "/proc/net/unix"
};

int kmod_setup(void) {
        unsigned i, n = 0;
        const char * cmdline[3 + ELEMENTSOF(kmod_table) + 1];
        ExecCommand command;
        ExecContext context;
        pid_t pid;
        int r;

        for (i = 0; i < ELEMENTSOF(kmod_table); i += 2) {

                if (access(kmod_table[i+1], F_OK) >= 0)
                        continue;

                log_debug("Your kernel apparently lacks built-in %s support. Might be a good idea to compile it in. "
                          "We'll now try to work around this by calling '/sbin/modprobe %s'...",
                          kmod_table[i], kmod_table[i]);

                cmdline[3 + n++] = kmod_table[i];
        }

        if (n <= 0)
                return 0;

        cmdline[0] = "/sbin/modprobe";
        cmdline[1] = "-qab";
        cmdline[2] = "--";
        cmdline[3 + n] = NULL;

        zero(command);
        zero(context);

        command.path = (char*) cmdline[0];
        command.argv = (char**) cmdline;

        exec_context_init(&context);
        r = exec_spawn(&command, NULL, &context, NULL, 0, NULL, false, false, false, false, NULL, NULL, &pid);
        exec_context_done(&context);

        if (r < 0) {
                log_error("Failed to spawn %s: %s", cmdline[0], strerror(-r));
                return r;
        }

        return wait_for_terminate_and_warn(cmdline[0], pid);
}
