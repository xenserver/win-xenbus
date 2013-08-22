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
#include <xen.h>
#include <util.h>

#include "suspend.h"
#include "thread.h"
#include "fdo.h"
#include "sync.h"
#include "dbg_print.h"
#include "assert.h"

struct _XENBUS_SUSPEND_CALLBACK {
    LIST_ENTRY  ListEntry;
    VOID        (*Function)(PVOID);
    PVOID       Argument;
};

struct _XENBUS_SUSPEND_CONTEXT {
    LONG                    References;
    ULONG                   Count;
    LIST_ENTRY              EarlyList;
    LIST_ENTRY              LateList;
    KSPIN_LOCK              Lock;
    PXENBUS_DEBUG_INTERFACE DebugInterface;
    PXENBUS_DEBUG_CALLBACK  DebugCallback;
};

#define SUSPEND_TAG 'PSUS'

static FORCEINLINE PVOID
__SuspendAllocate(
    IN  ULONG   Length
    )
{
    return __AllocateNonPagedPoolWithTag(Length, SUSPEND_TAG);
}

static FORCEINLINE VOID
__SuspendFree(
    IN  PVOID   Buffer
    )
{
    __FreePoolWithTag(Buffer, SUSPEND_TAG);
}

static NTSTATUS
SuspendRegister(
    IN  PXENBUS_SUSPEND_CONTEXT         Context,
    IN  XENBUS_SUSPEND_CALLBACK_TYPE    Type,
    IN  VOID                            (*Function)(PVOID),
    IN  PVOID                           Argument OPTIONAL,
    OUT PXENBUS_SUSPEND_CALLBACK        *Callback
    )
{
    KIRQL                               Irql;
    NTSTATUS                            status;

    *Callback = __SuspendAllocate(sizeof (XENBUS_SUSPEND_CALLBACK));

    status = STATUS_NO_MEMORY;
    if (*Callback == NULL)
        goto fail1;

    (*Callback)->Function = Function;
    (*Callback)->Argument = Argument;

    KeAcquireSpinLock(&Context->Lock, &Irql);

    switch (Type) {
    case SUSPEND_CALLBACK_EARLY:
        InsertTailList(&Context->EarlyList, &(*Callback)->ListEntry);
        break;

    case SUSPEND_CALLBACK_LATE:
        InsertTailList(&Context->LateList, &(*Callback)->ListEntry);
        break;

    default:
        ASSERT(FALSE);
        break;
    }

    KeReleaseSpinLock(&Context->Lock, Irql);

    return STATUS_SUCCESS;

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

static VOID
SuspendDeregister(
    IN  PXENBUS_SUSPEND_CONTEXT     Context,
    IN  PXENBUS_SUSPEND_CALLBACK    Callback
    )
{
    KIRQL                           Irql;

    KeAcquireSpinLock(&Context->Lock, &Irql);
    RemoveEntryList(&Callback->ListEntry);
    KeReleaseSpinLock(&Context->Lock, Irql);

    __SuspendFree(Callback);
}

static ULONG
SuspendCount(
    IN  PXENBUS_SUSPEND_CONTEXT     Context
    )
{
    //
    // No locking is required here since the system will be
    // single-threaded with interrupts disabled when the
    // value is incremented.
    //
    return Context->Count;
}

static VOID
SuspendAcquire(
    IN  PXENBUS_SUSPEND_CONTEXT     Context
    )
{
    InterlockedIncrement(&Context->References);
}

static VOID
SuspendRelease(
    IN  PXENBUS_SUSPEND_CONTEXT     Context
    )
{
    ASSERT(Context->References != 0);
    InterlockedDecrement(&Context->References);
}

#define SUSPEND_OPERATION(_Type, _Name, _Arguments) \
        Suspend ## _Name,

static XENBUS_SUSPEND_OPERATIONS  Operations = {
    DEFINE_SUSPEND_OPERATIONS
};

#undef SUSPEND_OPERATION

VOID
#pragma prefast(suppress:28167) // Function changes IRQL
SuspendTrigger(
    IN  PXENBUS_SUSPEND_INTERFACE   Interface
    )
{
    PXENBUS_SUSPEND_CONTEXT         Context = Interface->Context;
    KIRQL                           Irql;
    NTSTATUS                        status;

    KeAcquireSpinLock(&Context->Lock, &Irql);

    LogPrintf(LOG_LEVEL_INFO,
              "SUSPEND: ====>\n");

    SyncCapture();
    SyncDisableInterrupts();

    LogPrintf(LOG_LEVEL_INFO,
              "SUSPEND: SCHEDOP_shutdown:SHUTDOWN_suspend ====>\n");
    status = SchedShutdown(SHUTDOWN_suspend);
    LogPrintf(LOG_LEVEL_INFO,
              "SUSPEND: SCHEDOP_shutdown:SHUTDOWN_suspend <==== (%08x)\n",
              status);

    if (NT_SUCCESS(status)) {
        PLIST_ENTRY ListEntry;

        Context->Count++;

        for (ListEntry = Context->EarlyList.Flink;
             ListEntry != &Context->EarlyList;
             ListEntry = ListEntry->Flink) {
            PXENBUS_SUSPEND_CALLBACK  Callback;

            Callback = CONTAINING_RECORD(ListEntry, XENBUS_SUSPEND_CALLBACK, ListEntry);
            Callback->Function(Callback->Argument);
        }
    }

    SyncEnableInterrupts();

    if (NT_SUCCESS(status)) {
        PLIST_ENTRY ListEntry;

        for (ListEntry = Context->LateList.Flink;
             ListEntry != &Context->LateList;
             ListEntry = ListEntry->Flink) {
            PXENBUS_SUSPEND_CALLBACK  Callback;

            Callback = CONTAINING_RECORD(ListEntry, XENBUS_SUSPEND_CALLBACK, ListEntry);
            Callback->Function(Callback->Argument);
        }
    }

    SyncRelease();

    LogPrintf(LOG_LEVEL_INFO, "SUSPEND: <====\n");

    KeReleaseSpinLock(&Context->Lock, Irql);
}

static VOID
SuspendDebugCallback(
    IN  PVOID               Argument,
    IN  BOOLEAN             Crashing
    )
{
    PXENBUS_SUSPEND_CONTEXT Context = Argument;
    PLIST_ENTRY             ListEntry;

    UNREFERENCED_PARAMETER(Crashing);

    DEBUG(Printf,
          Context->DebugInterface,
          Context->DebugCallback,
          "Count = %u\n",
          Context->Count);

    for (ListEntry = Context->EarlyList.Flink;
         ListEntry != &Context->EarlyList;
         ListEntry = ListEntry->Flink) {
        PXENBUS_SUSPEND_CALLBACK    Callback;
        PCHAR                       Name;
        ULONG_PTR                   Offset;

        Callback = CONTAINING_RECORD(ListEntry, XENBUS_SUSPEND_CALLBACK, ListEntry);

        ModuleLookup((ULONG_PTR)Callback->Function, &Name, &Offset);

        if (Name == NULL) {
            DEBUG(Printf,
                  Context->DebugInterface,
                  Context->DebugCallback,
                  "EARLY: %p (%p)\n",
                  Callback->Function,
                  Callback->Argument);
        } else {
            DEBUG(Printf,
                  Context->DebugInterface,
                  Context->DebugCallback,
                  "EARLY: %s + %p (%p)\n",
                  Name,
                  (PVOID)Offset,
                  Callback->Argument);
        }
    }

    for (ListEntry = Context->LateList.Flink;
         ListEntry != &Context->LateList;
         ListEntry = ListEntry->Flink) {
        PXENBUS_SUSPEND_CALLBACK    Callback;
        PCHAR                       Name;
        ULONG_PTR                   Offset;

        Callback = CONTAINING_RECORD(ListEntry, XENBUS_SUSPEND_CALLBACK, ListEntry);

        ModuleLookup((ULONG_PTR)Callback->Function, &Name, &Offset);

        if (Name == NULL) {
            DEBUG(Printf,
                  Context->DebugInterface,
                  Context->DebugCallback,
                  "LATE: %p (%p)\n",
                  Callback->Function,
                  Callback->Argument);
        } else {
            DEBUG(Printf,
                  Context->DebugInterface,
                  Context->DebugCallback,
                  "LATE: %s + %p (%p)\n",
                  Name,
                  (PVOID)Offset,
                  Callback->Argument);
        }
    }
}
                     
NTSTATUS
SuspendInitialize(
    IN  PXENBUS_FDO                 Fdo,
    OUT PXENBUS_SUSPEND_INTERFACE   Interface
    )
{
    PXENBUS_SUSPEND_CONTEXT         Context;
    NTSTATUS                        status;

    Trace("====>\n");

    Context = __SuspendAllocate(sizeof (XENBUS_SUSPEND_CONTEXT));

    status = STATUS_NO_MEMORY;
    if (Context == NULL)
        goto fail1;

    InitializeListHead(&Context->EarlyList);
    InitializeListHead(&Context->LateList);
    KeInitializeSpinLock(&Context->Lock);

    Context->DebugInterface = FdoGetDebugInterface(Fdo);

    DEBUG(Acquire, Context->DebugInterface);

    status = DEBUG(Register,
                   Context->DebugInterface,
                   __MODULE__ "|SUSPEND",
                   SuspendDebugCallback,
                   Context,
                   &Context->DebugCallback);
    if (!NT_SUCCESS(status))
        goto fail2;

    Interface->Context = Context;
    Interface->Operations = &Operations;

    Trace("<====\n");

    return STATUS_SUCCESS;

fail2:
    Error("fail2\n");

    DEBUG(Release, Context->DebugInterface);
    Context->DebugInterface = NULL;

    RtlZeroMemory(&Context->Lock, sizeof (KSPIN_LOCK));
    RtlZeroMemory(&Context->LateList, sizeof (LIST_ENTRY));
    RtlZeroMemory(&Context->EarlyList, sizeof (LIST_ENTRY));

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

VOID
SuspendTeardown(
    IN OUT  PXENBUS_SUSPEND_INTERFACE   Interface
    )
{
    PXENBUS_SUSPEND_CONTEXT             Context = Interface->Context;

    Trace("====>\n");

    if (!IsListEmpty(&Context->LateList) || !IsListEmpty(&Context->EarlyList))
        BUG("OUTSTANDING CALLBACKS");

    Context->Count = 0;

    DEBUG(Deregister,
          Context->DebugInterface,
          Context->DebugCallback);
    Context->DebugCallback = NULL;

    DEBUG(Release, Context->DebugInterface);
    Context->DebugInterface = NULL;

    RtlZeroMemory(&Context->Lock, sizeof (KSPIN_LOCK));
    RtlZeroMemory(&Context->LateList, sizeof (LIST_ENTRY));
    RtlZeroMemory(&Context->EarlyList, sizeof (LIST_ENTRY));

    ASSERT(IsZeroMemory(Context, sizeof (XENBUS_SUSPEND_CONTEXT)));
    __SuspendFree(Context);

    RtlZeroMemory(Interface, sizeof (XENBUS_SUSPEND_INTERFACE));

    Trace("<====\n");
}
