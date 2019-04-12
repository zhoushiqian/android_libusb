/*-*- Mode: C; c-basic-offset: 8; indent-tabs-mode: nil -*-*/

/***
  This file is part of systemd.

  Copyright 2010 Lennart Poettering

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

#include <assert.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>

#include "label.h"
#include "util.h"
#include "path-util.h"
#include "mkdir.h"

int mkdir_label(const char *path, mode_t mode) {
        return label_mkdir(path, mode, true);
}

static int mkdir_safe_internal(const char *path, mode_t mode, uid_t uid, gid_t gid, bool apply) {
        struct stat st;

        if (label_mkdir(path, mode, apply) >= 0)
                if (chmod_and_chown(path, mode, uid, gid) < 0)
                        return -errno;

        if (lstat(path, &st) < 0)
                return -errno;

        if ((st.st_mode & 0777) != mode ||
            st.st_uid != uid ||
            st.st_gid != gid ||
            !S_ISDIR(st.st_mode)) {
                errno = EEXIST;
                return -errno;
        }

        return 0;
}

int mkdir_safe(const char *path, mode_t mode, uid_t uid, gid_t gid) {
        return mkdir_safe_internal(path, mode, uid, gid, false);
}

int mkdir_safe_label(const char *path, mode_t mode, uid_t uid, gid_t gid) {
        return mkdir_safe_internal(path, mode, uid, gid, true);
}

static int is_dir(const char* path) {
        struct stat st;

        if (stat(path, &st) < 0)
                return -errno;

        return S_ISDIR(st.st_mode);
}

static int mkdir_parents_internal(const char *prefix, const char *path, mode_t mode, bool apply) {
        const char *p, *e;
        int r;

        assert(path);

        if (prefix && !path_startswith(path, prefix))
                return -ENOTDIR;

        /* return immediately if directory exists */
        e = strrchr(path, '/');
        if (!e)
                return -EINVAL;

        if (e == path)
                return 0;

        p = strndupa(path, e - path);
        r = is_dir(p);
        if (r > 0)
                return 0;
        if (r == 0)
                return -ENOTDIR;

        /* create every parent directory in the path, except the last component */
        p = path + strspn(path, "/");
        for (;;) {
                char t[strlen(path) + 1];

                e = p + strcspn(p, "/");
                p = e + strspn(e, "/");

                /* Is this the last component? If so, then we're
                 * done */
                if (*p == 0)
                        return 0;

                memcpy(t, path, e - path);
                t[e-path] = 0;

                if (prefix && path_startswith(prefix, t))
                        continue;

                r = label_mkdir(t, mode, apply);
                if (r < 0 && errno != EEXIST)
                        return -errno;
        }
}

int mkdir_parents(const char *path, mode_t mode) {
        return mkdir_parents_internal(NULL, path, mode, false);
}

int mkdir_parents_label(const char *path, mode_t mode) {
        return mkdir_parents_internal(NULL, path, mode, true);
}

int mkdir_parents_prefix(const char *prefix, const char *path, mode_t mode) {
        return mkdir_parents_internal(prefix, path, mode, true);
}

static int mkdir_p_internal(const char *prefix, const char *path, mode_t mode, bool apply) {
        int r;

        /* Like mkdir -p */

        r = mkdir_parents_internal(prefix, path, mode, apply);
        if (r < 0)
                return r;

        r = label_mkdir(path, mode, apply);
        if (r < 0 && (errno != EEXIST || is_dir(path) <= 0))
                return -errno;

        return 0;
}

int mkdir_p(const char *path, mode_t mode) {
        return mkdir_p_internal(NULL, path, mode, false);
}

int mkdir_p_label(const char *path, mode_t mode) {
        return mkdir_p_internal(NULL, path, mode, true);
}

int mkdir_p_prefix(const char *prefix, const char *path, mode_t mode) {
        return mkdir_p_internal(prefix, path, mode, false);
}
