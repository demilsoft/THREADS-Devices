#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <THREADSLib.h>
#include <Messaging.h>
#include <Scheduler.h>
#include <TList.h>
#include <libuser.h>
#include <SystemCalls.h>

static int	ClockDriver(char*);
static int	DiskDriver(char*);
static void sysCall4(system_call_arguments_t* args);
static int disksize_real(int unit, int* sector, int* track, int* disk);
static int disk_io(void* dbuff, int diskOp, int unit, int track, int first, int sectors,int* status);
static inline void checkKernelMode(const char* functionName);

#define USERMODE   set_psr(get_psr() & ~PSR_KERNEL_MODE)
#define KERNELMODE set_psr(get_psr() | PSR_KERNEL_MODE)


typedef struct disk_request_struct
{
    void* buffer;
    int unit;
    int request;
    int platter;
    int sectorCount;
    int currentTrack;
    int currentSector;
    int sectorsProcessed;
    int bufferPosition;
    int status;
    int requestId;
} DiskRequest;

typedef struct devices_proc
{
    struct devices_proc* pNext;
    struct devices_proc* pPrev;
    int pid;
    int wakeTime;
    int waitSem;
    DiskRequest activeDiskRequest;
} devicesProc;


typedef struct 
{
    int tracks;
    int platters;
    char deviceName[THREADS_MAX_DEVICE_NAME];
} DiskInformation;

#define DISK_ARM_ALG_FCFS           0
#define DISK_ARM_ALG_SSF            1
#define DISK_ARM_ALG_ELEVATOR       2
#define DISK_ARM_ALG_ONE_DIRECTION  3
static int diskArmAlg = DISK_ARM_ALG_SSF;

static int OrderByWakeTime(void* pProc1, void* pProc2);
static int OrderByTrack(void* pReq1, void* pReq2);

static TList sleepingProcesses;
static int sleepingProcessesMutex;      // mutex to protect the sleepingProcs TList.
static devicesProc devicesProcs[MAXPROC]; // phase 4 proc table
static int running; /*semaphore to synchronize drivers and start3*/

static int diskpids[THREADS_MAX_DISKS];
static int diskSemaphores[THREADS_MAX_DISKS];      // notifications for the disk driver
static DiskInformation diskInfo[THREADS_MAX_DISKS];
static TList diskRequestQueue[THREADS_MAX_DISKS];
static int diskQueueMutex[THREADS_MAX_DISKS];


extern int DevicesEntryPoint(char*);

int SystemCallsEntryPoint(char* arg)
{
    char    buf[25];
    char    name[128];
    int		i;
    int		clockPID=0;
    int		status;

    checkKernelMode(__func__);

    /* Assignment system call handlers */
    systemCallVector[SYS_SLEEP] = sysCall4;
    systemCallVector[SYS_DISKREAD] = sysCall4;
    systemCallVector[SYS_DISKWRITE] = sysCall4;
    systemCallVector[SYS_DISKINFO] = sysCall4;

    /* Initialize the phase 4 process table */
    for (int i = 0; i < MAXPROC; ++i)
    {
        devicesProcs[i].waitSem = k_semcreate(0);
    }

    /* initialize the TList of sleeping processes and its protecting semaphore*/
    TListInitialize(&sleepingProcesses, 0, OrderByWakeTime);
    sleepingProcessesMutex = k_semcreate(1);

    /* initialize the TList of disk requests. */
    for (int i = 0; i < THREADS_MAX_DISKS; ++i)
    {
        TListInitialize(&diskRequestQueue[i], 0, OrderByTrack);
    }

    /* initialize the disk driver mailbox */
    for (int i = 0; i < THREADS_MAX_DISKS; ++i)
    {
        diskQueueMutex[i] = mailbox_create(1, sizeof(int));
    }
    /* initialize the disk driver mailbox */
    for (int i = 0; i < THREADS_MAX_DISKS; ++i)
    {
        diskSemaphores[i] = k_semcreate(0);
    }

    /*
     * Create clock device driver
     * I am assuming a semaphore here for coordination.  A mailbox can
     * be used instead -- your choice.
     */
    running = k_semcreate(0);
    clockPID = k_spawn("Clock driver", ClockDriver, NULL, THREADS_MIN_STACK_SIZE, HIGHEST_PRIORITY);
    if (clockPID < 0)
    {
        console_output(TRUE, "start3(): Can't create clock driver\n");
        stop(1);
    }

    /*
     * Wait for the clock driver to start. The idea is that ClockDriver
     * will V the semaphore "running" once it is running.
     */
    k_semp(running);

    /*
     * Create the disk device drivers here.  You may need to increase
     * the stack size depending on the complexity of your
     * driver, and perhaps do something with the pid returned.
     */
    for (i = 0; i < THREADS_MAX_DISKS; i++)
    {
        sprintf(buf, "%d", i);
        sprintf(name, "DiskDriver%d", i);
        diskpids[i] = k_spawn(name, DiskDriver, buf, THREADS_MIN_STACK_SIZE, HIGHEST_PRIORITY);
        if (diskpids[i] < 0)
        {
            console_output(TRUE, "start3(): Can't create disk driver %d\n", i);
            stop(1);
        }
    }
    k_semp(running);
    k_semp(running);

    /*
     * Create first user-level process and wait for it to finish.
     * These are lower-case because they are not system calls;
     * system calls cannot be invoked from kernel mode.
     * I'm assuming kernel-mode versions of the system calls
     * with lower-case names.
     */
    sys_spawn("DevicesEntryPoint", DevicesEntryPoint, NULL, 8 * THREADS_MIN_STACK_SIZE, 3);
    sys_wait(&status);

    /*
     * Zap the device drivers
     */
    k_kill(clockPID, SIG_TERM);  // clock driver
    k_wait(&status);  /* for the Clock Driver */

    /* The disk driver is waiting on the mailbox, release it to terminate */
    for (i = 0; i < THREADS_MAX_DISKS; i++)
    {
        k_semfree(diskSemaphores[i]);
        k_wait(&status);
    }
    return 0;
}

/*
*  Compares two proc structures for TList sorting.
*
*  Returns < 0 if proc1's wake time is larger than proc2's
*  or >= 0 otherwise.
*/
static int OrderByWakeTime(void* pProc1, void* pProc2)
{
    return (((struct devices_proc*)pProc2)->wakeTime - ((struct devices_proc*)pProc1)->wakeTime);
}

/*
*  Compares two disk request structures for TList sorting.
*
*  Returns < 0 if pReq1's track is larger than pReq2's
*  or >= 0 otherwise.
*/
static int OrderByTrack(void* pProc1, void* pProc2)
{
    return (((struct devices_proc*)pProc2)->activeDiskRequest.currentTrack -
        ((struct devices_proc*)pProc1)->activeDiskRequest.currentTrack);
}

/*
*  Prints the contents of a TList
*
*/
#ifdef  UNUSEDCODE__
static void DumpTrackValues(TList* pTrackList)
{
    devicesProc* pProc = NULL;

    printf("/********TRACK TList*********/\n");
    printf("  Count: %d\n", pTrackList->count);
    while ((pProc = TListGetNextNode(pTrackList, pProc)) != NULL)
    {
        printf("Track: %d\n", pProc->activeDiskRequest.currentTrack);
    }
    printf("/*****************/\n");
}
#endif

static int ClockDriver(char* arg)
{
    int result;
    int status;
    int curTime;
    devicesProc* pProc = NULL;

    /*
     * Let the parent know we are running and enable interrupts.
     */
    k_semv(running);

    set_psr(get_psr() | PSR_INTERRUPTS);

    while (!signaled())
    {
        result = wait_device("clock", &status);
        if (result != 0)
        {
            return 0;

        }

        /*
        * Compute the current time and wake up any processes
        * whose time has come.
        */
        curTime = system_clock();

        k_semp(sleepingProcessesMutex);

        static int i;
        pProc = TListGetNextNode(&sleepingProcesses, NULL);
        while (pProc != NULL)
        {
            if (pProc->wakeTime < curTime)
            {
                pProc = TListPopNode(&sleepingProcesses);

                k_semv(pProc->waitSem);
                pProc = TListGetNextNode(&sleepingProcesses, NULL);
            }
            else
            {
                pProc = NULL;
            }
        }
        i++;
        k_semv(sleepingProcessesMutex);
    }
    return 0;

}

/**********************************************************************
 * GetNextRequestingProc
 *
 * Gets the next requesting process based on the alg.
 *
***********************************************************************/
static devicesProc* GetNextRequestingProc(TList* pQueue,
    devicesProc* pCurProc,
    int* pForwardDirection,
    int currentArmPos)
{
    devicesProc* pNext = pCurProc;

    /* if FCFS, just pop and return */
    if (diskArmAlg == DISK_ARM_ALG_FCFS)
    {
        // FCFS Mode
        return TListPopNode(pQueue);
    }

    if (diskArmAlg == DISK_ARM_ALG_SSF)
    {
        /* If there is no current proc, then find the nearest track */
        if (pCurProc == NULL)
        {
            int lastDistance = 2500; // max tracks or greater
            int distance = 0;
            devicesProc* pProc = NULL;
            /* find the nearest requested track. */
            while ((pProc = TListGetNextNode(pQueue, pProc)) != NULL)
            {
                distance = abs(currentArmPos - pProc->activeDiskRequest.currentTrack);
                if (distance < lastDistance)
                {
                    pNext = pProc;
                    lastDistance = distance;
                }
                else
                {
                    // break because the TList is ordered and distance increased
                    break;
                }
            }
        }
        else
        {
            /* get the closest relative to the current, looks in both directions. */
            pNext = TListGetClosestNode(pQueue, pCurProc);
        }
    }
    else if (diskArmAlg == DISK_ARM_ALG_ELEVATOR)
    {
        devicesProc* pProc = NULL;

        if (pCurProc == NULL)
        {
            /* find the next based on direction. */
            for (int i = 0; i < 2; i++)
            {
                /* find the nearest requested track. */
                do
                {
                    if (*pForwardDirection)
                    {
                        /* find one that is greater than the current and stop */
                        pProc = TListGetNextNode(pQueue, pProc);
                        if (pProc != NULL && currentArmPos <= pProc->activeDiskRequest.currentTrack)
                        {
                            pNext = pProc;
                            break;
                        }
                    }
                    else
                    {
                        /* find one that is less than the current and stop */
                        pProc = TListGetPreviousNode(pQueue, pProc);
                        if (pProc != NULL && currentArmPos >= pProc->activeDiskRequest.currentTrack)
                        {
                            pNext = pProc;
                            break;
                        }
                    }
                } while (pProc != NULL);

                if (pProc == NULL && pQueue->count > 0)
                {
                    /* none met the criteria, reverse and look again. */
                    *pForwardDirection = ((*pForwardDirection) + 1) % 2;
                }
                else
                {
                    break;
                }
            }
        }
        else
        {
            if (*pForwardDirection)
            {
                pNext = TListGetNextNode(pQueue, pCurProc);
                if (pNext == NULL)
                {
                    pNext = TListGetPreviousNode(pQueue, pCurProc);

                    if (pNext != NULL)
                    {
                        // swap the arm direction 
                        *pForwardDirection = ((*pForwardDirection) + 1) % 2;
                    }
                }
            }
            else
            {
                pNext = TListGetPreviousNode(pQueue, pCurProc);
                if (pNext == NULL)
                {
                    pNext = TListGetNextNode(pQueue, pCurProc);

                    if (pNext != NULL)
                    {
                        // swap the arm direction 
                        *pForwardDirection = ((*pForwardDirection) + 1) % 2;
                    }
                }
            }
        }
    }
    else if (diskArmAlg == DISK_ARM_ALG_ONE_DIRECTION)
    {
        devicesProc* pProc = NULL;

        if (pCurProc == NULL)
        {
            /* find the nearest requested track looking forward only. */
            do
            {
                pProc = TListGetNextNode(pQueue, pProc);
                if (pProc != NULL && currentArmPos <= pProc->activeDiskRequest.currentTrack)
                {
                    pNext = pProc;
                    break;
                }
            } while (pProc != NULL);

            if (pProc == NULL)
            {
                pNext = TListGetNextNode(pQueue, pProc);
            }
        }
        else
        {
            pNext = TListGetNextNode(pQueue, pCurProc);
        }
    }
    return pNext;
}

/* read or writes sectors up to the end of the track. */
static int DiskTrackIO(DiskRequest* pRequest, int unit)
{
    int completedRequest = 0;
    int finishedIO = 0;
    int status;
    device_control_block_t devRequest;
    int result;

    while (!finishedIO)
    {
        devRequest.command = pRequest->request;
        devRequest.control1 = pRequest->platter;
        devRequest.control2 = pRequest->currentSector;

        if (devRequest.command == DISK_READ)
        {
            devRequest.input_data = (void*)((unsigned char*)pRequest->buffer + (pRequest->sectorsProcessed * THREADS_DISK_SECTOR_SIZE));
        }
        else
        {
            devRequest.output_data = (void*)((unsigned char*)pRequest->buffer + (pRequest->sectorsProcessed * THREADS_DISK_SECTOR_SIZE));
        }

        result = device_control(diskInfo[unit].deviceName, devRequest);


        if (result != 0)
        {
            // TODO: REPORT ERROR
            stop(1);
        }
        wait_device(diskInfo[unit].deviceName, &status);

        /* successfully processed the sector. */
        pRequest->sectorsProcessed++;
        pRequest->currentSector++;

        /* check to see if we are finished with the request. */
        if (pRequest->sectorCount == pRequest->sectorsProcessed)
        {
            completedRequest = 1;
            finishedIO = 1;
        }
        else if (pRequest->currentSector >= THREADS_DISK_SECTOR_COUNT)
        {
            /* reset the sector back 0. */
            pRequest->currentSector = 0;
            pRequest->currentTrack = (pRequest->currentTrack + 1) % diskInfo[unit].tracks;
            finishedIO = 1;
        }
    }
    return completedRequest;
}


static int DiskDriver(char* arg)
{
    int requestCompleted;
    int unit = atoi(arg);
    DiskRequest* pCurrentRequest;
    devicesProc* pRequestingProc = NULL;
    devicesProc* pNextRequestingProc = NULL;
    int currentTrack = 0;
    int result;
    int status;
    int myMutex;
    int forward = 1;  // direction of arm travel.
    TList* pQueue;
    device_control_block_t devRequest;


    set_psr(get_psr() | PSR_INTERRUPTS);

    pQueue = &diskRequestQueue[unit];
    myMutex = diskQueueMutex[unit];

    sprintf(diskInfo[unit].deviceName, "disk%d", unit);

    /* Get the number of tracks for this disk */
    devRequest.command = DISK_INFO;
    result = device_control(diskInfo[unit].deviceName, devRequest);
    if (result != 0)
    {
        console_output(TRUE, "DiskDriver %d: did not get DEV_OK on DISK_TRACKS call\n", unit);
        console_output(TRUE, "DiskDriver %d: is the file disk%d present???\n", unit, unit);
        stop(1);
    }

    wait_device(diskInfo[unit].deviceName, &status);
    diskInfo[unit].tracks = status & 0xffff;
    diskInfo[unit].platters = status >> 16;

    currentTrack = 0;


    /* start the track at 0 and platter 0 */
    devRequest.command = DISK_SEEK;
    devRequest.control1 = 0;
    devRequest.control2 = 0;
    result = device_control(diskInfo[unit].deviceName, devRequest);

    wait_device(diskInfo[unit].deviceName, &status);
    k_semv(running);

    while (!signaled())
    {
        if (k_semp(diskSemaphores[unit]) != 0)
        {
            // TODO make a test case that exercises this option.

            break;
        }

        mailbox_send(myMutex, NULL, 0, TRUE);
        pRequestingProc = NULL;
        while (pQueue->count > 0)
        {

            pRequestingProc = GetNextRequestingProc(pQueue, NULL, &forward, currentTrack);
            do
            {
                pCurrentRequest = &pRequestingProc->activeDiskRequest;

                /* Move the arm if needed */
                if (pCurrentRequest->currentTrack != currentTrack)
                {
                    currentTrack = pCurrentRequest->currentTrack;

                    /* start the track at 0 */
                    devRequest.command = DISK_SEEK;
                    devRequest.control1 = currentTrack;
                    devRequest.control2 = 0;
                    result = device_control(diskInfo[unit].deviceName, devRequest);

                    if (result != 0)
                    {
                        // TODO: REPORT ERROR
                        stop(1);
                    }
                    mailbox_receive(myMutex, NULL, 0, TRUE);
                    wait_device(diskInfo[unit].deviceName, &status);
                    mailbox_send(myMutex, NULL, 0, TRUE);
                }

                /* perform the I/O */
                mailbox_receive(myMutex, NULL, 0, TRUE);
                requestCompleted = DiskTrackIO(pCurrentRequest, unit);
                mailbox_send(myMutex, NULL, 0, TRUE);

                if (requestCompleted)
                {
                    devicesProc* pDoneProc;
                    pDoneProc = pRequestingProc;

                    /* move to the next one in the TList. */
                    pRequestingProc = GetNextRequestingProc(pQueue, pRequestingProc, &forward, currentTrack);

                    /* in FCFS the current request is already off the TList. */
                    if (diskArmAlg != DISK_ARM_ALG_FCFS)
                    {
                        /* remove the node that is complete */
                        TListRemoveNode(pQueue, pDoneProc);
                    }

                    /* signal the process of the complete io */
                    pDoneProc->activeDiskRequest.status = 0;
                    k_semv(pDoneProc->waitSem);
                }
                else
                {
                    /* get the next node from the TList */
                    pNextRequestingProc = GetNextRequestingProc(pQueue, pRequestingProc, &forward, currentTrack);

                    /* remove the request and re-insert in the ordered TList */
                    TListRemoveNode(pQueue, pRequestingProc);
                    TListAddNodeInOrder(pQueue, pRequestingProc);

                    /* move the current proc pointers */
                    pRequestingProc = pNextRequestingProc;
                }
            } while (pRequestingProc != NULL);
        }
        mailbox_receive(myMutex, NULL, 0, TRUE);
    }
    return 0;
}

/*
 *  Routine:  sys_disk_io
 *
 *  Description: This is the call entry point for disk input.
 *
 *  Arguments:
 *        char  *deviceName  -- which disk
 *        int   operation - read or write
 *        void  *dataBuffer  -- Buffer to place the data
 *        int   platter      -- platter to read from
 *        int   track        -- first track to read
 *        int   firstSector  -- first sector to read
 *        int   sectors      -- number of sectors to read
 *        int   *status      -- pointer to output value
 *                (output value: completion status)
 *
 *  Return Value: 0 means success, -1 means error occurs
 *
 */
static int sys_disk_io(char* deviceName, int operation, void* dataBuffer, int platter, int track, int firstSector, int sectors, int* status)
{
    int procPosition;
    DiskRequest* pRequest;
    static int id;
    int deviceHandle;
    int unit;

    deviceHandle = device_handle(deviceName);

    if (deviceHandle < 0)
    {
        return -1;
    }

    unit = deviceHandle - 1;
    if (track < 0 || track >= diskInfo[unit].tracks || firstSector >= THREADS_DISK_SECTOR_COUNT || platter < 0 || platter >= diskInfo[unit].platters)
    {
        return -1;
    }

    procPosition = k_getpid() % MAXPROC;

    devicesProcs[procPosition].pid = k_getpid();
    pRequest = &devicesProcs[procPosition].activeDiskRequest;

    pRequest->buffer = dataBuffer;
    pRequest->unit = unit;
    pRequest->currentTrack = track;
    pRequest->currentSector = firstSector;
    pRequest->sectorCount = sectors;
    pRequest->platter = platter;
    pRequest->request = operation;
    pRequest->sectorsProcessed = pRequest->bufferPosition = 0;
    pRequest->status = -1;
    pRequest->requestId = id++;

    mailbox_send(diskQueueMutex[unit], NULL, 0, TRUE);
    if (diskArmAlg == DISK_ARM_ALG_FCFS)
    {
        TListAddNode(&diskRequestQueue[unit], &devicesProcs[procPosition]);
    }
    else
    {
        TListAddNodeInOrder(&diskRequestQueue[unit], &devicesProcs[procPosition]);
    }
    mailbox_receive(diskQueueMutex[unit], NULL, 0, TRUE);

    /* conditional send here to driver  wake if the TList is empty*/
    k_semv(diskSemaphores[unit]);

    k_semp(devicesProcs[procPosition].waitSem);


    *status = pRequest->status;

    return 0;
} 


/*
 *  Routine:  DiskInfo
 *
 *  Description: This is the call entry point for getting the disk size.
 *
 *  Arguments:
 *        char  *deviceName  -- which disk
 *		  int	*sectorSize  -- # bytes in a sector
 *		  int	*sectorCount -- # sectors in a track
 *		  int   *trackCount  -- # tracks in the disk
 *		  int	*platters    -- # platters on the disk
 *                (output value: completion status)
 *
 *  Return Value: 0 means success, -1 means error occurs
 *
 */
int sys_diskinfo(char* deviceName, int* sectorSize, int* sectorCount, int* trackCount, int* platterCount)
{
    int deviceHandle;
    deviceHandle = device_handle(deviceName);
    int unit;

    deviceHandle = device_handle(deviceName);

    if (deviceHandle < 0)
    {
        return -1;
    }

    unit = deviceHandle - 1;

    if (sectorSize != NULL)
    {
        *sectorSize = THREADS_DISK_SECTOR_SIZE;
    }

    if (trackCount != NULL)
    {
        *trackCount = diskInfo[unit].tracks;
    }

    if (sectorCount != NULL)
    {
        *sectorCount = THREADS_DISK_SECTOR_COUNT;
    }

    if (platterCount != NULL)
    {
        *platterCount = diskInfo[unit].platters;
    }

    return 0;
}

/*
 *  Routine:  sleep_real
 *
 *  Description: This is the implementation for timed delay.
 *
 *  Arguments:    int seconds -- number of seconds to sleep
 *
 *  Return Value: 0 means success, -1 means error occurs
 *
 */
int  sleep_real(int seconds)
{
    int procPosition;
    int curTime;
    if (seconds <= 0)
    {
        return -1;
    }

    procPosition = k_getpid() % MAXPROC;

    curTime = system_clock();

    // save the wake up time in microseconds
    devicesProcs[procPosition].wakeTime = curTime + (seconds * 1000 * 1000);

    k_semp(sleepingProcessesMutex);
    // add this to the TList of sleeping processes
    TListAddNodeInOrder(&sleepingProcesses, &devicesProcs[procPosition]);

    k_semv(sleepingProcessesMutex);

    k_semp(devicesProcs[procPosition].waitSem);

    return 0;
}

static void sysCall4(system_call_arguments_t* args)
{
    char* pFunctionName = "Undefined";

    if (args == NULL)
    {
        console_output(TRUE, "sysCall(): Invalid syscall %d, no arguments.\n", args->call_id);
        console_output(TRUE, "sysCall(): process %d terminating\n", k_getpid());
        sys_exit(1);
    }

    switch (args->call_id)
    {
    case SYS_SLEEP:
        pFunctionName = "SYS_SLEEP";
        args->arguments[3] = (intptr_t)sleep_real((int)args->arguments[0]);
        break;
    case SYS_DISKREAD:
        pFunctionName = "SYS_DISKREAD";
        args->arguments[3] = (intptr_t)sys_disk_io(
            (char *)args->arguments[0],
            DISK_READ,
            (void*)args->arguments[1],
            (int)args->arguments[2],
            (int)args->arguments[3],
            (int)args->arguments[4],
            (int)args->arguments[5],
            (int *)&args->arguments[0]);
        break;
    case SYS_DISKWRITE:
        pFunctionName = "SYS_DISKWRITE";
        args->arguments[3] = (intptr_t)sys_disk_io(
            (char*)args->arguments[0],
            DISK_WRITE,
            (void *)args->arguments[1],
            (int)args->arguments[2],
            (int)args->arguments[3],
            (int)args->arguments[4],
            (int)args->arguments[5],
            (int*)&args->arguments[0]);
        break;
    case SYS_DISKINFO:
        pFunctionName = "SYS_DISKSIZE";
        args->arguments[3] = (intptr_t)sys_diskinfo(
            (char *)args->arguments[0],
            (int*)&args->arguments[0],
            (int*)&args->arguments[1],
            (int*)&args->arguments[2],
            (int*)&args->arguments[4]);
        break;
    default:
        console_output(TRUE, "Bad system call number!");
        stop(1);
        break;
    }

    // if signaled when in the sys handler, then terminate
    if (signaled())
    {
        console_output(TRUE, "%s - Process signaled while in system call.", pFunctionName);
        sys_exit(0);
    }

    USERMODE;
}



struct psr_bits {
    unsigned int cur_int_enable : 1;
    unsigned int cur_mode : 1;
    unsigned int prev_int_enable : 1;
    unsigned int prev_mode : 1;
    unsigned int unused : 28;
};

union psr_values {
    struct psr_bits bits;
    unsigned int integer_part;
};


/*****************************************************************************
   Name - checkKernelMode
   Purpose - Checks the PSR for kernel mode and stops if in user mode
   Parameters -
   Returns -
   Side Effects - Will stop if not in kernel mode
****************************************************************************/
static inline void checkKernelMode(const char* functionName)
{
    union psr_values psrValue;


    //    console_output(TRUE, "checkKernelMode(): verifying kernel mode for %d, %s\n", 1, functionName);

    psrValue.integer_part = get_psr();
    if (psrValue.bits.cur_mode == 0)
    {
        console_output(FALSE, "Kernel mode expected, but function called in user mode.\n");
        stop(1);
    }
}
