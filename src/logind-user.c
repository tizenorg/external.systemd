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

#include <string.h>
#include <unistd.h>
#include <errno.h>

#include "logind-user.h"
#include "util.h"
#include "cgroup-util.h"
#include "hashmap.h"
#include "strv.h"

User* user_new(Manager *m, uid_t uid, gid_t gid, const char *name) {
        User *u;

        assert(m);
        assert(name);

        u = new0(User, 1);
        if (!u)
                return NULL;

        u->name = strdup(name);
        if (!u->name) {
                free(u);
                return NULL;
        }

        if (asprintf(&u->state_file, "/run/systemd/users/%lu", (unsigned long) uid) < 0) {
                free(u->name);
                free(u);
                return NULL;
        }

        if (hashmap_put(m->users, ULONG_TO_PTR((unsigned long) uid), u) < 0) {
                free(u->state_file);
                free(u->name);
                free(u);
                return NULL;
        }

        u->manager = m;
        u->uid = uid;
        u->gid = gid;

        return u;
}

void user_free(User *u) {
        assert(u);

        if (u->in_gc_queue)
                LIST_REMOVE(User, gc_queue, u->manager->user_gc_queue, u);

        while (u->sessions)
                session_free(u->sessions);

        free(u->cgroup_path);

        free(u->service);
        free(u->runtime_path);

        hashmap_remove(u->manager->users, ULONG_TO_PTR((unsigned long) u->uid));

        free(u->name);
        free(u->state_file);
        free(u);
}

int user_save(User *u) {
        FILE *f;
        int r;
        char *temp_path;

        assert(u);
        assert(u->state_file);

        if (!u->started)
                return 0;

        r = safe_mkdir("/run/systemd/users", 0755, 0, 0);
        if (r < 0)
                goto finish;

        r = fopen_temporary(u->state_file, &f, &temp_path);
        if (r < 0)
                goto finish;

        fchmod(fileno(f), 0644);

        fprintf(f,
                "# This is private data. Do not parse.\n"
                "NAME=%s\n"
                "STATE=%s\n",
                u->name,
                user_state_to_string(user_get_state(u)));

        if (u->cgroup_path)
                fprintf(f,
                        "CGROUP=%s\n",
                        u->cgroup_path);

        if (u->runtime_path)
                fprintf(f,
                        "RUNTIME=%s\n",
                        u->runtime_path);

        if (u->service)
                fprintf(f,
                        "SERVICE=%s\n",
                        u->service);

        if (u->display)
                fprintf(f,
                        "DISPLAY=%s\n",
                        u->display->id);

        if (u->sessions) {
                Session *i;

                fputs("SESSIONS=", f);
                LIST_FOREACH(sessions_by_user, i, u->sessions) {
                        fprintf(f,
                                "%s%c",
                                i->id,
                                i->sessions_by_user_next ? ' ' : '\n');
                }

                fputs("SEATS=", f);
                LIST_FOREACH(sessions_by_user, i, u->sessions) {
                        if (i->seat)
                                fprintf(f,
                                        "%s%c",
                                        i->seat->id,
                                        i->sessions_by_user_next ? ' ' : '\n');
                }

                fputs("ACTIVE_SESSIONS=", f);
                LIST_FOREACH(sessions_by_user, i, u->sessions)
                        if (session_is_active(i))
                                fprintf(f,
                                        "%lu%c",
                                        (unsigned long) i->user->uid,
                                        i->sessions_by_user_next ? ' ' : '\n');

                fputs("ACTIVE_SEATS=", f);
                LIST_FOREACH(sessions_by_user, i, u->sessions) {
                        if (session_is_active(i) && i->seat)
                                fprintf(f,
                                        "%s%c",
                                        i->seat->id,
                                        i->sessions_by_user_next ? ' ' : '\n');
                }
        }

        fflush(f);

        if (ferror(f) || rename(temp_path, u->state_file) < 0) {
                r = -errno;
                unlink(u->state_file);
                unlink(temp_path);
        }

        fclose(f);
        free(temp_path);

finish:
        if (r < 0)
                log_error("Failed to save user data for %s: %s", u->name, strerror(-r));

        return r;
}

int user_load(User *u) {
        int r;
        char *display = NULL;
        Session *s = NULL;

        assert(u);

        r = parse_env_file(u->state_file, NEWLINE,
                           "CGROUP", &u->cgroup_path,
                           "RUNTIME", &u->runtime_path,
                           "SERVICE", &u->service,
                           "DISPLAY", &display,
                           NULL);
        if (r < 0) {
                free(display);

                if (r == -ENOENT)
                        return 0;

                log_error("Failed to read %s: %s", u->state_file, strerror(-r));
                return r;
        }

        if (display) {
                s = hashmap_get(u->manager->sessions, display);
                free(display);
        }

        if (s && s->display && display_is_local(s->display))
                u->display = s;

        return r;
}

static int user_mkdir_runtime_path(User *u) {
        char *p;
        int r;

        assert(u);

        r = safe_mkdir("/run/user", 0755, 0, 0);
        if (r < 0) {
                log_error("Failed to create /run/user: %s", strerror(-r));
                return r;
        }

        if (!u->runtime_path) {
                p = strappend("/run/user/", u->name);

                if (!p) {
                        log_error("Out of memory");
                        return -ENOMEM;
                }
        } else
                p = u->runtime_path;

        r = safe_mkdir(p, 0700, u->uid, u->gid);
        if (r < 0) {
                log_error("Failed to create runtime directory %s: %s", p, strerror(-r));
                free(p);
                u->runtime_path = NULL;
                return r;
        }

        u->runtime_path = p;
        return 0;
}

static int user_create_cgroup(User *u) {
        char **k;
        char *p;
        int r;

        assert(u);

        if (!u->cgroup_path) {
                if (asprintf(&p, "%s/%s", u->manager->cgroup_path, u->name) < 0) {
                        log_error("Out of memory");
                        return -ENOMEM;
                }
        } else
                p = u->cgroup_path;

        r = cg_create(SYSTEMD_CGROUP_CONTROLLER, p);
        if (r < 0) {
                log_error("Failed to create cgroup "SYSTEMD_CGROUP_CONTROLLER":%s: %s", p, strerror(-r));
                free(p);
                u->cgroup_path = NULL;
                return r;
        }

        u->cgroup_path = p;

        STRV_FOREACH(k, u->manager->controllers) {

                if (strv_contains(u->manager->reset_controllers, *k))
                        continue;

                r = cg_create(*k, p);
                if (r < 0)
                        log_warning("Failed to create cgroup %s:%s: %s", *k, p, strerror(-r));
        }

        return 0;
}

static int user_start_service(User *u) {
        assert(u);

        return 0;
}

int user_start(User *u) {
        int r;

        assert(u);

        if (u->started)
                return 0;

        log_info("New user %s logged in.", u->name);

        /* Make XDG_RUNTIME_DIR */
        r = user_mkdir_runtime_path(u);
        if (r < 0)
                return r;

        /* Create cgroup */
        r = user_create_cgroup(u);
        if (r < 0)
                return r;

        /* Spawn user systemd */
        r = user_start_service(u);
        if (r < 0)
                return r;

        dual_timestamp_get(&u->timestamp);

        u->started = true;

        /* Save new user data */
        user_save(u);

        user_send_signal(u, true);

        return 0;
}

static int user_stop_service(User *u) {
        assert(u);

        if (!u->service)
                return 0;

        return 0;
}

static int user_shall_kill(User *u) {
        assert(u);

        if (!u->manager->kill_user_processes)
                return false;

        if (strv_contains(u->manager->kill_exclude_users, u->name))
                return false;

        if (strv_isempty(u->manager->kill_only_users))
                return true;

        return strv_contains(u->manager->kill_only_users, u->name);
}

static int user_terminate_cgroup(User *u) {
        int r;
        char **k;

        assert(u);

        if (!u->cgroup_path)
                return 0;

        cg_trim(SYSTEMD_CGROUP_CONTROLLER, u->cgroup_path, false);

        if (user_shall_kill(u)) {

                r = cg_kill_recursive_and_wait(SYSTEMD_CGROUP_CONTROLLER, u->cgroup_path, true);
                if (r < 0)
                        log_error("Failed to kill user cgroup: %s", strerror(-r));
        } else {

                r = cg_is_empty_recursive(SYSTEMD_CGROUP_CONTROLLER, u->cgroup_path, true);
                if (r < 0)
                        log_error("Failed to check user cgroup: %s", strerror(-r));
                else if (r > 0) {
                        r = cg_delete(SYSTEMD_CGROUP_CONTROLLER, u->cgroup_path);
                        if (r < 0)
                                log_error("Failed to delete user cgroup: %s", strerror(-r));
                } else
                        r = -EBUSY;
        }

        STRV_FOREACH(k, u->manager->controllers)
                cg_trim(*k, u->cgroup_path, true);

        free(u->cgroup_path);
        u->cgroup_path = NULL;

        return r;
}

static int user_remove_runtime_path(User *u) {
        int r;

        assert(u);

        if (!u->runtime_path)
                return 0;

        r = rm_rf(u->runtime_path, false, true, false);
        if (r < 0)
                log_error("Failed to remove runtime directory %s: %s", u->runtime_path, strerror(-r));

        free(u->runtime_path);
        u->runtime_path = NULL;

        return r;
}

int user_stop(User *u) {
        Session *s;
        int r = 0, k;
        assert(u);

        if (u->started)
                log_info("User %s logged out.", u->name);

        LIST_FOREACH(sessions_by_user, s, u->sessions) {
                k = session_stop(s);
                if (k < 0)
                        r = k;
        }

        /* Kill systemd */
        k = user_stop_service(u);
        if (k < 0)
                r = k;

        /* Kill cgroup */
        k = user_terminate_cgroup(u);
        if (k < 0)
                r = k;

        /* Kill XDG_RUNTIME_DIR */
        k = user_remove_runtime_path(u);
        if (k < 0)
                r = k;

        unlink(u->state_file);
        user_add_to_gc_queue(u);

        if (u->started)
                user_send_signal(u, false);

        u->started = false;

        return r;
}

int user_get_idle_hint(User *u, dual_timestamp *t) {
        Session *s;
        bool idle_hint = true;
        dual_timestamp ts = { 0, 0 };

        assert(u);

        LIST_FOREACH(sessions_by_user, s, u->sessions) {
                dual_timestamp k;
                int ih;

                ih = session_get_idle_hint(s, &k);
                if (ih < 0)
                        return ih;

                if (!ih) {
                        if (!idle_hint) {
                                if (k.monotonic < ts.monotonic)
                                        ts = k;
                        } else {
                                idle_hint = false;
                                ts = k;
                        }
                } else if (idle_hint) {

                        if (k.monotonic > ts.monotonic)
                                ts = k;
                }
        }

        if (t)
                *t = ts;

        return idle_hint;
}

int user_check_gc(User *u, bool drop_not_started) {
        int r;
        char *p;

        assert(u);

        if (drop_not_started && !u->started)
                return 0;

        if (u->sessions)
                return 1;

        if (asprintf(&p, "/var/lib/systemd/linger/%s", u->name) < 0)
                return -ENOMEM;

        r = access(p, F_OK) >= 0;
        free(p);

        if (r > 0)
                return 1;

        if (u->cgroup_path) {
                r = cg_is_empty_recursive(SYSTEMD_CGROUP_CONTROLLER, u->cgroup_path, false);
                if (r < 0)
                        return r;

                if (r <= 0)
                        return 1;
        }

        return 0;
}

void user_add_to_gc_queue(User *u) {
        assert(u);

        if (u->in_gc_queue)
                return;

        LIST_PREPEND(User, gc_queue, u->manager->user_gc_queue, u);
        u->in_gc_queue = true;
}

UserState user_get_state(User *u) {
        Session *i;

        assert(u);

        if (!u->sessions)
                return USER_LINGERING;

        LIST_FOREACH(sessions_by_user, i, u->sessions)
                if (session_is_active(i))
                        return USER_ACTIVE;

        return USER_ONLINE;
}

int user_kill(User *u, int signo) {
        int r = 0, q;
        Set *pid_set = NULL;

        assert(u);

        if (!u->cgroup_path)
                return -ESRCH;

        pid_set = set_new(trivial_hash_func, trivial_compare_func);
        if (!pid_set)
                return -ENOMEM;

        q = cg_kill_recursive(SYSTEMD_CGROUP_CONTROLLER, u->cgroup_path, signo, false, true, false, pid_set);
        if (q < 0)
                if (q != -EAGAIN && q != -ESRCH && q != -ENOENT)
                        r = q;

        if (pid_set)
                set_free(pid_set);

        return r;
}

static const char* const user_state_table[_USER_STATE_MAX] = {
        [USER_OFFLINE] = "offline",
        [USER_LINGERING] = "lingering",
        [USER_ONLINE] = "online",
        [USER_ACTIVE] = "active"
};

DEFINE_STRING_TABLE_LOOKUP(user_state, UserState);
