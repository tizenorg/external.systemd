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
#include <string.h>
#include <assert.h>

#include "util.h"
#include "unit-name.h"

#define VALID_CHARS                             \
        "0123456789"                            \
        "abcdefghijklmnopqrstuvwxyz"            \
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"            \
        ":-_.\\"

bool unit_name_is_valid_no_type(const char *n, bool template_ok) {
        const char *e, *i, *at;

        /* Valid formats:
         *
         *         string@instance.suffix
         *         string.suffix
         */

        assert(n);

        if (strlen(n) >= UNIT_NAME_MAX)
                return false;

        e = strrchr(n, '.');
        if (!e || e == n)
                return false;

        for (i = n, at = NULL; i < e; i++) {

                if (*i == '@' && !at)
                        at = i;

                if (!strchr("@" VALID_CHARS, *i))
                        return false;
        }

        if (at) {
                if (at == n)
                        return false;

                if (!template_ok && at+1 == e)
                        return false;
        }

        return true;
}

bool unit_instance_is_valid(const char *i) {
        assert(i);

        /* The max length depends on the length of the string, so we
         * don't really check this here. */

        if (i[0] == 0)
                return false;

        /* We allow additional @ in the instance string, we do not
         * allow them in the prefix! */

        for (; *i; i++)
                if (!strchr("@" VALID_CHARS, *i))
                        return false;

        return true;
}

bool unit_prefix_is_valid(const char *p) {

        /* We don't allow additional @ in the instance string */

        if (p[0] == 0)
                return false;

        for (; *p; p++)
                if (!strchr(VALID_CHARS, *p))
                        return false;

        return true;
}

int unit_name_to_instance(const char *n, char **instance) {
        const char *p, *d;
        char *i;

        assert(n);
        assert(instance);

        /* Everything past the first @ and before the last . is the instance */
        if (!(p = strchr(n, '@'))) {
                *instance = NULL;
                return 0;
        }

        assert_se(d = strrchr(n, '.'));
        assert(p < d);

        if (!(i = strndup(p+1, d-p-1)))
                return -ENOMEM;

        *instance = i;
        return 0;
}

char *unit_name_to_prefix_and_instance(const char *n) {
        const char *d;

        assert(n);

        assert_se(d = strrchr(n, '.'));

        return strndup(n, d - n);
}

char *unit_name_to_prefix(const char *n) {
        const char *p;

        if ((p = strchr(n, '@')))
                return strndup(n, p - n);

        return unit_name_to_prefix_and_instance(n);
}

char *unit_name_change_suffix(const char *n, const char *suffix) {
        char *e, *r;
        size_t a, b;

        assert(n);
        assert(unit_name_is_valid_no_type(n, true));
        assert(suffix);

        assert_se(e = strrchr(n, '.'));
        a = e - n;
        b = strlen(suffix);

        if (!(r = new(char, a + b + 1)))
                return NULL;

        memcpy(r, n, a);
        memcpy(r+a, suffix, b+1);

        return r;
}

char *unit_name_build(const char *prefix, const char *instance, const char *suffix) {
        assert(prefix);
        assert(unit_prefix_is_valid(prefix));
        assert(!instance || unit_instance_is_valid(instance));
        assert(suffix);

        if (!instance)
                return strappend(prefix, suffix);

        return join(prefix, "@", instance, suffix, NULL);
}

static char* do_escape(const char *f, char *t) {
        assert(f);
        assert(t);

        for (; *f; f++) {
                if (*f == '/')
                        *(t++) = '-';
                else if (*f == '-' || *f == '\\' || !strchr(VALID_CHARS, *f)) {
                        *(t++) = '\\';
                        *(t++) = 'x';
                        *(t++) = hexchar(*f >> 4);
                        *(t++) = hexchar(*f);
                } else
                        *(t++) = *f;
        }

        return t;
}

char *unit_name_build_escape(const char *prefix, const char *instance, const char *suffix) {
        char *r, *t;
        size_t a, b, c;

        assert(prefix);
        assert(suffix);

        /* Takes a arbitrary string for prefix and instance plus a
         * suffix and makes a nice string suitable as unit name of it,
         * escaping all weird chars on the way.
         *
         * / becomes ., and all chars not allowed in a unit name get
         * escaped as \xFF, including \ and ., of course. This
         * escaping is hence reversible.
         *
         * This is primarily useful to make nice unit names from
         * strings, but is actually useful for any kind of string.
         */

        a = strlen(prefix);
        c = strlen(suffix);

        if (instance) {
                b = strlen(instance);

                if (!(r = new(char, a*4 + 1 + b*4 + c + 1)))
                        return NULL;

                t = do_escape(prefix, r);
                *(t++) = '@';
                t = do_escape(instance, t);
        } else {

                if (!(r = new(char, a*4 + c + 1)))
                        return NULL;

                t = do_escape(prefix, r);
        }

        strcpy(t, suffix);
        return r;
}

char *unit_name_escape(const char *f) {
        char *r, *t;

        if (!(r = new(char, strlen(f)*4+1)))
                return NULL;

        t = do_escape(f, r);
        *t = 0;

        return r;

}

char *unit_name_unescape(const char *f) {
        char *r, *t;

        assert(f);

        if (!(r = strdup(f)))
                return NULL;

        for (t = r; *f; f++) {
                if (*f == '-')
                        *(t++) = '/';
                else if (*f == '\\') {
                        int a, b;

                        if (f[1] != 'x' ||
                            (a = unhexchar(f[2])) < 0 ||
                            (b = unhexchar(f[3])) < 0) {
                                /* Invalid escape code, let's take it literal then */
                                *(t++) = '\\';
                        } else {
                                *(t++) = (char) ((a << 4) | b);
                                f += 3;
                        }
                } else
                        *(t++) = *f;
        }

        *t = 0;

        return r;
}

bool unit_name_is_template(const char *n) {
        const char *p;

        assert(n);

        if (!(p = strchr(n, '@')))
                return false;

        return p[1] == '.';
}

char *unit_name_replace_instance(const char *f, const char *i) {
        const char *p, *e;
        char *r, *k;
        size_t a;

        assert(f);

        p = strchr(f, '@');
        assert_se(e = strrchr(f, '.'));

        a = p - f;

        if (p) {
                size_t b;

                b = strlen(i);

                if (!(r = new(char, a + 1 + b + strlen(e) + 1)))
                        return NULL;

                k = mempcpy(r, f, a + 1);
                k = mempcpy(k, i, b);
        } else {

                if (!(r = new(char, a + strlen(e) + 1)))
                        return NULL;

                k = mempcpy(r, f, a);
        }

        strcpy(k, e);
        return r;
}

char *unit_name_template(const char *f) {
        const char *p, *e;
        char *r;
        size_t a;

        if (!(p = strchr(f, '@')))
                return strdup(f);

        assert_se(e = strrchr(f, '.'));
        a = p - f + 1;

        if (!(r = new(char, a + strlen(e) + 1)))
                return NULL;

        strcpy(mempcpy(r, f, a), e);
        return r;

}

char *unit_name_from_path(const char *path, const char *suffix) {
        char *p, *r;

        assert(path);
        assert(suffix);

        if (!(p = strdup(path)))
                return NULL;

        path_kill_slashes(p);

        path = p[0] == '/' ? p + 1 : p;

        if (path[0] == 0) {
                free(p);
                return strappend("-", suffix);
        }

        r = unit_name_build_escape(path, NULL, suffix);
        free(p);

        return r;
}

char *unit_name_from_path_instance(const char *prefix, const char *path, const char *suffix) {
        char *p, *r;

        assert(path);
        assert(suffix);

        if (!(p = strdup(path)))
                return NULL;

        path_kill_slashes(p);

        path = p[0] == '/' ? p + 1 : p;

        if (path[0] == 0) {
                free(p);
                return unit_name_build_escape(prefix, "-", suffix);
        }

        r = unit_name_build_escape(prefix, path, suffix);
        free(p);

        return r;
}

char *unit_name_to_path(const char *name) {
        char *w, *e;

        assert(name);

        if (!(w = unit_name_to_prefix(name)))
                return NULL;

        e = unit_name_unescape(w);
        free(w);

        if (!e)
                return NULL;

        if (e[0] != '/') {
                w = strappend("/", e);
                free(e);

                if (!w)
                        return NULL;

                e = w;
        }

        return e;
}

char *unit_name_path_unescape(const char *f) {
        char *e;

        assert(f);

        if (!(e = unit_name_unescape(f)))
                return NULL;

        if (e[0] != '/') {
                char *w;

                w = strappend("/", e);
                free(e);

                if (!w)
                        return NULL;

                e = w;
        }

        return e;
}
