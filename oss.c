/*
 * oss.c
 * Author: ehman
 * Date: 2026-04-29
 * Environment: Linux, gcc
 * Description: OSS memory management simulator (Project 6)
 *
 * Page replacement: FIFO
 * - Each process has 16 pages (1K each)
 * - System has 64 frames (64K total)
 * - Page fault blocks process for 14ms (disk I/O time)
 * - Dirty bit set on writes, adds extra time on swap-out
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

/* ── FIFO queue for frame replacement ───────────────────────────────────── */
static int fifoQueue[TOTAL_FRAMES];  /* stores frame indices in FIFO order */
static int fifoHead = 0;
static int fifoTail = 0;
static int fifoCount = 0;

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

/* ── FIFO queue helpers ──────────────────────────────────────────────────── */
static void fifoEnqueue(int frame) {
    fifoQueue[fifoTail] = frame;
    fifoTail = (fifoTail + 1) % TOTAL_FRAMES;
    fifoCount++;
}

static int fifoDequeue(void) {
    if (fifoCount == 0) return -1;
    int frame = fifoQueue[fifoHead];
    fifoHead = (fifoHead + 1) % TOTAL_FRAMES;
    fifoCount--;
    return frame;
}

/* ── Find a free frame ───────────────────────────────────────────────────── */
static int findFreeFrame(void) {
    for (int f = 0; f < TOTAL_FRAMES; f++)
        if (!frameTable[f].occupied) return f;
    return -1;
}

/* ── Release all frames held by a process ───────────────────────────────── */
static void releaseFrames(int slot) {
    for (int f = 0; f < TOTAL_FRAMES; f++) {
        if (frameTable[f].occupied &&
            frameTable[f].pid == processTable[slot].pid) {
            frameTable[f].occupied = 0;
            frameTable[f].dirty    = 0;
            frameTable[f].pid      = -1;
            frameTable[f].page     = -1;
        }
    }
    for (int p = 0; p < PAGES_PER_PROC; p++)
        processTable[slot].pageTable[p] = -1;
}

/*
 * ── Handle a memory request ───────────────────────────────────────────────
 * Returns 1 if granted immediately, 0 if page fault (process blocked)
 */
static int handleMemoryRequest(int slot, int address, int rwFlag) {
    int page  = address / PAGE_SIZE;
    int frame = processTable[slot].pageTable[page];

    if (frame != -1) {
        /* ── Page hit ── */
        if (rwFlag == 1) {
            frameTable[frame].dirty = 1;
            logWrite("OSS: Address %d in frame %d, writing data"
                     " to frame at %u:%09u\n",
                     address, frame,
                     simClock->seconds, simClock->nanoseconds);
        } else {
            logWrite("OSS: Address %d in frame %d, giving data to P%d"
                     " at %u:%09u\n",
                     address, frame, slot,
                     simClock->seconds, simClock->nanoseconds);
        }
        advanceClock(NO_FAULT_TIME_NS);
        return 1;  /* granted immediately */

    } else {
        /* ── Page fault ── */
        totalPageFaults++;
        logWrite("OSS: Address %d is not in a frame, page fault\n", address);

        /* Find a free frame or evict via FIFO */
        int newFrame = findFreeFrame();

        if (newFrame == -1) {
            /* No free frame — evict the oldest via FIFO */
            newFrame = fifoDequeue();

            /* Find which process/page owns this frame and clear it */
            int evictPid  = frameTable[newFrame].pid;
            int evictPage = frameTable[newFrame].page;

            /* Find the slot of the evicted process */
            for (int i = 0; i < MAX_PROCS; i++) {
                if (processTable[i].occupied &&
                    processTable[i].pid == evictPid) {
                    processTable[i].pageTable[evictPage] = -1;
                    break;
                }
            }

            logWrite("OSS: Clearing frame %d and swapping in P%d page %d\n",
                     newFrame, slot, page);

            if (frameTable[newFrame].dirty) {
                logWrite("OSS: Dirty bit of frame %d set,"
                         " adding additional time to clock\n", newFrame);
                advanceClock(DISK_RW_TIME_NS);  /* extra time for dirty write */
            }
        } else {
            logWrite("OSS: Using free frame %d for P%d page %d\n",
                     newFrame, slot, page);
        }

        /* Load the new page into the frame */
        frameTable[newFrame].occupied = 1;
        frameTable[newFrame].dirty    = (rwFlag == 1) ? 1 : 0;
        frameTable[newFrame].pid      = processTable[slot].pid;
        frameTable[newFrame].page     = page;
        processTable[slot].pageTable[page] = newFrame;

        /* Add to FIFO queue */
        fifoEnqueue(newFrame);

        /* Block the process for disk I/O time */
        unsigned long long unblockNs =
            (unsigned long long)simClock->seconds * BILLION +
            simClock->nanoseconds + DISK_RW_TIME_NS;

        processTable[slot].blocked     = 1;
        processTable[slot].unblockSec  = (int)(unblockNs / BILLION);
        processTable[slot].unblockNano = (int)(unblockNs % BILLION);

        logWrite("OSS: P%d blocked until %d:%09d\n",
                 slot,
                 processTable[slot].unblockSec,
                 processTable[slot].unblockNano);

        return 0;  /* page fault — process blocked */
    }
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

    /* Initialize FIFO queue */
    fifoHead  = 0;
    fifoTail  = 0;
    fifoCount = 0;

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

        /* ── Check if any blocked process can now be unblocked ── */
        for (int i = 0; i < MAX_PROCS; i++) {
            if (!processTable[i].occupied) continue;
            if (!processTable[i].blocked)  continue;

            unsigned long long unblockNs =
                (unsigned long long)processTable[i].unblockSec * BILLION +
                processTable[i].unblockNano;

            if (nowNs >= unblockNs) {
                processTable[i].blocked = 0;
                logWrite("OSS: P%d unblocked at %u:%09u\n",
                         i, simClock->seconds, simClock->nanoseconds);

                /* Send grant message to unblocked process */
                msgbuffer grant;
                grant.mtype   = (long)processTable[i].pid;
                grant.address = 1;
                grant.rwFlag  = 0;
                msgsnd(msqid, &grant,
                       sizeof(msgbuffer) - sizeof(long), 0);
            }
        }

        /* ── If all processes blocked, advance clock to next unblock ── */
        int allBlocked = 1;
        for (int i = 0; i < MAX_PROCS; i++) {
            if (processTable[i].occupied && !processTable[i].blocked) {
                allBlocked = 0;
                break;
            }
        }
        if (allBlocked && countActive() > 0) {
            /* Find earliest unblock time and jump to it */
            unsigned long long earliest = (unsigned long long)-1;
            for (int i = 0; i < MAX_PROCS; i++) {
                if (!processTable[i].occupied) continue;
                unsigned long long t2 =
                    (unsigned long long)processTable[i].unblockSec * BILLION +
                    processTable[i].unblockNano;
                if (t2 < earliest) earliest = t2;
            }
            if (earliest != (unsigned long long)-1 && earliest > nowNs) {
                unsigned int jump = (unsigned int)(earliest - nowNs);
                advanceClock(jump);
                nowNs = earliest;
            }
        }

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

                /* Release all frames held by this process */
                releaseFrames(i);

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

                int granted = handleMemoryRequest(i, addr, rw);

                if (granted) {
                    /* Send grant back immediately */
                    msgbuffer grant;
                    grant.mtype   = (long)processTable[i].pid;
                    grant.address = 1;
                    grant.rwFlag  = 0;
                    msgsnd(msqid, &grant,
                           sizeof(msgbuffer) - sizeof(long), 0);
                }
                /* If not granted (page fault), process stays blocked
                 * and will be unblocked by the check above */
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