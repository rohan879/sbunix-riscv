#include "sbush.h"

static struct job jobs[MAX_JOBS];
static int next_job_id = 1;

static void job_clear(struct job *j) {
    memset(j, 0, sizeof(*j));
}

static int job_has_live_members(struct job *j) {
    for (int i = 0; i < j->npids; i++) {
        if (j->pids[i] > 0)
            return 1;
    }
    return 0;
}

int job_add(pid_t pgid, const pid_t *pids, int npids, const char *cmd) {
    for (int i = 0; i < MAX_JOBS; i++) {
        if (jobs[i].pgid == 0) {
            jobs[i].id = next_job_id++;
            jobs[i].pgid = pgid;
            jobs[i].state = JOB_RUNNING;
            jobs[i].npids = 0;
            if (pids && npids > 0) {
                for (int j = 0; j < npids && j < MAX_PIPES; j++) {
                    jobs[i].pids[j] = pids[j];
                    jobs[i].npids++;
                }
            } else {
                jobs[i].pids[0] = pgid;
                jobs[i].npids = 1;
            }
            strncpy(jobs[i].cmd, cmd, 127);
            jobs[i].cmd[127] = '\0';
            return jobs[i].id;
        }
    }
    return -1;
}

void job_remove(pid_t pgid) {
    for (int i = 0; i < MAX_JOBS; i++) {
        if (jobs[i].pgid == pgid) {
            job_clear(&jobs[i]);
            return;
        }
    }
}

void job_set_state(pid_t pgid, enum job_state state) {
    for (int i = 0; i < MAX_JOBS; i++) {
        if (jobs[i].pgid == pgid) {
            jobs[i].state = state;
            return;
        }
    }
}

struct job *job_find(int id) {
    for (int i = 0; i < MAX_JOBS; i++)
        if (jobs[i].id == id && jobs[i].pgid != 0)
            return &jobs[i];
    return NULL;
}

struct job *job_find_pgid(pid_t pgid) {
    for (int i = 0; i < MAX_JOBS; i++)
        if (jobs[i].pgid == pgid)
            return &jobs[i];
    return NULL;
}

struct job *job_find_pid(pid_t pid) {
    for (int i = 0; i < MAX_JOBS; i++) {
        if (jobs[i].pgid == 0)
            continue;
        for (int j = 0; j < jobs[i].npids; j++) {
            if (jobs[i].pids[j] == pid)
                return &jobs[i];
        }
    }
    return NULL;
}

static void job_note_member_done(struct job *j, pid_t pid) {
    for (int i = 0; i < j->npids; i++) {
        if (j->pids[i] == pid) {
            j->pids[i] = 0;
            return;
        }
    }
}

void job_mark_status(pid_t pid, int status, int notify_done) {
    /* BF: Child status is reported per PID, while job control operates on
     * process groups; map member PIDs back to their owning job here. */
    struct job *j = job_find_pid(pid);
    if (!j)
        return;
    if (WIFSTOPPED(status)) {
        j->state = JOB_STOPPED;
        return;
    }
    if (WIFCONTINUED(status)) {
        j->state = JOB_RUNNING;
        return;
    }
    job_note_member_done(j, pid);
    if (!job_has_live_members(j)) {
        if (notify_done)
            printf("[%d] Done\t%s\n", j->id, j->cmd);
        job_clear(j);
    }
}

void job_sync(int notify_done) {
    /* BF: Synchronize before jobs/fg/bg so dead background entries cannot
     * be selected instead of the newest stopped foreground job. */
    int status;
    pid_t p;
    while ((p = waitpid(-1, &status, WNOHANG | WUNTRACED | WCONTINUED)) > 0)
        job_mark_status(p, status, notify_done);
}

void job_list(void) {
    for (int i = 0; i < MAX_JOBS; i++) {
        if (jobs[i].pgid == 0) continue;
        if (jobs[i].state == JOB_DONE) {
            job_clear(&jobs[i]);
            continue;
        }
        const char *state = "Running";
        if (jobs[i].state == JOB_STOPPED) state = "Stopped";
        printf("[%d] %s\t%s\n", jobs[i].id, state, jobs[i].cmd);
    }
}

struct job *job_last(void) {
    struct job *last = NULL;
    int max_id = 0;
    for (int i = 0; i < MAX_JOBS; i++) {
        if (jobs[i].pgid != 0 && jobs[i].state != JOB_DONE &&
            jobs[i].id > max_id) {
            max_id = jobs[i].id;
            last = &jobs[i];
        }
    }
    return last;
}

struct job *job_last_stopped(void) {
    struct job *last = NULL;
    int max_id = 0;
    for (int i = 0; i < MAX_JOBS; i++) {
        if (jobs[i].pgid != 0 && jobs[i].state == JOB_STOPPED &&
            jobs[i].id > max_id) {
            max_id = jobs[i].id;
            last = &jobs[i];
        }
    }
    return last;
}

void job_reap(void) {
    for (int i = 0; i < MAX_JOBS; i++) {
        if (jobs[i].pgid == 0) continue;
        if (jobs[i].state == JOB_DONE) {
            printf("[%d] Done\t%s\n", jobs[i].id, jobs[i].cmd);
            job_clear(&jobs[i]);
        }
    }
}
