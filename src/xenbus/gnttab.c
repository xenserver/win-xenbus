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
#include <xen.h>
#include <util.h>

#include "gnttab.h"
#include "fdo.h"
#include "range_set.h"
#include "dbg_print.h"
#include "assert.h"

#define GNTTAB_MAXIMUM_FRAME_COUNT  32
#define GNTTAB_ENTRY_PER_FRAME      (PAGE_SIZE / sizeof (grant_entry_v1_t))

// Xen requires that we avoid the first 8 entries of the table and
// we also reserve 1 entry for the crash kernel
#define GNTTAB_RESERVED_ENTRY_COUNT 9

#define GNTTAB_DESCRIPTOR_MAGIC 'DTNG'

#define MAXNAMELEN  128

struct _XENBUS_GNTTAB_CACHE {
    PXENBUS_GNTTAB_CONTEXT  Context;
    CHAR                    Name[MAXNAMELEN];
    VOID                    (*AcquireLock)(PVOID);
    VOID                    (*ReleaseLock)(PVOID);
    PVOID                   Argument;
    PXENBUS_CACHE           Cache;
};

struct _XENBUS_GNTTAB_DESCRIPTOR {
    ULONG               Magic;
    ULONG               Reference;
    grant_entry_v1_t    Entry;
};

struct _XENBUS_GNTTAB_CONTEXT {
    LONG                        References;
    PFN_NUMBER                  Pfn;
    LONG                        FrameIndex;
    grant_entry_v1_t            *Entry;
    PXENBUS_RANGE_SET           RangeSet;
    PXENBUS_CACHE_INTERFACE     CacheInterface;
    PXENBUS_SUSPEND_INTERFACE   SuspendInterface;
    PXENBUS_SUSPEND_CALLBACK    SuspendCallbackEarly;
    PXENBUS_DEBUG_INTERFACE     DebugInterface;
    PXENBUS_DEBUG_CALLBACK      DebugCallback;
};

#define GNTTAB_TAG  'TTNG'

static FORCEINLINE PVOID
__GnttabAllocate(
    IN  ULONG   Length
    )
{
    return __AllocateNonPagedPoolWithTag(Length, GNTTAB_TAG);
}

static FORCEINLINE VOID
__GnttabFree(
    IN  PVOID   Buffer
    )
{
    __FreePoolWithTag(Buffer, GNTTAB_TAG);
}

static FORCEINLINE NTSTATUS
__GnttabExpand(
    IN  PXENBUS_GNTTAB_CONTEXT  Context
    )
{
    LONG                        FrameIndex;
    PFN_NUMBER                  Pfn;
    LONGLONG                    Start;
    LONGLONG                    End;
    NTSTATUS                    status;

    FrameIndex = InterlockedIncrement(&Context->FrameIndex);

    status = STATUS_INSUFFICIENT_RESOURCES;
    ASSERT3U(FrameIndex, <=, GNTTAB_MAXIMUM_FRAME_COUNT);
    if (FrameIndex == GNTTAB_MAXIMUM_FRAME_COUNT)
        goto fail1;

    Pfn = Context->Pfn + FrameIndex;

    status = MemoryAddToPhysmap(Pfn,
                                XENMAPSPACE_grant_table,
                                FrameIndex);
    ASSERT(NT_SUCCESS(status));

    Start = __max(GNTTAB_RESERVED_ENTRY_COUNT, FrameIndex * GNTTAB_ENTRY_PER_FRAME);
    End = ((FrameIndex + 1) * GNTTAB_ENTRY_PER_FRAME) - 1;

    Info("adding refrences [%08llx - %08llx]\n", Start, End);

    RangeSetPut(Context->RangeSet, Start, End);

    return STATUS_SUCCESS;

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

static FORCEINLINE VOID
__GnttabShrink(
    IN  PXENBUS_GNTTAB_CONTEXT  Context
    )
{
    LONGLONG                    Entry;

    for (Entry = GNTTAB_RESERVED_ENTRY_COUNT;
         Entry < (LONGLONG)((Context->FrameIndex + 1) * GNTTAB_ENTRY_PER_FRAME);
         Entry++) {
        NTSTATUS    status;

        status = RangeSetGet(Context->RangeSet, Entry);
        ASSERT(NT_SUCCESS(status));
    }

    Context->FrameIndex = -1;
}

static NTSTATUS
GnttabDescriptorCtor(
    IN  PVOID                   Argument,
    IN  PVOID                   Object
    )
{
    PXENBUS_GNTTAB_CACHE        Cache = Argument;
    PXENBUS_GNTTAB_CONTEXT      Context = Cache->Context;
    PXENBUS_GNTTAB_DESCRIPTOR   Descriptor = Object;
    LONGLONG                    Reference;
    NTSTATUS                    status;

    if (!RangeSetIsEmpty(Context->RangeSet))
        goto done;

    status = __GnttabExpand(Context);
    if (!NT_SUCCESS(status))
        goto fail1;

done:
    status = RangeSetPop(Context->RangeSet, &Reference);
    if (!NT_SUCCESS(status))
        goto fail2;

    Descriptor->Magic = GNTTAB_DESCRIPTOR_MAGIC;
    Descriptor->Reference = (ULONG)Reference;

    return STATUS_SUCCESS;

fail2:
    Error("fail2\n");

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

static VOID
GnttabDescriptorDtor(
    IN  PVOID                   Argument,
    IN  PVOID                   Object
    )
{
    PXENBUS_GNTTAB_CACHE        Cache = Argument;
    PXENBUS_GNTTAB_CONTEXT      Context = Cache->Context;
    PXENBUS_GNTTAB_DESCRIPTOR   Descriptor = Object;
    NTSTATUS                    status;

    status = RangeSetPut(Context->RangeSet,
                         (LONGLONG)Descriptor->Reference,
                         (LONGLONG)Descriptor->Reference);
    ASSERT(NT_SUCCESS(status));
}

static VOID
GnttabAcquireLock(
    IN  PVOID                   Argument
    )
{
    PXENBUS_GNTTAB_CACHE        Cache = Argument;

    Cache->AcquireLock(Cache->Argument);
}

static VOID
GnttabReleaseLock(
    IN  PVOID                   Argument
    )
{
    PXENBUS_GNTTAB_CACHE        Cache = Argument;

    Cache->ReleaseLock(Cache->Argument);
}

static NTSTATUS
GnttabCreateCache(
    IN  PXENBUS_GNTTAB_CONTEXT  Context,
    IN  const CHAR              *Name,
    IN  ULONG                   Reservation,
    IN  VOID                    (*AcquireLock)(PVOID),
    IN  VOID                    (*ReleaseLock)(PVOID),
    IN  PVOID                   Argument,
    OUT PXENBUS_GNTTAB_CACHE    *Cache
    )
{
    NTSTATUS                    status;

    *Cache = __GnttabAllocate(sizeof (XENBUS_GNTTAB_CACHE));

    status = STATUS_NO_MEMORY;
    if (*Cache == NULL)
        goto fail1;

    (*Cache)->Context = Context;

    status = RtlStringCbPrintfA((*Cache)->Name,
                                sizeof ((*Cache)->Name),
                                "%s_gnttab",
                                Name);
    if (!NT_SUCCESS(status))
        goto fail2;

    (*Cache)->AcquireLock = AcquireLock;
    (*Cache)->ReleaseLock = ReleaseLock;
    (*Cache)->Argument = Argument;

    status = CACHE(Create,
                   Context->CacheInterface,
                   (*Cache)->Name,
                   sizeof (XENBUS_GNTTAB_DESCRIPTOR),
                   Reservation,
                   GnttabDescriptorCtor,
                   GnttabDescriptorDtor,
                   GnttabAcquireLock,
                   GnttabReleaseLock,
                   *Cache,
                   &(*Cache)->Cache);
    if (!NT_SUCCESS(status))
        goto fail3;

    return STATUS_SUCCESS;

fail3:
    Error("fail3\n");

    (*Cache)->Argument = NULL;
    (*Cache)->ReleaseLock = NULL;
    (*Cache)->AcquireLock = NULL;

    RtlZeroMemory((*Cache)->Name, sizeof ((*Cache)->Name));
    
fail2:
    Error("fail2\n");

    (*Cache)->Context = NULL;

    ASSERT(IsZeroMemory(*Cache, sizeof (XENBUS_GNTTAB_CACHE)));
    __GnttabFree(*Cache);

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

static VOID
GnttabDestroyCache(
    IN  PXENBUS_GNTTAB_CONTEXT  Context,
    IN  PXENBUS_GNTTAB_CACHE    Cache
    )
{
    CACHE(Destroy,
          Context->CacheInterface,
          Cache->Cache);
    Cache->Cache = NULL;

    Cache->Argument = NULL;
    Cache->ReleaseLock = NULL;
    Cache->AcquireLock = NULL;

    RtlZeroMemory(Cache->Name, sizeof (Cache->Name));
    
    Cache->Context = NULL;

    ASSERT(IsZeroMemory(Cache, sizeof (XENBUS_GNTTAB_CACHE)));
    __GnttabFree(Cache);
}

static NTSTATUS
GnttabPermitForeignAccess( 
    IN  PXENBUS_GNTTAB_CONTEXT      Context,
    IN  PXENBUS_GNTTAB_CACHE        Cache,
    IN  BOOLEAN                     Locked,
    IN  USHORT                      Domain,
    IN  PFN_NUMBER                  Pfn,
    IN  BOOLEAN                     ReadOnly,
    OUT PXENBUS_GNTTAB_DESCRIPTOR   *Descriptor
    )
{
    grant_entry_v1_t                *Entry;
    NTSTATUS                        status;

    *Descriptor = CACHE(Get,
                        Context->CacheInterface,
                        Cache->Cache,
                        Locked);

    status = STATUS_INSUFFICIENT_RESOURCES;
    if (*Descriptor == NULL)
        goto fail1;

    (*Descriptor)->Entry.flags = (ReadOnly) ? GTF_readonly : 0;
    (*Descriptor)->Entry.domid = Domain;

    (*Descriptor)->Entry.frame = (uint32_t)Pfn;
    ASSERT3U((*Descriptor)->Entry.frame, ==, Pfn);

    Entry = &Context->Entry[(*Descriptor)->Reference];

    *Entry = (*Descriptor)->Entry;
    KeMemoryBarrier();

    Entry->flags |= GTF_permit_access;
    KeMemoryBarrier();

    return STATUS_SUCCESS;

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

static NTSTATUS
GnttabRevokeForeignAccess(
    IN  PXENBUS_GNTTAB_CONTEXT      Context,
    IN  PXENBUS_GNTTAB_CACHE        Cache,
    IN  BOOLEAN                     Locked,
    IN  PXENBUS_GNTTAB_DESCRIPTOR   Descriptor
    )
{
    grant_entry_v1_t                *Entry;
    volatile SHORT                  *Flags;
    ULONG                           Attempt;
    NTSTATUS                        status;

    ASSERT3U(Descriptor->Magic, ==, GNTTAB_DESCRIPTOR_MAGIC);
    ASSERT3U(Descriptor->Reference, >=, GNTTAB_RESERVED_ENTRY_COUNT);
    ASSERT3U(Descriptor->Reference, <, (Context->FrameIndex + 1) * GNTTAB_ENTRY_PER_FRAME);

    Entry = &Context->Entry[Descriptor->Reference];
    Flags = (volatile SHORT *)&Entry->flags;

    Attempt = 0;
    while (Attempt++ < 100) {
        uint16_t    Old;
        uint16_t    New;

        Old = *Flags;
        Old &= ~(GTF_reading | GTF_writing);

        New = Old & ~GTF_permit_access;

        if (InterlockedCompareExchange16(Flags, New, Old) == Old)
            break;

        SchedYield();
    }

    status = STATUS_UNSUCCESSFUL;
    if (Attempt == 100)
        goto fail1;

    RtlZeroMemory(Entry, sizeof (grant_entry_v1_t));
    RtlZeroMemory(&Descriptor->Entry, sizeof (grant_entry_v1_t));

    CACHE(Put,
          Context->CacheInterface,
          Cache->Cache,
          Descriptor,
          Locked);

    return STATUS_SUCCESS;

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

static ULONG
GnttabReference(
    IN  PXENBUS_GNTTAB_CONTEXT      Context,
    IN  PXENBUS_GNTTAB_DESCRIPTOR   Descriptor
    )
{
    UNREFERENCED_PARAMETER(Context);

    ASSERT3U(Descriptor->Magic, ==, GNTTAB_DESCRIPTOR_MAGIC);

    return (ULONG)Descriptor->Reference;
}

static VOID
GnttabAcquire(
    IN  PXENBUS_GNTTAB_CONTEXT  Context
    )
{
    InterlockedIncrement(&Context->References);
}

static VOID
GnttabRelease(
    IN  PXENBUS_GNTTAB_CONTEXT  Context
    )
{
    ASSERT(Context->References != 0);
    InterlockedDecrement(&Context->References);
}

#define GNTTAB_OPERATION(_Type, _Name, _Arguments) \
        Gnttab ## _Name,

static XENBUS_GNTTAB_OPERATIONS  Operations = {
    DEFINE_GNTTAB_OPERATIONS
};

#undef GNTTAB_OPERATION

static FORCEINLINE VOID
__GnttabMap(
    IN  PXENBUS_GNTTAB_CONTEXT  Context
    )
{
    LONG                        Index;
    PFN_NUMBER                  Pfn;
    NTSTATUS                    status;

    Pfn = Context->Pfn;

    for (Index = 0; Index <= Context->FrameIndex; Index++) {
        status = MemoryAddToPhysmap(Pfn,
                                    XENMAPSPACE_grant_table,
                                    Index);
        ASSERT(NT_SUCCESS(status));

        Pfn++;
    }
}

static FORCEINLINE VOID
__GnttabUnmap(
    IN  PXENBUS_GNTTAB_CONTEXT  Context
    )
{
    ASSERT3S(Context->FrameIndex, ==, -1);

    // Not clear what to do here
}

static VOID
GnttabSuspendCallbackEarly(
    IN  PVOID               Argument
    )
{
    PXENBUS_GNTTAB_CONTEXT  Context = Argument;

    __GnttabMap(Context);
}
                     
static VOID
GnttabDebugCallback(
    IN  PVOID               Argument,
    IN  BOOLEAN             Crashing
    )
{
    PXENBUS_GNTTAB_CONTEXT  Context = Argument;

    UNREFERENCED_PARAMETER(Crashing);

    DEBUG(Printf,
          Context->DebugInterface,
          Context->DebugCallback,
          "Pfn = %p\n",
          (PVOID)Context->Pfn);
    
    DEBUG(Printf,
          Context->DebugInterface,
          Context->DebugCallback,
          "FrameIndex = %d\n",
          Context->FrameIndex);
}
                     
NTSTATUS
GnttabInitialize(
    IN  PXENBUS_FDO                 Fdo,
    OUT PXENBUS_GNTTAB_INTERFACE    Interface
    )
{
    PXENBUS_RESOURCE                Memory;
    PHYSICAL_ADDRESS                Address;
    PXENBUS_GNTTAB_CONTEXT          Context;
    NTSTATUS                        status;

    Trace("====>\n");

    Context = __GnttabAllocate(sizeof (XENBUS_GNTTAB_CONTEXT));

    status = STATUS_NO_MEMORY;
    if (Context == NULL)
        goto fail1;

    Memory = FdoGetResource(Fdo, MEMORY_RESOURCE);
    Context->Pfn = (PFN_NUMBER)(Memory->Translated.u.Memory.Start.QuadPart >> PAGE_SHIFT);
    Context->FrameIndex = -1;

    __GnttabMap(Context);

    Memory->Translated.u.Memory.Start.QuadPart += (GNTTAB_MAXIMUM_FRAME_COUNT * PAGE_SIZE);

    ASSERT3U(Memory->Translated.u.Memory.Length, >=, (GNTTAB_MAXIMUM_FRAME_COUNT * PAGE_SIZE));
    Memory->Translated.u.Memory.Length -= (GNTTAB_MAXIMUM_FRAME_COUNT * PAGE_SIZE);

    Address.QuadPart = (ULONGLONG)Context->Pfn << PAGE_SHIFT;
    Context->Entry = (grant_entry_v1_t *)MmMapIoSpace(Address,
                                                      GNTTAB_MAXIMUM_FRAME_COUNT * PAGE_SIZE,
                                                      MmCached);
    status = STATUS_UNSUCCESSFUL;
    if (Context->Entry == NULL)
        goto fail2;

    Info("grant_entry_v1_t *: %p\n", Context->Entry);

    status = RangeSetInitialize(&Context->RangeSet);
    if (!NT_SUCCESS(status))
        goto fail3;

    Context->CacheInterface = FdoGetCacheInterface(Fdo);

    CACHE(Acquire, Context->CacheInterface);

    Context->SuspendInterface = FdoGetSuspendInterface(Fdo);

    SUSPEND(Acquire, Context->SuspendInterface);

    status = SUSPEND(Register,
                     Context->SuspendInterface,
                     SUSPEND_CALLBACK_EARLY,
                     GnttabSuspendCallbackEarly,
                     Context,
                     &Context->SuspendCallbackEarly);
    if (!NT_SUCCESS(status))
        goto fail4;

    Context->DebugInterface = FdoGetDebugInterface(Fdo);

    DEBUG(Acquire, Context->DebugInterface);

    status = DEBUG(Register,
                   Context->DebugInterface,
                   __MODULE__ "|GNTTAB",
                   GnttabDebugCallback,
                   Context,
                   &Context->DebugCallback);
    if (!NT_SUCCESS(status))
        goto fail5;

    Interface->Context = Context;
    Interface->Operations = &Operations;

    Trace("<====\n");

    return STATUS_SUCCESS;

fail5:
    Error("fail5\n");

    DEBUG(Release, Context->DebugInterface);
    Context->DebugInterface = NULL;

    SUSPEND(Deregister,
            Context->SuspendInterface,
            Context->SuspendCallbackEarly);
    Context->SuspendCallbackEarly = NULL;

fail4:
    Error("fail4\n");

    SUSPEND(Release, Context->SuspendInterface);
    Context->SuspendInterface = NULL;

    CACHE(Release, Context->CacheInterface);
    Context->CacheInterface = NULL;

    RangeSetTeardown(Context->RangeSet);

fail3:
    Error("fail3\n");

    Context->Entry = NULL;

fail2:
    Error("fail2\n");

    __GnttabUnmap(Context);

    ASSERT3S(Context->FrameIndex, ==, -1);
    Context->FrameIndex = 0;
    Context->Pfn = 0;

    ASSERT(IsZeroMemory(Context, sizeof (XENBUS_GNTTAB_CONTEXT)));
    __GnttabFree(Context);

fail1:
    Error("fail1 (%08x)\n", status);

    RtlZeroMemory(Interface, sizeof (XENBUS_GNTTAB_INTERFACE));

    return status;
}

VOID
GnttabTeardown(
    IN OUT  PXENBUS_GNTTAB_INTERFACE    Interface
    )
{
    PXENBUS_GNTTAB_CONTEXT              Context = Interface->Context;

    Trace("====>\n");

    DEBUG(Deregister,
          Context->DebugInterface,
          Context->DebugCallback);
    Context->DebugCallback = NULL;

    DEBUG(Release, Context->DebugInterface);
    Context->DebugInterface = NULL;

    SUSPEND(Deregister,
            Context->SuspendInterface,
            Context->SuspendCallbackEarly);
    Context->SuspendCallbackEarly = NULL;

    SUSPEND(Release, Context->SuspendInterface);
    Context->SuspendInterface = NULL;

    CACHE(Release, Context->CacheInterface);
    Context->CacheInterface = NULL;

    __GnttabShrink(Context);
    RangeSetTeardown(Context->RangeSet);

    Context->Entry = NULL;

    __GnttabUnmap(Context);

    ASSERT3S(Context->FrameIndex, ==, -1);
    Context->FrameIndex = 0;
    Context->Pfn = 0;

    ASSERT(IsZeroMemory(Context, sizeof (XENBUS_GNTTAB_CONTEXT)));
    __GnttabFree(Context);

    RtlZeroMemory(Interface, sizeof (XENBUS_GNTTAB_INTERFACE));

    Trace("<====\n");
}
