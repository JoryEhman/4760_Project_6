/*
 * shared.h
 * Author: ehman
 * Date: 2026-04-29
 * Environment: Linux, gcc
 * Description: Shared definitions for OSS memory management (Project 6)
 */

#ifndef SHARED_H
#define SHARED_H

#include <sys/types.h>

/* ── Constants ───────────────────────────────────────────────────────────── */
#define MAX_PROCS        20
#define MAX_RUNNING      18
#define MAX_LOG_LINES 10000
#define BILLION    1000000000ULL

/* ── Memory layout ───────────────────────────────────────────────────────── */
#define TOTAL_FRAMES     64    /* 64K total memory / 1K page size          */
#define PAGES_PER_PROC   16    /* each process has 16K / 1K page size      */
#define PAGE_SIZE      1024    /* 1K pages                                  */

/* ── Timing constants ────────────────────────────────────────────────────── */
#define DISK_RW_TIME_NS  14000000   /* 14ms for disk read/write             */
#define NO_FAULT_TIME_NS      100   /* 100ns for a hit                      */

/* ── IPC keys ────────────────────────────────────────────────────────────── */
#define SHM_KEY_PATH  "oss.c"
#define SHM_KEY_ID    1
#define MQ_KEY_ID     2

/* ── Simulated clock ─────────────────────────────────────────────────────── */
typedef struct {
    unsigned int seconds;
    unsigned int nanoseconds;
} SimClock;

/* ── Frame table entry ───────────────────────────────────────────────────── */
typedef struct {
    int occupied;    /* 1 = frame in use, 0 = free          */
    int dirty;       /* 1 = written to, 0 = clean           */
    int pid;         /* which process owns this frame        */
    int page;        /* which page of that process           */
} Frame;

/* ── Process Control Block ───────────────────────────────────────────────── */
typedef struct {
    int   occupied;
    pid_t pid;
    int   startSeconds;
    int   startNano;
    int   blocked;           /* 1 = waiting on page fault I/O    */
    int   unblockSec;        /* simulated time to unblock        */
    int   unblockNano;
    int   pageTable[PAGES_PER_PROC]; /* frame number or -1 if not loaded */
    int   localPid;          /* simulated PID (slot + 1)         */
} PCB;

/* ── Message buffer ──────────────────────────────────────────────────────── */
/*
 * oss -> user_proc:  address == 1   your turn
 *                    address == 0   terminate now
 *
 * user_proc -> oss:  address >= 0   memory address being accessed
 *                    rwFlag  == 0   read
 *                    rwFlag  == 1   write
 *                    address == -1  terminating
 */
typedef struct {
    long mtype;
    int  address;   /* memory address (logical) or -1 for terminate */
    int  rwFlag;    /* 0 = read, 1 = write                          */
} msgbuffer;

#endif