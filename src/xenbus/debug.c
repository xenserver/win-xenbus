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
#include <stdarg.h>
#include <stdlib.h>
#include <xen.h>
#include <util.h>

#include "high.h"
#include "debug.h"
#include "fdo.h"
#include "log.h"
#include "assert.h"

#define MAXIMUM_PREFIX_LENGTH   32

struct _XENBUS_DEBUG_CALLBACK {
    LIST_ENTRY          ListEntry;
    PVOID               Caller;
    CHAR                Prefix[MAXIMUM_PREFIX_LENGTH];
    VOID                (*Function)(PVOID, BOOLEAN);
    PVOID               Argument;
};

struct _XENBUS_DEBUG_CONTEXT {
    LONG                        References;
    KBUGCHECK_CALLBACK_RECORD   BugCheckCallbackRecord;
    LIST_ENTRY                  List;
    HIGH_LOCK                   Lock;
};

#define DEBUG_TAG   'UBED'

static FORCEINLINE PVOID
__DebugAllocate(
    IN  ULONG   Length
    )
{
    return __AllocateNonPagedPoolWithTag(Length, DEBUG_TAG);
}

static FORCEINLINE VOID
__DebugFree(
    IN  PVOID   Buffer
    )
{
    __FreePoolWithTag(Buffer, DEBUG_TAG);
}

extern USHORT
RtlCaptureStackBackTrace(
    __in        ULONG   FramesToSkip,
    __in        ULONG   FramesToCapture,
    __out       PVOID   *BackTrace,
    __out_opt   PULONG  BackTraceHash
    );

static NTSTATUS
DebugRegister(
    IN  PXENBUS_DEBUG_CONTEXT   Context,
    IN  const CHAR              *Prefix,
    IN  VOID                    (*Function)(PVOID, BOOLEAN),
    IN  PVOID                   Argument OPTIONAL,
    OUT PXENBUS_DEBUG_CALLBACK  *Callback
    )
{
    ULONG                       Length;
    KIRQL                       Irql;
    NTSTATUS                    status;

    *Callback = __DebugAllocate(sizeof (XENBUS_DEBUG_CALLBACK));

    status = STATUS_NO_MEMORY;
    if (*Callback == NULL)
        goto fail1;

    (VOID) RtlCaptureStackBackTrace(1, 1, &(*Callback)->Caller, NULL);    

    Length = (ULONG)__min(strlen(Prefix), MAXIMUM_PREFIX_LENGTH - 1);
    RtlCopyMemory((*Callback)->Prefix, Prefix, Length);

    (*Callback)->Function = Function;
    (*Callback)->Argument = Argument;

    AcquireHighLock(&Context->Lock, &Irql);
    InsertTailList(&Context->List, &(*Callback)->ListEntry);
    ReleaseHighLock(&Context->Lock, Irql);

    return STATUS_SUCCESS;

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

static VOID
DebugPrintf(
    IN  PXENBUS_DEBUG_CONTEXT   Context,
    IN  PXENBUS_DEBUG_CALLBACK  Callback,
    IN  const CHAR              *Format,
    ...
    )
{
    va_list                     Arguments;

    UNREFERENCED_PARAMETER(Context);

    LogQemuPrintf("%s: ", Callback->Prefix);

    va_start(Arguments, Format);
    LogQemuVPrintf(Format, Arguments);
    va_end(Arguments);
}

static VOID
DebugDeregister(
    IN  PXENBUS_DEBUG_CONTEXT   Context,
    IN  PXENBUS_DEBUG_CALLBACK  Callback
    )
{
    KIRQL                       Irql;

    AcquireHighLock(&Context->Lock, &Irql);
    RemoveEntryList(&Callback->ListEntry);
    ReleaseHighLock(&Context->Lock, Irql);

    __DebugFree(Callback);
}

static VOID
DebugAcquire(
    IN  PXENBUS_DEBUG_CONTEXT   Context
    )
{
    InterlockedIncrement(&Context->References);
}

static VOID
DebugRelease(
    IN  PXENBUS_DEBUG_CONTEXT   Context
    )
{
    ASSERT(Context->References != 0);
    InterlockedDecrement(&Context->References);
}

#define DEBUG_OPERATION(_Type, _Name, _Arguments) \
        Debug ## _Name,

static XENBUS_DEBUG_OPERATIONS  Operations = {
    DEFINE_DEBUG_OPERATIONS
};

#undef DEBUG_OPERATION

static FORCEINLINE VOID
__DebugTrigger(
    IN  PXENBUS_DEBUG_CONTEXT   Context,
    IN  BOOLEAN                 Crashing
    )
{
    PLIST_ENTRY                 ListEntry;

    for (ListEntry = Context->List.Flink;
         ListEntry != &Context->List;
         ListEntry = ListEntry->Flink) {
        PXENBUS_DEBUG_CALLBACK  Callback;
        PCHAR                   Name;
        ULONG_PTR               Offset;

        Callback = CONTAINING_RECORD(ListEntry, XENBUS_DEBUG_CALLBACK, ListEntry);

        ModuleLookup((ULONG_PTR)Callback->Function, &Name, &Offset);

        if (Name == NULL) {
            ModuleLookup((ULONG_PTR)Callback->Caller, &Name, &Offset);

            if (Name != NULL) {
                LogQemuPrintf("XEN|DEBUG: SKIPPING %p PREFIX '%s' REGISTERED BY %s + %p\n",
                            Callback->Function,
                            Callback->Prefix,
                            Name,
                            Offset);
            } else {
                LogQemuPrintf("XEN|DEBUG: SKIPPING %p PREFIX '%s' REGISTERED BY %p\n",
                            Callback->Function,
                            Callback->Prefix,
                            Callback->Caller);
            }
        } else {
            LogQemuPrintf("XEN|DEBUG: ====> (%s + %p)\n", Name, Offset);
            Callback->Function(Callback->Argument, Crashing);
            LogQemuPrintf("XEN|DEBUG: <==== (%s + %p)\n", Name, Offset);
        }
    }
}
    
VOID
DebugTrigger(
    IN  PXENBUS_DEBUG_INTERFACE Interface
    )
{
    PXENBUS_DEBUG_CONTEXT       Context = Interface->Context;
    KIRQL                       Irql;

    Trace("====>\n");

    KeRaiseIrql(HIGH_LEVEL, &Irql);

    __DebugTrigger(Context, FALSE);

    KeLowerIrql(Irql);

    Trace("<====\n");
}

KBUGCHECK_CALLBACK_ROUTINE DebugBugCheckCallback;

VOID                     
DebugBugCheckCallback(
    IN  PVOID   Argument,
    IN  ULONG   Length
    )
{
    PXENBUS_DEBUG_CONTEXT   Context = Argument;

    if (Length >= sizeof (XENBUS_DEBUG_CONTEXT))
        __DebugTrigger(Context, TRUE);
}

NTSTATUS
DebugInitialize(
    IN  PXENBUS_FDO                     Fdo,
    OUT PXENBUS_DEBUG_INTERFACE         Interface
    )
{
    PXENBUS_DEBUG_CONTEXT               Context;
    NTSTATUS                            status;

    UNREFERENCED_PARAMETER(Fdo);

    Trace("====>\n");

    Context = __DebugAllocate(sizeof (XENBUS_DEBUG_CONTEXT));

    status = STATUS_NO_MEMORY;
    if (Context == NULL)
        goto fail1;

    InitializeListHead(&Context->List);
    InitializeHighLock(&Context->Lock);

    KeInitializeCallbackRecord(&Context->BugCheckCallbackRecord);

    status = STATUS_UNSUCCESSFUL;
    if (!KeRegisterBugCheckCallback(&Context->BugCheckCallbackRecord,
                                    DebugBugCheckCallback,
                                    Context,
                                    sizeof (XENBUS_DEBUG_CONTEXT),
                                    (PUCHAR)__MODULE__))
        goto fail2;

    Interface->Context = Context;
    Interface->Operations = &Operations;

    Trace("<====\n");

    return STATUS_SUCCESS;

fail2:
    Error("fail2\n");

    RtlZeroMemory(&Context->Lock, sizeof (KSPIN_LOCK));
    RtlZeroMemory(&Context->List, sizeof (LIST_ENTRY));

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

VOID
DebugTeardown(
    IN OUT  PXENBUS_DEBUG_INTERFACE     Interface
    )
{
    PXENBUS_DEBUG_CONTEXT               Context = Interface->Context;

    Trace("====>\n");

    (VOID) KeDeregisterBugCheckCallback(&Context->BugCheckCallbackRecord);
    RtlZeroMemory(&Context->BugCheckCallbackRecord, sizeof (KBUGCHECK_CALLBACK_RECORD));

    if (!IsListEmpty(&Context->List)) {
        PLIST_ENTRY ListEntry;

        for (ListEntry = Context->List.Flink;
             ListEntry != &Context->List;
             ListEntry = ListEntry->Flink) {
            PXENBUS_DEBUG_CALLBACK  Callback;
            PCHAR                   Name;
            ULONG_PTR               Offset;

            Callback = CONTAINING_RECORD(ListEntry, XENBUS_DEBUG_CALLBACK, ListEntry);

            ModuleLookup((ULONG_PTR)Callback->Caller, &Name, &Offset);

            if (Name != NULL) {
                Error("CALLBACK: %p PREFIX '%s' REGISTERED BY %s + %p\n",
                      Callback->Function,
                      Callback->Prefix,
                      Name,
                      (PVOID)Offset);
            } else {
                Error("CALLBACK: %p PREFIX '%s' REGISTERED BY %p\n",
                      Callback->Function,
                      Callback->Prefix,
                      Callback->Caller);
            }
        }

        BUG("OUTSTANDING CALLBACKS");
    }

    RtlZeroMemory(&Context->Lock, sizeof (KSPIN_LOCK));
    RtlZeroMemory(&Context->List, sizeof (LIST_ENTRY));

    ASSERT(IsZeroMemory(Context, sizeof (XENBUS_DEBUG_CONTEXT)));
    __DebugFree(Context);

    RtlZeroMemory(Interface, sizeof (XENBUS_DEBUG_INTERFACE));

    Trace("<====\n");
}
