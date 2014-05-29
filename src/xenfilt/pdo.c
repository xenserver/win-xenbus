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

#include "emulated.h"
#include "unplug.h"
#include "names.h"
#include "fdo.h"
#include "pdo.h"
#include "thread.h"
#include "driver.h"
#include "dbg_print.h"
#include "assert.h"

#define PDO_TAG 'ODP'

#define MAXNAMELEN  128

struct _XENFILT_PDO {
    PXENFILT_DX                 Dx;
    PDEVICE_OBJECT              LowerDeviceObject;
    PDEVICE_OBJECT              PhysicalDeviceObject;

    PXENFILT_THREAD             SystemPowerThread;
    PIRP                        SystemPowerIrp;
    PXENFILT_THREAD             DevicePowerThread;
    PIRP                        DevicePowerIrp;

    PXENFILT_EMULATED_INTERFACE EmulatedInterface;
    PXENFILT_EMULATED_OBJECT    EmulatedObject;

    PXENFILT_FDO                Fdo;
    BOOLEAN                     Missing;
    const CHAR                  *Reason;
};

static FORCEINLINE PVOID
__PdoAllocate(
    IN  ULONG   Length
    )
{
    return __AllocateNonPagedPoolWithTag(Length, PDO_TAG);
}

static FORCEINLINE VOID
__PdoFree(
    IN  PVOID   Buffer
    )
{
    __FreePoolWithTag(Buffer, PDO_TAG);
}

static FORCEINLINE VOID
__PdoSetDevicePnpState(
    IN  PXENFILT_PDO        Pdo,
    IN  DEVICE_PNP_STATE    State
    )
{
    PXENFILT_DX             Dx = Pdo->Dx;

    // We can never transition out of the deleted state
    ASSERT(Dx->DevicePnpState != Deleted || State == Deleted);

    Dx->PreviousDevicePnpState = Dx->DevicePnpState;
    Dx->DevicePnpState = State;
}

VOID
PdoSetDevicePnpState(
    IN  PXENFILT_PDO        Pdo,
    IN  DEVICE_PNP_STATE    State
    )
{
    __PdoSetDevicePnpState(Pdo, State);
}

static FORCEINLINE VOID
__PdoRestoreDevicePnpState(
    IN  PXENFILT_PDO        Pdo,
    IN  DEVICE_PNP_STATE    State
    )
{
    PXENFILT_DX             Dx = Pdo->Dx;

    if (Dx->DevicePnpState == State)
        Dx->DevicePnpState = Dx->PreviousDevicePnpState;
}

static FORCEINLINE DEVICE_PNP_STATE
__PdoGetDevicePnpState(
    IN  PXENFILT_PDO    Pdo
    )
{
    PXENFILT_DX         Dx = Pdo->Dx;

    return Dx->DevicePnpState;
}

DEVICE_PNP_STATE
PdoGetDevicePnpState(
    IN  PXENFILT_PDO    Pdo
    )
{
    return __PdoGetDevicePnpState(Pdo);
}

static FORCEINLINE VOID
__PdoSetDevicePowerState(
    IN  PXENFILT_PDO        Pdo,
    IN  DEVICE_POWER_STATE  State
    )
{
    PXENFILT_DX             Dx = Pdo->Dx;

    Dx->DevicePowerState = State;
}

static FORCEINLINE DEVICE_POWER_STATE
__PdoGetDevicePowerState(
    IN  PXENFILT_PDO    Pdo
    )
{
    PXENFILT_DX         Dx = Pdo->Dx;

    return Dx->DevicePowerState;
}

static FORCEINLINE VOID
__PdoSetSystemPowerState(
    IN  PXENFILT_PDO        Pdo,
    IN  SYSTEM_POWER_STATE  State
    )
{
    PXENFILT_DX             Dx = Pdo->Dx;

    Dx->SystemPowerState = State;
}

static FORCEINLINE SYSTEM_POWER_STATE
__PdoGetSystemPowerState(
    IN  PXENFILT_PDO    Pdo
    )
{
    PXENFILT_DX         Dx = Pdo->Dx;

    return Dx->SystemPowerState;
}

PDEVICE_OBJECT
PdoGetPhysicalDeviceObject(
    IN  PXENFILT_PDO    Pdo
    )
{
    return Pdo->PhysicalDeviceObject;
}

static FORCEINLINE VOID
__PdoSetMissing(
    IN  PXENFILT_PDO    Pdo,
    IN  const CHAR      *Reason
    )
{
    Pdo->Reason = Reason;
    Pdo->Missing = TRUE;
}

VOID
PdoSetMissing(
    IN  PXENFILT_PDO    Pdo,
    IN  const CHAR      *Reason
    )
{
    __PdoSetMissing(Pdo, Reason);
}

static FORCEINLINE BOOLEAN
__PdoIsMissing(
    IN  PXENFILT_PDO    Pdo
    )
{
    return Pdo->Missing;
}

BOOLEAN
PdoIsMissing(
    IN  PXENFILT_PDO    Pdo
    )
{
    return __PdoIsMissing(Pdo);
}

static FORCEINLINE VOID
__PdoLink(
    IN  PXENFILT_PDO    Pdo,
    IN  PXENFILT_FDO    Fdo
    )
{
    Pdo->Fdo = Fdo;
    FdoAddPhysicalDeviceObject(Fdo, Pdo->Dx->DeviceObject);
}

static FORCEINLINE VOID
__PdoUnlink(
    IN  PXENFILT_PDO    Pdo
    )
{
    PXENFILT_FDO        Fdo = Pdo->Fdo;

    ASSERT(Fdo != NULL);

    FdoRemovePhysicalDeviceObject(Fdo, Pdo->Dx->DeviceObject);

    Pdo->Fdo = NULL;
}

static FORCEINLINE PXENFILT_FDO
__PdoGetFdo(
    IN  PXENFILT_PDO Pdo
    )
{
    return Pdo->Fdo;
}

static FORCEINLINE PXENFILT_EMULATED_INTERFACE
__PdoGetEmulatedInterface(
    IN  PXENFILT_PDO Pdo
    )
{
    return Pdo->EmulatedInterface;
}

PXENFILT_EMULATED_INTERFACE
PdoGetEmulatedInterface(
    IN  PXENFILT_PDO Pdo
    )
{
    return __PdoGetEmulatedInterface(Pdo);
}

static FORCEINLINE PXENFILT_UNPLUG_INTERFACE
__PdoGetUnplugInterface(
    IN  PXENFILT_PDO Pdo
    )
{
    return FdoGetUnplugInterface(__PdoGetFdo(Pdo));
}

PXENFILT_UNPLUG_INTERFACE
PdoGetUnplugInterface(
    IN  PXENFILT_PDO Pdo
    )
{
    return __PdoGetUnplugInterface(Pdo);
}

static FORCEINLINE VOID
__PdoSetName(
    IN  PXENFILT_PDO    Pdo
    )
{
    PXENFILT_DX         Dx = Pdo->Dx;
    const CHAR          *Text;
    NTSTATUS            status;

    Text = EmulatedGetObjectText(Pdo->EmulatedObject);

    status = RtlStringCbPrintfA(Dx->Name,
                                MAX_DEVICE_ID_LEN,
                                "%s",
                                Text);
    ASSERT(NT_SUCCESS(status));
}

static FORCEINLINE PCHAR
__PdoGetName(
    IN  PXENFILT_PDO    Pdo
    )
{
    PXENFILT_DX         Dx = Pdo->Dx;

    return Dx->Name;
}

__drv_functionClass(IO_COMPLETION_ROUTINE)
__drv_sameIRQL
static NTSTATUS
__PdoForwardIrpSynchronously(
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
PdoForwardIrpSynchronously(
    IN  PXENFILT_PDO    Pdo,
    IN  PIRP            Irp
    )
{
    KEVENT              Event;
    NTSTATUS            status;

    ASSERT3U(KeGetCurrentIrql(), ==, PASSIVE_LEVEL);

    KeInitializeEvent(&Event, NotificationEvent, FALSE);

    IoCopyCurrentIrpStackLocationToNext(Irp);
    IoSetCompletionRoutine(Irp,
                           __PdoForwardIrpSynchronously,
                           &Event,
                           TRUE,
                           TRUE,
                           TRUE);

    status = IoCallDriver(Pdo->LowerDeviceObject, Irp);
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
PdoStartDevice(
    IN  PXENFILT_PDO    Pdo,
    IN  PIRP            Irp
    )
{
    POWER_STATE         PowerState;
    NTSTATUS            status;

    status = IoAcquireRemoveLock(&Pdo->Dx->RemoveLock, Irp);
    if (!NT_SUCCESS(status))
        goto fail1;

    status = PdoForwardIrpSynchronously(Pdo, Irp);
    if (!NT_SUCCESS(status))
        goto fail2;

    __PdoSetSystemPowerState(Pdo, PowerSystemWorking);
    __PdoSetDevicePowerState(Pdo, PowerDeviceD0);

    PowerState.DeviceState = PowerDeviceD0;
    PoSetPowerState(Pdo->Dx->DeviceObject,
                    DevicePowerState,
                    PowerState);

    __PdoSetDevicePnpState(Pdo, Started);

    IoReleaseRemoveLock(&Pdo->Dx->RemoveLock, Irp);

    Irp->IoStatus.Status = STATUS_SUCCESS;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return STATUS_SUCCESS;

fail2:
    IoReleaseRemoveLock(&Pdo->Dx->RemoveLock, Irp);

fail1:
    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

__drv_functionClass(IO_COMPLETION_ROUTINE)
__drv_sameIRQL
static NTSTATUS
__PdoQueryStopDevice(
    IN  PDEVICE_OBJECT  DeviceObject,
    IN  PIRP            Irp,
    IN  PVOID           Context
    )
{
    PXENFILT_PDO        Pdo = Context;

    UNREFERENCED_PARAMETER(DeviceObject);

    if (Irp->PendingReturned)
        IoMarkIrpPending(Irp);

    IoReleaseRemoveLock(&Pdo->Dx->RemoveLock, Irp);

    return STATUS_SUCCESS;
}

static DECLSPEC_NOINLINE NTSTATUS
PdoQueryStopDevice(
    IN  PXENFILT_PDO    Pdo,
    IN  PIRP            Irp
    )
{
    NTSTATUS            status;

    status = IoAcquireRemoveLock(&Pdo->Dx->RemoveLock, Irp);
    if (!NT_SUCCESS(status))
        goto fail1;

    __PdoSetDevicePnpState(Pdo, StopPending);
    Irp->IoStatus.Status = STATUS_SUCCESS;

    IoCopyCurrentIrpStackLocationToNext(Irp);
    IoSetCompletionRoutine(Irp,
                           __PdoQueryStopDevice,
                           Pdo,
                           TRUE,
                           TRUE,
                           TRUE);

    status = IoCallDriver(Pdo->LowerDeviceObject, Irp);

    return status;

fail1:
    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

__drv_functionClass(IO_COMPLETION_ROUTINE)
__drv_sameIRQL
static NTSTATUS
__PdoCancelStopDevice(
    IN  PDEVICE_OBJECT  DeviceObject,
    IN  PIRP            Irp,
    IN  PVOID           Context
    )
{
    PXENFILT_PDO        Pdo = Context;

    UNREFERENCED_PARAMETER(DeviceObject);

    if (Irp->PendingReturned)
        IoMarkIrpPending(Irp);

    IoReleaseRemoveLock(&Pdo->Dx->RemoveLock, Irp);

    return STATUS_SUCCESS;
}

static DECLSPEC_NOINLINE NTSTATUS
PdoCancelStopDevice(
    IN  PXENFILT_PDO    Pdo,
    IN  PIRP            Irp
    )
{
    NTSTATUS            status;

    status = IoAcquireRemoveLock(&Pdo->Dx->RemoveLock, Irp);
    if (!NT_SUCCESS(status))
        goto fail1;

    Irp->IoStatus.Status = STATUS_SUCCESS;

    __PdoRestoreDevicePnpState(Pdo, StopPending);

    IoCopyCurrentIrpStackLocationToNext(Irp);
    IoSetCompletionRoutine(Irp,
                           __PdoCancelStopDevice,
                           Pdo,
                           TRUE,
                           TRUE,
                           TRUE);

    status = IoCallDriver(Pdo->LowerDeviceObject, Irp);

    return status;

fail1:
    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

__drv_functionClass(IO_COMPLETION_ROUTINE)
__drv_sameIRQL
static NTSTATUS
__PdoStopDevice(
    IN  PDEVICE_OBJECT  DeviceObject,
    IN  PIRP            Irp,
    IN  PVOID           Context
    )
{
    PXENFILT_PDO        Pdo = Context;

    UNREFERENCED_PARAMETER(DeviceObject);

    if (Irp->PendingReturned)
        IoMarkIrpPending(Irp);

    IoReleaseRemoveLock(&Pdo->Dx->RemoveLock, Irp);

    return STATUS_SUCCESS;
}

static DECLSPEC_NOINLINE NTSTATUS
PdoStopDevice(
    IN  PXENFILT_PDO    Pdo,
    IN  PIRP            Irp
    )
{
    POWER_STATE         PowerState;
    NTSTATUS            status;

    status = IoAcquireRemoveLock(&Pdo->Dx->RemoveLock, Irp);
    if (!NT_SUCCESS(status))
        goto fail1;

    if (__PdoGetDevicePowerState(Pdo) != PowerDeviceD0)
        goto done;

    __PdoSetDevicePowerState(Pdo, PowerDeviceD3);
    __PdoSetSystemPowerState(Pdo, PowerSystemShutdown);

    PowerState.DeviceState = PowerDeviceD3;
    PoSetPowerState(Pdo->Dx->DeviceObject,
                    DevicePowerState,
                    PowerState);

done:
    __PdoSetDevicePnpState(Pdo, Stopped);
    Irp->IoStatus.Status = STATUS_SUCCESS;

    IoCopyCurrentIrpStackLocationToNext(Irp);
    IoSetCompletionRoutine(Irp,
                           __PdoStopDevice,
                           Pdo,
                           TRUE,
                           TRUE,
                           TRUE);

    status = IoCallDriver(Pdo->LowerDeviceObject, Irp);

    return status;

fail1:
    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

__drv_functionClass(IO_COMPLETION_ROUTINE)
__drv_sameIRQL
static NTSTATUS
__PdoQueryRemoveDevice(
    IN  PDEVICE_OBJECT  DeviceObject,
    IN  PIRP            Irp,
    IN  PVOID           Context
    )
{
    PXENFILT_PDO        Pdo = Context;

    UNREFERENCED_PARAMETER(DeviceObject);

    if (Irp->PendingReturned)
        IoMarkIrpPending(Irp);

    IoReleaseRemoveLock(&Pdo->Dx->RemoveLock, Irp);

    return STATUS_SUCCESS;
}

static DECLSPEC_NOINLINE NTSTATUS
PdoQueryRemoveDevice(
    IN  PXENFILT_PDO    Pdo,
    IN  PIRP            Irp
    )
{
    NTSTATUS            status;

    status = IoAcquireRemoveLock(&Pdo->Dx->RemoveLock, Irp);
    if (!NT_SUCCESS(status))
        goto fail1;

    __PdoSetDevicePnpState(Pdo, RemovePending);
    Irp->IoStatus.Status = STATUS_SUCCESS;

    IoCopyCurrentIrpStackLocationToNext(Irp);
    IoSetCompletionRoutine(Irp,
                           __PdoQueryRemoveDevice,
                           Pdo,
                           TRUE,
                           TRUE,
                           TRUE);

    status = IoCallDriver(Pdo->LowerDeviceObject, Irp);

    return status;

fail1:
    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

__drv_functionClass(IO_COMPLETION_ROUTINE)
__drv_sameIRQL
static NTSTATUS
__PdoCancelRemoveDevice(
    IN  PDEVICE_OBJECT  DeviceObject,
    IN  PIRP            Irp,
    IN  PVOID           Context
    )
{
    PXENFILT_PDO        Pdo = Context;

    UNREFERENCED_PARAMETER(DeviceObject);

    if (Irp->PendingReturned)
        IoMarkIrpPending(Irp);

    IoReleaseRemoveLock(&Pdo->Dx->RemoveLock, Irp);

    return STATUS_SUCCESS;
}

static DECLSPEC_NOINLINE NTSTATUS
PdoCancelRemoveDevice(
    IN  PXENFILT_PDO    Pdo,
    IN  PIRP            Irp
    )
{
    NTSTATUS            status;

    status = IoAcquireRemoveLock(&Pdo->Dx->RemoveLock, Irp);
    if (!NT_SUCCESS(status))
        goto fail1;

    __PdoRestoreDevicePnpState(Pdo, RemovePending);
    Irp->IoStatus.Status = STATUS_SUCCESS;

    IoCopyCurrentIrpStackLocationToNext(Irp);
    IoSetCompletionRoutine(Irp,
                           __PdoCancelRemoveDevice,
                           Pdo,
                           TRUE,
                           TRUE,
                           TRUE);

    status = IoCallDriver(Pdo->LowerDeviceObject, Irp);

    return status;

fail1:
    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

__drv_functionClass(IO_COMPLETION_ROUTINE)
__drv_sameIRQL
static NTSTATUS
__PdoSurpriseRemoval(
    IN  PDEVICE_OBJECT  DeviceObject,
    IN  PIRP            Irp,
    IN  PVOID           Context
    )
{
    PXENFILT_PDO        Pdo = Context;

    UNREFERENCED_PARAMETER(DeviceObject);

    if (Irp->PendingReturned)
        IoMarkIrpPending(Irp);

    IoReleaseRemoveLock(&Pdo->Dx->RemoveLock, Irp);

    return STATUS_SUCCESS;
}

static DECLSPEC_NOINLINE NTSTATUS
PdoSurpriseRemoval(
    IN  PXENFILT_PDO    Pdo,
    IN  PIRP            Irp
    )
{
    NTSTATUS            status;

    status = IoAcquireRemoveLock(&Pdo->Dx->RemoveLock, Irp);
    if (!NT_SUCCESS(status))
        goto fail1;

    __PdoSetDevicePnpState(Pdo, SurpriseRemovePending);
    Irp->IoStatus.Status = STATUS_SUCCESS;

    IoCopyCurrentIrpStackLocationToNext(Irp);
    IoSetCompletionRoutine(Irp,
                           __PdoSurpriseRemoval,
                           Pdo,
                           TRUE,
                           TRUE,
                           TRUE);

    status = IoCallDriver(Pdo->LowerDeviceObject, Irp);

    return status;

fail1:
    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

static DECLSPEC_NOINLINE NTSTATUS
PdoRemoveDevice(
    IN  PXENFILT_PDO    Pdo,
    IN  PIRP            Irp
    )
{
    PXENFILT_FDO        Fdo = __PdoGetFdo(Pdo);
    POWER_STATE         PowerState;
    NTSTATUS            status;

    status = IoAcquireRemoveLock(&Pdo->Dx->RemoveLock, Irp);
    if (!NT_SUCCESS(status))
        goto fail1;

    if (__PdoGetDevicePowerState(Pdo) != PowerDeviceD0)
        goto done;

    __PdoSetDevicePowerState(Pdo, PowerDeviceD3);
    __PdoSetSystemPowerState(Pdo, PowerSystemShutdown);

    PowerState.DeviceState = PowerDeviceD3;
    PoSetPowerState(Pdo->Dx->DeviceObject,
                    DevicePowerState,
                    PowerState);

done:
    if (__PdoIsMissing(Pdo)) {
        __PdoSetDevicePnpState(Pdo, Deleted);
        IoReleaseRemoveLockAndWait(&Pdo->Dx->RemoveLock, Irp);
    } else {
        __PdoSetDevicePnpState(Pdo, Enumerated);
        IoReleaseRemoveLock(&Pdo->Dx->RemoveLock, Irp);
    }

    status = PdoForwardIrpSynchronously(Pdo, Irp);
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    if (__PdoIsMissing(Pdo)) {
        FdoAcquireMutex(Fdo);
        PdoDestroy(Pdo);
        FdoReleaseMutex(Fdo);
    }

    return status;

fail1:
    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

__drv_functionClass(IO_COMPLETION_ROUTINE)
__drv_sameIRQL
static NTSTATUS
__PdoQueryInterface(
    IN  PDEVICE_OBJECT  DeviceObject,
    IN  PIRP            Irp,
    IN  PVOID           Context
    )
{
    PXENFILT_PDO        Pdo = Context;

    UNREFERENCED_PARAMETER(DeviceObject);

    if (Irp->PendingReturned)
        IoMarkIrpPending(Irp);

    IoReleaseRemoveLock(&Pdo->Dx->RemoveLock, Irp);

    return STATUS_SUCCESS;
}

#define GET(_Interface) __PdoGet ## _Interface ## Interface

#define DEFINE_HANDLER(_Version, _Interface)                        \
static NTSTATUS                                                     \
PdoQuery ## _Interface ## Interface(                                \
    IN  PXENFILT_PDO            Pdo,                                \
    IN  PIRP                    Irp                                 \
    )                                                               \
{                                                                   \
    PIO_STACK_LOCATION          StackLocation;                      \
    USHORT                      Size;                               \
    USHORT                      Version;                            \
    PINTERFACE                  Interface;                          \
    NTSTATUS                    status;                             \
                                                                    \
    status = Irp->IoStatus.Status;                                  \
                                                                    \
    StackLocation = IoGetCurrentIrpStackLocation(Irp);              \
    Size = StackLocation->Parameters.QueryInterface.Size;           \
    Version = StackLocation->Parameters.QueryInterface.Version;     \
    Interface = StackLocation->Parameters.QueryInterface.Interface; \
                                                                    \
    if (Version != (_Version))                                      \
        goto done;                                                  \
                                                                    \
    status = STATUS_BUFFER_TOO_SMALL;                               \
    if (Size < sizeof (INTERFACE))                                  \
        goto done;                                                  \
                                                                    \
    Interface->Size = sizeof (INTERFACE);                           \
    Interface->Version = (_Version);                                \
    Interface->Context = GET(_Interface)(Pdo);                      \
    Interface->InterfaceReference = NULL;                           \
    Interface->InterfaceDereference = NULL;                         \
                                                                    \
    Irp->IoStatus.Information = 0;                                  \
    status = STATUS_SUCCESS;                                        \
                                                                    \
done:                                                               \
    return status;                                                  \
}                                                                   \

DEFINE_HANDLER(EMULATED_INTERFACE_VERSION, Emulated)
DEFINE_HANDLER(UNPLUG_INTERFACE_VERSION, Unplug)

struct _INTERFACE_ENTRY {
    const GUID  *Guid;
    const CHAR  *Name;
    NTSTATUS    (*Handler)(PXENFILT_PDO, PIRP);
};

#define HANDLER(_Interface) PdoQuery ## _Interface ## Interface

#define DEFINE_ENTRY(_Guid, _Interface)   \
    { &GUID_ ## _Guid, #_Guid, HANDLER(_Interface) }

struct _INTERFACE_ENTRY PdoInterfaceTable[] = {
    DEFINE_ENTRY(EMULATED_INTERFACE, Emulated),
    DEFINE_ENTRY(UNPLUG_INTERFACE, Unplug),
    { NULL, NULL, NULL }
};

static DECLSPEC_NOINLINE NTSTATUS
PdoQueryInterface(
    IN  PXENFILT_PDO        Pdo,
    IN  PIRP                Irp
    )
{
    PIO_STACK_LOCATION      StackLocation;
    const GUID              *InterfaceType;
    struct _INTERFACE_ENTRY *Entry;
    NTSTATUS                status;

    status = IoAcquireRemoveLock(&Pdo->Dx->RemoveLock, Irp);
    if (!NT_SUCCESS(status))
        goto fail1;

    if (Irp->IoStatus.Status != STATUS_NOT_SUPPORTED)
        goto done;

    StackLocation = IoGetCurrentIrpStackLocation(Irp);
    InterfaceType = StackLocation->Parameters.QueryInterface.InterfaceType;

    for (Entry = PdoInterfaceTable; Entry->Guid != NULL; Entry++) {
        if (IsEqualGUID(InterfaceType, Entry->Guid)) {
            Trace("%s: %s\n",
                  __PdoGetName(Pdo),
                  Entry->Name);
            Irp->IoStatus.Status = Entry->Handler(Pdo, Irp);
            goto done;
        }
    }

done:
    IoCopyCurrentIrpStackLocationToNext(Irp);
    IoSetCompletionRoutine(Irp,
                            __PdoQueryInterface,
                            Pdo,
                            TRUE,
                            TRUE,
                            TRUE);

    status = IoCallDriver(Pdo->LowerDeviceObject, Irp);

    return status;

fail1:
    Error("fail1 (%08x)\n", status);

    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

static DECLSPEC_NOINLINE NTSTATUS
PdoEject(
    IN  PXENFILT_PDO    Pdo,
    IN  PIRP            Irp
    )
{
    PXENFILT_FDO        Fdo = __PdoGetFdo(Pdo);
    NTSTATUS            status;

    __PdoSetMissing(Pdo, "Ejected");
    __PdoSetDevicePnpState(Pdo, Deleted);

    status = PdoForwardIrpSynchronously(Pdo, Irp);
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    FdoAcquireMutex(Fdo);
    PdoDestroy(Pdo);
    FdoReleaseMutex(Fdo);

    return status;
}

__drv_functionClass(IO_COMPLETION_ROUTINE)
__drv_sameIRQL
static NTSTATUS
__PdoDispatchPnp(
    IN  PDEVICE_OBJECT  DeviceObject,
    IN  PIRP            Irp,
    IN  PVOID           Context
    )
{
    PXENFILT_PDO        Pdo = Context;

    UNREFERENCED_PARAMETER(DeviceObject);

    if (Irp->PendingReturned)
        IoMarkIrpPending(Irp);

    IoReleaseRemoveLock(&Pdo->Dx->RemoveLock, Irp);
    return STATUS_SUCCESS;
}

static DECLSPEC_NOINLINE NTSTATUS
PdoDispatchPnp(
    IN  PXENFILT_PDO    Pdo,
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
        status = PdoStartDevice(Pdo, Irp);
        break;

    case IRP_MN_QUERY_STOP_DEVICE:
        status = PdoQueryStopDevice(Pdo, Irp);
        break;

    case IRP_MN_CANCEL_STOP_DEVICE:
        status = PdoCancelStopDevice(Pdo, Irp);
        break;

    case IRP_MN_STOP_DEVICE:
        status = PdoStopDevice(Pdo, Irp);
        break;

    case IRP_MN_QUERY_REMOVE_DEVICE:
        status = PdoQueryRemoveDevice(Pdo, Irp);
        break;

    case IRP_MN_SURPRISE_REMOVAL:
        status = PdoSurpriseRemoval(Pdo, Irp);
        break;

    case IRP_MN_REMOVE_DEVICE:
        status = PdoRemoveDevice(Pdo, Irp);
        break;

    case IRP_MN_CANCEL_REMOVE_DEVICE:
        status = PdoCancelRemoveDevice(Pdo, Irp);
        break;

    case IRP_MN_QUERY_INTERFACE:
        status = PdoQueryInterface(Pdo, Irp);
        break;

    case IRP_MN_EJECT:
        status = PdoEject(Pdo, Irp);
        break;

    default:
        status = IoAcquireRemoveLock(&Pdo->Dx->RemoveLock, Irp);
        if (!NT_SUCCESS(status))
            goto fail1;

        IoCopyCurrentIrpStackLocationToNext(Irp);
        IoSetCompletionRoutine(Irp,
                               __PdoDispatchPnp,
                               Pdo,
                               TRUE,
                               TRUE,
                               TRUE);

        status = IoCallDriver(Pdo->LowerDeviceObject, Irp);
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
__PdoSetDevicePowerUp(
    IN  PXENFILT_PDO    Pdo,
    IN  PIRP            Irp
    )
{
    PIO_STACK_LOCATION  StackLocation;
    DEVICE_POWER_STATE  DeviceState;
    NTSTATUS            status;

    StackLocation = IoGetCurrentIrpStackLocation(Irp);
    DeviceState = StackLocation->Parameters.Power.State.DeviceState;

    ASSERT3U(DeviceState, <,  __PdoGetDevicePowerState(Pdo));

    status = PdoForwardIrpSynchronously(Pdo, Irp);
    if (!NT_SUCCESS(status))
        goto done;

    Info("%s: %s -> %s\n",
         __PdoGetName(Pdo),
         PowerDeviceStateName(__PdoGetDevicePowerState(Pdo)),
         PowerDeviceStateName(DeviceState));

    __PdoSetDevicePowerState(Pdo, DeviceState);

done:
    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

static FORCEINLINE NTSTATUS
__PdoSetDevicePowerDown(
    IN  PXENFILT_PDO    Pdo,
    IN  PIRP            Irp
    )
{
    PIO_STACK_LOCATION  StackLocation;
    DEVICE_POWER_STATE  DeviceState;
    NTSTATUS            status;

    StackLocation = IoGetCurrentIrpStackLocation(Irp);
    DeviceState = StackLocation->Parameters.Power.State.DeviceState;

    ASSERT3U(DeviceState, >,  __PdoGetDevicePowerState(Pdo));

    Info("%s: %s -> %s\n",
         __PdoGetName(Pdo),
         PowerDeviceStateName(__PdoGetDevicePowerState(Pdo)),
         PowerDeviceStateName(DeviceState));

    __PdoSetDevicePowerState(Pdo, DeviceState);

    status = PdoForwardIrpSynchronously(Pdo, Irp);
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

static FORCEINLINE NTSTATUS
__PdoSetDevicePower(
    IN  PXENFILT_PDO    Pdo,
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

    if (DeviceState == __PdoGetDevicePowerState(Pdo)) {
        status = PdoForwardIrpSynchronously(Pdo, Irp);
        IoCompleteRequest(Irp, IO_NO_INCREMENT);

        goto done;
    }

    status = (DeviceState < __PdoGetDevicePowerState(Pdo)) ?
             __PdoSetDevicePowerUp(Pdo, Irp) :
             __PdoSetDevicePowerDown(Pdo, Irp);

done:
    Trace("<==== (%s:%s)(%08x)\n",
          PowerDeviceStateName(DeviceState), 
          PowerActionName(PowerAction),
          status);
    return status;
}

static FORCEINLINE NTSTATUS
__PdoSetSystemPowerUp(
    IN  PXENFILT_PDO    Pdo,
    IN  PIRP            Irp
    )
{
    PIO_STACK_LOCATION  StackLocation;
    SYSTEM_POWER_STATE  SystemState;
    NTSTATUS            status;

    StackLocation = IoGetCurrentIrpStackLocation(Irp);
    SystemState = StackLocation->Parameters.Power.State.SystemState;

    ASSERT3U(SystemState, <,  __PdoGetSystemPowerState(Pdo));

    status = PdoForwardIrpSynchronously(Pdo, Irp);
    if (!NT_SUCCESS(status))
        goto done;

    Info("%s: %s -> %s\n",
         __PdoGetName(Pdo),
         PowerSystemStateName(__PdoGetSystemPowerState(Pdo)),
         PowerSystemStateName(SystemState));

    __PdoSetSystemPowerState(Pdo, SystemState);

done:
    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

static FORCEINLINE NTSTATUS
__PdoSetSystemPowerDown(
    IN  PXENFILT_PDO     Pdo,
    IN  PIRP            Irp
    )
{
    PIO_STACK_LOCATION  StackLocation;
    SYSTEM_POWER_STATE  SystemState;
    NTSTATUS            status;

    StackLocation = IoGetCurrentIrpStackLocation(Irp);
    SystemState = StackLocation->Parameters.Power.State.SystemState;

    ASSERT3U(SystemState, >,  __PdoGetSystemPowerState(Pdo));

    Info("%s: %s -> %s\n",
         __PdoGetName(Pdo),
         PowerSystemStateName(__PdoGetSystemPowerState(Pdo)),
         PowerSystemStateName(SystemState));

    __PdoSetSystemPowerState(Pdo, SystemState);

    status = PdoForwardIrpSynchronously(Pdo, Irp);
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

static FORCEINLINE NTSTATUS
__PdoSetSystemPower(
    IN  PXENFILT_PDO    Pdo,
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

    if (SystemState == __PdoGetSystemPowerState(Pdo)) {
        status = PdoForwardIrpSynchronously(Pdo, Irp);
        IoCompleteRequest(Irp, IO_NO_INCREMENT);

        goto done;
    }

    status = (SystemState < __PdoGetSystemPowerState(Pdo)) ?
             __PdoSetSystemPowerUp(Pdo, Irp) :
             __PdoSetSystemPowerDown(Pdo, Irp);

done:
    Trace("<==== (%s:%s)(%08x)\n",
          PowerSystemStateName(SystemState), 
          PowerActionName(PowerAction),
          status);
    return status;
}

static FORCEINLINE NTSTATUS
__PdoQueryDevicePowerUp(
    IN  PXENFILT_PDO    Pdo,
    IN  PIRP            Irp
    )
{
    PIO_STACK_LOCATION  StackLocation;
    DEVICE_POWER_STATE  DeviceState;
    NTSTATUS            status;

    StackLocation = IoGetCurrentIrpStackLocation(Irp);
    DeviceState = StackLocation->Parameters.Power.State.DeviceState;

    ASSERT3U(DeviceState, <,  __PdoGetDevicePowerState(Pdo));

    status = PdoForwardIrpSynchronously(Pdo, Irp);

    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

static FORCEINLINE NTSTATUS
__PdoQueryDevicePowerDown(
    IN  PXENFILT_PDO    Pdo,
    IN  PIRP            Irp
    )
{
    PIO_STACK_LOCATION  StackLocation;
    DEVICE_POWER_STATE  DeviceState;
    NTSTATUS            status;

    StackLocation = IoGetCurrentIrpStackLocation(Irp);
    DeviceState = StackLocation->Parameters.Power.State.DeviceState;

    ASSERT3U(DeviceState, >,  __PdoGetDevicePowerState(Pdo));

    status = PdoForwardIrpSynchronously(Pdo, Irp);
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

static FORCEINLINE NTSTATUS
__PdoQueryDevicePower(
    IN  PXENFILT_PDO    Pdo,
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

    if (DeviceState == __PdoGetDevicePowerState(Pdo)) {
        status = PdoForwardIrpSynchronously(Pdo, Irp);
        IoCompleteRequest(Irp, IO_NO_INCREMENT);

        goto done;
    }

    status = (DeviceState < __PdoGetDevicePowerState(Pdo)) ?
             __PdoQueryDevicePowerUp(Pdo, Irp) :
             __PdoQueryDevicePowerDown(Pdo, Irp);

done:
    Trace("<==== (%s:%s)(%08x)\n",
          PowerDeviceStateName(DeviceState), 
          PowerActionName(PowerAction),
          status);
    return status;
}

static FORCEINLINE NTSTATUS
__PdoQuerySystemPowerUp(
    IN  PXENFILT_PDO     Pdo,
    IN  PIRP            Irp
    )
{
    PIO_STACK_LOCATION  StackLocation;
    SYSTEM_POWER_STATE  SystemState;
    NTSTATUS            status;

    StackLocation = IoGetCurrentIrpStackLocation(Irp);
    SystemState = StackLocation->Parameters.Power.State.SystemState;

    ASSERT3U(SystemState, <,  __PdoGetSystemPowerState(Pdo));

    status = PdoForwardIrpSynchronously(Pdo, Irp);

    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

static FORCEINLINE NTSTATUS
__PdoQuerySystemPowerDown(
    IN  PXENFILT_PDO    Pdo,
    IN  PIRP            Irp
    )
{
    PIO_STACK_LOCATION  StackLocation;
    SYSTEM_POWER_STATE  SystemState;
    NTSTATUS            status;

    StackLocation = IoGetCurrentIrpStackLocation(Irp);
    SystemState = StackLocation->Parameters.Power.State.SystemState;

    ASSERT3U(SystemState, >,  __PdoGetSystemPowerState(Pdo));

    status = PdoForwardIrpSynchronously(Pdo, Irp);
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

static FORCEINLINE NTSTATUS
__PdoQuerySystemPower(
    IN  PXENFILT_PDO    Pdo,
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

    if (SystemState == __PdoGetSystemPowerState(Pdo)) {
        status = PdoForwardIrpSynchronously(Pdo, Irp);
        IoCompleteRequest(Irp, IO_NO_INCREMENT);

        goto done;
    }

    status = (SystemState < __PdoGetSystemPowerState(Pdo)) ?
             __PdoQuerySystemPowerUp(Pdo, Irp) :
             __PdoQuerySystemPowerDown(Pdo, Irp);

done:
    Trace("<==== (%s:%s)(%08x)\n",
          PowerSystemStateName(SystemState), 
          PowerActionName(PowerAction),
          status);

    return status;
}

static NTSTATUS
PdoDevicePower(
    IN  PXENFILT_THREAD Self,
    IN  PVOID           Context
    )
{
    PXENFILT_PDO        Pdo = Context;
    PKEVENT             Event;

    Event = ThreadGetEvent(Self);

    for (;;) {
        PIRP                Irp;
        PIO_STACK_LOCATION  StackLocation;
        UCHAR               MinorFunction;

        if (Pdo->DevicePowerIrp == NULL) {
            (VOID) KeWaitForSingleObject(Event,
                                         Executive,
                                         KernelMode,
                                         FALSE,
                                         NULL);
            KeClearEvent(Event);
        }

        if (ThreadIsAlerted(Self))
            break;

        Irp = Pdo->DevicePowerIrp;

        if (Irp == NULL)
            continue;

        Pdo->DevicePowerIrp = NULL;
        KeMemoryBarrier();

        StackLocation = IoGetCurrentIrpStackLocation(Irp);
        MinorFunction = StackLocation->MinorFunction;

        switch (StackLocation->MinorFunction) {
        case IRP_MN_SET_POWER:
            (VOID) __PdoSetDevicePower(Pdo, Irp);
            break;

        case IRP_MN_QUERY_POWER:
            (VOID) __PdoQueryDevicePower(Pdo, Irp);
            break;

        default:
            ASSERT(FALSE);
            break;
        }

        IoReleaseRemoveLock(&Pdo->Dx->RemoveLock, Irp);
    }

    return STATUS_SUCCESS;
}

static NTSTATUS
PdoSystemPower(
    IN  PXENFILT_THREAD Self,
    IN  PVOID           Context
    )
{
    PXENFILT_PDO        Pdo = Context;
    PKEVENT             Event;

    Event = ThreadGetEvent(Self);

    for (;;) {
        PIRP                Irp;
        PIO_STACK_LOCATION  StackLocation;
        UCHAR               MinorFunction;

        if (Pdo->SystemPowerIrp == NULL) {
            (VOID) KeWaitForSingleObject(Event,
                                         Executive,
                                         KernelMode,
                                         FALSE,
                                         NULL);
            KeClearEvent(Event);
        }

        if (ThreadIsAlerted(Self))
            break;

        Irp = Pdo->SystemPowerIrp;

        if (Irp == NULL)
            continue;

        Pdo->SystemPowerIrp = NULL;
        KeMemoryBarrier();

        StackLocation = IoGetCurrentIrpStackLocation(Irp);
        MinorFunction = StackLocation->MinorFunction;

        switch (StackLocation->MinorFunction) {
        case IRP_MN_SET_POWER:
            (VOID) __PdoSetSystemPower(Pdo, Irp);
            break;

        case IRP_MN_QUERY_POWER:
            (VOID) __PdoQuerySystemPower(Pdo, Irp);
            break;

        default:
            ASSERT(FALSE);
            break;
        }

        IoReleaseRemoveLock(&Pdo->Dx->RemoveLock, Irp);
    }

    return STATUS_SUCCESS;
}

__drv_functionClass(IO_COMPLETION_ROUTINE)
__drv_sameIRQL
static NTSTATUS
__PdoDispatchPower(
    IN  PDEVICE_OBJECT  DeviceObject,
    IN  PIRP            Irp,
    IN  PVOID           Context
    )
{
    PXENFILT_PDO        Pdo = Context;

    UNREFERENCED_PARAMETER(DeviceObject);

    if (Irp->PendingReturned)
        IoMarkIrpPending(Irp);

    IoReleaseRemoveLock(&Pdo->Dx->RemoveLock, Irp);
    return STATUS_SUCCESS;
}

static DECLSPEC_NOINLINE NTSTATUS
PdoDispatchPower(
    IN  PXENFILT_PDO    Pdo,
    IN  PIRP            Irp
    )
{
    PIO_STACK_LOCATION  StackLocation;
    UCHAR               MinorFunction;
    POWER_STATE_TYPE    PowerType;
    NTSTATUS            status;

    status = IoAcquireRemoveLock(&Pdo->Dx->RemoveLock, Irp);
    if (!NT_SUCCESS(status))
        goto fail1;

    StackLocation = IoGetCurrentIrpStackLocation(Irp);
    MinorFunction = StackLocation->MinorFunction;

    if (MinorFunction != IRP_MN_QUERY_POWER &&
        MinorFunction != IRP_MN_SET_POWER) {
        IoCopyCurrentIrpStackLocationToNext(Irp);
        IoSetCompletionRoutine(Irp,
                               __PdoDispatchPower,
                               Pdo,
                               TRUE,
                               TRUE,
                               TRUE);

        status = IoCallDriver(Pdo->LowerDeviceObject, Irp);

        goto done;
    }

    PowerType = StackLocation->Parameters.Power.Type;

    Trace("====> (%02x:%s)\n",
          MinorFunction, 
          PowerMinorFunctionName(MinorFunction)); 

    switch (PowerType) {
    case DevicePowerState:
        IoMarkIrpPending(Irp);

        ASSERT3P(Pdo->DevicePowerIrp, ==, NULL);
        Pdo->DevicePowerIrp = Irp;
        KeMemoryBarrier();

        ThreadWake(Pdo->DevicePowerThread);

        status = STATUS_PENDING;
        break;

    case SystemPowerState:
        IoMarkIrpPending(Irp);

        ASSERT3P(Pdo->SystemPowerIrp, ==, NULL);
        Pdo->SystemPowerIrp = Irp;
        KeMemoryBarrier();

        ThreadWake(Pdo->SystemPowerThread);

        status = STATUS_PENDING;
        break;

    default:
        IoCopyCurrentIrpStackLocationToNext(Irp);
        IoSetCompletionRoutine(Irp,
                               __PdoDispatchPower,
                               Pdo,
                               TRUE,
                               TRUE,
                               TRUE);

        status = IoCallDriver(Pdo->LowerDeviceObject, Irp);
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
__PdoDispatchDefault(
    IN  PDEVICE_OBJECT  DeviceObject,
    IN  PIRP            Irp,
    IN  PVOID           Context
    )
{
    PXENFILT_PDO        Pdo = Context;

    UNREFERENCED_PARAMETER(DeviceObject);

    if (Irp->PendingReturned)
        IoMarkIrpPending(Irp);

    IoReleaseRemoveLock(&Pdo->Dx->RemoveLock, Irp);

    return STATUS_SUCCESS;
}

static DECLSPEC_NOINLINE NTSTATUS
PdoDispatchDefault(
    IN  PXENFILT_PDO    Pdo,
    IN  PIRP            Irp
    )
{
    NTSTATUS            status;

    status = IoAcquireRemoveLock(&Pdo->Dx->RemoveLock, Irp);
    if (!NT_SUCCESS(status))
        goto fail1;

    IoCopyCurrentIrpStackLocationToNext(Irp);
    IoSetCompletionRoutine(Irp,
                           __PdoDispatchDefault,
                           Pdo,
                           TRUE,
                           TRUE,
                           TRUE);

    status = IoCallDriver(Pdo->LowerDeviceObject, Irp);

    return status;

fail1:
    Error("fail1 (%08x)\n", status);

    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

NTSTATUS
PdoDispatch(
    IN  PXENFILT_PDO    Pdo,
    IN  PIRP            Irp
    )
{
    PIO_STACK_LOCATION  StackLocation;
    NTSTATUS            status;

    StackLocation = IoGetCurrentIrpStackLocation(Irp);

    switch (StackLocation->MajorFunction) {
    case IRP_MJ_PNP:
        status = PdoDispatchPnp(Pdo, Irp);
        break;

    case IRP_MJ_POWER:
        status = PdoDispatchPower(Pdo, Irp);
        break;

    default:
        status = PdoDispatchDefault(Pdo, Irp);
        break;
    }

    return status;
}

NTSTATUS
PdoCreate(
    PXENFILT_FDO                    Fdo,
    PDEVICE_OBJECT                  PhysicalDeviceObject,
    XENFILT_EMULATED_OBJECT_TYPE    Type
    )
{
    PDEVICE_OBJECT                  LowerDeviceObject;
    ULONG                           DeviceType;
    PDEVICE_OBJECT                  FilterDeviceObject;
    PXENFILT_DX                     Dx;
    PXENFILT_PDO                    Pdo;
    NTSTATUS                        status;

    LowerDeviceObject = IoGetAttachedDeviceReference(PhysicalDeviceObject);
    DeviceType = LowerDeviceObject->DeviceType;
    ObDereferenceObject(LowerDeviceObject);

#pragma prefast(suppress:28197) // Possibly leaking memory 'PhysicalDeviceObject'
    status = IoCreateDevice(DriverGetDriverObject(),
                            sizeof(XENFILT_DX),
                            NULL,
                            DeviceType,
                            FILE_DEVICE_SECURE_OPEN,
                            FALSE,
                            &FilterDeviceObject);
    if (!NT_SUCCESS(status))
        goto fail1;

    Dx = (PXENFILT_DX)FilterDeviceObject->DeviceExtension;
    RtlZeroMemory(Dx, sizeof (XENFILT_DX));

    Dx->Type = PHYSICAL_DEVICE_OBJECT;
    Dx->DeviceObject = FilterDeviceObject;
    Dx->DevicePnpState = Present;
    Dx->SystemPowerState = PowerSystemShutdown;
    Dx->DevicePowerState = PowerDeviceD3;

    IoInitializeRemoveLock(&Dx->RemoveLock, PDO_TAG, 0, 0);

    Pdo = __PdoAllocate(sizeof (XENFILT_PDO));

    status = STATUS_NO_MEMORY;
    if (Pdo == NULL)
        goto fail2;

    LowerDeviceObject = IoAttachDeviceToDeviceStack(FilterDeviceObject,
                                                    PhysicalDeviceObject);

    status = STATUS_UNSUCCESSFUL;
    if (LowerDeviceObject == NULL)
        goto fail3;

    Pdo->Dx = Dx;
    Pdo->PhysicalDeviceObject = PhysicalDeviceObject;
    Pdo->LowerDeviceObject = LowerDeviceObject;

    status = ThreadCreate(PdoSystemPower, Pdo, &Pdo->SystemPowerThread);
    if (!NT_SUCCESS(status))
        goto fail4;

    status = ThreadCreate(PdoDevicePower, Pdo, &Pdo->DevicePowerThread);
    if (!NT_SUCCESS(status))
        goto fail5;

    Pdo->EmulatedInterface = FdoGetEmulatedInterface(Fdo);

    status = EmulatedAddObject(__PdoGetEmulatedInterface(Pdo),
                               Type,
                               FdoGetPrefix(Fdo),
                               Pdo->LowerDeviceObject,
                               &Pdo->EmulatedObject);
    if (!NT_SUCCESS(status))
        goto fail6;

    __PdoSetName(Pdo);

    Info("%p (%s)\n",
          FilterDeviceObject,
          __PdoGetName(Pdo));

    Dx->Pdo = Pdo;

#pragma prefast(suppress:28182) // Dereferencing NULL pointer
    FilterDeviceObject->DeviceType = LowerDeviceObject->DeviceType;
    FilterDeviceObject->Characteristics = LowerDeviceObject->Characteristics;

    FilterDeviceObject->Flags |= LowerDeviceObject->Flags;
    FilterDeviceObject->Flags &= ~DO_DEVICE_INITIALIZING;

    __PdoLink(Pdo, Fdo);

    return STATUS_SUCCESS;

fail6:
    Error("fail6\n");

    ThreadAlert(Pdo->DevicePowerThread);
    ThreadJoin(Pdo->DevicePowerThread);
    Pdo->DevicePowerThread = NULL;

fail5:
    Error("fail5\n");

    ThreadAlert(Pdo->SystemPowerThread);
    ThreadJoin(Pdo->SystemPowerThread);
    Pdo->SystemPowerThread = NULL;

fail4:
    Error("fail4\n");

    Pdo->PhysicalDeviceObject = NULL;
    Pdo->LowerDeviceObject = NULL;
    Pdo->Dx = NULL;

    IoDetachDevice(LowerDeviceObject);

fail3:
    Error("fail3\n");

    ASSERT(IsZeroMemory(Pdo, sizeof (XENFILT_PDO)));
    __PdoFree(Pdo);

fail2:
    Error("fail2\n");

    IoDeleteDevice(FilterDeviceObject);

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

VOID
PdoDestroy(
    IN  PXENFILT_PDO    Pdo
    )
{
    PDEVICE_OBJECT      LowerDeviceObject = Pdo->LowerDeviceObject;
    PXENFILT_DX         Dx = Pdo->Dx;
    PDEVICE_OBJECT      FilterDeviceObject = Dx->DeviceObject;

    ASSERT3U(__PdoGetDevicePnpState(Pdo), ==, Deleted);

    ASSERT(__PdoIsMissing(Pdo));
    Pdo->Missing = FALSE;

    __PdoUnlink(Pdo);

    Info("%p (%s) (%s)\n",
         FilterDeviceObject,
         __PdoGetName(Pdo),
         Pdo->Reason);
    Pdo->Reason = NULL;

    Dx->Pdo = NULL;

    EmulatedRemoveObject(__PdoGetEmulatedInterface(Pdo),
                         Pdo->EmulatedObject);
    Pdo->EmulatedObject = NULL;
    Pdo->EmulatedInterface = NULL;

    ThreadAlert(Pdo->DevicePowerThread);
    ThreadJoin(Pdo->DevicePowerThread);
    Pdo->DevicePowerThread = NULL;

    ThreadAlert(Pdo->SystemPowerThread);
    ThreadJoin(Pdo->SystemPowerThread);
    Pdo->SystemPowerThread = NULL;

    Pdo->PhysicalDeviceObject = NULL;
    Pdo->LowerDeviceObject = NULL;
    Pdo->Dx = NULL;

    IoDetachDevice(LowerDeviceObject);

    ASSERT(IsZeroMemory(Pdo, sizeof (XENFILT_PDO)));
    __PdoFree(Pdo);

    IoDeleteDevice(FilterDeviceObject);
}
