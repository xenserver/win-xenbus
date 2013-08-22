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

#define GNTTAB_MAXIMUM_ENTRY_FRAME_COUNT    32
#define GNTTAB_ENTRY_PER_FRAME              (PAGE_SIZE / sizeof (grant_entry_v2_t))
#define GNTTAB_STATUS_PER_FRAME             (PAGE_SIZE / sizeof (grant_status_t))

#define GNTTAB_STATUS_FRAME(_Frame) \
        ((((_Frame) * GNTTAB_ENTRY_PER_FRAME) + GNTTAB_STATUS_PER_FRAME - 1) / GNTTAB_STATUS_PER_FRAME)
#define GNTTAB_MAXIMUM_STATUS_FRAME_COUNT \
        GNTTAB_STATUS_FRAME(GNTTAB_MAXIMUM_ENTRY_FRAME_COUNT)

#define GNTTAB_RESERVED_ENTRY_COUNT 8

#define GNTTAB_INVALID_REFERENCE    0

#define GNTTAB_IS_INVALID_REFERENCE(_Reference) \
        ((_Reference) < GNTTAB_RESERVED_ENTRY_COUNT)

typedef struct _GNTTAB_DESCRIPTOR {
    LIST_ENTRY          ListEntry;
    ULONG               Next;
    PVOID               Caller;
    grant_entry_v2_t    Entry;
} GNTTAB_DESCRIPTOR, *PGNTTAB_DESCRIPTOR;

struct _XENBUS_GNTTAB_CONTEXT {
    LONG                        References;
    PFN_NUMBER                  Pfn;
    ULONG                       EntryFrameCount;
    ULONG                       StatusFrameCount;
    grant_entry_v2_t            *Entry;
    grant_status_t              *Status;
    KSPIN_LOCK                  Lock;
    ULONG                       HeadFreeReference;
    PULONG                      TailFreeReference;
    GNTTAB_DESCRIPTOR           Descriptor[GNTTAB_MAXIMUM_ENTRY_FRAME_COUNT * GNTTAB_ENTRY_PER_FRAME];
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
    ULONG                       EntryFrameIndex;
    ULONG                       StatusFrameIndex;
    PFN_NUMBER                  Pfn;
    ULONG                       Start;
    ULONG                       End;
    ULONG                       Reference;
    NTSTATUS                    status;

    EntryFrameIndex = Context->EntryFrameCount;

    status = STATUS_INSUFFICIENT_RESOURCES;
    ASSERT3U(EntryFrameIndex, <=, GNTTAB_MAXIMUM_ENTRY_FRAME_COUNT);
    if (EntryFrameIndex == GNTTAB_MAXIMUM_ENTRY_FRAME_COUNT)
        goto fail1;

    Pfn = Context->Pfn + EntryFrameIndex;

    status = MemoryAddToPhysmap(Pfn,
                                XENMAPSPACE_grant_table,
                                EntryFrameIndex);
    ASSERT(NT_SUCCESS(status));

    Context->EntryFrameCount++;    

    Reference = (EntryFrameIndex * GNTTAB_ENTRY_PER_FRAME) + GNTTAB_ENTRY_PER_FRAME - 1;
    StatusFrameIndex = Reference / GNTTAB_STATUS_PER_FRAME;

    if (StatusFrameIndex < Context->StatusFrameCount)
        goto done;

    Pfn = Context->Pfn + GNTTAB_MAXIMUM_ENTRY_FRAME_COUNT + StatusFrameIndex;

    status = MemoryAddToPhysmap(Pfn,
                                XENMAPSPACE_grant_table,
                                StatusFrameIndex | XENMAPIDX_grant_table_status);
    ASSERT(NT_SUCCESS(status));

    Context->StatusFrameCount++;    
    ASSERT3U(StatusFrameIndex, <, Context->StatusFrameCount);

done:
    Start = __max(GNTTAB_RESERVED_ENTRY_COUNT, EntryFrameIndex * GNTTAB_ENTRY_PER_FRAME);
    End = (Context->EntryFrameCount * GNTTAB_ENTRY_PER_FRAME) - 1;

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

static FORCEINLINE VOID
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
    grant_entry_v2_t            *Entry;

    ASSERT(!GNTTAB_IS_INVALID_REFERENCE(Reference));

    Frame = va_arg(Arguments, PFN_NUMBER);
    ReadOnly = va_arg(Arguments, BOOLEAN);

    Descriptor = &Context->Descriptor[Reference];
    ASSERT(IsZeroMemory(&Descriptor->Entry, sizeof (grant_entry_v2_t)));

    Descriptor->Entry.full_page.hdr.domid = Domain;
    Descriptor->Entry.full_page.hdr.flags = (ReadOnly) ? GTF_readonly : 0;
    Descriptor->Entry.full_page.frame = Frame;

    Entry = &Context->Entry[Reference];

    *Entry = Descriptor->Entry;
    KeMemoryBarrier();

    Entry->hdr.flags |= GTF_permit_access;
    KeMemoryBarrier();
}

static FORCEINLINE VOID
__GnttabPermitForeignAccessSubPage( 
    IN  PXENBUS_GNTTAB_CONTEXT  Context,
    IN  ULONG                   Reference,
    IN  USHORT                  Domain,
    IN  va_list                 Arguments
    )
{
    PGNTTAB_DESCRIPTOR          Descriptor;
    PFN_NUMBER                  Frame;
    ULONG                       Offset;
    ULONG                       Length;
    grant_entry_v2_t            *Entry;

    ASSERT(!GNTTAB_IS_INVALID_REFERENCE(Reference));

    Frame = va_arg(Arguments, PFN_NUMBER);
    Offset = va_arg(Arguments, ULONG);
    Length = va_arg(Arguments, ULONG);

    Descriptor = &Context->Descriptor[Reference];
    ASSERT(IsZeroMemory(&Descriptor->Entry, sizeof (grant_entry_v2_t)));

    Descriptor->Entry.sub_page.hdr.domid = Domain;
    Descriptor->Entry.sub_page.hdr.flags = GTF_sub_page;
    Descriptor->Entry.sub_page.frame = Frame;
    Descriptor->Entry.sub_page.page_off = (USHORT)Offset;
    Descriptor->Entry.sub_page.length = (USHORT)Length;

    Entry = &Context->Entry[Reference];

    *Entry = Descriptor->Entry;
    KeMemoryBarrier();

    Entry->hdr.flags |= GTF_permit_access;
    KeMemoryBarrier();
}

static FORCEINLINE VOID
__GnttabPermitForeignAccessTransitive( 
    IN  PXENBUS_GNTTAB_CONTEXT  Context,
    IN  ULONG                   Reference,
    IN  USHORT                  Domain,
    IN  va_list                 Arguments
    )
{
    PGNTTAB_DESCRIPTOR          Descriptor;
    ULONG                       RemoteReference;
    USHORT                      RemoteDomain;
    grant_entry_v2_t            *Entry;

    ASSERT(!GNTTAB_IS_INVALID_REFERENCE(Reference));

    RemoteReference = va_arg(Arguments, ULONG);
    RemoteDomain = va_arg(Arguments, USHORT);

    Descriptor = &Context->Descriptor[Reference];
    ASSERT(IsZeroMemory(&Descriptor->Entry, sizeof (grant_entry_v2_t)));

    Descriptor->Entry.transitive.hdr.domid = Domain;
    Descriptor->Entry.transitive.hdr.flags = GTF_transitive;
    Descriptor->Entry.transitive.trans_domid = RemoteDomain;
    Descriptor->Entry.transitive.gref = RemoteReference;

    Entry = &Context->Entry[Reference];

    *Entry = Descriptor->Entry;
    KeMemoryBarrier();

    Entry->hdr.flags |= GTF_permit_access;
    KeMemoryBarrier();
}

static VOID
GnttabPermitForeignAccess(
    IN  PXENBUS_GNTTAB_CONTEXT      Context,
    IN  ULONG                       Reference,
    IN  USHORT                      Domain,
    IN  XENBUS_GNTTAB_ENTRY_TYPE    Type,
    ...
    )
{
    va_list                         Arguments;

    va_start(Arguments, Type);
    switch (Type) {
    case GNTTAB_ENTRY_FULL_PAGE:
        __GnttabPermitForeignAccessFullPage(Context, Reference, Domain, Arguments);
        break;

    case GNTTAB_ENTRY_SUB_PAGE:
        __GnttabPermitForeignAccessSubPage(Context, Reference, Domain, Arguments);
        break;

    case GNTTAB_ENTRY_TRANSITIVE:
        __GnttabPermitForeignAccessTransitive(Context, Reference, Domain, Arguments);
        break;

    default:
        ASSERT(FALSE);
        break;
    }
    va_end(Arguments);
}

static VOID
GnttabRevokeForeignAccess(
    IN  PXENBUS_GNTTAB_CONTEXT  Context,
    IN  ULONG                   Reference
    )
{
    grant_entry_v2_t            *Entry;
    PGNTTAB_DESCRIPTOR          Descriptor;
    ULONG                       Attempt;

    Entry = &Context->Entry[Reference];

    Entry->hdr.flags = 0;
    KeMemoryBarrier();

    Attempt = 0;
    while (Attempt++ < 100) {
        grant_status_t  Status;

        Status = Context->Status[Reference];
        KeMemoryBarrier();

        if ((Status & (GTF_reading | GTF_writing)) == 0)
            break;

        _mm_pause();
    }
    if (Attempt == 100)
        Warning("Reference %08x is still busy\n");

    RtlZeroMemory(Entry, sizeof (grant_entry_v2_t));

    Descriptor = &Context->Descriptor[Reference];
    RtlZeroMemory(&Descriptor->Entry, sizeof (grant_entry_v2_t));
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
    ULONG                       Version;
    ULONG                       Index;
    PFN_NUMBER                  Pfn;
    NTSTATUS                    status;

    Version = 2;

    status = GrantTableSetVersion(Version);
    ASSERT(NT_SUCCESS(status));

    status = GrantTableGetVersion(&Version);
    if (NT_SUCCESS(status))
        ASSERT3U(Version, ==, 2);

    Pfn = Context->Pfn;

    for (Index = 0; Index < Context->EntryFrameCount; Index++) {
        status = MemoryAddToPhysmap(Pfn,
                                    XENMAPSPACE_grant_table,
                                    Index);
        ASSERT(NT_SUCCESS(status));

        Pfn++;
    }

    Pfn = Context->Pfn + GNTTAB_MAXIMUM_ENTRY_FRAME_COUNT;

    for (Index = 0; Index < Context->StatusFrameCount; Index++) {
        status = MemoryAddToPhysmap(Pfn,
                                    XENMAPSPACE_grant_table,
                                    Index | XENMAPIDX_grant_table_status);
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
          "EntryFrameCount = %u\n",
          Context->EntryFrameCount);

    DEBUG(Printf,
          Context->DebugInterface,
          Context->DebugCallback,
          "StatusFrameCount = %u\n",
          Context->StatusFrameCount);
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

    Memory->Translated.u.Memory.Start.QuadPart += (GNTTAB_MAXIMUM_ENTRY_FRAME_COUNT * PAGE_SIZE) +
                                                  (GNTTAB_MAXIMUM_STATUS_FRAME_COUNT * PAGE_SIZE);

    ASSERT3U(Memory->Translated.u.Memory.Length, >=, (GNTTAB_MAXIMUM_ENTRY_FRAME_COUNT * PAGE_SIZE) +
                                                     (GNTTAB_MAXIMUM_STATUS_FRAME_COUNT * PAGE_SIZE));
    Memory->Translated.u.Memory.Length -= (GNTTAB_MAXIMUM_ENTRY_FRAME_COUNT * PAGE_SIZE) +
                                          (GNTTAB_MAXIMUM_STATUS_FRAME_COUNT * PAGE_SIZE);

    Address.QuadPart = (ULONGLONG)Context->Pfn << PAGE_SHIFT;
    Context->Entry = (grant_entry_v2_t *)MmMapIoSpace(Address,
                                                      GNTTAB_MAXIMUM_ENTRY_FRAME_COUNT * PAGE_SIZE,
                                                      MmCached);
    status = STATUS_UNSUCCESSFUL;
    if (Context->Entry == NULL)
        goto fail2;

    Info("grant_entry_v2_t *: %p\n", Context->Entry);

    Address.QuadPart = (ULONGLONG)(Context->Pfn + GNTTAB_MAXIMUM_ENTRY_FRAME_COUNT) << PAGE_SHIFT;
    Context->Status = (grant_status_t *)MmMapIoSpace(Address,
                                                     GNTTAB_MAXIMUM_STATUS_FRAME_COUNT * PAGE_SIZE,
                                                     MmCached);
    status = STATUS_UNSUCCESSFUL;
    if (Context->Status == NULL)
        goto fail3;

    Info("grant_status_t *: %p\n", Context->Status);

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

    __GnttabEmpty(Context);

    Context->TailFreeReference = NULL;

    RtlZeroMemory(&Context->Lock, sizeof (KSPIN_LOCK));
    RtlZeroMemory(&Context->List, sizeof (LIST_ENTRY));

    Context->Status = NULL;

fail3:
    Error("fail3\n");

    Context->Entry = NULL;

fail2:
    Error("fail2\n");

    __GnttabUnmap(Context);

    Context->StatusFrameCount = 0;
    Context->EntryFrameCount = 0;
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

    Context->Status = NULL;

    Context->Entry = NULL;

    __GnttabUnmap(Context);

    Context->StatusFrameCount = 0;
    Context->EntryFrameCount = 0;
    Context->Pfn = 0;

    ASSERT(IsZeroMemory(Context, sizeof (XENBUS_GNTTAB_CONTEXT)));
    __GnttabFree(Context);

    RtlZeroMemory(Interface, sizeof (XENBUS_GNTTAB_INTERFACE));

    Trace("<====\n");
}
