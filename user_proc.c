/*
 * user_proc.c
 * Author: ehman
 * Date: 2026-04-29
 * Environment: Linux, gcc
 * Description: User process for OSS memory management (Project 6)
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/msg.h>
#include "shared.h"

int main(int argc, char *argv[]) {

    if (argc < 2) {
        fprintf(stderr, "user_proc: usage: user_proc <maxSeconds>\n");
        return 1;
    }

    int maxSeconds = atoi(argv[1]);

    srand((unsigned)(time(NULL) ^ ((unsigned long)getpid() * 2654435761UL)));

    /* ── Attach shared memory ── */
    key_t shmkey = ftok(SHM_KEY_PATH, SHM_KEY_ID);
    if (shmkey == -1) { perror("ftok shm"); return 1; }

    int shmid = shmget(shmkey, sizeof(SimClock), 0);
    if (shmid == -1) { perror("shmget"); return 1; }

    SimClock *simClock = (SimClock *)shmat(shmid, NULL, 0);
    if (simClock == (void *)-1) { perror("shmat"); return 1; }

    /* ── Attach message queue ── */
    key_t msgkey = ftok(SHM_KEY_PATH, MQ_KEY_ID);
    if (msgkey == -1) { perror("ftok msg"); return 1; }

    int msqid = msgget(msgkey, 0);
    if (msqid == -1) { perror("msgget"); return 1; }

    pid_t myPid     = getpid();
    pid_t parentPid = getppid();

    unsigned int birthSec = simClock->seconds;

    /* ── Main loop ── */
    while (1) {

        /* Wait for OSS to give us a turn */
        msgbuffer msg;
        if (msgrcv(msqid, &msg, sizeof(msgbuffer) - sizeof(long),
                   (long)myPid, 0) == -1) {
            perror("msgrcv");
            break;
        }

        /* OSS told us to terminate */
        if (msg.address == 0 && msg.rwFlag == 0) break;

        /* Check if time is up */
        if ((int)(simClock->seconds - birthSec) >= maxSeconds) {
            msgbuffer reply;
            reply.mtype   = (long)parentPid;
            reply.address = -1;
            reply.rwFlag  = 0;
            msgsnd(msqid, &reply, sizeof(msgbuffer) - sizeof(long), 0);
            break;
        }

        /* ── Generate random memory request ── */
        int page    = rand() % PAGES_PER_PROC;
        int offset  = rand() % PAGE_SIZE;
        int address = page * PAGE_SIZE + offset;
        int rwFlag  = (rand() % 100 < 80) ? 0 : 1; /* 80% read, 20% write */

        /* Send request to OSS */
        msgbuffer reply;
        reply.mtype   = (long)parentPid;
        reply.address = address;
        reply.rwFlag  = rwFlag;
        msgsnd(msqid, &reply, sizeof(msgbuffer) - sizeof(long), 0);

        /* Wait for OSS to grant the request */
        msgbuffer grant;
        if (msgrcv(msqid, &grant, sizeof(msgbuffer) - sizeof(long),
                   (long)myPid, 0) == -1) {
            perror("msgrcv grant");
            break;
        }
    }

    shmdt(simClock);
    return 0;
}