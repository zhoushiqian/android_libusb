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

#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <ftw.h>

#include "cgroup-util.h"
#include "log.h"
#include "set.h"
#include "macro.h"
#include "util.h"
#include "path-util.h"
#include "strv.h"
#include "unit-name.h"
#include "fileio.h"
#include "special.h"

int cg_enumerate_processes(const char *controller, const char *path, FILE **_f) {
        _cleanup_free_ char *fs = NULL;
        FILE *f;
        int r;

        assert(_f);

        r = cg_get_path(controller, path, "cgroup.procs", &fs);
        if (r < 0)
                return r;

        f = fopen(fs, "re");
        if (!f)
                return -errno;

        *_f = f;
        return 0;
}

int cg_read_pid(FILE *f, pid_t *_pid) {
        unsigned long ul;

        /* Note that the cgroup.procs might contain duplicates! See
         * cgroups.txt for details. */

        assert(f);
        assert(_pid);

        errno = 0;
        if (fscanf(f, "%lu", &ul) != 1) {

                if (feof(f))
                        return 0;

                return errno ? -errno : -EIO;
        }

        if (ul <= 0)
                return -EIO;

        *_pid = (pid_t) ul;
        return 1;
}

int cg_enumerate_subgroups(const char *controller, const char *path, DIR **_d) {
        _cleanup_free_ char *fs = NULL;
        int r;
        DIR *d;

        assert(_d);

        /* This is not recursive! */

        r = cg_get_path(controller, path, NULL, &fs);
        if (r < 0)
                return r;

        d = opendir(fs);
        if (!d)
                return -errno;

        *_d = d;
        return 0;
}

int cg_read_subgroup(DIR *d, char **fn) {
        struct dirent *de;

        assert(d);
        assert(fn);

        FOREACH_DIRENT(de, d, return -errno) {
                char *b;

                if (de->d_type != DT_DIR)
                        continue;

                if (streq(de->d_name, ".") ||
                    streq(de->d_name, ".."))
                        continue;

                b = strdup(de->d_name);
                if (!b)
                        return -ENOMEM;

                *fn = b;
                return 1;
        }

        return 0;
}

int cg_rmdir(const char *controller, const char *path) {
        _cleanup_free_ char *p = NULL;
        int r;

        r = cg_get_path(controller, path, NULL, &p);
        if (r < 0)
                return r;

        r = rmdir(p);
        if (r < 0 && errno != ENOENT)
                return -errno;

        return 0;
}

int cg_kill(const char *controller, const char *path, int sig, bool sigcont, bool ignore_self, Set *s) {
        _cleanup_set_free_ Set *allocated_set = NULL;
        bool done = false;
        int r, ret = 0;
        pid_t my_pid;

        assert(sig >= 0);

        /* This goes through the tasks list and kills them all. This
         * is repeated until no further processes are added to the
         * tasks list, to properly handle forking processes */

        if (!s) {
                s = allocated_set = set_new(trivial_hash_func, trivial_compare_func);
                if (!s)
                        return -ENOMEM;
        }

        my_pid = getpid();

        do {
                _cleanup_fclose_ FILE *f = NULL;
                pid_t pid = 0;
                done = true;

                r = cg_enumerate_processes(controller, path, &f);
                if (r < 0) {
                        if (ret >= 0 && r != -ENOENT)
                                return r;

                        return ret;
                }

                while ((r = cg_read_pid(f, &pid)) > 0) {

                        if (ignore_self && pid == my_pid)
                                continue;

                        if (set_get(s, LONG_TO_PTR(pid)) == LONG_TO_PTR(pid))
                                continue;

                        /* If we haven't killed this process yet, kill
                         * it */
                        if (kill(pid, sig) < 0) {
                                if (ret >= 0 && errno != ESRCH)
                                        ret = -errno;
                        } else if (ret == 0) {

                                if (sigcont)
                                        kill(pid, SIGCONT);

                                ret = 1;
                        }

                        done = false;

                        r = set_put(s, LONG_TO_PTR(pid));
                        if (r < 0) {
                                if (ret >= 0)
                                        return r;

                                return ret;
                        }
                }

                if (r < 0) {
                        if (ret >= 0)
                                return r;

                        return ret;
                }

                /* To avoid racing against processes which fork
                 * quicker than we can kill them we repeat this until
                 * no new pids need to be killed. */

        } while (!done);

        return ret;
}

int cg_kill_recursive(const char *controller, const char *path, int sig, bool sigcont, bool ignore_self, bool rem, Set *s) {
        _cleanup_set_free_ Set *allocated_set = NULL;
        _cleanup_closedir_ DIR *d = NULL;
        int r, ret = 0;
        char *fn;

        assert(path);
        assert(sig >= 0);

        if (!s) {
                s = allocated_set = set_new(trivial_hash_func, trivial_compare_func);
                if (!s)
                        return -ENOMEM;
        }

        ret = cg_kill(controller, path, sig, sigcont, ignore_self, s);

        r = cg_enumerate_subgroups(controller, path, &d);
        if (r < 0) {
                if (ret >= 0 && r != -ENOENT)
                        return r;

                return ret;
        }

        while ((r = cg_read_subgroup(d, &fn)) > 0) {
                _cleanup_free_ char *p = NULL;

                p = strjoin(path, "/", fn, NULL);
                free(fn);
                if (!p)
                        return -ENOMEM;

                r = cg_kill_recursive(controller, p, sig, sigcont, ignore_self, rem, s);
                if (ret >= 0 && r != 0)
                        ret = r;
        }

        if (ret >= 0 && r < 0)
                ret = r;

        if (rem) {
                r = cg_rmdir(controller, path);
                if (r < 0 && ret >= 0 && r != -ENOENT && r != -EBUSY)
                        return r;
        }

        return ret;
}

int cg_kill_recursive_and_wait(const char *controller, const char *path, bool rem) {
        unsigned i;

        assert(path);

        /* This safely kills all processes; first it sends a SIGTERM,
         * then checks 8 times after 200ms whether the group is now
         * empty, then kills everything that is left with SIGKILL and
         * finally checks 5 times after 200ms each whether the group
         * is finally empty. */

        for (i = 0; i < 15; i++) {
                int sig, r;

                if (i <= 0)
                        sig = SIGTERM;
                else if (i == 9)
                        sig = SIGKILL;
                else
                        sig = 0;

                r = cg_kill_recursive(controller, path, sig, true, true, rem, NULL);
                if (r <= 0)
                        return r;

                usleep(200 * USEC_PER_MSEC);
        }

        return 0;
}

int cg_migrate(const char *cfrom, const char *pfrom, const char *cto, const char *pto, bool ignore_self) {
        bool done = false;
        _cleanup_set_free_ Set *s = NULL;
        int r, ret = 0;
        pid_t my_pid;

        assert(cfrom);
        assert(pfrom);
        assert(cto);
        assert(pto);

        s = set_new(trivial_hash_func, trivial_compare_func);
        if (!s)
                return -ENOMEM;

        my_pid = getpid();

        do {
                _cleanup_fclose_ FILE *f = NULL;
                pid_t pid = 0;
                done = true;

                r = cg_enumerate_processes(cfrom, pfrom, &f);
                if (r < 0) {
                        if (ret >= 0 && r != -ENOENT)
                                return r;

                        return ret;
                }

                while ((r = cg_read_pid(f, &pid)) > 0) {

                        /* This might do weird stuff if we aren't a
                         * single-threaded program. However, we
                         * luckily know we are not */
                        if (ignore_self && pid == my_pid)
                                continue;

                        if (set_get(s, LONG_TO_PTR(pid)) == LONG_TO_PTR(pid))
                                continue;

                        r = cg_attach(cto, pto, pid);
                        if (r < 0) {
                                if (ret >= 0 && r != -ESRCH)
                                        ret = r;
                        } else if (ret == 0)
                                ret = 1;

                        done = false;

                        r = set_put(s, LONG_TO_PTR(pid));
                        if (r < 0) {
                                if (ret >= 0)
                                        return r;

                                return ret;
                        }
                }

                if (r < 0) {
                        if (ret >= 0)
                                return r;

                        return ret;
                }
        } while (!done);

        return ret;
}

int cg_migrate_recursive(
                const char *cfrom,
                const char *pfrom,
                const char *cto,
                const char *pto,
                bool ignore_self,
                bool rem) {

        _cleanup_closedir_ DIR *d = NULL;
        int r, ret = 0;
        char *fn;

        assert(cfrom);
        assert(pfrom);
        assert(cto);
        assert(pto);

        ret = cg_migrate(cfrom, pfrom, cto, pto, ignore_self);

        r = cg_enumerate_subgroups(cfrom, pfrom, &d);
        if (r < 0) {
                if (ret >= 0 && r != -ENOENT)
                        return r;

                return ret;
        }

        while ((r = cg_read_subgroup(d, &fn)) > 0) {
                _cleanup_free_ char *p = NULL;

                p = strjoin(pfrom, "/", fn, NULL);
                free(fn);
                if (!p) {
                        if (ret >= 0)
                                return -ENOMEM;

                        return ret;
                }

                r = cg_migrate_recursive(cfrom, p, cto, pto, ignore_self, rem);
                if (r != 0 && ret >= 0)
                        ret = r;
        }

        if (r < 0 && ret >= 0)
                ret = r;

        if (rem) {
                r = cg_rmdir(cfrom, pfrom);
                if (r < 0 && ret >= 0 && r != -ENOENT && r != -EBUSY)
                        return r;
        }

        return ret;
}

static const char *normalize_controller(const char *controller) {

        assert(controller);

        if (streq(controller, SYSTEMD_CGROUP_CONTROLLER))
                return "systemd";
        else if (startswith(controller, "name="))
                return controller + 5;
        else
                return controller;
}

static int join_path(const char *controller, const char *path, const char *suffix, char **fs) {
        char *t = NULL;

        if (!isempty(controller)) {
                if (!isempty(path) && !isempty(suffix))
                        t = strjoin("/sys/fs/cgroup/", controller, "/", path, "/", suffix, NULL);
                else if (!isempty(path))
                        t = strjoin("/sys/fs/cgroup/", controller, "/", path, NULL);
                else if (!isempty(suffix))
                        t = strjoin("/sys/fs/cgroup/", controller, "/", suffix, NULL);
                else
                        t = strappend("/sys/fs/cgroup/", controller);
        } else {
                if (!isempty(path) && !isempty(suffix))
                        t = strjoin(path, "/", suffix, NULL);
                else if (!isempty(path))
                        t = strdup(path);
                else
                        return -EINVAL;
        }

        if (!t)
                return -ENOMEM;

        path_kill_slashes(t);

        *fs = t;
        return 0;
}

int cg_get_path(const char *controller, const char *path, const char *suffix, char **fs) {
        const char *p;
        static __thread bool good = false;

        assert(fs);

        if (controller && !cg_controller_is_valid(controller, true))
                return -EINVAL;

        if (_unlikely_(!good)) {
                int r;

                r = path_is_mount_point("/sys/fs/cgroup", false);
                if (r <= 0)
                        return r < 0 ? r : -ENOENT;

                /* Cache this to save a few stat()s */
                good = true;
        }

        p = controller ? normalize_controller(controller) : NULL;

        return join_path(p, path, suffix, fs);
}

static int check_hierarchy(const char *p) {
        char *cc;

        assert(p);

        /* Check if this controller actually really exists */
        cc = alloca(sizeof("/sys/fs/cgroup/") + strlen(p));
        strcpy(stpcpy(cc, "/sys/fs/cgroup/"), p);
        if (access(cc, F_OK) < 0)
                return -errno;

        return 0;
}

int cg_get_path_and_check(const char *controller, const char *path, const char *suffix, char **fs) {
        const char *p;
        int r;

        assert(fs);

        if (!cg_controller_is_valid(controller, true))
                return -EINVAL;

        /* Normalize the controller syntax */
        p = normalize_controller(controller);

        /* Check if this controller actually really exists */
        r = check_hierarchy(p);
        if (r < 0)
                return r;

        return join_path(p, path, suffix, fs);
}

static int trim_cb(const char *path, const struct stat *sb, int typeflag, struct FTW *ftwbuf) {
        assert(path);
        assert(sb);
        assert(ftwbuf);

        if (typeflag != FTW_DP)
                return 0;

        if (ftwbuf->level < 1)
                return 0;

        rmdir(path);
        return 0;
}

int cg_trim(const char *controller, const char *path, bool delete_root) {
        _cleanup_free_ char *fs = NULL;
        int r = 0;

        assert(path);

        r = cg_get_path(controller, path, NULL, &fs);
        if (r < 0)
                return r;

        errno = 0;
        if (nftw(fs, trim_cb, 64, FTW_DEPTH|FTW_MOUNT|FTW_PHYS) != 0)
                r = errno ? -errno : -EIO;

        if (delete_root) {
                if (rmdir(fs) < 0 && errno != ENOENT)
                        return -errno;
        }

        return r;
}

int cg_delete(const char *controller, const char *path) {
        _cleanup_free_ char *parent = NULL;
        int r;

        assert(path);

        r = path_get_parent(path, &parent);
        if (r < 0)
                return r;

        r = cg_migrate_recursive(controller, path, controller, parent, false, true);
        return r == -ENOENT ? 0 : r;
}

int cg_attach(const char *controller, const char *path, pid_t pid) {
        _cleanup_free_ char *fs = NULL;
        char c[DECIMAL_STR_MAX(pid_t) + 2];
        int r;

        assert(path);
        assert(pid >= 0);

        r = cg_get_path_and_check(controller, path, "cgroup.procs", &fs);
        if (r < 0)
                return r;

        if (pid == 0)
                pid = getpid();

        snprintf(c, sizeof(c), "%lu\n", (unsigned long) pid);

        return write_string_file(fs, c);
}

int cg_set_group_access(
                const char *controller,
                const char *path,
                mode_t mode,
                uid_t uid,
                gid_t gid) {

        _cleanup_free_ char *fs = NULL;
        int r;

        assert(path);

        if (mode != (mode_t) -1)
                mode &= 0777;

        r = cg_get_path(controller, path, NULL, &fs);
        if (r < 0)
                return r;

        return chmod_and_chown(fs, mode, uid, gid);
}

int cg_set_task_access(
                const char *controller,
                const char *path,
                mode_t mode,
                uid_t uid,
                gid_t gid) {

        _cleanup_free_ char *fs = NULL, *procs = NULL;
        int r;

        assert(path);

        if (mode == (mode_t) -1 && uid == (uid_t) -1 && gid == (gid_t) -1)
                return 0;

        if (mode != (mode_t) -1)
                mode &= 0666;

        r = cg_get_path(controller, path, "cgroup.procs", &fs);
        if (r < 0)
                return r;

        r = chmod_and_chown(fs, mode, uid, gid);
        if (r < 0)
                return r;

        /* Compatibility, Always keep values for "tasks" in sync with
         * "cgroup.procs" */
        r = cg_get_path(controller, path, "tasks", &procs);
        if (r < 0)
                return r;

        return chmod_and_chown(procs, mode, uid, gid);
}

int cg_pid_get_path(const char *controller, pid_t pid, char **path) {
        _cleanup_fclose_ FILE *f = NULL;
        char line[LINE_MAX];
        const char *fs;
        size_t cs;

        assert(path);
        assert(pid >= 0);

        if (controller) {
                if (!cg_controller_is_valid(controller, true))
                        return -EINVAL;

                controller = normalize_controller(controller);
        } else
                controller = SYSTEMD_CGROUP_CONTROLLER;

        if (pid == 0)
                fs = "/proc/self/cgroup";
        else
                fs = procfs_file_alloca(pid, "cgroup");

        f = fopen(fs, "re");
        if (!f)
                return errno == ENOENT ? -ESRCH : -errno;

        cs = strlen(controller);

        FOREACH_LINE(line, f, return -errno) {
                char *l, *p, *w, *e;
                size_t k;
                char *state;
                bool found = false;

                truncate_nl(line);

                l = strchr(line, ':');
                if (!l)
                        continue;

                l++;
                e = strchr(l, ':');
                if (!e)
                        continue;

                *e = 0;

                FOREACH_WORD_SEPARATOR(w, k, l, ",", state) {

                        if (k == cs && memcmp(w, controller, cs) == 0) {
                                found = true;
                                break;
                        }

                        if (k == 5 + cs &&
                            memcmp(w, "name=", 5) == 0 &&
                            memcmp(w+5, controller, cs) == 0) {
                                found = true;
                                break;
                        }
                }

                if (!found)
                        continue;

                p = strdup(e + 1);
                if (!p)
                        return -ENOMEM;

                *path = p;
                return 0;
        }

        return -ENOENT;
}

int cg_install_release_agent(const char *controller, const char *agent) {
        _cleanup_free_ char *fs = NULL, *contents = NULL;
        char *sc;
        int r;

        assert(agent);

        r = cg_get_path(controller, NULL, "release_agent", &fs);
        if (r < 0)
                return r;

        r = read_one_line_file(fs, &contents);
        if (r < 0)
                return r;

        sc = strstrip(contents);
        if (sc[0] == 0) {
                r = write_string_file(fs, agent);
                if (r < 0)
                        return r;
        } else if (!streq(sc, agent))
                return -EEXIST;

        free(fs);
        fs = NULL;
        r = cg_get_path(controller, NULL, "notify_on_release", &fs);
        if (r < 0)
                return r;

        free(contents);
        contents = NULL;
        r = read_one_line_file(fs, &contents);
        if (r < 0)
                return r;

        sc = strstrip(contents);
        if (streq(sc, "0")) {
                r = write_string_file(fs, "1");
                if (r < 0)
                        return r;

                return 1;
        }

        if (!streq(sc, "1"))
                return -EIO;

        return 0;
}

int cg_uninstall_release_agent(const char *controller) {
        _cleanup_free_ char *fs = NULL;
        int r;

        r = cg_get_path(controller, NULL, "notify_on_release", &fs);
        if (r < 0)
                return r;

        r = write_string_file(fs, "0");
        if (r < 0)
                return r;

        free(fs);
        fs = NULL;

        r = cg_get_path(controller, NULL, "release_agent", &fs);
        if (r < 0)
                return r;

        r = write_string_file(fs, "");
        if (r < 0)
                return r;

        return 0;
}

int cg_is_empty(const char *controller, const char *path, bool ignore_self) {
        _cleanup_fclose_ FILE *f = NULL;
        pid_t pid = 0, self_pid;
        bool found = false;
        int r;

        assert(path);

        r = cg_enumerate_processes(controller, path, &f);
        if (r < 0)
                return r == -ENOENT ? 1 : r;

        self_pid = getpid();

        while ((r = cg_read_pid(f, &pid)) > 0) {

                if (ignore_self && pid == self_pid)
                        continue;

                found = true;
                break;
        }

        if (r < 0)
                return r;

        return !found;
}

int cg_is_empty_by_spec(const char *spec, bool ignore_self) {
        _cleanup_free_ char *controller = NULL, *path = NULL;
        int r;

        assert(spec);

        r = cg_split_spec(spec, &controller, &path);
        if (r < 0)
                return r;

        return cg_is_empty(controller, path, ignore_self);
}

int cg_is_empty_recursive(const char *controller, const char *path, bool ignore_self) {
        _cleanup_closedir_ DIR *d = NULL;
        char *fn;
        int r;

        assert(path);

        r = cg_is_empty(controller, path, ignore_self);
        if (r <= 0)
                return r;

        r = cg_enumerate_subgroups(controller, path, &d);
        if (r < 0)
                return r == -ENOENT ? 1 : r;

        while ((r = cg_read_subgroup(d, &fn)) > 0) {
                _cleanup_free_ char *p = NULL;

                p = strjoin(path, "/", fn, NULL);
                free(fn);
                if (!p)
                        return -ENOMEM;

                r = cg_is_empty_recursive(controller, p, ignore_self);
                if (r <= 0)
                        return r;
        }

        if (r < 0)
                return r;

        return 1;
}

int cg_split_spec(const char *spec, char **controller, char **path) {
        const char *e;
        char *t = NULL, *u = NULL;
        _cleanup_free_ char *v = NULL;

        assert(spec);

        if (*spec == '/') {
                if (!path_is_safe(spec))
                        return -EINVAL;

                if (path) {
                        t = strdup(spec);
                        if (!t)
                                return -ENOMEM;

                        path_kill_slashes(t);
                        *path = t;
                }

                if (controller)
                        *controller = NULL;

                return 0;
        }

        e = strchr(spec, ':');
        if (!e) {
                if (!cg_controller_is_valid(spec, true))
                        return -EINVAL;

                if (controller) {
                        t = strdup(normalize_controller(spec));
                        if (!t)
                                return -ENOMEM;

                        *controller = t;
                }

                if (path)
                        *path = NULL;

                return 0;
        }

        v = strndup(spec, e-spec);
        if (!v)
                return -ENOMEM;
        t = strdup(normalize_controller(v));
        if (!t)
                return -ENOMEM;
        if (!cg_controller_is_valid(t, true)) {
                free(t);
                return -EINVAL;
        }

        u = strdup(e+1);
        if (!u) {
                free(t);
                return -ENOMEM;
        }
        if (!path_is_safe(u) ||
            !path_is_absolute(u)) {
                free(t);
                free(u);
                return -EINVAL;
        }

        path_kill_slashes(u);

        if (controller)
                *controller = t;
        else
                free(t);

        if (path)
                *path = u;
        else
                free(u);

        return 0;
}

int cg_join_spec(const char *controller, const char *path, char **spec) {
        char *s;

        assert(path);

        if (!controller)
                controller = "systemd";
        else {
                if (!cg_controller_is_valid(controller, true))
                        return -EINVAL;

                controller = normalize_controller(controller);
        }

        if (!path_is_absolute(path))
                return -EINVAL;

        s = strjoin(controller, ":", path, NULL);
        if (!s)
                return -ENOMEM;

        path_kill_slashes(s + strlen(controller) + 1);

        *spec = s;
        return 0;
}

int cg_mangle_path(const char *path, char **result) {
        _cleanup_free_ char *c = NULL, *p = NULL;
        char *t;
        int r;

        assert(path);
        assert(result);

        /* First check if it already is a filesystem path */
        if (path_startswith(path, "/sys/fs/cgroup")) {

                t = strdup(path);
                if (!t)
                        return -ENOMEM;

                path_kill_slashes(t);
                *result = t;
                return 0;
        }

        /* Otherwise treat it as cg spec */
        r = cg_split_spec(path, &c, &p);
        if (r < 0)
                return r;

        return cg_get_path(c ? c : SYSTEMD_CGROUP_CONTROLLER, p ? p : "/", NULL, result);
}

int cg_get_root_path(char **path) {
        char *p, *e;
        int r;

        assert(path);

        r = cg_pid_get_path(SYSTEMD_CGROUP_CONTROLLER, 1, &p);
        if (r < 0)
                return r;

        e = endswith(p, "/" SPECIAL_SYSTEM_SLICE);
        if (e)
                *e = 0;

        *path = p;
        return 0;
}

char **cg_shorten_controllers(char **controllers) {
        char **f, **t;

        if (!controllers)
                return controllers;

        for (f = controllers, t = controllers; *f; f++) {
                const char *p;
                int r;

                p = normalize_controller(*f);

                if (streq(p, "systemd")) {
                        free(*f);
                        continue;
                }

                if (!cg_controller_is_valid(p, true)) {
                        log_warning("Controller %s is not valid, removing from controllers list.", p);
                        free(*f);
                        continue;
                }

                r = check_hierarchy(p);
                if (r < 0) {
                        log_debug("Controller %s is not available, removing from controllers list.", p);
                        free(*f);
                        continue;
                }

                *(t++) = *f;
        }

        *t = NULL;
        return strv_uniq(controllers);
}

int cg_pid_get_path_shifted(pid_t pid, char **root, char **cgroup) {
        _cleanup_free_ char *cg_root = NULL;
        char *cg_process, *p;
        int r;

        r = cg_get_root_path(&cg_root);
        if (r < 0)
                return r;

        r = cg_pid_get_path(SYSTEMD_CGROUP_CONTROLLER, pid, &cg_process);
        if (r < 0)
                return r;

        p = path_startswith(cg_process, cg_root);
        if (p)
                p--;
        else
                p = cg_process;

        if (cgroup) {
                char* c;

                c = strdup(p);
                if (!c) {
                        free(cg_process);
                        return -ENOMEM;
                }

                *cgroup = c;
        }

        if (root) {
                cg_process[p-cg_process] = 0;
                *root = cg_process;
        } else
                free(cg_process);

        return 0;
}

int cg_path_decode_unit(const char *cgroup, char **unit){
        char *e, *c, *s;

        assert(cgroup);
        assert(unit);

        e = strchrnul(cgroup, '/');
        c = strndupa(cgroup, e - cgroup);
        c = cg_unescape(c);

        if (!unit_name_is_valid(c, false))
                return -EINVAL;

        s = strdup(c);
        if (!s)
                return -ENOMEM;

        *unit = s;
        return 0;
}

static const char *skip_slices(const char *p) {
        /* Skips over all slice assignments */

        for (;;) {
                size_t n;

                p += strspn(p, "/");

                n = strcspn(p, "/");
                if (n <= 6 || memcmp(p + n - 6, ".slice", 6) != 0)
                        return p;

                p += n;
        }
}

int cg_path_get_unit(const char *path, char **unit) {
        const char *e;

        assert(path);
        assert(unit);

        e = skip_slices(path);

        return cg_path_decode_unit(e, unit);
}

int cg_pid_get_unit(pid_t pid, char **unit) {
        _cleanup_free_ char *cgroup = NULL;
        int r;

        assert(unit);

        r = cg_pid_get_path_shifted(pid, NULL, &cgroup);
        if (r < 0)
                return r;

        return cg_path_get_unit(cgroup, unit);
}

static const char *skip_session(const char *p) {
        size_t n;

        assert(p);

        p += strspn(p, "/");

        n = strcspn(p, "/");
        if (n <= 12 || memcmp(p, "session-", 8) != 0 || memcmp(p + n - 6, ".scope", 6) != 0)
                return NULL;

        p += n;
        p += strspn(p, "/");

        return p;
}

int cg_path_get_user_unit(const char *path, char **unit) {
        const char *e;

        assert(path);
        assert(unit);

        /* We always have to parse the path from the beginning as unit
         * cgroups might have arbitrary child cgroups and we shouldn't get
         * confused by those */

        /* Skip slices, if there are any */
        e = skip_slices(path);

        /* Skip the session scope, require that there is one */
        e = skip_session(e);
        if (!e)
                return -ENOENT;

        /* And skip more slices */
        e = skip_slices(e);

        return cg_path_decode_unit(e, unit);
}

int cg_pid_get_user_unit(pid_t pid, char **unit) {
        _cleanup_free_ char *cgroup = NULL;
        int r;

        assert(unit);

        r = cg_pid_get_path_shifted(pid, NULL, &cgroup);
        if (r < 0)
                return r;

        return cg_path_get_user_unit(cgroup, unit);
}

int cg_path_get_machine_name(const char *path, char **machine) {
        const char *e, *n, *x;
        char *s, *r;
        size_t l;

        assert(path);
        assert(machine);

        /* Skip slices, if there are any */
        e = skip_slices(path);

        n = strchrnul(e, '/');
        if (e == n)
                return -ENOENT;

        s = strndupa(e, n - e);
        s = cg_unescape(s);

        x = startswith(s, "machine-");
        if (!x)
                return -ENOENT;
        if (!endswith(x, ".scope"))
                return -ENOENT;

        l = strlen(x);
        if (l <= 6)
                return -ENOENT;

        r = strndup(x, l - 6);
        if (!r)
                return -ENOMEM;

        *machine = r;
        return 0;
}

int cg_pid_get_machine_name(pid_t pid, char **machine) {
        _cleanup_free_ char *cgroup = NULL;
        int r;

        assert(machine);

        r = cg_pid_get_path_shifted(pid, NULL, &cgroup);
        if (r < 0)
                return r;

        return cg_path_get_machine_name(cgroup, machine);
}

int cg_path_get_session(const char *path, char **session) {
        const char *e, *n, *x;
        char *s, *r;
        size_t l;

        assert(path);
        assert(session);

        /* Skip slices, if there are any */
        e = skip_slices(path);

        n = strchrnul(e, '/');
        if (e == n)
                return -ENOENT;

        s = strndupa(e, n - e);
        s = cg_unescape(s);

        x = startswith(s, "session-");
        if (!x)
                return -ENOENT;
        if (!endswith(x, ".scope"))
                return -ENOENT;

        l = strlen(x);
        if (l <= 6)
                return -ENOENT;

        r = strndup(x, l - 6);
        if (!r)
                return -ENOMEM;

        *session = r;
        return 0;
}

int cg_pid_get_session(pid_t pid, char **session) {
        _cleanup_free_ char *cgroup = NULL;
        int r;

        assert(session);

        r = cg_pid_get_path_shifted(pid, NULL, &cgroup);
        if (r < 0)
                return r;

        return cg_path_get_session(cgroup, session);
}

int cg_path_get_owner_uid(const char *path, uid_t *uid) {
        _cleanup_free_ char *slice = NULL;
        const char *e;
        char *s;
        int r;

        assert(path);
        assert(uid);

        r = cg_path_get_slice(path, &slice);
        if (r < 0)
                return r;

        e = startswith(slice, "user-");
        if (!e)
                return -ENOENT;
        if (!endswith(slice, ".slice"))
                return -ENOENT;

        s = strndupa(e, strlen(e) - 6);
        if (!s)
                return -ENOMEM;

        return parse_uid(s, uid);
}

int cg_pid_get_owner_uid(pid_t pid, uid_t *uid) {
        _cleanup_free_ char *cgroup = NULL;
        int r;

        assert(uid);

        r = cg_pid_get_path_shifted(pid, NULL, &cgroup);
        if (r < 0)
                return r;

        return cg_path_get_owner_uid(cgroup, uid);
}

int cg_path_get_slice(const char *p, char **slice) {
        const char *e = NULL;
        size_t m = 0;

        assert(p);
        assert(slice);

        for (;;) {
                size_t n;

                p += strspn(p, "/");

                n = strcspn(p, "/");
                if (n <= 6 || memcmp(p + n - 6, ".slice", 6) != 0) {
                        char *s;

                        if (!e)
                                return -ENOENT;

                        s = strndup(e, m);
                        if (!s)
                                return -ENOMEM;

                        *slice = s;
                        return 0;
                }

                e = p;
                m = n;

                p += n;
        }
}

int cg_pid_get_slice(pid_t pid, char **slice) {
        _cleanup_free_ char *cgroup = NULL;
        int r;

        assert(slice);

        r = cg_pid_get_path_shifted(pid, NULL, &cgroup);
        if (r < 0)
                return r;

        return cg_path_get_slice(cgroup, slice);
}

int cg_controller_from_attr(const char *attr, char **controller) {
        const char *dot;
        char *c;

        assert(attr);
        assert(controller);

        if (!filename_is_safe(attr))
                return -EINVAL;

        dot = strchr(attr, '.');
        if (!dot) {
                *controller = NULL;
                return 0;
        }

        c = strndup(attr, dot - attr);
        if (!c)
                return -ENOMEM;

        if (!cg_controller_is_valid(c, false)) {
                free(c);
                return -EINVAL;
        }

        *controller = c;
        return 1;
}

char *cg_escape(const char *p) {
        bool need_prefix = false;

        /* This implements very minimal escaping for names to be used
         * as file names in the cgroup tree: any name which might
         * conflict with a kernel name or is prefixed with '_' is
         * prefixed with a '_'. That way, when reading cgroup names it
         * is sufficient to remove a single prefixing underscore if
         * there is one. */

        /* The return value of this function (unlike cg_unescape())
         * needs free()! */

        if (p[0] == 0 ||
            p[0] == '_' ||
            p[0] == '.' ||
            streq(p, "notify_on_release") ||
            streq(p, "release_agent") ||
            streq(p, "tasks"))
                need_prefix = true;
        else {
                const char *dot;

                dot = strrchr(p, '.');
                if (dot) {

                        if (dot - p == 6 && memcmp(p, "cgroup", 6) == 0)
                                need_prefix = true;
                        else {
                                char *n;

                                n = strndupa(p, dot - p);

                                if (check_hierarchy(n) >= 0)
                                        need_prefix = true;
                        }
                }
        }

        if (need_prefix)
                return strappend("_", p);
        else
                return strdup(p);
}

char *cg_unescape(const char *p) {
        assert(p);

        /* The return value of this function (unlike cg_escape())
         * doesn't need free()! */

        if (p[0] == '_')
                return (char*) p+1;

        return (char*) p;
}

#define CONTROLLER_VALID                        \
        "0123456789"                            \
        "abcdefghijklmnopqrstuvwxyz"            \
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"            \
        "_"

bool cg_controller_is_valid(const char *p, bool allow_named) {
        const char *t, *s;

        if (!p)
                return false;

        if (allow_named) {
                s = startswith(p, "name=");
                if (s)
                        p = s;
        }

        if (*p == 0 || *p == '_')
                return false;

        for (t = p; *t; t++)
                if (!strchr(CONTROLLER_VALID, *t))
                        return false;

        if (t - p > FILENAME_MAX)
                return false;

        return true;
}

int cg_slice_to_path(const char *unit, char **ret) {
        _cleanup_free_ char *p = NULL, *s = NULL, *e = NULL;
        const char *dash;

        assert(unit);
        assert(ret);

        if (!unit_name_is_valid(unit, false))
                return -EINVAL;

        if (!endswith(unit, ".slice"))
                return -EINVAL;

        p = unit_name_to_prefix(unit);
        if (!p)
                return -ENOMEM;

        dash = strchr(p, '-');
        while (dash) {
                _cleanup_free_ char *escaped = NULL;
                char n[dash - p + sizeof(".slice")];

                strcpy(stpncpy(n, p, dash - p), ".slice");

                if (!unit_name_is_valid(n, false))
                        return -EINVAL;

                escaped = cg_escape(n);
                if (!escaped)
                        return -ENOMEM;

                if (!strextend(&s, escaped, "/", NULL))
                        return -ENOMEM;

                dash = strchr(dash+1, '-');
        }

        e = cg_escape(unit);
        if (!e)
                return -ENOMEM;

        if (!strextend(&s, e, NULL))
                return -ENOMEM;

        *ret = s;
        s = NULL;

        return 0;
}

int cg_set_attribute(const char *controller, const char *path, const char *attribute, const char *value) {
        _cleanup_free_ char *p = NULL;
        int r;

        r = cg_get_path(controller, path, attribute, &p);
        if (r < 0)
                return r;

        return write_string_file(p, value);
}

static const char mask_names[] =
        "cpu\0"
        "cpuacct\0"
        "blkio\0"
        "memory\0"
        "devices\0";

int cg_create_with_mask(CGroupControllerMask mask, const char *path) {
        CGroupControllerMask bit = 1;
        const char *n;
        int r;

        /* This one will create a cgroup in our private tree, but also
         * duplicate it in the trees specified in mask, and remove it
         * in all others */

        /* First create the cgroup in our own hierarchy. */
        r = cg_create(SYSTEMD_CGROUP_CONTROLLER, path);
        if (r < 0)
                return r;

        /* Then, do the same in the other hierarchies */
        NULSTR_FOREACH(n, mask_names) {
                if (bit & mask)
                        cg_create(n, path);
                else
                        cg_trim(n, path, true);

                bit <<= 1;
        }

        return r;
}

int cg_attach_with_mask(CGroupControllerMask mask, const char *path, pid_t pid) {
        CGroupControllerMask bit = 1;
        const char *n;
        int r;

        r = cg_attach(SYSTEMD_CGROUP_CONTROLLER, path, pid);

        NULSTR_FOREACH(n, mask_names) {
                if (bit & mask)
                        cg_attach(n, path, pid);
                else {
                        char prefix[strlen(path) + 1], *slash;

                        /* OK, this one is a bit harder... Now we need
                         * to add to the closest parent cgroup we
                         * can find */
                        strcpy(prefix, path);
                        while ((slash = strrchr(prefix, '/'))) {
                                int q;
                                *slash = 0;

                                q = cg_attach(n, prefix, pid);
                                if (q >= 0)
                                        break;
                        }
                }

                bit <<= 1;
        }

        return r;
}

int cg_attach_many_with_mask(CGroupControllerMask mask, const char *path, Set* pids) {
        Iterator i;
        void *pidp;
        int r = 0;

        SET_FOREACH(pidp, pids, i) {
                pid_t pid = PTR_TO_LONG(pidp);
                int k;

                k = cg_attach_with_mask(mask, path, pid);
                if (k < 0)
                        r = k;
        }

        return r;
}

int cg_migrate_with_mask(CGroupControllerMask mask, const char *from, const char *to) {
        CGroupControllerMask bit = 1;
        const char *n;
        int r;

        if (path_equal(from, to))
                return 0;

        r = cg_migrate_recursive(SYSTEMD_CGROUP_CONTROLLER, from, SYSTEMD_CGROUP_CONTROLLER, to, false, true);

        NULSTR_FOREACH(n, mask_names) {
                if (bit & mask)
                        cg_migrate_recursive(SYSTEMD_CGROUP_CONTROLLER, to, n, to, false, false);
                else {
                        char prefix[strlen(to) + 1], *slash;

                        strcpy(prefix, to);
                        while ((slash = strrchr(prefix, '/'))) {
                                int q;

                                *slash = 0;

                                q = cg_migrate_recursive(SYSTEMD_CGROUP_CONTROLLER, to, n, prefix, false, false);
                                if (q >= 0)
                                        break;
                        }
                }

                bit <<= 1;
        }

        return r;
}

int cg_trim_with_mask(CGroupControllerMask mask, const char *path, bool delete_root) {
        CGroupControllerMask bit = 1;
        const char *n;
        int r;

        r = cg_trim(SYSTEMD_CGROUP_CONTROLLER, path, delete_root);
        if (r < 0)
                return r;

        NULSTR_FOREACH(n, mask_names) {
                if (bit & mask)
                        cg_trim(n, path, delete_root);

                bit <<= 1;
        }

        return r;
}

CGroupControllerMask cg_mask_supported(void) {
        CGroupControllerMask bit = 1, mask = 0;
        const char *n;

        NULSTR_FOREACH(n, mask_names) {
                if (check_hierarchy(n) >= 0)
                        mask |= bit;

                bit <<= 1;
        }

        return mask;
}
