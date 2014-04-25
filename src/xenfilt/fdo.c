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

#include "emulated.h"
#include "unplug.h"
#include "names.h"
#include "fdo.h"
#include "pdo.h"
#include "thread.h"
#include "driver.h"
#include "registry.h"
#include "mutex.h"
#include "dbg_print.h"
#include "assert.h"

#define FDO_TAG 'ODF'

#define MAXNAMELEN  128

struct _XENFILT_FDO {
    PXENFILT_DX                     Dx;
    PDEVICE_OBJECT                  LowerDeviceObject;
    PDEVICE_OBJECT                  PhysicalDeviceObject;

    PXENFILT_THREAD                 SystemPowerThread;
    PIRP                            SystemPowerIrp;
    PXENFILT_THREAD                 DevicePowerThread;
    PIRP                            DevicePowerIrp;

    MUTEX                           Mutex;
    ULONG                           References;

    CHAR                            Prefix[MAXNAMELEN];

    XENFILT_EMULATED_OBJECT_TYPE    Type;
    XENFILT_EMULATED_INTERFACE      EmulatedInterface;
    XENFILT_UNPLUG_INTERFACE        UnplugInterface;
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

static FORCEINLINE VOID
__FdoSetDevicePnpState(
    IN  PXENFILT_FDO        Fdo,
    IN  DEVICE_PNP_STATE    State
    )
{
    PXENFILT_DX             Dx = Fdo->Dx;

    // We can never transition out of the deleted state
    ASSERT(Dx->DevicePnpState != Deleted || State == Deleted);

    Dx->PreviousDevicePnpState = Dx->DevicePnpState;
    Dx->DevicePnpState = State;
}

static FORCEINLINE VOID
__FdoRestoreDevicePnpState(
    IN  PXENFILT_FDO        Fdo,
    IN  DEVICE_PNP_STATE    State
    )
{
    PXENFILT_DX             Dx = Fdo->Dx;

    if (Dx->DevicePnpState == State)
        Dx->DevicePnpState = Dx->PreviousDevicePnpState;
}

static FORCEINLINE DEVICE_PNP_STATE
__FdoGetDevicePnpState(
    IN  PXENFILT_FDO    Fdo
    )
{
    PXENFILT_DX         Dx = Fdo->Dx;

    return Dx->DevicePnpState;
}

static FORCEINLINE VOID
__FdoSetDevicePowerState(
    IN  PXENFILT_FDO        Fdo,
    IN  DEVICE_POWER_STATE  State
    )
{
    PXENFILT_DX             Dx = Fdo->Dx;

    Dx->DevicePowerState = State;
}

static FORCEINLINE DEVICE_POWER_STATE
__FdoGetDevicePowerState(
    IN  PXENFILT_FDO    Fdo
    )
{
    PXENFILT_DX         Dx = Fdo->Dx;

    return Dx->DevicePowerState;
}

static FORCEINLINE VOID
__FdoSetSystemPowerState(
    IN  PXENFILT_FDO        Fdo,
    IN  SYSTEM_POWER_STATE  State
    )
{
    PXENFILT_DX              Dx = Fdo->Dx;

    Dx->SystemPowerState = State;
}

static FORCEINLINE SYSTEM_POWER_STATE
__FdoGetSystemPowerState(
    IN  PXENFILT_FDO    Fdo
    )
{
    PXENFILT_DX         Dx = Fdo->Dx;

    return Dx->SystemPowerState;
}

static FORCEINLINE VOID
__FdoSetName(
    IN  PXENFILT_FDO    Fdo,
    IN  PANSI_STRING    Ansi
    )
{
    PXENFILT_DX         Dx = Fdo->Dx;
    NTSTATUS            status;

    status = RtlStringCbPrintfA(Dx->Name, MAX_DEVICE_ID_LEN, "%Z", Ansi);
    ASSERT(NT_SUCCESS(status));
}

static FORCEINLINE PCHAR
__FdoGetName(
    IN  PXENFILT_FDO    Fdo
    )
{
    PXENFILT_DX         Dx = Fdo->Dx;

    return Dx->Name;
}

static FORCEINLINE NTSTATUS
__FdoSetEmulatedType(
    IN  PXENFILT_FDO    Fdo,
    IN  PANSI_STRING    Ansi
    )
{
    NTSTATUS            status;

    status = STATUS_INVALID_PARAMETER;
    if (_strnicmp(Ansi->Buffer, "DEVICE", Ansi->Length) == 0)
        Fdo->Type = XENFILT_EMULATED_OBJECT_TYPE_DEVICE;
    else if (_strnicmp(Ansi->Buffer, "DISK", Ansi->Length) == 0)
        Fdo->Type = XENFILT_EMULATED_OBJECT_TYPE_DISK;
    else
        goto fail1;

    return STATUS_SUCCESS;

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

static FORCEINLINE XENFILT_EMULATED_OBJECT_TYPE
__FdoGetEmulatedType(
    IN  PXENFILT_FDO    Fdo
    )
{
    return Fdo->Type;
}

static FORCEINLINE PDEVICE_OBJECT
__FdoGetPhysicalDeviceObject(
    IN  PXENFILT_FDO    Fdo
    )
{
    return Fdo->PhysicalDeviceObject;
}


static FORCEINLINE NTSTATUS
__FdoSetWindowsPEPrefix(
    IN  PXENFILT_FDO    Fdo
    )
{
    HANDLE                  ServiceKey;
    DWORD                   WindowsPEMode;
    NTSTATUS                status;

    status = RegistryOpenServiceKey(KEY_READ,
                                    &ServiceKey);
    if (!NT_SUCCESS(status))
        goto fail1;

    status = RegistryQueryDwordValue(ServiceKey,
                                     "WindowsPEMode",
                                     &WindowsPEMode);
    if (!NT_SUCCESS(status))
        goto fail2;

    status = STATUS_UNSUCCESSFUL;

    if (!WindowsPEMode)
        goto fail3;

    Fdo->Prefix[0]=0;
    
    RegistryCloseKey(ServiceKey);

    return STATUS_SUCCESS;

fail3:
    Error("fail3\n");

fail2:
    Error("fail2\n");
    RegistryCloseKey(ServiceKey);

fail1:
    Error("fail1\n");

    return status;
}

static FORCEINLINE NTSTATUS
__FdoSetPrefix(
    IN  PXENFILT_FDO        Fdo
    )
{
    HANDLE                  HardwareKey;
    PANSI_STRING            ParentIdPrefix;
    NTSTATUS                status;

    status = RegistryOpenHardwareKey(__FdoGetPhysicalDeviceObject(Fdo),
                                     KEY_READ,
                                     &HardwareKey);
    if (!NT_SUCCESS(status))
        goto fail1;

    status = RegistryQuerySzValue(HardwareKey,
                                  "ParentIdPrefix",
                                  &ParentIdPrefix);
    if (!NT_SUCCESS(status))
        goto fail2;

    status = RtlStringCbPrintfA(Fdo->Prefix,
                                MAXNAMELEN,
                                "%Z",
                                ParentIdPrefix);
    ASSERT(NT_SUCCESS(status));

    RegistryFreeSzValue(ParentIdPrefix);

    RegistryCloseKey(HardwareKey);

    return STATUS_SUCCESS;

fail2:
    Error("fail2\n");

    RegistryCloseKey(HardwareKey);

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

static FORCEINLINE PCHAR
__FdoGetPrefix(
    IN  PXENFILT_FDO    Fdo
    )
{
    return Fdo->Prefix;
}

PCHAR
FdoGetPrefix(
    IN  PXENFILT_FDO    Fdo
    )
{
    return __FdoGetPrefix(Fdo);
}

VOID
FdoAddPhysicalDeviceObject(
    IN  PXENFILT_FDO    Fdo,
    IN  PDEVICE_OBJECT  DeviceObject
    )
{
    PXENFILT_DX         Dx;

    Dx = (PXENFILT_DX)DeviceObject->DeviceExtension;
    ASSERT3U(Dx->Type, ==, PHYSICAL_DEVICE_OBJECT);

    InsertTailList(&Fdo->Dx->ListEntry, &Dx->ListEntry);
    ASSERT3U(Fdo->References, !=, 0);
    Fdo->References++;
}

VOID
FdoRemovePhysicalDeviceObject(
    IN  PXENFILT_FDO    Fdo,
    IN  PDEVICE_OBJECT  DeviceObject
    )
{
    PXENFILT_DX         Dx;

    Dx = (PXENFILT_DX)DeviceObject->DeviceExtension;
    ASSERT3U(Dx->Type, ==, PHYSICAL_DEVICE_OBJECT);

    RemoveEntryList(&Dx->ListEntry);
    ASSERT3U(Fdo->References, !=, 0);
    --Fdo->References;
}

static FORCEINLINE VOID
__FdoAcquireMutex(
    IN  PXENFILT_FDO     Fdo
    )
{
    AcquireMutex(&Fdo->Mutex);
}

VOID
FdoAcquireMutex(
    IN  PXENFILT_FDO     Fdo
    )
{
    __FdoAcquireMutex(Fdo);
}

static FORCEINLINE VOID
__FdoReleaseMutex(
    IN  PXENFILT_FDO     Fdo
    )
{
    ReleaseMutex(&Fdo->Mutex);
}

VOID
FdoReleaseMutex(
    IN  PXENFILT_FDO     Fdo
    )
{
    __FdoReleaseMutex(Fdo);

    if (Fdo->References == 0)
        FdoDestroy(Fdo);
}

PXENFILT_EMULATED_INTERFACE
FdoGetEmulatedInterface(
    IN  PXENFILT_FDO     Fdo
    )
{
    return &Fdo->EmulatedInterface;
}

static FORCEINLINE PXENFILT_UNPLUG_INTERFACE
__FdoGetUnplugInterface(
    IN  PXENFILT_FDO     Fdo
    )
{
    return &Fdo->UnplugInterface;
}

PXENFILT_UNPLUG_INTERFACE
FdoGetUnplugInterface(
    IN  PXENFILT_FDO     Fdo
    )
{
    return __FdoGetUnplugInterface(Fdo);
}

static FORCEINLINE VOID
__FdoEnumerate(
    IN  PXENFILT_FDO        Fdo,
    IN  PDEVICE_RELATIONS   Relations
    )
{
    PDEVICE_OBJECT          *PhysicalDeviceObject;
    ULONG                   Count;
    PLIST_ENTRY             ListEntry;
    ULONG                   Index;
    NTSTATUS                status;

    Count = Relations->Count;
    ASSERT(Count != 0);

    PhysicalDeviceObject = __FdoAllocate(sizeof (PDEVICE_OBJECT) * Count);

    status = STATUS_NO_MEMORY;
    if (PhysicalDeviceObject == NULL)
        goto fail1;

    RtlCopyMemory(PhysicalDeviceObject,
                  Relations->Objects,
                  sizeof (PDEVICE_OBJECT) * Count);

    AcquireMutex(&Fdo->Mutex);

    // Remove any PDOs that do not appear in the device list
    ListEntry = Fdo->Dx->ListEntry.Flink;
    while (ListEntry != &Fdo->Dx->ListEntry) {
        PLIST_ENTRY     Next = ListEntry->Flink;
        PXENFILT_DX     Dx = CONTAINING_RECORD(ListEntry, XENFILT_DX, ListEntry);
        PXENFILT_PDO    Pdo = Dx->Pdo;
        BOOLEAN         Missing;

        Missing = TRUE;
        for (Index = 0; Index < Count; Index++) {
            if (PdoGetPhysicalDeviceObject(Pdo) == PhysicalDeviceObject[Index]) {
                Missing = FALSE;
#pragma prefast(suppress:6387)  // PhysicalDeviceObject[Index] could be NULL
                ObDereferenceObject(PhysicalDeviceObject[Index]);
                PhysicalDeviceObject[Index] = NULL; // avoid duplication
                break;
            }
        }

        if (Missing && !PdoIsMissing(Pdo)) {
            if (PdoGetDevicePnpState(Pdo) == Present) {
                PdoSetDevicePnpState(Pdo, Deleted);
                PdoDestroy(Pdo);
            } else {
                PdoSetMissing(Pdo, "device disappeared");
            }
        }

        ListEntry = Next;
    }

    // Walk the list and create PDO filters for any new devices
    for (Index = 0; Index < Count; Index++) {
#pragma warning(suppress:6385)  // Reading invalid data from 'PhysicalDeviceObject'
        if (PhysicalDeviceObject[Index] != NULL) {
            (VOID) PdoCreate(Fdo,
                             PhysicalDeviceObject[Index],
                             __FdoGetEmulatedType(Fdo));
            ObDereferenceObject(PhysicalDeviceObject[Index]);
        }
    }
    
    ReleaseMutex(&Fdo->Mutex);

    __FdoFree(PhysicalDeviceObject);
    return;

fail1:
    Error("fail1 (%08x)\n", status);
}

static FORCEINLINE VOID
__FdoS4ToS3(
    IN  PXENFILT_FDO            Fdo
    )
{
    KIRQL                       Irql;
    PXENFILT_UNPLUG_INTERFACE   UnplugInterface;

    ASSERT3U(__FdoGetSystemPowerState(Fdo), ==, PowerSystemHibernate);

    KeRaiseIrql(DISPATCH_LEVEL, &Irql); // Flush out any attempt to use pageable memory

    UnplugInterface = __FdoGetUnplugInterface(Fdo);

    UNPLUG(Acquire, UnplugInterface);
    UNPLUG(Replay, UnplugInterface);
    UNPLUG(Release, UnplugInterface);

    __FdoSetSystemPowerState(Fdo, PowerSystemSleeping3);

    KeLowerIrql(Irql);
}

static FORCEINLINE VOID
__FdoS3ToS4(
    IN  PXENFILT_FDO    Fdo
    )
{
    ASSERT3U(__FdoGetSystemPowerState(Fdo), ==, PowerSystemSleeping3);

    __FdoSetSystemPowerState(Fdo, PowerSystemHibernate);
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
    IN  PXENFILT_FDO    Fdo,
    IN  PIRP            Irp
    )
{
    KEVENT              Event;
    NTSTATUS            status;

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

static DECLSPEC_NOINLINE NTSTATUS
FdoStartDevice(
    IN  PXENFILT_FDO    Fdo,
    IN  PIRP            Irp
    )
{
    POWER_STATE         PowerState;
    NTSTATUS            status;

    status = IoAcquireRemoveLock(&Fdo->Dx->RemoveLock, Irp);
    if (!NT_SUCCESS(status))
        goto fail1;

    status = FdoForwardIrpSynchronously(Fdo, Irp);
    if (!NT_SUCCESS(status))
        goto fail2;

    __FdoSetSystemPowerState(Fdo, PowerSystemHibernate);

    __FdoS4ToS3(Fdo);
    
    __FdoSetSystemPowerState(Fdo, PowerSystemWorking);

    __FdoSetDevicePowerState(Fdo, PowerDeviceD0);

    PowerState.DeviceState = PowerDeviceD0;
    PoSetPowerState(Fdo->Dx->DeviceObject,
                    DevicePowerState,
                    PowerState);

    __FdoSetDevicePnpState(Fdo, Started);

    IoReleaseRemoveLock(&Fdo->Dx->RemoveLock, Irp);

    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;

fail2:
    Error("fail2\n");

    IoReleaseRemoveLock(&Fdo->Dx->RemoveLock, Irp);

fail1:
    Error("fail1 (%08x)\n", status);

    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

__drv_functionClass(IO_COMPLETION_ROUTINE)
__drv_sameIRQL
static NTSTATUS
__FdoQueryStopDevice(
    IN  PDEVICE_OBJECT  DeviceObject,
    IN  PIRP            Irp,
    IN  PVOID           Context
    )
{
    PXENFILT_FDO        Fdo = Context;

    UNREFERENCED_PARAMETER(DeviceObject);

    if (Irp->PendingReturned)
        IoMarkIrpPending(Irp);

    IoReleaseRemoveLock(&Fdo->Dx->RemoveLock, Irp);

    return STATUS_SUCCESS;
}

static DECLSPEC_NOINLINE NTSTATUS
FdoQueryStopDevice(
    IN  PXENFILT_FDO    Fdo,
    IN  PIRP            Irp
    )
{
    NTSTATUS            status;

    status = IoAcquireRemoveLock(&Fdo->Dx->RemoveLock, Irp);
    if (!NT_SUCCESS(status))
        goto fail1;

    __FdoSetDevicePnpState(Fdo, StopPending);
    Irp->IoStatus.Status = STATUS_SUCCESS;

    IoCopyCurrentIrpStackLocationToNext(Irp);
    IoSetCompletionRoutine(Irp,
                           __FdoQueryStopDevice,
                           Fdo,
                           TRUE,
                           TRUE,
                           TRUE);

    status = IoCallDriver(Fdo->LowerDeviceObject, Irp);

    return status;

fail1:
    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

__drv_functionClass(IO_COMPLETION_ROUTINE)
__drv_sameIRQL
static NTSTATUS
__FdoCancelStopDevice(
    IN  PDEVICE_OBJECT  DeviceObject,
    IN  PIRP            Irp,
    IN  PVOID           Context
    )
{
    PXENFILT_FDO        Fdo = Context;

    UNREFERENCED_PARAMETER(DeviceObject);

    if (Irp->PendingReturned)
        IoMarkIrpPending(Irp);

    IoReleaseRemoveLock(&Fdo->Dx->RemoveLock, Irp);

    return STATUS_SUCCESS;
}

static DECLSPEC_NOINLINE NTSTATUS
FdoCancelStopDevice(
    IN  PXENFILT_FDO    Fdo,
    IN  PIRP            Irp
    )
{
    NTSTATUS            status;

    status = IoAcquireRemoveLock(&Fdo->Dx->RemoveLock, Irp);
    if (!NT_SUCCESS(status))
        goto fail1;

    Irp->IoStatus.Status = STATUS_SUCCESS;

    __FdoRestoreDevicePnpState(Fdo, StopPending);

    IoCopyCurrentIrpStackLocationToNext(Irp);
    IoSetCompletionRoutine(Irp,
                           __FdoCancelStopDevice,
                           Fdo,
                           TRUE,
                           TRUE,
                           TRUE);

    status = IoCallDriver(Fdo->LowerDeviceObject, Irp);

    return status;

fail1:
    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

__drv_functionClass(IO_COMPLETION_ROUTINE)
__drv_sameIRQL
static NTSTATUS
__FdoStopDevice(
    IN  PDEVICE_OBJECT  DeviceObject,
    IN  PIRP            Irp,
    IN  PVOID           Context
    )
{
    PXENFILT_FDO        Fdo = Context;

    UNREFERENCED_PARAMETER(DeviceObject);

    if (Irp->PendingReturned)
        IoMarkIrpPending(Irp);

    IoReleaseRemoveLock(&Fdo->Dx->RemoveLock, Irp);

    return STATUS_SUCCESS;
}

static DECLSPEC_NOINLINE NTSTATUS
FdoStopDevice(
    IN  PXENFILT_FDO    Fdo,
    IN  PIRP            Irp
    )
{
    POWER_STATE         PowerState;
    NTSTATUS            status;

    status = IoAcquireRemoveLock(&Fdo->Dx->RemoveLock, Irp);
    if (!NT_SUCCESS(status))
        goto fail1;

    if (__FdoGetDevicePowerState(Fdo) != PowerDeviceD0)
        goto done;

    PowerState.DeviceState = PowerDeviceD3;
    PoSetPowerState(Fdo->Dx->DeviceObject,
                    DevicePowerState,
                    PowerState);

    __FdoSetDevicePowerState(Fdo, PowerDeviceD3);

    __FdoSetSystemPowerState(Fdo, PowerSystemSleeping3);
    __FdoS3ToS4(Fdo);
    __FdoSetSystemPowerState(Fdo, PowerSystemShutdown);

done:
    __FdoSetDevicePnpState(Fdo, Stopped);
    Irp->IoStatus.Status = STATUS_SUCCESS;

    IoCopyCurrentIrpStackLocationToNext(Irp);
    IoSetCompletionRoutine(Irp,
                           __FdoStopDevice,
                           Fdo,
                           TRUE,
                           TRUE,
                           TRUE);

    status = IoCallDriver(Fdo->LowerDeviceObject, Irp);

    return status;

fail1:
    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

__drv_functionClass(IO_COMPLETION_ROUTINE)
__drv_sameIRQL
static NTSTATUS
__FdoQueryRemoveDevice(
    IN  PDEVICE_OBJECT  DeviceObject,
    IN  PIRP            Irp,
    IN  PVOID           Context
    )
{
    PXENFILT_FDO        Fdo = Context;

    UNREFERENCED_PARAMETER(DeviceObject);

    if (Irp->PendingReturned)
        IoMarkIrpPending(Irp);

    IoReleaseRemoveLock(&Fdo->Dx->RemoveLock, Irp);

    return STATUS_SUCCESS;
}

static DECLSPEC_NOINLINE NTSTATUS
FdoQueryRemoveDevice(
    IN  PXENFILT_FDO    Fdo,
    IN  PIRP            Irp
    )
{
    NTSTATUS            status;

    status = IoAcquireRemoveLock(&Fdo->Dx->RemoveLock, Irp);
    if (!NT_SUCCESS(status))
        goto fail1;

    __FdoSetDevicePnpState(Fdo, RemovePending);
    Irp->IoStatus.Status = STATUS_SUCCESS;

    IoCopyCurrentIrpStackLocationToNext(Irp);
    IoSetCompletionRoutine(Irp,
                           __FdoQueryRemoveDevice,
                           Fdo,
                           TRUE,
                           TRUE,
                           TRUE);

    status = IoCallDriver(Fdo->LowerDeviceObject, Irp);

    return status;

fail1:
    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

__drv_functionClass(IO_COMPLETION_ROUTINE)
__drv_sameIRQL
static NTSTATUS
__FdoCancelRemoveDevice(
    IN  PDEVICE_OBJECT  DeviceObject,
    IN  PIRP            Irp,
    IN  PVOID           Context
    )
{
    PXENFILT_FDO        Fdo = Context;

    UNREFERENCED_PARAMETER(DeviceObject);

    if (Irp->PendingReturned)
        IoMarkIrpPending(Irp);

    IoReleaseRemoveLock(&Fdo->Dx->RemoveLock, Irp);

    return STATUS_SUCCESS;
}

static DECLSPEC_NOINLINE NTSTATUS
FdoCancelRemoveDevice(
    IN  PXENFILT_FDO    Fdo,
    IN  PIRP            Irp
    )
{
    NTSTATUS            status;

    status = IoAcquireRemoveLock(&Fdo->Dx->RemoveLock, Irp);
    if (!NT_SUCCESS(status))
        goto fail1;

    __FdoRestoreDevicePnpState(Fdo, RemovePending);
    Irp->IoStatus.Status = STATUS_SUCCESS;

    IoCopyCurrentIrpStackLocationToNext(Irp);
    IoSetCompletionRoutine(Irp,
                           __FdoCancelRemoveDevice,
                           Fdo,
                           TRUE,
                           TRUE,
                           TRUE);

    status = IoCallDriver(Fdo->LowerDeviceObject, Irp);

    return status;

fail1:
    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

__drv_functionClass(IO_COMPLETION_ROUTINE)
__drv_sameIRQL
static NTSTATUS
__FdoSurpriseRemoval(
    IN  PDEVICE_OBJECT  DeviceObject,
    IN  PIRP            Irp,
    IN  PVOID           Context
    )
{
    PXENFILT_FDO        Fdo = Context;

    UNREFERENCED_PARAMETER(DeviceObject);

    if (Irp->PendingReturned)
        IoMarkIrpPending(Irp);

    IoReleaseRemoveLock(&Fdo->Dx->RemoveLock, Irp);

    return STATUS_SUCCESS;
}

static DECLSPEC_NOINLINE NTSTATUS
FdoSurpriseRemoval(
    IN  PXENFILT_FDO    Fdo,
    IN  PIRP            Irp
    )
{
    NTSTATUS            status;

    status = IoAcquireRemoveLock(&Fdo->Dx->RemoveLock, Irp);
    if (!NT_SUCCESS(status))
        goto fail1;

    __FdoSetDevicePnpState(Fdo, SurpriseRemovePending);
    Irp->IoStatus.Status = STATUS_SUCCESS;

    IoCopyCurrentIrpStackLocationToNext(Irp);
    IoSetCompletionRoutine(Irp,
                           __FdoSurpriseRemoval,
                           Fdo,
                           TRUE,
                           TRUE,
                           TRUE);

    status = IoCallDriver(Fdo->LowerDeviceObject, Irp);

    return status;

fail1:
    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

static DECLSPEC_NOINLINE NTSTATUS
FdoRemoveDevice(
    IN  PXENFILT_FDO    Fdo,
    IN  PIRP            Irp
    )
{
    POWER_STATE         PowerState;
    NTSTATUS            status;

    status = IoAcquireRemoveLock(&Fdo->Dx->RemoveLock, Irp);
    if (!NT_SUCCESS(status))
        goto fail1;

    if (__FdoGetDevicePowerState(Fdo) != PowerDeviceD0)
        goto done;

    PowerState.DeviceState = PowerDeviceD3;
    PoSetPowerState(Fdo->Dx->DeviceObject,
                    DevicePowerState,
                    PowerState);

    __FdoSetDevicePowerState(Fdo, PowerDeviceD3);

    __FdoSetSystemPowerState(Fdo, PowerSystemSleeping3);
    __FdoS3ToS4(Fdo);
    __FdoSetSystemPowerState(Fdo, PowerSystemShutdown);

done:
    __FdoSetDevicePnpState(Fdo, Deleted);

    IoReleaseRemoveLockAndWait(&Fdo->Dx->RemoveLock, Irp);

    status = FdoForwardIrpSynchronously(Fdo, Irp);
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    AcquireMutex(&Fdo->Mutex);
    ASSERT3U(Fdo->References, !=, 0);
    --Fdo->References;
    ReleaseMutex(&Fdo->Mutex);

    if (Fdo->References == 0)
        FdoDestroy(Fdo);

    return status;

fail1:
    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

__drv_functionClass(IO_COMPLETION_ROUTINE)
__drv_sameIRQL
static NTSTATUS
__FdoQueryDeviceRelations(
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

static DECLSPEC_NOINLINE NTSTATUS
FdoQueryDeviceRelations(
    IN  PXENFILT_FDO    Fdo,
    IN  PIRP            Irp
    )
{
    KEVENT              Event;
    PIO_STACK_LOCATION  StackLocation;
    ULONG               Size;
    PDEVICE_RELATIONS   Relations;
    PLIST_ENTRY         ListEntry;
    ULONG               Count;
    NTSTATUS            status;

    status = IoAcquireRemoveLock(&Fdo->Dx->RemoveLock, Irp);
    if (!NT_SUCCESS(status))
        goto fail1;

    KeInitializeEvent(&Event, NotificationEvent, FALSE);

    IoCopyCurrentIrpStackLocationToNext(Irp);
    IoSetCompletionRoutine(Irp,
                           __FdoQueryDeviceRelations,
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

    if (!NT_SUCCESS(status))
        goto fail2;

    StackLocation = IoGetCurrentIrpStackLocation(Irp);
    if (StackLocation->Parameters.QueryDeviceRelations.Type != BusRelations)
        goto done;

    Relations = (PDEVICE_RELATIONS)Irp->IoStatus.Information;

    if (Relations->Count != 0)
        __FdoEnumerate(Fdo, Relations);

    ExFreePool(Relations);

    AcquireMutex(&Fdo->Mutex);

    Count = 0;
    for (ListEntry = Fdo->Dx->ListEntry.Flink;
         ListEntry != &Fdo->Dx->ListEntry;
         ListEntry = ListEntry->Flink)
        Count++;

    Size = FIELD_OFFSET(DEVICE_RELATIONS, Objects) + (sizeof (DEVICE_OBJECT) * __min(Count, 1));

    Relations = ExAllocatePoolWithTag(PagedPool, Size, 'TLIF');

    status = STATUS_NO_MEMORY;
    if (Relations == NULL)
        goto fail3;

    RtlZeroMemory(Relations, Size);

    for (ListEntry = Fdo->Dx->ListEntry.Flink;
         ListEntry != &Fdo->Dx->ListEntry;
         ListEntry = ListEntry->Flink) {
        PXENFILT_DX     Dx = CONTAINING_RECORD(ListEntry, XENFILT_DX, ListEntry);
        PXENFILT_PDO    Pdo = Dx->Pdo;

        ASSERT3U(Dx->Type, ==, PHYSICAL_DEVICE_OBJECT);

        if (PdoGetDevicePnpState(Pdo) == Present)
            PdoSetDevicePnpState(Pdo, Enumerated);

        ObReferenceObject(PdoGetPhysicalDeviceObject(Pdo));
        Relations->Objects[Relations->Count++] = PdoGetPhysicalDeviceObject(Pdo);
    }

    ASSERT3U(Relations->Count, ==, Count);

    Trace("%d PDO(s)\n", Relations->Count);

    ReleaseMutex(&Fdo->Mutex);

    Irp->IoStatus.Information = (ULONG_PTR)Relations;
    status = STATUS_SUCCESS;

done:
    IoReleaseRemoveLock(&Fdo->Dx->RemoveLock, Irp);

    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;

fail3:
    ReleaseMutex(&Fdo->Mutex);

fail2:
    IoReleaseRemoveLock(&Fdo->Dx->RemoveLock, Irp);

fail1:
    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

__drv_functionClass(IO_COMPLETION_ROUTINE)
__drv_sameIRQL
static NTSTATUS
__FdoDispatchPnp(
    IN  PDEVICE_OBJECT  DeviceObject,
    IN  PIRP            Irp,
    IN  PVOID           Context
    )
{
    PXENFILT_FDO        Fdo = Context;

    UNREFERENCED_PARAMETER(DeviceObject);

    if (Irp->PendingReturned)
        IoMarkIrpPending(Irp);

    IoReleaseRemoveLock(&Fdo->Dx->RemoveLock, Irp);
    return STATUS_SUCCESS;
}

static DECLSPEC_NOINLINE NTSTATUS
FdoDispatchPnp(
    IN  PXENFILT_FDO    Fdo,
    IN  PIRP            Irp
    )
{
    PIO_STACK_LOCATION  StackLocation;
    UCHAR               MinorFunction;
    NTSTATUS            status;

    StackLocation = IoGetCurrentIrpStackLocation(Irp);
    MinorFunction = StackLocation->MinorFunction;

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

    default:
        status = IoAcquireRemoveLock(&Fdo->Dx->RemoveLock, Irp);
        if (!NT_SUCCESS(status))
            goto fail1;

        IoCopyCurrentIrpStackLocationToNext(Irp);
        IoSetCompletionRoutine(Irp,
                               __FdoDispatchPnp,
                               Fdo,
                               TRUE,
                               TRUE,
                               TRUE);

        status = IoCallDriver(Fdo->LowerDeviceObject, Irp);
        break;
    }

    return status;

fail1:
    Error("fail1 (%08x)\n", status);

    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

static FORCEINLINE NTSTATUS
__FdoSetDevicePowerUp(
    IN  PXENFILT_FDO    Fdo,
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

    __FdoSetDevicePowerState(Fdo, DeviceState);

done:
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

static FORCEINLINE NTSTATUS
__FdoSetDevicePowerDown(
    IN  PXENFILT_FDO    Fdo,
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

    __FdoSetDevicePowerState(Fdo, DeviceState);

    status = FdoForwardIrpSynchronously(Fdo, Irp);
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

static FORCEINLINE NTSTATUS
__FdoSetDevicePower(
    IN  PXENFILT_FDO    Fdo,
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

static FORCEINLINE NTSTATUS
__FdoSetSystemPowerUp(
    IN  PXENFILT_FDO    Fdo,
    IN  PIRP            Irp
    )
{
    PIO_STACK_LOCATION  StackLocation;
    SYSTEM_POWER_STATE  SystemState;
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
        __FdoS4ToS3(Fdo);
    }

    __FdoSetSystemPowerState(Fdo, SystemState);

done:
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

static FORCEINLINE NTSTATUS
__FdoSetSystemPowerDown(
    IN  PXENFILT_FDO     Fdo,
    IN  PIRP            Irp
    )
{
    PIO_STACK_LOCATION  StackLocation;
    SYSTEM_POWER_STATE  SystemState;
    NTSTATUS            status;

    StackLocation = IoGetCurrentIrpStackLocation(Irp);
    SystemState = StackLocation->Parameters.Power.State.SystemState;

    ASSERT3U(SystemState, >,  __FdoGetSystemPowerState(Fdo));

    Info("%s: %s -> %s\n",
         __FdoGetName(Fdo),
         PowerSystemStateName(__FdoGetSystemPowerState(Fdo)),
         PowerSystemStateName(SystemState));

    __FdoSetSystemPowerState(Fdo, SystemState);

    status = FdoForwardIrpSynchronously(Fdo, Irp);
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

static FORCEINLINE NTSTATUS
__FdoSetSystemPower(
    IN  PXENFILT_FDO    Fdo,
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
    IN  PXENFILT_FDO    Fdo,
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
    IN  PXENFILT_FDO    Fdo,
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
    IN  PXENFILT_FDO    Fdo,
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

static FORCEINLINE NTSTATUS
__FdoQuerySystemPowerUp(
    IN  PXENFILT_FDO     Fdo,
    IN  PIRP            Irp
    )
{
    PIO_STACK_LOCATION  StackLocation;
    SYSTEM_POWER_STATE  SystemState;
    NTSTATUS            status;

    StackLocation = IoGetCurrentIrpStackLocation(Irp);
    SystemState = StackLocation->Parameters.Power.State.SystemState;

    ASSERT3U(SystemState, <,  __FdoGetSystemPowerState(Fdo));

    status = FdoForwardIrpSynchronously(Fdo, Irp);

    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

static FORCEINLINE NTSTATUS
__FdoQuerySystemPowerDown(
    IN  PXENFILT_FDO    Fdo,
    IN  PIRP            Irp
    )
{
    PIO_STACK_LOCATION  StackLocation;
    SYSTEM_POWER_STATE  SystemState;
    NTSTATUS            status;

    StackLocation = IoGetCurrentIrpStackLocation(Irp);
    SystemState = StackLocation->Parameters.Power.State.SystemState;

    ASSERT3U(SystemState, >,  __FdoGetSystemPowerState(Fdo));

    status = FdoForwardIrpSynchronously(Fdo, Irp);
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

static FORCEINLINE NTSTATUS
__FdoQuerySystemPower(
    IN  PXENFILT_FDO    Fdo,
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
    IN  PXENFILT_THREAD Self,
    IN  PVOID           Context
    )
{
    PXENFILT_FDO        Fdo = Context;
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

        IoReleaseRemoveLock(&Fdo->Dx->RemoveLock, Irp);
    }

    return STATUS_SUCCESS;
}

static NTSTATUS
FdoSystemPower(
    IN  PXENFILT_THREAD Self,
    IN  PVOID           Context
    )
{
    PXENFILT_FDO        Fdo = Context;
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

        IoReleaseRemoveLock(&Fdo->Dx->RemoveLock, Irp);
    }

    return STATUS_SUCCESS;
}

__drv_functionClass(IO_COMPLETION_ROUTINE)
__drv_sameIRQL
static NTSTATUS
__FdoDispatchPower(
    IN  PDEVICE_OBJECT  DeviceObject,
    IN  PIRP            Irp,
    IN  PVOID           Context
    )
{
    PXENFILT_FDO        Fdo = Context;

    UNREFERENCED_PARAMETER(DeviceObject);

    if (Irp->PendingReturned)
        IoMarkIrpPending(Irp);

    IoReleaseRemoveLock(&Fdo->Dx->RemoveLock, Irp);
    return STATUS_SUCCESS;
}

static DECLSPEC_NOINLINE NTSTATUS
FdoDispatchPower(
    IN  PXENFILT_FDO    Fdo,
    IN  PIRP            Irp
    )
{
    PIO_STACK_LOCATION  StackLocation;
    UCHAR               MinorFunction;
    POWER_STATE_TYPE    PowerType;
    NTSTATUS            status;

    status = IoAcquireRemoveLock(&Fdo->Dx->RemoveLock, Irp);
    if (!NT_SUCCESS(status))
        goto fail1;

    StackLocation = IoGetCurrentIrpStackLocation(Irp);
    MinorFunction = StackLocation->MinorFunction;

    if (MinorFunction != IRP_MN_QUERY_POWER &&
        MinorFunction != IRP_MN_SET_POWER) {
        IoCopyCurrentIrpStackLocationToNext(Irp);
        IoSetCompletionRoutine(Irp,
                               __FdoDispatchPower,
                               Fdo,
                               TRUE,
                               TRUE,
                               TRUE);

        status = IoCallDriver(Fdo->LowerDeviceObject, Irp);

        goto done;
    }

    PowerType = StackLocation->Parameters.Power.Type;

    Trace("====> (%02x:%s)\n",
          MinorFunction, 
          PowerMinorFunctionName(MinorFunction)); 

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
        IoCopyCurrentIrpStackLocationToNext(Irp);
        IoSetCompletionRoutine(Irp,
                               __FdoDispatchPower,
                               Fdo,
                               TRUE,
                               TRUE,
                               TRUE);

        status = IoCallDriver(Fdo->LowerDeviceObject, Irp);
        break;
    }

    Trace("<==== (%02x:%s) (%08x)\n",
          MinorFunction, 
          PowerMinorFunctionName(MinorFunction),
          status);

done:
    return status;

fail1:
    Error("fail1 (%08x)\n", status);

    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

__drv_functionClass(IO_COMPLETION_ROUTINE)
__drv_sameIRQL
static NTSTATUS
__FdoDispatchDefault(
    IN  PDEVICE_OBJECT  DeviceObject,
    IN  PIRP            Irp,
    IN  PVOID           Context
    )
{
    PXENFILT_FDO        Fdo = Context;

    UNREFERENCED_PARAMETER(DeviceObject);

    if (Irp->PendingReturned)
        IoMarkIrpPending(Irp);

    IoReleaseRemoveLock(&Fdo->Dx->RemoveLock, Irp);

    return STATUS_SUCCESS;
}

static DECLSPEC_NOINLINE NTSTATUS
FdoDispatchDefault(
    IN  PXENFILT_FDO    Fdo,
    IN  PIRP            Irp
    )
{
    NTSTATUS            status;

    status = IoAcquireRemoveLock(&Fdo->Dx->RemoveLock, Irp);
    if (!NT_SUCCESS(status))
        goto fail1;

    IoCopyCurrentIrpStackLocationToNext(Irp);
    IoSetCompletionRoutine(Irp,
                           __FdoDispatchDefault,
                           Fdo,
                           TRUE,
                           TRUE,
                           TRUE);

    status = IoCallDriver(Fdo->LowerDeviceObject, Irp);

    return status;

fail1:
    Error("fail1 (%08x)\n", status);

    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

NTSTATUS
FdoDispatch(
    IN  PXENFILT_FDO    Fdo,
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

NTSTATUS
FdoCreate(
    IN  PDEVICE_OBJECT  PhysicalDeviceObject,
    IN  PANSI_STRING    Name,
    IN  PANSI_STRING    Type
    )
{
    PDEVICE_OBJECT      LowerDeviceObject;
    ULONG               DeviceType;
    PDEVICE_OBJECT      FilterDeviceObject;
    PXENFILT_DX         Dx;
    PXENFILT_FDO        Fdo;
    NTSTATUS            status;

    LowerDeviceObject = IoGetAttachedDeviceReference(PhysicalDeviceObject);
    DeviceType = LowerDeviceObject->DeviceType;
    ObDereferenceObject(LowerDeviceObject);

#pragma prefast(suppress:28197) // Possibly leaking memory 'FilterDeviceObject'
    status = IoCreateDevice(DriverGetDriverObject(),
                            sizeof (XENFILT_DX),
                            NULL,
                            DeviceType,
                            FILE_DEVICE_SECURE_OPEN,
                            FALSE,
                            &FilterDeviceObject);
    if (!NT_SUCCESS(status))
        goto fail1;

    Dx = (PXENFILT_DX)FilterDeviceObject->DeviceExtension;
    RtlZeroMemory(Dx, sizeof (XENFILT_DX));

    Dx->Type = FUNCTION_DEVICE_OBJECT;
    Dx->DeviceObject = FilterDeviceObject;
    Dx->DevicePnpState = Added;
    Dx->SystemPowerState = PowerSystemShutdown;
    Dx->DevicePowerState = PowerDeviceD3;

    IoInitializeRemoveLock(&Dx->RemoveLock, FDO_TAG, 0, 0);

    Fdo = __FdoAllocate(sizeof (XENFILT_FDO));

    status = STATUS_NO_MEMORY;
    if (Fdo == NULL)
        goto fail2;

    LowerDeviceObject = IoAttachDeviceToDeviceStack(FilterDeviceObject,
                                                    PhysicalDeviceObject);

    status = STATUS_UNSUCCESSFUL;
    if (LowerDeviceObject == NULL)
        goto fail3;

    Fdo->Dx = Dx;
    Fdo->PhysicalDeviceObject = PhysicalDeviceObject;
    Fdo->LowerDeviceObject = LowerDeviceObject;

    status = ThreadCreate(FdoSystemPower, Fdo, &Fdo->SystemPowerThread);
    if (!NT_SUCCESS(status))
        goto fail4;

    status = ThreadCreate(FdoDevicePower, Fdo, &Fdo->DevicePowerThread);
    if (!NT_SUCCESS(status))
        goto fail5;

    __FdoSetName(Fdo, Name);

    status = __FdoSetPrefix(Fdo);
    if (!NT_SUCCESS(status))
        status = __FdoSetWindowsPEPrefix(Fdo);
    if (!NT_SUCCESS(status))
        goto fail6;

    status = __FdoSetEmulatedType(Fdo, Type);
    if (!NT_SUCCESS(status))
        goto fail7;

    status = EmulatedInitialize(&Fdo->EmulatedInterface);
    if (!NT_SUCCESS(status))
        goto fail8;

    status = UnplugInitialize(&Fdo->UnplugInterface);
    if (!NT_SUCCESS(status))
        goto fail9;

    InitializeMutex(&Fdo->Mutex);
    InitializeListHead(&Dx->ListEntry);
    Fdo->References = 1;

    Info("%p (%s)\n",
         FilterDeviceObject,
         __FdoGetName(Fdo));

    Dx->Fdo = Fdo;

#pragma prefast(suppress:28182)  // Dereferencing NULL pointer
    FilterDeviceObject->DeviceType = LowerDeviceObject->DeviceType;
    FilterDeviceObject->Characteristics = LowerDeviceObject->Characteristics;

    FilterDeviceObject->Flags |= LowerDeviceObject->Flags;
    FilterDeviceObject->Flags &= ~DO_DEVICE_INITIALIZING;

    return STATUS_SUCCESS;

fail9:
    Error("fail9\n");

    EmulatedTeardown(&Fdo->EmulatedInterface);

fail8:
    Error("fail8\n");

    Fdo->Type = XENFILT_EMULATED_OBJECT_TYPE_INVALID;

fail7:
    Error("fail7\n");

    RtlZeroMemory(Fdo->Prefix, MAXNAMELEN);

fail6:
    Error("fail6\n");

    ThreadAlert(Fdo->DevicePowerThread);
    ThreadJoin(Fdo->DevicePowerThread);
    Fdo->DevicePowerThread = NULL;

fail5:
    Error("fail5\n");

    ThreadAlert(Fdo->SystemPowerThread);
    ThreadJoin(Fdo->SystemPowerThread);
    Fdo->SystemPowerThread = NULL;

fail4:
    Error("fail4\n");

    Fdo->PhysicalDeviceObject = NULL;
    Fdo->LowerDeviceObject = NULL;
    Fdo->Dx = NULL;

    IoDetachDevice(LowerDeviceObject);

fail3:
    Error("fail3\n");

    ASSERT(IsZeroMemory(Fdo, sizeof (XENFILT_FDO)));
    __FdoFree(Fdo);

fail2:
    Error("fail2\n");

    IoDeleteDevice(FilterDeviceObject);

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

VOID
FdoDestroy(
    IN  PXENFILT_FDO    Fdo
    )
{
    PDEVICE_OBJECT      LowerDeviceObject = Fdo->LowerDeviceObject;
    PXENFILT_DX         Dx = Fdo->Dx;
    PDEVICE_OBJECT      FilterDeviceObject = Dx->DeviceObject;

    ASSERT(IsListEmpty(&Dx->ListEntry));
    ASSERT3U(Fdo->References, ==, 0);
    ASSERT3U(__FdoGetDevicePnpState(Fdo), ==, Deleted);

    Info("%p (%s)\n",
         FilterDeviceObject,
         __FdoGetName(Fdo));

    Dx->Fdo = NULL;

    RtlZeroMemory(&Fdo->Mutex, sizeof (MUTEX));

    UnplugTeardown(&Fdo->UnplugInterface);

    EmulatedTeardown(&Fdo->EmulatedInterface);

    Fdo->Type = XENFILT_EMULATED_OBJECT_TYPE_INVALID;

    RtlZeroMemory(Fdo->Prefix, MAXNAMELEN);

    ThreadAlert(Fdo->DevicePowerThread);
    ThreadJoin(Fdo->DevicePowerThread);
    Fdo->DevicePowerThread = NULL;

    ThreadAlert(Fdo->SystemPowerThread);
    ThreadJoin(Fdo->SystemPowerThread);
    Fdo->SystemPowerThread = NULL;

    Fdo->LowerDeviceObject = NULL;
    Fdo->PhysicalDeviceObject = NULL;
    Fdo->Dx = NULL;

    IoDetachDevice(LowerDeviceObject);

    ASSERT(IsZeroMemory(Fdo, sizeof (XENFILT_FDO)));
    __FdoFree(Fdo);

    IoDeleteDevice(FilterDeviceObject);
}
