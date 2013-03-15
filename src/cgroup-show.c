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

#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <errno.h>

#include "util.h"
#include "macro.h"
#include "cgroup-util.h"
#include "cgroup-show.h"

static int compare(const void *a, const void *b) {
        const pid_t *p = a, *q = b;

        if (*p < *q)
                return -1;
        if (*p > *q)
                return 1;
        return 0;
}

static unsigned ilog10(unsigned long ul) {
        int n = 0;

        while (ul > 0) {
                n++;
                ul /= 10;
        }

        return n;
}

static int show_cgroup_one_by_path(const char *path, const char *prefix, unsigned n_columns, bool more, bool kernel_threads) {
        char *fn;
        FILE *f;
        size_t n = 0, n_allocated = 0;
        pid_t *pids = NULL;
        char *p;
        pid_t pid, biggest = 0;
        int r;

        if (n_columns <= 0)
                n_columns = columns();

        if (!prefix)
                prefix = "";

        if ((r = cg_fix_path(path, &p)) < 0)
                return r;

        r = asprintf(&fn, "%s/cgroup.procs", p);
        free(p);

        if (r < 0)
                return -ENOMEM;

        f = fopen(fn, "re");
        free(fn);

        if (!f)
                return -errno;

        while ((r = cg_read_pid(f, &pid)) > 0) {

                if (!kernel_threads && is_kernel_thread(pid) > 0)
                        continue;

                if (n >= n_allocated) {
                        pid_t *npids;

                        n_allocated = MAX(16U, n*2U);

                        if (!(npids = realloc(pids, sizeof(pid_t) * n_allocated))) {
                                r = -ENOMEM;
                                goto finish;
                        }

                        pids = npids;
                }

                assert(n < n_allocated);
                pids[n++] = pid;

                if (pid > biggest)
                        biggest = pid;
        }

        if (r < 0)
                goto finish;

        if (n > 0) {
                unsigned i, m;

                /* Filter duplicates */
                m = 0;
                for (i = 0; i < n; i++) {
                        unsigned j;

                        for (j = i+1; j < n; j++)
                                if (pids[i] == pids[j])
                                        break;

                        if (j >= n)
                                pids[m++] = pids[i];
                }
                n = m;

                /* And sort */
                qsort(pids, n, sizeof(pid_t), compare);

                if (n_columns > 8)
                        n_columns -= 8;
                else
                        n_columns = 20;

                for (i = 0; i < n; i++) {
                        char *t = NULL;

                        get_process_cmdline(pids[i], n_columns, true, &t);

                        printf("%s%s %*lu %s\n",
                               prefix,
                               (more || i < n-1) ? "\342\224\234" : "\342\224\224",
                               (int) ilog10(biggest),
                               (unsigned long) pids[i],
                               strna(t));

                        free(t);
                }
        }

        r = 0;

finish:
        free(pids);

        if (f)
                fclose(f);

        return r;
}

int show_cgroup_by_path(const char *path, const char *prefix, unsigned n_columns, bool kernel_threads) {
        DIR *d;
        char *last = NULL;
        char *p1 = NULL, *p2 = NULL, *fn = NULL, *gn = NULL;
        bool shown_pids = false;
        int r;

        if (n_columns <= 0)
                n_columns = columns();

        if (!prefix)
                prefix = "";

        if ((r = cg_fix_path(path, &fn)) < 0)
                return r;

        if (!(d = opendir(fn))) {
                free(fn);
                return -errno;
        }

        while ((r = cg_read_subgroup(d, &gn)) > 0) {

                if (!shown_pids) {
                        show_cgroup_one_by_path(path, prefix, n_columns, true, kernel_threads);
                        shown_pids = true;
                }

                if (last) {
                        printf("%s\342\224\234 %s\n", prefix, file_name_from_path(last));

                        if (!p1)
                                if (!(p1 = strappend(prefix, "\342\224\202 "))) {
                                        r = -ENOMEM;
                                        goto finish;
                                }

                        show_cgroup_by_path(last, p1, n_columns-2, kernel_threads);

                        free(last);
                        last = NULL;
                }

                r = asprintf(&last, "%s/%s", fn, gn);
                free(gn);

                if (r < 0) {
                        r = -ENOMEM;
                        goto finish;
                }
        }

        if (r < 0)
                goto finish;

        if (!shown_pids)
                show_cgroup_one_by_path(path, prefix, n_columns, !!last, kernel_threads);

        if (last) {
                printf("%s\342\224\224 %s\n", prefix, file_name_from_path(last));

                if (!p2)
                        if (!(p2 = strappend(prefix, "  "))) {
                                r = -ENOMEM;
                                goto finish;
                        }

                show_cgroup_by_path(last, p2, n_columns-2, kernel_threads);
        }

        r = 0;

finish:
        free(p1);
        free(p2);
        free(last);
        free(fn);

        closedir(d);

        return r;
}

int show_cgroup(const char *controller, const char *path, const char *prefix, unsigned n_columns, bool kernel_threads) {
        char *p;
        int r;

        assert(controller);
        assert(path);

        r = cg_get_path(controller, path, NULL, &p);
        if (r < 0)
                return r;

        r = show_cgroup_by_path(p, prefix, n_columns, kernel_threads);
        free(p);

        return r;
}
