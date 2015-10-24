/*-*- Mode: C; c-basic-offset: 8; indent-tabs-mode: nil -*-*/

/***
  This file is part of systemd.

  Copyright 2013 Intel Corporation

  Author: Auke Kok <auke-jan.h.kok@intel.com>

  systemd is free software; you can redistribute it and/or modify it
  under the terms of the GNU Lesser General Public License as published by
  the Free Software Foundation; either version 2.1 of the License, or
  (at your option) any later version.

  systemd is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with systemd; If not, see <http://www.gnu.org/licenses/>.
***/

#include <unistd.h>
#include <string.h>
#include <sys/xattr.h>

#include "util.h"
#include "smack-util.h"

bool use_smack(void) {
#ifdef HAVE_SMACK
        static int use_smack_cached = -1;

        if (use_smack_cached < 0)
                use_smack_cached = access("/sys/fs/smackfs/", F_OK) >= 0;

        return use_smack_cached;
#else
        return false;
#endif

}

int smack_label_path(const char *path, const char *label) {
#ifdef HAVE_SMACK
        if (!use_smack())
                return 0;

        if (label)
                return setxattr(path, "security.SMACK64", label, strlen(label), 0);
        else
                return lremovexattr(path, "security.SMACK64");
#else
        return 0;
#endif
}

int smack_label_get_path(const char *path, char **label) {
        int i;
        int r = 0;
        char buf[SMACK_LABEL_LEN + 1];
        char *result = NULL;

        assert(path);

#ifdef HAVE_SMACK
        if (!use_smack())
                return 0;

        r = lgetxattr(path, "security.SMACK64", buf, SMACK_LABEL_LEN + 1);
        if (r < 0)
                return -errno;
        else if (buf[0] == '\0' || buf[0] == '-')
                return -EINVAL;

        result = calloc(r + 1, 1);
        if (result == NULL)
                 return -errno;

        for (i = 0; i < (SMACK_LABEL_LEN + 1) && buf[i]; i++) {
                if (buf[i] <= ' ' || buf[i] > '~')
                    return -EINVAL;
                switch (buf[i]) {
                case '/':
                case '"':
                case '\\':
                case '\'':
                        return -EINVAL;
                default:
                        break;
                }

                if (result)
                        result[i] = buf[i];
        }

        if (result && i < (SMACK_LABEL_LEN + 1))
                result[i] = '\0';

        if (i < 0) {
                free(result);
                return -EINVAL;
        }
        *label = result;

        return i;
#endif

        return r;
}

int smack_label_fd(int fd, const char *label) {
#ifdef HAVE_SMACK
        if (!use_smack())
                return 0;

        return fsetxattr(fd, "security.SMACK64", label, strlen(label), 0);
#else
        return 0;
#endif
}

int smack_label_apply_pid(pid_t pid, const char *label) {
        int r = 0;
        const char *p;

        assert(label);

#ifdef HAVE_SMACK
        if (!use_smack())
                return 0;

        p = procfs_file_alloca(pid, "attr/current");
        r = write_string_file(p, label);
        if (r < 0)
                return r;
#endif

        return r;
}

int smack_label_ip_out_fd(int fd, const char *label) {
#ifdef HAVE_SMACK
        if (!use_smack())
                return 0;

        return fsetxattr(fd, "security.SMACK64IPOUT", label, strlen(label), 0);
#else
        return 0;
#endif
}

int smack_label_ip_in_fd(int fd, const char *label) {
#ifdef HAVE_SMACK
        if (!use_smack())
                return 0;

        return fsetxattr(fd, "security.SMACK64IPIN", label, strlen(label), 0);
#else
        return 0;
#endif
}
