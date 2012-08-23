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
#include <signal.h>
#include <dirent.h>
#include <unistd.h>

#include "unit.h"
#include "service.h"
#include "load-fragment.h"
#include "load-dropin.h"
#include "log.h"
#include "strv.h"
#include "unit-name.h"
#include "dbus-service.h"
#include "special.h"
#include "bus-errors.h"
#include "exit-status.h"
#include "def.h"
#include "util.h"

#ifdef HAVE_SYSV_COMPAT

#define DEFAULT_SYSV_TIMEOUT_USEC (5*USEC_PER_MINUTE)

typedef enum RunlevelType {
        RUNLEVEL_UP,
        RUNLEVEL_DOWN,
        RUNLEVEL_SYSINIT
} RunlevelType;

static const struct {
        const char *path;
        const char *target;
        const RunlevelType type;
} rcnd_table[] = {
        /* Standard SysV runlevels for start-up */
        { "rc1.d",  SPECIAL_RESCUE_TARGET,    RUNLEVEL_UP },
        { "rc2.d",  SPECIAL_RUNLEVEL2_TARGET, RUNLEVEL_UP },
        { "rc3.d",  SPECIAL_RUNLEVEL3_TARGET, RUNLEVEL_UP },
        { "rc4.d",  SPECIAL_RUNLEVEL4_TARGET, RUNLEVEL_UP },
        { "rc5.d",  SPECIAL_RUNLEVEL5_TARGET, RUNLEVEL_UP },

#ifdef TARGET_SUSE
        /* SUSE style boot.d */
        { "boot.d", SPECIAL_SYSINIT_TARGET,   RUNLEVEL_SYSINIT },
#endif

#if defined(TARGET_DEBIAN) || defined(TARGET_UBUNTU) || defined(TARGET_ANGSTROM)
        /* Debian style rcS.d */
        { "rcS.d",  SPECIAL_SYSINIT_TARGET,   RUNLEVEL_SYSINIT },
#endif

        /* Standard SysV runlevels for shutdown */
        { "rc0.d",  SPECIAL_POWEROFF_TARGET,  RUNLEVEL_DOWN },
        { "rc6.d",  SPECIAL_REBOOT_TARGET,    RUNLEVEL_DOWN }

        /* Note that the order here matters, as we read the
           directories in this order, and we want to make sure that
           sysv_start_priority is known when we first load the
           unit. And that value we only know from S links. Hence
           UP/SYSINIT must be read before DOWN */
};

#define RUNLEVELS_UP "12345"
/* #define RUNLEVELS_DOWN "06" */
#define RUNLEVELS_BOOT "bBsS"
#endif

static const UnitActiveState state_translation_table[_SERVICE_STATE_MAX] = {
        [SERVICE_DEAD] = UNIT_INACTIVE,
        [SERVICE_START_PRE] = UNIT_ACTIVATING,
        [SERVICE_START] = UNIT_ACTIVATING,
        [SERVICE_START_POST] = UNIT_ACTIVATING,
        [SERVICE_RUNNING] = UNIT_ACTIVE,
        [SERVICE_EXITED] = UNIT_ACTIVE,
        [SERVICE_RELOAD] = UNIT_RELOADING,
        [SERVICE_STOP] = UNIT_DEACTIVATING,
        [SERVICE_STOP_SIGTERM] = UNIT_DEACTIVATING,
        [SERVICE_STOP_SIGKILL] = UNIT_DEACTIVATING,
        [SERVICE_STOP_POST] = UNIT_DEACTIVATING,
        [SERVICE_FINAL_SIGTERM] = UNIT_DEACTIVATING,
        [SERVICE_FINAL_SIGKILL] = UNIT_DEACTIVATING,
        [SERVICE_FAILED] = UNIT_FAILED,
        [SERVICE_AUTO_RESTART] = UNIT_ACTIVATING
};

static void service_init(Unit *u) {
        Service *s = SERVICE(u);

        assert(u);
        assert(u->meta.load_state == UNIT_STUB);

        s->timeout_usec = DEFAULT_TIMEOUT_USEC;
        s->restart_usec = DEFAULT_RESTART_USEC;
        s->timer_watch.type = WATCH_INVALID;
#ifdef HAVE_SYSV_COMPAT
        s->sysv_start_priority = -1;
        s->sysv_start_priority_from_rcnd = -1;
#endif
        s->socket_fd = -1;
        s->guess_main_pid = true;

        exec_context_init(&s->exec_context);

        RATELIMIT_INIT(s->ratelimit, 10*USEC_PER_SEC, 5);

        s->control_command_id = _SERVICE_EXEC_COMMAND_INVALID;
}

static void service_unwatch_control_pid(Service *s) {
        assert(s);

        if (s->control_pid <= 0)
                return;

        unit_unwatch_pid(UNIT(s), s->control_pid);
        s->control_pid = 0;
}

static void service_unwatch_main_pid(Service *s) {
        assert(s);

        if (s->main_pid <= 0)
                return;

        unit_unwatch_pid(UNIT(s), s->main_pid);
        s->main_pid = 0;
}

static int service_set_main_pid(Service *s, pid_t pid) {
        pid_t ppid;

        assert(s);

        if (pid <= 1)
                return -EINVAL;

        if (pid == getpid())
                return -EINVAL;

        s->main_pid = pid;
        s->main_pid_known = true;

        if (get_parent_of_pid(pid, &ppid) >= 0 && ppid != getpid()) {
                log_warning("%s: Supervising process %lu which is not our child. We'll most likely not notice when it exits.",
                            s->meta.id, (unsigned long) pid);

                s->main_pid_alien = true;
        } else
                s->main_pid_alien = false;

        exec_status_start(&s->main_exec_status, pid);

        return 0;
}

static void service_close_socket_fd(Service *s) {
        assert(s);

        if (s->socket_fd < 0)
                return;

        close_nointr_nofail(s->socket_fd);
        s->socket_fd = -1;
}

static void service_connection_unref(Service *s) {
        assert(s);

        if (!s->accept_socket)
                return;

        socket_connection_unref(s->accept_socket);
        s->accept_socket = NULL;
}

static void service_done(Unit *u) {
        Service *s = SERVICE(u);

        assert(s);

        free(s->pid_file);
        s->pid_file = NULL;

#ifdef HAVE_SYSV_COMPAT
        free(s->sysv_path);
        s->sysv_path = NULL;

        free(s->sysv_runlevels);
        s->sysv_runlevels = NULL;
#endif

        free(s->status_text);
        s->status_text = NULL;

        exec_context_done(&s->exec_context);
        exec_command_free_array(s->exec_command, _SERVICE_EXEC_COMMAND_MAX);
        s->control_command = NULL;
        s->main_command = NULL;

        /* This will leak a process, but at least no memory or any of
         * our resources */
        service_unwatch_main_pid(s);
        service_unwatch_control_pid(s);

        if (s->bus_name)  {
                unit_unwatch_bus_name(UNIT(u), s->bus_name);
                free(s->bus_name);
                s->bus_name = NULL;
        }

        service_close_socket_fd(s);
        service_connection_unref(s);

        set_free(s->configured_sockets);

        unit_unwatch_timer(u, &s->timer_watch);
}

#ifdef HAVE_SYSV_COMPAT
static char *sysv_translate_name(const char *name) {
        char *r;

        if (!(r = new(char, strlen(name) + sizeof(".service"))))
                return NULL;

#if defined(TARGET_DEBIAN) || defined(TARGET_UBUNTU) || defined(TARGET_ANGSTROM)
        if (endswith(name, ".sh"))
                /* Drop Debian-style .sh suffix */
                strcpy(stpcpy(r, name) - 3, ".service");
#endif
#ifdef TARGET_SUSE
        if (startswith(name, "boot."))
                /* Drop SuSE-style boot. prefix */
                strcpy(stpcpy(r, name + 5), ".service");
#endif
#ifdef TARGET_FRUGALWARE
        if (startswith(name, "rc."))
                /* Drop Frugalware-style rc. prefix */
                strcpy(stpcpy(r, name + 3), ".service");
#endif
        else
                /* Normal init scripts */
                strcpy(stpcpy(r, name), ".service");

        return r;
}

static int sysv_translate_facility(const char *name, const char *filename, char **_r) {

        /* We silently ignore the $ prefix here. According to the LSB
         * spec it simply indicates whether something is a
         * standardized name or a distribution-specific one. Since we
         * just follow what already exists and do not introduce new
         * uses or names we don't care who introduced a new name. */

        static const char * const table[] = {
                /* LSB defined facilities */
                "local_fs",             SPECIAL_LOCAL_FS_TARGET,
#ifndef TARGET_MANDRIVA
                /* Due to unfortunate name selection in Mandriva,
                 * $network is provided by network-up which is ordered
                 * after network which actually starts interfaces.
                 * To break the loop, just ignore it */
                "network",              SPECIAL_NETWORK_TARGET,
#endif
                "named",                SPECIAL_NSS_LOOKUP_TARGET,
                "portmap",              SPECIAL_RPCBIND_TARGET,
                "remote_fs",            SPECIAL_REMOTE_FS_TARGET,
                "syslog",               SPECIAL_SYSLOG_TARGET,
                "time",                 SPECIAL_TIME_SYNC_TARGET,

                /* common extensions */
                "mail-transfer-agent",  SPECIAL_MAIL_TRANSFER_AGENT_TARGET,
                "x-display-manager",    SPECIAL_DISPLAY_MANAGER_SERVICE,
                "null",                 NULL,

#if defined(TARGET_DEBIAN) || defined(TARGET_UBUNTU) || defined(TARGET_ANGSTROM)
                "mail-transport-agent", SPECIAL_MAIL_TRANSFER_AGENT_TARGET,
#endif

#ifdef TARGET_FEDORA
                "MTA",                  SPECIAL_MAIL_TRANSFER_AGENT_TARGET,
                "smtpdaemon",           SPECIAL_MAIL_TRANSFER_AGENT_TARGET,
                "httpd",                SPECIAL_HTTP_DAEMON_TARGET,
#endif

#ifdef TARGET_SUSE
                "smtp",                 SPECIAL_MAIL_TRANSFER_AGENT_TARGET,
#endif
        };

        unsigned i;
        char *r;
        const char *n;

        assert(name);
        assert(_r);

        n = *name == '$' ? name + 1 : name;

        for (i = 0; i < ELEMENTSOF(table); i += 2) {

                if (!streq(table[i], n))
                        continue;

                if (!table[i+1])
                        return 0;

                if (!(r = strdup(table[i+1])))
                        return -ENOMEM;

                goto finish;
        }

        /* If we don't know this name, fallback heuristics to figure
         * out whether something is a target or a service alias. */

        if (*name == '$') {
                if (!unit_prefix_is_valid(n))
                        return -EINVAL;

                /* Facilities starting with $ are most likely targets */
                r = unit_name_build(n, NULL, ".target");
        } else if (filename && streq(name, filename))
                /* Names equaling the file name of the services are redundant */
                return 0;
        else
                /* Everything else we assume to be normal service names */
                r = sysv_translate_name(n);

        if (!r)
                return -ENOMEM;

finish:
        *_r = r;

        return 1;
}

static int sysv_fix_order(Service *s) {
        Meta *other;
        int r;

        assert(s);

        if (s->sysv_start_priority < 0)
                return 0;

        /* For each pair of services where at least one lacks a LSB
         * header, we use the start priority value to order things. */

        LIST_FOREACH(units_by_type, other, s->meta.manager->units_by_type[UNIT_SERVICE]) {
                Service *t;
                UnitDependency d;
                bool special_s, special_t;

                t = (Service*) other;

                if (s == t)
                        continue;

                if (t->meta.load_state != UNIT_LOADED)
                        continue;

                if (t->sysv_start_priority < 0)
                        continue;

                /* If both units have modern headers we don't care
                 * about the priorities */
                if ((s->meta.fragment_path || s->sysv_has_lsb) &&
                    (t->meta.fragment_path || t->sysv_has_lsb))
                        continue;

                special_s = s->sysv_runlevels && !chars_intersect(RUNLEVELS_UP, s->sysv_runlevels);
                special_t = t->sysv_runlevels && !chars_intersect(RUNLEVELS_UP, t->sysv_runlevels);

                if (special_t && !special_s)
                        d = UNIT_AFTER;
                else if (special_s && !special_t)
                        d = UNIT_BEFORE;
                else if (t->sysv_start_priority < s->sysv_start_priority)
                        d = UNIT_AFTER;
                else if (t->sysv_start_priority > s->sysv_start_priority)
                        d = UNIT_BEFORE;
                else
                        continue;

                /* FIXME: Maybe we should compare the name here lexicographically? */

                if ((r = unit_add_dependency(UNIT(s), d, UNIT(t), true)) < 0)
                        return r;
        }

        return 0;
}

static ExecCommand *exec_command_new(const char *path, const char *arg1) {
        ExecCommand *c;

        if (!(c = new0(ExecCommand, 1)))
                return NULL;

        if (!(c->path = strdup(path))) {
                free(c);
                return NULL;
        }

        if (!(c->argv = strv_new(path, arg1, NULL))) {
                free(c->path);
                free(c);
                return NULL;
        }

        return c;
}

static int sysv_exec_commands(Service *s) {
        ExecCommand *c;

        assert(s);
        assert(s->sysv_path);

        if (!(c = exec_command_new(s->sysv_path, "start")))
                return -ENOMEM;
        exec_command_append_list(s->exec_command+SERVICE_EXEC_START, c);

        if (!(c = exec_command_new(s->sysv_path, "stop")))
                return -ENOMEM;
        exec_command_append_list(s->exec_command+SERVICE_EXEC_STOP, c);

        if (!(c = exec_command_new(s->sysv_path, "reload")))
                return -ENOMEM;
        exec_command_append_list(s->exec_command+SERVICE_EXEC_RELOAD, c);

        return 0;
}

static int service_load_sysv_path(Service *s, const char *path) {
        FILE *f;
        Unit *u;
        unsigned line = 0;
        int r;
        enum {
                NORMAL,
                DESCRIPTION,
                LSB,
                LSB_DESCRIPTION
        } state = NORMAL;
        char *short_description = NULL, *long_description = NULL, *chkconfig_description = NULL, *description;
        struct stat st;

        assert(s);
        assert(path);

        u = UNIT(s);

        if (!(f = fopen(path, "re"))) {
                r = errno == ENOENT ? 0 : -errno;
                goto finish;
        }

        zero(st);
        if (fstat(fileno(f), &st) < 0) {
                r = -errno;
                goto finish;
        }

        free(s->sysv_path);
        if (!(s->sysv_path = strdup(path))) {
                r = -ENOMEM;
                goto finish;
        }

        s->sysv_mtime = timespec_load(&st.st_mtim);

        if (null_or_empty(&st)) {
                u->meta.load_state = UNIT_MASKED;
                r = 0;
                goto finish;
        }

        while (!feof(f)) {
                char l[LINE_MAX], *t;

                if (!fgets(l, sizeof(l), f)) {
                        if (feof(f))
                                break;

                        r = -errno;
                        log_error("Failed to read configuration file '%s': %s", path, strerror(-r));
                        goto finish;
                }

                line++;

                t = strstrip(l);
                if (*t != '#')
                        continue;

                if (state == NORMAL && streq(t, "### BEGIN INIT INFO")) {
                        state = LSB;
                        s->sysv_has_lsb = true;
                        continue;
                }

                if ((state == LSB_DESCRIPTION || state == LSB) && streq(t, "### END INIT INFO")) {
                        state = NORMAL;
                        continue;
                }

                t++;
                t += strspn(t, WHITESPACE);

                if (state == NORMAL) {

                        /* Try to parse Red Hat style chkconfig headers */

                        if (startswith_no_case(t, "chkconfig:")) {
                                int start_priority;
                                char runlevels[16], *k;

                                state = NORMAL;

                                if (sscanf(t+10, "%15s %i %*i",
                                           runlevels,
                                           &start_priority) != 2) {

                                        log_warning("[%s:%u] Failed to parse chkconfig line. Ignoring.", path, line);
                                        continue;
                                }

                                /* A start priority gathered from the
                                 * symlink farms is preferred over the
                                 * data from the LSB header. */
                                if (start_priority < 0 || start_priority > 99)
                                        log_warning("[%s:%u] Start priority out of range. Ignoring.", path, line);
                                else
                                        s->sysv_start_priority = start_priority;

                                char_array_0(runlevels);
                                k = delete_chars(runlevels, WHITESPACE "-");

                                if (k[0]) {
                                        char *d;

                                        if (!(d = strdup(k))) {
                                                r = -ENOMEM;
                                                goto finish;
                                        }

                                        free(s->sysv_runlevels);
                                        s->sysv_runlevels = d;
                                }

                        } else if (startswith_no_case(t, "description:")) {

                                size_t k = strlen(t);
                                char *d;
                                const char *j;

                                if (t[k-1] == '\\') {
                                        state = DESCRIPTION;
                                        t[k-1] = 0;
                                }

                                if ((j = strstrip(t+12)) && *j) {
                                        if (!(d = strdup(j))) {
                                                r = -ENOMEM;
                                                goto finish;
                                        }
                                } else
                                        d = NULL;

                                free(chkconfig_description);
                                chkconfig_description = d;

                        } else if (startswith_no_case(t, "pidfile:")) {

                                char *fn;

                                state = NORMAL;

                                fn = strstrip(t+8);
                                if (!path_is_absolute(fn)) {
                                        log_warning("[%s:%u] PID file not absolute. Ignoring.", path, line);
                                        continue;
                                }

                                if (!(fn = strdup(fn))) {
                                        r = -ENOMEM;
                                        goto finish;
                                }

                                free(s->pid_file);
                                s->pid_file = fn;
                        }

                } else if (state == DESCRIPTION) {

                        /* Try to parse Red Hat style description
                         * continuation */

                        size_t k = strlen(t);
                        char *j;

                        if (t[k-1] == '\\')
                                t[k-1] = 0;
                        else
                                state = NORMAL;

                        if ((j = strstrip(t)) && *j) {
                                char *d = NULL;

                                if (chkconfig_description)
                                        d = join(chkconfig_description, " ", j, NULL);
                                else
                                        d = strdup(j);

                                if (!d) {
                                        r = -ENOMEM;
                                        goto finish;
                                }

                                free(chkconfig_description);
                                chkconfig_description = d;
                        }

                } else if (state == LSB || state == LSB_DESCRIPTION) {

                        if (startswith_no_case(t, "Provides:")) {
                                char *i, *w;
                                size_t z;

                                state = LSB;

                                FOREACH_WORD_QUOTED(w, z, t+9, i) {
                                        char *n, *m;

                                        if (!(n = strndup(w, z))) {
                                                r = -ENOMEM;
                                                goto finish;
                                        }

                                        r = sysv_translate_facility(n, file_name_from_path(path), &m);
                                        free(n);

                                        if (r < 0)
                                                goto finish;

                                        if (r == 0)
                                                continue;

                                        if (unit_name_to_type(m) == UNIT_SERVICE)
                                                r = unit_add_name(u, m);
                                        else
                                                /* NB: SysV targets
                                                 * which are provided
                                                 * by a service are
                                                 * pulled in by the
                                                 * services, as an
                                                 * indication that the
                                                 * generic service is
                                                 * now available. This
                                                 * is strictly
                                                 * one-way. The
                                                 * targets do NOT pull
                                                 * in the SysV
                                                 * services! */
                                                r = unit_add_two_dependencies_by_name(u, UNIT_BEFORE, UNIT_WANTS, m, NULL, true);

                                        if (r < 0)
                                                log_error("[%s:%u] Failed to add LSB Provides name %s, ignoring: %s", path, line, m, strerror(-r));

                                        free(m);
                                }

                        } else if (startswith_no_case(t, "Required-Start:") ||
                                   startswith_no_case(t, "Should-Start:") ||
                                   startswith_no_case(t, "X-Start-Before:") ||
                                   startswith_no_case(t, "X-Start-After:")) {
                                char *i, *w;
                                size_t z;

                                state = LSB;

                                FOREACH_WORD_QUOTED(w, z, strchr(t, ':')+1, i) {
                                        char *n, *m;

                                        if (!(n = strndup(w, z))) {
                                                r = -ENOMEM;
                                                goto finish;
                                        }

                                        r = sysv_translate_facility(n, file_name_from_path(path), &m);

                                        if (r < 0) {
                                                log_error("[%s:%u] Failed to translate LSB dependency %s, ignoring: %s", path, line, n, strerror(-r));
                                                free(n);
                                                continue;
                                        }

                                        free(n);

                                        if (r == 0)
                                                continue;

                                        r = unit_add_dependency_by_name(u, startswith_no_case(t, "X-Start-Before:") ? UNIT_BEFORE : UNIT_AFTER, m, NULL, true);

                                        if (r < 0)
                                                log_error("[%s:%u] Failed to add dependency on %s, ignoring: %s", path, line, m, strerror(-r));

                                        free(m);
                                }
                        } else if (startswith_no_case(t, "Default-Start:")) {
                                char *k, *d;

                                state = LSB;

                                k = delete_chars(t+14, WHITESPACE "-");

                                if (k[0] != 0) {
                                        if (!(d = strdup(k))) {
                                                r = -ENOMEM;
                                                goto finish;
                                        }

                                        free(s->sysv_runlevels);
                                        s->sysv_runlevels = d;
                                }

                        } else if (startswith_no_case(t, "Description:")) {
                                char *d, *j;

                                state = LSB_DESCRIPTION;

                                if ((j = strstrip(t+12)) && *j) {
                                        if (!(d = strdup(j))) {
                                                r = -ENOMEM;
                                                goto finish;
                                        }
                                } else
                                        d = NULL;

                                free(long_description);
                                long_description = d;

                        } else if (startswith_no_case(t, "Short-Description:")) {
                                char *d, *j;

                                state = LSB;

                                if ((j = strstrip(t+18)) && *j) {
                                        if (!(d = strdup(j))) {
                                                r = -ENOMEM;
                                                goto finish;
                                        }
                                } else
                                        d = NULL;

                                free(short_description);
                                short_description = d;

                        } else if (state == LSB_DESCRIPTION) {

                                if (startswith(l, "#\t") || startswith(l, "#  ")) {
                                        char *j;

                                        if ((j = strstrip(t)) && *j) {
                                                char *d = NULL;

                                                if (long_description)
                                                        d = join(long_description, " ", t, NULL);
                                                else
                                                        d = strdup(j);

                                                if (!d) {
                                                        r = -ENOMEM;
                                                        goto finish;
                                                }

                                                free(long_description);
                                                long_description = d;
                                        }

                                } else
                                        state = LSB;
                        }
                }
        }

        if ((r = sysv_exec_commands(s)) < 0)
                goto finish;
        if (s->sysv_runlevels &&
            chars_intersect(RUNLEVELS_BOOT, s->sysv_runlevels) &&
            chars_intersect(RUNLEVELS_UP, s->sysv_runlevels)) {
                /* Service has both boot and "up" runlevels
                   configured.  Kill the "up" ones. */
                delete_chars(s->sysv_runlevels, RUNLEVELS_UP);
        }

        if (s->sysv_runlevels && !chars_intersect(RUNLEVELS_UP, s->sysv_runlevels)) {
                /* If there a runlevels configured for this service
                 * but none of the standard ones, then we assume this
                 * is some special kind of service (which might be
                 * needed for early boot) and don't create any links
                 * to it. */

                s->meta.default_dependencies = false;

                /* Don't timeout special services during boot (like fsck) */
                s->timeout_usec = 0;
        } else
                s->timeout_usec = DEFAULT_SYSV_TIMEOUT_USEC;

        /* Special setting for all SysV services */
        s->type = SERVICE_FORKING;
        s->remain_after_exit = !s->pid_file;
        s->guess_main_pid = false;
        s->restart = SERVICE_RESTART_NO;

        if (s->meta.manager->sysv_console)
                s->exec_context.std_output = EXEC_OUTPUT_TTY;

        s->exec_context.kill_mode = KILL_PROCESS;

        /* We use the long description only if
         * no short description is set. */

        if (short_description)
                description = short_description;
        else if (chkconfig_description)
                description = chkconfig_description;
        else if (long_description)
                description = long_description;
        else
                description = NULL;

        if (description) {
                char *d;

                if (!(d = strappend(s->sysv_has_lsb ? "LSB: " : "SYSV: ", description))) {
                        r = -ENOMEM;
                        goto finish;
                }

                u->meta.description = d;
        }

        /* The priority that has been set in /etc/rcN.d/ hierarchies
         * takes precedence over what is stored as default in the LSB
         * header */
        if (s->sysv_start_priority_from_rcnd >= 0)
                s->sysv_start_priority = s->sysv_start_priority_from_rcnd;

        u->meta.load_state = UNIT_LOADED;
        r = 0;

finish:

        if (f)
                fclose(f);

        free(short_description);
        free(long_description);
        free(chkconfig_description);

        return r;
}

static int service_load_sysv_name(Service *s, const char *name) {
        char **p;

        assert(s);
        assert(name);

        /* For SysV services we strip the boot.*, rc.* and *.sh
         * prefixes/suffixes. */
#if defined(TARGET_DEBIAN) || defined(TARGET_UBUNTU) || defined(TARGET_ANGSTROM)
        if (endswith(name, ".sh.service"))
                return -ENOENT;
#endif

#ifdef TARGET_SUSE
        if (startswith(name, "boot."))
                return -ENOENT;
#endif

#ifdef TARGET_FRUGALWARE
        if (startswith(name, "rc."))
                return -ENOENT;
#endif

        STRV_FOREACH(p, s->meta.manager->lookup_paths.sysvinit_path) {
                char *path;
                int r;

                path = join(*p, "/", name, NULL);
                if (!path)
                        return -ENOMEM;

                assert(endswith(path, ".service"));
                path[strlen(path)-8] = 0;

                r = service_load_sysv_path(s, path);

#if defined(TARGET_DEBIAN) || defined(TARGET_UBUNTU) || defined(TARGET_ANGSTROM)
                if (r >= 0 && s->meta.load_state == UNIT_STUB) {
                        /* Try Debian style *.sh source'able init scripts */
                        strcat(path, ".sh");
                        r = service_load_sysv_path(s, path);
                }
#endif
                free(path);

#ifdef TARGET_SUSE
                if (r >= 0 && s->meta.load_state == UNIT_STUB) {
                        /* Try SUSE style boot.* init scripts */

                        path = join(*p, "/boot.", name, NULL);
                        if (!path)
                                return -ENOMEM;

                        /* Drop .service suffix */
                        path[strlen(path)-8] = 0;
                        r = service_load_sysv_path(s, path);
                        free(path);
                }
#endif

#ifdef TARGET_FRUGALWARE
                if (r >= 0 && s->meta.load_state == UNIT_STUB) {
                        /* Try Frugalware style rc.* init scripts */

                        path = join(*p, "/rc.", name, NULL);
                        if (!path)
                                return -ENOMEM;

                        /* Drop .service suffix */
                        path[strlen(path)-8] = 0;
                        r = service_load_sysv_path(s, path);
                        free(path);
                }
#endif

                if (r < 0)
                        return r;

                if ((s->meta.load_state != UNIT_STUB))
                        break;
        }

        return 0;
}

static int service_load_sysv(Service *s) {
        const char *t;
        Iterator i;
        int r;

        assert(s);

        /* Load service data from SysV init scripts, preferably with
         * LSB headers ... */

        if (strv_isempty(s->meta.manager->lookup_paths.sysvinit_path))
                return 0;

        if ((t = s->meta.id))
                if ((r = service_load_sysv_name(s, t)) < 0)
                        return r;

        if (s->meta.load_state == UNIT_STUB)
                SET_FOREACH(t, s->meta.names, i) {
                        if (t == s->meta.id)
                                continue;

                        if ((r = service_load_sysv_name(s, t)) < 0)
                                return r;

                        if (s->meta.load_state != UNIT_STUB)
                                break;
                }

        return 0;
}
#endif

static int fsck_fix_order(Service *s) {
        Meta *other;
        int r;

        assert(s);

        if (s->fsck_passno <= 0)
                return 0;

        /* For each pair of services where both have an fsck priority
         * we order things based on it. */

        LIST_FOREACH(units_by_type, other, s->meta.manager->units_by_type[UNIT_SERVICE]) {
                Service *t;
                UnitDependency d;

                t = (Service*) other;

                if (s == t)
                        continue;

                if (t->meta.load_state != UNIT_LOADED)
                        continue;

                if (t->fsck_passno <= 0)
                        continue;

                if (t->fsck_passno < s->fsck_passno)
                        d = UNIT_AFTER;
                else if (t->fsck_passno > s->fsck_passno)
                        d = UNIT_BEFORE;
                else
                        continue;

                if ((r = unit_add_dependency(UNIT(s), d, UNIT(t), true)) < 0)
                        return r;
        }

        return 0;
}

static int service_verify(Service *s) {
        assert(s);

        if (s->meta.load_state != UNIT_LOADED)
                return 0;

        if (!s->exec_command[SERVICE_EXEC_START]) {
                log_error("%s lacks ExecStart setting. Refusing.", s->meta.id);
                return -EINVAL;
        }

        if (s->type != SERVICE_ONESHOT &&
            s->exec_command[SERVICE_EXEC_START]->command_next) {
                log_error("%s has more than one ExecStart setting, which is only allowed for Type=oneshot services. Refusing.", s->meta.id);
                return -EINVAL;
        }

        if (s->type == SERVICE_ONESHOT &&
            s->exec_command[SERVICE_EXEC_RELOAD]) {
                log_error("%s has an ExecReload setting, which is not allowed for Type=oneshot services. Refusing.", s->meta.id);
                return -EINVAL;
        }

        if (s->type == SERVICE_DBUS && !s->bus_name) {
                log_error("%s is of type D-Bus but no D-Bus service name has been specified. Refusing.", s->meta.id);
                return -EINVAL;
        }

        if (s->exec_context.pam_name && s->exec_context.kill_mode != KILL_CONTROL_GROUP) {
                log_error("%s has PAM enabled. Kill mode must be set to 'control-group'. Refusing.", s->meta.id);
                return -EINVAL;
        }

        return 0;
}

static int service_add_default_dependencies(Service *s) {
        int r;

        assert(s);

        /* Add a number of automatic dependencies useful for the
         * majority of services. */

        /* First, pull in base system */
        if (s->meta.manager->running_as == MANAGER_SYSTEM) {

                if ((r = unit_add_two_dependencies_by_name(UNIT(s), UNIT_AFTER, UNIT_REQUIRES, SPECIAL_BASIC_TARGET, NULL, true)) < 0)
                        return r;

        } else if (s->meta.manager->running_as == MANAGER_USER) {

                if ((r = unit_add_two_dependencies_by_name(UNIT(s), UNIT_AFTER, UNIT_REQUIRES, SPECIAL_SOCKETS_TARGET, NULL, true)) < 0)
                        return r;
        }

        /* Second, activate normal shutdown */
        return unit_add_two_dependencies_by_name(UNIT(s), UNIT_BEFORE, UNIT_CONFLICTS, SPECIAL_SHUTDOWN_TARGET, NULL, true);
}

static void service_fix_output(Service *s) {
        assert(s);

        /* If nothing has been explicitly configured, patch default
         * output in. If input is socket/tty we avoid this however,
         * since in that case we want output to default to the same
         * place as we read input from. */

        if (s->exec_context.std_error == EXEC_OUTPUT_INHERIT &&
            s->exec_context.std_output == EXEC_OUTPUT_INHERIT &&
            s->exec_context.std_input == EXEC_INPUT_NULL)
                s->exec_context.std_error = s->meta.manager->default_std_error;

        if (s->exec_context.std_output == EXEC_OUTPUT_INHERIT &&
            s->exec_context.std_input == EXEC_INPUT_NULL)
                s->exec_context.std_output = s->meta.manager->default_std_output;
}

static int service_load(Unit *u) {
        int r;
        Service *s = SERVICE(u);

        assert(s);

        /* Load a .service file */
        if ((r = unit_load_fragment(u)) < 0)
                return r;

#ifdef HAVE_SYSV_COMPAT
        /* Load a classic init script as a fallback, if we couldn't find anything */
        if (u->meta.load_state == UNIT_STUB)
                if ((r = service_load_sysv(s)) < 0)
                        return r;
#endif

        /* Still nothing found? Then let's give up */
        if (u->meta.load_state == UNIT_STUB)
                return -ENOENT;

        /* We were able to load something, then let's add in the
         * dropin directories. */
        if ((r = unit_load_dropin(unit_follow_merge(u))) < 0)
                return r;

        /* This is a new unit? Then let's add in some extras */
        if (u->meta.load_state == UNIT_LOADED) {
                service_fix_output(s);

                if ((r = unit_add_exec_dependencies(u, &s->exec_context)) < 0)
                        return r;

                if ((r = unit_add_default_cgroups(u)) < 0)
                        return r;

#ifdef HAVE_SYSV_COMPAT
                if ((r = sysv_fix_order(s)) < 0)
                        return r;
#endif

                if ((r = fsck_fix_order(s)) < 0)
                        return r;

                if (s->bus_name)
                        if ((r = unit_watch_bus_name(u, s->bus_name)) < 0)
                                return r;

                if (s->type == SERVICE_NOTIFY && s->notify_access == NOTIFY_NONE)
                        s->notify_access = NOTIFY_MAIN;

                if (s->type == SERVICE_DBUS || s->bus_name)
                        if ((r = unit_add_two_dependencies_by_name(u, UNIT_AFTER, UNIT_REQUIRES, SPECIAL_DBUS_SOCKET, NULL, true)) < 0)
                                return r;

                if (s->meta.default_dependencies)
                        if ((r = service_add_default_dependencies(s)) < 0)
                                return r;
        }

        return service_verify(s);
}

static void service_dump(Unit *u, FILE *f, const char *prefix) {

        ServiceExecCommand c;
        Service *s = SERVICE(u);
        const char *prefix2;
        char *p2;

        assert(s);

        p2 = strappend(prefix, "\t");
        prefix2 = p2 ? p2 : prefix;

        fprintf(f,
                "%sService State: %s\n"
                "%sPermissionsStartOnly: %s\n"
                "%sRootDirectoryStartOnly: %s\n"
                "%sRemainAfterExit: %s\n"
                "%sGuessMainPID: %s\n"
                "%sType: %s\n"
                "%sRestart: %s\n"
                "%sNotifyAccess: %s\n",
                prefix, service_state_to_string(s->state),
                prefix, yes_no(s->permissions_start_only),
                prefix, yes_no(s->root_directory_start_only),
                prefix, yes_no(s->remain_after_exit),
                prefix, yes_no(s->guess_main_pid),
                prefix, service_type_to_string(s->type),
                prefix, service_restart_to_string(s->restart),
                prefix, notify_access_to_string(s->notify_access));

        if (s->control_pid > 0)
                fprintf(f,
                        "%sControl PID: %lu\n",
                        prefix, (unsigned long) s->control_pid);

        if (s->main_pid > 0)
                fprintf(f,
                        "%sMain PID: %lu\n"
                        "%sMain PID Known: %s\n"
                        "%sMain PID Alien: %s\n",
                        prefix, (unsigned long) s->main_pid,
                        prefix, yes_no(s->main_pid_known),
                        prefix, yes_no(s->main_pid_alien));

        if (s->pid_file)
                fprintf(f,
                        "%sPIDFile: %s\n",
                        prefix, s->pid_file);

        if (s->bus_name)
                fprintf(f,
                        "%sBusName: %s\n"
                        "%sBus Name Good: %s\n",
                        prefix, s->bus_name,
                        prefix, yes_no(s->bus_name_good));

        exec_context_dump(&s->exec_context, f, prefix);

        for (c = 0; c < _SERVICE_EXEC_COMMAND_MAX; c++) {

                if (!s->exec_command[c])
                        continue;

                fprintf(f, "%s-> %s:\n",
                        prefix, service_exec_command_to_string(c));

                exec_command_dump_list(s->exec_command[c], f, prefix2);
        }

#ifdef HAVE_SYSV_COMPAT
        if (s->sysv_path)
                fprintf(f,
                        "%sSysV Init Script Path: %s\n"
                        "%sSysV Init Script has LSB Header: %s\n"
                        "%sSysVEnabled: %s\n",
                        prefix, s->sysv_path,
                        prefix, yes_no(s->sysv_has_lsb),
                        prefix, yes_no(s->sysv_enabled));

        if (s->sysv_start_priority >= 0)
                fprintf(f,
                        "%sSysVStartPriority: %i\n",
                        prefix, s->sysv_start_priority);

        if (s->sysv_runlevels)
                fprintf(f, "%sSysVRunLevels: %s\n",
                        prefix, s->sysv_runlevels);
#endif

        if (s->fsck_passno > 0)
                fprintf(f,
                        "%sFsckPassNo: %i\n",
                        prefix, s->fsck_passno);

        if (s->status_text)
                fprintf(f, "%sStatus Text: %s\n",
                        prefix, s->status_text);

        free(p2);
}

static int service_load_pid_file(Service *s, bool warn_if_missing) {
        char *k;
        int r;
        pid_t pid;

        assert(s);

        if (!s->pid_file)
                return -ENOENT;

        if ((r = read_one_line_file(s->pid_file, &k)) < 0) {
                if (warn_if_missing)
                        log_warning("Failed to read PID file %s after %s. The service might be broken.",
                                    s->pid_file, service_state_to_string(s->state));
                return r;
        }

        r = parse_pid(k, &pid);
        free(k);

        if (r < 0)
                return r;

        if (kill(pid, 0) < 0 && errno != EPERM) {
                log_warning("PID %lu read from file %s does not exist. Your service or init script might be broken.",
                            (unsigned long) pid, s->pid_file);
                return -ESRCH;
        }

        if (s->main_pid_known) {
                if (pid == s->main_pid)
                        return 0;

                log_debug("Main PID changing: %lu -> %lu",
                          (unsigned long) s->main_pid, (unsigned long) pid);
                service_unwatch_main_pid(s);
                s->main_pid_known = false;
        }

        if ((r = service_set_main_pid(s, pid)) < 0)
                return r;

        if ((r = unit_watch_pid(UNIT(s), pid)) < 0)
                /* FIXME: we need to do something here */
                return r;

        return 0;
}

static int service_search_main_pid(Service *s) {
        pid_t pid;
        int r;

        assert(s);

        /* If we know it anyway, don't ever fallback to unreliable
         * heuristics */
        if (s->main_pid_known)
                return 0;

        if (!s->guess_main_pid)
                return 0;

        assert(s->main_pid <= 0);

        if ((pid = cgroup_bonding_search_main_pid_list(s->meta.cgroup_bondings)) <= 0)
                return -ENOENT;

        if ((r = service_set_main_pid(s, pid)) < 0)
                return r;

        if ((r = unit_watch_pid(UNIT(s), pid)) < 0)
                /* FIXME: we need to do something here */
                return r;

        return 0;
}

static int service_get_sockets(Service *s, Set **_set) {
        Set *set;
        Iterator i;
        char *t;
        int r;

        assert(s);
        assert(_set);

        if (s->socket_fd >= 0)
                return 0;

        if (!set_isempty(s->configured_sockets))
                return 0;

        /* Collects all Socket objects that belong to this
         * service. Note that a service might have multiple sockets
         * via multiple names. */

        if (!(set = set_new(NULL, NULL)))
                return -ENOMEM;

        SET_FOREACH(t, s->meta.names, i) {
                char *k;
                Unit *p;

                /* Look for all socket objects that go by any of our
                 * units and collect their fds */

                if (!(k = unit_name_change_suffix(t, ".socket"))) {
                        r = -ENOMEM;
                        goto fail;
                }

                p = manager_get_unit(s->meta.manager, k);
                free(k);

                if (!p)
                        continue;

                if ((r = set_put(set, p)) < 0)
                        goto fail;
        }

        *_set = set;
        return 0;

fail:
        set_free(set);
        return r;
}

static int service_notify_sockets_dead(Service *s) {
        Iterator i;
        Set *set, *free_set = NULL;
        Socket *sock;
        int r;

        assert(s);

        /* Notifies all our sockets when we die */

        if (s->socket_fd >= 0)
                return 0;

        if (!set_isempty(s->configured_sockets))
                set = s->configured_sockets;
        else {
                if ((r = service_get_sockets(s, &free_set)) < 0)
                        return r;

                set = free_set;
        }

        SET_FOREACH(sock, set, i)
                socket_notify_service_dead(sock);

        set_free(free_set);

        return 0;
}

static void service_set_state(Service *s, ServiceState state) {
        ServiceState old_state;
        assert(s);

        old_state = s->state;
        s->state = state;

        if (state != SERVICE_START_PRE &&
            state != SERVICE_START &&
            state != SERVICE_START_POST &&
            state != SERVICE_RELOAD &&
            state != SERVICE_STOP &&
            state != SERVICE_STOP_SIGTERM &&
            state != SERVICE_STOP_SIGKILL &&
            state != SERVICE_STOP_POST &&
            state != SERVICE_FINAL_SIGTERM &&
            state != SERVICE_FINAL_SIGKILL &&
            state != SERVICE_AUTO_RESTART)
                unit_unwatch_timer(UNIT(s), &s->timer_watch);

        if (state != SERVICE_START &&
            state != SERVICE_START_POST &&
            state != SERVICE_RUNNING &&
            state != SERVICE_RELOAD &&
            state != SERVICE_STOP &&
            state != SERVICE_STOP_SIGTERM &&
            state != SERVICE_STOP_SIGKILL) {
                service_unwatch_main_pid(s);
                s->main_command = NULL;
        }

        if (state != SERVICE_START_PRE &&
            state != SERVICE_START &&
            state != SERVICE_START_POST &&
            state != SERVICE_RELOAD &&
            state != SERVICE_STOP &&
            state != SERVICE_STOP_SIGTERM &&
            state != SERVICE_STOP_SIGKILL &&
            state != SERVICE_STOP_POST &&
            state != SERVICE_FINAL_SIGTERM &&
            state != SERVICE_FINAL_SIGKILL) {
                service_unwatch_control_pid(s);
                s->control_command = NULL;
                s->control_command_id = _SERVICE_EXEC_COMMAND_INVALID;
        }

        if (state == SERVICE_DEAD ||
            state == SERVICE_STOP ||
            state == SERVICE_STOP_SIGTERM ||
            state == SERVICE_STOP_SIGKILL ||
            state == SERVICE_STOP_POST ||
            state == SERVICE_FINAL_SIGTERM ||
            state == SERVICE_FINAL_SIGKILL ||
            state == SERVICE_FAILED ||
            state == SERVICE_AUTO_RESTART)
                service_notify_sockets_dead(s);

        if (state != SERVICE_START_PRE &&
            state != SERVICE_START &&
            state != SERVICE_START_POST &&
            state != SERVICE_RUNNING &&
            state != SERVICE_RELOAD &&
            state != SERVICE_STOP &&
            state != SERVICE_STOP_SIGTERM &&
            state != SERVICE_STOP_SIGKILL &&
            state != SERVICE_STOP_POST &&
            state != SERVICE_FINAL_SIGTERM &&
            state != SERVICE_FINAL_SIGKILL &&
            !(state == SERVICE_DEAD && s->meta.job)) {
                service_close_socket_fd(s);
                service_connection_unref(s);
        }

        /* For the inactive states unit_notify() will trim the cgroup,
         * but for exit we have to do that ourselves... */
        if (state == SERVICE_EXITED && s->meta.manager->n_reloading <= 0)
                cgroup_bonding_trim_list(s->meta.cgroup_bondings, true);

        if (old_state != state)
                log_debug("%s changed %s -> %s", s->meta.id, service_state_to_string(old_state), service_state_to_string(state));

        unit_notify(UNIT(s), state_translation_table[old_state], state_translation_table[state], !s->reload_failure);
        s->reload_failure = false;
}

static int service_coldplug(Unit *u) {
        Service *s = SERVICE(u);
        int r;

        assert(s);
        assert(s->state == SERVICE_DEAD);

        if (s->deserialized_state != s->state) {

                if (s->deserialized_state == SERVICE_START_PRE ||
                    s->deserialized_state == SERVICE_START ||
                    s->deserialized_state == SERVICE_START_POST ||
                    s->deserialized_state == SERVICE_RELOAD ||
                    s->deserialized_state == SERVICE_STOP ||
                    s->deserialized_state == SERVICE_STOP_SIGTERM ||
                    s->deserialized_state == SERVICE_STOP_SIGKILL ||
                    s->deserialized_state == SERVICE_STOP_POST ||
                    s->deserialized_state == SERVICE_FINAL_SIGTERM ||
                    s->deserialized_state == SERVICE_FINAL_SIGKILL ||
                    s->deserialized_state == SERVICE_AUTO_RESTART) {

                        if (s->deserialized_state == SERVICE_AUTO_RESTART || s->timeout_usec > 0) {
                                usec_t k;

                                k = s->deserialized_state == SERVICE_AUTO_RESTART ? s->restart_usec : s->timeout_usec;

                                if ((r = unit_watch_timer(UNIT(s), k, &s->timer_watch)) < 0)
                                        return r;
                        }
                }

                if ((s->deserialized_state == SERVICE_START &&
                     (s->type == SERVICE_FORKING ||
                      s->type == SERVICE_DBUS ||
                      s->type == SERVICE_ONESHOT ||
                      s->type == SERVICE_NOTIFY)) ||
                    s->deserialized_state == SERVICE_START_POST ||
                    s->deserialized_state == SERVICE_RUNNING ||
                    s->deserialized_state == SERVICE_RELOAD ||
                    s->deserialized_state == SERVICE_STOP ||
                    s->deserialized_state == SERVICE_STOP_SIGTERM ||
                    s->deserialized_state == SERVICE_STOP_SIGKILL)
                        if (s->main_pid > 0)
                                if ((r = unit_watch_pid(UNIT(s), s->main_pid)) < 0)
                                        return r;

                if (s->deserialized_state == SERVICE_START_PRE ||
                    s->deserialized_state == SERVICE_START ||
                    s->deserialized_state == SERVICE_START_POST ||
                    s->deserialized_state == SERVICE_RELOAD ||
                    s->deserialized_state == SERVICE_STOP ||
                    s->deserialized_state == SERVICE_STOP_SIGTERM ||
                    s->deserialized_state == SERVICE_STOP_SIGKILL ||
                    s->deserialized_state == SERVICE_STOP_POST ||
                    s->deserialized_state == SERVICE_FINAL_SIGTERM ||
                    s->deserialized_state == SERVICE_FINAL_SIGKILL)
                        if (s->control_pid > 0)
                                if ((r = unit_watch_pid(UNIT(s), s->control_pid)) < 0)
                                        return r;

                service_set_state(s, s->deserialized_state);
        }

        return 0;
}

static int service_collect_fds(Service *s, int **fds, unsigned *n_fds) {
        Iterator i;
        int r;
        int *rfds = NULL;
        unsigned rn_fds = 0;
        Set *set, *free_set = NULL;
        Socket *sock;

        assert(s);
        assert(fds);
        assert(n_fds);

        if (s->socket_fd >= 0)
                return 0;

        if (!set_isempty(s->configured_sockets))
                set = s->configured_sockets;
        else {
                if ((r = service_get_sockets(s, &free_set)) < 0)
                        return r;

                set = free_set;
        }

        SET_FOREACH(sock, set, i) {
                int *cfds;
                unsigned cn_fds;

                if ((r = socket_collect_fds(sock, &cfds, &cn_fds)) < 0)
                        goto fail;

                if (!cfds)
                        continue;

                if (!rfds) {
                        rfds = cfds;
                        rn_fds = cn_fds;
                } else {
                        int *t;

                        if (!(t = new(int, rn_fds+cn_fds))) {
                                free(cfds);
                                r = -ENOMEM;
                                goto fail;
                        }

                        memcpy(t, rfds, rn_fds * sizeof(int));
                        memcpy(t+rn_fds, cfds, cn_fds * sizeof(int));
                        free(rfds);
                        free(cfds);

                        rfds = t;
                        rn_fds = rn_fds+cn_fds;
                }
        }

        *fds = rfds;
        *n_fds = rn_fds;

        set_free(free_set);

        return 0;

fail:
        set_free(set);
        free(rfds);

        return r;
}

static int service_spawn(
                Service *s,
                ExecCommand *c,
                bool timeout,
                bool pass_fds,
                bool apply_permissions,
                bool apply_chroot,
                bool apply_tty_stdin,
                bool set_notify_socket,
                pid_t *_pid) {

        pid_t pid;
        int r;
        int *fds = NULL, *fdsbuf = NULL;
        unsigned n_fds = 0, n_env = 0;
        char **argv = NULL, **final_env = NULL, **our_env = NULL;

        assert(s);
        assert(c);
        assert(_pid);

        if (pass_fds ||
            s->exec_context.std_input == EXEC_INPUT_SOCKET ||
            s->exec_context.std_output == EXEC_OUTPUT_SOCKET ||
            s->exec_context.std_error == EXEC_OUTPUT_SOCKET) {

                if (s->socket_fd >= 0) {
                        fds = &s->socket_fd;
                        n_fds = 1;
                } else {
                        if ((r = service_collect_fds(s, &fdsbuf, &n_fds)) < 0)
                                goto fail;

                        fds = fdsbuf;
                }
        }

        if (timeout && s->timeout_usec) {
                if ((r = unit_watch_timer(UNIT(s), s->timeout_usec, &s->timer_watch)) < 0)
                        goto fail;
        } else
                unit_unwatch_timer(UNIT(s), &s->timer_watch);

        if (!(argv = unit_full_printf_strv(UNIT(s), c->argv))) {
                r = -ENOMEM;
                goto fail;
        }

        if (!(our_env = new0(char*, 4))) {
                r = -ENOMEM;
                goto fail;
        }

        if (set_notify_socket)
                if (asprintf(our_env + n_env++, "NOTIFY_SOCKET=%s", s->meta.manager->notify_socket) < 0) {
                        r = -ENOMEM;
                        goto fail;
                }

        if (s->main_pid > 0)
                if (asprintf(our_env + n_env++, "MAINPID=%lu", (unsigned long) s->main_pid) < 0) {
                        r = -ENOMEM;
                        goto fail;
                }

        if (!(final_env = strv_env_merge(2,
                                         s->meta.manager->environment,
                                         our_env,
                                         NULL))) {
                r = -ENOMEM;
                goto fail;
        }

        r = exec_spawn(c,
                       argv,
                       &s->exec_context,
                       fds, n_fds,
                       final_env,
                       apply_permissions,
                       apply_chroot,
                       apply_tty_stdin,
                       s->meta.manager->confirm_spawn,
                       s->meta.cgroup_bondings,
                       s->meta.cgroup_attributes,
                       &pid);

        if (r < 0)
                goto fail;


        if ((r = unit_watch_pid(UNIT(s), pid)) < 0)
                /* FIXME: we need to do something here */
                goto fail;

        free(fdsbuf);
        strv_free(argv);
        strv_free(our_env);
        strv_free(final_env);

        *_pid = pid;

        return 0;

fail:
        free(fdsbuf);
        strv_free(argv);
        strv_free(our_env);
        strv_free(final_env);

        if (timeout)
                unit_unwatch_timer(UNIT(s), &s->timer_watch);

        return r;
}

static int main_pid_good(Service *s) {
        assert(s);

        /* Returns 0 if the pid is dead, 1 if it is good, -1 if we
         * don't know */

        /* If we know the pid file, then lets just check if it is
         * still valid */
        if (s->main_pid_known) {

                /* If it's an alien child let's check if it is still
                 * alive ... */
                if (s->main_pid_alien)
                        return kill(s->main_pid, 0) >= 0 || errno != ESRCH;

                /* .. otherwise assume we'll get a SIGCHLD for it,
                 * which we really should wait for to collect exit
                 * status and code */
                return s->main_pid > 0;
        }

        /* We don't know the pid */
        return -EAGAIN;
}

static int control_pid_good(Service *s) {
        assert(s);

        return s->control_pid > 0;
}

static int cgroup_good(Service *s) {
        int r;

        assert(s);

        if ((r = cgroup_bonding_is_empty_list(s->meta.cgroup_bondings)) < 0)
                return r;

        return !r;
}

static void service_enter_dead(Service *s, bool success, bool allow_restart) {
        int r;
        assert(s);

        if (!success)
                s->failure = true;

        if (allow_restart &&
            !s->forbid_restart &&
            (s->restart == SERVICE_RESTART_ALWAYS ||
             (s->restart == SERVICE_RESTART_ON_SUCCESS && !s->failure) ||
             (s->restart == SERVICE_RESTART_ON_FAILURE && s->failure) ||
             (s->restart == SERVICE_RESTART_ON_ABORT && s->failure &&
              (s->main_exec_status.code == CLD_KILLED ||
               s->main_exec_status.code == CLD_DUMPED)))) {

                if ((r = unit_watch_timer(UNIT(s), s->restart_usec, &s->timer_watch)) < 0)
                        goto fail;

                service_set_state(s, SERVICE_AUTO_RESTART);
        } else
                service_set_state(s, s->failure ? SERVICE_FAILED : SERVICE_DEAD);

        s->forbid_restart = false;

        return;

fail:
        log_warning("%s failed to run install restart timer: %s", s->meta.id, strerror(-r));
        service_enter_dead(s, false, false);
}

static void service_enter_signal(Service *s, ServiceState state, bool success);

static void service_enter_stop_post(Service *s, bool success) {
        int r;
        assert(s);

        if (!success)
                s->failure = true;

        service_unwatch_control_pid(s);

        if ((s->control_command = s->exec_command[SERVICE_EXEC_STOP_POST])) {
                s->control_command_id = SERVICE_EXEC_STOP_POST;

                if ((r = service_spawn(s,
                                       s->control_command,
                                       true,
                                       false,
                                       !s->permissions_start_only,
                                       !s->root_directory_start_only,
                                       true,
                                       false,
                                       &s->control_pid)) < 0)
                        goto fail;


                service_set_state(s, SERVICE_STOP_POST);
        } else
                service_enter_signal(s, SERVICE_FINAL_SIGTERM, true);

        return;

fail:
        log_warning("%s failed to run 'stop-post' task: %s", s->meta.id, strerror(-r));
        service_enter_signal(s, SERVICE_FINAL_SIGTERM, false);
}

static void service_enter_signal(Service *s, ServiceState state, bool success) {
        int r;
        Set *pid_set = NULL;
        bool wait_for_exit = false;

        assert(s);

        if (!success)
                s->failure = true;

        if (s->exec_context.kill_mode != KILL_NONE) {
                int sig = (state == SERVICE_STOP_SIGTERM || state == SERVICE_FINAL_SIGTERM) ? s->exec_context.kill_signal : SIGKILL;

                if (s->main_pid > 0) {
                        if (kill_and_sigcont(s->main_pid, sig) < 0 && errno != ESRCH)
                                log_warning("Failed to kill main process %li: %m", (long) s->main_pid);
                        else
                                wait_for_exit = !s->main_pid_alien;
                }

                if (s->control_pid > 0) {
                        if (kill_and_sigcont(s->control_pid, sig) < 0 && errno != ESRCH)
                                log_warning("Failed to kill control process %li: %m", (long) s->control_pid);
                        else
                                wait_for_exit = true;
                }

                if (s->exec_context.kill_mode == KILL_CONTROL_GROUP) {

                        if (!(pid_set = set_new(trivial_hash_func, trivial_compare_func))) {
                                r = -ENOMEM;
                                goto fail;
                        }

                        /* Exclude the main/control pids from being killed via the cgroup */
                        if (s->main_pid > 0)
                                if ((r = set_put(pid_set, LONG_TO_PTR(s->main_pid))) < 0)
                                        goto fail;

                        if (s->control_pid > 0)
                                if ((r = set_put(pid_set, LONG_TO_PTR(s->control_pid))) < 0)
                                        goto fail;

                        if ((r = cgroup_bonding_kill_list(s->meta.cgroup_bondings, sig, true, pid_set)) < 0) {
                                if (r != -EAGAIN && r != -ESRCH && r != -ENOENT)
                                        log_warning("Failed to kill control group: %s", strerror(-r));
                        } else if (r > 0)
                                wait_for_exit = true;

                        set_free(pid_set);
                        pid_set = NULL;
                }
        }

        if (wait_for_exit) {
                if (s->timeout_usec > 0)
                        if ((r = unit_watch_timer(UNIT(s), s->timeout_usec, &s->timer_watch)) < 0)
                                goto fail;

                service_set_state(s, state);
        } else if (state == SERVICE_STOP_SIGTERM || state == SERVICE_STOP_SIGKILL)
                service_enter_stop_post(s, true);
        else
                service_enter_dead(s, true, true);

        return;

fail:
        log_warning("%s failed to kill processes: %s", s->meta.id, strerror(-r));

        if (state == SERVICE_STOP_SIGTERM || state == SERVICE_STOP_SIGKILL)
                service_enter_stop_post(s, false);
        else
                service_enter_dead(s, false, true);

        if (pid_set)
                set_free(pid_set);
}

static void service_enter_stop(Service *s, bool success) {
        int r;

        assert(s);

        if (!success)
                s->failure = true;

        service_unwatch_control_pid(s);

        if ((s->control_command = s->exec_command[SERVICE_EXEC_STOP])) {
                s->control_command_id = SERVICE_EXEC_STOP;

                if ((r = service_spawn(s,
                                       s->control_command,
                                       true,
                                       false,
                                       !s->permissions_start_only,
                                       !s->root_directory_start_only,
                                       false,
                                       false,
                                       &s->control_pid)) < 0)
                        goto fail;

                service_set_state(s, SERVICE_STOP);
        } else
                service_enter_signal(s, SERVICE_STOP_SIGTERM, true);

        return;

fail:
        log_warning("%s failed to run 'stop' task: %s", s->meta.id, strerror(-r));
        service_enter_signal(s, SERVICE_STOP_SIGTERM, false);
}

static void service_enter_running(Service *s, bool success) {
        int main_pid_ok, cgroup_ok;
        assert(s);

        if (!success)
                s->failure = true;

        main_pid_ok = main_pid_good(s);
        cgroup_ok = cgroup_good(s);

        if ((main_pid_ok > 0 || (main_pid_ok < 0 && cgroup_ok != 0)) &&
            (s->bus_name_good || s->type != SERVICE_DBUS))
                service_set_state(s, SERVICE_RUNNING);
        else if (s->remain_after_exit)
                service_set_state(s, SERVICE_EXITED);
        else
                service_enter_stop(s, true);
}

static void service_enter_start_post(Service *s) {
        int r;
        assert(s);

        service_unwatch_control_pid(s);

        if ((s->control_command = s->exec_command[SERVICE_EXEC_START_POST])) {
                s->control_command_id = SERVICE_EXEC_START_POST;

                if ((r = service_spawn(s,
                                       s->control_command,
                                       true,
                                       false,
                                       !s->permissions_start_only,
                                       !s->root_directory_start_only,
                                       false,
                                       false,
                                       &s->control_pid)) < 0)
                        goto fail;

                service_set_state(s, SERVICE_START_POST);
        } else
                service_enter_running(s, true);

        return;

fail:
        log_warning("%s failed to run 'start-post' task: %s", s->meta.id, strerror(-r));
        service_enter_stop(s, false);
}

static void service_enter_start(Service *s) {
        pid_t pid;
        int r;
        ExecCommand *c;

        assert(s);

        assert(s->exec_command[SERVICE_EXEC_START]);
        assert(!s->exec_command[SERVICE_EXEC_START]->command_next || s->type == SERVICE_ONESHOT);

        if (s->type == SERVICE_FORKING)
                service_unwatch_control_pid(s);
        else
                service_unwatch_main_pid(s);

        if (s->type == SERVICE_FORKING) {
                s->control_command_id = SERVICE_EXEC_START;
                c = s->control_command = s->exec_command[SERVICE_EXEC_START];

                s->main_command = NULL;
        } else {
                s->control_command_id = _SERVICE_EXEC_COMMAND_INVALID;
                s->control_command = NULL;

                c = s->main_command = s->exec_command[SERVICE_EXEC_START];
        }

        if ((r = service_spawn(s,
                               c,
                               s->type == SERVICE_FORKING || s->type == SERVICE_DBUS || s->type == SERVICE_NOTIFY,
                               true,
                               true,
                               true,
                               true,
                               s->notify_access != NOTIFY_NONE,
                               &pid)) < 0)
                goto fail;

        if (s->type == SERVICE_SIMPLE) {
                /* For simple services we immediately start
                 * the START_POST binaries. */

                service_set_main_pid(s, pid);
                service_enter_start_post(s);

        } else  if (s->type == SERVICE_FORKING) {

                /* For forking services we wait until the start
                 * process exited. */

                s->control_pid = pid;
                service_set_state(s, SERVICE_START);

        } else if (s->type == SERVICE_ONESHOT ||
                   s->type == SERVICE_DBUS ||
                   s->type == SERVICE_NOTIFY) {

                /* For oneshot services we wait until the start
                 * process exited, too, but it is our main process. */

                /* For D-Bus services we know the main pid right away,
                 * but wait for the bus name to appear on the
                 * bus. Notify services are similar. */

                service_set_main_pid(s, pid);
                service_set_state(s, SERVICE_START);
        } else
                assert_not_reached("Unknown service type");

        return;

fail:
        log_warning("%s failed to run 'start' task: %s", s->meta.id, strerror(-r));
        service_enter_signal(s, SERVICE_FINAL_SIGTERM, false);
}

static void service_enter_start_pre(Service *s) {
        int r;

        assert(s);

        service_unwatch_control_pid(s);

        if ((s->control_command = s->exec_command[SERVICE_EXEC_START_PRE])) {
                s->control_command_id = SERVICE_EXEC_START_PRE;

                if ((r = service_spawn(s,
                                       s->control_command,
                                       true,
                                       false,
                                       !s->permissions_start_only,
                                       !s->root_directory_start_only,
                                       true,
                                       false,
                                       &s->control_pid)) < 0)
                        goto fail;

                service_set_state(s, SERVICE_START_PRE);
        } else
                service_enter_start(s);

        return;

fail:
        log_warning("%s failed to run 'start-pre' task: %s", s->meta.id, strerror(-r));
        service_enter_dead(s, false, true);
}

static void service_enter_restart(Service *s) {
        int r;
        DBusError error;

        assert(s);
        dbus_error_init(&error);

        if (s->meta.job) {
                log_info("Job pending for unit, delaying automatic restart.");

                if ((r = unit_watch_timer(UNIT(s), s->restart_usec, &s->timer_watch)) < 0)
                        goto fail;
        }

        service_enter_dead(s, true, false);

        if ((r = manager_add_job(s->meta.manager, JOB_START, UNIT(s), JOB_FAIL, false, &error, NULL)) < 0)
                goto fail;

        log_debug("%s scheduled restart job.", s->meta.id);
        return;

fail:
        log_warning("%s failed to schedule restart job: %s", s->meta.id, bus_error(&error, -r));
        service_enter_dead(s, false, false);

        dbus_error_free(&error);
}

static void service_enter_reload(Service *s) {
        int r;

        assert(s);

        service_unwatch_control_pid(s);

        if ((s->control_command = s->exec_command[SERVICE_EXEC_RELOAD])) {
                s->control_command_id = SERVICE_EXEC_RELOAD;

                if ((r = service_spawn(s,
                                       s->control_command,
                                       true,
                                       false,
                                       !s->permissions_start_only,
                                       !s->root_directory_start_only,
                                       false,
                                       false,
                                       &s->control_pid)) < 0)
                        goto fail;

                service_set_state(s, SERVICE_RELOAD);
        } else
                service_enter_running(s, true);

        return;

fail:
        log_warning("%s failed to run 'reload' task: %s", s->meta.id, strerror(-r));
        s->reload_failure = true;
        service_enter_running(s, true);
}

static void service_run_next_control(Service *s, bool success) {
        int r;

        assert(s);
        assert(s->control_command);
        assert(s->control_command->command_next);

        if (!success)
                s->failure = true;

        assert(s->control_command_id != SERVICE_EXEC_START);

        s->control_command = s->control_command->command_next;
        service_unwatch_control_pid(s);

        if ((r = service_spawn(s,
                               s->control_command,
                               true,
                               false,
                               !s->permissions_start_only,
                               !s->root_directory_start_only,
                               s->control_command_id == SERVICE_EXEC_START_PRE ||
                               s->control_command_id == SERVICE_EXEC_STOP_POST,
                               false,
                               &s->control_pid)) < 0)
                goto fail;

        return;

fail:
        log_warning("%s failed to run next control task: %s", s->meta.id, strerror(-r));

        if (s->state == SERVICE_START_PRE)
                service_enter_signal(s, SERVICE_FINAL_SIGTERM, false);
        else if (s->state == SERVICE_STOP)
                service_enter_signal(s, SERVICE_STOP_SIGTERM, false);
        else if (s->state == SERVICE_STOP_POST)
                service_enter_dead(s, false, true);
        else if (s->state == SERVICE_RELOAD) {
                s->reload_failure = true;
                service_enter_running(s, true);
        } else
                service_enter_stop(s, false);
}

static void service_run_next_main(Service *s, bool success) {
        pid_t pid;
        int r;

        assert(s);
        assert(s->main_command);
        assert(s->main_command->command_next);
        assert(s->type == SERVICE_ONESHOT);

        if (!success)
                s->failure = true;

        s->main_command = s->main_command->command_next;
        service_unwatch_main_pid(s);

        if ((r = service_spawn(s,
                               s->main_command,
                               false,
                               true,
                               true,
                               true,
                               true,
                               s->notify_access != NOTIFY_NONE,
                               &pid)) < 0)
                goto fail;

        service_set_main_pid(s, pid);

        return;

fail:
        log_warning("%s failed to run next main task: %s", s->meta.id, strerror(-r));
        service_enter_stop(s, false);
}

static int service_start(Unit *u) {
        Service *s = SERVICE(u);

        assert(s);

        /* We cannot fulfill this request right now, try again later
         * please! */
        if (s->state == SERVICE_STOP ||
            s->state == SERVICE_STOP_SIGTERM ||
            s->state == SERVICE_STOP_SIGKILL ||
            s->state == SERVICE_STOP_POST ||
            s->state == SERVICE_FINAL_SIGTERM ||
            s->state == SERVICE_FINAL_SIGKILL)
                return -EAGAIN;

        /* Already on it! */
        if (s->state == SERVICE_START_PRE ||
            s->state == SERVICE_START ||
            s->state == SERVICE_START_POST)
                return 0;

        assert(s->state == SERVICE_DEAD || s->state == SERVICE_FAILED || s->state == SERVICE_AUTO_RESTART);

        /* Make sure we don't enter a busy loop of some kind. */
        if (!ratelimit_test(&s->ratelimit)) {
                log_warning("%s start request repeated too quickly, refusing to start.", u->meta.id);
                return -ECANCELED;
        }

        s->failure = false;
        s->main_pid_known = false;
        s->main_pid_alien = false;
        s->forbid_restart = false;

        service_enter_start_pre(s);
        return 0;
}

static int service_stop(Unit *u) {
        Service *s = SERVICE(u);

        assert(s);

        /* This is a user request, so don't do restarts on this
         * shutdown. */
        s->forbid_restart = true;

        /* Already on it */
        if (s->state == SERVICE_STOP ||
            s->state == SERVICE_STOP_SIGTERM ||
            s->state == SERVICE_STOP_SIGKILL ||
            s->state == SERVICE_STOP_POST ||
            s->state == SERVICE_FINAL_SIGTERM ||
            s->state == SERVICE_FINAL_SIGKILL)
                return 0;

        /* Don't allow a restart */
        if (s->state == SERVICE_AUTO_RESTART) {
                service_set_state(s, SERVICE_DEAD);
                return 0;
        }

        /* If there's already something running we go directly into
         * kill mode. */
        if (s->state == SERVICE_START_PRE ||
            s->state == SERVICE_START ||
            s->state == SERVICE_START_POST ||
            s->state == SERVICE_RELOAD) {
                service_enter_signal(s, SERVICE_STOP_SIGTERM, true);
                return 0;
        }

        assert(s->state == SERVICE_RUNNING ||
               s->state == SERVICE_EXITED);

        service_enter_stop(s, true);
        return 0;
}

static int service_reload(Unit *u) {
        Service *s = SERVICE(u);

        assert(s);

        assert(s->state == SERVICE_RUNNING || s->state == SERVICE_EXITED);

        service_enter_reload(s);
        return 0;
}

static bool service_can_reload(Unit *u) {
        Service *s = SERVICE(u);

        assert(s);

        return !!s->exec_command[SERVICE_EXEC_RELOAD];
}

static int service_serialize(Unit *u, FILE *f, FDSet *fds) {
        Service *s = SERVICE(u);

        assert(u);
        assert(f);
        assert(fds);

        unit_serialize_item(u, f, "state", service_state_to_string(s->state));
        unit_serialize_item(u, f, "failure", yes_no(s->failure));

        if (s->control_pid > 0)
                unit_serialize_item_format(u, f, "control-pid", "%lu", (unsigned long) s->control_pid);

        if (s->main_pid_known && s->main_pid > 0)
                unit_serialize_item_format(u, f, "main-pid", "%lu", (unsigned long) s->main_pid);

        unit_serialize_item(u, f, "main-pid-known", yes_no(s->main_pid_known));

        if (s->status_text)
                unit_serialize_item(u, f, "status-text", s->status_text);

        /* There's a minor uncleanliness here: if there are multiple
         * commands attached here, we will start from the first one
         * again */
        if (s->control_command_id >= 0)
                unit_serialize_item(u, f, "control-command", service_exec_command_to_string(s->control_command_id));

        if (s->socket_fd >= 0) {
                int copy;

                if ((copy = fdset_put_dup(fds, s->socket_fd)) < 0)
                        return copy;

                unit_serialize_item_format(u, f, "socket-fd", "%i", copy);
        }

        if (s->main_exec_status.pid > 0) {
                unit_serialize_item_format(u, f, "main-exec-status-pid", "%lu", (unsigned long) s->main_exec_status.pid);
                dual_timestamp_serialize(f, "main-exec-status-start", &s->main_exec_status.start_timestamp);
                dual_timestamp_serialize(f, "main-exec-status-exit", &s->main_exec_status.exit_timestamp);

                if (dual_timestamp_is_set(&s->main_exec_status.exit_timestamp)) {
                        unit_serialize_item_format(u, f, "main-exec-status-code", "%i", s->main_exec_status.code);
                        unit_serialize_item_format(u, f, "main-exec-status-status", "%i", s->main_exec_status.status);
                }
        }

        return 0;
}

static int service_deserialize_item(Unit *u, const char *key, const char *value, FDSet *fds) {
        Service *s = SERVICE(u);

        assert(u);
        assert(key);
        assert(value);
        assert(fds);

        if (streq(key, "state")) {
                ServiceState state;

                if ((state = service_state_from_string(value)) < 0)
                        log_debug("Failed to parse state value %s", value);
                else
                        s->deserialized_state = state;
        } else if (streq(key, "failure")) {
                int b;

                if ((b = parse_boolean(value)) < 0)
                        log_debug("Failed to parse failure value %s", value);
                else
                        s->failure = b || s->failure;
        } else if (streq(key, "control-pid")) {
                pid_t pid;

                if (parse_pid(value, &pid) < 0)
                        log_debug("Failed to parse control-pid value %s", value);
                else
                        s->control_pid = pid;
        } else if (streq(key, "main-pid")) {
                pid_t pid;

                if (parse_pid(value, &pid) < 0)
                        log_debug("Failed to parse main-pid value %s", value);
                else
                        service_set_main_pid(s, (pid_t) pid);
        } else if (streq(key, "main-pid-known")) {
                int b;

                if ((b = parse_boolean(value)) < 0)
                        log_debug("Failed to parse main-pid-known value %s", value);
                else
                        s->main_pid_known = b;
        } else if (streq(key, "status-text")) {
                char *t;

                if ((t = strdup(value))) {
                        free(s->status_text);
                        s->status_text = t;
                }

        } else if (streq(key, "control-command")) {
                ServiceExecCommand id;

                if ((id = service_exec_command_from_string(value)) < 0)
                        log_debug("Failed to parse exec-command value %s", value);
                else {
                        s->control_command_id = id;
                        s->control_command = s->exec_command[id];
                }
        } else if (streq(key, "socket-fd")) {
                int fd;

                if (safe_atoi(value, &fd) < 0 || fd < 0 || !fdset_contains(fds, fd))
                        log_debug("Failed to parse socket-fd value %s", value);
                else {

                        if (s->socket_fd >= 0)
                                close_nointr_nofail(s->socket_fd);
                        s->socket_fd = fdset_remove(fds, fd);
                }
        } else if (streq(key, "main-exec-status-pid")) {
                pid_t pid;

                if (parse_pid(value, &pid) < 0)
                        log_debug("Failed to parse main-exec-status-pid value %s", value);
                else
                        s->main_exec_status.pid = pid;
        } else if (streq(key, "main-exec-status-code")) {
                int i;

                if (safe_atoi(value, &i) < 0)
                        log_debug("Failed to parse main-exec-status-code value %s", value);
                else
                        s->main_exec_status.code = i;
        } else if (streq(key, "main-exec-status-status")) {
                int i;

                if (safe_atoi(value, &i) < 0)
                        log_debug("Failed to parse main-exec-status-status value %s", value);
                else
                        s->main_exec_status.status = i;
        } else if (streq(key, "main-exec-status-start"))
                dual_timestamp_deserialize(value, &s->main_exec_status.start_timestamp);
        else if (streq(key, "main-exec-status-exit"))
                dual_timestamp_deserialize(value, &s->main_exec_status.exit_timestamp);
        else
                log_debug("Unknown serialization key '%s'", key);

        return 0;
}

static UnitActiveState service_active_state(Unit *u) {
        assert(u);

        return state_translation_table[SERVICE(u)->state];
}

static const char *service_sub_state_to_string(Unit *u) {
        assert(u);

        return service_state_to_string(SERVICE(u)->state);
}

static bool service_check_gc(Unit *u) {
        Service *s = SERVICE(u);

        assert(s);

        /* Never clean up services that still have a process around,
         * even if the service is formally dead. */
        if (cgroup_good(s) > 0 ||
            main_pid_good(s) > 0 ||
            control_pid_good(s) > 0)
                return true;

#ifdef HAVE_SYSV_COMPAT
        if (s->sysv_path)
                return true;
#endif

        return false;
}

static bool service_check_snapshot(Unit *u) {
        Service *s = SERVICE(u);

        assert(s);

        return !s->got_socket_fd;
}

static void service_sigchld_event(Unit *u, pid_t pid, int code, int status) {
        Service *s = SERVICE(u);
        bool success;

        assert(s);
        assert(pid >= 0);

        if (!s->meta.fragment_path)
                success = is_clean_exit_lsb(code, status);
        else
                success = is_clean_exit(code, status);

        if (s->main_pid == pid) {
                /* Forking services may occasionally move to a new PID.
                 * As long as they update the PID file before exiting the old
                 * PID, they're fine. */
                if (service_load_pid_file(s, false) == 0)
                        return;

                s->main_pid = 0;
                exec_status_exit(&s->main_exec_status, &s->exec_context, pid, code, status);

                /* If this is not a forking service than the main
                 * process got started and hence we copy the exit
                 * status so that it is recorded both as main and as
                 * control process exit status */
                if (s->main_command) {
                        s->main_command->exec_status = s->main_exec_status;

                        if (s->main_command->ignore)
                                success = true;
                }

                log_full(success ? LOG_DEBUG : LOG_NOTICE,
                         "%s: main process exited, code=%s, status=%i", u->meta.id, sigchld_code_to_string(code), status);
                s->failure = s->failure || !success;

                if (s->main_command &&
                    s->main_command->command_next &&
                    success) {

                        /* There is another command to *
                         * execute, so let's do that. */

                        log_debug("%s running next main command for state %s", u->meta.id, service_state_to_string(s->state));
                        service_run_next_main(s, success);

                } else {

                        /* The service exited, so the service is officially
                         * gone. */
                        s->main_command = NULL;

                        switch (s->state) {

                        case SERVICE_START_POST:
                        case SERVICE_RELOAD:
                        case SERVICE_STOP:
                                /* Need to wait until the operation is
                                 * done */
                                break;

                        case SERVICE_START:
                                if (s->type == SERVICE_ONESHOT) {
                                        /* This was our main goal, so let's go on */
                                        if (success)
                                                service_enter_start_post(s);
                                        else
                                                service_enter_signal(s, SERVICE_FINAL_SIGTERM, false);
                                        break;
                                } else {
                                        assert(s->type == SERVICE_DBUS || s->type == SERVICE_NOTIFY);

                                        /* Fall through */
                                }

                        case SERVICE_RUNNING:
                                service_enter_running(s, success);
                                break;

                        case SERVICE_STOP_SIGTERM:
                        case SERVICE_STOP_SIGKILL:

                                if (!control_pid_good(s))
                                        service_enter_stop_post(s, success);

                                /* If there is still a control process, wait for that first */
                                break;

                        default:
                                assert_not_reached("Uh, main process died at wrong time.");
                        }
                }

        } else if (s->control_pid == pid) {

                s->control_pid = 0;

                if (s->control_command) {
                        exec_status_exit(&s->control_command->exec_status, &s->exec_context, pid, code, status);

                        if (s->control_command->ignore)
                                success = true;
                }

               log_full(success ? LOG_DEBUG : LOG_NOTICE,
                         "%s: control process exited, code=%s status=%i", u->meta.id, sigchld_code_to_string(code), status);
                s->failure = s->failure || !success;

                if (s->control_command &&
                    s->control_command->command_next &&
                    success) {

                        /* There is another command to *
                         * execute, so let's do that. */

                        log_debug("%s running next control command for state %s", u->meta.id, service_state_to_string(s->state));
                        service_run_next_control(s, success);

                } else {
                        /* No further commands for this step, so let's
                         * figure out what to do next */

                        s->control_command = NULL;
                        s->control_command_id = _SERVICE_EXEC_COMMAND_INVALID;

                        log_debug("%s got final SIGCHLD for state %s", u->meta.id, service_state_to_string(s->state));

                        switch (s->state) {

                        case SERVICE_START_PRE:
                                if (success)
                                        service_enter_start(s);
                                else
                                        service_enter_signal(s, SERVICE_FINAL_SIGTERM, false);
                                break;

                        case SERVICE_START:
                                assert(s->type == SERVICE_FORKING);

                                /* Let's try to load the pid
                                 * file here if we can. We
                                 * ignore the return value,
                                 * since the PID file might
                                 * actually be created by a
                                 * START_POST script */

                                if (success) {
                                        service_load_pid_file(s, !s->exec_command[SERVICE_EXEC_START_POST]);
                                        service_search_main_pid(s);

                                        service_enter_start_post(s);
                                } else
                                        service_enter_signal(s, SERVICE_FINAL_SIGTERM, false);

                                break;

                        case SERVICE_START_POST:
                                if (success) {
                                        service_load_pid_file(s, true);
                                        service_search_main_pid(s);
                                }

                                s->reload_failure = !success;
                                service_enter_running(s, true);
                                break;

                        case SERVICE_RELOAD:
                                if (success) {
                                        service_load_pid_file(s, true);
                                        service_search_main_pid(s);
                                }

                                s->reload_failure = !success;
                                service_enter_running(s, true);
                                break;

                        case SERVICE_STOP:
                                service_enter_signal(s, SERVICE_STOP_SIGTERM, success);
                                break;

                        case SERVICE_STOP_SIGTERM:
                        case SERVICE_STOP_SIGKILL:
                                if (main_pid_good(s) <= 0)
                                        service_enter_stop_post(s, success);

                                /* If there is still a service
                                 * process around, wait until
                                 * that one quit, too */
                                break;

                        case SERVICE_STOP_POST:
                        case SERVICE_FINAL_SIGTERM:
                        case SERVICE_FINAL_SIGKILL:
                                service_enter_dead(s, success, true);
                                break;

                        default:
                                assert_not_reached("Uh, control process died at wrong time.");
                        }
                }
        }

        /* Notify clients about changed exit status */
        unit_add_to_dbus_queue(u);
}

static void service_timer_event(Unit *u, uint64_t elapsed, Watch* w) {
        Service *s = SERVICE(u);

        assert(s);
        assert(elapsed == 1);

        assert(w == &s->timer_watch);

        switch (s->state) {

        case SERVICE_START_PRE:
        case SERVICE_START:
                log_warning("%s operation timed out. Terminating.", u->meta.id);
                service_enter_signal(s, SERVICE_FINAL_SIGTERM, false);
                break;

        case SERVICE_START_POST:
                log_warning("%s operation timed out. Stopping.", u->meta.id);
                service_enter_stop(s, false);
                break;

        case SERVICE_RELOAD:
                log_warning("%s operation timed out. Stopping.", u->meta.id);
                s->reload_failure = true;
                service_enter_running(s, true);
                break;

        case SERVICE_STOP:
                log_warning("%s stopping timed out. Terminating.", u->meta.id);
                service_enter_signal(s, SERVICE_STOP_SIGTERM, false);
                break;

        case SERVICE_STOP_SIGTERM:
                if (s->exec_context.send_sigkill) {
                        log_warning("%s stopping timed out. Killing.", u->meta.id);
                        service_enter_signal(s, SERVICE_STOP_SIGKILL, false);
                } else {
                        log_warning("%s stopping timed out. Skipping SIGKILL.", u->meta.id);
                        service_enter_stop_post(s, false);
                }

                break;

        case SERVICE_STOP_SIGKILL:
                /* Uh, we sent a SIGKILL and it is still not gone?
                 * Must be something we cannot kill, so let's just be
                 * weirded out and continue */

                log_warning("%s still around after SIGKILL. Ignoring.", u->meta.id);
                service_enter_stop_post(s, false);
                break;

        case SERVICE_STOP_POST:
                log_warning("%s stopping timed out (2). Terminating.", u->meta.id);
                service_enter_signal(s, SERVICE_FINAL_SIGTERM, false);
                break;

        case SERVICE_FINAL_SIGTERM:
                if (s->exec_context.send_sigkill) {
                        log_warning("%s stopping timed out (2). Killing.", u->meta.id);
                        service_enter_signal(s, SERVICE_FINAL_SIGKILL, false);
                } else {
                        log_warning("%s stopping timed out (2). Skipping SIGKILL. Entering failed mode.", u->meta.id);
                        service_enter_dead(s, false, true);
                }

                break;

        case SERVICE_FINAL_SIGKILL:
                log_warning("%s still around after SIGKILL (2). Entering failed mode.", u->meta.id);
                service_enter_dead(s, false, true);
                break;

        case SERVICE_AUTO_RESTART:
                log_info("%s holdoff time over, scheduling restart.", u->meta.id);
                service_enter_restart(s);
                break;

        default:
                assert_not_reached("Timeout at wrong time.");
        }
}

static void service_cgroup_notify_event(Unit *u) {
        Service *s = SERVICE(u);

        assert(u);

        log_debug("%s: cgroup is empty", u->meta.id);

        switch (s->state) {

                /* Waiting for SIGCHLD is usually more interesting,
                 * because it includes return codes/signals. Which is
                 * why we ignore the cgroup events for most cases,
                 * except when we don't know pid which to expect the
                 * SIGCHLD for. */

        case SERVICE_RUNNING:
                service_enter_running(s, true);
                break;

        case SERVICE_STOP_SIGTERM:
        case SERVICE_STOP_SIGKILL:

                if (main_pid_good(s) <= 0 && !control_pid_good(s))
                        service_enter_stop_post(s, true);

                break;

        case SERVICE_FINAL_SIGTERM:
        case SERVICE_FINAL_SIGKILL:
                if (main_pid_good(s) <= 0 && !control_pid_good(s))
                        service_enter_dead(s, true, true);

                break;

        default:
                ;
        }
}

static void service_notify_message(Unit *u, pid_t pid, char **tags) {
        Service *s = SERVICE(u);
        const char *e;

        assert(u);

        if (s->notify_access == NOTIFY_NONE) {
                log_warning("%s: Got notification message from PID %lu, but reception is disabled.",
                            u->meta.id, (unsigned long) pid);
                return;
        }

        if (s->notify_access == NOTIFY_MAIN && pid != s->main_pid) {
                log_warning("%s: Got notification message from PID %lu, but reception only permitted for PID %lu",
                            u->meta.id, (unsigned long) pid, (unsigned long) s->main_pid);
                return;
        }

        log_debug("%s: Got message", u->meta.id);

        /* Interpret MAINPID= */
        if ((e = strv_find_prefix(tags, "MAINPID=")) &&
            (s->state == SERVICE_START ||
             s->state == SERVICE_START_POST ||
             s->state == SERVICE_RUNNING ||
             s->state == SERVICE_RELOAD)) {

                if (parse_pid(e + 8, &pid) < 0)
                        log_warning("Failed to parse notification message %s", e);
                else {
                        log_debug("%s: got %s", u->meta.id, e);
                        service_set_main_pid(s, pid);
                }
        }

        /* Interpret READY= */
        if (s->type == SERVICE_NOTIFY &&
            s->state == SERVICE_START &&
            strv_find(tags, "READY=1")) {
                log_debug("%s: got READY=1", u->meta.id);

                service_enter_start_post(s);
        }

        /* Interpret STATUS= */
        if ((e = strv_find_prefix(tags, "STATUS="))) {
                char *t;

                if (e[7]) {
                        if (!(t = strdup(e+7))) {
                                log_error("Failed to allocate string.");
                                return;
                        }

                        log_debug("%s: got %s", u->meta.id, e);

                        free(s->status_text);
                        s->status_text = t;
                } else {
                        free(s->status_text);
                        s->status_text = NULL;
                }

        }

        /* Notify clients about changed status or main pid */
        unit_add_to_dbus_queue(u);
}

#ifdef HAVE_SYSV_COMPAT

#ifdef TARGET_SUSE
static void sysv_facility_in_insserv_conf(Manager *mgr) {
        FILE *f=NULL;
        int r;

        if (!(f = fopen("/etc/insserv.conf", "re"))) {
                r = errno == ENOENT ? 0 : -errno;
                goto finish;
        }

        while (!feof(f)) {
                char l[LINE_MAX], *t;
                char **parsed = NULL;

                if (!fgets(l, sizeof(l), f)) {
                        if (feof(f))
                                break;

                        r = -errno;
                        log_error("Failed to read configuration file '/etc/insserv.conf': %s", strerror(-r));
                        goto finish;
                }

                t = strstrip(l);
                if (*t != '$' && *t != '<')
                        continue;

                parsed = strv_split(t,WHITESPACE);
                /* we ignore <interactive>, not used, equivalent to X-Interactive */
                if (parsed && !startswith_no_case (parsed[0], "<interactive>")) {
                        char *facility;
                        Unit *u;
                        if (sysv_translate_facility(parsed[0], NULL, &facility) < 0)
                                continue;
                        if ((u = manager_get_unit(mgr, facility)) && (u->meta.type == UNIT_TARGET)) {
                                UnitDependency e;
                                char *dep = NULL, *name, **j;

                                STRV_FOREACH (j, parsed+1) {
                                        if (*j[0]=='+') {
                                                e = UNIT_WANTS;
                                                name = *j+1;
                                        }
                                        else {
                                                e = UNIT_REQUIRES;
                                                name = *j;
                                        }
                                        if (sysv_translate_facility(name, NULL, &dep) < 0)
                                                continue;

                                        r = unit_add_two_dependencies_by_name(u, UNIT_BEFORE, e, dep, NULL, true);
                                        free(dep);
                                }
                        }
                        free(facility);
                }
                strv_free(parsed);
        }
finish:
        if (f)
                fclose(f);

}
#endif

static int service_enumerate(Manager *m) {
        char **p;
        unsigned i;
        DIR *d = NULL;
        char *path = NULL, *fpath = NULL, *name = NULL;
        Set *runlevel_services[ELEMENTSOF(rcnd_table)], *shutdown_services = NULL;
        Unit *service;
        Iterator j;
        int r;

        assert(m);

        if (m->running_as != MANAGER_SYSTEM)
                return 0;

        zero(runlevel_services);

        STRV_FOREACH(p, m->lookup_paths.sysvrcnd_path)
                for (i = 0; i < ELEMENTSOF(rcnd_table); i ++) {
                        struct dirent *de;

                        free(path);
                        path = join(*p, "/", rcnd_table[i].path, NULL);
                        if (!path) {
                                r = -ENOMEM;
                                goto finish;
                        }

                        if (d)
                                closedir(d);

                        if (!(d = opendir(path))) {
                                if (errno != ENOENT)
                                        log_warning("opendir() failed on %s: %s", path, strerror(errno));

                                continue;
                        }

                        while ((de = readdir(d))) {
                                int a, b;

                                if (ignore_file(de->d_name))
                                        continue;

                                if (de->d_name[0] != 'S' && de->d_name[0] != 'K')
                                        continue;

                                if (strlen(de->d_name) < 4)
                                        continue;

                                a = undecchar(de->d_name[1]);
                                b = undecchar(de->d_name[2]);

                                if (a < 0 || b < 0)
                                        continue;

                                free(fpath);
                                fpath = join(path, "/", de->d_name, NULL);
                                if (!fpath) {
                                        r = -ENOMEM;
                                        goto finish;
                                }

                                if (access(fpath, X_OK) < 0) {

                                        if (errno != ENOENT)
                                                log_warning("access() failed on %s: %s", fpath, strerror(errno));

                                        continue;
                                }

                                free(name);
                                if (!(name = sysv_translate_name(de->d_name + 3))) {
                                        r = -ENOMEM;
                                        goto finish;
                                }

                                if ((r = manager_load_unit_prepare(m, name, NULL, NULL, &service)) < 0) {
                                        log_warning("Failed to prepare unit %s: %s", name, strerror(-r));
                                        continue;
                                }

                                if (de->d_name[0] == 'S')  {

                                        if (rcnd_table[i].type == RUNLEVEL_UP || rcnd_table[i].type == RUNLEVEL_SYSINIT) {
                                                SERVICE(service)->sysv_start_priority_from_rcnd =
                                                        MAX(a*10 + b, SERVICE(service)->sysv_start_priority_from_rcnd);

                                                SERVICE(service)->sysv_enabled = true;
                                        }

                                        if ((r = set_ensure_allocated(&runlevel_services[i], trivial_hash_func, trivial_compare_func)) < 0)
                                                goto finish;

                                        if ((r = set_put(runlevel_services[i], service)) < 0)
                                                goto finish;

                                } else if (de->d_name[0] == 'K' &&
                                           (rcnd_table[i].type == RUNLEVEL_DOWN ||
                                            rcnd_table[i].type == RUNLEVEL_SYSINIT)) {

                                        if ((r = set_ensure_allocated(&shutdown_services, trivial_hash_func, trivial_compare_func)) < 0)
                                                goto finish;

                                        if ((r = set_put(shutdown_services, service)) < 0)
                                                goto finish;
                                }
                        }
                }

        /* Now we loaded all stubs and are aware of the lowest
        start-up priority for all services, not let's actually load
        the services, this will also tell us which services are
        actually native now */
        manager_dispatch_load_queue(m);

        /* If this is a native service, rely on native ways to pull in
         * a service, don't pull it in via sysv rcN.d links. */
        for (i = 0; i < ELEMENTSOF(rcnd_table); i ++)
                SET_FOREACH(service, runlevel_services[i], j) {
                        service = unit_follow_merge(service);

                        if (service->meta.fragment_path)
                                continue;

                        if ((r = unit_add_two_dependencies_by_name_inverse(service, UNIT_AFTER, UNIT_WANTS, rcnd_table[i].target, NULL, true)) < 0)
                                goto finish;
                }

        /* We honour K links only for halt/reboot. For the normal
         * runlevels we assume the stop jobs will be implicitly added
         * by the core logic. Also, we don't really distinguish here
         * between the runlevels 0 and 6 and just add them to the
         * special shutdown target. On SUSE the boot.d/ runlevel is
         * also used for shutdown, so we add links for that too to the
         * shutdown target.*/
        SET_FOREACH(service, shutdown_services, j) {
                service = unit_follow_merge(service);

                if (service->meta.fragment_path)
                        continue;

                if ((r = unit_add_two_dependencies_by_name(service, UNIT_BEFORE, UNIT_CONFLICTS, SPECIAL_SHUTDOWN_TARGET, NULL, true)) < 0)
                        goto finish;
        }

        r = 0;

#ifdef TARGET_SUSE
	sysv_facility_in_insserv_conf (m);
#endif

finish:
        free(path);
        free(fpath);
        free(name);

        for (i = 0; i < ELEMENTSOF(rcnd_table); i++)
                set_free(runlevel_services[i]);
        set_free(shutdown_services);

        if (d)
                closedir(d);

        return r;
}
#endif

static void service_bus_name_owner_change(
                Unit *u,
                const char *name,
                const char *old_owner,
                const char *new_owner) {

        Service *s = SERVICE(u);

        assert(s);
        assert(name);

        assert(streq(s->bus_name, name));
        assert(old_owner || new_owner);

        if (old_owner && new_owner)
                log_debug("%s's D-Bus name %s changed owner from %s to %s", u->meta.id, name, old_owner, new_owner);
        else if (old_owner)
                log_debug("%s's D-Bus name %s no longer registered by %s", u->meta.id, name, old_owner);
        else
                log_debug("%s's D-Bus name %s now registered by %s", u->meta.id, name, new_owner);

        s->bus_name_good = !!new_owner;

        if (s->type == SERVICE_DBUS) {

                /* service_enter_running() will figure out what to
                 * do */
                if (s->state == SERVICE_RUNNING)
                        service_enter_running(s, true);
                else if (s->state == SERVICE_START && new_owner)
                        service_enter_start_post(s);

        } else if (new_owner &&
                   s->main_pid <= 0 &&
                   (s->state == SERVICE_START ||
                    s->state == SERVICE_START_POST ||
                    s->state == SERVICE_RUNNING ||
                    s->state == SERVICE_RELOAD)) {

                /* Try to acquire PID from bus service */
                log_debug("Trying to acquire PID from D-Bus name...");

                bus_query_pid(u->meta.manager, name);
        }
}

static void service_bus_query_pid_done(
                Unit *u,
                const char *name,
                pid_t pid) {

        Service *s = SERVICE(u);

        assert(s);
        assert(name);

        log_debug("%s's D-Bus name %s is now owned by process %u", u->meta.id, name, (unsigned) pid);

        if (s->main_pid <= 0 &&
            (s->state == SERVICE_START ||
             s->state == SERVICE_START_POST ||
             s->state == SERVICE_RUNNING ||
             s->state == SERVICE_RELOAD))
                service_set_main_pid(s, pid);
}

int service_set_socket_fd(Service *s, int fd, Socket *sock) {
        assert(s);
        assert(fd >= 0);

        /* This is called by the socket code when instantiating a new
         * service for a stream socket and the socket needs to be
         * configured. */

        if (s->meta.load_state != UNIT_LOADED)
                return -EINVAL;

        if (s->socket_fd >= 0)
                return -EBUSY;

        if (s->state != SERVICE_DEAD)
                return -EAGAIN;

        s->socket_fd = fd;
        s->got_socket_fd = true;
        s->accept_socket = sock;

        return 0;
}

static void service_reset_failed(Unit *u) {
        Service *s = SERVICE(u);

        assert(s);

        if (s->state == SERVICE_FAILED)
                service_set_state(s, SERVICE_DEAD);

        s->failure = false;
}

static bool service_need_daemon_reload(Unit *u) {
        Service *s = SERVICE(u);

        assert(s);

#ifdef HAVE_SYSV_COMPAT
        if (s->sysv_path) {
                struct stat st;

                zero(st);
                if (stat(s->sysv_path, &st) < 0)
                        /* What, cannot access this anymore? */
                        return true;

                if (s->sysv_mtime > 0 &&
                    timespec_load(&st.st_mtim) != s->sysv_mtime)
                        return true;
        }
#endif

        return false;
}

static int service_kill(Unit *u, KillWho who, KillMode mode, int signo, DBusError *error) {
        Service *s = SERVICE(u);
        int r = 0;
        Set *pid_set = NULL;

        assert(s);

        if (s->main_pid <= 0 && who == KILL_MAIN) {
                dbus_set_error(error, BUS_ERROR_NO_SUCH_PROCESS, "No main process to kill");
                return -ESRCH;
        }

        if (s->control_pid <= 0 && who == KILL_CONTROL) {
                dbus_set_error(error, BUS_ERROR_NO_SUCH_PROCESS, "No control process to kill");
                return -ESRCH;
        }

        if (who == KILL_CONTROL || who == KILL_ALL)
                if (s->control_pid > 0)
                        if (kill(s->control_pid, signo) < 0)
                                r = -errno;

        if (who == KILL_MAIN || who == KILL_ALL)
                if (s->main_pid > 0)
                        if (kill(s->main_pid, signo) < 0)
                                r = -errno;

        if (who == KILL_ALL && mode == KILL_CONTROL_GROUP) {
                int q;

                if (!(pid_set = set_new(trivial_hash_func, trivial_compare_func)))
                        return -ENOMEM;

                /* Exclude the control/main pid from being killed via the cgroup */
                if (s->control_pid > 0)
                        if ((q = set_put(pid_set, LONG_TO_PTR(s->control_pid))) < 0) {
                                r = q;
                                goto finish;
                        }

                if (s->main_pid > 0)
                        if ((q = set_put(pid_set, LONG_TO_PTR(s->main_pid))) < 0) {
                                r = q;
                                goto finish;
                        }

                if ((q = cgroup_bonding_kill_list(s->meta.cgroup_bondings, signo, false, pid_set)) < 0)
                        if (q != -EAGAIN && q != -ESRCH && q != -ENOENT)
                                r = q;
        }

finish:
        if (pid_set)
                set_free(pid_set);

        return r;
}

static const char* const service_state_table[_SERVICE_STATE_MAX] = {
        [SERVICE_DEAD] = "dead",
        [SERVICE_START_PRE] = "start-pre",
        [SERVICE_START] = "start",
        [SERVICE_START_POST] = "start-post",
        [SERVICE_RUNNING] = "running",
        [SERVICE_EXITED] = "exited",
        [SERVICE_RELOAD] = "reload",
        [SERVICE_STOP] = "stop",
        [SERVICE_STOP_SIGTERM] = "stop-sigterm",
        [SERVICE_STOP_SIGKILL] = "stop-sigkill",
        [SERVICE_STOP_POST] = "stop-post",
        [SERVICE_FINAL_SIGTERM] = "final-sigterm",
        [SERVICE_FINAL_SIGKILL] = "final-sigkill",
        [SERVICE_FAILED] = "failed",
        [SERVICE_AUTO_RESTART] = "auto-restart",
};

DEFINE_STRING_TABLE_LOOKUP(service_state, ServiceState);

static const char* const service_restart_table[_SERVICE_RESTART_MAX] = {
        [SERVICE_RESTART_NO] = "no",
        [SERVICE_RESTART_ON_SUCCESS] = "on-success",
        [SERVICE_RESTART_ON_FAILURE] = "on-failure",
        [SERVICE_RESTART_ON_ABORT] = "on-abort",
        [SERVICE_RESTART_ALWAYS] = "always"
};

DEFINE_STRING_TABLE_LOOKUP(service_restart, ServiceRestart);

static const char* const service_type_table[_SERVICE_TYPE_MAX] = {
        [SERVICE_SIMPLE] = "simple",
        [SERVICE_FORKING] = "forking",
        [SERVICE_ONESHOT] = "oneshot",
        [SERVICE_DBUS] = "dbus",
        [SERVICE_NOTIFY] = "notify"
};

DEFINE_STRING_TABLE_LOOKUP(service_type, ServiceType);

static const char* const service_exec_command_table[_SERVICE_EXEC_COMMAND_MAX] = {
        [SERVICE_EXEC_START_PRE] = "ExecStartPre",
        [SERVICE_EXEC_START] = "ExecStart",
        [SERVICE_EXEC_START_POST] = "ExecStartPost",
        [SERVICE_EXEC_RELOAD] = "ExecReload",
        [SERVICE_EXEC_STOP] = "ExecStop",
        [SERVICE_EXEC_STOP_POST] = "ExecStopPost",
};

DEFINE_STRING_TABLE_LOOKUP(service_exec_command, ServiceExecCommand);

static const char* const notify_access_table[_NOTIFY_ACCESS_MAX] = {
        [NOTIFY_NONE] = "none",
        [NOTIFY_MAIN] = "main",
        [NOTIFY_ALL] = "all"
};

DEFINE_STRING_TABLE_LOOKUP(notify_access, NotifyAccess);

const UnitVTable service_vtable = {
        .suffix = ".service",
        .sections =
                "Unit\0"
                "Service\0"
                "Install\0",
        .show_status = true,

        .init = service_init,
        .done = service_done,
        .load = service_load,

        .coldplug = service_coldplug,

        .dump = service_dump,

        .start = service_start,
        .stop = service_stop,
        .reload = service_reload,

        .can_reload = service_can_reload,

        .kill = service_kill,

        .serialize = service_serialize,
        .deserialize_item = service_deserialize_item,

        .active_state = service_active_state,
        .sub_state_to_string = service_sub_state_to_string,

        .check_gc = service_check_gc,
        .check_snapshot = service_check_snapshot,

        .sigchld_event = service_sigchld_event,
        .timer_event = service_timer_event,

        .reset_failed = service_reset_failed,

        .need_daemon_reload = service_need_daemon_reload,

        .cgroup_notify_empty = service_cgroup_notify_event,
        .notify_message = service_notify_message,

        .bus_name_owner_change = service_bus_name_owner_change,
        .bus_query_pid_done = service_bus_query_pid_done,

        .bus_interface = "org.freedesktop.systemd1.Service",
        .bus_message_handler = bus_service_message_handler,
        .bus_invalidating_properties =  bus_service_invalidating_properties,

#ifdef HAVE_SYSV_COMPAT
        .enumerate = service_enumerate
#endif
};
