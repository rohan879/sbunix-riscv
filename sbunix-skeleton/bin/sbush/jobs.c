#include "sbush.h"

static struct job jobs[MAX_JOBS];
static int next_job_id = 1;

int job_add(pid_t pid, const char *cmd) {
    for (int i = 0; i < MAX_JOBS; i++) {
        if (jobs[i].pid == 0) {
            jobs[i].id = next_job_id++;
            jobs[i].pid = pid;
            jobs[i].state = JOB_RUNNING;
            strncpy(jobs[i].cmd, cmd, 127);
            jobs[i].cmd[127] = '\0';
            return jobs[i].id;
        }
    }
    return -1;
}

void job_remove(pid_t pid) {
    for (int i = 0; i < MAX_JOBS; i++) {
        if (jobs[i].pid == pid) {
            jobs[i].pid = 0;
            jobs[i].id = 0;
            return;
        }
    }
}

void job_set_state(pid_t pid, enum job_state state) {
    for (int i = 0; i < MAX_JOBS; i++) {
        if (jobs[i].pid == pid) {
            jobs[i].state = state;
            return;
        }
    }
}

struct job *job_find(int id) {
    for (int i = 0; i < MAX_JOBS; i++)
        if (jobs[i].id == id && jobs[i].pid != 0)
            return &jobs[i];
    return NULL;
}

struct job *job_find_pid(pid_t pid) {
    for (int i = 0; i < MAX_JOBS; i++)
        if (jobs[i].pid == pid)
            return &jobs[i];
    return NULL;
}

void job_list(void) {
    for (int i = 0; i < MAX_JOBS; i++) {
        if (jobs[i].pid == 0) continue;
        const char *state = "Running";
        if (jobs[i].state == JOB_STOPPED) state = "Stopped";
        if (jobs[i].state == JOB_DONE)    state = "Done";
        printf("[%d] %s\t%s\n", jobs[i].id, state, jobs[i].cmd);
    }
}

void job_reap(void) {
    for (int i = 0; i < MAX_JOBS; i++) {
        if (jobs[i].pid == 0) continue;
        int status;
        pid_t ret = waitpid(jobs[i].pid, &status, WNOHANG);
        if (ret > 0) {
            jobs[i].state = JOB_DONE;
            printf("[%d] Done\t%s\n", jobs[i].id, jobs[i].cmd);
            jobs[i].pid = 0;
        }
    }
}
