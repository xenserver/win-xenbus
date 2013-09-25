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
#include "dbg_print.h"
#include "assert.h"

#define GNTTAB_MAXIMUM_FRAME_COUNT    32
#define GNTTAB_ENTRY_PER_FRAME              (PAGE_SIZE / sizeof (grant_entry_v1_t))

#define GNTTAB_RESERVED_ENTRY_COUNT 8

#define GNTTAB_INVALID_REFERENCE    0

#define GNTTAB_IS_INVALID_REFERENCE(_Reference) \
        ((_Reference) < GNTTAB_RESERVED_ENTRY_COUNT)

typedef struct _GNTTAB_DESCRIPTOR {
    LIST_ENTRY          ListEntry;
    ULONG               Next;
    PVOID               Caller;
    grant_entry_v1_t    Entry;
} GNTTAB_DESCRIPTOR, *PGNTTAB_DESCRIPTOR;

struct _XENBUS_GNTTAB_CONTEXT {
    LONG                        References;
    PFN_NUMBER                  Pfn;
    ULONG                       FrameCount;
    grant_entry_v1_t            *Entry;
    KSPIN_LOCK                  Lock;
    ULONG                       HeadFreeReference;
    PULONG                      TailFreeReference;
    GNTTAB_DESCRIPTOR           Descriptor[GNTTAB_MAXIMUM_FRAME_COUNT * GNTTAB_ENTRY_PER_FRAME];
    LIST_ENTRY                  List;
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

static NTSTATUS
GnttabExpand(
    IN  PXENBUS_GNTTAB_CONTEXT  Context
    )
{
    ULONG                       FrameIndex;
    PFN_NUMBER                  Pfn;
    ULONG                       Start;
    ULONG                       End;
    ULONG                       Reference;
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

    Trace("adding refrences [%08x - %08x]\n", Start, End);

    for (Reference = Start; Reference <= End; Reference++) {
        PGNTTAB_DESCRIPTOR  Descriptor = &Context->Descriptor[Reference];

        *Context->TailFreeReference = Reference;

        ASSERT(GNTTAB_IS_INVALID_REFERENCE(Descriptor->Next));
        Context->TailFreeReference = &Descriptor->Next;
    }

    return STATUS_SUCCESS;

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

static FORCEINLINE VOID
__GnttabEmpty(
    IN  PXENBUS_GNTTAB_CONTEXT  Context
    )
{
    while (!GNTTAB_IS_INVALID_REFERENCE(Context->HeadFreeReference)) {
        ULONG               Reference = Context->HeadFreeReference;
        PGNTTAB_DESCRIPTOR  Descriptor = &Context->Descriptor[Reference];

        Context->HeadFreeReference = Descriptor->Next;
        Descriptor->Next = GNTTAB_INVALID_REFERENCE;
    }

    Context->TailFreeReference = &Context->HeadFreeReference;
}

extern USHORT
RtlCaptureStackBackTrace(
    __in        ULONG   FramesToSkip,
    __in        ULONG   FramesToCapture,
    __out       PVOID   *BackTrace,
    __out_opt   PULONG  BackTraceHash
    );

static NTSTATUS
GnttabGet(
    IN  PXENBUS_GNTTAB_CONTEXT  Context,
    OUT PULONG                  Reference
    )
{
    PGNTTAB_DESCRIPTOR          Descriptor;
    KIRQL                       Irql;
    NTSTATUS                    status;

    KeAcquireSpinLock(&Context->Lock, &Irql);

    if (!GNTTAB_IS_INVALID_REFERENCE(Context->HeadFreeReference))
        goto done;

    ASSERT3P(Context->TailFreeReference, ==, &Context->HeadFreeReference);

    status = GnttabExpand(Context);
    if (!NT_SUCCESS(status))
        goto fail1;

done:
    ASSERT(!GNTTAB_IS_INVALID_REFERENCE(Context->HeadFreeReference));

    *Reference = Context->HeadFreeReference;
    Descriptor = &Context->Descriptor[*Reference];

    Context->HeadFreeReference = Descriptor->Next;

    if (GNTTAB_IS_INVALID_REFERENCE(Context->HeadFreeReference))
        Context->TailFreeReference = &Context->HeadFreeReference;

    Descriptor->Next = GNTTAB_INVALID_REFERENCE;

    ASSERT(IsZeroMemory(Descriptor, sizeof (GNTTAB_DESCRIPTOR)));

    InsertTailList(&Context->List, &Descriptor->ListEntry);

    KeReleaseSpinLock(&Context->Lock, Irql);

    (VOID) RtlCaptureStackBackTrace(1, 1, &Descriptor->Caller, NULL);    

    return STATUS_SUCCESS;

fail1:
    Error("fail1 (%08x)\n", status);

    KeReleaseSpinLock(&Context->Lock, Irql);

    return status;
}

static VOID
GnttabPut(
    IN  PXENBUS_GNTTAB_CONTEXT  Context,
    IN  ULONG                   Reference
    )
{
    KIRQL                       Irql;
    PGNTTAB_DESCRIPTOR          Descriptor;

    ASSERT(!GNTTAB_IS_INVALID_REFERENCE(Reference));

    Descriptor = &Context->Descriptor[Reference];

    Descriptor->Caller = NULL;

    KeAcquireSpinLock(&Context->Lock, &Irql);

    RemoveEntryList(&Descriptor->ListEntry);
    RtlZeroMemory(&Descriptor->ListEntry, sizeof (LIST_ENTRY));

    ASSERT(IsZeroMemory(Descriptor, sizeof (GNTTAB_DESCRIPTOR)));

    ASSERT(GNTTAB_IS_INVALID_REFERENCE(Descriptor->Next));

    *Context->TailFreeReference = Reference;

    ASSERT(GNTTAB_IS_INVALID_REFERENCE(Descriptor->Next));
    Context->TailFreeReference = &Descriptor->Next;

    KeReleaseSpinLock(&Context->Lock, Irql);
}

static FORCEINLINE NTSTATUS
__GnttabPermitForeignAccessFullPage( 
    IN  PXENBUS_GNTTAB_CONTEXT  Context,
    IN  ULONG                   Reference,
    IN  USHORT                  Domain,
    IN  va_list                 Arguments
    )
{
    PGNTTAB_DESCRIPTOR          Descriptor;
    PFN_NUMBER                  Frame;
    BOOLEAN                     ReadOnly;
    grant_entry_v1_t            *Entry;

    ASSERT(!GNTTAB_IS_INVALID_REFERENCE(Reference));

    Frame = va_arg(Arguments, PFN_NUMBER);
    ReadOnly = va_arg(Arguments, BOOLEAN);

    Descriptor = &Context->Descriptor[Reference];
    ASSERT(IsZeroMemory(&Descriptor->Entry, sizeof (grant_entry_v1_t)));

    Descriptor->Entry.flags = (ReadOnly) ? GTF_readonly : 0;
    Descriptor->Entry.domid = Domain;

    Descriptor->Entry.frame = (uint32_t)Frame;
    ASSERT3U(Descriptor->Entry.frame, ==, Frame);

    Entry = &Context->Entry[Reference];

    *Entry = Descriptor->Entry;
    KeMemoryBarrier();

    Entry->flags |= GTF_permit_access;
    KeMemoryBarrier();

    return STATUS_SUCCESS;
}

static NTSTATUS
GnttabPermitForeignAccess(
    IN  PXENBUS_GNTTAB_CONTEXT      Context,
    IN  ULONG                       Reference,
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
        status =__GnttabPermitForeignAccessFullPage(Context, Reference, Domain, Arguments);
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
    IN  PXENBUS_GNTTAB_CONTEXT  Context,
    IN  ULONG                   Reference
    )
{
    grant_entry_v1_t            *Entry;
    volatile SHORT              *Flags;
    PGNTTAB_DESCRIPTOR          Descriptor;
    ULONG                       Attempt;
    NTSTATUS                    status;

    Entry = &Context->Entry[Reference];
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

    Descriptor = &Context->Descriptor[Reference];
    RtlZeroMemory(&Descriptor->Entry, sizeof (grant_entry_v1_t));

    return STATUS_SUCCESS;

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

static NTSTATUS
GnttabCopy(
    IN  PXENBUS_GNTTAB_CONTEXT          Context,
    IN  PLIST_ENTRY                     List,
    IN  ULONG                           Count
    )
{
    struct gnttab_copy                  *op;
    PLIST_ENTRY                         ListEntry;
    ULONG                               Index;
    NTSTATUS                            status;

    UNREFERENCED_PARAMETER(Context);

    op = __GnttabAllocate(sizeof (struct gnttab_copy) * Count);

    status = STATUS_NO_MEMORY;
    if (op == NULL)
        goto fail1;

    Index = 0;
    for (ListEntry = List->Flink;
         ListEntry != List;
         ListEntry = ListEntry->Flink) {
        PXENBUS_GNTTAB_COPY_OPERATION   Operation;

        Operation = CONTAINING_RECORD(ListEntry, XENBUS_GNTTAB_COPY_OPERATION, ListEntry);

        ASSERT3U(Operation->Length, <=, PAGE_SIZE);
        ASSERT3U(Operation->Offset + Operation->Length, <=, PAGE_SIZE);
        ASSERT3U(Operation->RemoteOffset + Operation->Length, <=, PAGE_SIZE);

        op[Index].source.domid = Operation->RemoteDomain;
        op[Index].source.u.ref = Operation->RemoteReference;
        op[Index].source.offset = (USHORT)Operation->RemoteOffset;

        op[Index].dest.domid = DOMID_SELF;
        op[Index].dest.u.gmfn = Operation->Pfn;
        op[Index].dest.offset = (USHORT)Operation->Offset;

        op[Index].len = (USHORT)Operation->Length;
        op[Index].flags = GNTCOPY_source_gref;

        Index++;
    }
    ASSERT3U(Index, ==, Count);

    status = GrantTableCopy(op, Count);
    if (!NT_SUCCESS(status))
        goto fail2;

    status = STATUS_UNSUCCESSFUL;
    for (Index = 0; Index < Count; Index++) {
        if (op[Index].status != 0)
            goto fail3;
    }

    __GnttabFree(op);

    return STATUS_SUCCESS;

fail3:
    Error("fail3\n");

fail2:
    Error("fail2\n");

    __GnttabFree(op);

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
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

    InitializeListHead(&Context->List);
    KeInitializeSpinLock(&Context->Lock);

    ASSERT(GNTTAB_IS_INVALID_REFERENCE(Context->HeadFreeReference));
    Context->TailFreeReference = &Context->HeadFreeReference;

    GnttabExpand(Context);

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

    Context->TailFreeReference = NULL;

    RtlZeroMemory(&Context->Lock, sizeof (KSPIN_LOCK));
    RtlZeroMemory(&Context->List, sizeof (LIST_ENTRY));

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

    if (!IsListEmpty(&Context->List))
        BUG("OUTSTANDING REFERENCES");

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

    Context->TailFreeReference = NULL;

    RtlZeroMemory(&Context->Lock, sizeof (KSPIN_LOCK));
    RtlZeroMemory(&Context->List, sizeof (LIST_ENTRY));

    Context->Entry = NULL;

    __GnttabUnmap(Context);

    Context->FrameCount = 0;
    Context->Pfn = 0;

    ASSERT(IsZeroMemory(Context, sizeof (XENBUS_GNTTAB_CONTEXT)));
    __GnttabFree(Context);

    RtlZeroMemory(Interface, sizeof (XENBUS_GNTTAB_INTERFACE));

    Trace("<====\n");
}
