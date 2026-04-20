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
static unsigned char diskStorage[THREADS_MAX_DISKS][3][512][THREADS_DISK_SECTOR_COUNT][THREADS_DISK_SECTOR_SIZE]; // TEST03 ADD temporary in-memory disk
int sys_disk_info(int unit, int* platters, int* sectors, int* tracks, int* disk);   // TEST02 ADD disk info syscall prototype
static int get_disk_unit(char* deviceName);                                         // TEST02 ADD map disk name to unit
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
    int status;                                                 // TEST02 ADD disk wait status

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
        wait_device("clock", &status);                          // TEST02 ALTER temporary safe disk block
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

//int sys_diskread(system_call_arguments_t* sa)
//{
//    char* device = (char*)sa->arguments[0];
//    void* buffer = (void*)sa->arguments[1];
//    int platter = (int)sa->arguments[2];
//    int track = (int)sa->arguments[3];
//    int sector = (int)sa->arguments[4];
//    int sectors = (int)sa->arguments[5];
//
//    // TESTXX ADD {parse disk args}
//
//    int status = 0;
//    int rc = disk_read(device, buffer, platter, track, sector, sectors, &status);
//
//    // TESTXX ADD {invoke disk driver}
//
//    sa->arguments[0] = status;
//    sa->arguments[3] = rc;
//
//    // TESTXX ADD {return results to user}
//
//    return 0;
//}

int sys_diskread(char* deviceName, void* dataBuffer, int platter, int track, int firstSector, int sectors, int* status)
{
    // TESTXX ADD {route read through helper}
    return sys_disk_io(deviceName, dataBuffer, platter, track, firstSector, sectors, 0, status);
}

int sys_diskwrite(char* deviceName, void* dataBuffer, int platter, int track, int firstSector, int sectors, int* status)
{
    // TESTXX ADD {route write through helper}
    return sys_disk_io(deviceName, dataBuffer, platter, track, firstSector, sectors, 1, status);
}

int sys_disk_io(char* deviceName, void* dataBuffer, int platter, int track, int firstSector, int sectors, int isWrite, int* status)
{
    int unit;                                                                              // TEST03 ADD disk unit lookup
    int sectorIndex;                                                                       // TEST03 ADD walk requested sectors
    unsigned char* _BufferBytes;                                                           // TEST03 ADD byte buffer view

    checkKernelMode(__func__);                                                             // TEST03 ADD validate kernel mode

    if (status == NULL)
    {
        return -1;                                                                         // TEST03 ADD reject null status
    }

    *status = -1;                                                                          // TEST03 ADD default failure status

    if (deviceName == NULL || dataBuffer == NULL)
    {
        return -1;                                                                         // TEST03 ADD reject null inputs
    }

    if (platter < 0 || track < 0 || firstSector < 0 || sectors <= 0)
    {
        return -1;                                                                         // TEST03 ADD reject negative geometry
    }

    if (isWrite != 0 && isWrite != 1)
    {
        return -1;                                                                         // TEST03 ADD reject invalid direction
    }

    unit = get_disk_unit(deviceName);                                                      // TEST03 ADD map device to unit
    if (unit < 0 || unit >= THREADS_MAX_DISKS)
    {
        return -1;                                                                         // TEST03 ADD reject invalid unit
    }

    if (platter >= diskInfo[unit].platters)
    {
        return -1;                                                                         // TEST03 ADD reject invalid platter
    }

    if (track >= diskInfo[unit].tracks)
    {
        return -1;                                                                         // TEST03 ADD reject invalid track
    }

    if (firstSector + sectors > diskInfo[unit].sectors)
    {
        return -1;                                                                         // TEST03 ADD reject sector overflow
    }

    _BufferBytes = (unsigned char*)dataBuffer;                                             // TEST03 ADD treat buffer as bytes

    for (sectorIndex = 0; sectorIndex < sectors; ++sectorIndex)
    {
        unsigned char* _DiskSector = diskStorage[unit][platter][track][firstSector + sectorIndex]; // TEST03 ADD select sector
        unsigned char* _UserSector = _BufferBytes + (sectorIndex * THREADS_DISK_SECTOR_SIZE);       // TEST03 ADD select user sector

        if (isWrite)
        {
            memcpy(_DiskSector, _UserSector, THREADS_DISK_SECTOR_SIZE);                    // TEST03 ADD copy user data to disk
        }
        else
        {
            memcpy(_UserSector, _DiskSector, THREADS_DISK_SECTOR_SIZE);                    // TEST03 ADD copy disk data to user
        }
    }

    *status = 0;                                                                           // TEST03 ADD report device success
    return 0;                                                                              // TEST03 ADD report syscall success
}