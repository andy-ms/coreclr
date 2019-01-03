// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.

#include <cstdint>
#include <cassert>
#include <cstddef>
#include <memory>
#include "windows.h"
#include "psapi.h"
#include "env/gcenv.structs.h"
#include "env/gcenv.base.h"
#include "env/gcenv.os.h"
#include "env/gcenv.ee.h"
#include "env/gcenv.windows.inl"
#include "env/volatile.h"
#include "gcconfig.h"

GCSystemInfo g_SystemInfo;

typedef BOOL (WINAPI *PGET_PROCESS_MEMORY_INFO)(HANDLE handle, PROCESS_MEMORY_COUNTERS* memCounters, uint32_t cb);
static PGET_PROCESS_MEMORY_INFO GCGetProcessMemoryInfo = 0;

static size_t g_RestrictedPhysicalMemoryLimit = (size_t)UINTPTR_MAX;

// For 32-bit processes the virtual address range could be smaller than the amount of physical
// memory on the machine/in the container, we need to restrict by the VM.
static bool g_UseRestrictedVirtualMemory = false;

typedef BOOL (WINAPI *PIS_PROCESS_IN_JOB)(HANDLE processHandle, HANDLE jobHandle, BOOL* result);
typedef BOOL (WINAPI *PQUERY_INFORMATION_JOB_OBJECT)(HANDLE jobHandle, JOBOBJECTINFOCLASS jobObjectInfoClass, void* lpJobObjectInfo, DWORD cbJobObjectInfoLength, LPDWORD lpReturnLength);

namespace {

static bool g_fEnableGCNumaAware;

struct CPU_Group_Info 
{
    WORD    nr_active;  // at most 64
    WORD    reserved[1];
    WORD    begin;
    WORD    end;
    DWORD_PTR   active_mask;
    DWORD   groupWeight;
    DWORD   activeThreadWeight;
};

static bool g_fEnableGCCPUGroups;
static bool g_fHadSingleProcessorAtStartup;
static DWORD  g_nGroups;
static DWORD g_nProcessors;
static CPU_Group_Info *g_CPUGroupInfoArray;

void InitNumaNodeInfo()
{
    ULONG highest = 0;
    
    g_fEnableGCNumaAware = false;

    if (!GCConfig::GetGCNumaAware())
        return;

    // fail to get the highest numa node number
    if (!GetNumaHighestNodeNumber(&highest) || (highest == 0))
        return;

    g_fEnableGCNumaAware = true;
    return;
}

#if (defined(_TARGET_AMD64_) || defined(_TARGET_ARM64_))
// Calculate greatest common divisor
DWORD GCD(DWORD u, DWORD v)
{
    while (v != 0)
    {
        DWORD dwTemp = v;
        v = u % v;
        u = dwTemp;
    }

    return u;
}

// Calculate least common multiple
DWORD LCM(DWORD u, DWORD v)
{
    return u / GCD(u, v) * v;
}
#endif

bool InitCPUGroupInfoArray()
{
#if (defined(_TARGET_AMD64_) || defined(_TARGET_ARM64_))
    BYTE *bBuffer = NULL;
    SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *pSLPIEx = NULL;
    SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *pRecord = NULL;
    DWORD cbSLPIEx = 0;
    DWORD byteOffset = 0;
    DWORD dwNumElements = 0;
    DWORD dwWeight = 1;

    if (GetLogicalProcessorInformationEx(RelationGroup, pSLPIEx, &cbSLPIEx) &&
                      GetLastError() != ERROR_INSUFFICIENT_BUFFER)
        return false;

    assert(cbSLPIEx);

    // Fail to allocate buffer
    bBuffer = new (std::nothrow) BYTE[ cbSLPIEx ];
    if (bBuffer == NULL)
        return false;

    pSLPIEx = (SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *)bBuffer;
    if (!GetLogicalProcessorInformationEx(RelationGroup, pSLPIEx, &cbSLPIEx))
    {
        delete[] bBuffer;
        return false;
    }

    pRecord = pSLPIEx;
    while (byteOffset < cbSLPIEx)
    {
        if (pRecord->Relationship == RelationGroup)
        {
            g_nGroups = pRecord->Group.ActiveGroupCount;
            break;
        }
        byteOffset += pRecord->Size;
        pRecord = (SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *)(bBuffer + byteOffset);
    }

    g_CPUGroupInfoArray = new (std::nothrow) CPU_Group_Info[g_nGroups];
    if (g_CPUGroupInfoArray == NULL) 
    {
        delete[] bBuffer;
        return false;
    }

    for (DWORD i = 0; i < g_nGroups; i++)
    {
        g_CPUGroupInfoArray[i].nr_active   = (WORD)pRecord->Group.GroupInfo[i].ActiveProcessorCount;
        g_CPUGroupInfoArray[i].active_mask = pRecord->Group.GroupInfo[i].ActiveProcessorMask;
        g_nProcessors += g_CPUGroupInfoArray[i].nr_active;
        dwWeight = LCM(dwWeight, (DWORD)g_CPUGroupInfoArray[i].nr_active);
    }

    // The number of threads per group that can be supported will depend on the number of CPU groups
    // and the number of LPs within each processor group. For example, when the number of LPs in
    // CPU groups is the same and is 64, the number of threads per group before weight overflow
    // would be 2^32/2^6 = 2^26 (64M threads)
    for (DWORD i = 0; i < g_nGroups; i++)
    {
        g_CPUGroupInfoArray[i].groupWeight = dwWeight / (DWORD)g_CPUGroupInfoArray[i].nr_active;
        g_CPUGroupInfoArray[i].activeThreadWeight = 0;
    }

    delete[] bBuffer;  // done with it; free it
    return true;
#else
    return false;
#endif
}

bool InitCPUGroupInfoRange()
{
#if (defined(_TARGET_AMD64_) || defined(_TARGET_ARM64_))
    WORD begin   = 0;
    WORD nr_proc = 0;

    for (WORD i = 0; i < g_nGroups; i++) 
    {
        nr_proc += g_CPUGroupInfoArray[i].nr_active;
        g_CPUGroupInfoArray[i].begin = begin;
        g_CPUGroupInfoArray[i].end   = nr_proc - 1;
        begin = nr_proc;
    }

    return true;
#else
    return false;
#endif
}

void InitCPUGroupInfo()
{
    g_fEnableGCCPUGroups = false;

#if (defined(_TARGET_AMD64_) || defined(_TARGET_ARM64_))
    if (!GCConfig::GetGCCpuGroup())
        return;

    if (!InitCPUGroupInfoArray())
        return;

    if (!InitCPUGroupInfoRange())
        return;

    // only enable CPU groups if more than one group exists
    g_fEnableGCCPUGroups = g_nGroups > 1;
#endif // _TARGET_AMD64_ || _TARGET_ARM64_

    // Determine if the process is affinitized to a single processor (or if the system has a single processor)
    DWORD_PTR processAffinityMask, systemAffinityMask;
    if (::GetProcessAffinityMask(::GetCurrentProcess(), &processAffinityMask, &systemAffinityMask))
    {
        processAffinityMask &= systemAffinityMask;
        if (processAffinityMask != 0 && // only one CPU group is involved
            (processAffinityMask & (processAffinityMask - 1)) == 0) // only one bit is set
        {
            g_fHadSingleProcessorAtStartup = true;
        }
    }
}

void GetProcessMemoryLoad(LPMEMORYSTATUSEX pMSEX)
{
    pMSEX->dwLength = sizeof(MEMORYSTATUSEX);
    BOOL fRet = ::GlobalMemoryStatusEx(pMSEX);
    assert(fRet);
}

static size_t GetRestrictedPhysicalMemoryLimit()
{
    LIMITED_METHOD_CONTRACT;

    // The limit was cached already
    if (g_RestrictedPhysicalMemoryLimit != (size_t)UINTPTR_MAX)
        return g_RestrictedPhysicalMemoryLimit;

    size_t job_physical_memory_limit = (size_t)UINTPTR_MAX;
    uint64_t total_virtual = 0;
    uint64_t total_physical = 0;
    BOOL in_job_p = FALSE;
    HINSTANCE hinstKernel32 = 0;

    PIS_PROCESS_IN_JOB GCIsProcessInJob = 0;
    PQUERY_INFORMATION_JOB_OBJECT GCQueryInformationJobObject = 0;

    hinstKernel32 = LoadLibraryEx(L"kernel32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!hinstKernel32)
        goto exit;

    GCIsProcessInJob = (PIS_PROCESS_IN_JOB)GetProcAddress(hinstKernel32, "IsProcessInJob");
    if (!GCIsProcessInJob)
        goto exit;

    if (!GCIsProcessInJob(GetCurrentProcess(), NULL, &in_job_p))
        goto exit;

    if (in_job_p)
    {
        GCGetProcessMemoryInfo = (PGET_PROCESS_MEMORY_INFO)GetProcAddress(hinstKernel32, "K32GetProcessMemoryInfo");

        if (!GCGetProcessMemoryInfo)
            goto exit;

        GCQueryInformationJobObject = (PQUERY_INFORMATION_JOB_OBJECT)GetProcAddress(hinstKernel32, "QueryInformationJobObject");

        if (!GCQueryInformationJobObject)
            goto exit;

        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limit_info;
        if (GCQueryInformationJobObject (NULL, JobObjectExtendedLimitInformation, &limit_info, 
            sizeof(limit_info), NULL))
        {
            size_t job_memory_limit = (size_t)UINTPTR_MAX;
            size_t job_process_memory_limit = (size_t)UINTPTR_MAX;
            size_t job_workingset_limit = (size_t)UINTPTR_MAX;

            // Notes on the NT job object:
            //
            // You can specific a bigger process commit or working set limit than 
            // job limit which is pointless so we use the smallest of all 3 as
            // to calculate our "physical memory load" or "available physical memory"
            // when running inside a job object, ie, we treat this as the amount of physical memory
            // our process is allowed to use.
            // 
            // The commit limit is already reflected by default when you run in a 
            // job but the physical memory load is not.
            //
            if ((limit_info.BasicLimitInformation.LimitFlags & JOB_OBJECT_LIMIT_JOB_MEMORY) != 0)
                job_memory_limit = limit_info.JobMemoryLimit;
            if ((limit_info.BasicLimitInformation.LimitFlags & JOB_OBJECT_LIMIT_PROCESS_MEMORY) != 0)
                job_process_memory_limit = limit_info.ProcessMemoryLimit;
            if ((limit_info.BasicLimitInformation.LimitFlags & JOB_OBJECT_LIMIT_WORKINGSET) != 0)
                job_workingset_limit = limit_info.BasicLimitInformation.MaximumWorkingSetSize;

            job_physical_memory_limit = min (job_memory_limit, job_process_memory_limit);
            job_physical_memory_limit = min (job_physical_memory_limit, job_workingset_limit);

            MEMORYSTATUSEX ms;
            ::GetProcessMemoryLoad(&ms);
            total_virtual = ms.ullTotalVirtual;
            total_physical = ms.ullAvailPhys;

            // A sanity check in case someone set a larger limit than there is actual physical memory.
            job_physical_memory_limit = (size_t) min (job_physical_memory_limit, ms.ullTotalPhys);
        }
    }

exit:
    if (job_physical_memory_limit == (size_t)UINTPTR_MAX)
    {
        job_physical_memory_limit = 0;

        if (hinstKernel32 != 0)
        {
            FreeLibrary(hinstKernel32);
            hinstKernel32 = 0;
            GCGetProcessMemoryInfo = 0;
        }
    }

    // Check to see if we are limited by VM.
    if (total_virtual == 0)
    {
        MEMORYSTATUSEX ms;
        ::GetProcessMemoryLoad(&ms);

        total_virtual = ms.ullTotalVirtual;
        total_physical = ms.ullTotalPhys;
    }

    if (job_physical_memory_limit != 0)
    {
        total_physical = job_physical_memory_limit;
    }

    if (total_virtual < total_physical)
    {
        if (hinstKernel32 != 0)
        {
            // We can also free the lib here - if we are limited by VM we will not be calling
            // GetProcessMemoryInfo.
            FreeLibrary(hinstKernel32);
            GCGetProcessMemoryInfo = 0;
        }
        g_UseRestrictedVirtualMemory = true;
        job_physical_memory_limit = (size_t)total_virtual;
    }

    VolatileStore(&g_RestrictedPhysicalMemoryLimit, job_physical_memory_limit);
    return g_RestrictedPhysicalMemoryLimit;
}

// This function checks to see if GetLogicalProcessorInformation API is supported. 
// On success, this function allocates a SLPI array, sets nEntries to number 
// of elements in the SLPI array and returns a pointer to the SLPI array after filling it with information. 
//
// Note: If successful, GetLPI allocates memory for the SLPI array and expects the caller to
// free the memory once the caller is done using the information in the SLPI array.
SYSTEM_LOGICAL_PROCESSOR_INFORMATION *GetLPI(PDWORD nEntries) 
{
    DWORD cbslpi = 0;
    DWORD dwNumElements = 0;
    SYSTEM_LOGICAL_PROCESSOR_INFORMATION *pslpi = NULL;

    // We setup the first call to GetLogicalProcessorInformation to fail so that we can obtain
    // the size of the buffer required to allocate for the SLPI array that is returned

    if (!GetLogicalProcessorInformation(pslpi, &cbslpi) &&
            GetLastError() != ERROR_INSUFFICIENT_BUFFER)
    {
        // If we fail with anything other than an ERROR_INSUFFICIENT_BUFFER here, we punt with failure.
        return NULL;
    }

    _ASSERTE(cbslpi);

    // compute the number of SLPI entries required to hold the information returned from GLPI

    dwNumElements = cbslpi / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION);

    // allocate a buffer in the free heap to hold an array of SLPI entries from GLPI, number of elements in the array is dwNumElements 

    pslpi = new (std::nothrow) SYSTEM_LOGICAL_PROCESSOR_INFORMATION[ dwNumElements ];

    if (pslpi == NULL)
    {
        // the memory allocation failed
        return NULL;
    }      

    // Make call to GetLogicalProcessorInformation. Returns array of SLPI structures

    if (!GetLogicalProcessorInformation(pslpi, &cbslpi))
    {
        // GetLogicalProcessorInformation failed
        delete[] pslpi ; //Allocation was fine but the API call itself failed and so we are releasing the memory before the return NULL.
        return NULL ;
    } 

    // GetLogicalProcessorInformation successful, set nEntries to number of entries in the SLPI array
    *nEntries  = dwNumElements;

    return pslpi;    // return pointer to SLPI array

}//GetLPI

// This function returns the size of highest level cache on the physical chip.   If it cannot
// determine the cachesize this function returns 0.
size_t GetLogicalProcessorCacheSizeFromOS()
{
    size_t cache_size = 0;
    DWORD nEntries = 0;

    // Try to use GetLogicalProcessorInformation API and get a valid pointer to the SLPI array if successful.  Returns NULL
    // if API not present or on failure.

    SYSTEM_LOGICAL_PROCESSOR_INFORMATION *pslpi = GetLPI(&nEntries) ;   

    if (pslpi == NULL)
    {
        // GetLogicalProcessorInformation not supported or failed.  
        goto Exit;
    }

    // Crack the information. Iterate through all the SLPI array entries for all processors in system.
    // Will return the greatest of all the processor cache sizes or zero
    {
        size_t last_cache_size = 0;

        for (DWORD i=0; i < nEntries; i++)
        {
            if (pslpi[i].Relationship == RelationCache)
            {
                last_cache_size = max(last_cache_size, pslpi[i].Cache.Size);
            }             
        }  
        cache_size = last_cache_size;
    }
Exit:

    if(pslpi)
        delete[] pslpi;  // release the memory allocated for the SLPI array.    

    return cache_size;
}

} // anonymous namespace

// Initialize the interface implementation
// Return:
//  true if it has succeeded, false if it has failed
bool GCToOSInterface::Initialize()
{
    SYSTEM_INFO systemInfo;
    GetSystemInfo(&systemInfo);

    g_SystemInfo.dwNumberOfProcessors = systemInfo.dwNumberOfProcessors;
    g_SystemInfo.dwPageSize = systemInfo.dwPageSize;
    g_SystemInfo.dwAllocationGranularity = systemInfo.dwAllocationGranularity;

    assert(systemInfo.dwPageSize == 0x1000);

    InitNumaNodeInfo();
    InitCPUGroupInfo();

    return true;
}

// Shutdown the interface implementation
void GCToOSInterface::Shutdown()
{
    // nothing to do.
}

// Get numeric id of the current thread if possible on the 
// current platform. It is indended for logging purposes only.
// Return:
//  Numeric id of the current thread or 0 if the 
uint64_t GCToOSInterface::GetCurrentThreadIdForLogging()
{
    return ::GetCurrentThreadId();
}

// Get id of the process
uint32_t GCToOSInterface::GetCurrentProcessId()
{
    return ::GetCurrentThreadId();
}

// Set ideal affinity for the current thread
// Parameters:
//  affinity - ideal processor affinity for the thread
// Return:
//  true if it has succeeded, false if it has failed
bool GCToOSInterface::SetCurrentThreadIdealAffinity(GCThreadAffinity* affinity)
{
    bool success = true;

#if !defined(FEATURE_CORESYSTEM)
    SetThreadIdealProcessor(GetCurrentThread(), (DWORD)affinity->Processor);
#else
    PROCESSOR_NUMBER proc;

    if (affinity->Group != -1)
    {
        proc.Group = (WORD)affinity->Group;
        proc.Number = (BYTE)affinity->Processor;
        proc.Reserved = 0;

        success = !!SetThreadIdealProcessorEx(GetCurrentThread(), &proc, NULL);
    }
    else
    {
        if (GetThreadIdealProcessorEx(GetCurrentThread(), &proc))
        {
            proc.Number = affinity->Processor;
            success = !!SetThreadIdealProcessorEx(GetCurrentThread(), &proc, NULL);
        }
    }
#endif

    return success;
}

// Get the number of the current processor
uint32_t GCToOSInterface::GetCurrentProcessorNumber()
{
    assert(GCToOSInterface::CanGetCurrentProcessorNumber());
    return ::GetCurrentProcessorNumber();
}

// Check if the OS supports getting current processor number
bool GCToOSInterface::CanGetCurrentProcessorNumber()
{
    // on all Windows platforms we support this API exists
    return true;
}

// Flush write buffers of processors that are executing threads of the current process
void GCToOSInterface::FlushProcessWriteBuffers()
{
    ::FlushProcessWriteBuffers();
}

// Break into a debugger
void GCToOSInterface::DebugBreak()
{
    ::DebugBreak();
}

// Causes the calling thread to sleep for the specified number of milliseconds
// Parameters:
//  sleepMSec   - time to sleep before switching to another thread
void GCToOSInterface::Sleep(uint32_t sleepMSec)
{
    // TODO(segilles) CLR implementation of __SwitchToThread spins for short sleep durations
    // to avoid context switches - is that interesting or useful here?
    if (sleepMSec > 0) 
    {
        ::SleepEx(sleepMSec, FALSE);
    }
}

// Causes the calling thread to yield execution to another thread that is ready to run on the current processor.
// Parameters:
//  switchCount - number of times the YieldThread was called in a loop
void GCToOSInterface::YieldThread(uint32_t switchCount)
{
    UNREFERENCED_PARAMETER(switchCount);
    SwitchToThread();
}

// Reserve virtual memory range.
// Parameters:
//  address   - starting virtual address, it can be NULL to let the function choose the starting address
//  size      - size of the virtual memory range
//  alignment - requested memory alignment, 0 means no specific alignment requested
//  flags     - flags to control special settings like write watching
// Return:
//  Starting virtual address of the reserved range
void* GCToOSInterface::VirtualReserve(size_t size, size_t alignment, uint32_t flags)
{
    // Windows already ensures 64kb alignment on VirtualAlloc. The current CLR
    // implementation ignores it on Windows, other than making some sanity checks on it.
    UNREFERENCED_PARAMETER(alignment);
    assert((alignment & (alignment - 1)) == 0);
    assert(alignment <= 0x10000);
    DWORD memFlags = (flags & VirtualReserveFlags::WriteWatch) ? (MEM_RESERVE | MEM_WRITE_WATCH) : MEM_RESERVE;
    return ::VirtualAlloc(nullptr, size, memFlags, PAGE_READWRITE);
}

// Release virtual memory range previously reserved using VirtualReserve
// Parameters:
//  address - starting virtual address
//  size    - size of the virtual memory range
// Return:
//  true if it has succeeded, false if it has failed
bool GCToOSInterface::VirtualRelease(void* address, size_t size)
{
    return !!::VirtualFree(address, 0, MEM_RELEASE);
}

// Commit virtual memory range. It must be part of a range reserved using VirtualReserve.
// Parameters:
//  address - starting virtual address
//  size    - size of the virtual memory range
// Return:
//  true if it has succeeded, false if it has failed
bool GCToOSInterface::VirtualCommit(void* address, size_t size, uint32_t node)
{
    if (node == NUMA_NODE_UNDEFINED)
    {
        return ::VirtualAlloc(address, size, MEM_COMMIT, PAGE_READWRITE) != nullptr;
    }
    else
    {
        assert(g_fEnableGCNumaAware);
        return ::VirtualAllocExNuma(::GetCurrentProcess(), address, size, MEM_COMMIT, PAGE_READWRITE, node) != nullptr;
    }
}

// Decomit virtual memory range.
// Parameters:
//  address - starting virtual address
//  size    - size of the virtual memory range
// Return:
//  true if it has succeeded, false if it has failed
bool GCToOSInterface::VirtualDecommit(void* address, size_t size)
{
    return !!::VirtualFree(address, size, MEM_DECOMMIT);
}

// Reset virtual memory range. Indicates that data in the memory range specified by address and size is no
// longer of interest, but it should not be decommitted.
// Parameters:
//  address - starting virtual address
//  size    - size of the virtual memory range
//  unlock  - true if the memory range should also be unlocked
// Return:
//  true if it has succeeded, false if it has failed. Returns false also if
//  unlocking was requested but the unlock failed.
bool GCToOSInterface::VirtualReset(void * address, size_t size, bool unlock)
{
    bool success = ::VirtualAlloc(address, size, MEM_RESET, PAGE_READWRITE) != nullptr;
    if (success && unlock)
    {
        ::VirtualUnlock(address, size);
    }

    return success;
}

// Check if the OS supports write watching
bool GCToOSInterface::SupportsWriteWatch()
{
    void* mem = GCToOSInterface::VirtualReserve(g_SystemInfo.dwAllocationGranularity, 0, VirtualReserveFlags::WriteWatch);
    if (mem != nullptr)
    {
        GCToOSInterface::VirtualRelease(mem, g_SystemInfo.dwAllocationGranularity);
        return true;
    }

    return false;
}

// Reset the write tracking state for the specified virtual memory range.
// Parameters:
//  address - starting virtual address
//  size    - size of the virtual memory range
void GCToOSInterface::ResetWriteWatch(void* address, size_t size)
{
    ::ResetWriteWatch(address, size);
}

// Retrieve addresses of the pages that are written to in a region of virtual memory
// Parameters:
//  resetState         - true indicates to reset the write tracking state
//  address            - starting virtual address
//  size               - size of the virtual memory range
//  pageAddresses      - buffer that receives an array of page addresses in the memory region
//  pageAddressesCount - on input, size of the lpAddresses array, in array elements
//                       on output, the number of page addresses that are returned in the array.
// Return:
//  true if it has succeeded, false if it has failed
bool GCToOSInterface::GetWriteWatch(bool resetState, void* address, size_t size, void** pageAddresses, uintptr_t* pageAddressesCount)
{
    uint32_t flags = resetState ? 1 : 0;
    ULONG granularity;

    bool success = ::GetWriteWatch(flags, address, size, pageAddresses, (ULONG_PTR*)pageAddressesCount, &granularity) == 0;
    if (success)
    {
        assert(granularity == OS_PAGE_SIZE);
    }

    return success;
}

// Get size of the largest cache on the processor die
// Parameters:
//  trueSize - true to return true cache size, false to return scaled up size based on
//             the processor architecture
// Return:
//  Size of the cache
size_t GCToOSInterface::GetCacheSizePerLogicalCpu(bool trueSize)
{
    static size_t maxSize;
    static size_t maxTrueSize;

    if (maxSize)
    {
        // maxSize and maxTrueSize cached
        if (trueSize)
        {
            return maxTrueSize;
        }
        else
        {
            return maxSize;
        }
    }

#if defined(_AMD64_) || defined (_X86_)
    int dwBuffer[4];

    __cpuid(dwBuffer, 0);

    int maxCpuId = dwBuffer[0];

    if (dwBuffer[1] == 'uneG') 
    {
        if (dwBuffer[3] == 'Ieni') 
        {
            if (dwBuffer[2] == 'letn') 
            {
                maxTrueSize = GetLogicalProcessorCacheSizeFromOS(); //use OS API for cache enumeration on LH and above
#ifdef BIT64
                if (maxCpuId >= 2)
                {
                    // If we're running on a Prescott or greater core, EM64T tests
                    // show that starting with a gen0 larger than LLC improves performance.
                    // Thus, start with a gen0 size that is larger than the cache.  The value of
                    // 3 is a reasonable tradeoff between workingset and performance.
                    maxSize = maxTrueSize * 3;
                }
                else
#endif
                {
                    maxSize = maxTrueSize;
                }
            }
        }
    }

    if (dwBuffer[1] == 'htuA') {
        if (dwBuffer[3] == 'itne') {
            if (dwBuffer[2] == 'DMAc') {
                __cpuid(dwBuffer, 0x80000000);
                if (dwBuffer[0] >= 0x80000006)
                {
                    __cpuid(dwBuffer, 0x80000006);

                    DWORD dwL2CacheBits = dwBuffer[2];
                    DWORD dwL3CacheBits = dwBuffer[3];

                    maxTrueSize = (size_t)((dwL2CacheBits >> 16) * 1024);    // L2 cache size in ECX bits 31-16
                            
                    __cpuid(dwBuffer, 0x1);
                    DWORD dwBaseFamily = (dwBuffer[0] & (0xF << 8)) >> 8;
                    DWORD dwExtFamily  = (dwBuffer[0] & (0xFF << 20)) >> 20;
                    DWORD dwFamily = dwBaseFamily >= 0xF ? dwBaseFamily + dwExtFamily : dwBaseFamily;

                    if (dwFamily >= 0x10)
                    {
                        BOOL bSkipAMDL3 = FALSE;

                        if (dwFamily == 0x10)   // are we running on a Barcelona (Family 10h) processor?
                        {
                            // check model
                            DWORD dwBaseModel = (dwBuffer[0] & (0xF << 4)) >> 4 ;
                            DWORD dwExtModel  = (dwBuffer[0] & (0xF << 16)) >> 16;
                            DWORD dwModel = dwBaseFamily >= 0xF ? (dwExtModel << 4) | dwBaseModel : dwBaseModel;

                            switch (dwModel)
                            {
                                case 0x2:
                                    // 65nm parts do not benefit from larger Gen0
                                    bSkipAMDL3 = TRUE;
                                    break;

                                case 0x4:
                                default:
                                    bSkipAMDL3 = FALSE;
                            }
                        }

                        if (!bSkipAMDL3)
                        {
                            // 45nm Greyhound parts (and future parts based on newer northbridge) benefit
                            // from increased gen0 size, taking L3 into account
                            __cpuid(dwBuffer, 0x80000008);
                            DWORD dwNumberOfCores = (dwBuffer[2] & (0xFF)) + 1;     // NC is in ECX bits 7-0

                            DWORD dwL3CacheSize = (size_t)((dwL3CacheBits >> 18) * 512 * 1024);  // L3 size in EDX bits 31-18 * 512KB
                            // L3 is shared between cores
                            dwL3CacheSize = dwL3CacheSize / dwNumberOfCores;
                            maxTrueSize += dwL3CacheSize;       // due to exclusive caches, add L3 size (possibly zero) to L2
                                                                // L1 is too small to worry about, so ignore it
                        }
                    }


                    maxSize = maxTrueSize;
                }
            }
        }
    }

#else
    maxSize = maxTrueSize = GetLogicalProcessorCacheSizeFromOS() ; // Returns the size of the highest level processor cache
#endif

#if defined(_ARM64_)
    // Bigger gen0 size helps arm64 targets
    maxSize = maxTrueSize * 3;
#endif

    //    printf("GetCacheSizePerLogicalCpu returns %d, adjusted size %d\n", maxSize, maxTrueSize);
    if (trueSize)
        return maxTrueSize;
    else
        return maxSize;
}

// Sets the calling thread's affinity to only run on the processor specified
// in the GCThreadAffinity structure.
// Parameters:
//  affinity - The requested affinity for the calling thread. At most one processor
//             can be provided.
// Return:
//  true if setting the affinity was successful, false otherwise.
bool GCToOSInterface::SetThreadAffinity(GCThreadAffinity* affinity)
{
    assert(affinity != nullptr);
    if (affinity->Group != GCThreadAffinity::None)
    {
        assert(affinity->Processor != GCThreadAffinity::None);

        GROUP_AFFINITY ga;
        ga.Group = (WORD)affinity->Group;
        ga.Reserved[0] = 0; // reserve must be filled with zero
        ga.Reserved[1] = 0; // otherwise call may fail
        ga.Reserved[2] = 0;
        ga.Mask = (size_t)1 << affinity->Processor;
        return !!SetThreadGroupAffinity(GetCurrentThread(), &ga, nullptr);
    }
    else if (affinity->Processor != GCThreadAffinity::None)
    {
        return !!SetThreadAffinityMask(GetCurrentThread(), (DWORD_PTR)1 << affinity->Processor);
    }

    // Given affinity must specify at least one processor to use.
    return false;
}

// Boosts the calling thread's thread priority to a level higher than the default
// for new threads.
// Parameters:
//  None.
// Return:
//  true if the priority boost was successful, false otherwise.
bool GCToOSInterface::BoostThreadPriority()
{
    return !!SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
}

// Get affinity mask of the current process
// Parameters:
//  processMask - affinity mask for the specified process
//  systemMask  - affinity mask for the system
// Return:
//  true if it has succeeded, false if it has failed
// Remarks:
//  A process affinity mask is a bit vector in which each bit represents the processors that
//  a process is allowed to run on. A system affinity mask is a bit vector in which each bit
//  represents the processors that are configured into a system.
//  A process affinity mask is a subset of the system affinity mask. A process is only allowed
//  to run on the processors configured into a system. Therefore, the process affinity mask cannot
//  specify a 1 bit for a processor when the system affinity mask specifies a 0 bit for that processor.
bool GCToOSInterface::GetCurrentProcessAffinityMask(uintptr_t* processMask, uintptr_t* systemMask)
{
    return !!::GetProcessAffinityMask(::GetCurrentProcess(), (PDWORD_PTR)processMask, (PDWORD_PTR)systemMask);
}

// Get number of processors assigned to the current process
// Return:
//  The number of processors
uint32_t GCToOSInterface::GetCurrentProcessCpuCount()
{
    static int cCPUs = 0;

    if (cCPUs != 0)
        return cCPUs;

    int count = 0;
    DWORD_PTR pmask, smask;

    if (!GetProcessAffinityMask(GetCurrentProcess(), &pmask, &smask))
    {
        count = 1;
    }
    else
    {
        pmask &= smask;

        while (pmask)
        {
            pmask &= (pmask - 1);
            count++;
        }

        // GetProcessAffinityMask can return pmask=0 and smask=0 on systems with more
        // than 64 processors, which would leave us with a count of 0.  Since the GC
        // expects there to be at least one processor to run on (and thus at least one
        // heap), we'll return 64 here if count is 0, since there are likely a ton of
        // processors available in that case.  The GC also cannot (currently) handle
        // the case where there are more than 64 processors, so we will return a
        // maximum of 64 here.
        if (count == 0 || count > 64)
            count = 64;
    }

    cCPUs = count;

    return count;
}

// Return the size of the user-mode portion of the virtual address space of this process.
// Return:
//  non zero if it has succeeded, 0 if it has failed
size_t GCToOSInterface::GetVirtualMemoryLimit()
{
    MEMORYSTATUSEX memStatus;
    GetProcessMemoryLoad(&memStatus);
    assert(memStatus.ullAvailVirtual != 0);
    return (size_t)memStatus.ullAvailVirtual;
}

// Get the physical memory that this process can use.
// Return:
//  non zero if it has succeeded, 0 if it has failed
// Remarks:
//  If a process runs with a restricted memory limit, it returns the limit. If there's no limit 
//  specified, it returns amount of actual physical memory.
uint64_t GCToOSInterface::GetPhysicalMemoryLimit(bool* is_restricted)
{
    if (is_restricted)
        *is_restricted = false;

    size_t restricted_limit = GetRestrictedPhysicalMemoryLimit();
    if (restricted_limit != 0)
    {
        if (is_restricted)
            *is_restricted = true;

        return restricted_limit;
    }

    MEMORYSTATUSEX memStatus;
    GetProcessMemoryLoad(&memStatus);
    assert(memStatus.ullTotalPhys != 0);
    return memStatus.ullTotalPhys;
}

// Get memory status
// Parameters:
//  memory_load - A number between 0 and 100 that specifies the approximate percentage of physical memory
//      that is in use (0 indicates no memory use and 100 indicates full memory use).
//  available_physical - The amount of physical memory currently available, in bytes.
//  available_page_file - The maximum amount of memory the current process can commit, in bytes.
void GCToOSInterface::GetMemoryStatus(uint32_t* memory_load, uint64_t* available_physical, uint64_t* available_page_file)
{
    uint64_t restricted_limit = GetRestrictedPhysicalMemoryLimit();
    if (restricted_limit != 0)
    {
        size_t workingSetSize;
        BOOL status = FALSE;
        if (!g_UseRestrictedVirtualMemory)
        {
            PROCESS_MEMORY_COUNTERS pmc;
            status = GCGetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc));
            workingSetSize = pmc.WorkingSetSize;
        }

        if(status)
        {
            if (memory_load)
                *memory_load = (uint32_t)((float)workingSetSize * 100.0 / (float)restricted_limit);
            if (available_physical)
            {
                if(workingSetSize > restricted_limit)
                    *available_physical = 0;
                else
                    *available_physical = restricted_limit - workingSetSize;
            }
            // Available page file doesn't mean much when physical memory is restricted since
            // we don't know how much of it is available to this process so we are not going to 
            // bother to make another OS call for it.
            if (available_page_file)
                *available_page_file = 0;

            return;
        }
    }

    MEMORYSTATUSEX ms;
    ::GetProcessMemoryLoad(&ms);
    
    if (g_UseRestrictedVirtualMemory)
    {
        _ASSERTE (ms.ullTotalVirtual == restricted_limit);
        if (memory_load != NULL)
            *memory_load = (uint32_t)((float)(ms.ullTotalVirtual - ms.ullAvailVirtual) * 100.0 / (float)ms.ullTotalVirtual);
        if (available_physical != NULL)
            *available_physical = ms.ullTotalVirtual;

        // Available page file isn't helpful when we are restricted by virtual memory
        // since the amount of memory we can reserve is less than the amount of
        // memory we can commit.
        if (available_page_file != NULL)
            *available_page_file = 0;
    }
    else
    {
        if (memory_load != NULL)
            *memory_load = ms.dwMemoryLoad;
        if (available_physical != NULL)
            *available_physical = ms.ullAvailPhys;
        if (available_page_file != NULL)
            *available_page_file = ms.ullAvailPageFile;
    }
}

// Get a high precision performance counter
// Return:
//  The counter value
int64_t GCToOSInterface::QueryPerformanceCounter()
{
    LARGE_INTEGER ts;
    if (!::QueryPerformanceCounter(&ts))
    {
        assert(false && "Failed to query performance counter");
    }

    return ts.QuadPart;
}

// Get a frequency of the high precision performance counter
// Return:
//  The counter frequency
int64_t GCToOSInterface::QueryPerformanceFrequency()
{
    LARGE_INTEGER ts;
    if (!::QueryPerformanceFrequency(&ts))
    {
        assert(false && "Failed to query performance counter");
    }

    return ts.QuadPart;
}

// Get a time stamp with a low precision
// Return:
//  Time stamp in milliseconds
uint32_t GCToOSInterface::GetLowPrecisionTimeStamp()
{
    return ::GetTickCount();
}

// Gets the total number of processors on the machine, not taking
// into account current process affinity.
// Return:
//  Number of processors on the machine
uint32_t GCToOSInterface::GetTotalProcessorCount()
{
    if (CanEnableGCCPUGroups())
    {
        return g_nProcessors;
    }
    else
    {
        return g_SystemInfo.dwNumberOfProcessors;
    }
}
 
bool GCToOSInterface::CanEnableGCNumaAware()
{
    return g_fEnableGCNumaAware;
}

bool GCToOSInterface::GetNumaProcessorNode(PPROCESSOR_NUMBER proc_no, uint16_t *node_no)
{
    assert(g_fEnableGCNumaAware);
    return ::GetNumaProcessorNodeEx(proc_no, node_no) != FALSE;
}

bool GCToOSInterface::CanEnableGCCPUGroups()
{
    return g_fEnableGCCPUGroups;
}

void GCToOSInterface::GetGroupForProcessor(uint16_t processor_number, uint16_t* group_number, uint16_t* group_processor_number)
{
    assert(g_fEnableGCCPUGroups);

#if !defined(FEATURE_REDHAWK) && (defined(_TARGET_AMD64_) || defined(_TARGET_ARM64_))
    WORD bTemp = 0;
    WORD bDiff = processor_number - bTemp;

    for (WORD i=0; i < g_nGroups; i++)
    {
        bTemp += g_CPUGroupInfoArray[i].nr_active;
        if (bTemp > processor_number)
        {
            *group_number = i;
            *group_processor_number = bDiff;
            break;
        }
        bDiff = processor_number - bTemp;
    }
#else
    *group_number = 0;
    *group_processor_number = 0;
#endif
}

// Parameters of the GC thread stub
struct GCThreadStubParam
{
    GCThreadFunction GCThreadFunction;
    void* GCThreadParam;
};

// GC thread stub to convert GC thread function to an OS specific thread function
static DWORD GCThreadStub(void* param)
{
    GCThreadStubParam *stubParam = (GCThreadStubParam*)param;
    GCThreadFunction function = stubParam->GCThreadFunction;
    void* threadParam = stubParam->GCThreadParam;

    delete stubParam;

    function(threadParam);

    return 0;
}

// Initialize the critical section
void CLRCriticalSection::Initialize()
{
    ::InitializeCriticalSection(&m_cs);
}

// Destroy the critical section
void CLRCriticalSection::Destroy()
{
    ::DeleteCriticalSection(&m_cs);
}

// Enter the critical section. Blocks until the section can be entered.
void CLRCriticalSection::Enter()
{
    ::EnterCriticalSection(&m_cs);
}

// Leave the critical section
void CLRCriticalSection::Leave()
{
    ::LeaveCriticalSection(&m_cs);
}

// WindowsEvent is an implementation of GCEvent that forwards
// directly to Win32 APIs.
class GCEvent::Impl
{
private:
    HANDLE m_hEvent;

public:
    Impl() : m_hEvent(INVALID_HANDLE_VALUE) {}

    bool IsValid() const
    {
        return m_hEvent != INVALID_HANDLE_VALUE;
    }

    void Set()
    {
        assert(IsValid());
        BOOL result = SetEvent(m_hEvent);
        assert(result && "SetEvent failed");
    }

    void Reset()
    {
        assert(IsValid());
        BOOL result = ResetEvent(m_hEvent);
        assert(result && "ResetEvent failed");
    }

    uint32_t Wait(uint32_t timeout, bool alertable)
    {
        UNREFERENCED_PARAMETER(alertable);
        assert(IsValid());

        return WaitForSingleObject(m_hEvent, timeout);
    }

    void CloseEvent()
    {
        assert(IsValid());
        BOOL result = CloseHandle(m_hEvent);
        assert(result && "CloseHandle failed");
        m_hEvent = INVALID_HANDLE_VALUE;
    }

    bool CreateAutoEvent(bool initialState)
    {
        m_hEvent = CreateEvent(nullptr, false, initialState, nullptr);
        return IsValid();
    }

    bool CreateManualEvent(bool initialState)
    {
        m_hEvent = CreateEvent(nullptr, true, initialState, nullptr);
        return IsValid();
    }
};

GCEvent::GCEvent()
  : m_impl(nullptr)
{
}

void GCEvent::CloseEvent()
{
    assert(m_impl != nullptr);
    m_impl->CloseEvent();
}

void GCEvent::Set()
{
    assert(m_impl != nullptr);
    m_impl->Set();
}

void GCEvent::Reset()
{
    assert(m_impl != nullptr);
    m_impl->Reset();
}

uint32_t GCEvent::Wait(uint32_t timeout, bool alertable)
{
    assert(m_impl != nullptr);
    return m_impl->Wait(timeout, alertable);
}

bool GCEvent::CreateAutoEventNoThrow(bool initialState)
{
    // [DESKTOP TODO] The difference between events and OS events is
    // whether or not the hosting API is made aware of them. When (if)
    // we implement hosting support for Local GC, we will need to be
    // aware of the host here.
    return CreateOSAutoEventNoThrow(initialState);
}

bool GCEvent::CreateManualEventNoThrow(bool initialState)
{
    // [DESKTOP TODO] The difference between events and OS events is
    // whether or not the hosting API is made aware of them. When (if)
    // we implement hosting support for Local GC, we will need to be
    // aware of the host here.
    return CreateOSManualEventNoThrow(initialState);
}

bool GCEvent::CreateOSAutoEventNoThrow(bool initialState)
{
    assert(m_impl == nullptr);
    std::unique_ptr<GCEvent::Impl> event(new (std::nothrow) GCEvent::Impl());
    if (!event)
    {
        return false;
    }

    if (!event->CreateAutoEvent(initialState))
    {
        return false;
    }

    m_impl = event.release();
    return true;
}

bool GCEvent::CreateOSManualEventNoThrow(bool initialState)
{
    assert(m_impl == nullptr);
    std::unique_ptr<GCEvent::Impl> event(new (std::nothrow) GCEvent::Impl());
    if (!event)
    {
        return false;
    }

    if (!event->CreateManualEvent(initialState))
    {
        return false;
    }

    m_impl = event.release();
    return true;
}
