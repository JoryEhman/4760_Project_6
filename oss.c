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

/* ── Round-robin index ───────────────────────────────────────────────────── */
static int lastScheduled = 0;

/* ── Globals ─────────────────────────────────────────────────────────────── */
SimClock *simClock = NULL;
int       shmid    = -1;
int       msqid    = -1;
FILE     *logfp    = NULL;
int       logLines =  0;

PCB   processTable[MAX_PROCS];
Frame frameTable[TOTAL_FRAMES];

/* ── Statistics ──────────────────────────────────────────────────────────── */
static int totalReads      = 0;
static int totalWrites     = 0;
static int totalPageFaults = 0;

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
    p->occupied     = 1;
    p->pid          = pid;
    p->startSeconds = (int)simClock->seconds;
    p->startNano    = (int)simClock->nanoseconds;
    p->blocked      = 0;
    p->unblockSec   = 0;
    p->unblockNano  = 0;
    p->localPid     = slot + 1;
    for (int i = 0; i < PAGES_PER_PROC; i++)
        p->pageTable[i] = -1;

    logWrite("OSS: Launched user_proc PID %d in slot %d at %u:%09u\n",
             pid, slot, simClock->seconds, simClock->nanoseconds);
    return 0;
}

/* ── Print process table ─────────────────────────────────────────────────── */
static void printProcessTable(void) {
    logWrite("\n--- Process Table @ %u:%09u ---\n",
             simClock->seconds, simClock->nanoseconds);
    logWrite("%-5s %-8s %-8s\n", "Slot", "PID", "Blocked");
    for (int i = 0; i < MAX_PROCS; i++) {
        if (!processTable[i].occupied) continue;
        logWrite("%-5d %-8d %-8d\n",
                 i, processTable[i].pid, processTable[i].blocked);
    }
    logWrite("---\n\n");
}

/* ── Print memory map ────────────────────────────────────────────────────── */
static void printMemoryMap(void) {
    logWrite("\n--- Memory Map @ %u:%09u ---\n",
             simClock->seconds, simClock->nanoseconds);
    logWrite("%-8s %-8s %-8s %-8s %-8s\n",
             "Frame", "Occupied", "Dirty", "Process", "Page");
    for (int f = 0; f < TOTAL_FRAMES; f++) {
        logWrite("%-8d %-8s %-8d %-8d %-8d\n",
                 f,
                 frameTable[f].occupied ? "Yes" : "No",
                 frameTable[f].dirty,
                 frameTable[f].pid,
                 frameTable[f].page);
    }

    /* Page tables per process */
    for (int i = 0; i < MAX_PROCS; i++) {
        if (!processTable[i].occupied) continue;
        logWrite("P%d page table: [ ", processTable[i].localPid);
        for (int p = 0; p < PAGES_PER_PROC; p++)
            logWrite("%d ", processTable[i].pageTable[p]);
        logWrite("]\n");
    }
    logWrite("---\n\n");
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

    /* ── Launch timing ── */
    int launched = 0;
    unsigned long long lastLaunchNs = 0;
    unsigned long long intervalNs   =
        (unsigned long long)(i_val * (double)BILLION);

    /* ── Print timing ── */
    unsigned long long lastPrintNs  = 0;
    unsigned long long lastMemMapNs = 0;

    /* ── Main loop ── */
    while (launched < n || countActive() > 0) {

        unsigned long long nowNs =
            (unsigned long long)simClock->seconds * BILLION +
            simClock->nanoseconds;

        /* ── Try to launch a new child ── */
        if (launched < n && countActive() < s) {
            if (launched == 0 || nowNs - lastLaunchNs >= intervalNs) {
                int slot = findFreeSlot();
                if (slot != -1) {
                    if (launchChild(slot, t) == 0) {
                        launched++;
                        lastLaunchNs = nowNs;
                    }
                }
            }
        }

        /* ── Advance clock by 10ms ── */
        advanceClock(10000000);
        nowNs = (unsigned long long)simClock->seconds * BILLION +
                simClock->nanoseconds;

        /* ── Schedule one unblocked process (round-robin) ── */
        int scheduled = 0;
        for (int count = 0; count < MAX_PROCS && !scheduled; count++) {
            int i = (lastScheduled + count) % MAX_PROCS;
            if (!processTable[i].occupied) continue;
            if (processTable[i].blocked)   continue;

            lastScheduled = (i + 1) % MAX_PROCS;
            scheduled = 1;

            /* Send turn message */
            msgbuffer msg;
            msg.mtype   = (long)processTable[i].pid;
            msg.address = 1;
            msg.rwFlag  = 0;
            msgsnd(msqid, &msg, sizeof(msgbuffer) - sizeof(long), 0);

            /* Wait for reply */
            msgbuffer reply;
            if (msgrcv(msqid, &reply,
                       sizeof(msgbuffer) - sizeof(long),
                       getpid(), 0) == -1) {
                if (errno == EINTR) cleanup(0);
                perror("msgrcv");
                cleanup(0);
            }

            if (reply.address == -1) {
                /* ── Process terminating ── */
                logWrite("OSS: P%d PID %d terminating at %u:%09u\n",
                         i, processTable[i].pid,
                         simClock->seconds, simClock->nanoseconds);

                waitpid(processTable[i].pid, NULL, 0);
                processTable[i].occupied = 0;

            } else {
                /* ── Memory request ── */
                int addr = reply.address;
                int rw   = reply.rwFlag;
                int page = addr / PAGE_SIZE;

                if (rw == 0) {
                    totalReads++;
                    logWrite("OSS: P%d requesting read of address %d"
                             " (page %d) at %u:%09u\n",
                             i, addr, page,
                             simClock->seconds, simClock->nanoseconds);
                } else {
                    totalWrites++;
                    logWrite("OSS: P%d requesting write of address %d"
                             " (page %d) at %u:%09u\n",
                             i, addr, page,
                             simClock->seconds, simClock->nanoseconds);
                }

                /* For now just grant everything and advance clock by 100ns */
                advanceClock(NO_FAULT_TIME_NS);

                /* Send grant back */
                msgbuffer grant;
                grant.mtype   = (long)processTable[i].pid;
                grant.address = 1;
                grant.rwFlag  = 0;
                msgsnd(msqid, &grant,
                       sizeof(msgbuffer) - sizeof(long), 0);
            }
        }

        /* ── Print process table every 500ms ── */
        nowNs = (unsigned long long)simClock->seconds * BILLION +
                simClock->nanoseconds;
        if (nowNs - lastPrintNs >= 500000000ULL) {
            printProcessTable();
            lastPrintNs = nowNs;
        }

        /* ── Print memory map every 1 second ── */
        if (nowNs - lastMemMapNs >= 1000000000ULL) {
            printMemoryMap();
            lastMemMapNs = nowNs;
        }
    }

    /* ── Final report ── */
    double faultPct = (totalReads + totalWrites) > 0
        ? (double)totalPageFaults /
          (double)(totalReads + totalWrites) * 100.0 : 0.0;

    logWrite("\n=== OSS Final Report ===\n");
    logWrite("Total launched:     %d\n", launched);
    logWrite("Total reads:        %d\n", totalReads);
    logWrite("Total writes:       %d\n", totalWrites);
    logWrite("Total page faults:  %d\n", totalPageFaults);
    logWrite("Page fault %%:       %.1f%%\n", faultPct);
    logWrite("Final sim time:     %u:%09u\n",
             simClock->seconds, simClock->nanoseconds);

    cleanup(0);
    return 0;
}