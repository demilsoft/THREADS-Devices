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
#define DISK_ARM_ALG   DISK_ARM_ALG_FCFS

 ///////////////////////// Types and Structures ////////////////////////
typedef struct devices_proc
{
    struct devices_proc* pNext;
    struct devices_proc* pPrev;
    int pid;
} DevicesProcess;

typedef struct
{
    int tracks;
    int platters;
    char deviceName[THREADS_MAX_DEVICE_NAME];
} DiskInformation;
////////////////////// User Types and Structures //////////////////////
typedef struct sleep_request
{
    struct sleep_request* pNext;                 // TEST00 ADD link next sleeper
    int pid;                                     // TEST00 ADD store sleeping pid
    int wakeTime;                                // TEST00 ADD store absolute wake time
} SleepRequest;
///////////////////////// Types and Structures ////////////////////////

//////////////////////// Prototypes ////////////////////////////// 
static DevicesProcess devicesProcs[MAXPROC];
static DiskInformation diskInfo[THREADS_MAX_DISKS];

static int ClockDriver(char*);
static int DiskDriver(char*);
static inline void checkKernelMode(const char* functionName);
extern int DevicesEntryPoint(void* pArgs);                          // TEST00 ALTER match test signature
//////////////////////  Helper Prototypes ////////////////////////
static SleepRequest sleepRequests[MAXPROC];                         // TEST00 ADD sleep request table
static SleepRequest* pSleepHead = NULL;                             // TEST00 ADD sorted sleep queue head

static void insert_sleep_request(SleepRequest* _SleepRequest);      // TEST00 ADD queue sleeping process
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

    console_output(FALSE, "Devices: entered SystemCallsEntryPoint\n"); // TEST00 ADD debug

    /* Assign system call handlers */

    /* Initialize the process table */
    for (int i = 0; i < MAXPROC; ++i)
    {
        devicesProcs[i].pNext = NULL;                // TEST00 ADD clear process links
        devicesProcs[i].pPrev = NULL;                // TEST00 ADD clear process links
        devicesProcs[i].pid = -1;                    // TEST00 ADD mark slot unused

        sleepRequests[i].pNext = NULL;               // TEST00 ADD clear sleep links
        sleepRequests[i].pid = -1;                   // TEST00 ADD mark sleep unused
        sleepRequests[i].wakeTime = 0;               // TEST00 ADD clear wake time
    }

    /* Create and start the clock driver */
    clockPID = k_spawn("Clock driver", ClockDriver, NULL, THREADS_MIN_STACK_SIZE, HIGHEST_PRIORITY);
    if (clockPID < 0)
    {
        console_output(TRUE, "start3(): Can't create clock driver\n");
        stop(1);
    }

    console_output(FALSE, "Devices: clock driver started\n");          // TEST00 ADD debug

    /* Create the disk drivers */
    for (i = 0; i < THREADS_MAX_DISKS; i++)
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
    sys_spawn("DevicesEntryPoint", DevicesEntryPoint, NULL, 8 * THREADS_MIN_STACK_SIZE, 3);
    
    console_output(FALSE, "Devices: user process spawned\n");          // TEST00 ADD debug

    sys_wait(&status);

    return 0;
}


static int ClockDriver(char* arg)
{
    int result;
    int status;

    set_psr(get_psr() | PSR_INTERRUPTS);

    while (!signaled())
    {
        result = wait_device("clock", &status);
        if (result != 0)
        {
            return 0;
        }

        int currentTime;                                         // TEST00 ALTER use clock device time
        SleepRequest* _WakeRequest;                              // TEST00 ADD next sleeper ready

        currentTime = status;                                    // TEST00 ALTER use wait_device status

        while (pSleepHead != NULL && pSleepHead->wakeTime <= currentTime)
        {
            _WakeRequest = pSleepHead;                           // TEST00 ADD capture ready sleeper
            pSleepHead = pSleepHead->pNext;                      // TEST00 ADD remove queue head
            unblock(_WakeRequest->pid);                     // TEST00 ADD wake sleeping process
        }
    }
    return 0;
}


static int DiskDriver(char* arg)
{
    int unit = atoi(arg);
    int currentTrack = 0;
    device_control_block_t devRequest;

    set_psr(get_psr() | PSR_INTERRUPTS);

    /* Read the disk info */

    /* Operating loop */
    int status;                                                    // TEST00 ADD disk wait status

    while (!signaled())
    {
        wait_device("clock", &status);
        //wait_device(diskInfo[unit].deviceName, &status);           // TEST00 ALTER block on disk device
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
    int pid;                                                     // TEST00 ADD current process id
    int currentTime;                                             // TEST00 ADD current system time
    SleepRequest* _SleepRequest;                                 // TEST00 ADD request table entry

    checkKernelMode(__func__);

    if (seconds < 0)
    {
        return ERR_INVALID;                                      // TEST00 ADD reject negative sleep
    }

    if (seconds == 0)
    {
        return ERR_OK;                                           // TEST00 ADD allow zero sleep
    }

    pid = getpid();                                              // TEST00 ADD lookup current pid
    currentTime = 0;                                              // TEST00 ADD Temporary

    _SleepRequest = &sleepRequests[pid % MAXPROC];               // TEST00 ADD select request slot
    _SleepRequest->pNext = NULL;                                 // TEST00 ADD clear next pointer
    _SleepRequest->pid = pid;                                    // TEST00 ADD record sleeping pid
    _SleepRequest->wakeTime = currentTime + (seconds * 1000000); // TEST00 ADD compute wake deadline

    insert_sleep_request(_SleepRequest);                         // TEST00 ADD insert into sleep queue

    block(0);                                                 // TEST00 ADD block until wakeup

    return ERR_OK;                                               // TEST00 ADD sleep completed
}

static void insert_sleep_request(SleepRequest* _SleepRequest)
{
    SleepRequest* _Current;                                      // TEST00 ADD walk sleep queue
    SleepRequest* _Previous;                                     // TEST00 ADD track prior node

    if (pSleepHead == NULL || _SleepRequest->wakeTime < pSleepHead->wakeTime)
    {
        _SleepRequest->pNext = pSleepHead;                       // TEST00 ADD insert at queue head
        pSleepHead = _SleepRequest;                              // TEST00 ADD update sleep head
        return;
    }

    _Previous = pSleepHead;                                      // TEST00 ADD start at queue head
    _Current = pSleepHead->pNext;                                // TEST00 ADD advance to next node

    while (_Current != NULL && _Current->wakeTime <= _SleepRequest->wakeTime)
    {
        _Previous = _Current;                                    // TEST00 ADD move previous forward
        _Current = _Current->pNext;                              // TEST00 ADD move current forward
    }

    _SleepRequest->pNext = _Current;                             // TEST00 ADD link new request
    _Previous->pNext = _SleepRequest;                            // TEST00 ADD splice into queue
}

