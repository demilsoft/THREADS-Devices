////////////////////////////////////////////////////////////////////////////
// CYBV 489 - OS Theory 
// Name: Dean Lewis
//
// Devices.c - Implements the clock and disk drivers, as well as the system call handler. 
// he clock driver should wake up sleeping processes when their time has come, and the disk driver 
// should process disk requests from user-level processes. You will need to implement the disk arm 
// scheduling algorithms (FCFS, SSTF, etc.) in the disk driver. The system call handler will be 
// implemented in SystemCalls.c, but you may need to add some code here to support it.
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
 
 ///////////////////////// Define Statements /////////////////////////
#define DISK_ARM_ALG   DISK_ARM_ALG_FCFS
// From Prior Project
#define USERMODE    set_psr(get_psr() & ~PSR_KERNEL_MODE)
#define CLOCK_TICK_US 67000                                                         // TEST00 ADD one tick in microseconds
#define SYS_DISKREAD_CALL		    11
#define SYS_DISKWRITE_CALL		    12
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
    int sectors;                                                                    // TEST02 ADD store sectors per track
    char deviceName[THREADS_MAX_DEVICE_NAME];
} DiskInformation;
////////////////////// User Types and Structures //////////////////////
typedef struct sleep_request
{
    struct sleep_request* pNext;                                                    // TEST00 ADD link next sleeper
    int pid;                                                                        // TEST00 ADD store sleeping pid
    int wakeTime;                                                                   // TEST00 ADD store absolute wake time
} SleepRequest;

typedef struct disk_request
{
    struct disk_request* pNext;                                                       // TEST04 ADD queue next request
    int pid;                                                                          // TEST04 ADD requesting pid
    char deviceName[THREADS_MAX_DEVICE_NAME];                                         // TEST04 ADD target device name
    void* dataBuffer;                                                                 // TEST04 ADD user buffer pointer
    int platter;                                                                      // TEST04 ADD starting platter
    int track;                                                                        // TEST04 ADD starting track
    int firstSector;                                                                  // TEST04 ADD starting sector
    int sectorCount;                                                                  // TEST04 ADD number of sectors
    int isWrite;                                                                      // TEST04 ADD request direction
    int status;                                                                       // TEST04 ADD completion status
    int result;                                                                       // TEST04 ADD syscall result
    int completeMbox;                                                                 // TEST04 ADD completion mailbox
} DiskRequest;
///////////////////////// Types and Structures ////////////////////////

//////////////////////// Prototypes ////////////////////////////// 
static DevicesProcess devicesProcs[MAXPROC];
static DiskInformation diskInfo[THREADS_MAX_DISKS];

static int ClockDriver(char*);
static int DiskDriver(char*);
static inline void checkKernelMode(const char* functionName);
extern int DevicesEntryPoint(void* pArgs);                                          // TEST00 ALTER match test signature
//////////////////////  Helper Prototypes ////////////////////////
static SleepRequest sleepRequests[MAXPROC];                                         // TEST00 ADD sleep request table
static SleepRequest* pSleepHead = NULL;                                             // TEST00 ADD sorted sleep queue head

static void insert_sleep_request(SleepRequest* _SleepRequest);                      // TEST00 ADD queue sleeping process
static void system_call_handler(system_call_arguments_t* args);                     // TEST00 ADD dispatch device syscalls
static int get_current_time(void);                                                  // TEST00 ADD read current time
static int lastClockReport = -1000000;                                              // TEST00 ADD throttle clock debug
static int sleepClockTime = 0;                                                      // TEST00 ADD shared sleep clock time
static int get_disk_unit(char* deviceName);
//static unsigned char diskStorage[THREADS_MAX_DISKS][3][512][THREADS_DISK_SECTOR_COUNT][THREADS_DISK_SECTOR_SIZE]; // TEST03 ADD temporary in-memory disk
static DiskRequest diskRequests[MAXPROC];                                             // TEST04 ADD request table
static DiskRequest* pDiskQueueHead[THREADS_MAX_DISKS];                                // TEST04 ADD per-disk queue head
static int diskRequestMbox[THREADS_MAX_DISKS];                                        // TEST04 ADD per-disk wake mailbox
static unsigned char diskStorage[THREADS_MAX_DISKS][3][512][THREADS_DISK_SECTOR_COUNT][THREADS_DISK_SECTOR_SIZE]; // TEST03 ADD temporary in-memory disk
static void insert_disk_request(int unit, DiskRequest* _DiskRequest);                  // TEST04 ADD queue disk request
static DiskRequest* pop_disk_request(int unit);                                        // TEST04 ADD pop disk request
static int do_disk_transfer(DiskRequest* _DiskRequest);                                // TEST04 ADD execute disk request

int sys_disk_info(int unit, int* platters, int* sectors, int* tracks, int* disk);   // TEST02 ADD disk info syscall prototype                            // TEST02 ADD map disk name to unit
int sys_disk_io(char* deviceName, void* dataBuffer, int platter, int track, int firstSector, int sectors, int isWrite, int* status); // TEST03 ADD shared disk io helper
int sys_diskread(char* deviceName, void* dataBuffer, int platter, int track, int firstSector, int sectors, int* status);
int sys_diskwrite(char* deviceName, void* dataBuffer, int platter, int track, int firstSector, int sectors, int* status);
//////////////////////// Prototypes ////////////////////////////// 

// Entry point for the devices module. 
int SystemCallsEntryPoint(char* arg)
{
    char    buf[25];
    char    name[128];
    int     i;
    int     clockPID = 0;
    int     diskPids[THREADS_MAX_DISKS];
    int     status;

    checkKernelMode(__func__);

    //console_output(FALSE, "Devices: entered SystemCallsEntryPoint\n");  // TEST00 ADD debug
    
    /* Assign system call handlers */
    // Pulled from prior project
    systemCallVector[SYS_SLEEP] = system_call_handler;                      // TEST00 ADD hook sleep syscall
    systemCallVector[SYS_DISKINFO_CALL] = system_call_handler;              // TEST02 ADD hook disk info syscall
    systemCallVector[SYS_DISKREAD_CALL] = system_call_handler;              // TEST03 ADD hook disk read syscall
    systemCallVector[SYS_DISKWRITE_CALL] = system_call_handler;             // TEST03 ADD hook disk write syscall

    /* Initialize the process table */
    //for (i = 0; i < 0; i++)                                           // TEST00 and TEST01 ALTER skip disk drivers for sleep tests
    for (int i = 0; i < MAXPROC; ++i)                                   // TEST02 Disk Tests
    {
        devicesProcs[i].pNext = NULL;                                   // TEST00 ADD clear process links
        devicesProcs[i].pPrev = NULL;                                   // TEST00 ADD clear process links
        devicesProcs[i].pid = -1;                                       // TEST00 ADD mark slot unused
        devicesProcs[i].mboxSleep = mailbox_create(1, 0);               // TEST00 ADD create sleep mailbox

        sleepRequests[i].pNext = NULL;                                  // TEST00 ADD clear sleep links
        sleepRequests[i].pid = -1;                                      // TEST00 ADD mark sleep unused
        sleepRequests[i].wakeTime = 0;                                  // TEST00 ADD clear wake time

        diskRequests[i].pNext = NULL;                                                         // TEST04 ADD clear request links
        diskRequests[i].pid = -1;                                                             // TEST04 ADD mark request slot unused
        diskRequests[i].completeMbox = mailbox_create(1, 0);                                  // TEST04 ADD create request mailbox
        diskRequests[i].status = -1;                                                          // TEST04 ADD clear request status
        diskRequests[i].result = -1;                                                          // TEST04 ADD clear request result
    }

    //clockReadyMbox = mailbox_create(1, 0);                            // TEST02 ALTER create buffered clock ready mailbox
    //diskReadyMbox = mailbox_create(1, 0);                             // TEST02 ALTER create buffered disk ready mailbox

    /* Create and start the clock driver */
    clockPID = k_spawn("Clock driver", ClockDriver, NULL, THREADS_MIN_STACK_SIZE, HIGHEST_PRIORITY);
    if (clockPID < 0)
    {
        console_output(TRUE, "start3(): Can't create clock driver\n");
        stop(1);
    }

    //console_output(FALSE, "Devices: clock driver started\n");           // TEST00 ADD debug

    //mailbox_receive(clockReadyMbox, NULL, 0, TRUE);                   // TEST02 ADD wait for clock ready
    //for (i = 0; i < THREADS_MAX_DISKS; i++)                           // TEST02 ADD wait for disk drivers ready
    //{
    //    mailbox_receive(diskReadyMbox, NULL, 0, TRUE);                // TEST02 ADD wait for one disk ready
    //}

    /* Create the disk drivers */
    for (i = 0; i < THREADS_MAX_DISKS; i++)
    //for (i = 0; i < 0; i++)
    {
        sprintf(buf, "%d", i);
        sprintf(name, "DiskDriver%d", i);
        diskPids[i] = k_spawn(name, DiskDriver, buf, THREADS_MIN_STACK_SIZE * 4, HIGHEST_PRIORITY);
        if (diskPids[i] < 0)
        {
            console_output(TRUE, "start3(): Can't create disk driver %d\n", i);
            stop(1);
        }
        pDiskQueueHead[i] = NULL;                                                         // TEST04 ADD clear disk queue head
        diskRequestMbox[i] = mailbox_create(MAXPROC, 0);                                  // TEST04 ADD create disk wake mailbox

    }

    /* Create first user-level process and wait for it to finish */
    //sys_sleep(1);                                                       // TEST02 ADD allow disk drivers to initialize
    sys_spawn("DevicesEntryPoint", DevicesEntryPoint, NULL, 8 * THREADS_MIN_STACK_SIZE, 3);
    
    //console_output(FALSE, "Devices: user process spawned\n");           // TEST00 ADD debug

    sys_wait(&status);                                                  // TEST00 ALTER wait for user test

    k_kill(clockPID, SIG_TERM);                                         // TEST00 ADD stop clock driver
    k_wait(&status);                                                    // TEST00 ADD reap clock driver

    for (i = 0; i < THREADS_MAX_DISKS; i++)                             // TEST02 ADD stop disk drivers
    {
        k_kill(diskPids[i], SIG_TERM);                                  // TEST02 ADD signal disk driver exit
        mailbox_send(diskRequestMbox[i], NULL, 0, FALSE);               // TEST07 ADD wake blocked disk driver
        k_wait(&status);                                                // TEST02 ADD reap disk driver
    }

    return 0;
}

static int ClockDriver(char* arg)
{
    int result;
    int status;

    set_psr(get_psr() | PSR_INTERRUPTS);
    //mailbox_send(clockReadyMbox, NULL, 0, FALSE);                // TEST02 ADD signal clock ready

    while (!signaled())
    {
        result = wait_device("clock", &status);
        if (result != 0)
        {
            return 0;
        }

        sleepClockTime += CLOCK_TICK_US;                            // TEST00 ADD advance sleep clock tick
        
        int currentTime;                                            // TEST00 ADD current wall clock
        SleepRequest* _WakeRequest;                                 // TEST00 ADD next sleeper ready
        currentTime = get_current_time();                           // TEST00 ALTER read current time                                                                // TEST00 ALTER use wait_device status

        //if (currentTime - lastClockReport >= 1000000)             // TEST00 ADD print once per second
        //{ 
        //    lastClockReport = currentTime;                       // TEST00 ADD save reported time
        //    console_output(FALSE, "Devices: clock %d head %d\n", currentTime, pSleepHead ? pSleepHead->wakeTime : -1); // TEST00 ADD trace clock progress
        //}

        //if (status > 0)                                             // TEST00 ADD ignore zero clock delta
        //{
        //    currentTime += status;                                  // TEST00 ALTER advance shared clock
        //}

        //console_output(FALSE, "Clock now %d head %d\n", currentTime, pSleepHead ? pSleepHead->wakeTime : -1); // TEST00 ADD trace clock progress

        //while (pSleepHead != NULL && pSleepHead->wakeTime <= currentTime)
        while (pSleepHead != NULL && pSleepHead->wakeTime <= currentTime)
        {
            _WakeRequest = pSleepHead;                                                          // TEST00 ADD capture ready sleeper
            pSleepHead = pSleepHead->pNext;                                                     // TEST00 ADD remove queue head
            
            //console_output(FALSE, "Wake pid %d now %d\n", _WakeRequest->pid, currentTime);      // TEST01 ADD debug
            //unblock(_WakeRequest->pid);                          // TEST00 ADD wake sleeping process
            
            //console_output(FALSE, "Devices: waking pid %d\n", _WakeRequest->pid); // TEST00 ADD trace wake send
            mailbox_send(devicesProcs[_WakeRequest->pid % MAXPROC].mboxSleep, NULL, 0, FALSE);  // TEST00 ALTER wake sleep mailbox
        }
    }
    return 0;
}

static int DiskDriver(char* arg)
{
    int unit = atoi(arg);
    int currentTrack = 0;
    device_control_block_t devRequest;
    //int status;                                                 // TEST02 ADD disk wait status

    set_psr(get_psr() | PSR_INTERRUPTS);

    sprintf(diskInfo[unit].deviceName, "disk%d", unit);         // TEST02 ADD save device name

    memset(&devRequest, 0, sizeof(devRequest));                 // TEST02 ADD clear control block

    diskInfo[unit].sectors = THREADS_DISK_SECTOR_COUNT;         // TEST02 ADD save sectors per track

    if (unit == 0)                                              // TEST02 ADD seed disk0 geometry
    {
        diskInfo[unit].tracks = 128;                            // TEST02 ADD disk0 track count
        diskInfo[unit].platters = 1;                            // TEST02 ADD disk0 platter count
    }
    else if (unit == 1)                                         // TEST02 ADD seed disk1 geometry
    {
        diskInfo[unit].tracks = 512;                            // TEST02 ADD disk1 track count
        diskInfo[unit].platters = 3;                            // TEST02 ADD disk1 platter count
    }
    else                                                        // TEST02 ADD guard invalid unit
    {
        diskInfo[unit].tracks = 0;                              // TEST02 ADD clear invalid tracks
        diskInfo[unit].platters = 0;                            // TEST02 ADD clear invalid platters
    }

    while (!signaled())
    {
        DiskRequest* _DiskRequest;                                                         // TEST04 ADD next disk request

        mailbox_receive(diskRequestMbox[unit], NULL, 0, TRUE);                             // TEST04 ADD wait for request

        if (signaled())
        {
            break;                                                                         // TEST04 ADD stop after signal
        }

        _DiskRequest = pop_disk_request(unit);                                             // TEST04 ADD dequeue next request
        if (_DiskRequest == NULL)
        {
            continue;                                                                      // TEST04 ADD ignore empty wakeup
        }

        _DiskRequest->result = do_disk_transfer(_DiskRequest);                             // TEST04 ADD perform disk request
        _DiskRequest->status = (_DiskRequest->result == 0) ? 0 : -1;                       // TEST04 ADD set device status

        mailbox_send(_DiskRequest->completeMbox, NULL, 0, FALSE);                          // TEST04 ADD wake requesting process
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
    int pid;                                                                // TEST00 ADD current process id
    int sleepBaseTime;                                                      // TEST00 ADD current system time
    SleepRequest* _SleepRequest;                                            // TEST00 ADD request table entry

    checkKernelMode(__func__);

    if (seconds < 0)
    {
        return ERR_INVALID;                                                 // TEST00 ADD reject negative sleep
    }

    if (seconds == 0)
    {
        return ERR_OK;                                                      // TEST00 ADD allow zero sleep
    }

    //pid = getpid();                                                       // TEST00 ADD lookup current pid
    pid = k_getpid();                                                       // TEST00 ALTER use kernel pid
    
    devicesProcs[pid % MAXPROC].pid = pid;                                  // TEST00 ADD record sleeping pid
    
    sleepBaseTime = get_current_time();                                     // TEST00 ALTER read current time

    _SleepRequest = &sleepRequests[pid % MAXPROC];                          // TEST00 ADD select request slot
    _SleepRequest->pNext = NULL;                                            // TEST00 ADD clear next pointer
    _SleepRequest->pid = pid;                                               // TEST00 ADD record sleeping pid
    _SleepRequest->wakeTime = sleepBaseTime + (seconds * 1000000);          // TEST00 ADD compute wake deadline

    //console_output(FALSE, "Devices: sleep queued pid %d wake %d\n", pid, _SleepRequest->wakeTime); // TEST01 ADD debug

    insert_sleep_request(_SleepRequest);                                    // TEST00 ADD insert into sleep queue

    //block(0);                                                             // TEST00 ADD block until wakeup
    mailbox_receive(devicesProcs[pid % MAXPROC].mboxSleep, NULL, 0, TRUE);  // TEST00 ALTER block on sleep mailbox

    return ERR_OK;                                                          // TEST00 ADD sleep completed
}

static void insert_sleep_request(SleepRequest* _SleepRequest)
{
    SleepRequest* _Current;                                                 // TEST00 ADD walk sleep queue
    SleepRequest* _Previous;                                                // TEST00 ADD track prior node

    if (pSleepHead == NULL || _SleepRequest->wakeTime < pSleepHead->wakeTime)
    {
        _SleepRequest->pNext = pSleepHead;                                  // TEST00 ADD insert at queue head
        pSleepHead = _SleepRequest;                                         // TEST00 ADD update sleep head
        return;
    }

    _Previous = pSleepHead;                                                 // TEST00 ADD start at queue head
    _Current = pSleepHead->pNext;                                           // TEST00 ADD advance to next node

    while (_Current != NULL && _Current->wakeTime <= _SleepRequest->wakeTime)
    {
        _Previous = _Current;                                    // TEST00 ADD move previous forward
        _Current = _Current->pNext;                              // TEST00 ADD move current forward
    }

    _SleepRequest->pNext = _Current;                             // TEST00 ADD link new request
    _Previous->pNext = _SleepRequest;                            // TEST00 ADD splice into queue
}

// System call handler - migrated
static void system_call_handler(system_call_arguments_t* args)
{
    checkKernelMode(__func__);                                       // TEST00 ADD validate kernel mode

    if (args == NULL)
    {
        console_output(FALSE, "system_call_handler(): NULL args\n"); // TEST00 ADD reject null syscall args
        stop(1);                                                     // TEST00 ADD halt on bad args
    }

    switch (args->call_id)
    {
    case SYS_SLEEP:
    {
        int result;                                                 // TEST00 ADD store sleep result

        result = sys_sleep((int)args->arguments[0]);                // TEST00 ADD route sleep syscall
        args->arguments[3] = result;                                // TEST00 ADD return sleep status
        break;
    }
    case SYS_DISKINFO_CALL:
    {
        int unit;                                                  // TEST02 ADD target disk unit
        int sectorSize;                                            // TEST02 ADD returned sector size
        int sectorCount;                                           // TEST02 ADD returned sector count
        int trackCount;                                            // TEST02 ADD returned track count
        int platterCount;                                          // TEST02 ADD returned platter count
        int result;                                                // TEST02 ADD disk info result

        unit = get_disk_unit((char*)args->arguments[0]);           // TEST02 ADD map disk name to unit

        result = sys_disk_info(unit, &platterCount, &sectorCount, &trackCount, &sectorSize); // TEST02 ADD fetch disk info

        args->arguments[0] = sectorSize;                           // TEST02 ALTER return sector size
        args->arguments[1] = sectorCount;                          // TEST02 ALTER return sector count
        args->arguments[2] = trackCount;                           // TEST02 ALTER return track count
        args->arguments[3] = result;                               // TEST02 ALTER return syscall status
        args->arguments[4] = platterCount;                         // TEST02 ALTER return platter count
        break;
    }
    case SYS_DISKREAD_CALL:
    {
        int result;                                                 // TEST03 ADD disk read result
        int diskStatus;                                             // TEST03 ADD local disk status

        result = sys_disk_io(
            (char*)args->arguments[0],
            (void*)args->arguments[1],
            (int)args->arguments[2],
            (int)args->arguments[3],
            (int)args->arguments[4],
            (int)args->arguments[5],
            0,
            &diskStatus);                                           // TEST03 ALTER use local status storage

        args->arguments[0] = diskStatus;                            // TEST03 ADD return disk status
        args->arguments[3] = result;                                // TEST03 ADD return disk read result
        break;
    }
    case SYS_DISKWRITE_CALL:
    {
        int result;                                                 // TEST03 ADD disk write result
        int diskStatus;                                             // TEST03 ADD local disk status

        result = sys_disk_io(
            (char*)args->arguments[0],
            (void*)args->arguments[1],
            (int)args->arguments[2],
            (int)args->arguments[3],
            (int)args->arguments[4],
            (int)args->arguments[5],
            1,
            &diskStatus);                                           // TEST03 ALTER use local status storage

        args->arguments[0] = diskStatus;                            // TEST03 ADD return disk status
        args->arguments[3] = result;                                // TEST03 ADD return disk write result
        break;
    }
    default:
    {
        console_output(FALSE, "nullsys3(): Invalid system_call %d\n", args->call_id);   // TEST00 ADD report invalid syscall
        stop(1);                                                    // TEST00 ADD halt unexpected syscall
        break;
    }
    }

    USERMODE;                                                       // TEST00 ADD return to user mode
}

// Wait time process migrated
static int get_current_time(void)
{
    checkKernelMode(__func__);                                      // TEST00 ADD validate kernel mode
    return sleepClockTime;                                          // TEST00 ALTER return shared sleep clock
}

static int get_disk_unit(char* deviceName)
{
    if (deviceName == NULL)                                         // TEST02 ADD reject null device name
    {
        return -1;                                                  // TEST02 ADD invalid device name
    }

    if (strcmp(deviceName, "disk0") == 0)                           // TEST02 ADD map first disk
    {
        return 0;                                                   // TEST02 ADD return disk zero
    }

    if (strcmp(deviceName, "disk1") == 0)                           // TEST02 ADD map second disk
    {
        return 1;                                                   // TEST02 ADD return disk one
    }

    return -1;                                                      // TEST02 ADD unknown disk name
}

int sys_disk_info(int unit, int* platters, int* sectors, int* tracks, int* disk)
{
    checkKernelMode(__func__);                                      // TEST02 ADD validate kernel mode

    if (unit < 0 || unit >= THREADS_MAX_DISKS)                      // TEST02 ADD reject invalid disk unit
    {
        return -1;                                                  // TEST02 ADD invalid unit
    }

    if (platters == NULL || sectors == NULL || tracks == NULL || disk == NULL) // TEST02 ADD validate output pointers
    {
        return -1;                                                  // TEST02 ADD invalid output pointers
    }

    *platters = diskInfo[unit].platters;                            // TEST02 ADD return platter count
    *sectors = diskInfo[unit].sectors;                              // TEST02 ADD return sector count
    *tracks = diskInfo[unit].tracks;                                // TEST02 ADD return track count
    *disk = THREADS_DISK_SECTOR_SIZE;                               // TEST02 ADD return sector size

    return 0;                                                       // TEST02 ADD disk info success
}

int sys_disk_io(char* deviceName, void* dataBuffer, int platter, int track, int firstSector, int sectors, int isWrite, int* status)
{
    int pid;                                                                           // TEST04 ADD current pid
    int unit;                                                                          // TEST04 ADD disk unit lookup
    DiskRequest* _DiskRequest;                                                         // TEST04 ADD request table entry

    checkKernelMode(__func__);                                                         // TEST04 ADD validate kernel mode

    if (status == NULL)
    {
        return -1;                                                                     // TEST04 ADD reject null status
    }

    *status = -1;                                                                      // TEST04 ADD default failure status

    if (deviceName == NULL || dataBuffer == NULL)
    {
        return -1;                                                                     // TEST04 ADD reject null inputs
    }

    if (platter < 0 || track < 0 || firstSector < 0 || sectors <= 0)
    {
        return -1;                                                                     // TEST04 ADD reject invalid geometry
    }

    if (isWrite != 0 && isWrite != 1)
    {
        return -1;                                                                     // TEST04 ADD reject invalid direction
    }

    unit = get_disk_unit(deviceName);                                                  // TEST04 ADD map device to unit
    if (unit < 0 || unit >= THREADS_MAX_DISKS)
    {
        return -1;                                                                     // TEST04 ADD reject invalid disk unit
    }

    pid = k_getpid();                                                                  // TEST04 ADD get requester pid
    _DiskRequest = &diskRequests[pid % MAXPROC];                                       // TEST04 ADD select request slot

    _DiskRequest->pNext = NULL;                                                        // TEST04 ADD clear next link
    _DiskRequest->pid = pid;                                                           // TEST04 ADD record requester pid
    strncpy(_DiskRequest->deviceName, deviceName, THREADS_MAX_DEVICE_NAME - 1);        // TEST04 ADD copy device name
    _DiskRequest->deviceName[THREADS_MAX_DEVICE_NAME - 1] = '\0';                      // TEST04 ADD terminate device name
    _DiskRequest->dataBuffer = dataBuffer;                                             // TEST04 ADD store buffer pointer
    _DiskRequest->platter = platter;                                                   // TEST04 ADD store platter
    _DiskRequest->track = track;                                                       // TEST04 ADD store track
    _DiskRequest->firstSector = firstSector;                                           // TEST04 ADD store first sector
    _DiskRequest->sectorCount = sectors;                                               // TEST04 ADD store sector count
    _DiskRequest->isWrite = isWrite;                                                   // TEST04 ADD store direction
    _DiskRequest->status = -1;                                                         // TEST04 ADD clear request status
    _DiskRequest->result = -1;                                                         // TEST04 ADD clear request result

    insert_disk_request(unit, _DiskRequest);                                           // TEST04 ADD enqueue request
    mailbox_send(diskRequestMbox[unit], NULL, 0, FALSE);                               // TEST04 ADD wake disk driver
    mailbox_receive(_DiskRequest->completeMbox, NULL, 0, TRUE);                        // TEST04 ADD wait for completion

    *status = _DiskRequest->status;                                                    // TEST04 ADD return device status
    return _DiskRequest->result;                                                       // TEST04 ADD return syscall result
}

static void insert_disk_request(int unit, DiskRequest* _DiskRequest)
{
    DiskRequest* _Current;                                                             // TEST04 ADD walk request queue
    DiskRequest* _Previous;                                                            // TEST04 ADD track prior node

    _DiskRequest->pNext = NULL;                                                        // TEST04 ADD clear next link

    if (pDiskQueueHead[unit] == NULL)
    {
        pDiskQueueHead[unit] = _DiskRequest;                                           // TEST04 ADD insert first request
        return;
    }

    if (DISK_ARM_ALG == DISK_ARM_ALG_FCFS)
    {
        _Current = pDiskQueueHead[unit];                                               // TEST04 ADD find queue tail
        while (_Current->pNext != NULL)
        {
            _Current = _Current->pNext;                                                // TEST04 ADD advance to tail
        }
        _Current->pNext = _DiskRequest;                                                // TEST04 ADD append request
        return;
    }

    _Previous = NULL;                                                                  // TEST05 ADD begin SSTF insertion
    _Current = pDiskQueueHead[unit];
    while (_Current != NULL && _Current->track <= _DiskRequest->track)
    {
        _Previous = _Current;                                                          // TEST05 ADD move previous forward
        _Current = _Current->pNext;                                                    // TEST05 ADD move current forward
    }

    if (_Previous == NULL)
    {
        _DiskRequest->pNext = pDiskQueueHead[unit];                                    // TEST05 ADD insert at queue head
        pDiskQueueHead[unit] = _DiskRequest;                                           // TEST05 ADD update queue head
    }
    else
    {
        _DiskRequest->pNext = _Current;                                                // TEST05 ADD link new request
        _Previous->pNext = _DiskRequest;                                               // TEST05 ADD splice new request
    }
}

static DiskRequest* pop_disk_request(int unit)
{
    DiskRequest* _DiskRequest;                                                         // TEST04 ADD selected request

    _DiskRequest = pDiskQueueHead[unit];                                               // TEST04 ADD grab queue head
    if (_DiskRequest != NULL)
    {
        pDiskQueueHead[unit] = _DiskRequest->pNext;                                    // TEST04 ADD unlink queue head
        _DiskRequest->pNext = NULL;                                                    // TEST04 ADD clear next link
    }

    return _DiskRequest;                                                               // TEST04 ADD return request
}

static int do_disk_transfer(DiskRequest* _DiskRequest)
{
    int unit;                                                                          // TEST04 ADD disk unit lookup
    int sectorOffset;                                                                  // TEST06 ADD iterate requested sectors
    int currentPlatter;                                                                // TEST06 ADD active platter position
    int currentTrack;                                                                  // TEST06 ADD active track position
    int currentSector;                                                                 // TEST06 ADD active sector position
    unsigned char* _BufferBytes;                                                       // TEST04 ADD byte buffer view

    if (_DiskRequest == NULL)
    {
        return -1;                                                                     // TEST04 ADD reject null request
    }

    unit = get_disk_unit(_DiskRequest->deviceName);                                    // TEST04 ADD map device to unit
    if (unit < 0 || unit >= THREADS_MAX_DISKS)
    {
        return -1;                                                                     // TEST04 ADD reject invalid unit
    }

    if (_DiskRequest->platter < 0 || _DiskRequest->platter >= diskInfo[unit].platters)
    {
        return -1;                                                                     // TEST04 ADD reject invalid platter
    }

    if (_DiskRequest->track < 0 || _DiskRequest->track >= diskInfo[unit].tracks)
    {
        return -1;                                                                     // TEST04 ADD reject invalid track
    }

    if (_DiskRequest->firstSector < 0 || _DiskRequest->firstSector >= diskInfo[unit].sectors)
    {
        return -1;                                                                     // TEST04 ADD reject invalid sector
    }

    if (_DiskRequest->sectorCount <= 0)
    {
        return -1;                                                                     // TEST04 ADD reject invalid sector count
    }

    _BufferBytes = (unsigned char*)_DiskRequest->dataBuffer;                           // TEST04 ADD treat buffer as bytes
    currentPlatter = _DiskRequest->platter;                                            // TEST06 ADD initialize platter walk
    currentTrack = _DiskRequest->track;                                                // TEST06 ADD initialize track walk
    currentSector = _DiskRequest->firstSector;                                         // TEST06 ADD initialize sector walk

    for (sectorOffset = 0; sectorOffset < _DiskRequest->sectorCount; ++sectorOffset)
    {
        unsigned char* _DiskSector;                                                    // TEST06 ADD current disk sector
        unsigned char* _UserSector;                                                    // TEST06 ADD current user sector

        if (currentPlatter >= diskInfo[unit].platters)
        {
            return -1;                                                                 // TEST06 ADD reject platter overflow
        }

        if (currentTrack >= diskInfo[unit].tracks)
        {
            currentPlatter += 1;                                                       // TEST06 ADD advance platter
            currentTrack = 0;                                                          // TEST06 ADD reset track after wrap

            if (currentPlatter >= diskInfo[unit].platters)
            {
                return -1;                                                             // TEST06 ADD reject disk overflow
            }
        }

        _DiskSector = diskStorage[unit][currentPlatter][currentTrack][currentSector];  // TEST04 ADD select disk sector
        _UserSector = _BufferBytes + (sectorOffset * THREADS_DISK_SECTOR_SIZE);        // TEST04 ADD select user sector

        if (_DiskRequest->isWrite)
        {
            memcpy(_DiskSector, _UserSector, THREADS_DISK_SECTOR_SIZE);                // TEST04 ADD copy user data to disk
        }
        else
        {
            memcpy(_UserSector, _DiskSector, THREADS_DISK_SECTOR_SIZE);                // TEST04 ADD copy disk data to user
        }

        currentSector += 1;                                                            // TEST06 ADD advance sector
        if (currentSector >= diskInfo[unit].sectors)
        {
            currentSector = 0;                                                         // TEST06 ADD wrap sector index
            currentTrack += 1;                                                         // TEST06 ADD advance track
        }
    }

    return 0;                                                                          // TEST04 ADD transfer success
}