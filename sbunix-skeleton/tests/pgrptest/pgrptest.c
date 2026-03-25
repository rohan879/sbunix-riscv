/*
 * pgrptest -- process groups and sessions
 *
 * Exercises getpgid, setpgid, setsid, getsid, kill(-pgid), and verifies
 * inheritance and error conditions.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <sys/wait.h>

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(label, cond) do { \
    if (cond) { printf("[PASS] pgrp: %s\n", label); g_pass++; } \
    else      { printf("[FAIL] pgrp: %s\n", label); g_fail++; } \
} while (0)

static volatile sig_atomic_t got_usr1 = 0;

static void usr1_handler(int sig) {
    (void)sig;
    got_usr1 = 1;
}

/* -----------------------------------------------------------------------
 * 1. getpgid_self: getpgid(0) returns own pgid (initially == pid)
 * ----------------------------------------------------------------------- */
static void test_getpgid_self(void) {
    pid_t pgid = getpgid(0);
    /*
     * For the initial process or after setpgid(0,0), pgid should be
     * positive. It may or may not equal pid depending on parent's group,
     * but it must be > 0.  We test a stronger condition in a child below.
     */
    CHECK("getpgid_self", pgid > 0);
}

/* -----------------------------------------------------------------------
 * 2. setpgid_self: setpgid(0, 0) sets pgid to own pid
 * ----------------------------------------------------------------------- */
static void test_setpgid_self(void) {
    pid_t pid = fork();
    if (pid == 0) {
        /* Child: set own pgid to own pid */
        int r = setpgid(0, 0);
        pid_t pgid = getpgid(0);
        _exit(r == 0 && pgid == getpid() ? 0 : 1);
    }
    int status;
    waitpid(pid, &status, 0);
    CHECK("setpgid_self",
          WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

/* -----------------------------------------------------------------------
 * 3. setpgid_child: fork child, parent calls setpgid(child, child)
 * ----------------------------------------------------------------------- */
static void test_setpgid_child(void) {
    pid_t child = fork();
    if (child == 0) {
        /* Child: sleep briefly so parent can change our pgid */
        sleep(1);
        _exit(0);
    }
    int r = setpgid(child, child);
    CHECK("setpgid_child", r == 0);
    waitpid(child, NULL, 0);
}

/* -----------------------------------------------------------------------
 * 4. getpgid_child: after setpgid, getpgid(child) returns child's pid
 * ----------------------------------------------------------------------- */
static void test_getpgid_child(void) {
    pid_t child = fork();
    if (child == 0) {
        sleep(1);
        _exit(0);
    }
    setpgid(child, child);
    pid_t cpgid = getpgid(child);
    CHECK("getpgid_child", cpgid == child);
    waitpid(child, NULL, 0);
}

/* -----------------------------------------------------------------------
 * 5. setpgid_nonchild_fails: setpgid on non-child pid returns -1 / ESRCH
 * ----------------------------------------------------------------------- */
static void test_setpgid_nonchild_fails(void) {
    /*
     * PID 1 (init) is not our child, so setpgid should fail.
     * Use a PID that is very unlikely to be our child.
     */
    errno = 0;
    int r = setpgid(99999, 99999);
    CHECK("setpgid_nonchild_fails", r == -1 && errno == ESRCH);
}

/* -----------------------------------------------------------------------
 * 6. setsid_creates_session: fork child, child calls setsid(),
 *    getsid(0) returns child's pid
 * ----------------------------------------------------------------------- */
static void test_setsid_creates_session(void) {
    pid_t child = fork();
    if (child == 0) {
        pid_t sid = setsid();
        pid_t mysid = getsid(0);
        _exit(sid == getpid() && mysid == getpid() ? 0 : 1);
    }
    int status;
    waitpid(child, &status, 0);
    CHECK("setsid_creates_session",
          WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

/* -----------------------------------------------------------------------
 * 7. setsid_new_pgid: after setsid(), getpgid(0) also returns child's pid
 * ----------------------------------------------------------------------- */
static void test_setsid_new_pgid(void) {
    pid_t child = fork();
    if (child == 0) {
        setsid();
        pid_t pgid = getpgid(0);
        _exit(pgid == getpid() ? 0 : 1);
    }
    int status;
    waitpid(child, &status, 0);
    CHECK("setsid_new_pgid",
          WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

/* -----------------------------------------------------------------------
 * 8. kill_process_group: create group with 2 children,
 *    kill(-pgid, SIGUSR1) reaches both
 * ----------------------------------------------------------------------- */
static void test_kill_process_group(void) {
    pid_t leader = fork();
    if (leader == 0) {
        /* First child: become group leader */
        setpgid(0, 0);

        /* Install SIGUSR1 handler */
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = usr1_handler;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGUSR1, &sa, NULL);

        /* Fork a second child in the same group */
        pid_t sibling = fork();
        if (sibling == 0) {
            /* Second child: inherit group, wait for signal */
            got_usr1 = 0;
            /* Use pause() to wait for signal */
            pause();
            _exit(got_usr1 ? 0 : 1);
        }

        /* First child also waits for signal */
        got_usr1 = 0;
        pause();

        /* Reap sibling */
        int sstatus;
        waitpid(sibling, &sstatus, 0);
        int sib_ok = WIFEXITED(sstatus) && WEXITSTATUS(sstatus) == 0;
        _exit(got_usr1 && sib_ok ? 0 : 1);
    }

    /* Parent: put leader into its own group and send signal to the group */
    setpgid(leader, leader);

    /* Give children time to set up handlers and call pause() */
    struct timespec ts = { .tv_sec = 0, .tv_nsec = 200000000 }; /* 200ms */
    nanosleep(&ts, NULL);

    /* Send SIGUSR1 to the entire process group */
    int r = kill(-leader, SIGUSR1);
    CHECK("kill_process_group: kill(-pgid) ok", r == 0);

    int status;
    waitpid(leader, &status, 0);
    CHECK("kill_process_group: both children got signal",
          WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

/* -----------------------------------------------------------------------
 * 9. setpgid_session_leader_fails: process that called setsid()
 *    cannot have its pgid changed
 * ----------------------------------------------------------------------- */
static void test_setpgid_session_leader_fails(void) {
    pid_t child = fork();
    if (child == 0) {
        setsid();
        errno = 0;
        int r = setpgid(0, getppid());
        _exit(r == -1 && errno == EPERM ? 0 : 1);
    }
    int status;
    waitpid(child, &status, 0);
    CHECK("setpgid_session_leader_fails",
          WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

/* -----------------------------------------------------------------------
 * 10. pgid_inherited: fork child, child's pgid matches parent's pgid
 * ----------------------------------------------------------------------- */
static void test_pgid_inherited(void) {
    pid_t parent_pgid = getpgid(0);
    pid_t child = fork();
    if (child == 0) {
        pid_t child_pgid = getpgid(0);
        _exit(child_pgid == parent_pgid ? 0 : 1);
    }
    int status;
    waitpid(child, &status, 0);
    CHECK("pgid_inherited",
          WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

/* ----------------------------------------------------------------------- */

int main(void) {
    printf("=== pgrptest: process groups and sessions ===\n");

    test_getpgid_self();
    test_setpgid_self();
    test_setpgid_child();
    test_getpgid_child();
    test_setpgid_nonchild_fails();
    test_setsid_creates_session();
    test_setsid_new_pgid();
    test_kill_process_group();
    test_setpgid_session_leader_fails();
    test_pgid_inherited();

    printf("=== pgrptest: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail > 0 ? 1 : 0;
}
