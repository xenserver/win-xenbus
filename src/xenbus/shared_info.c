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
#include <xen.h>
#include <util.h>

#include "shared_info.h"
#include "fdo.h"
#include "dbg_print.h"
#include "assert.h"

struct _XENBUS_SHARED_INFO_CONTEXT {
    LONG                        References;
    PFN_NUMBER                  Pfn;
    shared_info_t               *Shared;
    PXENBUS_SUSPEND_INTERFACE   SuspendInterface;
    PXENBUS_SUSPEND_CALLBACK    SuspendCallbackEarly;
    PXENBUS_DEBUG_INTERFACE     DebugInterface;
    PXENBUS_DEBUG_CALLBACK      DebugCallback;
};

#define SHARED_INFO_TAG 'OFNI'

static FORCEINLINE PVOID
__SharedInfoAllocate(
    IN  ULONG   Length
    )
{
    return __AllocateNonPagedPoolWithTag(Length, SHARED_INFO_TAG);
}

static FORCEINLINE VOID
__SharedInfoFree(
    IN  PVOID   Buffer
    )
{
    __FreePoolWithTag(Buffer, SHARED_INFO_TAG);
}

static FORCEINLINE BOOLEAN
__SharedInfoSetBit(
    IN  ULONG_PTR volatile  *Mask,
    IN  ULONG               Bit
    )
{
    ULONG_PTR               Old;
    ULONG_PTR               New;

    ASSERT3U(Bit, <, sizeof (ULONG_PTR) * 8);

    KeMemoryBarrier();

    do {
        Old = *Mask;
        New = Old | ((ULONG_PTR)1 << Bit);
    } while (InterlockedCompareExchangePointer((PVOID *)Mask, (PVOID)New, (PVOID)Old) != (PVOID)Old);

    KeMemoryBarrier();

    return (Old & ((ULONG_PTR)1 << Bit)) ? FALSE : TRUE;    // return TRUE if we set the bit
}

static FORCEINLINE BOOLEAN
__SharedInfoClearBit(
    IN  ULONG_PTR volatile  *Mask,
    IN  ULONG               Bit
    )
{
    ULONG_PTR               Old;
    ULONG_PTR               New;

    ASSERT3U(Bit, <, sizeof (ULONG_PTR) * 8);

    KeMemoryBarrier();

    do {
        Old = *Mask;
        New = Old & ~((ULONG_PTR)1 << Bit);
    } while (InterlockedCompareExchangePointer((PVOID *)Mask, (PVOID)New, (PVOID)Old) != (PVOID)Old);

    KeMemoryBarrier();

    return (Old & ((ULONG_PTR)1 << Bit)) ? TRUE : FALSE;    // return TRUE if we cleared the bit
}

static FORCEINLINE BOOLEAN
__SharedInfoClearBitUnlocked(
    IN  ULONG_PTR   *Mask,
    IN  ULONG       Bit
    )
{
    ULONG_PTR       Old;

    ASSERT3U(Bit, <, sizeof (ULONG_PTR) * 8);

    KeMemoryBarrier();

    Old = *Mask;
    *Mask = Old & ~((ULONG_PTR)1 << Bit);

    return (Old & ((ULONG_PTR)1 << Bit)) ? TRUE : FALSE;    // return TRUE if we cleared the bit
}

static FORCEINLINE BOOLEAN
__SharedInfoTestBit(
    IN  ULONG_PTR   *Mask,
    IN  ULONG       Bit
    )
{
    ASSERT3U(Bit, <, sizeof (ULONG_PTR) * 8);

    KeMemoryBarrier();

    return (*Mask & ((ULONG_PTR)1 << Bit)) ? TRUE : FALSE;    // return TRUE if the bit is set
}

static FORCEINLINE VOID
__SharedInfoMaskAll(
    IN  PXENBUS_SHARED_INFO_CONTEXT Context
    )
{
    shared_info_t                   *Shared;
    ULONG                           Port;

    Shared = Context->Shared;

    for (Port = 0; Port < EVTCHN_SELECTOR_COUNT * EVTCHN_PER_SELECTOR; Port += EVTCHN_PER_SELECTOR) {
        ULONG SelectorBit;

        SelectorBit = Port / EVTCHN_PER_SELECTOR;

        Shared->evtchn_mask[SelectorBit] = (ULONG_PTR)-1;
    }
}

static BOOLEAN
SharedInfoEvtchnPoll(
    IN  PXENBUS_SHARED_INFO_CONTEXT Context,
    IN  BOOLEAN                     (*Function)(PVOID, ULONG),
    IN  PVOID                       Argument
    )
{
    shared_info_t                   *Shared;
    static ULONG                    Port;
    BOOLEAN                         DoneSomething;

    Shared = Context->Shared;

    DoneSomething = FALSE;

    for (;;) {
        UCHAR       Pending;
        ULONG_PTR   SelectorMask;

        KeMemoryBarrier();

        Pending = _InterlockedExchange8((CHAR *)&Shared->vcpu_info[0].evtchn_upcall_pending, 0);
        if (Pending == 0)
            break;

        SelectorMask = (ULONG_PTR)InterlockedExchangePointer((PVOID *)&Shared->vcpu_info[0].evtchn_pending_sel, (PVOID)0);

        KeMemoryBarrier();

        while (SelectorMask != 0) {
            ULONG   SelectorBit;
            ULONG   PortBit;

            SelectorBit = Port / EVTCHN_PER_SELECTOR;
            PortBit = Port % EVTCHN_PER_SELECTOR;

            if (__SharedInfoTestBit(&SelectorMask, SelectorBit)) {
                ULONG_PTR   PortMask;

                PortMask = Shared->evtchn_pending[SelectorBit];
                PortMask &= ~Shared->evtchn_mask[SelectorBit];

                while (PortMask != 0 && PortBit < EVTCHN_PER_SELECTOR) {
                    if (__SharedInfoTestBit(&PortMask, PortBit)) {
                        DoneSomething |= Function(Argument, (SelectorBit * EVTCHN_PER_SELECTOR) + PortBit);

                        (VOID) __SharedInfoClearBitUnlocked(&PortMask, PortBit);
                    }

                    PortBit++;
                }

                // Are we done with this selector?
                if (PortMask == 0)
                    (VOID) __SharedInfoClearBitUnlocked(&SelectorMask, SelectorBit);
            }

            Port = (SelectorBit + 1) * EVTCHN_PER_SELECTOR;

            if (Port >= EVTCHN_SELECTOR_COUNT * EVTCHN_PER_SELECTOR)
                Port = 0;
        }
    }

    return DoneSomething;
}

static VOID
SharedInfoEvtchnAck(
    IN  PXENBUS_SHARED_INFO_CONTEXT Context,
    IN  ULONG                       Port
    )
{
    shared_info_t                   *Shared;
    ULONG                           SelectorBit;
    ULONG                           PortBit;

    Shared = Context->Shared;

    SelectorBit = Port / EVTCHN_PER_SELECTOR;
    PortBit = Port % EVTCHN_PER_SELECTOR;

    (VOID) __SharedInfoClearBit(&Shared->evtchn_pending[SelectorBit], PortBit);
}

static VOID
SharedInfoEvtchnMask(
    IN  PXENBUS_SHARED_INFO_CONTEXT Context,
    IN  ULONG                       Port
    )
{
    shared_info_t                   *Shared;
    ULONG                           SelectorBit;
    ULONG                           PortBit;

    Shared = Context->Shared;

    SelectorBit = Port / EVTCHN_PER_SELECTOR;
    PortBit = Port % EVTCHN_PER_SELECTOR;

    (VOID) __SharedInfoSetBit(&Shared->evtchn_mask[SelectorBit], PortBit);
}

static BOOLEAN
SharedInfoEvtchnUnmask(
    IN  PXENBUS_SHARED_INFO_CONTEXT Context,
    IN  ULONG                       Port
    )
{
    shared_info_t                   *Shared;
    ULONG                           SelectorBit;
    ULONG                           PortBit;

    Shared = Context->Shared;

    SelectorBit = Port / EVTCHN_PER_SELECTOR;
    PortBit = Port % EVTCHN_PER_SELECTOR;

    // Check whether the port is masked
    if (!__SharedInfoClearBit(&Shared->evtchn_mask[SelectorBit], PortBit))
        return FALSE;

    KeMemoryBarrier();

    // If we cleared the mask then check whether something was pending
    if (!__SharedInfoClearBit(&Shared->evtchn_pending[SelectorBit], PortBit))
        return FALSE;

    return TRUE;
}

static LARGE_INTEGER
SharedInfoGetTime(
    IN  PXENBUS_SHARED_INFO_CONTEXT Context
    )
{
    shared_info_t                   *Shared;
    ULONG                           Version;
    ULONGLONG                       Seconds;
    ULONGLONG                       NanoSeconds;
    LARGE_INTEGER                   Now;
    TIME_FIELDS                     Time;
    KIRQL                           Irql;
    NTSTATUS                        status;

    // Make sure we don't suspend
    KeRaiseIrql(DISPATCH_LEVEL, &Irql); 

    Shared = Context->Shared;

    do {
        Version = Shared->wc_version;
        KeMemoryBarrier();

        Seconds = Shared->wc_sec;
        NanoSeconds = Shared->wc_nsec;
        KeMemoryBarrier();
    } while (Shared->wc_version != Version);

    // Get the number of nanoseconds since boot
    status = HvmGetTime(&Now);
    if (!NT_SUCCESS(status))
        Now.QuadPart = Shared->vcpu_info[0].time.system_time;

    KeLowerIrql(Irql);

    Trace("WALLCLOCK: Seconds = %llu NanoSeconds = %llu\n",
          Seconds,
          NanoSeconds);

    Trace("BOOT: Seconds = %llu NanoSeconds = %llu\n",
          Now.QuadPart / 1000000000ull,
          Now.QuadPart % 1000000000ull);

    // Convert wallclock from Unix epoch (1970) to Windows epoch (1601)
    Seconds += 11644473600ull;

    // Add in time since host boot
    Seconds += Now.QuadPart / 1000000000ull;
    NanoSeconds += Now.QuadPart % 1000000000ull;

    // Converto to system time format
    Now.QuadPart = (Seconds * 10000000ull) + (NanoSeconds / 100ull);

    RtlTimeToTimeFields(&Now, &Time);

    Trace("TOD: %04u/%02u/%02u %02u:%02u:%02u\n",
          Time.Year,
          Time.Month,
          Time.Day,
          Time.Hour,
          Time.Minute,
          Time.Second);

    return Now;
}

static VOID
SharedInfoAcquire(
    IN  PXENBUS_SHARED_INFO_CONTEXT Context
    )
{
    InterlockedIncrement(&Context->References);
}

static VOID
SharedInfoRelease(
    IN  PXENBUS_SHARED_INFO_CONTEXT Context
    )
{
    ASSERT(Context->References != 0);
    InterlockedDecrement(&Context->References);
}

#define SHARED_INFO_OPERATION(_Type, _Name, _Arguments) \
        SharedInfo ## _Name,

static XENBUS_SHARED_INFO_OPERATIONS  Operations = {
    DEFINE_SHARED_INFO_OPERATIONS
};

#undef SHARED_INFO_OPERATION

static FORCEINLINE VOID
__SharedInfoMap(
    IN  PXENBUS_SHARED_INFO_CONTEXT Context
    )
{
    NTSTATUS                        status;

    // This, unfortunately, seems to be a necessary hack to
    // get the domain wallclock updated correctly.
    // The HVM parameter in question is defined in Xen kernel
    // patch 32-on-64-geneva-drivers.patch.
#define HVM_PARAM_32BIT 8

#if defined(__i386__)
    status = HvmSetParam(HVM_PARAM_32BIT, 1);
#elif defined(__x86_64__)
    status = HvmSetParam(HVM_PARAM_32BIT, 0);
#else
#error 'Unrecognised architecture'
#endif
    ASSERT(NT_SUCCESS(status));

#undef  HVM_PARAM_32BIT

    status = MemoryAddToPhysmap(Context->Pfn,
                                XENMAPSPACE_shared_info,
                                0);
    ASSERT(NT_SUCCESS(status));
}

static FORCEINLINE VOID
__SharedInfoUnmap(
    IN  PXENBUS_SHARED_INFO_CONTEXT Context
    )
{
    UNREFERENCED_PARAMETER(Context);

    // Not clear what to do here
}

static VOID
SharedInfoSuspendCallbackEarly(
    IN  PVOID                   Argument
    )
{
    PXENBUS_SHARED_INFO_CONTEXT Context = Argument;

    __SharedInfoMap(Context);
    __SharedInfoMaskAll(Context);
}

static VOID
SharedInfoDebugCallback(
    IN  PVOID                   Argument,
    IN  BOOLEAN                 Crashing
    )
{
    PXENBUS_SHARED_INFO_CONTEXT Context = Argument;

    DEBUG(Printf,
          Context->DebugInterface,
          Context->DebugCallback,
          "Pfn = %p\n",
          (PVOID)Context->Pfn);

    if (!Crashing) {
        shared_info_t   *Shared;
        ULONG           Selector;

        Shared = Context->Shared;

        KeMemoryBarrier();

        DEBUG(Printf,
              Context->DebugInterface,
              Context->DebugCallback,
              "PENDING: %s SELECTOR MASK: %p\n",
              Shared->vcpu_info[0].evtchn_upcall_pending ? "TRUE" : "FALSE",
              (PVOID)Shared->vcpu_info[0].evtchn_pending_sel);

        for (Selector = 0; Selector < EVTCHN_SELECTOR_COUNT; Selector++) {
            DEBUG(Printf,
                  Context->DebugInterface,
                  Context->DebugCallback,
                  "PENDING: [%04x - %04x]: %p\n",
                  Selector * EVTCHN_PER_SELECTOR,
                  ((Selector + 1) * EVTCHN_PER_SELECTOR) - 1,
                  (PVOID)Shared->evtchn_pending[Selector]);

            DEBUG(Printf,
                  Context->DebugInterface,
                  Context->DebugCallback,
                  " MASKED: [%04x - %04x]: %p\n",
                  Selector * EVTCHN_PER_SELECTOR,
                  ((Selector + 1) * EVTCHN_PER_SELECTOR) - 1,
                  (PVOID)Shared->evtchn_mask[Selector]);
        }
    }
}

NTSTATUS
SharedInfoInitialize(
    IN  PXENBUS_FDO                     Fdo,
    OUT PXENBUS_SHARED_INFO_INTERFACE   Interface
    )
{
    PXENBUS_RESOURCE                    Memory;
    PHYSICAL_ADDRESS                    Address;
    PXENBUS_SHARED_INFO_CONTEXT         Context;
    NTSTATUS                            status;

    Trace("====>\n");

    Context = __SharedInfoAllocate(sizeof (XENBUS_SHARED_INFO_CONTEXT));

    status = STATUS_NO_MEMORY;
    if (Context == NULL)
        goto fail1;

    Memory = FdoGetResource(Fdo, MEMORY_RESOURCE);
    Context->Pfn = (PFN_NUMBER)(Memory->Translated.u.Memory.Start.QuadPart >> PAGE_SHIFT);

    __SharedInfoMap(Context);

    Memory->Translated.u.Memory.Start.QuadPart += PAGE_SIZE;

    ASSERT3U(Memory->Translated.u.Memory.Length, >=, PAGE_SIZE);
    Memory->Translated.u.Memory.Length -= PAGE_SIZE;

    Address.QuadPart = (ULONGLONG)Context->Pfn << PAGE_SHIFT;
    Context->Shared = (shared_info_t *)MmMapIoSpace(Address, PAGE_SIZE, MmCached);

    status = STATUS_UNSUCCESSFUL;
    if (Context->Shared == NULL)
        goto fail2;

    Info("shared_info_t *: %p\n", Context->Shared);

    __SharedInfoMaskAll(Context);

    Context->SuspendInterface = FdoGetSuspendInterface(Fdo);

    SUSPEND(Acquire, Context->SuspendInterface);

    status = SUSPEND(Register,
                     Context->SuspendInterface,
                     SUSPEND_CALLBACK_EARLY,
                     SharedInfoSuspendCallbackEarly,
                     Context,
                     &Context->SuspendCallbackEarly);
    if (!NT_SUCCESS(status))
        goto fail3;

    Context->DebugInterface = FdoGetDebugInterface(Fdo);

    DEBUG(Acquire, Context->DebugInterface);

    status = DEBUG(Register,
                   Context->DebugInterface,
                   __MODULE__ "|SHARED_INFO",
                   SharedInfoDebugCallback,
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

    Context->Shared = NULL;

fail2:
    Error("fail2\n");

    __SharedInfoUnmap(Context);

    Context->Pfn = 0;

    ASSERT(IsZeroMemory(Context, sizeof (XENBUS_SHARED_INFO_CONTEXT)));
    __SharedInfoFree(Context);

fail1:
    Error("fail1 (%08x)\n", status);

    RtlZeroMemory(Interface, sizeof (XENBUS_SHARED_INFO_INTERFACE));

    return status;
}

VOID
SharedInfoTeardown(
    IN OUT  PXENBUS_SHARED_INFO_INTERFACE   Interface
    )
{
    PXENBUS_SHARED_INFO_CONTEXT             Context = Interface->Context;

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

    Context->Shared = NULL;

    __SharedInfoUnmap(Context);

    Context->Pfn = 0;

    ASSERT(IsZeroMemory(Context, sizeof (XENBUS_SHARED_INFO_CONTEXT)));
    __SharedInfoFree(Context);

    RtlZeroMemory(Interface, sizeof (XENBUS_SHARED_INFO_INTERFACE));

    Trace("<====\n");
}

