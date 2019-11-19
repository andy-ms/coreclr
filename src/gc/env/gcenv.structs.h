// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.
#ifndef __GCENV_STRUCTS_INCLUDED__
#define __GCENV_STRUCTS_INCLUDED__
//
// Structs shared between the GC and the environment
//

struct GCSystemInfo
{
    uint32_t        dwNumberOfProcessors;
    uint32_t        dwPageSize;
    uint32_t        dwAllocationGranularity;
};

typedef void * HANDLE;

#ifdef PLATFORM_UNIX

typedef char TCHAR;
#define _T(s) s

#else

#ifndef _INC_WINDOWS
typedef wchar_t TCHAR;
#define _T(s) L##s
#endif

#endif

#ifdef PLATFORM_UNIX

class EEThreadId
{
    pthread_t m_id;
    // Indicates whether the m_id is valid or not. pthread_t doesn't have any
    // portable "invalid" value.
    bool m_isValid;

public:
    EEThreadId() : m_id{0}, m_isValid{false} {}

    EEThreadId(const EEThreadId& other) //TODO: kill, should be subsumed by below
        : m_id{other.m_id}, m_isValid{other.m_isValid} {}
    EEThreadId(const volatile EEThreadId& other)
        : m_id{other.m_id}, m_isValid{other.m_isValid} {}
    EEThreadId(const volatile EEThreadId&& other)
        : m_id{other.m_id}, m_isValid{other.m_isValid} {}

    void operator=(const volatile EEThreadId& other) volatile {
        m_id = other.m_id;
        m_isValid = other.m_isValid;
    }
    void operator=(const volatile EEThreadId&& other) volatile {
        m_id = other.m_id;
        m_isValid = other.m_isValid;
    }

    bool operator==(const volatile EEThreadId& other) const volatile {
        return m_isValid
            ? (other.m_isValid && pthread_equal(m_id, other.m_id))
            : !other.m_isValid;
    }
    bool operator==(const volatile EEThreadId&& other) const volatile {
        return m_isValid
            ? (other.m_isValid && pthread_equal(m_id, other.m_id))
            : !other.m_isValid;
    }

    bool IsCurrentThread()
    {
        return m_isValid && pthread_equal(m_id, pthread_self());
    }

    void SetToCurrentThread()
    {
        m_id = pthread_self();
        m_isValid = true;
    }

    void Clear()
    {
        m_isValid = false;
    }

    bool IsValid() const volatile
    {
        return m_isValid;
    }
};

#else // PLATFORM_UNIX

#ifndef _INC_WINDOWS
extern "C" uint32_t __stdcall GetCurrentThreadId();
#endif

class EEThreadId
{
    uint64_t m_uiId;
public:

    EEThreadId() : m_uiId{0} {}
    EEThreadId(const EEThreadId& other) //TODO: kill, should be subsumed by below
        : m_uiId{other.m_uiId} {}
    EEThreadId(const volatile EEThreadId& other)
        : m_uiId{other.m_uiId} {}
    EEThreadId(const volatile EEThreadId&& other)
        : m_uiId{other.m_uiId} {}

    void operator=(const volatile EEThreadId& other) volatile {
        m_uiId = other.m_uiId;
    }
    void operator=(const volatile EEThreadId&& other) volatile {
        m_uiId = other.m_uiId;
    }

    bool operator==(const volatile EEThreadId& other) const volatile {
        return m_uiId == other.m_uiId;
    }
    bool operator==(const volatile EEThreadId&& other) const volatile {
        return m_uiId == other.m_uiId;
    }

    bool IsCurrentThread()
    {
        return m_uiId == ::GetCurrentThreadId();
    }

    void SetToCurrentThread()
    {
        m_uiId = ::GetCurrentThreadId();
    }

    void Clear()
    {
        m_uiId = 0;
    }

    bool IsValid() const volatile
    {
        return m_uiId != 0;
    }
};

#endif // PLATFORM_UNIX

#ifndef _INC_WINDOWS

#ifdef PLATFORM_UNIX

typedef struct _RTL_CRITICAL_SECTION {
    pthread_mutex_t mutex;
} CRITICAL_SECTION, RTL_CRITICAL_SECTION, *PRTL_CRITICAL_SECTION;

#else

#pragma pack(push, 8)

typedef struct _RTL_CRITICAL_SECTION {
    void* DebugInfo;

    //
    //  The following three fields control entering and exiting the critical
    //  section for the resource
    //

    int32_t LockCount;
    int32_t RecursionCount;
    HANDLE OwningThread;        // from the thread's ClientId->UniqueThread
    HANDLE LockSemaphore;
    uintptr_t SpinCount;        // force size on 64-bit systems when packed
} CRITICAL_SECTION, RTL_CRITICAL_SECTION, *PRTL_CRITICAL_SECTION;

#pragma pack(pop)

#endif

#endif // _INC_WINDOWS

#endif // __GCENV_STRUCTS_INCLUDED__
