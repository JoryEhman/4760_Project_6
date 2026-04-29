/*
 * oss.c
 * Author: ehman
 * Date: 2026-04-29
 * Environment: Linux, gcc
 * Description: OSS memory management simulator (Project 6)
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/msg.h>
#include <sys/wait.h>
#include <signal.h>
#include <stdarg.h>
#include "shared.h"

/* ── Globals ─────────────────────────────────────────────────────────────── */
SimClock *simClock = NULL;
int       shmid    = -1;
int       msqid    = -1;
FILE     *logfp    = NULL;
int       logLines =  0;

PCB   processTable[MAX_PROCS];
Frame frameTable[TOTAL_FRAMES];

/* ── Logging ─────────────────────────────────────────────────────────────── */
static void logWrite(const char *fmt, ...) {
    if (logLines >= MAX_LOG_LINES) return;

    va_list args;
    char buf[1024];

    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    for (char *p = buf; *p; p++)
        if (*p == '\n') logLines++;

    printf("%s", buf);
    if (logfp) {
        fprintf(logfp, "%s", buf);
        fflush(logfp);
    }
}

/* ── Cleanup ─────────────────────────────────────────────────────────────── */
static void cleanup(int sig) {
    (void)sig;

    for (int i = 0; i < MAX_PROCS; i++)
        if (processTable[i].occupied)
            kill(processTable[i].pid, SIGTERM);

    while (waitpid(-1, NULL, WNOHANG) > 0);

    if (simClock) shmdt(simClock);
    if (shmid != -1) shmctl(shmid, IPC_RMID, NULL);
    if (msqid != -1) msgctl(msqid, IPC_RMID, NULL);
    if (logfp)  fclose(logfp);

    exit(0);
}

/* ── Advance clock ───────────────────────────────────────────────────────── */
static void advanceClock(unsigned int ns) {
    simClock->nanoseconds += ns;
    while (simClock->nanoseconds >= BILLION) {
        simClock->seconds++;
        simClock->nanoseconds -= BILLION;
    }
}

/* ── Find free slot ──────────────────────────────────────────────────────── */
static int findFreeSlot(void) {
    for (int i = 0; i < MAX_PROCS; i++)
        if (!processTable[i].occupied) return i;
    return -1;
}

/* ── Count active processes ──────────────────────────────────────────────── */
static int countActive(void) {
    int count = 0;
    for (int i = 0; i < MAX_PROCS; i++)
        if (processTable[i].occupied) count++;
    return count;
}

/* ── Launch a child ──────────────────────────────────────────────────────── */
static int launchChild(int slot, int t) {
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return -1; }

    if (pid == 0) {
        char tStr[16];
        snprintf(tStr, sizeof(tStr), "%d", t);
        execl("./user_proc", "user_proc", tStr, NULL);
        perror("execl");
        exit(1);
    }

    PCB *p = &processTable[slot];
    p->occupied    = 1;
    p->pid         = pid;
    p->startSeconds = (int)simClock->seconds;
    p->startNano    = (int)simClock->nanoseconds;
    p->blocked     = 0;
    p->unblockSec  = 0;
    p->unblockNano = 0;
    p->localPid    = slot + 1;
    for (int i = 0; i < PAGES_PER_PROC; i++)
        p->pageTable[i] = -1;

    logWrite("OSS: Launched user_proc PID %d in slot %d at %u:%09u\n",
             pid, slot, simClock->seconds, simClock->nanoseconds);
    return 0;
}

/* ── Main ────────────────────────────────────────────────────────────────── */
int main(int argc, char *argv[]) {

    int    n     = 18;
    int    s     = 18;
    int    t     = 5;
    double i_val = 0.5;
    char   logfile[256] = "oss.log";

    int opt;
    while ((opt = getopt(argc, argv, "hn:s:t:i:f:")) != -1) {
        switch (opt) {
            case 'h':
                printf("Usage: %s [-h] [-n proc] [-s simul] [-t timeLimit]"
                       " [-i fraction] [-f logfile]\n", argv[0]);
                return 0;
            case 'n': n     = atoi(optarg); break;
            case 's': s     = atoi(optarg); break;
            case 't': t     = atoi(optarg); break;
            case 'i': i_val = atof(optarg); break;
            case 'f': strncpy(logfile, optarg, sizeof(logfile) - 1); break;
            default:
                fprintf(stderr, "Usage: %s [-h] [-n proc] [-s simul]"
                        " [-t timeLimit] [-i fraction] [-f logfile]\n",
                        argv[0]);
                return 1;
        }
    }

    if (s > MAX_RUNNING) s = MAX_RUNNING;
    if (s < 1) s = 1;
    if (n < 1) n = 1;
    if (t < 1) t = 1;

    /* ── Signal handlers ── */
    struct sigaction sa;
    sa.sa_handler = cleanup;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGALRM, &sa, NULL);
    alarm(5);

    /* ── Open log file ── */
    logfp = fopen(logfile, "w");
    if (!logfp) { perror("fopen"); return 1; }

    /* ── IPC setup ── */
    key_t shmkey = ftok(SHM_KEY_PATH, SHM_KEY_ID);
    if (shmkey == -1) { perror("ftok shm"); cleanup(0); return 1; }

    key_t msgkey = ftok(SHM_KEY_PATH, MQ_KEY_ID);
    if (msgkey == -1) { perror("ftok msg"); cleanup(0); return 1; }

    shmid = shmget(shmkey, sizeof(SimClock), IPC_CREAT | 0666);
    if (shmid == -1) { perror("shmget"); cleanup(0); return 1; }

    simClock = (SimClock *)shmat(shmid, NULL, 0);
    if (simClock == (void *)-1) { perror("shmat"); cleanup(0); return 1; }

    simClock->seconds     = 0;
    simClock->nanoseconds = 0;

    msqid = msgget(msgkey, IPC_CREAT | 0666);
    if (msqid == -1) { perror("msgget"); cleanup(0); return 1; }

    /* ── Initialize tables ── */
    for (int i = 0; i < MAX_PROCS; i++) {
        processTable[i].occupied = 0;
        processTable[i].blocked  = 0;
        processTable[i].localPid = 0;
        for (int p = 0; p < PAGES_PER_PROC; p++)
            processTable[i].pageTable[p] = -1;
    }

    for (int f = 0; f < TOTAL_FRAMES; f++) {
        frameTable[f].occupied = 0;
        frameTable[f].dirty    = 0;
        frameTable[f].pid      = -1;
        frameTable[f].page     = -1;
    }

    logWrite("OSS: Starting. n=%d s=%d t=%d i=%.2f logfile=%s\n",
             n, s, t, i_val, logfile);
    logWrite("OSS: Frame table: %d frames, %d pages per process\n",
             TOTAL_FRAMES, PAGES_PER_PROC);

    cleanup(0);
    return 0;
}