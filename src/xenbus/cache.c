/* Copyright (c) Citrix Systems Inc.
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, 
 * with or without modification, are permitted provided 
 * that the following conditions are met:
 * 
 * *   Redistributions of source code must retain the above 
 *     copyright notice, this list of conditions and the 
 *     following disclaimer.
 * *   Redistributions in binary form must reproduce the above 
 *     copyright notice, this list of conditions and the 
 *     following disclaimer in the documentation and/or other 
 *     materials provided with the distribution.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND 
 * CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, 
 * INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF 
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE 
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR 
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, 
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, 
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR 
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS 
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, 
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING 
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE 
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF 
 * SUCH DAMAGE.
 */

#include <ntddk.h>
#include <ntstrsafe.h>
#include <stdlib.h>
#include <util.h>

#include "cache.h"
#include "dbg_print.h"
#include "assert.h"

extern ULONG
NTAPI
RtlRandomEx (
    __inout PULONG Seed
    );

typedef struct _OBJECT_HEADER {
    ULONG       Magic;

#define OBJECT_HEADER_MAGIC 0x02121996

    LIST_ENTRY  ListEntry;
} OBJECT_HEADER, *POBJECT_HEADER;

#define MAXIMUM_SLOTS   6

typedef struct _CACHE_MAGAZINE {
    PVOID   Slot[MAXIMUM_SLOTS];
} CACHE_MAGAZINE, *PCACHE_MAGAZINE;

typedef struct _CACHE_FIST {
    LONG    Defer;
    ULONG   Probability;
    ULONG   Seed;
} CACHE_FIST, *PCACHE_FIST;

#define MAXNAMELEN  128

struct _XENBUS_CACHE {
    LIST_ENTRY      ListEntry;
    CHAR            Name[MAXNAMELEN];
    ULONG           Size;
    ULONG           Reservation;
    NTSTATUS        (*Ctor)(PVOID, PVOID);
    VOID            (*Dtor)(PVOID, PVOID);
    VOID            (*AcquireLock)(PVOID);
    VOID            (*ReleaseLock)(PVOID);
    PVOID           Argument;
    LIST_ENTRY      GetList;
    PLIST_ENTRY     PutList;
    CACHE_MAGAZINE  Magazine[MAXIMUM_PROCESSORS];
    LONG            Allocated;
    LONG            MaximumAllocated;
    LONG            Population;
    LONG            MinimumPopulation;
    CACHE_FIST      FIST;
};

struct _XENBUS_CACHE_CONTEXT {
    LONG                            References;
    PXENBUS_DEBUG_INTERFACE         DebugInterface;
    PXENBUS_DEBUG_CALLBACK          DebugCallback;
    PXENBUS_STORE_INTERFACE         StoreInterface;
    KSPIN_LOCK                      Lock;
    LIST_ENTRY                      List;
    KTIMER                          Timer;
    KDPC                            Dpc;
};

#define CACHE_TAG   'HCAC'

static FORCEINLINE PVOID
__CacheAllocate(
    IN  ULONG   Length
    )
{
    return __AllocateNonPagedPoolWithTag(Length, CACHE_TAG);
}

static FORCEINLINE VOID
__CacheFree(
    IN  PVOID   Buffer
    )
{
    __FreePoolWithTag(Buffer, CACHE_TAG);
}

static FORCEINLINE VOID
__CacheFill(
    IN  PXENBUS_CACHE   Cache,
    IN  PLIST_ENTRY     List
    )
{
    // Not really a doubly-linked list; it's actually a singly-linked
    // list via the Flink field.
    while (List != NULL) {
        PLIST_ENTRY     Next;
        POBJECT_HEADER  Header;

        Next = List->Flink;
        List->Flink = NULL;
        ASSERT3P(List->Blink, ==, NULL);

        Header = CONTAINING_RECORD(List, OBJECT_HEADER, ListEntry);
        ASSERT3U(Header->Magic, ==, OBJECT_HEADER_MAGIC);

        InsertTailList(&Cache->GetList, &Header->ListEntry);

        List = Next;
    }
}

static FORCEINLINE VOID
__CacheSwizzle(
    IN  PXENBUS_CACHE   Cache
    )
{
    PLIST_ENTRY         List;

    List = InterlockedExchangePointer(&Cache->PutList, NULL);

    __CacheFill(Cache, List);
}

static FORCEINLINE NTSTATUS
__CacheCreateObject(
    IN  PXENBUS_CACHE   Cache,
    OUT POBJECT_HEADER  *Header
    )
{
    PVOID               Object;
    NTSTATUS            status;

    (*Header) = __CacheAllocate(sizeof (OBJECT_HEADER) + Cache->Size);

    status = STATUS_NO_MEMORY;
    if (*Header == NULL)
        goto fail1;

    (*Header)->Magic = OBJECT_HEADER_MAGIC;

    Object = (*Header) + 1;

    status = Cache->Ctor(Cache->Argument, Object);
    if (!NT_SUCCESS(status))
        goto fail2;

    return STATUS_SUCCESS;

fail2:
    Error("fail2\n");

    (*Header)->Magic = 0;

    ASSERT(IsZeroMemory(*Header, sizeof (OBJECT_HEADER)));
    __CacheFree(*Header);

fail1:
    Error("fail1 (%08x)\n", status);

    return status;    
}

static FORCEINLINE PVOID
__CacheGetShared(
    IN  PXENBUS_CACHE   Cache,
    IN  BOOLEAN         Locked
    )
{
    LONG                Population;
    POBJECT_HEADER      Header;
    PVOID               Object;
    LONG                Allocated;
    NTSTATUS            status;

    Population = InterlockedDecrement(&Cache->Population);

    if (Population >= 0) {
        PLIST_ENTRY     ListEntry;

        if (!Locked)
            Cache->AcquireLock(Cache->Argument);

        if (Population < Cache->MinimumPopulation)
            Cache->MinimumPopulation = Population;

        if (IsListEmpty(&Cache->GetList))
            __CacheSwizzle(Cache);

        ListEntry = RemoveHeadList(&Cache->GetList);
        ASSERT(ListEntry != &Cache->GetList);

        if (!Locked)
            Cache->ReleaseLock(Cache->Argument);

        RtlZeroMemory(ListEntry, sizeof (LIST_ENTRY));

        Header = CONTAINING_RECORD(ListEntry, OBJECT_HEADER, ListEntry);
        ASSERT3U(Header->Magic, ==, OBJECT_HEADER_MAGIC);

        goto done;
    }

    (VOID) InterlockedIncrement(&Cache->Population);

    status = __CacheCreateObject(Cache, &Header);
    if (!NT_SUCCESS(status))
        goto fail1;

    Allocated = InterlockedIncrement(&Cache->Allocated);

    if (Allocated > Cache->MaximumAllocated) {
        if (!Locked)
            Cache->AcquireLock(Cache->Argument);

        if (Allocated > Cache->MaximumAllocated)
            Cache->MaximumAllocated = Allocated;

        if (!Locked)
            Cache->ReleaseLock(Cache->Argument);
    }

done:
    Object = Header + 1;

    return Object;

fail1:
    Error("fail1 (%08x)\n", status);

    return NULL;    
}

static FORCEINLINE VOID
__CachePutShared(
    IN  PXENBUS_CACHE   Cache,
    IN  PVOID           Object,
    IN  BOOLEAN         Locked
    )
{
    POBJECT_HEADER      Header;
    PLIST_ENTRY         Old;
    PLIST_ENTRY         New;

    ASSERT(Object != NULL);

    Header = Object;
    --Header;
    ASSERT3U(Header->Magic, ==, OBJECT_HEADER_MAGIC);

    ASSERT(IsZeroMemory(&Header->ListEntry, sizeof (LIST_ENTRY)));

    if (!Locked) {
        New = &Header->ListEntry;

        do {
            Old = Cache->PutList;
            New->Flink = Old;
        } while (InterlockedCompareExchangePointer(&Cache->PutList, New, Old) != Old);
    } else {
        InsertTailList(&Cache->GetList, &Header->ListEntry);
    }

    KeMemoryBarrier();

    (VOID) InterlockedIncrement(&Cache->Population);
}

static FORCEINLINE PVOID
__CacheGetMagazine(
    IN  PXENBUS_CACHE   Cache,
    IN  ULONG           Cpu
    )
{
    PCACHE_MAGAZINE     Magazine;
    ULONG               Index;

    Magazine = &Cache->Magazine[Cpu];

    for (Index = 0; Index < MAXIMUM_SLOTS; Index++) {
        PVOID   Object;

        if (Magazine->Slot[Index] != NULL) {
            Object = Magazine->Slot[Index];
            Magazine->Slot[Index] = NULL;

            return Object;
        }
    }

    return NULL;
}

static FORCEINLINE BOOLEAN
__CachePutMagazine(
    IN  PXENBUS_CACHE   Cache,
    IN  ULONG           Cpu,
    IN  PVOID           Object
    )
{
    PCACHE_MAGAZINE      Magazine;
    ULONG               Index;

    Magazine = &Cache->Magazine[Cpu];

    for (Index = 0; Index < MAXIMUM_SLOTS; Index++) {
        if (Magazine->Slot[Index] == NULL) {
            Magazine->Slot[Index] = Object;
            return TRUE;
        }
    }

    return FALSE;
}

static PVOID
CacheGet(
    IN  PXENBUS_CACHE_CONTEXT   Context,
    IN  PXENBUS_CACHE           Cache,
    IN  BOOLEAN                 Locked
    )
{
    KIRQL                       Irql;
    ULONG                       Cpu;
    PVOID                       Object;

    UNREFERENCED_PARAMETER(Context);

    if (Cache->FIST.Probability != 0) {
        LONG    Defer;

        Defer = InterlockedDecrement(&Cache->FIST.Defer);

        if (Defer <= 0) {
            ULONG   Random = RtlRandomEx(&Cache->FIST.Seed);
            ULONG   Threshold = (MAXLONG / 100) * Cache->FIST.Probability;

            if (Random < Threshold)
                return NULL;
        }
    }

    KeRaiseIrql(DISPATCH_LEVEL, &Irql);
    Cpu = KeGetCurrentProcessorNumber();

    Object = __CacheGetMagazine(Cache, Cpu);
    if (Object == NULL)
        Object = __CacheGetShared(Cache, Locked);

    KeLowerIrql(Irql);

    return Object;
}

static VOID
CachePut(
    IN  PXENBUS_CACHE_CONTEXT   Context,
    IN  PXENBUS_CACHE           Cache,
    IN  PVOID                   Object,
    IN  BOOLEAN                 Locked
    )
{
    KIRQL                       Irql;
    ULONG                       Cpu;

    UNREFERENCED_PARAMETER(Context);

    KeRaiseIrql(DISPATCH_LEVEL, &Irql);
    Cpu = KeGetCurrentProcessorNumber();

    if (!__CachePutMagazine(Cache, Cpu, Object))
        __CachePutShared(Cache, Object, Locked);

    KeLowerIrql(Irql);
}

static FORCEINLINE
__CacheFlushMagazines(
    IN  PXENBUS_CACHE   Cache
    )
{
    ULONG               Cpu;

    for (Cpu = 0; Cpu < MAXIMUM_PROCESSORS; Cpu++) {
        PVOID   Object;

        while ((Object = __CacheGetMagazine(Cache, Cpu)) != NULL)
            __CachePutShared(Cache, Object, TRUE);
    }
}

static FORCEINLINE VOID
__CacheTrimShared(
    IN      PXENBUS_CACHE   Cache,
    IN OUT  PLIST_ENTRY     List
    )
{
    LONG                    Population;
    LONG                    Excess;

    Population = Cache->Population;

    KeMemoryBarrier();

    Excess = __max((LONG)Cache->MinimumPopulation - (LONG)Cache->Reservation, 0);
    
    while (Excess != 0) {
        PLIST_ENTRY     ListEntry;

        Population = InterlockedDecrement(&Cache->Population);
        if (Population < 0) {
            Population = InterlockedIncrement(&Cache->Population);
            break;
        }

        if (IsListEmpty(&Cache->GetList))
            __CacheSwizzle(Cache);

        ListEntry = RemoveHeadList(&Cache->GetList);
        ASSERT(ListEntry != &Cache->GetList);

        InsertTailList(List, ListEntry);

        InterlockedDecrement(&Cache->Allocated);
        --Excess;
    }

    Cache->MinimumPopulation = Population;
}

static FORCEINLINE VOID
__CacheDestroyObject(
    IN  PXENBUS_CACHE   Cache,
    IN  POBJECT_HEADER  Header
    )
{
    PVOID               Object;

    Object = Header + 1;

    Cache->Dtor(Cache->Argument, Object);

    Header->Magic = 0;

    ASSERT(IsZeroMemory(Header, sizeof (OBJECT_HEADER)));
    __CacheFree(Header);
}

static FORCEINLINE VOID
__CacheEmpty(
    IN      PXENBUS_CACHE   Cache,
    IN OUT  PLIST_ENTRY     List
    )
{
    while (!IsListEmpty(List)) {
        PLIST_ENTRY     ListEntry;
        POBJECT_HEADER  Header;

        ListEntry = RemoveHeadList(List);
        RtlZeroMemory(ListEntry, sizeof (LIST_ENTRY));

        Header = CONTAINING_RECORD(ListEntry, OBJECT_HEADER, ListEntry);
        ASSERT3U(Header->Magic, ==, OBJECT_HEADER_MAGIC);

        __CacheDestroyObject(Cache, Header);
    }
}

#define TIME_US(_us)        ((_us) * 10)
#define TIME_MS(_ms)        (TIME_US((_ms) * 1000))
#define TIME_RELATIVE(_t)   (-(_t))

#define CACHE_PERIOD  1000

KDEFERRED_ROUTINE   CacheDpc;

VOID
CacheDpc(
    IN  PKDPC       Dpc,
    IN  PVOID       _Context,
    IN  PVOID       Argument1,
    IN  PVOID       Argument2
    )
{
    PXENBUS_CACHE_CONTEXT   Context = _Context;
    PLIST_ENTRY             Entry;
    KIRQL                   Irql;

    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(Argument1);
    UNREFERENCED_PARAMETER(Argument2);
    
    ASSERT(Context != NULL);

    KeAcquireSpinLock(&Context->Lock, &Irql);

    for (Entry = Context->List.Flink;
         Entry != &Context->List;
         Entry = Entry->Flink) {
        PXENBUS_CACHE   Cache = CONTAINING_RECORD(Entry, XENBUS_CACHE, ListEntry);
        LIST_ENTRY      List;

        InitializeListHead(&List);

        Cache->AcquireLock(Cache->Argument);
        __CacheTrimShared(Cache, &List);
        Cache->ReleaseLock(Cache->Argument);

        __CacheEmpty(Cache, &List);
        ASSERT(IsListEmpty(&List));
    }

    KeReleaseSpinLock(&Context->Lock, Irql);
}

static FORCEINLINE VOID
__CacheGetFISTEntries(
    IN  PXENBUS_CACHE_CONTEXT   Context,
    IN  PXENBUS_CACHE           Cache
    )
{
    CHAR                        Node[sizeof ("fist/cache/") + MAXNAMELEN];
    PCHAR                       Buffer;
    LARGE_INTEGER               Now;
    NTSTATUS                    status;

    status = RtlStringCbPrintfA(Node,
                                sizeof (Node),
                                "fist/cache/%s",
                                Cache->Name);
    ASSERT(NT_SUCCESS(status));

    status = STORE(Read,
                   Context->StoreInterface,
                   NULL,
                   Node,
                   "defer",
                   &Buffer);
    if (!NT_SUCCESS(status)) {
        Cache->FIST.Defer = 0;
    } else {
        Cache->FIST.Defer = (ULONG)strtol(Buffer, NULL, 0);

        STORE(Free,
              Context->StoreInterface,
              Buffer);
    }

    status = STORE(Read,
                   Context->StoreInterface,
                   NULL,
                   Node,
                   "probability",
                   &Buffer);
    if (!NT_SUCCESS(status)) {
        Cache->FIST.Probability = 0;
    } else {
        Cache->FIST.Probability = (ULONG)strtol(Buffer, NULL, 0);

        STORE(Free,
              Context->StoreInterface,
              Buffer);
    }

    if (Cache->FIST.Probability > 100)
        Cache->FIST.Probability = 100;

    if (Cache->FIST.Probability != 0)
        Info("%s: Defer = %d Probability = %d\n",
             Cache->Name,
             Cache->FIST.Defer,
             Cache->FIST.Probability);

    KeQuerySystemTime(&Now);
    Cache->FIST.Seed = Now.LowPart;
}

static NTSTATUS
CacheCreate(
    IN  PXENBUS_CACHE_CONTEXT   Context,
    IN  const CHAR              *Name,
    IN  ULONG                   Size,
    IN  ULONG                   Reservation,
    IN  NTSTATUS                (*Ctor)(PVOID, PVOID),
    IN  VOID                    (*Dtor)(PVOID, PVOID),
    IN  VOID                    (*AcquireLock)(PVOID),
    IN  VOID                    (*ReleaseLock)(PVOID),
    IN  PVOID                   Argument,
    OUT PXENBUS_CACHE           *Cache
    )
{
    LIST_ENTRY                  List;
    KIRQL                       Irql;
    NTSTATUS                    status;

    Trace("====> (%s)\n", Name);

    *Cache = __CacheAllocate(sizeof (XENBUS_CACHE));

    status = STATUS_NO_MEMORY;
    if (*Cache == NULL)
        goto fail1;

    status = RtlStringCbPrintfA((*Cache)->Name,
                                sizeof ((*Cache)->Name),
                                "%s",
                                Name);
    if (!NT_SUCCESS(status))
        goto fail2;

    (*Cache)->Size = Size;
    (*Cache)->Ctor = Ctor;
    (*Cache)->Dtor = Dtor;
    (*Cache)->AcquireLock = AcquireLock;
    (*Cache)->ReleaseLock = ReleaseLock;
    (*Cache)->Argument = Argument;

    __CacheGetFISTEntries(Context, *Cache);

    InitializeListHead(&(*Cache)->GetList);

    while (Reservation != 0) {
        POBJECT_HEADER  Header;

        status = __CacheCreateObject(*Cache, &Header);
        if (!NT_SUCCESS(status))
            goto fail3;

        (VOID) InterlockedIncrement(&(*Cache)->Allocated);

        InsertTailList(&(*Cache)->GetList, &Header->ListEntry);
        (VOID) InterlockedIncrement(&(*Cache)->Population);

        --Reservation;
    }
    (*Cache)->MaximumAllocated = (*Cache)->Allocated;
    (*Cache)->Reservation = (*Cache)->Population;

    KeAcquireSpinLock(&Context->Lock, &Irql);
    InsertTailList(&Context->List, &(*Cache)->ListEntry);
    KeReleaseSpinLock(&Context->Lock, Irql);

    Trace("<====\n");

    return STATUS_SUCCESS;

fail3:
    Error("fail3\n");

    InitializeListHead(&List);

    (*Cache)->MinimumPopulation = (*Cache)->Population;
    __CacheTrimShared(*Cache, &List);
    __CacheEmpty(*Cache, &List);

    ASSERT3U((*Cache)->Population, ==, 0);
    ASSERT3U((*Cache)->Allocated, ==, 0);

    RtlZeroMemory(&(*Cache)->GetList, sizeof (LIST_ENTRY));

    RtlZeroMemory(&(*Cache)->FIST, sizeof (CACHE_FIST));

    (*Cache)->Argument = NULL;
    (*Cache)->ReleaseLock = NULL;
    (*Cache)->AcquireLock = NULL;
    (*Cache)->Dtor = NULL;
    (*Cache)->Ctor = NULL;
    (*Cache)->Size = 0;

fail2:
    Error("fail2\n");

    RtlZeroMemory((*Cache)->Name, sizeof ((*Cache)->Name));
    
    ASSERT(IsZeroMemory(*Cache, sizeof (XENBUS_CACHE)));
    __CacheFree(*Cache);

fail1:
    Error("fail1 (%08x)\n", status);

    return status;    
}

static VOID
CacheDestroy(
    IN  PXENBUS_CACHE_CONTEXT   Context,
    IN  PXENBUS_CACHE           Cache
    )
{
    KIRQL                       Irql;
    LIST_ENTRY                  List;

    Trace("====> (%s)\n", Cache->Name);

    KeAcquireSpinLock(&Context->Lock, &Irql);
    RemoveEntryList(&Cache->ListEntry);
    KeReleaseSpinLock(&Context->Lock, Irql);

    RtlZeroMemory(&Cache->ListEntry, sizeof (LIST_ENTRY));

    Cache->Reservation = 0;
    Cache->MaximumAllocated = 0;

    InitializeListHead(&List);

    __CacheFlushMagazines(Cache);

    Cache->MinimumPopulation = Cache->Population;
    __CacheTrimShared(Cache, &List);
    __CacheEmpty(Cache, &List);

    ASSERT3U(Cache->Population, ==, 0);
    ASSERT3U(Cache->Allocated, ==, 0);

    RtlZeroMemory(&Cache->GetList, sizeof (LIST_ENTRY));

    RtlZeroMemory(&Cache->FIST, sizeof (CACHE_FIST));

    Cache->Argument = NULL;
    Cache->ReleaseLock = NULL;
    Cache->AcquireLock = NULL;
    Cache->Dtor = NULL;
    Cache->Ctor = NULL;
    Cache->Size = 0;

    RtlZeroMemory(Cache->Name, sizeof (Cache->Name));

    ASSERT(IsZeroMemory(Cache, sizeof (XENBUS_CACHE)));
    __CacheFree(Cache);

    Trace("<====\n");
}

static VOID
CacheAcquire(
    IN  PXENBUS_CACHE_CONTEXT Context
    )
{
    InterlockedIncrement(&Context->References);
}

static VOID
CacheRelease(
    IN  PXENBUS_CACHE_CONTEXT Context
    )
{
    ASSERT(Context->References != 0);
    InterlockedDecrement(&Context->References);
}

#define CACHE_OPERATION(_Type, _Name, _Arguments) \
        Cache ## _Name,

static XENBUS_CACHE_OPERATIONS  Operations = {
    DEFINE_CACHE_OPERATIONS
};

#undef CACHE_OPERATION

static VOID
CacheDebugCallback(
    IN  PVOID               Argument,
    IN  BOOLEAN             Crashing
    )
{
    PXENBUS_CACHE_CONTEXT   Context = Argument;

    UNREFERENCED_PARAMETER(Crashing);

    if (!IsListEmpty(&Context->List)) {
        PLIST_ENTRY ListEntry;

        DEBUG(Printf,
              Context->DebugInterface,
              Context->DebugCallback,
              "CACHES:\n");

        for (ListEntry = Context->List.Flink;
             ListEntry != &Context->List;
             ListEntry = ListEntry->Flink) {
            PXENBUS_CACHE   Cache;

            Cache = CONTAINING_RECORD(ListEntry, XENBUS_CACHE, ListEntry);

            DEBUG(Printf,
                  Context->DebugInterface,
                  Context->DebugCallback,
                  "- %s: Allocated = %d (Max = %d) Population = %d (Min = %d)\n",
                  Cache->Name,
                  Cache->Allocated,
                  Cache->MaximumAllocated,
                  Cache->Population,
                  Cache->MinimumPopulation);
        }
    }
}

NTSTATUS
CacheInitialize(
    IN  PXENBUS_FDO             Fdo,
    OUT PXENBUS_CACHE_INTERFACE Interface
    )
{
    PXENBUS_CACHE_CONTEXT       Context;
    NTSTATUS                    status;
    LARGE_INTEGER               Timeout;

    Trace("====>\n");

    Context = __CacheAllocate(sizeof (XENBUS_CACHE_CONTEXT));

    status = STATUS_NO_MEMORY;
    if (Context == NULL)
        goto fail1;

    InitializeListHead(&Context->List);
    KeInitializeSpinLock(&Context->Lock);

    Context->StoreInterface = FdoGetStoreInterface(Fdo);

    STORE(Acquire, Context->StoreInterface);

    Context->DebugInterface = FdoGetDebugInterface(Fdo);

    DEBUG(Acquire, Context->DebugInterface);

    status = DEBUG(Register,
                   Context->DebugInterface,
                   __MODULE__ "|CACHE",
                   CacheDebugCallback,
                   Context,
                   &Context->DebugCallback);
    if (!NT_SUCCESS(status))
        goto fail2;

    KeInitializeDpc(&Context->Dpc,
                    CacheDpc,
                    Context);

    Timeout.QuadPart = TIME_RELATIVE(TIME_MS(CACHE_PERIOD));

    KeInitializeTimer(&Context->Timer);
    KeSetTimerEx(&Context->Timer,
                 Timeout,
                 CACHE_PERIOD,
                 &Context->Dpc);

    Interface->Context = Context;
    Interface->Operations = &Operations;

    Trace("<====\n");

    return STATUS_SUCCESS;

fail2:
    Error("fail2\n");

    DEBUG(Release, Context->DebugInterface);
    Context->DebugInterface = NULL;

    STORE(Release, Context->StoreInterface);
    Context->StoreInterface = NULL;

    RtlZeroMemory(&Context->Lock, sizeof (KSPIN_LOCK));
    RtlZeroMemory(&Context->List, sizeof (LIST_ENTRY));

    ASSERT(IsZeroMemory(Context, sizeof (XENBUS_CACHE_CONTEXT)));
    __CacheFree(Context);

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

VOID
CacheTeardown(
    IN OUT  PXENBUS_CACHE_INTERFACE Interface
    )
{
    PXENBUS_CACHE_CONTEXT           Context = Interface->Context;

    Trace("====>\n");

    KeCancelTimer(&Context->Timer);
    KeFlushQueuedDpcs();

    RtlZeroMemory(&Context->Timer, sizeof (KTIMER));
    RtlZeroMemory(&Context->Dpc, sizeof (KDPC));

    if (!IsListEmpty(&Context->List))
        BUG("OUTSTANDING CACHES");

    DEBUG(Deregister,
          Context->DebugInterface,
          Context->DebugCallback);
    Context->DebugCallback = NULL;

    DEBUG(Release, Context->DebugInterface);
    Context->DebugInterface = NULL;

    STORE(Release, Context->StoreInterface);
    Context->StoreInterface = NULL;

    RtlZeroMemory(&Context->Lock, sizeof (KSPIN_LOCK));
    RtlZeroMemory(&Context->List, sizeof (LIST_ENTRY));

    ASSERT(IsZeroMemory(Context, sizeof (XENBUS_CACHE_CONTEXT)));
    __CacheFree(Context);

    RtlZeroMemory(Interface, sizeof (XENBUS_CACHE_INTERFACE));

    Trace("<====\n");
}




