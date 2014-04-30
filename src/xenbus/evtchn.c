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

#include "evtchn.h"
#include "fdo.h"
#include "dbg_print.h"
#include "assert.h"

typedef struct _EVTCHN_FIXED_PARAMETERS {
    BOOLEAN Mask;
} EVTCHN_FIXED_PARAMETERS, *PEVTCHN_FIXED_PARAMETERS;

typedef struct _EVTCHN_UNBOUND_PARAMETERS {
    USHORT  RemoteDomain;
    BOOLEAN Mask;
} EVTCHN_UNBOUND_PARAMETERS, *PEVTCHN_UNBOUND_PARAMETERS;

typedef struct _EVTCHN_INTER_DOMAIN_PARAMETERS {
    USHORT  RemoteDomain;
    ULONG   RemotePort;
    BOOLEAN Mask;
} EVTCHN_INTER_DOMAIN_PARAMETERS, *PEVTCHN_INTER_DOMAIN_PARAMETERS;

typedef struct _EVTCHN_VIRQ_PARAMETERS {
    ULONG   Index;
} EVTCHN_VIRQ_PARAMETERS, *PEVTCHN_VIRQ_PARAMETERS;

#pragma warning(push)
#pragma warning(disable:4201)   // nonstandard extension used : nameless struct/union

typedef struct _EVTCHN_PARAMETERS {
    union {
        EVTCHN_FIXED_PARAMETERS         Fixed;
        EVTCHN_UNBOUND_PARAMETERS       Unbound;
        EVTCHN_INTER_DOMAIN_PARAMETERS  InterDomain;
        EVTCHN_VIRQ_PARAMETERS          Virq;
    };
} EVTCHN_PARAMETERS, *PEVTCHN_PARAMETERS;

#pragma warning(pop)

#define EVTCHN_DESCRIPTOR_MAGIC 'DTVE'

struct _XENBUS_EVTCHN_DESCRIPTOR {
    ULONG                               Magic;
    LIST_ENTRY                          ListEntry;
    PVOID                               Caller;
    PKSERVICE_ROUTINE                   Callback;
    PVOID                               Argument;
    BOOLEAN                             Active; // Must be tested at >= DISPATCH_LEVEL
    XENBUS_EVTCHN_TYPE                  Type;
    EVTCHN_PARAMETERS                   Parameters;
    ULONG                               LocalPort;
};

struct _XENBUS_EVTCHN_CONTEXT {
    LONG                            References;
    PXENBUS_RESOURCE                Interrupt;
    PKINTERRUPT                     InterruptObject;
    BOOLEAN                         Enabled;
    PXENBUS_SUSPEND_INTERFACE       SuspendInterface;
    PXENBUS_SUSPEND_CALLBACK        SuspendCallbackEarly;
    PXENBUS_DEBUG_INTERFACE         DebugInterface;
    PXENBUS_DEBUG_CALLBACK          DebugCallback;
    PXENBUS_SHARED_INFO_INTERFACE   SharedInfoInterface;
    PXENBUS_EVTCHN_DESCRIPTOR       Descriptor[EVTCHN_SELECTOR_COUNT * EVTCHN_PER_SELECTOR];
    LIST_ENTRY                      List;
};

#define EVTCHN_TAG  'CTVE'

static FORCEINLINE PVOID
__EvtchnAllocate(
    IN  ULONG   Length
    )
{
    return __AllocateNonPagedPoolWithTag(Length, EVTCHN_TAG);
}

static FORCEINLINE VOID
__EvtchnFree(
    IN  PVOID   Buffer
    )
{
    __FreePoolWithTag(Buffer, EVTCHN_TAG);
}

#pragma warning(push)
#pragma warning(disable: 28230)
#pragma warning(disable: 28285)

_IRQL_requires_max_(HIGH_LEVEL) // HIGH_LEVEL is best approximation of DIRQL
_IRQL_saves_
_IRQL_raises_(HIGH_LEVEL) // HIGH_LEVEL is best approximation of DIRQL
static FORCEINLINE KIRQL
__AcquireInterruptLock(
    _Inout_ PKINTERRUPT             Interrupt
    )
{
    return KeAcquireInterruptSpinLock(Interrupt);
}

_IRQL_requires_(HIGH_LEVEL) // HIGH_LEVEL is best approximation of DIRQL
static FORCEINLINE VOID
__ReleaseInterruptLock(
    _Inout_ PKINTERRUPT             Interrupt,
    _In_ _IRQL_restores_ KIRQL      Irql
    )
{
#pragma prefast(suppress:28121) // The function is not permitted to be called at the current IRQ level
    KeReleaseInterruptSpinLock(Interrupt, Irql);
}

#pragma warning(pop)

static FORCEINLINE NTSTATUS
__EvtchnOpenFixed(
    IN  PXENBUS_EVTCHN_DESCRIPTOR   Descriptor,
    IN  va_list                     Arguments
    )
{
    ULONG                           LocalPort;
    BOOLEAN                         Mask;

    LocalPort = va_arg(Arguments, ULONG);
    Mask = va_arg(Arguments, BOOLEAN);

    Descriptor->Parameters.Fixed.Mask = Mask;

    Descriptor->LocalPort = LocalPort;

    return STATUS_SUCCESS;
}

static FORCEINLINE NTSTATUS
__EvtchnOpenUnbound(
    IN  PXENBUS_EVTCHN_DESCRIPTOR   Descriptor,
    IN  va_list                     Arguments
    )
{
    USHORT                          RemoteDomain;
    BOOLEAN                         Mask;
    ULONG                           LocalPort;
    NTSTATUS                        status;

    RemoteDomain = va_arg(Arguments, USHORT);
    Mask = va_arg(Arguments, BOOLEAN);

    status = EventChannelAllocateUnbound(RemoteDomain, &LocalPort);
    if (!NT_SUCCESS(status))
        goto fail1;

    Descriptor->Parameters.Unbound.RemoteDomain = RemoteDomain;
    Descriptor->Parameters.Unbound.Mask = Mask;

    Descriptor->LocalPort = LocalPort;

    return STATUS_SUCCESS;

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

static FORCEINLINE NTSTATUS
__EvtchnOpenInterDomain(
    IN  PXENBUS_EVTCHN_DESCRIPTOR   Descriptor,
    IN  va_list                     Arguments
    )
{
    USHORT                          RemoteDomain;
    ULONG                           RemotePort;
    BOOLEAN                         Mask;
    ULONG                           LocalPort;
    NTSTATUS                        status;

    RemoteDomain = va_arg(Arguments, USHORT);
    RemotePort = va_arg(Arguments, ULONG);
    Mask = va_arg(Arguments, BOOLEAN);

    status = EventChannelBindInterDomain(RemoteDomain, RemotePort, &LocalPort);
    if (!NT_SUCCESS(status))
        goto fail1;

    Descriptor->Parameters.InterDomain.RemoteDomain = RemoteDomain;
    Descriptor->Parameters.InterDomain.RemotePort = RemotePort;
    Descriptor->Parameters.InterDomain.Mask = Mask;

    Descriptor->LocalPort = LocalPort;

    return STATUS_SUCCESS;

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

static FORCEINLINE NTSTATUS
__EvtchnOpenVirq(
    IN  PXENBUS_EVTCHN_DESCRIPTOR   Descriptor,
    IN  va_list                     Arguments
    )
{
    ULONG                           Index;
    ULONG                           LocalPort;
    NTSTATUS                        status;

    Index = va_arg(Arguments, ULONG);

    status = EventChannelBindVirq(Index, &LocalPort);
    if (!NT_SUCCESS(status))
        goto fail1;

    Descriptor->Parameters.Virq.Index = Index;

    Descriptor->LocalPort = LocalPort;

    return status;

fail1:
    Error("fail1 (%08x)\n", status);

    return STATUS_SUCCESS;
}

extern USHORT
RtlCaptureStackBackTrace(
    __in        ULONG   FramesToSkip,
    __in        ULONG   FramesToCapture,
    __out       PVOID   *BackTrace,
    __out_opt   PULONG  BackTraceHash
    );

static PXENBUS_EVTCHN_DESCRIPTOR
EvtchnOpen(
    IN  PXENBUS_EVTCHN_CONTEXT  Context,
    IN  XENBUS_EVTCHN_TYPE      Type,
    IN  PKSERVICE_ROUTINE       Callback,
    IN  PVOID                   Argument OPTIONAL,
    ...
    )
{
    va_list                     Arguments;
    PXENBUS_EVTCHN_DESCRIPTOR   Descriptor;
    ULONG                       LocalPort;
    KIRQL                       Irql;
    NTSTATUS                    status;

    KeRaiseIrql(DISPATCH_LEVEL, &Irql); // Prevent suspend

    Descriptor = __EvtchnAllocate(sizeof (XENBUS_EVTCHN_DESCRIPTOR));

    status = STATUS_NO_MEMORY;
    if (Descriptor == NULL)
        goto fail1;

    Descriptor->Magic = EVTCHN_DESCRIPTOR_MAGIC;

    (VOID) RtlCaptureStackBackTrace(1, 1, &Descriptor->Caller, NULL);    

    Descriptor->Type = Type;
    Descriptor->Callback = Callback;
    Descriptor->Argument = Argument;

    va_start(Arguments, Argument);
    switch (Type) {
    case EVTCHN_FIXED:
        status = __EvtchnOpenFixed(Descriptor, Arguments);
        break;

    case EVTCHN_UNBOUND:
        status = __EvtchnOpenUnbound(Descriptor, Arguments);
        break;

    case EVTCHN_INTER_DOMAIN:
        status = __EvtchnOpenInterDomain(Descriptor, Arguments);
        break;

    case EVTCHN_VIRQ:
        status = __EvtchnOpenVirq(Descriptor, Arguments);
        break;

    default:
        status = STATUS_INVALID_PARAMETER;
        break;
    }
    va_end(Arguments);

    if (!NT_SUCCESS(status))
        goto fail2;

    LocalPort = Descriptor->LocalPort;

    ASSERT3U(LocalPort, <, sizeof (Context->Descriptor) / sizeof (Context->Descriptor[0]));

    (VOID) __AcquireInterruptLock(Context->InterruptObject);

    ASSERT3P(Context->Descriptor[LocalPort], ==, NULL);
    Context->Descriptor[LocalPort] = Descriptor;
    Descriptor->Active = TRUE;

    InsertTailList(&Context->List, &Descriptor->ListEntry);

    __ReleaseInterruptLock(Context->InterruptObject, DISPATCH_LEVEL);

    KeLowerIrql(Irql);

    return Descriptor;

fail2:
    Error("fail2\n");

    Descriptor->Argument = NULL;
    Descriptor->Callback = NULL;
    Descriptor->Type = 0;

    Descriptor->Caller = NULL;

    Descriptor->Magic = 0;

    ASSERT(IsZeroMemory(Descriptor, sizeof (XENBUS_EVTCHN_DESCRIPTOR)));
    __EvtchnFree(Descriptor);

fail1:
    Error("fail1 (%08x)\n", status);

    KeLowerIrql(Irql);

    return NULL;
}

#pragma warning(push)
#pragma warning(disable:4701)

static BOOLEAN
EvtchnUnmask(
    IN  PXENBUS_EVTCHN_CONTEXT      Context,
    IN  PXENBUS_EVTCHN_DESCRIPTOR   Descriptor,
    IN  BOOLEAN                     Locked
    )
{
    KIRQL                           Irql;
    BOOLEAN                         Pending;

    ASSERT3U(Descriptor->Magic, ==, EVTCHN_DESCRIPTOR_MAGIC);

    if (!Locked)
        Irql = __AcquireInterruptLock(Context->InterruptObject);

    if (Descriptor->Active) {
        Pending = SHARED_INFO(EvtchnUnmask,
                              Context->SharedInfoInterface,
                              Descriptor->LocalPort);

        if (Pending) {
            BOOLEAN Mask;

            switch (Descriptor->Type) {
            case EVTCHN_FIXED:
                Mask = Descriptor->Parameters.Fixed.Mask;
                break;

            case EVTCHN_UNBOUND:
                Mask = Descriptor->Parameters.Unbound.Mask;
                break;

            case EVTCHN_INTER_DOMAIN:
                Mask = Descriptor->Parameters.InterDomain.Mask;
                break;

            case EVTCHN_VIRQ:
                Mask = FALSE;
                break;

            default:
                ASSERT(FALSE);
                break;
            }

            if (Mask)
                SHARED_INFO(EvtchnMask,
                            Context->SharedInfoInterface,
                            Descriptor->LocalPort);
        }
    }

    if (!Locked)
#pragma prefast(suppress:28121) // The function is not permitted to be called at the current IRQ level
        __ReleaseInterruptLock(Context->InterruptObject, Irql);

    return Pending;
}

#pragma warning(pop)

static NTSTATUS
EvtchnSend(
    IN  PXENBUS_EVTCHN_CONTEXT      Context,
    IN  PXENBUS_EVTCHN_DESCRIPTOR   Descriptor
    )
{
    KIRQL                           Irql;
    NTSTATUS                        status;

    UNREFERENCED_PARAMETER(Context);

    ASSERT3U(Descriptor->Magic, ==, EVTCHN_DESCRIPTOR_MAGIC);

    // Make sure we don't suspend
    KeRaiseIrql(DISPATCH_LEVEL, &Irql);

    status = STATUS_UNSUCCESSFUL;
    if (!Descriptor->Active)
        goto done;

    status = EventChannelSend(Descriptor->LocalPort);

done:
    KeLowerIrql(Irql);

    return status;
}

static FORCEINLINE BOOLEAN
__EvtchnCallback(
    IN  PXENBUS_EVTCHN_CONTEXT      Context,
    IN  PXENBUS_EVTCHN_DESCRIPTOR   Descriptor
    )
{
    BOOLEAN                         DoneSomething;

    UNREFERENCED_PARAMETER(Context);

    ASSERT(Descriptor != NULL);
    ASSERT(Descriptor->Active);

#pragma prefast(suppress:6387) // Param 1 could be NULL
    DoneSomething = Descriptor->Callback(NULL, Descriptor->Argument);

    return DoneSomething;
}

static BOOLEAN
EvtchnTrigger(
    IN  PXENBUS_EVTCHN_CONTEXT      Context,
    IN  PXENBUS_EVTCHN_DESCRIPTOR   Descriptor
    )
{
    KIRQL                           Irql;
    BOOLEAN                         DoneSomething;

    ASSERT3U(Descriptor->Magic, ==, EVTCHN_DESCRIPTOR_MAGIC);

    Irql = __AcquireInterruptLock(Context->InterruptObject);

    if (Descriptor->Active) {
        DoneSomething = __EvtchnCallback(Context, Descriptor);
    } else {
        Warning("[%d]: INVALID PORT\n", Descriptor->LocalPort);
        DoneSomething = FALSE;
    }

#pragma prefast(suppress:28121) // The function is not permitted to be called at the current IRQ level
    __ReleaseInterruptLock(Context->InterruptObject, Irql);

    return DoneSomething;
}

static VOID
EvtchnClose(
    IN  PXENBUS_EVTCHN_CONTEXT      Context,
    IN  PXENBUS_EVTCHN_DESCRIPTOR   Descriptor
    )
{
    KIRQL                           Irql;

    ASSERT3U(Descriptor->Magic, ==, EVTCHN_DESCRIPTOR_MAGIC);

    Irql = __AcquireInterruptLock(Context->InterruptObject);

    RemoveEntryList(&Descriptor->ListEntry);
    RtlZeroMemory(&Descriptor->ListEntry, sizeof (LIST_ENTRY));

    if (Descriptor->Active) {
        ULONG   LocalPort = Descriptor->LocalPort;

        ASSERT3U(LocalPort, <, sizeof (Context->Descriptor) / sizeof (Context->Descriptor[0]));

        Descriptor->Active = FALSE;

        SHARED_INFO(EvtchnMask,
                    Context->SharedInfoInterface,
                    LocalPort);

        if (Descriptor->Type != EVTCHN_FIXED)
            (VOID) EventChannelClose(LocalPort);

        ASSERT(Context->Descriptor[LocalPort] != NULL);
        Context->Descriptor[LocalPort] = NULL;
    }

#pragma prefast(suppress:28121) // The function is not permitted to be called at the current IRQ level
    __ReleaseInterruptLock(Context->InterruptObject, Irql);

    Descriptor->LocalPort = 0;
    RtlZeroMemory(&Descriptor->Parameters, sizeof (EVTCHN_PARAMETERS));

    Descriptor->Argument = NULL;
    Descriptor->Callback = NULL;
    Descriptor->Type = 0;

    Descriptor->Caller = NULL;

    Descriptor->Magic = 0;

    ASSERT(IsZeroMemory(Descriptor, sizeof (XENBUS_EVTCHN_DESCRIPTOR)));
    __EvtchnFree(Descriptor);
}

static ULONG
EvtchnPort(
    IN  PXENBUS_EVTCHN_CONTEXT      Context,
    IN  PXENBUS_EVTCHN_DESCRIPTOR   Descriptor
    )
{
    UNREFERENCED_PARAMETER(Context);

    ASSERT3U(Descriptor->Magic, ==, EVTCHN_DESCRIPTOR_MAGIC);
    ASSERT(Descriptor->Active);

    return Descriptor->LocalPort;
}

static VOID
EvtchnAcquire(
    IN  PXENBUS_EVTCHN_CONTEXT  Context
    )
{
    InterlockedIncrement(&Context->References);
}

static VOID
EvtchnRelease(
    IN  PXENBUS_EVTCHN_CONTEXT  Context
    )
{
    ASSERT(Context->References != 0);
    InterlockedDecrement(&Context->References);
}

#define EVTCHN_OPERATION(_Type, _Name, _Arguments) \
        Evtchn ## _Name,

static XENBUS_EVTCHN_OPERATIONS  Operations = {
    DEFINE_EVTCHN_OPERATIONS
};

#undef EVTCHN_OPERATION

static BOOLEAN
EvtchnPollCallback(
    IN  PVOID                   _Context,
    IN  ULONG                   LocalPort
    )
{
    PXENBUS_EVTCHN_CONTEXT      Context = _Context;
    PXENBUS_EVTCHN_DESCRIPTOR   Descriptor;
    BOOLEAN                     Mask;
    BOOLEAN                     DoneSomething;

    ASSERT3U(LocalPort, <, sizeof (Context->Descriptor) / sizeof (Context->Descriptor[0]));

    Descriptor = Context->Descriptor[LocalPort];

    if (Descriptor == NULL) {
        Warning("[%d]: INVALID PORT\n", LocalPort);

        SHARED_INFO(EvtchnMask,
                    Context->SharedInfoInterface,
                    LocalPort);

        DoneSomething = FALSE;
        goto done;
    }

    Mask = FALSE;

    switch (Descriptor->Type) {
    case EVTCHN_FIXED:
        Mask = Descriptor->Parameters.Fixed.Mask;
        break;

    case EVTCHN_UNBOUND:
        Mask = Descriptor->Parameters.Unbound.Mask;
        break;

    case EVTCHN_INTER_DOMAIN:
        Mask = Descriptor->Parameters.InterDomain.Mask;
        break;

    case EVTCHN_VIRQ:
        break;

    default:
        ASSERT(FALSE);
        break;
    }

    if (Mask)
        SHARED_INFO(EvtchnMask,
                    Context->SharedInfoInterface,
                    LocalPort);

    SHARED_INFO(EvtchnAck,
                Context->SharedInfoInterface,
                LocalPort);

    DoneSomething = __EvtchnCallback(Context, Descriptor);

done:
    return DoneSomething;
}

BOOLEAN
EvtchnInterrupt(
    IN  PXENBUS_EVTCHN_INTERFACE    Interface
    )
{
    PXENBUS_EVTCHN_CONTEXT          Context = Interface->Context;

    return SHARED_INFO(EvtchnPoll,
                       Context->SharedInfoInterface,
                       EvtchnPollCallback,
                       Context);
}

static FORCEINLINE VOID
__EvtchnInterruptEnable(
    IN  PXENBUS_EVTCHN_CONTEXT  Context
    )
{
    NTSTATUS                    status;

    status = HvmSetParam(HVM_PARAM_CALLBACK_IRQ,
                         Context->Interrupt->Raw.u.Interrupt.Vector);
    ASSERT(NT_SUCCESS(status));
}

static FORCEINLINE VOID
__EvtchnInterruptDisable(
    IN  PXENBUS_EVTCHN_CONTEXT  Context
    )
{
    NTSTATUS                    status;

    UNREFERENCED_PARAMETER(Context);

    status = HvmSetParam(HVM_PARAM_CALLBACK_IRQ, 0);
    ASSERT(NT_SUCCESS(status));
}

static VOID
EvtchnSuspendCallbackEarly(
    IN  PVOID                   Argument
    )
{
    PXENBUS_EVTCHN_CONTEXT      Context = Argument;
    PLIST_ENTRY                 ListEntry;

    for (ListEntry = Context->List.Flink;
         ListEntry != &Context->List;
         ListEntry = ListEntry->Flink) {
        PXENBUS_EVTCHN_DESCRIPTOR   Descriptor;

        Descriptor = CONTAINING_RECORD(ListEntry, XENBUS_EVTCHN_DESCRIPTOR, ListEntry);

        if (Descriptor->Active) {
            ULONG   LocalPort = Descriptor->LocalPort;

            ASSERT3U(LocalPort, <, sizeof (Context->Descriptor) / sizeof (Context->Descriptor[0]));

            Descriptor->Active = FALSE;

            ASSERT(Context->Descriptor[LocalPort] != NULL);
            Context->Descriptor[LocalPort] = NULL;
        }
    }

    if (Context->Enabled)
        __EvtchnInterruptEnable(Context);
}

static VOID
EvtchnDebugCallback(
    IN  PVOID                   Argument,
    IN  BOOLEAN                 Crashing
    )
{
    PXENBUS_EVTCHN_CONTEXT      Context = Argument;

    UNREFERENCED_PARAMETER(Crashing);

    if (!IsListEmpty(&Context->List)) {
        PLIST_ENTRY ListEntry;

        DEBUG(Printf,
              Context->DebugInterface,
              Context->DebugCallback,
              "EVENT CHANNELS:\n");

        for (ListEntry = Context->List.Flink;
                ListEntry != &Context->List;
                ListEntry = ListEntry->Flink) {
            PXENBUS_EVTCHN_DESCRIPTOR   Descriptor;
            PCHAR                       Name;
            ULONG_PTR                   Offset;

            Descriptor = CONTAINING_RECORD(ListEntry, XENBUS_EVTCHN_DESCRIPTOR, ListEntry);

            ModuleLookup((ULONG_PTR)Descriptor->Caller, &Name, &Offset);

            if (Name != NULL) {
                DEBUG(Printf,
                      Context->DebugInterface,
                      Context->DebugCallback,
                      "- (%04x) BY %s + %p [%s]\n",
                      Descriptor->LocalPort,
                      Name,
                      (PVOID)Offset,
                      (Descriptor->Active) ? "TRUE" : "FALSE");
            } else {
                DEBUG(Printf,
                      Context->DebugInterface,
                      Context->DebugCallback,
                      "- (%04x) BY %p [%s]\n",
                      Descriptor->LocalPort,
                      (PVOID)Descriptor->Caller,
                      (Descriptor->Active) ? "TRUE" : "FALSE");
            }

            switch (Descriptor->Type) {
            case EVTCHN_FIXED:
                DEBUG(Printf,
                      Context->DebugInterface,
                      Context->DebugCallback,
                      "FIXED: Mask = %s\n",
                      (Descriptor->Parameters.Fixed.Mask) ? "TRUE" : "FALSE");
                break;

            case EVTCHN_UNBOUND:
                DEBUG(Printf,
                      Context->DebugInterface,
                      Context->DebugCallback,
                      "UNBOUND: RemoteDomain = %u Mask = %s\n",
                      Descriptor->Parameters.Unbound.RemoteDomain,
                      (Descriptor->Parameters.Unbound.Mask) ? "TRUE" : "FALSE");
                break;

            case EVTCHN_INTER_DOMAIN:
                DEBUG(Printf,
                      Context->DebugInterface,
                      Context->DebugCallback,
                      "INTER_DOMAIN: RemoteDomain = %u RemotePort = %u Mask = %s\n",
                      Descriptor->Parameters.InterDomain.RemoteDomain,
                      Descriptor->Parameters.InterDomain.RemotePort,
                      (Descriptor->Parameters.InterDomain.Mask) ? "TRUE" : "FALSE");
                break;

            case EVTCHN_VIRQ:
                DEBUG(Printf,
                      Context->DebugInterface,
                      Context->DebugCallback,
                      "VIRQ: Index = %u\n",
                      Descriptor->Parameters.Virq.Index);
                break;

            default:
                break;
            }
        }
    }
}

NTSTATUS
EvtchnInitialize(
    IN  PXENBUS_FDO                 Fdo,
    OUT PXENBUS_EVTCHN_INTERFACE    Interface
    )
{
    PXENBUS_EVTCHN_CONTEXT          Context;
    NTSTATUS                        status;

    Trace("====>\n");

    Context = __EvtchnAllocate(sizeof (XENBUS_EVTCHN_CONTEXT));

    status = STATUS_NO_MEMORY;
    if (Context == NULL)
        goto fail1;

    InitializeListHead(&Context->List);

    Context->SharedInfoInterface = FdoGetSharedInfoInterface(Fdo);

    SHARED_INFO(Acquire, Context->SharedInfoInterface);

    Context->Interrupt = FdoGetResource(Fdo, INTERRUPT_RESOURCE);
    Context->InterruptObject = FdoGetInterruptObject(Fdo);

    Context->SuspendInterface = FdoGetSuspendInterface(Fdo);

    SUSPEND(Acquire, Context->SuspendInterface);

    status = SUSPEND(Register,
                     Context->SuspendInterface,
                     SUSPEND_CALLBACK_EARLY,
                     EvtchnSuspendCallbackEarly,
                     Context,
                     &Context->SuspendCallbackEarly);
    if (!NT_SUCCESS(status))
        goto fail2;

    Context->DebugInterface = FdoGetDebugInterface(Fdo);

    DEBUG(Acquire, Context->DebugInterface);

    status = DEBUG(Register,
                   Context->DebugInterface,
                   __MODULE__ "|EVTCHN",
                   EvtchnDebugCallback,
                   Context,
                   &Context->DebugCallback);
    if (!NT_SUCCESS(status))
        goto fail3;

    Interface->Context = Context;
    Interface->Operations = &Operations;

    Trace("<====\n");

    return STATUS_SUCCESS;

fail3:
    Error("fail3\n");

    DEBUG(Release, Context->DebugInterface);
    Context->DebugInterface = NULL;

    SUSPEND(Deregister,
            Context->SuspendInterface,
            Context->SuspendCallbackEarly);
    Context->SuspendCallbackEarly = NULL;

fail2:
    Error("fail2\n");

    SUSPEND(Release, Context->SuspendInterface);
    Context->SuspendInterface = NULL;

    (VOID) HvmSetParam(HVM_PARAM_CALLBACK_IRQ, 0);
    Context->InterruptObject = NULL;
    Context->Interrupt = NULL;

    SHARED_INFO(Release, Context->SharedInfoInterface);
    Context->SharedInfoInterface = NULL;

    RtlZeroMemory(&Context->List, sizeof (LIST_ENTRY));

    ASSERT(IsZeroMemory(Context, sizeof (XENBUS_EVTCHN_CONTEXT)));
    __EvtchnFree(Context);

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

VOID
EvtchnEnable(
    IN PXENBUS_EVTCHN_INTERFACE Interface
    )
{
    PXENBUS_EVTCHN_CONTEXT      Context = Interface->Context;

    ASSERT(!Context->Enabled);

    __EvtchnInterruptEnable(Context);
    Context->Enabled = TRUE;
}

VOID
EvtchnDisable(
    IN PXENBUS_EVTCHN_INTERFACE Interface
    )
{
    PXENBUS_EVTCHN_CONTEXT      Context = Interface->Context;

    ASSERT(Context->Enabled);

    Context->Enabled = FALSE;
    __EvtchnInterruptDisable(Context);
}

VOID
EvtchnTeardown(
    IN OUT  PXENBUS_EVTCHN_INTERFACE    Interface
    )
{
    PXENBUS_EVTCHN_CONTEXT              Context = Interface->Context;

    Trace("====>\n");

    if (!IsListEmpty(&Context->List))
        BUG("OUTSTANDING EVENT CHANNELS");

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

    Context->InterruptObject = NULL;
    Context->Interrupt = NULL;

    SHARED_INFO(Release, Context->SharedInfoInterface);
    Context->SharedInfoInterface = NULL;

    RtlZeroMemory(&Context->List, sizeof (LIST_ENTRY));

    ASSERT(IsZeroMemory(Context, sizeof (XENBUS_EVTCHN_CONTEXT)));
    __EvtchnFree(Context);

    RtlZeroMemory(Interface, sizeof (XENBUS_EVTCHN_INTERFACE));

    Trace("<====\n");
}
