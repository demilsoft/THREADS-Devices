////////////////////////////////////////////////////////////////////////////
// CYBV 489 - OS Theory 
// Name: Dean Lewis
//
// Devices.c - THREADs Part 4
// 
// This code file implements the clock and disk drivers, and the system call handler. 
// The clock driver should wake up sleeping processes at the appropriate time, and the disk driver 
// should handle  user level process disk requests. Implements the disk arm 
// scheduling algorithms (FCFS or SSTF) in the disk driver. The system call handler will be 
// implemented using SystemCalls.c.
////////////////////////////////////////////////////////////////////////////

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
#include <Devices.h>

/* Set the disk arm scheduling algorithm.
* See Devices.h for available constants (DISK_ARM_ALG_FCFS, DISK_ARM_ALG_SSTF, etc.).
* You must implement FCFS and SSTF. Change this value to test each algorithm.
* Submissions will be assessed with DISK_ARM_ALG_FCFS and DISK_ARM_ALG_SSTF. */

//////////////////// SCHEDULING ALGORITHM SELECTION  ////////////////////////
/////////////////////////////// SSTF  or  FCFS //////////////////////////////
#define DISK_ARM_ALG   DISK_ARM_ALG_SSTF
// #define DISK_ARM_ALG   DISK_ARM_ALG_FCFS
//////////////////// SCHEDULING ALGORITHM SELECTION  ////////////////////////

///////////////////////// Define Statements /////////////////////////
// Migrated Prior Project
#define USERMODE    set_psr(get_psr() & ~PSR_KERNEL_MODE)
#define CLOCK_TICK_US 67000                                                         // TEST00 ADD microseconds
#define SYS_DISKREAD_CALL		    11                                              // TEST13 ADD local disk read syscall id
#define SYS_DISKWRITE_CALL		    12                                              // TEST13 ADD local disk write syscall id
#define SYS_DISKINFO_CALL           13                                              // TEST02 ADD local disk info syscall id
///////////////////////// Types and Structures ////////////////////////
typedef struct devices_proc
{
    struct devices_proc* pNext;
    struct devices_proc* pPrev;
    int pid;
    int mboxSleep;                                                                  // TEST00 ADD private sleep mailbox
} DevicesProcess;

typedef struct
{
    int tracks;
    int platters;
    int sectors;                                                                    // TEST02 ADD sectors per track
    char deviceName[THREADS_MAX_DEVICE_NAME];
} DiskInformation;
////////////////////// User Types and Structures //////////////////////
typedef struct sleep_request
{
    struct sleep_request* pNext;                                                    // TEST00 ADD next sleeper
    int pid;                                                                        // TEST00 ADD store sleep pid
    int wakeTime;                                                                   // TEST00 ADD store wake time
} SleepRequest;

typedef struct disk_request
{
    struct disk_request* pNext;                                                     // TEST04 ADD next req
    int pid;                                                                        // TEST04 ADD request pid
    char deviceName[THREADS_MAX_DEVICE_NAME];                                       // TEST04 ADD target device 
    void* dataBuffer;                                                               // TEST04 ADD user buffer ptr
    int platter;                                                                    // TEST04 ADD start platter
    int track;                                                                      // TEST04 ADD start track
    int firstSector;                                                                // TEST04 ADD start sector
    int sectorCount;                                                                // TEST04 ADD sectors
    int isWrite;                                                                    // TEST04 ADD request direction
    int status;                                                                     // TEST04 ADD complete status
    int result;                                                                     // TEST04 ADD syscall result
    int completeMbox;                                                               // TEST04 ADD complete mailbox
} DiskRequest;
///////////////////////// Types and Structures ////////////////////////

//////////////////////// Prototypes & Globals /////////////////////////
static int sleepClockTime = 0;                                                      // TEST00 ADD shared sleep clock

static DevicesProcess devicesProcs[MAXPROC];
static DiskInformation diskInfo[THREADS_MAX_DISKS];

static SleepRequest sleepRequests[MAXPROC];                                         // TEST00 ADD sleep req table
static SleepRequest* pSleepHead = NULL;                                             // TEST00 ADD sleep queue head
static DiskRequest diskRequests[MAXPROC];                                           // TEST04 ADD req table
static DiskRequest* pDiskQueueHead[THREADS_MAX_DISKS];                              // TEST04 ADD disk queue head
static DiskRequest* pop_disk_request(int unit);                                     // TEST04 ADD pop disk 
static DiskRequest* request_meth(int unit);                                         // TEST08 ADD select next 
static DiskRequest* fcfs_request(int unit);                                         // TEST08 ADD dequeue fcfs
static DiskRequest* sstf_request(int unit);                                         // TEST08 ADD dequeue sstf

static int ClockDriver(char*);
static int DiskDriver(char*);
static int clockReadyMbox;
static int diskRequestMbox[THREADS_MAX_DISKS];                                      // TEST04 ADD disk wake mailbox
static int diskReadyMbox;                                                           // TEST13 ADD disk driver ready mailbox
static int diskCurrentTrack[THREADS_MAX_DISKS];                                     // TEST08 ADD current arm track disk
static int diskTrackKnown[THREADS_MAX_DISKS];                                       // TEST21 ADD flag per disk
extern int DevicesEntryPoint(void* pArgs);                                          // TEST00 ALTER test signature
static inline void checkKernelMode(const char* functionName);
static void system_call_handler(system_call_arguments_t* args);                     // TEST00 ADD dispatch device syscalls

static int get_current_time(void);                                                  // TEST00 ADD read time
static int get_disk_unit(char* deviceName);
static void insert_disk_request(int unit, DiskRequest* _DiskRequest);               // TEST04 ADD queue disk 
static void insert_sleep_request(SleepRequest* _SleepRequest);                      // TEST00 ADD queue sleep process
static int disk_transfer(DiskRequest* _DiskRequest);                                // TEST04 ADD exec disk                      
int sys_disk_io(char* deviceName, void* dataBuffer, int platter, int track,
    int firstSector, int sectors, int isWrite, int* status);                        // TEST03 ADD shared disk io
//////////////////////// Prototypes & Globals ////////////////////////////// 

////////////////////////////////////////////////////////////////////////////
// Entry point for the devices module. 
////////////////////////////////////////////////////////////////////////////
int SystemCallsEntryPoint(char* arg)
{
    char    buf[25];
    char    name[128];
    int     i;
    int     clockPID = 0;
    int     diskPids[THREADS_MAX_DISKS];
    int     status;

    checkKernelMode(__func__);

    /* Assign system call handlers */
    // Pulled from prior project
    systemCallVector[SYS_SLEEP] = system_call_handler;                          // TEST00 ADD sleep syscall
    systemCallVector[SYS_DISKINFO_CALL] = system_call_handler;                  // TEST02 ADD disk info syscall
    systemCallVector[SYS_DISKREAD_CALL] = system_call_handler;                  // TEST03 ADD disk read syscall
    systemCallVector[SYS_DISKWRITE_CALL] = system_call_handler;                 // TEST03 ADD disk write syscall

    /* Initialize the process table */
    for (i = 0; i < MAXPROC; ++i)                                               // TEST02 Disk Proc table init
    {
        devicesProcs[i].pNext = NULL;
        devicesProcs[i].pPrev = NULL;
        devicesProcs[i].pid = -1;
        devicesProcs[i].mboxSleep = mailbox_create(1, 0);

        sleepRequests[i].pNext = NULL;
        sleepRequests[i].pid = -1;
        sleepRequests[i].wakeTime = 0;

        diskRequests[i].pNext = NULL;
        diskRequests[i].pid = -1;
        diskRequests[i].completeMbox = mailbox_create(1, 0);
        diskRequests[i].status = -1;
        diskRequests[i].result = -1;
    }

    /* Initialize disk geometry before drivers or tests start */
    for (i = 0; i < THREADS_MAX_DISKS; ++i)
    {
        memset(&diskInfo[i], 0, sizeof(DiskInformation));                       // TEST13 ADD clear disk info
        sprintf(diskInfo[i].deviceName, "disk%d", i);                           // TEST13 ADD disk name
        diskInfo[i].sectors = THREADS_DISK_SECTOR_COUNT;                        // TEST13 ADD sectors per track
    }

    diskInfo[0].tracks = 128;                                                   // TEST13 ADD init disk0 tracks
    diskInfo[0].platters = 1;                                                   // TEST13 ADD init disk0 platters
    diskInfo[1].tracks = 512;                                                   // TEST13 ADD init disk1 tracks
    diskInfo[1].platters = 3;                                                   // TEST13 ADD init disk1 platters

    clockReadyMbox = mailbox_create(1, 0);                                      // TEST13 ADD clock ready mailbox
    diskReadyMbox = mailbox_create(THREADS_MAX_DISKS, 0);                       // TEST13 ADD disk ready mailbox

    /* Create and start the clock driver */
    clockPID = k_spawn("Clock driver", ClockDriver, NULL, THREADS_MIN_STACK_SIZE, HIGHEST_PRIORITY);
    if (clockPID < 0)
    {
        console_output(TRUE, "start3(): Can't create clock driver\n");
        stop(1);
    }

    /* Create the disk drivers */
    for (i = 0; i < THREADS_MAX_DISKS; i++)
    {
        sprintf(buf, "%d", i);
        sprintf(name, "DiskDriver%d", i);

        pDiskQueueHead[i] = NULL;                                               // TEST04 ADD clear disk queue head
        diskRequestMbox[i] = mailbox_create(MAXPROC, 0);                        // TEST04 ADD create disk wake mailbox
        diskCurrentTrack[i] = 0;                                                // TEST08 ADD init arm position
        diskTrackKnown[i] = 0;

        diskPids[i] = k_spawn(name, DiskDriver, buf, THREADS_MIN_STACK_SIZE * 4, HIGHEST_PRIORITY);
        if (diskPids[i] < 0)
        {
            console_output(TRUE, "start3(): Can't create disk driver %d\n", i);
            stop(1);
        }
    }

    /* Wait for background drivers before starting the user test */
    mailbox_receive(clockReadyMbox, NULL, 0, TRUE);

    for (i = 0; i < THREADS_MAX_DISKS; i++)                                     // TEST13 ALTER wait for disk drivers ready first
    {
        mailbox_receive(diskReadyMbox, NULL, 0, TRUE);                          // TEST13 ALTER wait for one disk driver ready
    }

    /* Create first user-level process and wait for it to finish */
    sys_spawn("DevicesEntryPoint", DevicesEntryPoint, NULL, 8 * THREADS_MIN_STACK_SIZE, 3);

    sys_wait(&status);                                                          // TEST00 ALTER wait for user test

    k_kill(clockPID, SIG_TERM);                                                 // TEST00 ADD stop clock driver
    k_wait(&status);                                                            // TEST00 ADD reap clock driver

    for (i = 0; i < THREADS_MAX_DISKS; i++)                                     // TEST02 ADD stop disk drivers
    {
        k_kill(diskPids[i], SIG_TERM);                                          // TEST02 ADD signal disk exit
        mailbox_send(diskRequestMbox[i], NULL, 0, FALSE);                       // TEST07 ADD wake blocked disk 
        k_wait(&status);                                                        // TEST02 ADD wait disk 
    }

    return 0;
}

static int ClockDriver(char* arg)
{
    int result;
    int status;

    set_psr(get_psr() | PSR_INTERRUPTS);
    mailbox_send(clockReadyMbox, NULL, 0, FALSE);                               // TEST13 ADD signal clock ready

    while (!signaled())
    {
        result = wait_device("clock", &status);
        if (result != 0)
        {
            return 0;
        }

        sleepClockTime += CLOCK_TICK_US;                                        // TEST00 ADD fwd sleep clock

        int currentTime;                                                        // TEST00 ADD curr wall clock
        SleepRequest* _WakeRequest;                                             // TEST00 ADD next sleeper
        currentTime = get_current_time();                                       // TEST00 ALTER read curr time

        while (pSleepHead != NULL && pSleepHead->wakeTime <= currentTime)
        {
            _WakeRequest = pSleepHead;                                           // TEST00 ADD capture ready sleeper
            pSleepHead = pSleepHead->pNext;                                      // TEST00 ADD remove queue head

            mailbox_send(devicesProcs[_WakeRequest->pid % MAXPROC].mboxSleep,
                NULL, 0, FALSE);                                              // TEST00 ALTER wake sleep mailbox
        }
    }
    return 0;
}

static int DiskDriver(char* arg)
{
    int unit = atoi(arg);

    set_psr(get_psr() | PSR_INTERRUPTS);

    mailbox_send(diskReadyMbox, NULL, 0, FALSE);                                // TEST13 ADD signal disk ready

    while (!signaled())
    {
        DiskRequest* _DiskRequest;                                              // TEST04 ADD next disk req

        mailbox_receive(diskRequestMbox[unit], NULL, 0, TRUE);                  // TEST04 ADD wait for req

        if (signaled())
        {
            break;                                                              // TEST04 ADD stop after signal
        }

        _DiskRequest = request_meth(unit);                                      // TEST04 ADD dequeue next req
        if (_DiskRequest == NULL)
        {
            continue;                                                           // TEST04 ADD ignore empty
        }

        _DiskRequest->result = disk_transfer(_DiskRequest);                     // TEST04 ADD perform disk req
        _DiskRequest->status = (_DiskRequest->result == 0) ? 0 : -1;            // TEST13 ADD set req status

        mailbox_send(_DiskRequest->completeMbox, NULL, 0, FALSE);               // TEST04 ADD wake req process      
    }

    return 0;
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

    psrValue.integer_part = get_psr();
    if (psrValue.bits.cur_mode == 0)
    {
        console_output(FALSE, "Kernel mode expected, but function called in user mode.\n");
        stop(1);
    }
}

int sys_sleep(int seconds)
{
    int pid;                                                                    // TEST00 ADD cur proc id
    int sleepBaseTime;                                                          // TEST00 ADD cur system time
    SleepRequest* _SleepRequest;                                                // TEST00 ADD request table entry

    checkKernelMode(__func__);

    if (seconds <= 0)
    {
        return ERR_INVALID;                                                     // TEST20 ALTER reject 0 and - sleep
    }

    pid = k_getpid();                                                           // TEST00 ALTER use k pid

    devicesProcs[pid % MAXPROC].pid = pid;                                      // TEST00 ADD record sleep pid

    sleepBaseTime = get_current_time();                                         // TEST00 ALTER read cur time

    _SleepRequest = &sleepRequests[pid % MAXPROC];                              // TEST00 ADD select req slot
    _SleepRequest->pNext = NULL;                                                // TEST00 ADD clear next ptr
    _SleepRequest->pid = pid;                                                   // TEST00 ADD record sleep pid
    _SleepRequest->wakeTime = sleepBaseTime + (seconds * 1000000);              // TEST00 ADD compute wake

    insert_sleep_request(_SleepRequest);                                        // TEST00 ADD insert sleep queue
    mailbox_receive(devicesProcs[pid % MAXPROC].mboxSleep, NULL, 0, TRUE);      // TEST00 ALTER block sleep mailbox

    return ERR_OK;                                                              // TEST00 ADD sleep complete
}

static void insert_sleep_request(SleepRequest* _SleepRequest)
{
    SleepRequest* _Current;                                                     // TEST00 ADD set cur node
    SleepRequest* _Previous;                                                    // TEST00 ADD set prior node

    if (pSleepHead == NULL || _SleepRequest->wakeTime < pSleepHead->wakeTime)
    {
        _SleepRequest->pNext = pSleepHead;                                      // TEST00 ADD insert head
        pSleepHead = _SleepRequest;                                             // TEST00 ADD update head
        return;
    }

    _Previous = pSleepHead;                                                     // TEST00 ADD start head
    _Current = pSleepHead->pNext;                                               // TEST00 ADD advance next node

    while (_Current != NULL && _Current->wakeTime <= _SleepRequest->wakeTime)
    {
        _Previous = _Current;                                                   // TEST00 ADD move prev forward
        _Current = _Current->pNext;                                             // TEST00 ADD move cur forward
    }

    _SleepRequest->pNext = _Current;                                            // TEST00 ADD link new req
    _Previous->pNext = _SleepRequest;
}

// System call handler - migrated
static void system_call_handler(system_call_arguments_t* args)
{
    checkKernelMode(__func__);                                                  // TEST00 ADD validate kernel mode

    if (args == NULL)
    {
        console_output(FALSE, "system_call_handler(): NULL args\n");            // TEST00 ADD reject null syscall args
        stop(1);
    }

    switch (args->call_id)
    {
    case SYS_SLEEP:
    {
        int result;

        result = sys_sleep((int)args->arguments[0]);
        args->arguments[3] = result;
        break;
    }
    case SYS_DISKINFO_CALL:
    {
        int unit;
        int sectorSize;
        int sectorCount;
        int trackCount;
        int platterCount;
        int result;

        unit = get_disk_unit((char*)args->arguments[0]);                        // TEST02 ADD map disk name to unit

        result = sys_disk_info(unit, &platterCount, &sectorCount,
            &trackCount, &sectorSize);                                          // TEST02 ADD fetch disk info

        args->arguments[0] = sectorSize;                                        // TEST02 ALTER sector size
        args->arguments[1] = sectorCount;                                       // TEST02 ALTER sector count
        args->arguments[2] = trackCount;                                        // TEST02 ALTER track cnt
        args->arguments[3] = result;                                            // TEST02 ALTER syscall status
        args->arguments[4] = platterCount;                                      // TEST02 ALTER platter cnt
        break;
    }
    case SYS_DISKREAD_CALL:
    {
        int result;
        int diskStatus;

        result = sys_disk_io(
            (char*)args->arguments[0],
            (void*)args->arguments[1],
            (int)args->arguments[2],
            (int)args->arguments[3],
            (int)args->arguments[4],
            (int)args->arguments[5],
            0,
            &diskStatus);                                                       // TEST03 ALTER use local status storage

        args->arguments[0] = diskStatus;                                        // TEST03 ADD disk status
        args->arguments[3] = result;                                            // TEST03 ADD disk read result
        break;
    }
    case SYS_DISKWRITE_CALL:
    {
        int result;                                                             // TEST03 ADD disk write result
        int diskStatus;                                                         // TEST03 ADD local disk status

        result = sys_disk_io(
            (char*)args->arguments[0],
            (void*)args->arguments[1],
            (int)args->arguments[2],
            (int)args->arguments[3],
            (int)args->arguments[4],
            (int)args->arguments[5],
            1,
            &diskStatus);                                                       // TEST03 ALTER use local status storage

        args->arguments[0] = diskStatus;                                        // TEST03 ADD disk status
        args->arguments[3] = result;                                            // TEST03 ADD disk write result
        break;
    }
    default:
    {
        console_output(FALSE, "nullsys3(): Invalid system_call %d\n",
            args->call_id);                                                  // TEST00 ADD invalid syscall
        stop(1);
        break;
    }
    }

    USERMODE;                                                                   // TEST00 ADD return to user mode
}

// Wait time process migrated
static int get_current_time(void)
{
    checkKernelMode(__func__);                                                  // TEST00 ADD validate kernel mode
    return sleepClockTime;
}

static int get_disk_unit(char* deviceName)
{
    uint32_t deviceHandle;                                                      // TEST21 ADD device handle

    if (deviceName == NULL)
    {
        return -1;                                                              // TEST21 ADD reject null 
    }

    deviceHandle = device_handle(deviceName);                                   // TEST21 ADD resolve device handle
    if (deviceHandle == 0 || deviceHandle == 255 || deviceHandle > THREADS_MAX_DISKS)
    {
        return -1;                                                              // TEST21 ADD reject unknown device handle
    }

    if (strcmp(deviceName, "disk0") == 0)
    {
        return 0;
    }

    if (strcmp(deviceName, "disk1") == 0)
    {
        return 1;
    }

    return -1;                                                                  // TEST21 ADD reject disk name
}

// disk info syscall
int sys_disk_info(int unit, int* platters, int* sectors, int* tracks, int* disk)
{
    checkKernelMode(__func__);                                                  // TEST02 ADD validate kernel mode

    if (unit < 0 || unit >= THREADS_MAX_DISKS)                                  // TEST02 ADD reject invalid disk unit
    {
        return -1;
    }

    if (platters == NULL || sectors == NULL || tracks == NULL || disk == NULL)  // TEST02 ADD validate output ptr
    {
        return -1;
    }

    *platters = diskInfo[unit].platters;                                        // TEST02 ADD platter count
    *sectors = diskInfo[unit].sectors;                                          // TEST02 ADD sector count
    *tracks = diskInfo[unit].tracks;                                            // TEST02 ADD track count
    *disk = THREADS_DISK_SECTOR_SIZE;                                           // TEST02 ADD sector size

    return 0;
}

// shared disk io
int sys_disk_io(char* deviceName, void* dataBuffer, int platter, int track,
    int firstSector, int sectors, int isWrite, int* status)
{
    int pid;
    int unit;
    DiskRequest* _DiskRequest;

    checkKernelMode(__func__);                                                  // TEST04 ADD validate kernel mode

    if (status == NULL)
    {
        return -1;                                                              // TEST04 ADD reject null status
    }

    *status = -1;                                                               // TEST04 ADD default failure status

    if (deviceName == NULL || dataBuffer == NULL)
    {
        return -1;
    }

    if (platter < 0 || track < 0 || firstSector < 0 || sectors <= 0)
    {
        return -1;
    }

    if (isWrite != 0 && isWrite != 1)
    {
        return -1;
    }

    unit = get_disk_unit(deviceName);                                           // TEST04 ADD map device to unit
    if (unit < 0 || unit >= THREADS_MAX_DISKS)
    {
        return -1;                                                              // TEST04 ADD reject invalid disk unit
    }

    pid = k_getpid();                                                           // TEST04 ADD get requester pid
    _DiskRequest = &diskRequests[pid % MAXPROC];                                // TEST04 ADD select request slot

    _DiskRequest->pNext = NULL;                                                 // TEST04 ADD clear next link
    _DiskRequest->pid = pid;                                                    // TEST04 ADD record request pid
    strncpy(_DiskRequest->deviceName, deviceName, THREADS_MAX_DEVICE_NAME - 1); // TEST04 ADD copy device name
    _DiskRequest->deviceName[THREADS_MAX_DEVICE_NAME - 1] = '\0';               // TEST04 ADD kill device name
    _DiskRequest->dataBuffer = dataBuffer;                                      // TEST04 ADD buffer pointer
    _DiskRequest->platter = platter;                                            // TEST04 ADD platter
    _DiskRequest->track = track;                                                // TEST04 ADD track
    _DiskRequest->firstSector = firstSector;                                    // TEST04 ADD first sector
    _DiskRequest->sectorCount = sectors;                                        // TEST04 ADD sector cnt
    _DiskRequest->isWrite = isWrite;                                            // TEST04 ADD direction
    _DiskRequest->status = -1;                                                  // TEST04 ADD req status
    _DiskRequest->result = -1;                                                  // TEST04 ADD req result

    insert_disk_request(unit, _DiskRequest);                                    // TEST04 ADD enqueue req
    mailbox_send(diskRequestMbox[unit], NULL, 0, FALSE);                        // TEST04 ADD wake disk driver
    mailbox_receive(_DiskRequest->completeMbox, NULL, 0, TRUE);                 // TEST04 ADD wait for complete

    *status = _DiskRequest->status;                                             // TEST04 ADD return device status
    return _DiskRequest->result;                                                // TEST04 ADD return syscall result
}

// queue disk 
static void insert_disk_request(int unit, DiskRequest* _DiskRequest)
{
    DiskRequest* _Current;
    DiskRequest* _Previous;

    _DiskRequest->pNext = NULL;                                                 // TEST04 ADD clear next link

    if (pDiskQueueHead[unit] == NULL)
    {
        pDiskQueueHead[unit] = _DiskRequest;                                    // TEST04 ADD insert first request
        return;
    }

    if (DISK_ARM_ALG == DISK_ARM_ALG_FCFS)
    {
        _Current = pDiskQueueHead[unit];                                        // TEST04 ADD find queue tail
        while (_Current->pNext != NULL)
        {
            _Current = _Current->pNext;                                         // TEST04 ADD advance to tail
        }
        _Current->pNext = _DiskRequest;                                         // TEST04 ADD append request
        return;
    }

    _Previous = NULL;
    _Current = pDiskQueueHead[unit];
    while (_Current != NULL && _Current->track <= _DiskRequest->track)
    {
        _Previous = _Current;                                                   // TEST05 ADD move prev forward
        _Current = _Current->pNext;                                             // TEST05 ADD move cur forward
    }

    if (_Previous == NULL)
    {
        _DiskRequest->pNext = pDiskQueueHead[unit];                             // TEST05 ADD insert queue head
        pDiskQueueHead[unit] = _DiskRequest;                                    // TEST05 ADD update queue head
    }
    else
    {
        _DiskRequest->pNext = _Current;                                         // TEST05 ADD link new req
        _Previous->pNext = _DiskRequest;                                        // TEST05 ADD new req
    }
}

// pop disk 
static DiskRequest* pop_disk_request(int unit)
{
    DiskRequest* _DiskRequest;                                                  // TEST04 ADD selected req

    _DiskRequest = pDiskQueueHead[unit];                                        // TEST04 ADD grab queue head
    if (_DiskRequest != NULL)
    {
        pDiskQueueHead[unit] = _DiskRequest->pNext;                             // TEST04 ADD unlink queue head
        _DiskRequest->pNext = NULL;                                             // TEST04 ADD clear link
    }

    return _DiskRequest;
}

// dequeue fcfs
static DiskRequest* fcfs_request(int unit)
{
    return pop_disk_request(unit);
}

// dequeue sstf
static DiskRequest* sstf_request(int unit)
{
    DiskRequest* _Current;
    DiskRequest* _Previous;
    DiskRequest* _Best;
    DiskRequest* _BestPrevious;
    int bestDistance;

    _Current = pDiskQueueHead[unit];
    _Previous = NULL;
    _Best = NULL;
    _BestPrevious = NULL;
    bestDistance = 0x7fffffff;

    while (_Current != NULL)
    {
        int currentDistance = abs(_Current->track - diskCurrentTrack[unit]);    // TEST08 ADD seek distance

        if (_Best == NULL || currentDistance < bestDistance)
        {
            _Best = _Current;                                                   // TEST08 ADD store closer req
            _BestPrevious = _Previous;                                          // TEST08 ADD store prior node
            bestDistance = currentDistance;                                     // TEST08 ADD save distance
        }

        _Previous = _Current;                                                   // TEST08 ADD advance prev
        _Current = _Current->pNext;                                             // TEST08 ADD advance current
    }

    if (_Best == NULL)
    {
        return NULL;                                                            // TEST08 ADD empty queue
    }

    if (_BestPrevious == NULL)
    {
        pDiskQueueHead[unit] = _Best->pNext;                                    // TEST08 ADD remove head req
    }
    else
    {
        _BestPrevious->pNext = _Best->pNext;                                    // TEST08 ADD unlink req
    }

    _Best->pNext = NULL;                                                        // TEST08 ADD clear next ptr
    return _Best;                                                               // TEST08 ADD return selected req
}

// exec disk xfer
static int disk_transfer(DiskRequest* _DiskRequest)
{
    int unit;
    int sectorOffset;
    int currentPlatter;
    int currentTrack;
    int currentSector;
    int waitStatus;
    int controlResult;
    unsigned char* _BufferBytes;
    device_control_block_t _DevRequest;

    if (_DiskRequest == NULL)
    {
        return -1;
    }

    unit = get_disk_unit(_DiskRequest->deviceName);                             // TEST13 ADD map device to unit
    if (unit < 0 || unit >= THREADS_MAX_DISKS)
    {
        return -1;
    }

    if (_DiskRequest->platter < 0 || _DiskRequest->platter >= diskInfo[unit].platters)
    {
        return -1;
    }

    if (_DiskRequest->track < 0 || _DiskRequest->track >= diskInfo[unit].tracks)
    {
        return -1;
    }

    if (_DiskRequest->firstSector < 0 || _DiskRequest->firstSector >= diskInfo[unit].sectors)
    {
        return -1;
    }

    if (_DiskRequest->sectorCount <= 0)
    {
        return -1;
    }

    _BufferBytes = (unsigned char*)_DiskRequest->dataBuffer;                    // TEST13 ADD use bytes
    currentPlatter = _DiskRequest->platter;                                     // TEST13 ADD init platter 
    currentTrack = _DiskRequest->track;                                         // TEST13 ADD init track 
    currentSector = _DiskRequest->firstSector;                                  // TEST13 ADD init sector 

    for (sectorOffset = 0; sectorOffset < _DiskRequest->sectorCount; ++sectorOffset)
    {
        unsigned char* _UserSector;                                             // TEST13 ADD cur user sector

        if (currentPlatter >= diskInfo[unit].platters)
        {
            return -1;
        }

        if (currentTrack >= diskInfo[unit].tracks)
        {
            currentPlatter += 1;                                                // TEST13 ADD advance platter after track wrap
            currentTrack = 0;                                                   // TEST13 ADD reset track after platter wrap

            if (currentPlatter >= diskInfo[unit].platters)
            {
                return -1;                                                      // TEST13 ADD reject disk overflow
            }
        }

        _UserSector = _BufferBytes + (sectorOffset * THREADS_DISK_SECTOR_SIZE); // TEST13 ADD select user sector

        if (!diskTrackKnown[unit] || diskCurrentTrack[unit] != currentTrack)
        {
            memset(&_DevRequest, 0, sizeof(device_control_block_t));            // TEST13 ADD clear seek req
            _DevRequest.command = DISK_SEEK;                                    // TEST13 ADD disk seek
            _DevRequest.control1 = currentTrack;                                // TEST13 ADD seek track
            _DevRequest.control2 = 0;                                           // TEST13 ADD seek control
            _DevRequest.data_length = 0;                                        // TEST13 ADD 0 data

            controlResult = device_control(_DiskRequest->deviceName, _DevRequest);     // TEST13 ADD send seek req
            if (controlResult != 0)
            {
                return -1;
            }

            controlResult = wait_device(_DiskRequest->deviceName, &waitStatus); // TEST21 ALTER wait seek complete
            if (controlResult != 0)
            {
                return -1;
            }

            diskCurrentTrack[unit] = currentTrack;                              // TEST13 ADD save new arm pos
            diskTrackKnown[unit] = 1;                                           // TEST21 ADD set arm pos
        }

        memset(&_DevRequest, 0, sizeof(device_control_block_t));                // TEST13 ADD clear rw req
        _DevRequest.command = _DiskRequest->isWrite ? DISK_WRITE : DISK_READ;   // TEST13 ADD choose cmd
        _DevRequest.control1 = currentPlatter;                                  // TEST13 ADD select platter
        _DevRequest.control2 = currentSector;                                   // TEST13 ADD select sector
        _DevRequest.data_length = THREADS_DISK_SECTOR_SIZE;                     // TEST13 ADD xfer one sector

        _DevRequest.input_data = _UserSector;                                   // TEST06 ALTER attach sector buffer
        _DevRequest.output_data = _UserSector;                                  // TEST06 ALTER attach sector buffer

        if (_DiskRequest->isWrite)
        {
            _DevRequest.output_data = _UserSector;                              // TEST13 ADD set write buffer
        }
        else
        {
            _DevRequest.input_data = _UserSector;                               // TEST13 ADD set read buffer
        }

        controlResult = device_control(_DiskRequest->deviceName, _DevRequest);  // TEST13 ADD send read write req
        if (controlResult != 0)
        {
            return -1;
        }

        controlResult = wait_device(_DiskRequest->deviceName, &waitStatus);     // TEST21 ALTER wait for rw complete
        if (controlResult != 0)
        {
            return -1;
        }

        currentSector += 1;                                                     // TEST13 ADD move sector
        if (currentSector >= diskInfo[unit].sectors)
        {
            currentSector = 0;
            currentTrack += 1;                                                  // TEST13 ADD advance track
        }
    }

    return 0;                                                                   // TEST13 ADD xfer success
}

// select disk request method 
static DiskRequest* request_meth(int unit)
{
    if (DISK_ARM_ALG == DISK_ARM_ALG_FCFS)
    {
        return fcfs_request(unit);                                              // TEST08 ADD choose FCFS 
    }

    return sstf_request(unit);                                                  // TEST08 ADD choose SSTF 
}