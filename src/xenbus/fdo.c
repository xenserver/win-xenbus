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

#define INITGUID 1

#include <ntddk.h>
#include <wdmguid.h>
#include <ntstrsafe.h>
#include <stdlib.h>
#include <util.h>
#include <xen.h>

#include "names.h"
#include "registry.h"
#include "fdo.h"
#include "pdo.h"
#include "thread.h"
#include "mutex.h"
#include "shared_info.h"
#include "evtchn.h"
#include "debug.h"
#include "store.h"
#include "gnttab.h"
#include "suspend.h"
#include "sync.h"
#include "balloon.h"
#include "driver.h"
#include "log.h"
#include "assert.h"

#define FDO_TAG 'ODF'

#define MAXNAMELEN  128

struct _XENBUS_FDO {
    PXENBUS_DX                      Dx;
    PDEVICE_OBJECT                  LowerDeviceObject;
    PDEVICE_OBJECT                  PhysicalDeviceObject;
    DEVICE_CAPABILITIES             LowerDeviceCapabilities;
    BUS_INTERFACE_STANDARD          LowerBusInterface;
    ULONG                           Usage[DeviceUsageTypeDumpFile + 1];
    BOOLEAN                         NotDisableable;

    PXENBUS_THREAD                  SystemPowerThread;
    PIRP                            SystemPowerIrp;
    PXENBUS_THREAD                  DevicePowerThread;
    PIRP                            DevicePowerIrp;

    PXENBUS_THREAD                  ScanThread;
    KEVENT                          ScanEvent;
    PXENBUS_STORE_WATCH             ScanWatch;
    XENBUS_MUTEX                    Mutex;
    ULONG                           References;
    PXENBUS_THREAD                  SuspendThread;
    KEVENT                          SuspendEvent;
    PXENBUS_STORE_WATCH             SuspendWatch;
    PXENBUS_BALLOON                 Balloon;
    PXENBUS_THREAD                  BalloonThread;
    KEVENT                          BalloonEvent;
    PXENBUS_STORE_WATCH             BalloonWatch;

    XENBUS_RESOURCE                 Resource[RESOURCE_COUNT];
    PKINTERRUPT                     InterruptObject;
    BOOLEAN                         InterruptEnabled;

    XENBUS_SUSPEND_INTERFACE        SuspendInterface;
    XENBUS_SHARED_INFO_INTERFACE    SharedInfoInterface;
    XENBUS_EVTCHN_INTERFACE         EvtchnInterface;
    XENBUS_DEBUG_INTERFACE          DebugInterface;
    XENBUS_STORE_INTERFACE          StoreInterface;
    XENBUS_GNTTAB_INTERFACE         GnttabInterface;

    PXENBUS_EVTCHN_DESCRIPTOR       Evtchn;
    PXENBUS_SUSPEND_CALLBACK        SuspendCallbackLate;
};

static FORCEINLINE PVOID
__FdoAllocate(
    IN  ULONG   Length
    )
{
    return __AllocateNonPagedPoolWithTag(Length, FDO_TAG);
}

static FORCEINLINE VOID
__FdoFree(
    IN  PVOID   Buffer
    )
{
    __FreePoolWithTag(Buffer, FDO_TAG);
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

static FORCEINLINE VOID
__FdoSetDevicePnpState(
    IN  PXENBUS_FDO         Fdo,
    IN  DEVICE_PNP_STATE    State
    )
{
    PXENBUS_DX              Dx = Fdo->Dx;

    // We can never transition out of the deleted state
    ASSERT(Dx->DevicePnpState != Deleted || State == Deleted);

    Dx->PreviousDevicePnpState = Dx->DevicePnpState;
    Dx->DevicePnpState = State;
}

static FORCEINLINE VOID
__FdoRestoreDevicePnpState(
    IN  PXENBUS_FDO         Fdo,
    IN  DEVICE_PNP_STATE    State
    )
{
    PXENBUS_DX              Dx = Fdo->Dx;

    if (Dx->DevicePnpState == State)
        Dx->DevicePnpState = Dx->PreviousDevicePnpState;
}

static FORCEINLINE DEVICE_PNP_STATE
__FdoGetDevicePnpState(
    IN  PXENBUS_FDO Fdo
    )
{
    PXENBUS_DX      Dx = Fdo->Dx;

    return Dx->DevicePnpState;
}

static FORCEINLINE VOID
__FdoSetDevicePowerState(
    IN  PXENBUS_FDO         Fdo,
    IN  DEVICE_POWER_STATE  State
    )
{
    PXENBUS_DX              Dx = Fdo->Dx;

    Dx->DevicePowerState = State;
}

static FORCEINLINE DEVICE_POWER_STATE
__FdoGetDevicePowerState(
    IN  PXENBUS_FDO Fdo
    )
{
    PXENBUS_DX      Dx = Fdo->Dx;

    return Dx->DevicePowerState;
}

static FORCEINLINE VOID
__FdoSetSystemPowerState(
    IN  PXENBUS_FDO         Fdo,
    IN  SYSTEM_POWER_STATE  State
    )
{
    PXENBUS_DX              Dx = Fdo->Dx;

    Dx->SystemPowerState = State;
}

static FORCEINLINE SYSTEM_POWER_STATE
__FdoGetSystemPowerState(
    IN  PXENBUS_FDO Fdo
    )
{
    PXENBUS_DX      Dx = Fdo->Dx;

    return Dx->SystemPowerState;
}

static FORCEINLINE PDEVICE_OBJECT
__FdoGetPhysicalDeviceObject(
    IN  PXENBUS_FDO Fdo
    )
{
    return Fdo->PhysicalDeviceObject;
}

PDEVICE_OBJECT
FdoGetPhysicalDeviceObject(
    IN  PXENBUS_FDO Fdo
    )
{
    return __FdoGetPhysicalDeviceObject(Fdo);
}

PDMA_ADAPTER
FdoGetDmaAdapter(
    IN  PXENBUS_FDO         Fdo,
    IN  PDEVICE_DESCRIPTION DeviceDescriptor,
    OUT PULONG              NumberOfMapRegisters
    )
{
    PBUS_INTERFACE_STANDARD LowerBusInterface;

    LowerBusInterface = &Fdo->LowerBusInterface;

    return LowerBusInterface->GetDmaAdapter(LowerBusInterface->Context,
                                            DeviceDescriptor,
                                            NumberOfMapRegisters);
}

static FORCEINLINE NTSTATUS
__FdoSetName(
    IN  PXENBUS_FDO Fdo,
    IN  PWCHAR      Name
    )
{
    PXENBUS_DX      Dx = Fdo->Dx;
    UNICODE_STRING  Unicode;
    ANSI_STRING     Ansi;
    ULONG           Index;
    NTSTATUS        status;

    RtlInitUnicodeString(&Unicode, Name);

    Ansi.Buffer = Dx->Name;
    Ansi.MaximumLength = sizeof (Dx->Name);
    Ansi.Length = 0;

    status = RtlUnicodeStringToAnsiString(&Ansi, &Unicode, FALSE);
    if (!NT_SUCCESS(status))
        goto fail1;
    
    for (Index = 0; Dx->Name[Index] != '\0'; Index++) {
        if (!isalnum((UCHAR)Dx->Name[Index]))
            Dx->Name[Index] = '_';
    }

    return STATUS_SUCCESS;

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

static FORCEINLINE PCHAR
__FdoGetName(
    IN  PXENBUS_FDO Fdo
    )
{
    PXENBUS_DX      Dx = Fdo->Dx;

    return Dx->Name;
}

PCHAR
FdoGetName(
    IN  PXENBUS_FDO Fdo
    )
{
    return __FdoGetName(Fdo);
}

__drv_functionClass(IO_COMPLETION_ROUTINE)
__drv_sameIRQL
static NTSTATUS
__FdoDelegateIrp(
    IN  PDEVICE_OBJECT  DeviceObject,
    IN  PIRP            Irp,
    IN  PVOID           Context
    )
{
    PKEVENT             Event = Context;

    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Irp);

    KeSetEvent(Event, IO_NO_INCREMENT, FALSE);

    return STATUS_MORE_PROCESSING_REQUIRED;
}

NTSTATUS
FdoDelegateIrp(
    IN  PXENBUS_FDO     Fdo,
    IN  PIRP            Irp
    )
{
    PDEVICE_OBJECT      DeviceObject;
    PIO_STACK_LOCATION  StackLocation;
    PIRP                SubIrp;
    KEVENT              Event;
    PIO_STACK_LOCATION  SubStackLocation;
    NTSTATUS            status;

    ASSERT3U(KeGetCurrentIrql(), ==, PASSIVE_LEVEL);

    StackLocation = IoGetCurrentIrpStackLocation(Irp);

    // Find the top of the FDO stack and hold a reference
    DeviceObject = IoGetAttachedDeviceReference(Fdo->Dx->DeviceObject);

    // Get a new IRP for the FDO stack
    SubIrp = IoAllocateIrp(DeviceObject->StackSize, FALSE);

    status = STATUS_NO_MEMORY;
    if (SubIrp == NULL)
        goto done;

    // Copy in the information from the original IRP
    SubStackLocation = IoGetNextIrpStackLocation(SubIrp);

    KeInitializeEvent(&Event, NotificationEvent, FALSE);

    RtlCopyMemory(SubStackLocation, StackLocation,
                  FIELD_OFFSET(IO_STACK_LOCATION, CompletionRoutine));
    SubStackLocation->Control = 0;

    IoSetCompletionRoutine(SubIrp,
                           __FdoDelegateIrp,
                           &Event,
                           TRUE,
                           TRUE,
                           TRUE);

    // Default completion status
    SubIrp->IoStatus.Status = Irp->IoStatus.Status;

    status = IoCallDriver(DeviceObject, SubIrp);
    if (status == STATUS_PENDING) {
        (VOID) KeWaitForSingleObject(&Event,
                                     Executive,
                                     KernelMode,
                                     FALSE,
                                     NULL);
        status = SubIrp->IoStatus.Status;
    } else {
        ASSERT3U(status, ==, SubIrp->IoStatus.Status);
    }

    IoFreeIrp(SubIrp);

done:
    ObDereferenceObject(DeviceObject);

    return status;
}

__drv_functionClass(IO_COMPLETION_ROUTINE)
__drv_sameIRQL
static NTSTATUS
__FdoForwardIrpSynchronously(
    IN  PDEVICE_OBJECT  DeviceObject,
    IN  PIRP            Irp,
    IN  PVOID           Context
    )
{
    PKEVENT             Event = Context;

    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Irp);

    KeSetEvent(Event, IO_NO_INCREMENT, FALSE);

    return STATUS_MORE_PROCESSING_REQUIRED;
}

static NTSTATUS
FdoForwardIrpSynchronously(
    IN  PXENBUS_FDO Fdo,
    IN  PIRP        Irp
    )
{
    KEVENT          Event;
    NTSTATUS        status;

    ASSERT3U(KeGetCurrentIrql(), ==, PASSIVE_LEVEL);

    KeInitializeEvent(&Event, NotificationEvent, FALSE);

    IoCopyCurrentIrpStackLocationToNext(Irp);
    IoSetCompletionRoutine(Irp,
                           __FdoForwardIrpSynchronously,
                           &Event,
                           TRUE,
                           TRUE,
                           TRUE);

    status = IoCallDriver(Fdo->LowerDeviceObject, Irp);
    if (status == STATUS_PENDING) {
        (VOID) KeWaitForSingleObject(&Event,
                                     Executive,
                                     KernelMode,
                                     FALSE,
                                     NULL);
        status = Irp->IoStatus.Status;
    } else {
        ASSERT3U(status, ==, Irp->IoStatus.Status);
    }

    return status;
}

VOID
FdoAddPhysicalDeviceObject(
    IN  PXENBUS_FDO     Fdo,
    IN  PXENBUS_PDO     Pdo
    )
{
    PDEVICE_OBJECT      DeviceObject;
    PXENBUS_DX          Dx;

    DeviceObject = PdoGetDeviceObject(Pdo);
    Dx = (PXENBUS_DX)DeviceObject->DeviceExtension;
    ASSERT3U(Dx->Type, ==, PHYSICAL_DEVICE_OBJECT);

    InsertTailList(&Fdo->Dx->ListEntry, &Dx->ListEntry);
    ASSERT3U(Fdo->References, !=, 0);
    Fdo->References++;

    PdoResume(Pdo);
}

VOID
FdoRemovePhysicalDeviceObject(
    IN  PXENBUS_FDO     Fdo,
    IN  PXENBUS_PDO     Pdo
    )
{
    PDEVICE_OBJECT      DeviceObject;
    PXENBUS_DX          Dx;

    DeviceObject = PdoGetDeviceObject(Pdo);
    Dx = (PXENBUS_DX)DeviceObject->DeviceExtension;
    ASSERT3U(Dx->Type, ==, PHYSICAL_DEVICE_OBJECT);

    PdoSuspend(Pdo);

    RemoveEntryList(&Dx->ListEntry);
    ASSERT3U(Fdo->References, !=, 0);
    --Fdo->References;

    if (Fdo->ScanThread)
        ThreadWake(Fdo->ScanThread);
}

static FORCEINLINE VOID
__FdoAcquireMutex(
    IN  PXENBUS_FDO     Fdo
    )
{
    AcquireMutex(&Fdo->Mutex);
}

VOID
FdoAcquireMutex(
    IN  PXENBUS_FDO     Fdo
    )
{
    __FdoAcquireMutex(Fdo);
}

static FORCEINLINE VOID
__FdoReleaseMutex(
    IN  PXENBUS_FDO     Fdo
    )
{
    ReleaseMutex(&Fdo->Mutex);
}

VOID
FdoReleaseMutex(
    IN  PXENBUS_FDO     Fdo
    )
{
    __FdoReleaseMutex(Fdo);

    if (Fdo->References == 0)
        FdoDestroy(Fdo);
}

static FORCEINLINE BOOLEAN
__FdoEnumerate(
    IN  PXENBUS_FDO     Fdo,
    IN  PANSI_STRING    Classes
    )
{
    BOOLEAN             NeedInvalidate;
    PLIST_ENTRY         ListEntry;
    ULONG               Index;

    Trace("====>\n");

    NeedInvalidate = FALSE;

    if (DriverParameters.CreatePDOs == 0)
        goto done;

    __FdoAcquireMutex(Fdo);

    ListEntry = Fdo->Dx->ListEntry.Flink;
    while (ListEntry != &Fdo->Dx->ListEntry) {
        PLIST_ENTRY     Next = ListEntry->Flink;
        PXENBUS_DX      Dx = CONTAINING_RECORD(ListEntry, XENBUS_DX, ListEntry);
        PCHAR           Name = Dx->Name;
        PXENBUS_PDO     Pdo = Dx->Pdo;
        BOOLEAN         Missing;

        Name = PdoGetName(Pdo);
        Missing = TRUE;

        // If the PDO exists in either the class list or the synthetic list
        // from xenstore then we don't want to remove it.

        for (Index = 0; Classes[Index].Buffer != NULL; Index++) {
            PANSI_STRING Class = &Classes[Index];

            if (strcmp(Name, Class->Buffer) == 0) {
                Missing = FALSE;
                Class->Length = 0;  // avoid duplication
                break;
            }
        }

        if (Missing &&
            !PdoIsMissing(Pdo) &&
            PdoGetDevicePnpState(Pdo) != Deleted) {
            PdoSetMissing(Pdo, "device disappeared");

            // If the PDO has not yet been enumerated then we can go ahead
            // and mark it as deleted, otherwise we need to notify PnP manager and
            // wait for the REMOVE_DEVICE IRP.
            if (PdoGetDevicePnpState(Pdo) == Present) {
                PdoSetDevicePnpState(Pdo, Deleted);
                PdoDestroy(Pdo);
            } else {
                NeedInvalidate = TRUE;
            }
        }

        ListEntry = Next;
    }

    // Check the class list from xenstore against the supported list
    for (Index = 0; Classes[Index].Buffer != NULL; Index++) {
        PANSI_STRING    Class = &Classes[Index];
        ULONG           Entry;
        BOOLEAN         Supported;

        Supported = FALSE;

        for (Entry = 0;
             DriverParameters.SupportedClasses != NULL && DriverParameters.SupportedClasses[Entry].Buffer != NULL;
             Entry++) {
            if (strncmp(Class->Buffer,
                        DriverParameters.SupportedClasses[Entry].Buffer,
                        Class->Length) == 0) {
                Supported = TRUE;
                break;
            }
        }

        if (!Supported)
            Class->Length = 0;  // avoid creation
    }

    // Walk the class list and create PDOs for any new classes

    for (Index = 0; Classes[Index].Buffer != NULL; Index++) {
        PANSI_STRING Class = &Classes[Index];

        if (Class->Length != 0) {
            NTSTATUS    status;

            status = PdoCreate(Fdo, Class);
            if (NT_SUCCESS(status))
                NeedInvalidate = TRUE;
        }
    }

    __FdoReleaseMutex(Fdo);

done:
    Trace("<====\n");

    return NeedInvalidate;
}

static FORCEINLINE PANSI_STRING
__FdoMultiSzToUpcaseAnsi(
    IN  PCHAR       Buffer
    )
{
    PANSI_STRING    Ansi;
    LONG            Index;
    LONG            Count;
    NTSTATUS        status;

    Index = 0;
    Count = 0;
    for (;;) {
        if (Buffer[Index] == '\0') {
            Count++;
            Index++;

            // Check for double NUL
            if (Buffer[Index] == '\0')
                break;
        } else {
            Buffer[Index] = (CHAR)toupper(Buffer[Index]);
            Index++;
        }
    }

    Ansi = __FdoAllocate(sizeof (ANSI_STRING) * (Count + 1));

    status = STATUS_NO_MEMORY;
    if (Ansi == NULL)
        goto fail1;

    for (Index = 0; Index < Count; Index++) {
        ULONG   Length;

        Length = (ULONG)strlen(Buffer);
        Ansi[Index].MaximumLength = (USHORT)(Length + 1);
        Ansi[Index].Buffer = __FdoAllocate(Ansi[Index].MaximumLength);

        status = STATUS_NO_MEMORY;
        if (Ansi[Index].Buffer == NULL)
            goto fail2;

        RtlCopyMemory(Ansi[Index].Buffer, Buffer, Length);
        Ansi[Index].Length = (USHORT)Length;

        Buffer += Length + 1;
    }

    return Ansi;

fail2:
    Error("fail2\n");

    while (--Index >= 0)
        __FdoFree(Ansi[Index].Buffer);

    __FdoFree(Ansi);

fail1:
    Error("fail1 (%08x)\n", status);

    return NULL;
}

static FORCEINLINE VOID
__FdoFreeAnsi(
    IN  PANSI_STRING    Ansi
    )
{
    ULONG               Index;

    for (Index = 0; Ansi[Index].Buffer != NULL; Index++)
        __FdoFree(Ansi[Index].Buffer);
        
    __FdoFree(Ansi);
}

static FORCEINLINE PANSI_STRING
__FdoCombineAnsi(
    IN  PANSI_STRING    AnsiA,
    IN  PANSI_STRING    AnsiB
    )
{
    LONG                Count;
    ULONG               Index;
    PANSI_STRING        Ansi;
    NTSTATUS            status;

    Count = 0;

    for (Index = 0;
         AnsiA != NULL && AnsiA[Index].Buffer != NULL; 
         Index++)
        Count++;

    for (Index = 0;
         AnsiB != NULL && AnsiB[Index].Buffer != NULL; 
         Index++)
        Count++;

    Ansi = __FdoAllocate(sizeof (ANSI_STRING) * (Count + 1));

    status = STATUS_NO_MEMORY;
    if (Ansi == NULL)
        goto fail1;

    Count = 0;

    for (Index = 0;
         AnsiA != NULL && AnsiA[Index].Buffer != NULL; 
         Index++) {
        USHORT  Length;

        Length = AnsiA[Index].MaximumLength;

        Ansi[Count].MaximumLength = Length;
        Ansi[Count].Buffer = __FdoAllocate(Length);

        status = STATUS_NO_MEMORY;
        if (Ansi[Count].Buffer == NULL)
            goto fail2;

        RtlCopyMemory(Ansi[Count].Buffer, AnsiA[Index].Buffer, Length);
        Ansi[Count].Length = AnsiA[Index].Length;

        Count++;
    }

    for (Index = 0;
         AnsiB != NULL && AnsiB[Index].Buffer != NULL; 
         Index++) {
        USHORT  Length;

        Length = AnsiB[Index].MaximumLength;

        Ansi[Count].MaximumLength = Length;
        Ansi[Count].Buffer = __FdoAllocate(Length);

        status = STATUS_NO_MEMORY;
        if (Ansi[Count].Buffer == NULL)
            goto fail3;

        RtlCopyMemory(Ansi[Count].Buffer, AnsiB[Index].Buffer, Length);
        Ansi[Count].Length = AnsiB[Index].Length;

        Count++;
    }

    return Ansi;

fail3:
    Error("fail3\n");

fail2:
    Error("fail2\n");

    while (--Count >= 0) 
        __FdoFree(Ansi[Count].Buffer);

    __FdoFree(Ansi);

fail1:
    Error("fail1 (%08x)\n", status);

    return NULL;
}

static NTSTATUS
FdoScan(
    IN  PXENBUS_THREAD  Self,
    IN  PVOID           Context
    )
{
    PXENBUS_FDO         Fdo = Context;
    PKEVENT             Event;
    NTSTATUS            status;

    Trace("====>\n");

    Event = ThreadGetEvent(Self);

    for (;;) {
        PCHAR           Buffer;
        PANSI_STRING    StoreClasses;
        PANSI_STRING    Classes;
        BOOLEAN         NeedInvalidate;

        Trace("waiting...\n");

        (VOID) KeWaitForSingleObject(Event,
                                     Executive,
                                     KernelMode,
                                     FALSE,
                                     NULL);
        KeClearEvent(Event);

        Trace("awake\n");

        if (ThreadIsAlerted(Self))
            break;

        STORE(Acquire, &Fdo->StoreInterface);

        if (__FdoGetDevicePnpState(Fdo) != Started)
            goto loop;

        status = STORE(Directory,
                       &Fdo->StoreInterface,
                       NULL,
                       NULL,
                       "device",
                       &Buffer);
        if (NT_SUCCESS(status)) {
            StoreClasses = __FdoMultiSzToUpcaseAnsi(Buffer);
            STORE(Free,
                  &Fdo->StoreInterface,
                  Buffer);
        } else {
            StoreClasses = NULL;
        }

        Classes = __FdoCombineAnsi(StoreClasses, DriverParameters.SyntheticClasses);

        if (StoreClasses != NULL)
            __FdoFreeAnsi(StoreClasses);

        if (Classes == NULL)
            goto loop;

        NeedInvalidate = __FdoEnumerate(Fdo, Classes);

        __FdoFreeAnsi(Classes);

        if (NeedInvalidate) {
            NeedInvalidate = FALSE;
            IoInvalidateDeviceRelations(__FdoGetPhysicalDeviceObject(Fdo), 
                                        BusRelations);
        }

loop:
        STORE(Release, &Fdo->StoreInterface);

        KeSetEvent(&Fdo->ScanEvent, IO_NO_INCREMENT, FALSE);
    }

    KeSetEvent(&Fdo->ScanEvent, IO_NO_INCREMENT, FALSE);

    Trace("<====\n");
    return STATUS_SUCCESS;
}

static NTSTATUS
FdoSuspend(
    IN  PXENBUS_THREAD  Self,
    IN  PVOID           Context
    )
{
    PXENBUS_FDO         Fdo = Context;
    PKEVENT             Event;

    Trace("====>\n");

    // We really want to know what CPU this thread will run on
    KeSetSystemAffinityThread((KAFFINITY)1);

    Event = ThreadGetEvent(Self);

    for (;;) {
        PCHAR       Buffer;
        NTSTATUS    status;

        Trace("waiting...\n");

        (VOID) KeWaitForSingleObject(Event,
                                     Executive,
                                     KernelMode,
                                     FALSE,
                                     NULL);
        KeClearEvent(Event);

        Trace("awake\n");

        if (ThreadIsAlerted(Self))
            break;

        STORE(Acquire, &Fdo->StoreInterface);

        if (__FdoGetDevicePowerState(Fdo) != PowerDeviceD0)
            goto loop;

        status = STORE(Read,
                       &Fdo->StoreInterface,
                       NULL,
                       "control",
                       "shutdown",
                       &Buffer);
        if (!NT_SUCCESS(status))
            goto loop;

        if (strcmp(Buffer, "suspend") == 0) {
            (VOID) STORE(Remove,
                         &Fdo->StoreInterface,
                         NULL,
                         "control",
                         "shutdown");

            SuspendTrigger(&Fdo->SuspendInterface);
        }

        STORE(Free,
              &Fdo->StoreInterface,
              Buffer);

loop:
        STORE(Release, &Fdo->StoreInterface);

        KeSetEvent(&Fdo->SuspendEvent, IO_NO_INCREMENT, FALSE);
    }

    KeSetEvent(&Fdo->SuspendEvent, IO_NO_INCREMENT, FALSE);

    Trace("<====\n");
    return STATUS_SUCCESS;
}

#define TIME_US(_us)            ((_us) * 10)
#define TIME_MS(_ms)            (TIME_US((_ms) * 1000))
#define TIME_S(_s)              (TIME_MS((_s) * 1000))
#define TIME_RELATIVE(_t)       (-(_t))

static NTSTATUS
FdoBalloon(
    IN  PXENBUS_THREAD  Self,
    IN  PVOID           Context
    )
{
    PXENBUS_FDO         Fdo = Context;
    PKEVENT             Event;
    BOOLEAN             Active;
    static ULONGLONG    Maximum;    // Should never change in the lifetime of the VM
    NTSTATUS            status;

    Trace("====>\n");

    Event = ThreadGetEvent(Self);

    Active = FALSE;

    for (;;) {
        PCHAR       Buffer;
        ULONGLONG   Target;
        BOOLEAN     AllowInflation;
        BOOLEAN     AllowDeflation;

        if (!Active) {
            Trace("waiting...\n");

            (VOID) KeWaitForSingleObject(Event,
                                         Executive,
                                         KernelMode,
                                         FALSE,
                                         NULL);
            KeClearEvent(Event);

            Trace("awake\n");
        }

        if (ThreadIsAlerted(Self))
            break;

        STORE(Acquire, &Fdo->StoreInterface);

        if (__FdoGetDevicePowerState(Fdo) != PowerDeviceD0)
            goto loop;

        if (Maximum == 0) {
            status = STORE(Read,
                           &Fdo->StoreInterface,
                           NULL,
                           "memory",
                           "static-max",
                           &Buffer);
            if (!NT_SUCCESS(status))
                goto loop;

            Maximum = _strtoui64(Buffer, NULL, 10) / 4;
            STORE(Free,
                  &Fdo->StoreInterface,
                  Buffer);
        }

        status = STORE(Read,
                       &Fdo->StoreInterface,
                       NULL,
                       "memory",
                       "target",
                       &Buffer);
        if (!NT_SUCCESS(status))
            goto loop;

        Target = _strtoui64(Buffer, NULL, 10) / 4;
        STORE(Free,
              &Fdo->StoreInterface,
              Buffer);

        if (Target > Maximum)
            Target = Maximum;

        Info("Target = %llu page(s)\n", Maximum - Target);

        status = STORE(Read,
                       &Fdo->StoreInterface,
                       NULL,
                       "FIST/balloon",
                       "inflation",
                       &Buffer);
        if (NT_SUCCESS(status)) {
            AllowInflation = !strtol(Buffer, NULL, 2);
            STORE(Free,
                  &Fdo->StoreInterface,
                  Buffer);
        } else {
            AllowInflation = TRUE;
        }

        if (!AllowInflation)
            Warning("inflation disallowed\n");

        status = STORE(Read,
                       &Fdo->StoreInterface,
                       NULL,
                       "FIST/balloon",
                       "deflation",
                       &Buffer);
        if (NT_SUCCESS(status)) {
            AllowDeflation = !strtol(Buffer, NULL, 2);
            STORE(Free,
                  &Fdo->StoreInterface,
                  Buffer);
        } else {
            AllowDeflation = TRUE;
        }

        if (!AllowDeflation)
            Warning("deflation disallowed\n");

        Active = TRUE;
        (VOID) STORE(Printf,
                     &Fdo->StoreInterface,
                     NULL,
                     "control",
                     "balloon-active",
                     "%u",
                     1);

        if (!BalloonAdjust(Fdo->Balloon,
                           Maximum - Target,
                           AllowInflation,
                           AllowDeflation)) {
            LARGE_INTEGER   Timeout;

            Timeout.QuadPart = TIME_RELATIVE(TIME_S(1));

            KeDelayExecutionThread(KernelMode, FALSE, &Timeout);
            goto loop;
        }

        Active = FALSE;
        (VOID) STORE(Remove,
                     &Fdo->StoreInterface,
                     NULL,
                     "control",
                     "balloon-active");

loop:
        STORE(Release, &Fdo->StoreInterface);

        if (!Active)
            KeSetEvent(&Fdo->BalloonEvent, IO_NO_INCREMENT, FALSE);
    }

    ASSERT3U(BalloonGetSize(Fdo->Balloon), ==, 0);

    KeSetEvent(&Fdo->BalloonEvent, IO_NO_INCREMENT, FALSE);

    Trace("<====\n");
    return STATUS_SUCCESS;
}

static DECLSPEC_NOINLINE VOID
FdoParseResources(
    IN  PXENBUS_FDO             Fdo,
    IN  PCM_RESOURCE_LIST       RawResourceList,
    IN  PCM_RESOURCE_LIST       TranslatedResourceList
    )
{
    PCM_PARTIAL_RESOURCE_LIST   RawPartialList;
    PCM_PARTIAL_RESOURCE_LIST   TranslatedPartialList;
    ULONG                       Index;

    ASSERT3U(RawResourceList->Count, ==, 1);
    RawPartialList = &RawResourceList->List[0].PartialResourceList;

    ASSERT3U(RawPartialList->Version, ==, 1);
    ASSERT3U(RawPartialList->Revision, ==, 1);

    ASSERT3U(TranslatedResourceList->Count, ==, 1);
    TranslatedPartialList = &TranslatedResourceList->List[0].PartialResourceList;

    ASSERT3U(TranslatedPartialList->Version, ==, 1);
    ASSERT3U(TranslatedPartialList->Revision, ==, 1);

    for (Index = 0; Index < TranslatedPartialList->Count; Index++) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR RawPartialDescriptor;
        PCM_PARTIAL_RESOURCE_DESCRIPTOR TranslatedPartialDescriptor;

        RawPartialDescriptor = &RawPartialList->PartialDescriptors[Index];
        TranslatedPartialDescriptor = &TranslatedPartialList->PartialDescriptors[Index];

        Info("%s: [%d] %02x:%s\n",
             __FdoGetName(Fdo),
             Index,
             TranslatedPartialDescriptor->Type,
             PartialResourceDescriptorTypeName(TranslatedPartialDescriptor->Type));

        switch (TranslatedPartialDescriptor->Type) {
        case CmResourceTypeMemory:
            Info("RAW: SharedDisposition=%02x Flags=%04x Start = %08x.%08x Length = %08x\n",
                 RawPartialDescriptor->ShareDisposition,
                 RawPartialDescriptor->Flags,
                 RawPartialDescriptor->u.Memory.Start.HighPart,
                 RawPartialDescriptor->u.Memory.Start.LowPart,
                 RawPartialDescriptor->u.Memory.Length);

            Info("TRANSLATED: SharedDisposition=%02x Flags=%04x Start = %08x.%08x Length = %08x\n",
                 TranslatedPartialDescriptor->ShareDisposition,
                 TranslatedPartialDescriptor->Flags,
                 TranslatedPartialDescriptor->u.Memory.Start.HighPart,
                 TranslatedPartialDescriptor->u.Memory.Start.LowPart,
                 TranslatedPartialDescriptor->u.Memory.Length);

            Fdo->Resource[MEMORY_RESOURCE].Raw = *RawPartialDescriptor;
            Fdo->Resource[MEMORY_RESOURCE].Translated = *TranslatedPartialDescriptor;

            break;

        case CmResourceTypeInterrupt:
            Info("RAW: SharedDisposition=%02x Flags=%04x Level = %08x Vector = %08x Affinity = %p\n",
                 RawPartialDescriptor->ShareDisposition,
                 RawPartialDescriptor->Flags,
                 RawPartialDescriptor->u.Interrupt.Level,
                 RawPartialDescriptor->u.Interrupt.Vector,
                 (PVOID)RawPartialDescriptor->u.Interrupt.Affinity);

            Info("TRANSLATED: SharedDisposition=%02x Flags=%04x Level = %08x Vector = %08x Affinity = %p\n",
                 TranslatedPartialDescriptor->ShareDisposition,
                 TranslatedPartialDescriptor->Flags,
                 TranslatedPartialDescriptor->u.Interrupt.Level,
                 TranslatedPartialDescriptor->u.Interrupt.Vector,
                 (PVOID)TranslatedPartialDescriptor->u.Interrupt.Affinity);

            Fdo->Resource[INTERRUPT_RESOURCE].Raw = *RawPartialDescriptor;
            Fdo->Resource[INTERRUPT_RESOURCE].Translated = *TranslatedPartialDescriptor;

            break;

        default:
            break;
        }
    }

    Trace("<====\n");
}

static FORCEINLINE PXENBUS_RESOURCE
__FdoGetResource(
    IN  PXENBUS_FDO             Fdo,
    IN  XENBUS_RESOURCE_TYPE    Type
    )
{
    ASSERT3U(Type, <, RESOURCE_COUNT);

    return &Fdo->Resource[Type];
}

PXENBUS_RESOURCE
FdoGetResource(
    IN  PXENBUS_FDO             Fdo,
    IN  XENBUS_RESOURCE_TYPE    Type
    )
{
    return __FdoGetResource(Fdo, Type);
}

KSERVICE_ROUTINE    FdoInterrupt;

BOOLEAN
FdoInterrupt(
    IN  PKINTERRUPT         InterruptObject,
    IN  PVOID               Context
    )
{
    PXENBUS_FDO             Fdo = Context;

    UNREFERENCED_PARAMETER(InterruptObject);

    ASSERT(Fdo != NULL);

    if (!Fdo->InterruptEnabled)
        return FALSE;

    return EvtchnInterrupt(&Fdo->EvtchnInterface);
}

static FORCEINLINE VOID
__FdoSetInterruptObject(
    IN  PXENBUS_FDO Fdo,
    IN  PKINTERRUPT InterruptObject
    )
{
    Fdo->InterruptObject = InterruptObject;
}

static FORCEINLINE PKINTERRUPT
__FdoGetInterruptObject(
    IN  PXENBUS_FDO Fdo
    )
{
    return Fdo->InterruptObject;
}

PKINTERRUPT
FdoGetInterruptObject(
    IN  PXENBUS_FDO Fdo
    )
{
    return __FdoGetInterruptObject(Fdo);
}

static NTSTATUS
FdoConnectInterrupt(
    IN  PXENBUS_FDO                 Fdo
    )
{
    PXENBUS_RESOURCE                Interrupt;
    IO_CONNECT_INTERRUPT_PARAMETERS Connect;
    PKINTERRUPT                     InterruptObject;
    NTSTATUS                        status;

    Interrupt = __FdoGetResource(Fdo, INTERRUPT_RESOURCE);

    RtlZeroMemory(&Connect, sizeof (IO_CONNECT_INTERRUPT_PARAMETERS));
    Connect.Version = CONNECT_FULLY_SPECIFIED;
    Connect.FullySpecified.PhysicalDeviceObject = __FdoGetPhysicalDeviceObject(Fdo);
    Connect.FullySpecified.SynchronizeIrql = (KIRQL)Interrupt->Translated.u.Interrupt.Level;
    Connect.FullySpecified.ShareVector = (BOOLEAN)(Interrupt->Translated.ShareDisposition == CmResourceShareShared);
    Connect.FullySpecified.Vector = Interrupt->Translated.u.Interrupt.Vector;
    Connect.FullySpecified.Irql = (KIRQL)Interrupt->Translated.u.Interrupt.Level;
    Connect.FullySpecified.InterruptMode = (Interrupt->Translated.Flags & CM_RESOURCE_INTERRUPT_LATCHED) ?
                                           Latched :
                                           LevelSensitive;
    Connect.FullySpecified.ProcessorEnableMask = Interrupt->Translated.u.Interrupt.Affinity;
    Connect.FullySpecified.InterruptObject = &InterruptObject;
    Connect.FullySpecified.ServiceRoutine = FdoInterrupt;
    Connect.FullySpecified.ServiceContext = Fdo;

    status = IoConnectInterruptEx(&Connect);
    if (!NT_SUCCESS(status))
        goto fail1;

    __FdoSetInterruptObject(Fdo, InterruptObject);

    return STATUS_SUCCESS;

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

static VOID
FdoDisconnectInterrupt(
    IN  PXENBUS_FDO                     Fdo
    )
{
    PKINTERRUPT                         InterruptObject;
    IO_DISCONNECT_INTERRUPT_PARAMETERS  Disconnect;

    InterruptObject = __FdoGetInterruptObject(Fdo);
    __FdoSetInterruptObject(Fdo, NULL);

    RtlZeroMemory(&Disconnect, sizeof (IO_DISCONNECT_INTERRUPT_PARAMETERS));
    Disconnect.Version = CONNECT_FULLY_SPECIFIED;
    Disconnect.ConnectionContext.InterruptObject = InterruptObject;

    IoDisconnectInterruptEx(&Disconnect);
}

static FORCEINLINE VOID
__FdoEnableInterrupt(
    IN  PXENBUS_FDO Fdo
    )
{
    PKINTERRUPT     InterruptObject;
    KIRQL           Irql;

    InterruptObject = __FdoGetInterruptObject(Fdo);

    Irql = __AcquireInterruptLock(InterruptObject);
    Fdo->InterruptEnabled = TRUE;

    __ReleaseInterruptLock(InterruptObject, Irql);
}
    
static FORCEINLINE VOID
__FdoDisableInterrupt(
    IN  PXENBUS_FDO Fdo
    )
{
    PKINTERRUPT     InterruptObject;
    KIRQL           Irql;

    InterruptObject = __FdoGetInterruptObject(Fdo);

    Irql = __AcquireInterruptLock(InterruptObject);
    Fdo->InterruptEnabled = FALSE;

    __ReleaseInterruptLock(InterruptObject, Irql);
}

PXENBUS_DEBUG_INTERFACE
FdoGetDebugInterface(
    IN  PXENBUS_FDO     Fdo
    )
{
    return &Fdo->DebugInterface;
}

PXENBUS_SUSPEND_INTERFACE
FdoGetSuspendInterface(
    IN  PXENBUS_FDO     Fdo
    )
{
    return &Fdo->SuspendInterface;
}

PXENBUS_SHARED_INFO_INTERFACE
FdoGetSharedInfoInterface(
    IN  PXENBUS_FDO     Fdo
    )
{
    return &Fdo->SharedInfoInterface;
}

PXENBUS_EVTCHN_INTERFACE
FdoGetEvtchnInterface(
    IN  PXENBUS_FDO     Fdo
    )
{
    return &Fdo->EvtchnInterface;
}

PXENBUS_STORE_INTERFACE
FdoGetStoreInterface(
    IN  PXENBUS_FDO     Fdo
    )
{
    return &Fdo->StoreInterface;
}

PXENBUS_GNTTAB_INTERFACE
FdoGetGnttabInterface(
    IN  PXENBUS_FDO     Fdo
    )
{
    return &Fdo->GnttabInterface;
}

KSERVICE_ROUTINE FdoEvtchnCallback;

BOOLEAN
FdoEvtchnCallback(
    IN  PKINTERRUPT InterruptObject,
    IN  PVOID       Argument
    )
{
    PXENBUS_FDO     Fdo = Argument;

    UNREFERENCED_PARAMETER(InterruptObject);

    ASSERT(Fdo != NULL);

    DebugTrigger(&Fdo->DebugInterface);

    return TRUE;
}

#define BALLOON_PAUSE   60

static FORCEINLINE NTSTATUS
__FdoD3ToD0(
    IN  PXENBUS_FDO Fdo
    )
{
    POWER_STATE     PowerState;
    BOOLEAN         Pending;
    NTSTATUS        status;

    Trace("====>\n");

    ASSERT3U(KeGetCurrentIrql(), ==, DISPATCH_LEVEL);
    ASSERT3U(__FdoGetDevicePowerState(Fdo), ==, PowerDeviceD3);

    EVTCHN(Acquire, &Fdo->EvtchnInterface);

    Fdo->Evtchn = EVTCHN(Open,
                         &Fdo->EvtchnInterface,
                         EVTCHN_VIRQ,
                         FdoEvtchnCallback,
                         Fdo,
                         VIRQ_DEBUG);

    status = STATUS_UNSUCCESSFUL;
    if (Fdo->Evtchn == NULL)
        goto fail1;

    Pending = EVTCHN(Unmask,
                     &Fdo->EvtchnInterface,
                     Fdo->Evtchn,
                     FALSE);
    if (Pending)
        EVTCHN(Trigger,
               &Fdo->EvtchnInterface,
               Fdo->Evtchn);

    __FdoSetDevicePowerState(Fdo, PowerDeviceD0);

    STORE(Acquire, &Fdo->StoreInterface);

    status = STORE(Watch,
                   &Fdo->StoreInterface,
                   NULL,
                   "device",
                   ThreadGetEvent(Fdo->ScanThread),
                   &Fdo->ScanWatch);
    if (!NT_SUCCESS(status))
        goto fail2;

    status = STORE(Watch,
                   &Fdo->StoreInterface,
                   "control",
                   "shutdown",
                   ThreadGetEvent(Fdo->SuspendThread),
                   &Fdo->SuspendWatch);
    if (!NT_SUCCESS(status))
        goto fail3;

    (VOID) STORE(Printf,
                    &Fdo->StoreInterface,
                    NULL,
                    "control",
                    "feature-suspend",
                    "%u",
                    1);

    if (Fdo->Balloon != NULL) {
        status = STORE(Watch,
                       &Fdo->StoreInterface,
                       "memory",
                       "target",
                       ThreadGetEvent(Fdo->BalloonThread),
                       &Fdo->BalloonWatch);
        if (!NT_SUCCESS(status))
            goto fail4;

        (VOID) STORE(Printf,
                        &Fdo->StoreInterface,
                        NULL,
                        "control",
                        "feature-balloon",
                        "%u",
                        1);
    }

    PowerState.DeviceState = PowerDeviceD0;
    PoSetPowerState(Fdo->Dx->DeviceObject,
                    DevicePowerState,
                    PowerState);

    Trace("<====\n");

    return STATUS_SUCCESS;

fail4:
    Error("fail4\n");

    (VOID) STORE(Remove,
                 &Fdo->StoreInterface,
                 NULL,
                 "control",
                 "feature-suspend");

    (VOID) STORE(Unwatch,
                 &Fdo->StoreInterface,
                 Fdo->SuspendWatch);
    Fdo->SuspendWatch = NULL;

fail3:
    Error("fail3\n");

    (VOID) STORE(Unwatch,
                 &Fdo->StoreInterface,
                 Fdo->ScanWatch);
    Fdo->ScanWatch = NULL;

fail2:
    Error("fail2\n");

    STORE(Release, &Fdo->StoreInterface);

    __FdoSetDevicePowerState(Fdo, PowerDeviceD3);

    EVTCHN(Close,
           &Fdo->EvtchnInterface,
           Fdo->Evtchn);
    Fdo->Evtchn = NULL;

fail1:
    Error("fail1 (%08x)\n", status);

    EVTCHN(Release, &Fdo->EvtchnInterface);

    return status;
}

static FORCEINLINE VOID
__FdoD0ToD3(
    IN  PXENBUS_FDO Fdo
    )
{
    POWER_STATE     PowerState;

    Trace("====>\n");

    ASSERT3U(KeGetCurrentIrql(), ==, DISPATCH_LEVEL);
    ASSERT3U(__FdoGetDevicePowerState(Fdo), ==, PowerDeviceD0);

    PowerState.DeviceState = PowerDeviceD3;
    PoSetPowerState(Fdo->Dx->DeviceObject,
                    DevicePowerState,
                    PowerState);

    if (Fdo->Balloon != NULL) {
        (VOID) STORE(Remove,
                     &Fdo->StoreInterface,
                     NULL,
                     "control",
                     "feature-balloon");

        (VOID) STORE(Unwatch,
                     &Fdo->StoreInterface,
                     Fdo->BalloonWatch);
        Fdo->BalloonWatch = NULL;
    }

    (VOID) STORE(Remove,
                 &Fdo->StoreInterface,
                 NULL,
                 "control",
                 "feature-suspend");

    (VOID) STORE(Unwatch,
                 &Fdo->StoreInterface,
                 Fdo->SuspendWatch);
    Fdo->SuspendWatch = NULL;

    (VOID) STORE(Unwatch,
                 &Fdo->StoreInterface,
                 Fdo->ScanWatch);
    Fdo->ScanWatch = NULL;

    STORE(Release, &Fdo->StoreInterface);

    __FdoSetDevicePowerState(Fdo, PowerDeviceD3);

    EVTCHN(Close,
           &Fdo->EvtchnInterface,
           Fdo->Evtchn);
    Fdo->Evtchn = NULL;

    EVTCHN(Release, &Fdo->EvtchnInterface);

    Trace("<====\n");
}

static DECLSPEC_NOINLINE VOID
FdoSuspendCallbackLate(
    IN  PVOID   Argument
    )
{
    PXENBUS_FDO Fdo = Argument;
    NTSTATUS    status;

    __FdoD0ToD3(Fdo);

    status = __FdoD3ToD0(Fdo);
    ASSERT(NT_SUCCESS(status));
}

static DECLSPEC_NOINLINE NTSTATUS
FdoD3ToD0(
    IN  PXENBUS_FDO Fdo
    )
{
    KIRQL           Irql;
    PLIST_ENTRY     ListEntry;
    NTSTATUS        status;

    ASSERT3U(KeGetCurrentIrql(), ==, PASSIVE_LEVEL);

    KeRaiseIrql(DISPATCH_LEVEL, &Irql);

    status = __FdoD3ToD0(Fdo);
    if (!NT_SUCCESS(status))
        goto fail1;

    SUSPEND(Acquire, &Fdo->SuspendInterface);

    status = SUSPEND(Register,
                     &Fdo->SuspendInterface,
                     SUSPEND_CALLBACK_LATE,
                     FdoSuspendCallbackLate,
                     Fdo,
                     &Fdo->SuspendCallbackLate);
    if (!NT_SUCCESS(status))
        goto fail2;

    KeLowerIrql(Irql);

    __FdoAcquireMutex(Fdo);

    for (ListEntry = Fdo->Dx->ListEntry.Flink;
         ListEntry != &Fdo->Dx->ListEntry;
         ListEntry = ListEntry->Flink) {
        PXENBUS_DX  Dx = CONTAINING_RECORD(ListEntry, XENBUS_DX, ListEntry);
        PXENBUS_PDO Pdo = Dx->Pdo;

        ASSERT3U(Dx->Type, ==, PHYSICAL_DEVICE_OBJECT);

        PdoResume(Pdo);
    }

    __FdoReleaseMutex(Fdo);

    return STATUS_SUCCESS;

fail2:
    Error("fail2\n");

    SUSPEND(Release, &Fdo->SuspendInterface);

    __FdoD0ToD3(Fdo);

    KeLowerIrql(Irql);

    if (Fdo->Balloon != NULL) {
        KeClearEvent(&Fdo->BalloonEvent);
        ThreadWake(Fdo->BalloonThread);

        Trace("waiting for balloon thread\n");

        (VOID) KeWaitForSingleObject(&Fdo->BalloonEvent,
                                     Executive,
                                     KernelMode,
                                     FALSE,
                                     NULL);
    }

    KeClearEvent(&Fdo->SuspendEvent);
    ThreadWake(Fdo->SuspendThread);

    Trace("waiting for suspend thread\n");

    (VOID) KeWaitForSingleObject(&Fdo->SuspendEvent,
                                 Executive,
                                 KernelMode,
                                 FALSE,
                                 NULL);

    KeRaiseIrql(DISPATCH_LEVEL, &Irql);

fail1:
    Error("fail1 (%08x)\n", status);

    KeLowerIrql(Irql);

    return status;
}

static DECLSPEC_NOINLINE VOID
FdoD0ToD3(
    IN  PXENBUS_FDO Fdo
    )
{
    PLIST_ENTRY     ListEntry;
    KIRQL           Irql;

    ASSERT3U(KeGetCurrentIrql(), ==, PASSIVE_LEVEL);

    __FdoAcquireMutex(Fdo);

    for (ListEntry = Fdo->Dx->ListEntry.Flink;
         ListEntry != &Fdo->Dx->ListEntry;
         ListEntry = ListEntry->Flink) {
        PXENBUS_DX  Dx = CONTAINING_RECORD(ListEntry, XENBUS_DX, ListEntry);
        PXENBUS_PDO Pdo = Dx->Pdo;

        ASSERT3U(Dx->Type, ==, PHYSICAL_DEVICE_OBJECT);

        if (PdoGetDevicePnpState(Pdo) == Deleted ||
            PdoIsMissing(Pdo))
            continue;

        PdoSuspend(Pdo);
    }

    __FdoReleaseMutex(Fdo);

    KeRaiseIrql(DISPATCH_LEVEL, &Irql);

    SUSPEND(Deregister,
            &Fdo->SuspendInterface,
            Fdo->SuspendCallbackLate);
    Fdo->SuspendCallbackLate = NULL;

    SUSPEND(Release, &Fdo->SuspendInterface);

    __FdoD0ToD3(Fdo);

    KeLowerIrql(Irql);

    if (Fdo->Balloon != NULL) {
        KeClearEvent(&Fdo->BalloonEvent);
        ThreadWake(Fdo->BalloonThread);

        Trace("waiting for balloon thread\n");

        (VOID) KeWaitForSingleObject(&Fdo->BalloonEvent,
                                     Executive,
                                     KernelMode,
                                     FALSE,
                                     NULL);
    }

    KeClearEvent(&Fdo->SuspendEvent);
    ThreadWake(Fdo->SuspendThread);

    Trace("waiting for suspend thread\n");

    (VOID) KeWaitForSingleObject(&Fdo->SuspendEvent,
                                 Executive,
                                 KernelMode,
                                 FALSE,
                                 NULL);
}

static DECLSPEC_NOINLINE NTSTATUS
FdoS4ToS3(
    IN  PXENBUS_FDO         Fdo
    )
{
    KIRQL                   Irql;
    NTSTATUS                status;

    Trace("====>\n");

    ASSERT3U(KeGetCurrentIrql(), ==, PASSIVE_LEVEL);
    ASSERT3U(__FdoGetSystemPowerState(Fdo), ==, PowerSystemHibernate);

    KeRaiseIrql(DISPATCH_LEVEL, &Irql); // Flush out any attempt to use pageable memory

    status = DebugInitialize(Fdo, &Fdo->DebugInterface);
    if (!NT_SUCCESS(status))
        goto fail1;

    status = SuspendInitialize(Fdo, &Fdo->SuspendInterface);
    if (!NT_SUCCESS(status))
        goto fail2;

    status = SharedInfoInitialize(Fdo, &Fdo->SharedInfoInterface);
    if (!NT_SUCCESS(status))
        goto fail3;

    status = EvtchnInitialize(Fdo, &Fdo->EvtchnInterface);
    if (!NT_SUCCESS(status))
        goto fail4;

    status = StoreInitialize(Fdo, &Fdo->StoreInterface);
    if (!NT_SUCCESS(status))
        goto fail5;

    status = GnttabInitialize(Fdo, &Fdo->GnttabInterface);
    if (!NT_SUCCESS(status))
        goto fail6;

    __FdoSetSystemPowerState(Fdo, PowerSystemSleeping3);

    __FdoEnableInterrupt(Fdo);

    KeLowerIrql(Irql);

    Trace("<====\n");

    return STATUS_SUCCESS;

fail6:
    Error("fail6\n");

    StoreTeardown(&Fdo->StoreInterface);

fail5:
    Error("fail5\n");

    EvtchnTeardown(&Fdo->EvtchnInterface);

fail4:
    Error("fail4\n");

    SharedInfoTeardown(&Fdo->SharedInfoInterface);

fail3:
    Error("fail3\n");

    SuspendTeardown(&Fdo->SuspendInterface);

fail2:
    Error("fail2\n");

    DebugTeardown(&Fdo->DebugInterface);

fail1:
    Error("fail1 (%08x)\n", status);

    KeLowerIrql(Irql);

    return status;
}

static DECLSPEC_NOINLINE VOID
FdoS3ToS4(
    IN  PXENBUS_FDO Fdo
    )
{
    Trace("====>\n");

    ASSERT3U(KeGetCurrentIrql(), ==, PASSIVE_LEVEL);
    ASSERT3U(__FdoGetSystemPowerState(Fdo), ==, PowerSystemSleeping3);

    __FdoDisableInterrupt(Fdo);

    __FdoSetSystemPowerState(Fdo, PowerSystemHibernate);

    GnttabTeardown(&Fdo->GnttabInterface);

    StoreTeardown(&Fdo->StoreInterface);

    EvtchnTeardown(&Fdo->EvtchnInterface);

    SharedInfoTeardown(&Fdo->SharedInfoInterface);

    SuspendTeardown(&Fdo->SuspendInterface);

    DebugTeardown(&Fdo->DebugInterface);

    Trace("<====\n");
}

static DECLSPEC_NOINLINE NTSTATUS
FdoStartDevice(
    IN  PXENBUS_FDO     Fdo,
    IN  PIRP            Irp
    )
{
    PIO_STACK_LOCATION  StackLocation;
    NTSTATUS            status;

    ASSERT3U(KeGetCurrentIrql(), ==, PASSIVE_LEVEL);

    status = FdoForwardIrpSynchronously(Fdo, Irp);
    if (!NT_SUCCESS(status))
        goto fail1;

    StackLocation = IoGetCurrentIrpStackLocation(Irp);

    FdoParseResources(Fdo,
                      StackLocation->Parameters.StartDevice.AllocatedResources,
                      StackLocation->Parameters.StartDevice.AllocatedResourcesTranslated);

    status = FdoConnectInterrupt(Fdo);
    if (!NT_SUCCESS(status))
        goto fail2;

    KeInitializeEvent(&Fdo->ScanEvent, NotificationEvent, FALSE);

    status = ThreadCreate(FdoScan, Fdo, &Fdo->ScanThread);
    if (!NT_SUCCESS(status))
        goto fail3;

    KeInitializeEvent(&Fdo->SuspendEvent, NotificationEvent, FALSE);

    status = ThreadCreate(FdoSuspend, Fdo, &Fdo->SuspendThread);
    if (!NT_SUCCESS(status))
        goto fail4;

    if (Fdo->Balloon != NULL) {
        KeInitializeEvent(&Fdo->BalloonEvent, NotificationEvent, FALSE);

        status = ThreadCreate(FdoBalloon, Fdo, &Fdo->BalloonThread);
        if (!NT_SUCCESS(status))
            goto fail5;
    }

    __FdoSetSystemPowerState(Fdo, PowerSystemHibernate);

    status = FdoS4ToS3(Fdo);
    if (!NT_SUCCESS(status))
        goto fail6;

    __FdoSetSystemPowerState(Fdo, PowerSystemWorking);

    status = FdoD3ToD0(Fdo);
    if (!NT_SUCCESS(status))
        goto fail7;

    if (Fdo->Balloon != NULL) {
        BOOLEAN Warned;

        Warned = FALSE;

        for (;;) {
            LARGE_INTEGER   Timeout;

            Timeout.QuadPart = TIME_RELATIVE(TIME_S(BALLOON_PAUSE));

            status = KeWaitForSingleObject(&Fdo->BalloonEvent,
                                           Executive,
                                           KernelMode,
                                           FALSE,
                                           &Timeout);
            if (status != STATUS_TIMEOUT)
                break;

            if (!Warned) {
                Warning("Waiting for balloon\n");
                Warned = TRUE;
            }
        }
    }

    __FdoSetDevicePnpState(Fdo, Started);
    ThreadWake(Fdo->ScanThread);

    status = Irp->IoStatus.Status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;

fail7:
    Error("fail7\n");

    __FdoSetSystemPowerState(Fdo, PowerSystemSleeping3);
    FdoS3ToS4(Fdo);

fail6:
    Error("fail6\n");

    __FdoSetSystemPowerState(Fdo, PowerSystemShutdown);

    if (Fdo->Balloon != NULL) {
        ThreadAlert(Fdo->BalloonThread);
        ThreadJoin(Fdo->BalloonThread);
        Fdo->BalloonThread = NULL;
    }

fail5:
    Error("fail5\n");

    if (Fdo->Balloon != NULL)
        RtlZeroMemory(&Fdo->BalloonEvent, sizeof (KEVENT));

    ThreadAlert(Fdo->SuspendThread);
    ThreadJoin(Fdo->SuspendThread);
    Fdo->SuspendThread = NULL;

fail4:
    Error("fail4\n");

    RtlZeroMemory(&Fdo->SuspendEvent, sizeof (KEVENT));

    ThreadAlert(Fdo->ScanThread);
    ThreadJoin(Fdo->ScanThread);
    Fdo->ScanThread = NULL;

fail3:
    Error("fail3\n");

    RtlZeroMemory(&Fdo->ScanEvent, sizeof (KEVENT));

    FdoDisconnectInterrupt(Fdo);

fail2:
    Error("fail2\n");

    RtlZeroMemory(&Fdo->Resource, sizeof (XENBUS_RESOURCE) * RESOURCE_COUNT);

fail1:
    Error("fail1 (%08x)\n", status);

    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

static DECLSPEC_NOINLINE NTSTATUS
FdoQueryStopDevice(
    IN  PXENBUS_FDO Fdo,
    IN  PIRP        Irp
    )
{
    NTSTATUS        status;

    status = STATUS_UNSUCCESSFUL;
    if (Fdo->Balloon != NULL && BalloonGetSize(Fdo->Balloon) != 0)
        goto fail1;

    __FdoSetDevicePnpState(Fdo, StopPending);
    Irp->IoStatus.Status = STATUS_SUCCESS;

    IoSkipCurrentIrpStackLocation(Irp);
    status = IoCallDriver(Fdo->LowerDeviceObject, Irp);

    return status;

fail1:
    Error("fail1 (%08x)\n", status);

    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

static DECLSPEC_NOINLINE NTSTATUS
FdoCancelStopDevice(
    IN  PXENBUS_FDO Fdo,
    IN  PIRP        Irp
    )
{
    NTSTATUS        status;

    Irp->IoStatus.Status = STATUS_SUCCESS;

    __FdoRestoreDevicePnpState(Fdo, StopPending);

    IoSkipCurrentIrpStackLocation(Irp);
    status = IoCallDriver(Fdo->LowerDeviceObject, Irp);

    return status;
}

static DECLSPEC_NOINLINE NTSTATUS
FdoStopDevice(
    IN  PXENBUS_FDO Fdo,
    IN  PIRP        Irp
    )
{
    NTSTATUS        status;

    FdoD0ToD3(Fdo);

    __FdoSetSystemPowerState(Fdo, PowerSystemSleeping3);
    FdoS3ToS4(Fdo);
    __FdoSetSystemPowerState(Fdo, PowerSystemShutdown);

    if (Fdo->Balloon != NULL) {
        ThreadAlert(Fdo->BalloonThread);
        ThreadJoin(Fdo->BalloonThread);
        Fdo->BalloonThread = NULL;

        RtlZeroMemory(&Fdo->BalloonEvent, sizeof (KEVENT));
    }

    ThreadAlert(Fdo->SuspendThread);
    ThreadJoin(Fdo->SuspendThread);
    Fdo->SuspendThread = NULL;

    RtlZeroMemory(&Fdo->SuspendEvent, sizeof (KEVENT));

    ThreadAlert(Fdo->ScanThread);
    ThreadJoin(Fdo->ScanThread);
    Fdo->ScanThread = NULL;

    RtlZeroMemory(&Fdo->ScanEvent, sizeof (KEVENT));

    FdoDisconnectInterrupt(Fdo);

    RtlZeroMemory(&Fdo->Resource, sizeof (XENBUS_RESOURCE) * RESOURCE_COUNT);

    __FdoSetDevicePnpState(Fdo, Stopped);
    Irp->IoStatus.Status = STATUS_SUCCESS;

    IoSkipCurrentIrpStackLocation(Irp);
    status = IoCallDriver(Fdo->LowerDeviceObject, Irp);

    return status;
}

static DECLSPEC_NOINLINE NTSTATUS
FdoQueryRemoveDevice(
    IN  PXENBUS_FDO Fdo,
    IN  PIRP        Irp
    )
{
    NTSTATUS        status;

    status = STATUS_UNSUCCESSFUL;
    if (Fdo->Balloon != NULL && BalloonGetSize(Fdo->Balloon) != 0)
        goto fail1;

    __FdoSetDevicePnpState(Fdo, RemovePending);
    Irp->IoStatus.Status = STATUS_SUCCESS;

    IoSkipCurrentIrpStackLocation(Irp);
    status = IoCallDriver(Fdo->LowerDeviceObject, Irp);

    return status;

fail1:
    Error("fail1 (%08x)\n", status);

    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

static DECLSPEC_NOINLINE NTSTATUS
FdoCancelRemoveDevice(
    IN  PXENBUS_FDO Fdo,
    IN  PIRP        Irp
    )
{
    NTSTATUS        status;

    __FdoRestoreDevicePnpState(Fdo, RemovePending);

    Irp->IoStatus.Status = STATUS_SUCCESS;

    IoSkipCurrentIrpStackLocation(Irp);
    status = IoCallDriver(Fdo->LowerDeviceObject, Irp);

    return status;
}

static DECLSPEC_NOINLINE NTSTATUS
FdoSurpriseRemoval(
    IN  PXENBUS_FDO Fdo,
    IN  PIRP        Irp
    )
{
    PLIST_ENTRY     ListEntry;
    NTSTATUS        status;

    __FdoSetDevicePnpState(Fdo, SurpriseRemovePending);

    __FdoAcquireMutex(Fdo);

    for (ListEntry = Fdo->Dx->ListEntry.Flink;
         ListEntry != &Fdo->Dx->ListEntry;
         ListEntry = ListEntry->Flink) {
        PXENBUS_DX  Dx = CONTAINING_RECORD(ListEntry, XENBUS_DX, ListEntry);
        PXENBUS_PDO Pdo = Dx->Pdo;

        ASSERT3U(Dx->Type, ==, PHYSICAL_DEVICE_OBJECT);

        if (!PdoIsMissing(Pdo))
            PdoSetMissing(Pdo, "FDO surprise removed");
    }

    __FdoReleaseMutex(Fdo);

    Irp->IoStatus.Status = STATUS_SUCCESS;

    IoSkipCurrentIrpStackLocation(Irp);
    status = IoCallDriver(Fdo->LowerDeviceObject, Irp);

    return status;
}

static DECLSPEC_NOINLINE NTSTATUS
FdoRemoveDevice(
    IN  PXENBUS_FDO                     Fdo,
    IN  PIRP                            Irp
    )
{
    PLIST_ENTRY                         ListEntry;
    NTSTATUS                            status;

    ASSERT3U(KeGetCurrentIrql(), ==, PASSIVE_LEVEL);

    if (__FdoGetDevicePowerState(Fdo) != PowerDeviceD0)
        goto done;

    KeClearEvent(&Fdo->ScanEvent);
    ThreadWake(Fdo->ScanThread);

    Trace("waiting for scan thread\n");

    (VOID) KeWaitForSingleObject(&Fdo->ScanEvent,
                                 Executive,
                                 KernelMode,
                                 FALSE,
                                 NULL);

    __FdoAcquireMutex(Fdo);

    ListEntry = Fdo->Dx->ListEntry.Flink;
    while (ListEntry != &Fdo->Dx->ListEntry) {
        PLIST_ENTRY Flink = ListEntry->Flink;
        PXENBUS_DX  Dx = CONTAINING_RECORD(ListEntry, XENBUS_DX, ListEntry);
        PXENBUS_PDO Pdo = Dx->Pdo;

        ASSERT3U(Dx->Type, ==, PHYSICAL_DEVICE_OBJECT);

        if (!PdoIsMissing(Pdo))
            PdoSetMissing(Pdo, "FDO removed");

        if (PdoGetDevicePnpState(Pdo) != SurpriseRemovePending)
            PdoSetDevicePnpState(Pdo, Deleted);

        if (PdoGetDevicePnpState(Pdo) == Deleted)
            PdoDestroy(Pdo);

        ListEntry = Flink;
    }

    __FdoReleaseMutex(Fdo);

    FdoD0ToD3(Fdo);

    __FdoSetSystemPowerState(Fdo, PowerSystemSleeping3);
    FdoS3ToS4(Fdo);
    __FdoSetSystemPowerState(Fdo, PowerSystemShutdown);

    if (Fdo->Balloon != NULL) {
        ThreadAlert(Fdo->BalloonThread);
        ThreadJoin(Fdo->BalloonThread);
        Fdo->BalloonThread = NULL;

        RtlZeroMemory(&Fdo->BalloonEvent, sizeof (KEVENT));
    }

    ThreadAlert(Fdo->SuspendThread);
    ThreadJoin(Fdo->SuspendThread);
    Fdo->SuspendThread = NULL;

    RtlZeroMemory(&Fdo->SuspendEvent, sizeof (KEVENT));

    ThreadAlert(Fdo->ScanThread);
    ThreadJoin(Fdo->ScanThread);
    Fdo->ScanThread = NULL;

    RtlZeroMemory(&Fdo->ScanEvent, sizeof (KEVENT));

    FdoDisconnectInterrupt(Fdo);

    RtlZeroMemory(&Fdo->Resource, sizeof (XENBUS_RESOURCE) * RESOURCE_COUNT);

done:
    __FdoSetDevicePnpState(Fdo, Deleted);

    Irp->IoStatus.Status = STATUS_SUCCESS;

    IoSkipCurrentIrpStackLocation(Irp);
    status = IoCallDriver(Fdo->LowerDeviceObject, Irp);

    __FdoAcquireMutex(Fdo);
    ASSERT3U(Fdo->References, !=, 0);
    --Fdo->References;
    __FdoReleaseMutex(Fdo);

    if (Fdo->References == 0)
        FdoDestroy(Fdo);

    return status;
}

#define SCAN_PAUSE  10

static DECLSPEC_NOINLINE NTSTATUS
FdoQueryDeviceRelations(
    IN  PXENBUS_FDO     Fdo,
    IN  PIRP            Irp
    )
{
    PIO_STACK_LOCATION  StackLocation;
    ULONG               Size;
    PDEVICE_RELATIONS   Relations;
    ULONG               Count;
    PLIST_ENTRY         ListEntry;
    BOOLEAN             Warned;
    NTSTATUS            status;

    ASSERT3U(KeGetCurrentIrql(), ==, PASSIVE_LEVEL);

    StackLocation = IoGetCurrentIrpStackLocation(Irp);

    status = Irp->IoStatus.Status;

    if (StackLocation->Parameters.QueryDeviceRelations.Type != BusRelations) {
        IoSkipCurrentIrpStackLocation(Irp);
        status = IoCallDriver(Fdo->LowerDeviceObject, Irp);

        goto done;
    }

    Warned = FALSE;

    for (;;) {
        LARGE_INTEGER   Timeout;

        Timeout.QuadPart = TIME_RELATIVE(TIME_S(SCAN_PAUSE));

        status = KeWaitForSingleObject(&Fdo->ScanEvent,
                                       Executive,
                                       KernelMode,
                                       FALSE,
                                       &Timeout);
        if (status != STATUS_TIMEOUT)
            break;

        if (!Warned) {
            Warning("Waiting for device enumeration\n");
            Warned = TRUE;
        }
    }

    __FdoAcquireMutex(Fdo);

    Count = 0;
    for (ListEntry = Fdo->Dx->ListEntry.Flink;
         ListEntry != &Fdo->Dx->ListEntry;
         ListEntry = ListEntry->Flink)
        Count++;

    Size = FIELD_OFFSET(DEVICE_RELATIONS, Objects) + (sizeof (DEVICE_OBJECT) * __min(Count, 1));

    Relations = ExAllocatePoolWithTag(PagedPool, Size, 'SUB');

    status = STATUS_NO_MEMORY;
    if (Relations == NULL)
        goto fail1;

    RtlZeroMemory(Relations, Size);

    for (ListEntry = Fdo->Dx->ListEntry.Flink;
         ListEntry != &Fdo->Dx->ListEntry;
         ListEntry = ListEntry->Flink) {
        PXENBUS_DX  Dx = CONTAINING_RECORD(ListEntry, XENBUS_DX, ListEntry);
        PXENBUS_PDO Pdo = Dx->Pdo;

        ASSERT3U(Dx->Type, ==, PHYSICAL_DEVICE_OBJECT);

        if (PdoGetDevicePnpState(Pdo) == Deleted &&
            !PdoIsMissing(Pdo))
            PdoSetMissing(Pdo, "surprise remove");

        if (PdoIsMissing(Pdo))
            continue;

        if (PdoGetDevicePnpState(Pdo) == Present)
            PdoSetDevicePnpState(Pdo, Enumerated);

        ObReferenceObject(Dx->DeviceObject);
        Relations->Objects[Relations->Count++] = Dx->DeviceObject;
    }

    ASSERT3U(Relations->Count, <=, Count);

    Trace("%d PDO(s)\n", Relations->Count);

    __FdoReleaseMutex(Fdo);

    Irp->IoStatus.Information = (ULONG_PTR)Relations;
    Irp->IoStatus.Status = STATUS_SUCCESS;

    status = FdoForwardIrpSynchronously(Fdo, Irp);
    if (!NT_SUCCESS(status))
        goto fail2;

    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    __FdoAcquireMutex(Fdo);

    for (ListEntry = Fdo->Dx->ListEntry.Flink;
         ListEntry != &Fdo->Dx->ListEntry;
         ListEntry = ListEntry->Flink) {
        PXENBUS_DX  Dx = CONTAINING_RECORD(ListEntry, XENBUS_DX, ListEntry);
        PXENBUS_PDO Pdo = Dx->Pdo;

        ASSERT3U(Dx->Type, ==, PHYSICAL_DEVICE_OBJECT);

        if (PdoGetDevicePnpState(Pdo) == Deleted &&
            PdoIsMissing(Pdo))
            PdoDestroy(Pdo);
    }

    __FdoReleaseMutex(Fdo);

done:
    return status;

fail2:
    Error("fail2\n");

    __FdoAcquireMutex(Fdo);

fail1:
    Error("fail1 (%08x)\n", status);

    __FdoReleaseMutex(Fdo);

    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

static DECLSPEC_NOINLINE NTSTATUS
FdoQueryCapabilities(
    IN  PXENBUS_FDO         Fdo,
    IN  PIRP                Irp
    )
{
    PIO_STACK_LOCATION      StackLocation;
    PDEVICE_CAPABILITIES    Capabilities;
    SYSTEM_POWER_STATE      SystemPowerState;
    NTSTATUS                status;

    status = FdoForwardIrpSynchronously(Fdo, Irp);
    if (!NT_SUCCESS(status))
        goto fail1;

    StackLocation = IoGetCurrentIrpStackLocation(Irp);
    Capabilities = StackLocation->Parameters.DeviceCapabilities.Capabilities;

    Fdo->LowerDeviceCapabilities = *Capabilities;

    for (SystemPowerState = 0; SystemPowerState < PowerSystemMaximum; SystemPowerState++) {
        DEVICE_POWER_STATE  DevicePowerState;

        DevicePowerState = Fdo->LowerDeviceCapabilities.DeviceState[SystemPowerState];
        Trace("%s -> %s\n",
              PowerSystemStateName(SystemPowerState),
              PowerDeviceStateName(DevicePowerState));
    }

    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;

fail1:
    Error("fail1 (%08x)\n", status);

    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

static DECLSPEC_NOINLINE NTSTATUS
FdoDeviceUsageNotification(
    IN  PXENBUS_FDO                 Fdo,
    IN  PIRP                        Irp
    )
{
    PIO_STACK_LOCATION              StackLocation;
    DEVICE_USAGE_NOTIFICATION_TYPE  Type;
    BOOLEAN                         InPath;
    BOOLEAN                         NotDisableable;
    NTSTATUS                        status;

    StackLocation = IoGetCurrentIrpStackLocation(Irp);
    Type = StackLocation->Parameters.UsageNotification.Type;
    InPath = StackLocation->Parameters.UsageNotification.InPath;

    if (InPath) {
        Info("%s: ADDING %s\n",
             __FdoGetName(Fdo),
             DeviceUsageTypeName(Type));
        Fdo->Usage[Type]++;
    } else {
        if (Fdo->Usage[Type] != 0) {
            Info("%s: REMOVING %s\n",
                 __FdoGetName(Fdo),
                 DeviceUsageTypeName(Type));
            --Fdo->Usage[Type];
        }
    }

    status = FdoForwardIrpSynchronously(Fdo, Irp);
    if (!NT_SUCCESS(status))
        goto fail1;

    NotDisableable = FALSE;    
    for (Type = 0; Type <= DeviceUsageTypeDumpFile; Type++) {
        if (Fdo->Usage[Type] != 0) {
            NotDisableable = TRUE;
            break;
        }
    }

    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    if (Fdo->NotDisableable != NotDisableable) {
        Fdo->NotDisableable = NotDisableable;
    
        IoInvalidateDeviceState(__FdoGetPhysicalDeviceObject(Fdo));
    }

    return status;

fail1:
    Error("fail1 (%08x)\n", status);

    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

static DECLSPEC_NOINLINE NTSTATUS
FdoQueryPnpDeviceState(
    IN  PXENBUS_FDO                 Fdo,
    IN  PIRP                        Irp
    )
{
    ULONG_PTR                       State;
    NTSTATUS                        status;

    if (Irp->IoStatus.Status == STATUS_SUCCESS)
        State = Irp->IoStatus.Information;
    else if (Irp->IoStatus.Status == STATUS_NOT_SUPPORTED)
        State = 0;
    else
        goto done;

    if (Fdo->NotDisableable) {
        Info("%s: not disableable\n", __FdoGetName(Fdo));
        State |= PNP_DEVICE_NOT_DISABLEABLE;
    }

    Irp->IoStatus.Information = State;
    Irp->IoStatus.Status = STATUS_SUCCESS;

done:
    IoSkipCurrentIrpStackLocation(Irp);
    status = IoCallDriver(Fdo->LowerDeviceObject, Irp);

    return status;
}

static DECLSPEC_NOINLINE NTSTATUS
FdoDispatchPnp(
    IN  PXENBUS_FDO     Fdo,
    IN  PIRP            Irp
    )
{
    PIO_STACK_LOCATION  StackLocation;
    UCHAR               MinorFunction;
    NTSTATUS            status;

    StackLocation = IoGetCurrentIrpStackLocation(Irp);
    MinorFunction = StackLocation->MinorFunction;

    Trace("====> (%02x:%s)\n",
          MinorFunction, 
          PnpMinorFunctionName(MinorFunction)); 

    switch (StackLocation->MinorFunction) {
    case IRP_MN_START_DEVICE:
        status = FdoStartDevice(Fdo, Irp);
        break;

    case IRP_MN_QUERY_STOP_DEVICE:
        status = FdoQueryStopDevice(Fdo, Irp);
        break;

    case IRP_MN_CANCEL_STOP_DEVICE:
        status = FdoCancelStopDevice(Fdo, Irp);
        break;

    case IRP_MN_STOP_DEVICE:
        status = FdoStopDevice(Fdo, Irp);
        break;

    case IRP_MN_QUERY_REMOVE_DEVICE:
        status = FdoQueryRemoveDevice(Fdo, Irp);
        break;

    case IRP_MN_SURPRISE_REMOVAL:
        status = FdoSurpriseRemoval(Fdo, Irp);
        break;

    case IRP_MN_REMOVE_DEVICE:
        status = FdoRemoveDevice(Fdo, Irp);
        break;

    case IRP_MN_CANCEL_REMOVE_DEVICE:
        status = FdoCancelRemoveDevice(Fdo, Irp);
        break;

    case IRP_MN_QUERY_DEVICE_RELATIONS:
        status = FdoQueryDeviceRelations(Fdo, Irp);
        break;

    case IRP_MN_QUERY_CAPABILITIES:
        status = FdoQueryCapabilities(Fdo, Irp);
        break;

    case IRP_MN_DEVICE_USAGE_NOTIFICATION:
        status = FdoDeviceUsageNotification(Fdo, Irp);
        break;

    case IRP_MN_QUERY_PNP_DEVICE_STATE:
        status = FdoQueryPnpDeviceState(Fdo, Irp);
        break;

    default:
        IoSkipCurrentIrpStackLocation(Irp);
        status = IoCallDriver(Fdo->LowerDeviceObject, Irp);
        break;
    }

    Trace("<==== (%02x:%s)(%08x)\n",
          MinorFunction, 
          PnpMinorFunctionName(MinorFunction),
          status); 

    return status;
}

static FORCEINLINE NTSTATUS
__FdoSetDevicePowerUp(
    IN  PXENBUS_FDO     Fdo,
    IN  PIRP            Irp
    )
{
    PIO_STACK_LOCATION  StackLocation;
    DEVICE_POWER_STATE  DeviceState;
    NTSTATUS            status;

    StackLocation = IoGetCurrentIrpStackLocation(Irp);
    DeviceState = StackLocation->Parameters.Power.State.DeviceState;

    ASSERT3U(DeviceState, <,  __FdoGetDevicePowerState(Fdo));

    status = FdoForwardIrpSynchronously(Fdo, Irp);
    if (!NT_SUCCESS(status))
        goto done;

    Info("%s: %s -> %s\n",
         __FdoGetName(Fdo),
         PowerDeviceStateName(__FdoGetDevicePowerState(Fdo)),
         PowerDeviceStateName(DeviceState));

    ASSERT3U(DeviceState, ==, PowerDeviceD0);
    status = FdoD3ToD0(Fdo);
    ASSERT(NT_SUCCESS(status));

done:
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

static FORCEINLINE NTSTATUS
__FdoSetDevicePowerDown(
    IN  PXENBUS_FDO     Fdo,
    IN  PIRP            Irp
    )
{
    PIO_STACK_LOCATION  StackLocation;
    DEVICE_POWER_STATE  DeviceState;
    NTSTATUS            status;

    StackLocation = IoGetCurrentIrpStackLocation(Irp);
    DeviceState = StackLocation->Parameters.Power.State.DeviceState;

    ASSERT3U(DeviceState, >,  __FdoGetDevicePowerState(Fdo));

    Info("%s: %s -> %s\n",
         __FdoGetName(Fdo),
         PowerDeviceStateName(__FdoGetDevicePowerState(Fdo)),
         PowerDeviceStateName(DeviceState));

    ASSERT3U(DeviceState, ==, PowerDeviceD3);

    if (__FdoGetDevicePowerState(Fdo) == PowerDeviceD0)
        FdoD0ToD3(Fdo);

    status = FdoForwardIrpSynchronously(Fdo, Irp);
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

static FORCEINLINE NTSTATUS
__FdoSetDevicePower(
    IN  PXENBUS_FDO     Fdo,
    IN  PIRP            Irp
    )
{
    PIO_STACK_LOCATION  StackLocation;
    DEVICE_POWER_STATE  DeviceState;
    POWER_ACTION        PowerAction;
    NTSTATUS            status;

    StackLocation = IoGetCurrentIrpStackLocation(Irp);
    DeviceState = StackLocation->Parameters.Power.State.DeviceState;
    PowerAction = StackLocation->Parameters.Power.ShutdownType;

    Trace("====> (%s:%s)\n",
          PowerDeviceStateName(DeviceState), 
          PowerActionName(PowerAction));

    ASSERT3U(PowerAction, <,  PowerActionShutdown);

    if (DeviceState == __FdoGetDevicePowerState(Fdo)) {
        status = FdoForwardIrpSynchronously(Fdo, Irp);
        IoCompleteRequest(Irp, IO_NO_INCREMENT);

        goto done;
    }

    status = (DeviceState < __FdoGetDevicePowerState(Fdo)) ?
             __FdoSetDevicePowerUp(Fdo, Irp) :
             __FdoSetDevicePowerDown(Fdo, Irp);

done:
    Trace("<==== (%s:%s)(%08x)\n",
          PowerDeviceStateName(DeviceState), 
          PowerActionName(PowerAction),
          status);
    return status;
}

__drv_functionClass(REQUEST_POWER_COMPLETE)
__drv_sameIRQL
VOID
__FdoRequestSetDevicePower(
    IN  PDEVICE_OBJECT      DeviceObject,
    IN  UCHAR               MinorFunction,
    IN  POWER_STATE         PowerState,
    IN  PVOID               Context,
    IN  PIO_STATUS_BLOCK    IoStatus
    )
{
    PKEVENT                 Event = Context;

    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(MinorFunction);
    UNREFERENCED_PARAMETER(PowerState);

    ASSERT(NT_SUCCESS(IoStatus->Status));

    KeSetEvent(Event, IO_NO_INCREMENT, FALSE);
}

static VOID
FdoRequestSetDevicePower(
    IN  PXENBUS_FDO         Fdo,
    IN  DEVICE_POWER_STATE  DeviceState
    )
{
    POWER_STATE             PowerState;
    KEVENT                  Event;
    NTSTATUS                status;

    Trace("%s\n", PowerDeviceStateName(DeviceState));

    ASSERT3U(KeGetCurrentIrql(), ==, PASSIVE_LEVEL);

    PowerState.DeviceState = DeviceState;
    KeInitializeEvent(&Event, NotificationEvent, FALSE);

    status = PoRequestPowerIrp(Fdo->LowerDeviceObject,
                               IRP_MN_SET_POWER,
                               PowerState,
                               __FdoRequestSetDevicePower,
                               &Event,
                               NULL);
    ASSERT(NT_SUCCESS(status));

    (VOID) KeWaitForSingleObject(&Event,
                                 Executive,
                                 KernelMode,
                                 FALSE,
                                 NULL);
}

static FORCEINLINE NTSTATUS
__FdoSetSystemPowerUp(
    IN  PXENBUS_FDO     Fdo,
    IN  PIRP            Irp
    )
{

    PIO_STACK_LOCATION  StackLocation;
    SYSTEM_POWER_STATE  SystemState;
    DEVICE_POWER_STATE  DeviceState;
    NTSTATUS            status;

    StackLocation = IoGetCurrentIrpStackLocation(Irp);
    SystemState = StackLocation->Parameters.Power.State.SystemState;

    ASSERT3U(SystemState, <,  __FdoGetSystemPowerState(Fdo));

    status = FdoForwardIrpSynchronously(Fdo, Irp);
    if (!NT_SUCCESS(status))
        goto done;

    Info("%s: %s -> %s\n",
         __FdoGetName(Fdo),
         PowerSystemStateName(__FdoGetSystemPowerState(Fdo)),
         PowerSystemStateName(SystemState));

    if (SystemState < PowerSystemHibernate &&
        __FdoGetSystemPowerState(Fdo) >= PowerSystemHibernate) {
        __FdoSetSystemPowerState(Fdo, PowerSystemHibernate);
        (VOID) FdoS4ToS3(Fdo);
    }

    __FdoSetSystemPowerState(Fdo, SystemState);

    DeviceState = Fdo->LowerDeviceCapabilities.DeviceState[SystemState];
    FdoRequestSetDevicePower(Fdo, DeviceState);

done:
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

static FORCEINLINE NTSTATUS
__FdoSetSystemPowerDown(
    IN  PXENBUS_FDO     Fdo,
    IN  PIRP            Irp
    )
{
    PIO_STACK_LOCATION  StackLocation;
    SYSTEM_POWER_STATE  SystemState;
    DEVICE_POWER_STATE  DeviceState;
    NTSTATUS            status;

    StackLocation = IoGetCurrentIrpStackLocation(Irp);
    SystemState = StackLocation->Parameters.Power.State.SystemState;

    ASSERT3U(SystemState, >,  __FdoGetSystemPowerState(Fdo));

    DeviceState = Fdo->LowerDeviceCapabilities.DeviceState[SystemState];

    FdoRequestSetDevicePower(Fdo, DeviceState);

    Info("%s: %s -> %s\n",
         __FdoGetName(Fdo),
         PowerSystemStateName(__FdoGetSystemPowerState(Fdo)),
         PowerSystemStateName(SystemState));

    if (SystemState >= PowerSystemHibernate &&
        __FdoGetSystemPowerState(Fdo) < PowerSystemHibernate) {
        __FdoSetSystemPowerState(Fdo, PowerSystemSleeping3);
        FdoS3ToS4(Fdo);
    }

    __FdoSetSystemPowerState(Fdo, SystemState);

    status = FdoForwardIrpSynchronously(Fdo, Irp);
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

static FORCEINLINE NTSTATUS
__FdoSetSystemPower(
    IN  PXENBUS_FDO     Fdo,
    IN  PIRP            Irp
    )
{
    PIO_STACK_LOCATION  StackLocation;
    SYSTEM_POWER_STATE  SystemState;
    POWER_ACTION        PowerAction;
    NTSTATUS            status;

    StackLocation = IoGetCurrentIrpStackLocation(Irp);
    SystemState = StackLocation->Parameters.Power.State.SystemState;
    PowerAction = StackLocation->Parameters.Power.ShutdownType;

    Trace("====> (%s:%s)\n",
          PowerSystemStateName(SystemState), 
          PowerActionName(PowerAction));

    ASSERT3U(PowerAction, <,  PowerActionShutdown);

    if (SystemState == __FdoGetSystemPowerState(Fdo)) {
        status = FdoForwardIrpSynchronously(Fdo, Irp);
        IoCompleteRequest(Irp, IO_NO_INCREMENT);

        goto done;
    }

    status = (SystemState < __FdoGetSystemPowerState(Fdo)) ?
             __FdoSetSystemPowerUp(Fdo, Irp) :
             __FdoSetSystemPowerDown(Fdo, Irp);

done:
    Trace("<==== (%s:%s)(%08x)\n",
          PowerSystemStateName(SystemState), 
          PowerActionName(PowerAction),
          status);
    return status;
}

static FORCEINLINE NTSTATUS
__FdoQueryDevicePowerUp(
    IN  PXENBUS_FDO     Fdo,
    IN  PIRP            Irp
    )
{
    PIO_STACK_LOCATION  StackLocation;
    DEVICE_POWER_STATE  DeviceState;
    NTSTATUS            status;

    StackLocation = IoGetCurrentIrpStackLocation(Irp);
    DeviceState = StackLocation->Parameters.Power.State.DeviceState;

    ASSERT3U(DeviceState, <,  __FdoGetDevicePowerState(Fdo));

    status = FdoForwardIrpSynchronously(Fdo, Irp);

    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

static FORCEINLINE NTSTATUS
__FdoQueryDevicePowerDown(
    IN  PXENBUS_FDO     Fdo,
    IN  PIRP            Irp
    )
{
    PIO_STACK_LOCATION  StackLocation;
    DEVICE_POWER_STATE  DeviceState;
    NTSTATUS            status;

    StackLocation = IoGetCurrentIrpStackLocation(Irp);
    DeviceState = StackLocation->Parameters.Power.State.DeviceState;

    ASSERT3U(DeviceState, >,  __FdoGetDevicePowerState(Fdo));

    status = FdoForwardIrpSynchronously(Fdo, Irp);
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

static FORCEINLINE NTSTATUS
__FdoQueryDevicePower(
    IN  PXENBUS_FDO     Fdo,
    IN  PIRP            Irp
    )
{
    PIO_STACK_LOCATION  StackLocation;
    DEVICE_POWER_STATE  DeviceState;
    POWER_ACTION        PowerAction;
    NTSTATUS            status;

    StackLocation = IoGetCurrentIrpStackLocation(Irp);
    DeviceState = StackLocation->Parameters.Power.State.DeviceState;
    PowerAction = StackLocation->Parameters.Power.ShutdownType;

    Trace("====> (%s:%s)\n",
          PowerDeviceStateName(DeviceState), 
          PowerActionName(PowerAction));

    ASSERT3U(PowerAction, <,  PowerActionShutdown);

    if (DeviceState == __FdoGetDevicePowerState(Fdo)) {
        status = FdoForwardIrpSynchronously(Fdo, Irp);
        IoCompleteRequest(Irp, IO_NO_INCREMENT);

        goto done;
    }

    status = (DeviceState < __FdoGetDevicePowerState(Fdo)) ?
             __FdoQueryDevicePowerUp(Fdo, Irp) :
             __FdoQueryDevicePowerDown(Fdo, Irp);

done:
    Trace("<==== (%s:%s)(%08x)\n",
          PowerDeviceStateName(DeviceState), 
          PowerActionName(PowerAction),
          status);
    return status;
}

__drv_functionClass(REQUEST_POWER_COMPLETE)
__drv_sameIRQL
VOID
__FdoRequestQueryDevicePower(
    IN  PDEVICE_OBJECT      DeviceObject,
    IN  UCHAR               MinorFunction,
    IN  POWER_STATE         PowerState,
    IN  PVOID               Context,
    IN  PIO_STATUS_BLOCK    IoStatus
    )
{
    PKEVENT                 Event = Context;

    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(MinorFunction);
    UNREFERENCED_PARAMETER(PowerState);

    ASSERT(NT_SUCCESS(IoStatus->Status));

    KeSetEvent(Event, IO_NO_INCREMENT, FALSE);
}

static VOID
FdoRequestQueryDevicePower(
    IN  PXENBUS_FDO         Fdo,
    IN  DEVICE_POWER_STATE  DeviceState
    )
{
    POWER_STATE             PowerState;
    KEVENT                  Event;
    NTSTATUS                status;

    Trace("%s\n", PowerDeviceStateName(DeviceState));

    ASSERT3U(KeGetCurrentIrql(), ==, PASSIVE_LEVEL);

    PowerState.DeviceState = DeviceState;
    KeInitializeEvent(&Event, NotificationEvent, FALSE);

    status = PoRequestPowerIrp(Fdo->LowerDeviceObject,
                               IRP_MN_QUERY_POWER,
                               PowerState,
                               __FdoRequestQueryDevicePower,
                               &Event,
                               NULL);
    ASSERT(NT_SUCCESS(status));

    (VOID) KeWaitForSingleObject(&Event,
                                 Executive,
                                 KernelMode,
                                 FALSE,
                                 NULL);
}

static FORCEINLINE NTSTATUS
__FdoQuerySystemPowerUp(
    IN  PXENBUS_FDO     Fdo,
    IN  PIRP            Irp
    )
{

    PIO_STACK_LOCATION  StackLocation;
    SYSTEM_POWER_STATE  SystemState;
    DEVICE_POWER_STATE  DeviceState;
    NTSTATUS            status;

    StackLocation = IoGetCurrentIrpStackLocation(Irp);
    SystemState = StackLocation->Parameters.Power.State.SystemState;

    ASSERT3U(SystemState, <,  __FdoGetSystemPowerState(Fdo));

    status = FdoForwardIrpSynchronously(Fdo, Irp);
    if (!NT_SUCCESS(status))
        goto done;

    DeviceState = Fdo->LowerDeviceCapabilities.DeviceState[SystemState];

    FdoRequestQueryDevicePower(Fdo, DeviceState);

done:
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

static FORCEINLINE NTSTATUS
__FdoQuerySystemPowerDown(
    IN  PXENBUS_FDO     Fdo,
    IN  PIRP            Irp
    )
{
    PIO_STACK_LOCATION  StackLocation;
    SYSTEM_POWER_STATE  SystemState;
    DEVICE_POWER_STATE  DeviceState;
    NTSTATUS            status;

    StackLocation = IoGetCurrentIrpStackLocation(Irp);
    SystemState = StackLocation->Parameters.Power.State.SystemState;

    ASSERT3U(SystemState, >,  __FdoGetSystemPowerState(Fdo));

    DeviceState = Fdo->LowerDeviceCapabilities.DeviceState[SystemState];

    FdoRequestQueryDevicePower(Fdo, DeviceState);

    status = FdoForwardIrpSynchronously(Fdo, Irp);
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

static FORCEINLINE NTSTATUS
__FdoQuerySystemPower(
    IN  PXENBUS_FDO     Fdo,
    IN  PIRP            Irp
    )
{
    PIO_STACK_LOCATION  StackLocation;
    SYSTEM_POWER_STATE  SystemState;
    POWER_ACTION        PowerAction;
    NTSTATUS            status;

    StackLocation = IoGetCurrentIrpStackLocation(Irp);
    SystemState = StackLocation->Parameters.Power.State.SystemState;
    PowerAction = StackLocation->Parameters.Power.ShutdownType;

    Trace("====> (%s:%s)\n",
          PowerSystemStateName(SystemState), 
          PowerActionName(PowerAction));

    ASSERT3U(PowerAction, <,  PowerActionShutdown);

    if (SystemState == __FdoGetSystemPowerState(Fdo)) {
        status = FdoForwardIrpSynchronously(Fdo, Irp);
        IoCompleteRequest(Irp, IO_NO_INCREMENT);

        goto done;
    }

    status = (SystemState < __FdoGetSystemPowerState(Fdo)) ?
             __FdoQuerySystemPowerUp(Fdo, Irp) :
             __FdoQuerySystemPowerDown(Fdo, Irp);

done:
    Trace("<==== (%s:%s)(%08x)\n",
          PowerSystemStateName(SystemState), 
          PowerActionName(PowerAction),
          status);

    return status;
}

static NTSTATUS
FdoDevicePower(
    IN  PXENBUS_THREAD  Self,
    IN  PVOID           Context
    )
{
    PXENBUS_FDO         Fdo = Context;
    PKEVENT             Event;

    Event = ThreadGetEvent(Self);

    for (;;) {
        PIRP                Irp;
        PIO_STACK_LOCATION  StackLocation;
        UCHAR               MinorFunction;

        if (Fdo->DevicePowerIrp == NULL) {
            (VOID) KeWaitForSingleObject(Event,
                                         Executive,
                                         KernelMode,
                                         FALSE,
                                         NULL);
            KeClearEvent(Event);
        }

        if (ThreadIsAlerted(Self))
            break;

        Irp = Fdo->DevicePowerIrp;

        if (Irp == NULL)
            continue;

        Fdo->DevicePowerIrp = NULL;
        KeMemoryBarrier();

        StackLocation = IoGetCurrentIrpStackLocation(Irp);
        MinorFunction = StackLocation->MinorFunction;

        switch (StackLocation->MinorFunction) {
        case IRP_MN_SET_POWER:
            (VOID) __FdoSetDevicePower(Fdo, Irp);
            break;

        case IRP_MN_QUERY_POWER:
            (VOID) __FdoQueryDevicePower(Fdo, Irp);
            break;

        default:
            ASSERT(FALSE);
            break;
        }
    }

    return STATUS_SUCCESS;
}

static NTSTATUS
FdoSystemPower(
    IN  PXENBUS_THREAD  Self,
    IN  PVOID           Context
    )
{
    PXENBUS_FDO         Fdo = Context;
    PKEVENT             Event;

    Event = ThreadGetEvent(Self);

    for (;;) {
        PIRP                Irp;
        PIO_STACK_LOCATION  StackLocation;
        UCHAR               MinorFunction;

        if (Fdo->SystemPowerIrp == NULL) {
            (VOID) KeWaitForSingleObject(Event,
                                         Executive,
                                         KernelMode,
                                         FALSE,
                                         NULL);
            KeClearEvent(Event);
        }

        if (ThreadIsAlerted(Self))
            break;

        Irp = Fdo->SystemPowerIrp;

        if (Irp == NULL)
            continue;

        Fdo->SystemPowerIrp = NULL;
        KeMemoryBarrier();

        StackLocation = IoGetCurrentIrpStackLocation(Irp);
        MinorFunction = StackLocation->MinorFunction;

        switch (StackLocation->MinorFunction) {
        case IRP_MN_SET_POWER:
            (VOID) __FdoSetSystemPower(Fdo, Irp);
            break;

        case IRP_MN_QUERY_POWER:
            (VOID) __FdoQuerySystemPower(Fdo, Irp);
            break;

        default:
            ASSERT(FALSE);
            break;
        }
    }

    return STATUS_SUCCESS;
}

static DECLSPEC_NOINLINE NTSTATUS
FdoDispatchPower(
    IN  PXENBUS_FDO     Fdo,
    IN  PIRP            Irp
    )
{
    PIO_STACK_LOCATION  StackLocation;
    UCHAR               MinorFunction;
    POWER_STATE_TYPE    PowerType;
    POWER_ACTION        PowerAction;
    NTSTATUS            status;

    StackLocation = IoGetCurrentIrpStackLocation(Irp);
    MinorFunction = StackLocation->MinorFunction;

    if (MinorFunction != IRP_MN_QUERY_POWER &&
        MinorFunction != IRP_MN_SET_POWER) {
        IoSkipCurrentIrpStackLocation(Irp);
        status = IoCallDriver(Fdo->LowerDeviceObject, Irp);

        goto done;
    }

    PowerType = StackLocation->Parameters.Power.Type;
    PowerAction = StackLocation->Parameters.Power.ShutdownType;

    if (PowerAction >= PowerActionShutdown) {
        IoSkipCurrentIrpStackLocation(Irp);
        status = IoCallDriver(Fdo->LowerDeviceObject, Irp);

        goto done;
    }

    switch (PowerType) {
    case DevicePowerState:
        IoMarkIrpPending(Irp);

        ASSERT3P(Fdo->DevicePowerIrp, ==, NULL);
        Fdo->DevicePowerIrp = Irp;
        KeMemoryBarrier();

        ThreadWake(Fdo->DevicePowerThread);

        status = STATUS_PENDING;
        break;

    case SystemPowerState:
        IoMarkIrpPending(Irp);

        ASSERT3P(Fdo->SystemPowerIrp, ==, NULL);
        Fdo->SystemPowerIrp = Irp;
        KeMemoryBarrier();

        ThreadWake(Fdo->SystemPowerThread);

        status = STATUS_PENDING;
        break;

    default:
        IoSkipCurrentIrpStackLocation(Irp);
        status = IoCallDriver(Fdo->LowerDeviceObject, Irp);
        break;
    }

done:
    return status;
}

static DECLSPEC_NOINLINE NTSTATUS
FdoDispatchDefault(
    IN  PXENBUS_FDO Fdo,
    IN  PIRP        Irp
    )
{
    NTSTATUS        status;

    IoSkipCurrentIrpStackLocation(Irp);
    status = IoCallDriver(Fdo->LowerDeviceObject, Irp);

    return status;
}

NTSTATUS
FdoDispatch(
    IN  PXENBUS_FDO     Fdo,
    IN  PIRP            Irp
    )
{
    PIO_STACK_LOCATION  StackLocation;
    NTSTATUS            status;

    StackLocation = IoGetCurrentIrpStackLocation(Irp);

    switch (StackLocation->MajorFunction) {
    case IRP_MJ_PNP:
        status = FdoDispatchPnp(Fdo, Irp);
        break;

    case IRP_MJ_POWER:
        status = FdoDispatchPower(Fdo, Irp);
        break;

    default:
        status = FdoDispatchDefault(Fdo, Irp);
        break;
    }

    return status;
}

static FORCEINLINE NTSTATUS
__FdoAcquireLowerBusInterface(
    IN  PXENBUS_FDO     Fdo
    )
{
    KEVENT              Event;
    IO_STATUS_BLOCK     StatusBlock;
    PIRP                Irp;
    PIO_STACK_LOCATION  StackLocation;
    NTSTATUS            status;

    ASSERT3U(KeGetCurrentIrql(), ==, PASSIVE_LEVEL);

    KeInitializeEvent(&Event, NotificationEvent, FALSE);
    RtlZeroMemory(&StatusBlock, sizeof(IO_STATUS_BLOCK));

    Irp = IoBuildSynchronousFsdRequest(IRP_MJ_PNP,
                                       Fdo->LowerDeviceObject,
                                       NULL,
                                       0,
                                       NULL,
                                       &Event,
                                       &StatusBlock);

    status = STATUS_UNSUCCESSFUL;
    if (Irp == NULL)
        goto fail1;

    StackLocation = IoGetNextIrpStackLocation(Irp);
    StackLocation->MinorFunction = IRP_MN_QUERY_INTERFACE;

    StackLocation->Parameters.QueryInterface.InterfaceType = &GUID_BUS_INTERFACE_STANDARD;
    StackLocation->Parameters.QueryInterface.Size = sizeof (BUS_INTERFACE_STANDARD);
    StackLocation->Parameters.QueryInterface.Version = 1;
    StackLocation->Parameters.QueryInterface.Interface = (PINTERFACE)&Fdo->LowerBusInterface;
    
    Irp->IoStatus.Status = STATUS_NOT_SUPPORTED;

    status = IoCallDriver(Fdo->LowerDeviceObject, Irp);
    if (status == STATUS_PENDING) {
        (VOID) KeWaitForSingleObject(&Event,
                                     Executive,
                                     KernelMode,
                                     FALSE,
                                     NULL);
        status = StatusBlock.Status;
    }

    if (!NT_SUCCESS(status))
        goto fail2;

    status = STATUS_INVALID_PARAMETER;
    if (Fdo->LowerBusInterface.Version != 1)
        goto fail3;

    return STATUS_SUCCESS;

fail3:
    Error("fail3\n");

fail2:
    Error("fail2\n");

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

static FORCEINLINE VOID
__FdoReleaseLowerBusInterface(
    IN  PXENBUS_FDO         Fdo
    )
{
    PBUS_INTERFACE_STANDARD BusInterface;

    BusInterface = &Fdo->LowerBusInterface;
    BusInterface->InterfaceDereference(BusInterface->Context);

    RtlZeroMemory(BusInterface, sizeof (BUS_INTERFACE_STANDARD));
}

NTSTATUS
FdoCreate(
    IN  PDEVICE_OBJECT  PhysicalDeviceObject
    )
{
    PDEVICE_OBJECT      FunctionDeviceObject;
    PXENBUS_DX          Dx;
    PXENBUS_FDO         Fdo;
    WCHAR               Name[MAXNAMELEN * sizeof (WCHAR)];
    ULONG               Size;
    NTSTATUS            status;

#pragma prefast(suppress:28197) // Possibly leaking memory 'FunctionDeviceObject'
    status = IoCreateDevice(DriverObject,
                            sizeof (XENBUS_DX),
                            NULL,
                            FILE_DEVICE_BUS_EXTENDER,
                            FILE_DEVICE_SECURE_OPEN,
                            FALSE,
                            &FunctionDeviceObject);
    if (!NT_SUCCESS(status))
        goto fail1;

    Dx = (PXENBUS_DX)FunctionDeviceObject->DeviceExtension;
    RtlZeroMemory(Dx, sizeof (XENBUS_DX));

    Dx->Type = FUNCTION_DEVICE_OBJECT;
    Dx->DeviceObject = FunctionDeviceObject;
    Dx->DevicePnpState = Added;
    Dx->SystemPowerState = PowerSystemShutdown;
    Dx->DevicePowerState = PowerDeviceD3;

    Fdo = __FdoAllocate(sizeof (XENBUS_FDO));

    status = STATUS_NO_MEMORY;
    if (Fdo == NULL)
        goto fail2;

    Fdo->Dx = Dx;
    Fdo->PhysicalDeviceObject = PhysicalDeviceObject;
    Fdo->LowerDeviceObject = IoAttachDeviceToDeviceStack(FunctionDeviceObject,
                                                         PhysicalDeviceObject);

    status = ThreadCreate(FdoSystemPower, Fdo, &Fdo->SystemPowerThread);
    if (!NT_SUCCESS(status))
        goto fail3;

    status = ThreadCreate(FdoDevicePower, Fdo, &Fdo->DevicePowerThread);
    if (!NT_SUCCESS(status))
        goto fail4;

    status = IoGetDeviceProperty(PhysicalDeviceObject,
                                 DevicePropertyLocationInformation,
                                 sizeof (Name),
                                 Name,
                                 &Size);
    if (!NT_SUCCESS(status))
        goto fail5;

    status = __FdoSetName(Fdo, Name);
    if (!NT_SUCCESS(status))
        goto fail6;

    status = __FdoAcquireLowerBusInterface(Fdo);
    if (!NT_SUCCESS(status))
        goto fail7;

    if (DriverParameters.Balloon != 0) {
        status = BalloonInitialize(&Fdo->Balloon);
        if (!NT_SUCCESS(status))
            goto fail8;
    } else {
        Info("BALLOON DISABLED\n");
    }

    InitializeMutex(&Fdo->Mutex);
    InitializeListHead(&Dx->ListEntry);
    Fdo->References = 1;

    Info("%p (%s)\n",
         FunctionDeviceObject,
         __FdoGetName(Fdo));

    Dx->Fdo = Fdo;
    FunctionDeviceObject->Flags &= ~DO_DEVICE_INITIALIZING;

    return STATUS_SUCCESS;

fail8:
    Error("fail8\n");

    __FdoReleaseLowerBusInterface(Fdo);

fail7:
    Error("fail7\n");

fail6:
    Error("fail6\n");

fail5:
    Error("fail5\n");

    ThreadAlert(Fdo->DevicePowerThread);
    ThreadJoin(Fdo->DevicePowerThread);
    Fdo->DevicePowerThread = NULL;
    
fail4:
    Error("fail4\n");

    ThreadAlert(Fdo->SystemPowerThread);
    ThreadJoin(Fdo->SystemPowerThread);
    Fdo->SystemPowerThread = NULL;
    
fail3:
    Error("fail3\n");

#pragma prefast(suppress:28183) // Fdo->LowerDeviceObject could be NULL
    IoDetachDevice(Fdo->LowerDeviceObject);

    Fdo->PhysicalDeviceObject = NULL;
    Fdo->LowerDeviceObject = NULL;
    Fdo->Dx = NULL;

    ASSERT(IsZeroMemory(Fdo, sizeof (XENBUS_FDO)));
    __FdoFree(Fdo);

fail2:
    Error("fail2\n");

    IoDeleteDevice(FunctionDeviceObject);

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

VOID
FdoDestroy(
    IN  PXENBUS_FDO Fdo
    )
{
    PXENBUS_DX      Dx = Fdo->Dx;
    PDEVICE_OBJECT  FunctionDeviceObject = Dx->DeviceObject;

    ASSERT(IsListEmpty(&Dx->ListEntry));
    ASSERT3U(Fdo->References, ==, 0);
    ASSERT3U(__FdoGetDevicePnpState(Fdo), ==, Deleted);

    Fdo->NotDisableable = FALSE;

    Info("%p (%s)\n",
         FunctionDeviceObject,
         __FdoGetName(Fdo));

    Dx->Fdo = NULL;

    RtlZeroMemory(&Fdo->Mutex, sizeof (XENBUS_MUTEX));

    if (Fdo->Balloon != NULL) {
        BalloonTeardown(Fdo->Balloon);
        Fdo->Balloon = NULL;
    }

    __FdoReleaseLowerBusInterface(Fdo);

    ThreadAlert(Fdo->DevicePowerThread);
    ThreadJoin(Fdo->DevicePowerThread);
    Fdo->DevicePowerThread = NULL;

    ThreadAlert(Fdo->SystemPowerThread);
    ThreadJoin(Fdo->SystemPowerThread);
    Fdo->SystemPowerThread = NULL;

    IoDetachDevice(Fdo->LowerDeviceObject);

    RtlZeroMemory(&Fdo->LowerDeviceCapabilities, sizeof (DEVICE_CAPABILITIES));
    Fdo->LowerDeviceObject = NULL;
    Fdo->PhysicalDeviceObject = NULL;
    Fdo->Dx = NULL;

    ASSERT(IsZeroMemory(Fdo, sizeof (XENBUS_FDO)));
    __FdoFree(Fdo);

    IoDeleteDevice(FunctionDeviceObject);
}
