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
#include <stdarg.h>
#include <stdlib.h>
#include <xen.h>
#include <util.h>

#include "gnttab.h"
#include "fdo.h"
#include "range_set.h"
#include "pool.h"
#include "dbg_print.h"
#include "assert.h"

#define GNTTAB_MAXIMUM_FRAME_COUNT  32
#define GNTTAB_ENTRY_PER_FRAME      (PAGE_SIZE / sizeof (grant_entry_v1_t))

// Xen requires that we avoid the first 8 entries of the table and
// we also reserve 1 entry for the crash kernel
#define GNTTAB_RESERVED_ENTRY_COUNT 9

struct _XENBUS_GNTTAB_DESCRIPTOR {
    ULONG               Reference;
    grant_entry_v1_t    Entry;
};

struct _XENBUS_GNTTAB_CONTEXT {
    LONG                        References;
    PFN_NUMBER                  Pfn;
    ULONG                       FrameCount;
    grant_entry_v1_t            *Entry;
    KSPIN_LOCK                  Lock;
    PXENBUS_RANGE_SET           RangeSet;
    PXENBUS_POOL                DescriptorPool;
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
    ULONG                       FrameIndex;
    PFN_NUMBER                  Pfn;
    ULONGLONG                   Start;
    ULONGLONG                   End;
    NTSTATUS                    status;

    FrameIndex = Context->FrameCount;

    status = STATUS_INSUFFICIENT_RESOURCES;
    ASSERT3U(FrameIndex, <=, GNTTAB_MAXIMUM_FRAME_COUNT);
    if (FrameIndex == GNTTAB_MAXIMUM_FRAME_COUNT)
        goto fail1;

    Pfn = Context->Pfn + FrameIndex;

    status = MemoryAddToPhysmap(Pfn,
                                XENMAPSPACE_grant_table,
                                FrameIndex);
    ASSERT(NT_SUCCESS(status));

    Context->FrameCount++;    

    Start = __max(GNTTAB_RESERVED_ENTRY_COUNT, FrameIndex * GNTTAB_ENTRY_PER_FRAME);
    End = (Context->FrameCount * GNTTAB_ENTRY_PER_FRAME) - 1;

    Trace("adding refrences [%08llx - %08llx]\n", Start, End);

    RangeSetPut(Context->RangeSet, Start, End);

    return STATUS_SUCCESS;

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

static NTSTATUS
GnttabDescriptorCtor(
    IN  PVOID                   Argument,
    IN  PVOID                   Object
    )
{
    PXENBUS_GNTTAB_CONTEXT      Context = Argument;
    PXENBUS_GNTTAB_DESCRIPTOR   Descriptor = Object;
    NTSTATUS                    status;

    if (!RangeSetIsEmpty(Context->RangeSet))
        goto done;

    status = __GnttabExpand(Context);
    if (!NT_SUCCESS(status))
        goto fail1;

done:
    Descriptor->Reference = (ULONG)RangeSetPop(Context->RangeSet);

    return STATUS_SUCCESS;

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
    PXENBUS_GNTTAB_CONTEXT      Context = Argument;
    PXENBUS_GNTTAB_DESCRIPTOR   Descriptor = Object;
    NTSTATUS                    status;

    status = RangeSetPut(Context->RangeSet,
                         (ULONGLONG)Descriptor->Reference,
                         (ULONGLONG)Descriptor->Reference);
    ASSERT(NT_SUCCESS(status));
}

static FORCEINLINE VOID
__drv_requiresIRQL(DISPATCH_LEVEL)
__GnttabAcquireLock(
    IN  PXENBUS_GNTTAB_CONTEXT  Context
    )
{
    ASSERT3U(KeGetCurrentIrql(), ==, DISPATCH_LEVEL);

    KeAcquireSpinLockAtDpcLevel(&Context->Lock);
}

static DECLSPEC_NOINLINE VOID
GnttabAcquireLock(
    IN  PXENBUS_GNTTAB_CONTEXT  Context
    )
{
    __GnttabAcquireLock(Context);
}

static FORCEINLINE VOID
__drv_requiresIRQL(DISPATCH_LEVEL)
__GnttabReleaseLock(
    IN  PXENBUS_GNTTAB_CONTEXT  Context
    )
{
    ASSERT3U(KeGetCurrentIrql(), ==, DISPATCH_LEVEL);

#pragma prefast(disable:26110)
    KeReleaseSpinLockFromDpcLevel(&Context->Lock);
}

static DECLSPEC_NOINLINE VOID
GnttabReleaseLock(
    IN  PXENBUS_GNTTAB_CONTEXT  Context
    )
{
    __GnttabReleaseLock(Context);
}

static FORCEINLINE NTSTATUS
__GnttabFill(
    IN  PXENBUS_GNTTAB_CONTEXT  Context
    )
{
    NTSTATUS                    status;

    status = RangeSetInitialize(&Context->RangeSet);
    if (!NT_SUCCESS(status))
        goto fail1;

    status = PoolInitialize("GnttabDescriptor",
                            sizeof (XENBUS_GNTTAB_DESCRIPTOR),
                            GnttabDescriptorCtor,
                            GnttabDescriptorDtor,
                            GnttabAcquireLock,
                            GnttabReleaseLock,
                            Context,
                            &Context->DescriptorPool);
    if (!NT_SUCCESS(status))
        goto fail2;

    return STATUS_SUCCESS;

fail2:
    Error("fail2\n");

    RangeSetTeardown(Context->RangeSet);
    Context->RangeSet = NULL;

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

static FORCEINLINE VOID
__GnttabEmpty(
    IN  PXENBUS_GNTTAB_CONTEXT  Context
    )
{
    ULONGLONG                   Entry;

    PoolTeardown(Context->DescriptorPool);
    Context->DescriptorPool = NULL;

    for (Entry = GNTTAB_RESERVED_ENTRY_COUNT;
         Entry < Context->FrameCount * GNTTAB_ENTRY_PER_FRAME;
         Entry++) {
        NTSTATUS    status;

        status = RangeSetGet(Context->RangeSet, Entry);
        ASSERT(NT_SUCCESS(status));
    }

    RangeSetTeardown(Context->RangeSet);
    Context->RangeSet = NULL;
}

static PXENBUS_GNTTAB_DESCRIPTOR
GnttabGet(
    IN  PXENBUS_GNTTAB_CONTEXT  Context
    )
{
    return PoolGet(Context->DescriptorPool, FALSE);
}

static VOID
GnttabPut(
    IN  PXENBUS_GNTTAB_CONTEXT      Context,
    IN  PXENBUS_GNTTAB_DESCRIPTOR   Descriptor
    )
{
    PoolPut(Context->DescriptorPool, Descriptor, FALSE);
}

static FORCEINLINE NTSTATUS
__GnttabPermitForeignAccessFullPage( 
    IN  PXENBUS_GNTTAB_CONTEXT      Context,
    IN  PXENBUS_GNTTAB_DESCRIPTOR   Descriptor,
    IN  USHORT                      Domain,
    IN  va_list                     Arguments
    )
{
    PFN_NUMBER                      Frame;
    BOOLEAN                         ReadOnly;
    grant_entry_v1_t                *Entry;

    Frame = va_arg(Arguments, PFN_NUMBER);
    ReadOnly = va_arg(Arguments, BOOLEAN);

    ASSERT(IsZeroMemory(&Descriptor->Entry, sizeof (grant_entry_v1_t)));

    Descriptor->Entry.flags = (ReadOnly) ? GTF_readonly : 0;
    Descriptor->Entry.domid = Domain;

    Descriptor->Entry.frame = (uint32_t)Frame;
    ASSERT3U(Descriptor->Entry.frame, ==, Frame);

    Entry = &Context->Entry[Descriptor->Reference];

    *Entry = Descriptor->Entry;
    KeMemoryBarrier();

    Entry->flags |= GTF_permit_access;
    KeMemoryBarrier();

    return STATUS_SUCCESS;
}

static NTSTATUS
GnttabPermitForeignAccess(
    IN  PXENBUS_GNTTAB_CONTEXT      Context,
    IN  PXENBUS_GNTTAB_DESCRIPTOR   Descriptor,
    IN  USHORT                      Domain,
    IN  XENBUS_GNTTAB_ENTRY_TYPE    Type,
    ...
    )
{
    va_list                         Arguments;
    NTSTATUS                        status;

    va_start(Arguments, Type);
    switch (Type) {
    case GNTTAB_ENTRY_FULL_PAGE:
        status =__GnttabPermitForeignAccessFullPage(Context, Descriptor, Domain, Arguments);
        break;

    default:
        status = STATUS_NOT_SUPPORTED;
        break;
    }
    va_end(Arguments);

    return status;
}

static NTSTATUS
GnttabRevokeForeignAccess(
    IN  PXENBUS_GNTTAB_CONTEXT      Context,
    IN  PXENBUS_GNTTAB_DESCRIPTOR   Descriptor
    )
{
    grant_entry_v1_t                *Entry;
    volatile SHORT                  *Flags;
    ULONG                           Attempt;
    NTSTATUS                        status;

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
    ULONG                       Index;
    PFN_NUMBER                  Pfn;
    NTSTATUS                    status;

    Pfn = Context->Pfn;

    for (Index = 0; Index < Context->FrameCount; Index++) {
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
    UNREFERENCED_PARAMETER(Context);

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
    ULONG                   Allocated;
    ULONG                   MaximumAllocated;
    ULONG                   Count;
    ULONG                   MinimumCount;

    UNREFERENCED_PARAMETER(Crashing);

    DEBUG(Printf,
          Context->DebugInterface,
          Context->DebugCallback,
          "Pfn = %p\n",
          (PVOID)Context->Pfn);
    
    DEBUG(Printf,
          Context->DebugInterface,
          Context->DebugCallback,
          "FrameCount = %u\n",
          Context->FrameCount);

    PoolGetStatistics(Context->DescriptorPool,
                      &Allocated,
                      &MaximumAllocated,
                      &Count,
                      &MinimumCount);

    DEBUG(Printf,
          Context->DebugInterface,
          Context->DebugCallback,
          "DESCRIPTOR POOL: Allocated = %u (Maximum = %u)\n",
          Allocated,
          MaximumAllocated);

    DEBUG(Printf,
          Context->DebugInterface,
          Context->DebugCallback,
          "DESCRIPTOR POOL: Count = %u (Minimum = %u)\n",
          Count,
          MinimumCount);
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

    KeInitializeSpinLock(&Context->Lock);

    __GnttabFill(Context);

    Context->SuspendInterface = FdoGetSuspendInterface(Fdo);

    SUSPEND(Acquire, Context->SuspendInterface);

    status = SUSPEND(Register,
                     Context->SuspendInterface,
                     SUSPEND_CALLBACK_EARLY,
                     GnttabSuspendCallbackEarly,
                     Context,
                     &Context->SuspendCallbackEarly);
    if (!NT_SUCCESS(status))
        goto fail3;

    Context->DebugInterface = FdoGetDebugInterface(Fdo);

    DEBUG(Acquire, Context->DebugInterface);

    status = DEBUG(Register,
                   Context->DebugInterface,
                   __MODULE__ "|GNTTAB",
                   GnttabDebugCallback,
                   Context,
                   &Context->DebugCallback);
    if (!NT_SUCCESS(status))
        goto fail4;

    Interface->Context = Context;
    Interface->Operations = &Operations;

    Trace("<====\n");

    return STATUS_SUCCESS;

fail4:
    Error("fail4\n");

    DEBUG(Release, Context->DebugInterface);
    Context->DebugInterface = NULL;

    SUSPEND(Deregister,
            Context->SuspendInterface,
            Context->SuspendCallbackEarly);
    Context->SuspendCallbackEarly = NULL;

fail3:
    Error("fail3\n");

    SUSPEND(Release, Context->SuspendInterface);
    Context->SuspendInterface = NULL;

    __GnttabEmpty(Context);

    RtlZeroMemory(&Context->Lock, sizeof (KSPIN_LOCK));

    Context->Entry = NULL;

fail2:
    Error("fail2\n");

    __GnttabUnmap(Context);

    Context->FrameCount = 0;
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

    __GnttabEmpty(Context);

    RtlZeroMemory(&Context->Lock, sizeof (KSPIN_LOCK));

    Context->Entry = NULL;

    __GnttabUnmap(Context);

    Context->FrameCount = 0;
    Context->Pfn = 0;

    ASSERT(IsZeroMemory(Context, sizeof (XENBUS_GNTTAB_CONTEXT)));
    __GnttabFree(Context);

    RtlZeroMemory(Interface, sizeof (XENBUS_GNTTAB_INTERFACE));

    Trace("<====\n");
}
