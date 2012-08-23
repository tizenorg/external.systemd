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

#include <assert.h>
#include <errno.h>
#include <sys/timerfd.h>
#include <sys/epoll.h>

#include "set.h"
#include "unit.h"
#include "macro.h"
#include "strv.h"
#include "load-fragment.h"
#include "load-dropin.h"
#include "log.h"
#include "dbus-job.h"

Job* job_new(Manager *m, JobType type, Unit *unit) {
        Job *j;

        assert(m);
        assert(type < _JOB_TYPE_MAX);
        assert(unit);

        if (!(j = new0(Job, 1)))
                return NULL;

        j->manager = m;
        j->id = m->current_job_id++;
        j->type = type;
        j->unit = unit;

        j->timer_watch.type = WATCH_INVALID;

        /* We don't link it here, that's what job_dependency() is for */

        return j;
}

void job_free(Job *j) {
        assert(j);

        /* Detach from next 'bigger' objects */
        if (j->installed) {
                bus_job_send_removed_signal(j);

                if (j->unit->meta.job == j) {
                        j->unit->meta.job = NULL;
                        unit_add_to_gc_queue(j->unit);
                }

                hashmap_remove(j->manager->jobs, UINT32_TO_PTR(j->id));
                j->installed = false;
        }

        /* Detach from next 'smaller' objects */
        manager_transaction_unlink_job(j->manager, j, true);

        if (j->in_run_queue)
                LIST_REMOVE(Job, run_queue, j->manager->run_queue, j);

        if (j->in_dbus_queue)
                LIST_REMOVE(Job, dbus_queue, j->manager->dbus_job_queue, j);

        if (j->timer_watch.type != WATCH_INVALID) {
                assert(j->timer_watch.type == WATCH_JOB_TIMER);
                assert(j->timer_watch.data.job == j);
                assert(j->timer_watch.fd >= 0);

                assert_se(epoll_ctl(j->manager->epoll_fd, EPOLL_CTL_DEL, j->timer_watch.fd, NULL) >= 0);
                close_nointr_nofail(j->timer_watch.fd);
        }

        free(j->bus_client);
        free(j);
}

JobDependency* job_dependency_new(Job *subject, Job *object, bool matters, bool conflicts) {
        JobDependency *l;

        assert(object);

        /* Adds a new job link, which encodes that the 'subject' job
         * needs the 'object' job in some way. If 'subject' is NULL
         * this means the 'anchor' job (i.e. the one the user
         * explicitly asked for) is the requester. */

        if (!(l = new0(JobDependency, 1)))
                return NULL;

        l->subject = subject;
        l->object = object;
        l->matters = matters;
        l->conflicts = conflicts;

        if (subject)
                LIST_PREPEND(JobDependency, subject, subject->subject_list, l);
        else
                LIST_PREPEND(JobDependency, subject, object->manager->transaction_anchor, l);

        LIST_PREPEND(JobDependency, object, object->object_list, l);

        return l;
}

void job_dependency_free(JobDependency *l) {
        assert(l);

        if (l->subject)
                LIST_REMOVE(JobDependency, subject, l->subject->subject_list, l);
        else
                LIST_REMOVE(JobDependency, subject, l->object->manager->transaction_anchor, l);

        LIST_REMOVE(JobDependency, object, l->object->object_list, l);

        free(l);
}

void job_dump(Job *j, FILE*f, const char *prefix) {
        assert(j);
        assert(f);

        if (!prefix)
                prefix = "";

        fprintf(f,
                "%s-> Job %u:\n"
                "%s\tAction: %s -> %s\n"
                "%s\tState: %s\n"
                "%s\tForced: %s\n",
                prefix, j->id,
                prefix, j->unit->meta.id, job_type_to_string(j->type),
                prefix, job_state_to_string(j->state),
                prefix, yes_no(j->override));
}

bool job_is_anchor(Job *j) {
        JobDependency *l;

        assert(j);

        LIST_FOREACH(object, l, j->object_list)
                if (!l->subject)
                        return true;

        return false;
}

static bool types_match(JobType a, JobType b, JobType c, JobType d) {
        return
                (a == c && b == d) ||
                (a == d && b == c);
}

int job_type_merge(JobType *a, JobType b) {
        if (*a == b)
                return 0;

        /* Merging is associative! a merged with b merged with c is
         * the same as a merged with c merged with b. */

        /* Mergeability is transitive! if a can be merged with b and b
         * with c then a also with c */

        /* Also, if a merged with b cannot be merged with c, then
         * either a or b cannot be merged with c either */

        if (types_match(*a, b, JOB_START, JOB_VERIFY_ACTIVE))
                *a = JOB_START;
        else if (types_match(*a, b, JOB_START, JOB_RELOAD) ||
                 types_match(*a, b, JOB_START, JOB_RELOAD_OR_START) ||
                 types_match(*a, b, JOB_VERIFY_ACTIVE, JOB_RELOAD_OR_START) ||
                 types_match(*a, b, JOB_RELOAD, JOB_RELOAD_OR_START))
                *a = JOB_RELOAD_OR_START;
        else if (types_match(*a, b, JOB_START, JOB_RESTART) ||
                 types_match(*a, b, JOB_START, JOB_TRY_RESTART) ||
                 types_match(*a, b, JOB_VERIFY_ACTIVE, JOB_RESTART) ||
                 types_match(*a, b, JOB_RELOAD, JOB_RESTART) ||
                 types_match(*a, b, JOB_RELOAD_OR_START, JOB_RESTART) ||
                 types_match(*a, b, JOB_RELOAD_OR_START, JOB_TRY_RESTART) ||
                 types_match(*a, b, JOB_RESTART, JOB_TRY_RESTART))
                *a = JOB_RESTART;
        else if (types_match(*a, b, JOB_VERIFY_ACTIVE, JOB_RELOAD))
                *a = JOB_RELOAD;
        else if (types_match(*a, b, JOB_VERIFY_ACTIVE, JOB_TRY_RESTART) ||
                 types_match(*a, b, JOB_RELOAD, JOB_TRY_RESTART))
                *a = JOB_TRY_RESTART;
        else
                return -EEXIST;

        return 0;
}

bool job_type_is_mergeable(JobType a, JobType b) {
        return job_type_merge(&a, b) >= 0;
}

bool job_type_is_superset(JobType a, JobType b) {

        /* Checks whether operation a is a "superset" of b in its
         * actions */

        if (a == b)
                return true;

        switch (a) {
                case JOB_START:
                        return b == JOB_VERIFY_ACTIVE;

                case JOB_RELOAD:
                        return
                                b == JOB_VERIFY_ACTIVE;

                case JOB_RELOAD_OR_START:
                        return
                                b == JOB_RELOAD ||
                                b == JOB_START ||
                                b == JOB_VERIFY_ACTIVE;

                case JOB_RESTART:
                        return
                                b == JOB_START ||
                                b == JOB_VERIFY_ACTIVE ||
                                b == JOB_RELOAD ||
                                b == JOB_RELOAD_OR_START ||
                                b == JOB_TRY_RESTART;

                case JOB_TRY_RESTART:
                        return
                                b == JOB_VERIFY_ACTIVE ||
                                b == JOB_RELOAD;
                default:
                        return false;

        }
}

bool job_type_is_conflicting(JobType a, JobType b) {
        assert(a >= 0 && a < _JOB_TYPE_MAX);
        assert(b >= 0 && b < _JOB_TYPE_MAX);

        return (a == JOB_STOP) != (b == JOB_STOP);
}

bool job_type_is_redundant(JobType a, UnitActiveState b) {
        switch (a) {

        case JOB_START:
                return
                        b == UNIT_ACTIVE ||
                        b == UNIT_RELOADING;

        case JOB_STOP:
                return
                        b == UNIT_INACTIVE ||
                        b == UNIT_FAILED;

        case JOB_VERIFY_ACTIVE:
                return
                        b == UNIT_ACTIVE ||
                        b == UNIT_RELOADING;

        case JOB_RELOAD:
                return
                        b == UNIT_RELOADING;

        case JOB_RELOAD_OR_START:
                return
                        b == UNIT_ACTIVATING ||
                        b == UNIT_RELOADING;

        case JOB_RESTART:
                return
                        b == UNIT_ACTIVATING;

        case JOB_TRY_RESTART:
                return
                        b == UNIT_ACTIVATING;

        default:
                assert_not_reached("Invalid job type");
        }
}

bool job_is_runnable(Job *j) {
        Iterator i;
        Unit *other;

        assert(j);
        assert(j->installed);

        /* Checks whether there is any job running for the units this
         * job needs to be running after (in the case of a 'positive'
         * job type) or before (in the case of a 'negative' job
         * type. */

        /* First check if there is an override */
        if (j->ignore_order)
                return true;

        if (j->type == JOB_START ||
            j->type == JOB_VERIFY_ACTIVE ||
            j->type == JOB_RELOAD ||
            j->type == JOB_RELOAD_OR_START) {

                /* Immediate result is that the job is or might be
                 * started. In this case lets wait for the
                 * dependencies, regardless whether they are
                 * starting or stopping something. */

                SET_FOREACH(other, j->unit->meta.dependencies[UNIT_AFTER], i)
                        if (other->meta.job)
                                return false;
        }

        /* Also, if something else is being stopped and we should
         * change state after it, then lets wait. */

        SET_FOREACH(other, j->unit->meta.dependencies[UNIT_BEFORE], i)
                if (other->meta.job &&
                    (other->meta.job->type == JOB_STOP ||
                     other->meta.job->type == JOB_RESTART ||
                     other->meta.job->type == JOB_TRY_RESTART))
                        return false;

        /* This means that for a service a and a service b where b
         * shall be started after a:
         *
         *  start a + start b → 1st step start a, 2nd step start b
         *  start a + stop b  → 1st step stop b,  2nd step start a
         *  stop a  + start b → 1st step stop a,  2nd step start b
         *  stop a  + stop b  → 1st step stop b,  2nd step stop a
         *
         *  This has the side effect that restarts are properly
         *  synchronized too. */

        return true;
}

int job_run_and_invalidate(Job *j) {
        int r;
        uint32_t id;
        Manager *m;

        assert(j);
        assert(j->installed);

        if (j->in_run_queue) {
                LIST_REMOVE(Job, run_queue, j->manager->run_queue, j);
                j->in_run_queue = false;
        }

        if (j->state != JOB_WAITING)
                return 0;

        if (!job_is_runnable(j))
                return -EAGAIN;

        j->state = JOB_RUNNING;
        job_add_to_dbus_queue(j);

        /* While we execute this operation the job might go away (for
         * example: because it is replaced by a new, conflicting
         * job.) To make sure we don't access a freed job later on we
         * store the id here, so that we can verify the job is still
         * valid. */
        id = j->id;
        m = j->manager;

        switch (j->type) {

                case JOB_START:
                        r = unit_start(j->unit);

                        /* If this unit cannot be started, then simply
                         * wait */
                        if (r == -EBADR)
                                r = 0;

                        break;

                case JOB_VERIFY_ACTIVE: {
                        UnitActiveState t = unit_active_state(j->unit);
                        if (UNIT_IS_ACTIVE_OR_RELOADING(t))
                                r = -EALREADY;
                        else if (t == UNIT_ACTIVATING)
                                r = -EAGAIN;
                        else
                                r = -ENOEXEC;
                        break;
                }

                case JOB_STOP:
                        r = unit_stop(j->unit);

                        /* If this unit cannot stopped, then simply
                         * wait. */
                        if (r == -EBADR)
                                r = 0;
                        break;

                case JOB_RELOAD:
                        r = unit_reload(j->unit);
                        break;

                case JOB_RELOAD_OR_START:
                        if (unit_active_state(j->unit) == UNIT_ACTIVE) {
                                j->type = JOB_RELOAD;
                                r = unit_reload(j->unit);
                        } else {
                                j->type = JOB_START;
                                r = unit_start(j->unit);

                                if (r == -EBADR)
                                        r = 0;
                        }
                        break;

                case JOB_RESTART: {
                        UnitActiveState t = unit_active_state(j->unit);
                        if (t == UNIT_INACTIVE || t == UNIT_FAILED || t == UNIT_ACTIVATING) {
                                j->type = JOB_START;
                                r = unit_start(j->unit);
                        } else
                                r = unit_stop(j->unit);
                        break;
                }

                case JOB_TRY_RESTART: {
                        UnitActiveState t = unit_active_state(j->unit);
                        if (t == UNIT_INACTIVE || t == UNIT_FAILED || t == UNIT_DEACTIVATING)
                                r = -ENOEXEC;
                        else if (t == UNIT_ACTIVATING) {
                                j->type = JOB_START;
                                r = unit_start(j->unit);
                        } else {
                                j->type = JOB_RESTART;
                                r = unit_stop(j->unit);
                        }
                        break;
                }

                default:
                        assert_not_reached("Unknown job type");
        }

        if ((j = manager_get_job(m, id))) {
                if (r == -EALREADY)
                        r = job_finish_and_invalidate(j, JOB_DONE);
                else if (r == -ENOEXEC)
                        r = job_finish_and_invalidate(j, JOB_SKIPPED);
                else if (r == -EAGAIN)
                        j->state = JOB_WAITING;
                else if (r < 0)
                        r = job_finish_and_invalidate(j, JOB_FAILED);
        }

        return r;
}

static void job_print_status_message(Unit *u, JobType t, JobResult result) {
        assert(u);

        if (t == JOB_START) {

                switch (result) {

                case JOB_DONE:
                        unit_status_printf(u, "Started %s.\n", unit_description(u));
                        break;

                case JOB_FAILED:
                        unit_status_printf(u, "Starting %s " ANSI_HIGHLIGHT_ON "failed" ANSI_HIGHLIGHT_OFF ", see 'systemctl status %s' for details.\n", unit_description(u), u->meta.id);
                        break;

                case JOB_DEPENDENCY:
                        unit_status_printf(u, "Starting %s " ANSI_HIGHLIGHT_ON "aborted" ANSI_HIGHLIGHT_OFF " because a dependency failed.\n", unit_description(u));
                        break;

                case JOB_TIMEOUT:
                        unit_status_printf(u, "Starting %s " ANSI_HIGHLIGHT_ON "timed out" ANSI_HIGHLIGHT_OFF ".\n", unit_description(u), u->meta.id);
                        break;

                default:
                        ;
                }

        } else if (t == JOB_STOP) {

                switch (result) {

                case JOB_TIMEOUT:
                        unit_status_printf(u, "Stopping %s " ANSI_HIGHLIGHT_ON "timed out" ANSI_HIGHLIGHT_OFF ".\n", unit_description(u), u->meta.id);
                        break;

                case JOB_DONE:
                case JOB_FAILED:
                        unit_status_printf(u, "Stopped %s.\n", unit_description(u));
                        break;

                default:
                        ;
                }
        }
}

int job_finish_and_invalidate(Job *j, JobResult result) {
        Unit *u;
        Unit *other;
        JobType t;
        Iterator i;

        assert(j);
        assert(j->installed);

        job_add_to_dbus_queue(j);

        /* Patch restart jobs so that they become normal start jobs */
        if (result == JOB_DONE && (j->type == JOB_RESTART || j->type == JOB_TRY_RESTART)) {

                log_debug("Converting job %s/%s -> %s/%s",
                          j->unit->meta.id, job_type_to_string(j->type),
                          j->unit->meta.id, job_type_to_string(JOB_START));

                j->state = JOB_WAITING;
                j->type = JOB_START;

                job_add_to_run_queue(j);

                u = j->unit;
                goto finish;
        }

        j->result = result;

        log_debug("Job %s/%s finished, result=%s", j->unit->meta.id, job_type_to_string(j->type), job_result_to_string(result));

        if (result == JOB_FAILED)
                j->manager->n_failed_jobs ++;

        u = j->unit;
        t = j->type;
        job_free(j);

        job_print_status_message(u, t, result);

        /* Fail depending jobs on failure */
        if (result != JOB_DONE) {

                if (t == JOB_START ||
                    t == JOB_VERIFY_ACTIVE ||
                    t == JOB_RELOAD_OR_START) {

                        SET_FOREACH(other, u->meta.dependencies[UNIT_REQUIRED_BY], i)
                                if (other->meta.job &&
                                    (other->meta.job->type == JOB_START ||
                                     other->meta.job->type == JOB_VERIFY_ACTIVE ||
                                     other->meta.job->type == JOB_RELOAD_OR_START))
                                        job_finish_and_invalidate(other->meta.job, JOB_DEPENDENCY);

                        SET_FOREACH(other, u->meta.dependencies[UNIT_BOUND_BY], i)
                                if (other->meta.job &&
                                    (other->meta.job->type == JOB_START ||
                                     other->meta.job->type == JOB_VERIFY_ACTIVE ||
                                     other->meta.job->type == JOB_RELOAD_OR_START))
                                        job_finish_and_invalidate(other->meta.job, JOB_DEPENDENCY);

                        SET_FOREACH(other, u->meta.dependencies[UNIT_REQUIRED_BY_OVERRIDABLE], i)
                                if (other->meta.job &&
                                    !other->meta.job->override &&
                                    (other->meta.job->type == JOB_START ||
                                     other->meta.job->type == JOB_VERIFY_ACTIVE ||
                                     other->meta.job->type == JOB_RELOAD_OR_START))
                                        job_finish_and_invalidate(other->meta.job, JOB_DEPENDENCY);

                } else if (t == JOB_STOP) {

                        SET_FOREACH(other, u->meta.dependencies[UNIT_CONFLICTED_BY], i)
                                if (other->meta.job &&
                                    (other->meta.job->type == JOB_START ||
                                     other->meta.job->type == JOB_VERIFY_ACTIVE ||
                                     other->meta.job->type == JOB_RELOAD_OR_START))
                                        job_finish_and_invalidate(other->meta.job, JOB_DEPENDENCY);
                }
        }

        /* Trigger OnFailure dependencies that are not generated by
         * the unit itself. We don't tread JOB_CANCELED as failure in
         * this context. And JOB_FAILURE is already handled by the
         * unit itself. */
        if (result == JOB_TIMEOUT || result == JOB_DEPENDENCY) {
                log_notice("Job %s/%s failed with result '%s'.",
                           u->meta.id,
                           job_type_to_string(t),
                           job_result_to_string(result));

                unit_trigger_on_failure(u);
        }

finish:
        /* Try to start the next jobs that can be started */
        SET_FOREACH(other, u->meta.dependencies[UNIT_AFTER], i)
                if (other->meta.job)
                        job_add_to_run_queue(other->meta.job);
        SET_FOREACH(other, u->meta.dependencies[UNIT_BEFORE], i)
                if (other->meta.job)
                        job_add_to_run_queue(other->meta.job);

        manager_check_finished(u->meta.manager);

        return 0;
}

int job_start_timer(Job *j) {
        struct itimerspec its;
        struct epoll_event ev;
        int fd, r;
        assert(j);

        if (j->unit->meta.job_timeout <= 0 ||
            j->timer_watch.type == WATCH_JOB_TIMER)
                return 0;

        assert(j->timer_watch.type == WATCH_INVALID);

        if ((fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK|TFD_CLOEXEC)) < 0) {
                r = -errno;
                goto fail;
        }

        zero(its);
        timespec_store(&its.it_value, j->unit->meta.job_timeout);

        if (timerfd_settime(fd, 0, &its, NULL) < 0) {
                r = -errno;
                goto fail;
        }

        zero(ev);
        ev.data.ptr = &j->timer_watch;
        ev.events = EPOLLIN;

        if (epoll_ctl(j->manager->epoll_fd, EPOLL_CTL_ADD, fd, &ev) < 0) {
                r = -errno;
                goto fail;
        }

        j->timer_watch.type = WATCH_JOB_TIMER;
        j->timer_watch.fd = fd;
        j->timer_watch.data.job = j;

        return 0;

fail:
        if (fd >= 0)
                close_nointr_nofail(fd);

        return r;
}

void job_add_to_run_queue(Job *j) {
        assert(j);
        assert(j->installed);

        if (j->in_run_queue)
                return;

        LIST_PREPEND(Job, run_queue, j->manager->run_queue, j);
        j->in_run_queue = true;
}

void job_add_to_dbus_queue(Job *j) {
        assert(j);
        assert(j->installed);

        if (j->in_dbus_queue)
                return;

        /* We don't check if anybody is subscribed here, since this
         * job might just have been created and not yet assigned to a
         * connection/client. */

        LIST_PREPEND(Job, dbus_queue, j->manager->dbus_job_queue, j);
        j->in_dbus_queue = true;
}

char *job_dbus_path(Job *j) {
        char *p;

        assert(j);

        if (asprintf(&p, "/org/freedesktop/systemd1/job/%lu", (unsigned long) j->id) < 0)
                return NULL;

        return p;
}

void job_timer_event(Job *j, uint64_t n_elapsed, Watch *w) {
        assert(j);
        assert(w == &j->timer_watch);

        log_warning("Job %s/%s timed out.", j->unit->meta.id, job_type_to_string(j->type));
        job_finish_and_invalidate(j, JOB_TIMEOUT);
}

static const char* const job_state_table[_JOB_STATE_MAX] = {
        [JOB_WAITING] = "waiting",
        [JOB_RUNNING] = "running"
};

DEFINE_STRING_TABLE_LOOKUP(job_state, JobState);

static const char* const job_type_table[_JOB_TYPE_MAX] = {
        [JOB_START] = "start",
        [JOB_VERIFY_ACTIVE] = "verify-active",
        [JOB_STOP] = "stop",
        [JOB_RELOAD] = "reload",
        [JOB_RELOAD_OR_START] = "reload-or-start",
        [JOB_RESTART] = "restart",
        [JOB_TRY_RESTART] = "try-restart",
};

DEFINE_STRING_TABLE_LOOKUP(job_type, JobType);

static const char* const job_mode_table[_JOB_MODE_MAX] = {
        [JOB_FAIL] = "fail",
        [JOB_REPLACE] = "replace",
        [JOB_ISOLATE] = "isolate",
        [JOB_IGNORE_DEPENDENCIES] = "ignore-dependencies",
        [JOB_IGNORE_REQUIREMENTS] = "ignore-requirements"
};

DEFINE_STRING_TABLE_LOOKUP(job_mode, JobMode);

static const char* const job_result_table[_JOB_RESULT_MAX] = {
        [JOB_DONE] = "done",
        [JOB_CANCELED] = "canceled",
        [JOB_TIMEOUT] = "timeout",
        [JOB_FAILED] = "failed",
        [JOB_DEPENDENCY] = "dependency",
        [JOB_SKIPPED] = "skipped"
};

DEFINE_STRING_TABLE_LOOKUP(job_result, JobResult);
