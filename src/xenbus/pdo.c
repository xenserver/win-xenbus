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
#include <util.h>

#include <binding.h>
#include <shared_info_interface.h>
#include <evtchn_interface.h>
#include <debug_interface.h>
#include <store_interface.h>
#include <gnttab_interface.h>

#include "names.h"
#include "fdo.h"
#include "pdo.h"
#include "bus.h"
#include "driver.h"
#include "thread.h"
#include "log.h"
#include "assert.h"

#define PDO_TAG 'ODP'

struct _XENBUS_PDO {
    PXENBUS_DX                  Dx;

    PXENBUS_THREAD              SystemPowerThread;
    PIRP                        SystemPowerIrp;
    PXENBUS_THREAD              DevicePowerThread;
    PIRP                        DevicePowerIrp;

    PXENBUS_FDO                 Fdo;
    BOOLEAN                     Missing;
    const CHAR                  *Reason;
    UCHAR                       Revision;

    BUS_INTERFACE_STANDARD      BusInterface;

    PXENBUS_SUSPEND_INTERFACE   SuspendInterface;

    PXENBUS_SUSPEND_CALLBACK    SuspendCallbackLate;
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
    IN  PXENBUS_PDO         Pdo,
    IN  DEVICE_PNP_STATE    State
    )
{
    PXENBUS_DX              Dx = Pdo->Dx;

    // We can never transition out of the deleted state
    ASSERT(Dx->DevicePnpState != Deleted || State == Deleted);

    Dx->PreviousDevicePnpState = Dx->DevicePnpState;
    Dx->DevicePnpState = State;
}

VOID
PdoSetDevicePnpState(
    IN  PXENBUS_PDO         Pdo,
    IN  DEVICE_PNP_STATE    State
    )
{
    __PdoSetDevicePnpState(Pdo, State);
}

static FORCEINLINE VOID
__PdoRestoreDevicePnpState(
    IN  PXENBUS_PDO         Pdo,
    IN  DEVICE_PNP_STATE    State
    )
{
    PXENBUS_DX              Dx = Pdo->Dx;

    if (Dx->DevicePnpState == State)
        Dx->DevicePnpState = Dx->PreviousDevicePnpState;
}

static FORCEINLINE DEVICE_PNP_STATE
__PdoGetDevicePnpState(
    IN  PXENBUS_PDO Pdo
    )
{
    PXENBUS_DX      Dx = Pdo->Dx;

    return Dx->DevicePnpState;
}

DEVICE_PNP_STATE
PdoGetDevicePnpState(
    IN  PXENBUS_PDO Pdo
    )
{
    return __PdoGetDevicePnpState(Pdo);
}

static FORCEINLINE VOID
__PdoSetDevicePowerState(
    IN  PXENBUS_PDO         Pdo,
    IN  DEVICE_POWER_STATE  State
    )
{
    PXENBUS_DX              Dx = Pdo->Dx;

    Dx->DevicePowerState = State;
}

static FORCEINLINE DEVICE_POWER_STATE
__PdoGetDevicePowerState(
    IN  PXENBUS_PDO Pdo
    )
{
    PXENBUS_DX      Dx = Pdo->Dx;

    return Dx->DevicePowerState;
}

static FORCEINLINE VOID
__PdoSetSystemPowerState(
    IN  PXENBUS_PDO         Pdo,
    IN  SYSTEM_POWER_STATE  State
    )
{
    PXENBUS_DX              Dx = Pdo->Dx;

    Dx->SystemPowerState = State;
}

static FORCEINLINE SYSTEM_POWER_STATE
__PdoGetSystemPowerState(
    IN  PXENBUS_PDO Pdo
    )
{
    PXENBUS_DX      Dx = Pdo->Dx;

    return Dx->SystemPowerState;
}

static FORCEINLINE VOID
__PdoSetMissing(
    IN  PXENBUS_PDO Pdo,
    IN  const CHAR  *Reason
    )
{
    Pdo->Reason = Reason;
    Pdo->Missing = TRUE;
}

VOID
PdoSetMissing(
    IN  PXENBUS_PDO Pdo,
    IN  const CHAR  *Reason
    )
{
    __PdoSetMissing(Pdo, Reason);
}

static FORCEINLINE BOOLEAN
__PdoIsMissing(
    IN  PXENBUS_PDO Pdo
    )
{
    return Pdo->Missing;
}

BOOLEAN
PdoIsMissing(
    IN  PXENBUS_PDO Pdo
    )
{
    return __PdoIsMissing(Pdo);
}

static FORCEINLINE VOID
__PdoSetName(
    IN  PXENBUS_PDO     Pdo,
    IN  PANSI_STRING    Ansi
    )
{
    PXENBUS_DX          Dx = Pdo->Dx;
    NTSTATUS            status;

    status = RtlStringCbPrintfA(Dx->Name, MAX_DEVICE_ID_LEN, "%Z", Ansi);
    ASSERT(NT_SUCCESS(status));
}

static FORCEINLINE PCHAR
__PdoGetName(
    IN  PXENBUS_PDO Pdo
    )
{
    PXENBUS_DX      Dx = Pdo->Dx;

    return Dx->Name;
}

PCHAR
PdoGetName(
    IN  PXENBUS_PDO Pdo
    )
{
    return __PdoGetName(Pdo);
}

struct _REVISION_ENTRY {
    const CHAR  *Name;
    UCHAR       Revision;
};

static struct _REVISION_ENTRY PdoRevisionTable[] = {
    { "VIF", 0x02 },
    { "VBD", 0x02 },
    { "IFACE", 0x02 },
    { NULL, 0 }
};

static FORCEINLINE VOID
__PdoSetRevision(
    IN  PXENBUS_PDO         Pdo,
    IN  PANSI_STRING        Name
    )
{
    struct _REVISION_ENTRY  *Entry;

    Pdo->Revision = PCI_REVISION;

    for (Entry = PdoRevisionTable; Entry->Name != NULL; Entry++) {
        if (strcmp(Name->Buffer, Entry->Name) == 0) {
            Trace("%s: %02x\n",
                  __PdoGetName(Pdo),
                  Entry->Revision);
            Pdo->Revision = Entry->Revision;
            break;
        }
    }
}

static FORCEINLINE UCHAR
__PdoGetRevision(
    IN  PXENBUS_PDO Pdo
    )
{
    return Pdo->Revision;
}

static FORCEINLINE PDEVICE_OBJECT
__PdoGetDeviceObject(
    IN  PXENBUS_PDO Pdo
    )
{
    PXENBUS_DX      Dx = Pdo->Dx;

    return (Dx->DeviceObject);
}
    
PDEVICE_OBJECT
PdoGetDeviceObject(
    IN  PXENBUS_PDO Pdo
    )
{
    return __PdoGetDeviceObject(Pdo);
}

static FORCEINLINE VOID
__PdoLink(
    IN  PXENBUS_PDO Pdo,
    IN  PXENBUS_FDO Fdo
    )
{
    Pdo->Fdo = Fdo;
    FdoAddPhysicalDeviceObject(Fdo, Pdo);
}

static FORCEINLINE VOID
__PdoUnlink(
    IN  PXENBUS_PDO Pdo
    )
{
    PXENBUS_FDO     Fdo = Pdo->Fdo;

    ASSERT(Fdo != NULL);

    FdoRemovePhysicalDeviceObject(Fdo, Pdo);

    Pdo->Fdo = NULL;
}

static FORCEINLINE PXENBUS_FDO
__PdoGetFdo(
    IN  PXENBUS_PDO Pdo
    )
{
    return Pdo->Fdo;
}

PXENBUS_FDO
PdoGetFdo(
    IN  PXENBUS_PDO Pdo
    )
{
    return __PdoGetFdo(Pdo);
}

static FORCEINLINE PXENBUS_EVTCHN_INTERFACE
__PdoGetEvtchnInterface(
    IN  PXENBUS_PDO Pdo
    )
{
    return FdoGetEvtchnInterface(__PdoGetFdo(Pdo));
}

PXENBUS_EVTCHN_INTERFACE
PdoGetEvtchnInterface(
    IN  PXENBUS_PDO Pdo
    )
{
    return __PdoGetEvtchnInterface(Pdo);
}

static FORCEINLINE PXENBUS_DEBUG_INTERFACE
__PdoGetDebugInterface(
    IN  PXENBUS_PDO Pdo
    )
{
    return FdoGetDebugInterface(__PdoGetFdo(Pdo));
}

PXENBUS_DEBUG_INTERFACE
PdoGetDebugInterface(
    IN  PXENBUS_PDO Pdo
    )
{
    return __PdoGetDebugInterface(Pdo);
}

static FORCEINLINE PXENBUS_GNTTAB_INTERFACE
__PdoGetGnttabInterface(
    IN  PXENBUS_PDO Pdo
    )
{
    return FdoGetGnttabInterface(__PdoGetFdo(Pdo));
}

PXENBUS_GNTTAB_INTERFACE
PdoGetGnttabInterface(
    IN  PXENBUS_PDO Pdo
    )
{
    return __PdoGetGnttabInterface(Pdo);
}

static FORCEINLINE PXENBUS_SUSPEND_INTERFACE
__PdoGetSuspendInterface(
    IN  PXENBUS_PDO Pdo
    )
{
    return FdoGetSuspendInterface(__PdoGetFdo(Pdo));
}

PXENBUS_SUSPEND_INTERFACE
PdoGetSuspendInterface(
    IN  PXENBUS_PDO Pdo
    )
{
    return __PdoGetSuspendInterface(Pdo);
}

static FORCEINLINE PXENBUS_STORE_INTERFACE
__PdoGetStoreInterface(
    IN  PXENBUS_PDO Pdo
    )
{
    return FdoGetStoreInterface(__PdoGetFdo(Pdo));
}

PXENBUS_STORE_INTERFACE
PdoGetStoreInterface(
    IN  PXENBUS_PDO Pdo
    )
{
    return __PdoGetStoreInterface(Pdo);
}

static FORCEINLINE PXENBUS_SHARED_INFO_INTERFACE
__PdoGetSharedInfoInterface(
    IN  PXENBUS_PDO Pdo
    )
{
    return FdoGetSharedInfoInterface(__PdoGetFdo(Pdo));
}

PXENBUS_SHARED_INFO_INTERFACE
PdoGetSharedInfoInterface(
    IN  PXENBUS_PDO Pdo
    )
{
    return __PdoGetSharedInfoInterface(Pdo);
}

BOOLEAN
PdoTranslateAddress(
    IN      PXENBUS_PDO         Pdo,
    IN      PHYSICAL_ADDRESS    BusAddress,
    IN      ULONG               Length,
    IN OUT  PULONG              AddressSpace,
    OUT     PPHYSICAL_ADDRESS   TranslatedAddress
    )
{
    UNREFERENCED_PARAMETER(Pdo);
    UNREFERENCED_PARAMETER(BusAddress);
    UNREFERENCED_PARAMETER(Length);
    UNREFERENCED_PARAMETER(AddressSpace);
    UNREFERENCED_PARAMETER(TranslatedAddress);

    Trace("<===>\n");

    return FALSE;
}

ULONG
PdoSetData(
    IN  PXENBUS_PDO     Pdo,
    IN  ULONG           DataType,
    IN  PVOID           Buffer,
    IN  ULONG           Offset,
    IN  ULONG           Length
    )
{
    UNREFERENCED_PARAMETER(Pdo);
    UNREFERENCED_PARAMETER(DataType);
    UNREFERENCED_PARAMETER(Buffer);
    UNREFERENCED_PARAMETER(Offset);
    UNREFERENCED_PARAMETER(Length);

    // This function should not be called. Shout if it is.
    Warning("<===>\n");

    return 0;
}

ULONG
PdoGetData(
    IN  PXENBUS_PDO     Pdo,
    IN  ULONG           DataType,
    IN  PVOID           Buffer,
    IN  ULONG           Offset,
    IN  ULONG           Length
    )
{
    UNREFERENCED_PARAMETER(Pdo);
    UNREFERENCED_PARAMETER(DataType);
    UNREFERENCED_PARAMETER(Buffer);
    UNREFERENCED_PARAMETER(Offset);
    UNREFERENCED_PARAMETER(Length);

    // This function should not be called. Shout if it is.
    Warning("<===>\n");

    return 0;
}

static FORCEINLINE VOID
__PdoD3ToD0(
    IN  PXENBUS_PDO     Pdo
    )
{
    POWER_STATE         PowerState;

    Trace("(%s) ====>\n", __PdoGetName(Pdo));

    ASSERT3U(KeGetCurrentIrql(), ==, DISPATCH_LEVEL);
    ASSERT3U(__PdoGetDevicePowerState(Pdo), ==, PowerDeviceD3);

    __PdoSetDevicePowerState(Pdo, PowerDeviceD0);

    PowerState.DeviceState = PowerDeviceD0;
    PoSetPowerState(__PdoGetDeviceObject(Pdo),
                    DevicePowerState,
                    PowerState);

    Trace("(%s) <====\n", __PdoGetName(Pdo));
}

static FORCEINLINE VOID
__PdoD0ToD3(
    IN  PXENBUS_PDO     Pdo
    )
{
    POWER_STATE         PowerState;

    Trace("(%s) ====>\n", __PdoGetName(Pdo));

    ASSERT3U(KeGetCurrentIrql(), ==, DISPATCH_LEVEL);
    ASSERT3U(__PdoGetDevicePowerState(Pdo), ==, PowerDeviceD0);

    PowerState.DeviceState = PowerDeviceD3;
    PoSetPowerState(__PdoGetDeviceObject(Pdo),
                    DevicePowerState,
                    PowerState);

    __PdoSetDevicePowerState(Pdo, PowerDeviceD3);

    Trace("(%s) <====\n", __PdoGetName(Pdo));
}

static DECLSPEC_NOINLINE VOID
PdoSuspendCallbackLate(
    IN  PVOID   Argument
    )
{
    PXENBUS_PDO Pdo = Argument;

    __PdoD0ToD3(Pdo);
    __PdoD3ToD0(Pdo);
}

static DECLSPEC_NOINLINE NTSTATUS
PdoD3ToD0(
    IN  PXENBUS_PDO Pdo
    )
{
    KIRQL           Irql;
    NTSTATUS        status;

    ASSERT3U(KeGetCurrentIrql(), ==, PASSIVE_LEVEL);

    KeRaiseIrql(DISPATCH_LEVEL, &Irql);
    __PdoD3ToD0(Pdo);
    KeLowerIrql(Irql);

    Pdo->SuspendInterface = __PdoGetSuspendInterface(Pdo);

    SUSPEND(Acquire, Pdo->SuspendInterface);

    status = SUSPEND(Register,
                     Pdo->SuspendInterface,
                     SUSPEND_CALLBACK_LATE,
                     PdoSuspendCallbackLate,
                     Pdo,
                     &Pdo->SuspendCallbackLate);
    if (!NT_SUCCESS(status))
        goto fail1;

    return STATUS_SUCCESS;

fail1:
    Error("fail1 (%08x)\n", status);

    SUSPEND(Release, Pdo->SuspendInterface);
    Pdo->SuspendInterface = NULL;

    KeRaiseIrql(DISPATCH_LEVEL, &Irql);
    __PdoD0ToD3(Pdo);
    KeLowerIrql(Irql);

    return status;
}

static DECLSPEC_NOINLINE VOID
PdoD0ToD3(
    IN  PXENBUS_PDO Pdo
    )
{
    KIRQL           Irql;

    ASSERT3U(KeGetCurrentIrql(), ==, PASSIVE_LEVEL);

    SUSPEND(Deregister,
            Pdo->SuspendInterface,
            Pdo->SuspendCallbackLate);
    Pdo->SuspendCallbackLate = NULL;

    SUSPEND(Release, Pdo->SuspendInterface);
    Pdo->SuspendInterface = NULL;

    KeRaiseIrql(DISPATCH_LEVEL, &Irql);
    __PdoD0ToD3(Pdo);
    KeLowerIrql(Irql);
}

static DECLSPEC_NOINLINE VOID
PdoS4ToS3(
    IN  PXENBUS_PDO Pdo
    )
{
    Trace("(%s) ====>\n", __PdoGetName(Pdo));

    ASSERT3U(KeGetCurrentIrql(), ==, PASSIVE_LEVEL);
    ASSERT3U(__PdoGetSystemPowerState(Pdo), ==, PowerSystemHibernate);

    __PdoSetSystemPowerState(Pdo, PowerSystemSleeping3);

    Trace("(%s) <====\n", __PdoGetName(Pdo));
}

static DECLSPEC_NOINLINE VOID
PdoS3ToS4(
    IN  PXENBUS_PDO Pdo
    )
{
    Trace("(%s) ====>\n", __PdoGetName(Pdo));

    ASSERT3U(KeGetCurrentIrql(), ==, PASSIVE_LEVEL);
    ASSERT3U(__PdoGetSystemPowerState(Pdo), ==, PowerSystemSleeping3);

    __PdoSetSystemPowerState(Pdo, PowerSystemHibernate);

    Trace("(%s) <====\n", __PdoGetName(Pdo));
}

static DECLSPEC_NOINLINE VOID
PdoParseResources(
    IN  PXENBUS_PDO             Pdo,
    IN  PCM_RESOURCE_LIST       RawResourceList,
    IN  PCM_RESOURCE_LIST       TranslatedResourceList
    )
{
    PCM_PARTIAL_RESOURCE_LIST   RawPartialList;
    PCM_PARTIAL_RESOURCE_LIST   TranslatedPartialList;
    ULONG                       Index;

    UNREFERENCED_PARAMETER(Pdo);

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
             __PdoGetName(Pdo),
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
            break;

        default:
            break;
        }
    }

    Trace("<====\n");
}

static DECLSPEC_NOINLINE NTSTATUS
PdoStartDevice(
    IN  PXENBUS_PDO     Pdo,
    IN  PIRP            Irp
    )
{
    PIO_STACK_LOCATION  StackLocation;
    NTSTATUS            status;

    StackLocation = IoGetCurrentIrpStackLocation(Irp);

    PdoParseResources(Pdo,
                      StackLocation->Parameters.StartDevice.AllocatedResources,
                      StackLocation->Parameters.StartDevice.AllocatedResourcesTranslated);

    __PdoSetSystemPowerState(Pdo, PowerSystemHibernate);

    PdoS4ToS3(Pdo);
    
    __PdoSetSystemPowerState(Pdo, PowerSystemWorking);

    PdoD3ToD0(Pdo);

    __PdoSetDevicePnpState(Pdo, Started);
    status = STATUS_SUCCESS;

    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return STATUS_SUCCESS;
}

static DECLSPEC_NOINLINE NTSTATUS
PdoQueryStopDevice(
    IN  PXENBUS_PDO Pdo,
    IN  PIRP        Irp
    )
{
    NTSTATUS        status;

    __PdoSetDevicePnpState(Pdo, StopPending);
    status = STATUS_SUCCESS;

    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

static DECLSPEC_NOINLINE NTSTATUS
PdoCancelStopDevice(
    IN  PXENBUS_PDO Pdo,
    IN  PIRP        Irp
    )
{
    NTSTATUS        status;

    __PdoRestoreDevicePnpState(Pdo, StopPending);
    status = STATUS_SUCCESS;

    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

static DECLSPEC_NOINLINE NTSTATUS
PdoStopDevice(
    IN  PXENBUS_PDO Pdo,
    IN  PIRP        Irp
    )
{
    NTSTATUS        status;

    PdoD0ToD3(Pdo);

    __PdoSetSystemPowerState(Pdo, PowerSystemSleeping3);
    PdoS3ToS4(Pdo);
    __PdoSetSystemPowerState(Pdo, PowerSystemShutdown);

    __PdoSetDevicePnpState(Pdo, Stopped);
    status = STATUS_SUCCESS;

    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

static DECLSPEC_NOINLINE NTSTATUS
PdoQueryRemoveDevice(
    IN  PXENBUS_PDO Pdo,
    IN  PIRP        Irp
    )
{
    NTSTATUS        status;

    __PdoSetDevicePnpState(Pdo, RemovePending);
    status = STATUS_SUCCESS;

    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

static DECLSPEC_NOINLINE NTSTATUS
PdoCancelRemoveDevice(
    IN  PXENBUS_PDO Pdo,
    IN  PIRP        Irp
    )
{
    NTSTATUS        status;

    __PdoRestoreDevicePnpState(Pdo, RemovePending);
    status = STATUS_SUCCESS;

    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

static DECLSPEC_NOINLINE NTSTATUS
PdoSurpriseRemoval(
    IN  PXENBUS_PDO Pdo,
    IN  PIRP        Irp
    )
{
    NTSTATUS        status;

    Warning("%s\n", __PdoGetName(Pdo));

    __PdoSetDevicePnpState(Pdo, SurpriseRemovePending);
    status = STATUS_SUCCESS;

    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

static DECLSPEC_NOINLINE NTSTATUS
PdoRemoveDevice(
    IN  PXENBUS_PDO Pdo,
    IN  PIRP        Irp
    )
{
    PXENBUS_FDO     Fdo = __PdoGetFdo(Pdo);
    NTSTATUS        status;

    if (__PdoGetDevicePowerState(Pdo) != PowerDeviceD0)
        goto done;

    PdoD0ToD3(Pdo);

    __PdoSetSystemPowerState(Pdo, PowerSystemSleeping3);
    PdoS3ToS4(Pdo);
    __PdoSetSystemPowerState(Pdo, PowerSystemShutdown);

done:
    FdoAcquireMutex(Fdo);

    if (__PdoIsMissing(Pdo) ||
        __PdoGetDevicePnpState(Pdo) == SurpriseRemovePending)
        __PdoSetDevicePnpState(Pdo, Deleted);
    else
        __PdoSetDevicePnpState(Pdo, Enumerated);

    if (__PdoIsMissing(Pdo)) {
        if (__PdoGetDevicePnpState(Pdo) == Deleted)
            PdoDestroy(Pdo);
        else
            IoInvalidateDeviceRelations(FdoGetPhysicalDeviceObject(Fdo), 
                                        BusRelations);
    }

    FdoReleaseMutex(Fdo);

    status = STATUS_SUCCESS;

    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

static DECLSPEC_NOINLINE NTSTATUS
PdoQueryDeviceRelations(
    IN  PXENBUS_PDO     Pdo,
    IN  PIRP            Irp
    )
{
    PIO_STACK_LOCATION  StackLocation;
    PDEVICE_RELATIONS   Relations;
    NTSTATUS            status;

    StackLocation = IoGetCurrentIrpStackLocation(Irp);

    status = Irp->IoStatus.Status;

    if (StackLocation->Parameters.QueryDeviceRelations.Type != TargetDeviceRelation)
        goto done;

    Relations = ExAllocatePoolWithTag(PagedPool, sizeof (DEVICE_RELATIONS), 'SUB');

    status = STATUS_NO_MEMORY;
    if (Relations == NULL)
        goto done;

    RtlZeroMemory(Relations, sizeof (DEVICE_RELATIONS));

    Relations->Count = 1;
    ObReferenceObject(__PdoGetDeviceObject(Pdo));
    Relations->Objects[0] = __PdoGetDeviceObject(Pdo);

    Irp->IoStatus.Information = (ULONG_PTR)Relations;
    status = STATUS_SUCCESS;

done:
    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

static FORCEINLINE NTSTATUS
__PdoDelegateIrp(
    IN  PXENBUS_PDO Pdo,
    IN  PIRP        Irp
    )
{
    return FdoDelegateIrp(__PdoGetFdo(Pdo), Irp);
}

static NTSTATUS
PdoQueryBusInterface(
    IN  PXENBUS_PDO         Pdo,
    IN  PIRP                Irp
    )
{
    PIO_STACK_LOCATION      StackLocation;
    USHORT                  Size;
    USHORT                  Version;
    PBUS_INTERFACE_STANDARD BusInterface;
    NTSTATUS                status;

    status = Irp->IoStatus.Status;        

    StackLocation = IoGetCurrentIrpStackLocation(Irp);
    Size = StackLocation->Parameters.QueryInterface.Size;
    Version = StackLocation->Parameters.QueryInterface.Version;
    BusInterface = (PBUS_INTERFACE_STANDARD)StackLocation->Parameters.QueryInterface.Interface;

    if (StackLocation->Parameters.QueryInterface.Version != 1)
        goto done;

    status = STATUS_BUFFER_TOO_SMALL;        
    if (StackLocation->Parameters.QueryInterface.Size < sizeof (BUS_INTERFACE_STANDARD))
        goto done;

    *BusInterface = Pdo->BusInterface;
    BusInterface->InterfaceReference(BusInterface->Context);

    Irp->IoStatus.Information = 0;
    status = STATUS_SUCCESS;

done:
    return status;
}

static NTSTATUS
PdoQueryDebugInterface(
    IN  PXENBUS_PDO         Pdo,
    IN  PIRP                Irp
    )
{
    PIO_STACK_LOCATION      StackLocation;
    USHORT                  Size;
    USHORT                  Version;
    PINTERFACE              Interface;
    NTSTATUS                status;

    status = Irp->IoStatus.Status;        

    StackLocation = IoGetCurrentIrpStackLocation(Irp);
    Size = StackLocation->Parameters.QueryInterface.Size;
    Version = StackLocation->Parameters.QueryInterface.Version;
    Interface = StackLocation->Parameters.QueryInterface.Interface;

    if (StackLocation->Parameters.QueryInterface.Version != DEBUG_INTERFACE_VERSION)
        goto done;

    status = STATUS_BUFFER_TOO_SMALL;        
    if (StackLocation->Parameters.QueryInterface.Size < sizeof (INTERFACE))
        goto done;

    Interface->Size = sizeof (INTERFACE);
    Interface->Version = DEBUG_INTERFACE_VERSION;
    Interface->Context = __PdoGetDebugInterface(Pdo);
    Interface->InterfaceReference = NULL;
    Interface->InterfaceDereference = NULL;

    Irp->IoStatus.Information = 0;
    status = STATUS_SUCCESS;

done:
    return status;
}

static NTSTATUS
PdoQuerySuspendInterface(
    IN  PXENBUS_PDO             Pdo,
    IN  PIRP                    Irp
    )
{
    PIO_STACK_LOCATION          StackLocation;
    USHORT                      Size;
    USHORT                      Version;
    PINTERFACE                  Interface;
    NTSTATUS                    status;

    status = Irp->IoStatus.Status;        

    StackLocation = IoGetCurrentIrpStackLocation(Irp);
    Size = StackLocation->Parameters.QueryInterface.Size;
    Version = StackLocation->Parameters.QueryInterface.Version;
    Interface = StackLocation->Parameters.QueryInterface.Interface;

    if (StackLocation->Parameters.QueryInterface.Version != SUSPEND_INTERFACE_VERSION)
        goto done;

    status = STATUS_BUFFER_TOO_SMALL;        
    if (StackLocation->Parameters.QueryInterface.Size < sizeof (INTERFACE))
        goto done;

    Interface->Size = sizeof (INTERFACE);
    Interface->Version = SUSPEND_INTERFACE_VERSION;
    Interface->Context = __PdoGetSuspendInterface(Pdo);
    Interface->InterfaceReference = NULL;
    Interface->InterfaceDereference = NULL;

    Irp->IoStatus.Information = 0;
    status = STATUS_SUCCESS;

done:
    return status;
}

static NTSTATUS
PdoQuerySharedInfoInterface(
    IN  PXENBUS_PDO                 Pdo,
    IN  PIRP                        Irp
    )
{
    PIO_STACK_LOCATION              StackLocation;
    USHORT                          Size;
    USHORT                          Version;
    PINTERFACE                      Interface;
    NTSTATUS                        status;

    status = Irp->IoStatus.Status;        

    StackLocation = IoGetCurrentIrpStackLocation(Irp);
    Size = StackLocation->Parameters.QueryInterface.Size;
    Version = StackLocation->Parameters.QueryInterface.Version;
    Interface = StackLocation->Parameters.QueryInterface.Interface;

    if (StackLocation->Parameters.QueryInterface.Version != SHARED_INFO_INTERFACE_VERSION)
        goto done;

    status = STATUS_BUFFER_TOO_SMALL;        
    if (StackLocation->Parameters.QueryInterface.Size < sizeof (INTERFACE))
        goto done;

    Interface->Size = sizeof (INTERFACE);
    Interface->Version = SHARED_INFO_INTERFACE_VERSION;
    Interface->Context = __PdoGetSharedInfoInterface(Pdo);
    Interface->InterfaceReference = NULL;
    Interface->InterfaceDereference = NULL;

    Irp->IoStatus.Information = 0;
    status = STATUS_SUCCESS;

done:
    return status;
}

static NTSTATUS
PdoQueryEvtchnInterface(
    IN  PXENBUS_PDO             Pdo,
    IN  PIRP                    Irp
    )
{
    PIO_STACK_LOCATION          StackLocation;
    USHORT                      Size;
    USHORT                      Version;
    PINTERFACE                  Interface;
    NTSTATUS                    status;

    status = Irp->IoStatus.Status;        

    StackLocation = IoGetCurrentIrpStackLocation(Irp);
    Size = StackLocation->Parameters.QueryInterface.Size;
    Version = StackLocation->Parameters.QueryInterface.Version;
    Interface = StackLocation->Parameters.QueryInterface.Interface;

    if (StackLocation->Parameters.QueryInterface.Version != EVTCHN_INTERFACE_VERSION)
        goto done;

    status = STATUS_BUFFER_TOO_SMALL;        
    if (StackLocation->Parameters.QueryInterface.Size < sizeof (INTERFACE))
        goto done;

    Interface->Size = sizeof (INTERFACE);
    Interface->Version = EVTCHN_INTERFACE_VERSION;
    Interface->Context = __PdoGetEvtchnInterface(Pdo);
    Interface->InterfaceReference = NULL;
    Interface->InterfaceDereference = NULL;

    Irp->IoStatus.Information = 0;
    status = STATUS_SUCCESS;

done:
    return status;
}

static NTSTATUS
PdoQueryStoreInterface(
    IN  PXENBUS_PDO             Pdo,
    IN  PIRP                    Irp
    )
{
    PIO_STACK_LOCATION          StackLocation;
    USHORT                      Size;
    USHORT                      Version;
    PINTERFACE                  Interface;
    NTSTATUS                    status;

    status = Irp->IoStatus.Status;        

    StackLocation = IoGetCurrentIrpStackLocation(Irp);
    Size = StackLocation->Parameters.QueryInterface.Size;
    Version = StackLocation->Parameters.QueryInterface.Version;
    Interface = StackLocation->Parameters.QueryInterface.Interface;

    if (StackLocation->Parameters.QueryInterface.Version != STORE_INTERFACE_VERSION)
        goto done;

    status = STATUS_BUFFER_TOO_SMALL;        
    if (StackLocation->Parameters.QueryInterface.Size < sizeof (INTERFACE))
        goto done;

    Interface->Size = sizeof (INTERFACE);
    Interface->Version = STORE_INTERFACE_VERSION;
    Interface->Context = __PdoGetStoreInterface(Pdo);
    Interface->InterfaceReference = NULL;
    Interface->InterfaceDereference = NULL;

    Irp->IoStatus.Information = 0;
    status = STATUS_SUCCESS;

done:
    return status;
}

static NTSTATUS
PdoQueryGnttabInterface(
    IN  PXENBUS_PDO             Pdo,
    IN  PIRP                    Irp
    )
{
    PIO_STACK_LOCATION          StackLocation;
    USHORT                      Size;
    USHORT                      Version;
    PINTERFACE                  Interface;
    NTSTATUS                    status;

    status = Irp->IoStatus.Status;        

    StackLocation = IoGetCurrentIrpStackLocation(Irp);
    Size = StackLocation->Parameters.QueryInterface.Size;
    Version = StackLocation->Parameters.QueryInterface.Version;
    Interface = StackLocation->Parameters.QueryInterface.Interface;

    if (StackLocation->Parameters.QueryInterface.Version != GNTTAB_INTERFACE_VERSION)
        goto done;

    status = STATUS_BUFFER_TOO_SMALL;        
    if (StackLocation->Parameters.QueryInterface.Size < sizeof (INTERFACE))
        goto done;

    Interface->Size = sizeof (INTERFACE);
    Interface->Version = GNTTAB_INTERFACE_VERSION;
    Interface->Context = __PdoGetGnttabInterface(Pdo);
    Interface->InterfaceReference = NULL;
    Interface->InterfaceDereference = NULL;

    Irp->IoStatus.Information = 0;
    status = STATUS_SUCCESS;

done:
    return status;
}

struct _INTERFACE_ENTRY {
    const GUID  *Guid;
    const CHAR  *Name;
    NTSTATUS    (*Handler)(PXENBUS_PDO, PIRP);
};

#define DEFINE_HANDLER(_Guid, _Function)    \
        { &GUID_ ## _Guid, #_Guid, (_Function) }

struct _INTERFACE_ENTRY PdoInterfaceTable[] = {
    DEFINE_HANDLER(BUS_INTERFACE_STANDARD, PdoQueryBusInterface),
    DEFINE_HANDLER(DEBUG_INTERFACE, PdoQueryDebugInterface),
    DEFINE_HANDLER(SUSPEND_INTERFACE, PdoQuerySuspendInterface),
    DEFINE_HANDLER(SHARED_INFO_INTERFACE, PdoQuerySharedInfoInterface),
    DEFINE_HANDLER(EVTCHN_INTERFACE, PdoQueryEvtchnInterface),
    DEFINE_HANDLER(STORE_INTERFACE, PdoQueryStoreInterface),
    DEFINE_HANDLER(GNTTAB_INTERFACE, PdoQueryGnttabInterface),
    { NULL, NULL, NULL }
};

static DECLSPEC_NOINLINE NTSTATUS
PdoQueryInterface(
    IN  PXENBUS_PDO         Pdo,
    IN  PIRP                Irp
    )
{
    PIO_STACK_LOCATION      StackLocation;
    const GUID              *InterfaceType;
    struct _INTERFACE_ENTRY *Entry;
    NTSTATUS                status;

    status = Irp->IoStatus.Status;

    if (status != STATUS_NOT_SUPPORTED)
        goto done;

    StackLocation = IoGetCurrentIrpStackLocation(Irp);
    InterfaceType = StackLocation->Parameters.QueryInterface.InterfaceType;

    for (Entry = PdoInterfaceTable; Entry->Guid != NULL; Entry++) {
        if (IsEqualGUID(InterfaceType, Entry->Guid)) {
            Trace("%s: %s\n",
                  __PdoGetName(Pdo),
                  Entry->Name);
            status = Entry->Handler(Pdo, Irp);
            goto done;
        }
    }

    status = __PdoDelegateIrp(Pdo, Irp);

done:
    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

static DECLSPEC_NOINLINE NTSTATUS
PdoQueryCapabilities(
    IN  PXENBUS_PDO         Pdo,
    IN  PIRP                Irp
    )
{
    PIO_STACK_LOCATION      StackLocation;
    PDEVICE_CAPABILITIES    Capabilities;
    SYSTEM_POWER_STATE      SystemPowerState;
    NTSTATUS                status;

    UNREFERENCED_PARAMETER(Pdo);

    StackLocation = IoGetCurrentIrpStackLocation(Irp);
    Capabilities = StackLocation->Parameters.DeviceCapabilities.Capabilities;

    status = STATUS_INVALID_PARAMETER;
    if (Capabilities->Version != 1)
        goto done;

    Capabilities->DeviceD1 = 0;
    Capabilities->DeviceD2 = 0;
    Capabilities->LockSupported = 0;
    Capabilities->EjectSupported = 0;
    Capabilities->Removable = 1;
    Capabilities->DockDevice = 0;
    Capabilities->UniqueID = 1;
    Capabilities->SilentInstall = 1;
    Capabilities->RawDeviceOK = 0;
    Capabilities->SurpriseRemovalOK = 1;
    Capabilities->HardwareDisabled = 0;
    Capabilities->NoDisplayInUI = 0;

    Capabilities->Address = 0xffffffff;
    Capabilities->UINumber = 0xffffffff;

    for (SystemPowerState = 0; SystemPowerState < PowerSystemMaximum; SystemPowerState++) {
        switch (SystemPowerState) {
        case PowerSystemUnspecified:
        case PowerSystemSleeping1:
        case PowerSystemSleeping2:
            break;

        case PowerSystemWorking:
            Capabilities->DeviceState[SystemPowerState] = PowerDeviceD0;
            break;

        default:
            Capabilities->DeviceState[SystemPowerState] = PowerDeviceD3;
            break;
        }
    }

    Capabilities->SystemWake = PowerSystemUnspecified;
    Capabilities->DeviceWake = PowerDeviceUnspecified;
    Capabilities->D1Latency = 0;
    Capabilities->D2Latency = 0;
    Capabilities->D3Latency = 0;

    status = STATUS_SUCCESS;

done:
    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

static DECLSPEC_NOINLINE NTSTATUS
PdoQueryResourceRequirements(
    IN  PXENBUS_PDO                 Pdo,
    IN  PIRP                        Irp
    )
{
    IO_RESOURCE_DESCRIPTOR          Memory;
    IO_RESOURCE_DESCRIPTOR          Interrupt;
    ULONG                           Size;
    PIO_RESOURCE_REQUIREMENTS_LIST  Requirements;
    PIO_RESOURCE_LIST               List;
    NTSTATUS                        status;

    UNREFERENCED_PARAMETER(Pdo);

    RtlZeroMemory(&Memory, sizeof (IO_RESOURCE_DESCRIPTOR));
    Memory.Type = CmResourceTypeMemory;
    Memory.ShareDisposition = CmResourceShareDeviceExclusive;
    Memory.Flags = CM_RESOURCE_MEMORY_READ_WRITE |
                   CM_RESOURCE_MEMORY_PREFETCHABLE |
                   CM_RESOURCE_MEMORY_CACHEABLE;

    Memory.u.Memory.Length = PAGE_SIZE;
    Memory.u.Memory.Alignment = PAGE_SIZE;
    Memory.u.Memory.MinimumAddress.QuadPart = 0;
    Memory.u.Memory.MaximumAddress.QuadPart = -1;

    RtlZeroMemory(&Interrupt, sizeof (IO_RESOURCE_DESCRIPTOR));
    Interrupt.Type = CmResourceTypeInterrupt;
    Interrupt.ShareDisposition = CmResourceShareDeviceExclusive;
    Interrupt.Flags = CM_RESOURCE_INTERRUPT_LEVEL_SENSITIVE;

    Interrupt.u.Interrupt.MinimumVector = (ULONG)0;
    Interrupt.u.Interrupt.MaximumVector = (ULONG)-1;
    Interrupt.u.Interrupt.AffinityPolicy = IrqPolicyOneCloseProcessor;
    Interrupt.u.Interrupt.PriorityPolicy = IrqPriorityUndefined;

    Size = sizeof (IO_RESOURCE_DESCRIPTOR) * 2;
    Size += FIELD_OFFSET(IO_RESOURCE_LIST, Descriptors);
    Size += FIELD_OFFSET(IO_RESOURCE_REQUIREMENTS_LIST, List);

    Requirements = ExAllocatePoolWithTag(PagedPool, Size, 'SUB');

    status = STATUS_NO_MEMORY;
    if (Requirements == NULL)
        goto fail1;

    RtlZeroMemory(Requirements, Size);

    Requirements->ListSize = Size;
    Requirements->InterfaceType = Internal;
    Requirements->BusNumber = 0;
    Requirements->SlotNumber = 0;
    Requirements->AlternativeLists = 1;

    List = &Requirements->List[0];
    List->Version = 1;
    List->Revision = 1;
    List->Count = 2;
    List->Descriptors[0] = Memory;
    List->Descriptors[1] = Interrupt;

    Irp->IoStatus.Information = (ULONG_PTR)Requirements;

    Irp->IoStatus.Status = STATUS_SUCCESS;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return STATUS_SUCCESS;

fail1:
    Error("fail1 (%08x)\n", status);

    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

#define MAXTEXTLEN  128

static DECLSPEC_NOINLINE NTSTATUS
PdoQueryDeviceText(
    IN  PXENBUS_PDO     Pdo,
    IN  PIRP            Irp
    )
{
    PIO_STACK_LOCATION  StackLocation;
    PWCHAR              Buffer;
    UNICODE_STRING      Text;
    NTSTATUS            status;

    StackLocation = IoGetCurrentIrpStackLocation(Irp);

    switch (StackLocation->Parameters.QueryDeviceText.DeviceTextType) {
    case DeviceTextDescription:
        Trace("DeviceTextDescription\n");
        break;

    case DeviceTextLocationInformation:
        Trace("DeviceTextLocationInformation\n");
        break;

    default:
        Irp->IoStatus.Information = 0;
        status = STATUS_NOT_SUPPORTED;
        goto done;
    }

    Buffer = ExAllocatePoolWithTag(PagedPool, MAXTEXTLEN, 'SUB');

    status = STATUS_NO_MEMORY;
    if (Buffer == NULL)
        goto done;

    RtlZeroMemory(Buffer, MAXTEXTLEN);

    Text.Buffer = Buffer;
    Text.MaximumLength = MAXTEXTLEN;
    Text.Length = 0;

    switch (StackLocation->Parameters.QueryDeviceText.DeviceTextType) {
    case DeviceTextDescription: {
        status = RtlStringCbPrintfW(Buffer,
                                    MAXTEXTLEN,
                                    L"%hs %hs",
                                    FdoGetName(__PdoGetFdo(Pdo)),
                                    __PdoGetName(Pdo));
        ASSERT(NT_SUCCESS(status));

        Buffer += wcslen(Buffer);

        break;
    }
    case DeviceTextLocationInformation:
        status = RtlStringCbPrintfW(Buffer,
                                    MAXTEXTLEN,
                                    L"%hs",
                                    __PdoGetName(Pdo));
        ASSERT(NT_SUCCESS(status));

        Buffer += wcslen(Buffer);

        break;

    default:
        ASSERT(FALSE);
        break;
    }

    Text.Length = (USHORT)((ULONG_PTR)Buffer - (ULONG_PTR)Text.Buffer);

    Trace("%s: %wZ\n", __PdoGetName(Pdo), &Text);

    Irp->IoStatus.Information = (ULONG_PTR)Text.Buffer;
    status = STATUS_SUCCESS;

done:
    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

static DECLSPEC_NOINLINE NTSTATUS
PdoReadConfig(
    IN  PXENBUS_PDO Pdo,
    IN  PIRP        Irp
    )
{
    UNREFERENCED_PARAMETER(Pdo);

    Irp->IoStatus.Status = STATUS_NOT_SUPPORTED;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return STATUS_NOT_SUPPORTED;
}

static DECLSPEC_NOINLINE NTSTATUS
PdoWriteConfig(
    IN  PXENBUS_PDO Pdo,
    IN  PIRP        Irp
    )
{
    UNREFERENCED_PARAMETER(Pdo);

    Irp->IoStatus.Status = STATUS_NOT_SUPPORTED;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return STATUS_NOT_SUPPORTED;
}

#define MAX_DEVICE_ID_LEN   200

static DECLSPEC_NOINLINE NTSTATUS
PdoQueryId(
    IN  PXENBUS_PDO     Pdo,
    IN  PIRP            Irp
    )
{
    PIO_STACK_LOCATION  StackLocation;
    PWCHAR              Buffer;
    UNICODE_STRING      Id;
    ULONG               Type;
    NTSTATUS            status;

    StackLocation = IoGetCurrentIrpStackLocation(Irp);

    switch (StackLocation->Parameters.QueryId.IdType) {
    case BusQueryInstanceID:
        Trace("BusQueryInstanceID\n");
        break;

    case BusQueryDeviceID:
        Trace("BusQueryDeviceID\n");
        break;

    case BusQueryHardwareIDs:
        Trace("BusQueryHardwareIDs\n");
        break;

    case BusQueryCompatibleIDs:
        Trace("BusQueryCompatibleIDs\n");
        break;

    default:
        Irp->IoStatus.Information = 0;
        status = STATUS_NOT_SUPPORTED;
        goto done;
    }

    Buffer = ExAllocatePoolWithTag(PagedPool, MAX_DEVICE_ID_LEN, 'SUB');

    status = STATUS_NO_MEMORY;
    if (Buffer == NULL)
        goto done;

    RtlZeroMemory(Buffer, MAX_DEVICE_ID_LEN);

    Id.Buffer = Buffer;
    Id.MaximumLength = MAX_DEVICE_ID_LEN;
    Id.Length = 0;

    switch (StackLocation->Parameters.QueryId.IdType) {
    case BusQueryInstanceID:
        Type = REG_SZ;

        RtlAppendUnicodeToString(&Id, L"_");
        break;

    case BusQueryDeviceID:
        Type = REG_SZ;

        status = RtlStringCbPrintfW(Buffer,
                                    MAX_DEVICE_ID_LEN,
                                    L"XENBUS\\CLASS_%hs&REV_%02X",
                                    __PdoGetName(Pdo),
                                    __PdoGetRevision(Pdo));
        ASSERT(NT_SUCCESS(status));

        Buffer += wcslen(Buffer);

        break;

    case BusQueryHardwareIDs:
    case BusQueryCompatibleIDs: {
        ULONG   Length;

        Type = REG_MULTI_SZ;

        Length = MAX_DEVICE_ID_LEN;
        status = RtlStringCbPrintfW(Buffer,
                                    Length,
                                    L"XENBUS\\CLASS_%hs&REV_%02X",
                                    __PdoGetName(Pdo),
                                    __PdoGetRevision(Pdo));
        ASSERT(NT_SUCCESS(status));

        Buffer += wcslen(Buffer);
        Buffer++;

        Length = MAX_DEVICE_ID_LEN - (ULONG)((ULONG_PTR)Buffer - (ULONG_PTR)Id.Buffer); 
        status = RtlStringCbPrintfW(Buffer,
                                    Length,
                                    L"XENCLASS");
        ASSERT(NT_SUCCESS(status));

        Buffer += wcslen(Buffer);
        Buffer++;

        break;
    }
    default:
        Type = REG_NONE;

        ASSERT(FALSE);
        break;
    }

    Id.Length = (USHORT)((ULONG_PTR)Buffer - (ULONG_PTR)Id.Buffer);
    Buffer = Id.Buffer;

    ASSERT3U(KeGetCurrentIrql(), ==, PASSIVE_LEVEL);

    switch (Type) {
    case REG_SZ:
        Trace("- %ws\n", Buffer);
        break;

    case REG_MULTI_SZ:
        do {
            Trace("- %ws\n", Buffer);
            Buffer += wcslen(Buffer);
            Buffer++;
        } while (*Buffer != L'\0');
        break;

    default:
        ASSERT(FALSE);
        break;
    }

    Irp->IoStatus.Information = (ULONG_PTR)Id.Buffer;
    status = STATUS_SUCCESS;

done:
    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

static DECLSPEC_NOINLINE NTSTATUS
PdoQueryBusInformation(
    IN  PXENBUS_PDO         Pdo,
    IN  PIRP                Irp
    )
{
    PPNP_BUS_INFORMATION    Info;
    NTSTATUS                status;

    UNREFERENCED_PARAMETER(Pdo);

    Info = ExAllocatePoolWithTag(PagedPool, sizeof (PNP_BUS_INFORMATION), 'SUB');

    status = STATUS_NO_MEMORY;
    if (Info == NULL)
        goto done;

    RtlZeroMemory(Info, sizeof (PNP_BUS_INFORMATION));

    Info->BusTypeGuid = GUID_BUS_TYPE_INTERNAL;
    Info->LegacyBusType = Internal;
    Info->BusNumber = 0;

    Irp->IoStatus.Information = (ULONG_PTR)Info;
    status = STATUS_SUCCESS;

done:
    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

static DECLSPEC_NOINLINE NTSTATUS
PdoDeviceUsageNotification(
    IN  PXENBUS_PDO Pdo,
    IN  PIRP        Irp
    )
{
    NTSTATUS        status;

    status = __PdoDelegateIrp(Pdo, Irp);

    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

static DECLSPEC_NOINLINE NTSTATUS
PdoEject(
    IN  PXENBUS_PDO Pdo,
    IN  PIRP        Irp
    )
{
    PXENBUS_FDO     Fdo = __PdoGetFdo(Pdo);
    NTSTATUS        status;

    Trace("%s\n", __PdoGetName(Pdo));

    FdoAcquireMutex(Fdo);

    __PdoSetDevicePnpState(Pdo, Deleted);
    __PdoSetMissing(Pdo, "device ejected");

    PdoDestroy(Pdo);

    FdoReleaseMutex(Fdo);

    status = STATUS_SUCCESS;

    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

static DECLSPEC_NOINLINE NTSTATUS
PdoDispatchPnp(
    IN  PXENBUS_PDO     Pdo,
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

    case IRP_MN_CANCEL_REMOVE_DEVICE:
        status = PdoCancelRemoveDevice(Pdo, Irp);
        break;

    case IRP_MN_SURPRISE_REMOVAL:
        status = PdoSurpriseRemoval(Pdo, Irp);
        break;

    case IRP_MN_REMOVE_DEVICE:
        status = PdoRemoveDevice(Pdo, Irp);
        break;

    case IRP_MN_QUERY_DEVICE_RELATIONS:
        status = PdoQueryDeviceRelations(Pdo, Irp);
        break;

    case IRP_MN_QUERY_INTERFACE:
        status = PdoQueryInterface(Pdo, Irp);
        break;

    case IRP_MN_QUERY_CAPABILITIES:
        status = PdoQueryCapabilities(Pdo, Irp);
        break;

    case IRP_MN_QUERY_RESOURCE_REQUIREMENTS:
        status = PdoQueryResourceRequirements(Pdo, Irp);
        break;

    case IRP_MN_QUERY_DEVICE_TEXT:
        status = PdoQueryDeviceText(Pdo, Irp);
        break;

    case IRP_MN_READ_CONFIG:
        status = PdoReadConfig(Pdo, Irp);
        break;

    case IRP_MN_WRITE_CONFIG:
        status = PdoWriteConfig(Pdo, Irp);
        break;

    case IRP_MN_QUERY_ID:
        status = PdoQueryId(Pdo, Irp);
        break;

    case IRP_MN_QUERY_BUS_INFORMATION:
        status = PdoQueryBusInformation(Pdo, Irp);
        break;

    case IRP_MN_DEVICE_USAGE_NOTIFICATION:
        status = PdoDeviceUsageNotification(Pdo, Irp);
        break;

    case IRP_MN_EJECT:
        status = PdoEject(Pdo, Irp);
        break;

    default:
        status = Irp->IoStatus.Status;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        break;
    }

    Trace("<==== (%02x:%s)(%08x)\n",
          MinorFunction, 
          PnpMinorFunctionName(MinorFunction),
          status);

    return status;
}

static FORCEINLINE NTSTATUS
__PdoSetDevicePower(
    IN  PXENBUS_PDO     Pdo,
    IN  PIRP            Irp
    )
{
    PIO_STACK_LOCATION  StackLocation;
    DEVICE_POWER_STATE  DeviceState;
    POWER_ACTION        PowerAction;

    StackLocation = IoGetCurrentIrpStackLocation(Irp);
    DeviceState = StackLocation->Parameters.Power.State.DeviceState;
    PowerAction = StackLocation->Parameters.Power.ShutdownType;

    Trace("====> (%s:%s)\n",
          PowerDeviceStateName(DeviceState), 
          PowerActionName(PowerAction));

    ASSERT3U(PowerAction, <, PowerActionShutdown);

    if (__PdoGetDevicePowerState(Pdo) > DeviceState) {
        Trace("%s: POWERING UP: %s -> %s\n",
              __PdoGetName(Pdo),
              PowerDeviceStateName(__PdoGetDevicePowerState(Pdo)),
              PowerDeviceStateName(DeviceState));

        ASSERT3U(DeviceState, ==, PowerDeviceD0);
        PdoD3ToD0(Pdo);
    } else if (__PdoGetDevicePowerState(Pdo) < DeviceState) {
        Trace("%s: POWERING DOWN: %s -> %s\n",
              __PdoGetName(Pdo),
              PowerDeviceStateName(__PdoGetDevicePowerState(Pdo)),
              PowerDeviceStateName(DeviceState));

        ASSERT3U(DeviceState, ==, PowerDeviceD3);
        PdoD0ToD3(Pdo);
    }

    Irp->IoStatus.Status = STATUS_SUCCESS;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    Trace("<==== (%s:%s)\n",
          PowerDeviceStateName(DeviceState), 
          PowerActionName(PowerAction));

    return STATUS_SUCCESS;
}

static NTSTATUS
PdoDevicePower(
    IN  PXENBUS_THREAD  Self,
    IN  PVOID           Context
    )
{
    PXENBUS_PDO         Pdo = Context;
    PKEVENT             Event;

    Event = ThreadGetEvent(Self);

    for (;;) {
        PIRP    Irp;

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

        (VOID) __PdoSetDevicePower(Pdo, Irp);
    }

    return STATUS_SUCCESS;
}

static FORCEINLINE NTSTATUS
__PdoSetSystemPower(
    IN  PXENBUS_PDO     Pdo,
    IN  PIRP            Irp
    )
{
    PIO_STACK_LOCATION  StackLocation;
    SYSTEM_POWER_STATE  SystemState;
    POWER_ACTION        PowerAction;

    StackLocation = IoGetCurrentIrpStackLocation(Irp);
    SystemState = StackLocation->Parameters.Power.State.SystemState;
    PowerAction = StackLocation->Parameters.Power.ShutdownType;

    Trace("====> (%s:%s)\n",
          PowerSystemStateName(SystemState), 
          PowerActionName(PowerAction));

    ASSERT3U(PowerAction, <, PowerActionShutdown);

    if (__PdoGetSystemPowerState(Pdo) > SystemState) {
        if (SystemState < PowerSystemHibernate &&
            __PdoGetSystemPowerState(Pdo) >= PowerSystemHibernate) {
            __PdoSetSystemPowerState(Pdo, PowerSystemHibernate);
            PdoS4ToS3(Pdo);
        }

        Trace("%s: POWERING UP: %s -> %s\n",
              __PdoGetName(Pdo),
              PowerSystemStateName(__PdoGetSystemPowerState(Pdo)),
              PowerSystemStateName(SystemState));

    } else if (__PdoGetSystemPowerState(Pdo) < SystemState) {
        Trace("%s: POWERING DOWN: %s -> %s\n",
              __PdoGetName(Pdo),
              PowerSystemStateName(__PdoGetSystemPowerState(Pdo)),
              PowerSystemStateName(SystemState));

        if (SystemState >= PowerSystemHibernate &&
            __PdoGetSystemPowerState(Pdo) < PowerSystemHibernate) {
            __PdoSetSystemPowerState(Pdo, PowerSystemSleeping3);
            PdoS3ToS4(Pdo);
        }
    }

    __PdoSetSystemPowerState(Pdo, SystemState);

    Irp->IoStatus.Status = STATUS_SUCCESS;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    Trace("<==== (%s:%s)\n",
          PowerSystemStateName(SystemState), 
          PowerActionName(PowerAction));

    return STATUS_SUCCESS;
}

static NTSTATUS
PdoSystemPower(
    IN  PXENBUS_THREAD  Self,
    IN  PVOID           Context
    )
{
    PXENBUS_PDO         Pdo = Context;
    PKEVENT             Event;

    Event = ThreadGetEvent(Self);

    for (;;) {
        PIRP    Irp;

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

        (VOID) __PdoSetSystemPower(Pdo, Irp);
    }

    return STATUS_SUCCESS;
}

static DECLSPEC_NOINLINE NTSTATUS
PdoSetPower(
    IN  PXENBUS_PDO     Pdo,
    IN  PIRP            Irp
    )
{
    PIO_STACK_LOCATION  StackLocation;
    POWER_STATE_TYPE    PowerType;
    POWER_ACTION        PowerAction;
    NTSTATUS            status;
    
    StackLocation = IoGetCurrentIrpStackLocation(Irp);
    PowerType = StackLocation->Parameters.Power.Type;
    PowerAction = StackLocation->Parameters.Power.ShutdownType;

    if (PowerAction >= PowerActionShutdown) {
        Irp->IoStatus.Status = STATUS_SUCCESS;
        
        status = Irp->IoStatus.Status;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);

        goto done;
    }

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
        status = Irp->IoStatus.Status;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        break;
    }

done:
    return status;
}

static DECLSPEC_NOINLINE NTSTATUS
PdoQueryPower(
    IN  PXENBUS_PDO Pdo,
    IN  PIRP        Irp
    )
{
    NTSTATUS        status;

    UNREFERENCED_PARAMETER(Pdo);

    status = STATUS_SUCCESS;

    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

static DECLSPEC_NOINLINE NTSTATUS
PdoDispatchPower(
    IN  PXENBUS_PDO     Pdo,
    IN  PIRP            Irp
    )
{
    PIO_STACK_LOCATION  StackLocation;
    UCHAR               MinorFunction;
    NTSTATUS            status;

    StackLocation = IoGetCurrentIrpStackLocation(Irp);
    MinorFunction = StackLocation->MinorFunction;

    switch (StackLocation->MinorFunction) {
    case IRP_MN_SET_POWER:
        status = PdoSetPower(Pdo, Irp);
        break;

    case IRP_MN_QUERY_POWER:
        status = PdoQueryPower(Pdo, Irp);
        break;

    default:
        status = Irp->IoStatus.Status;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        break;
    }

    return status;
}

static DECLSPEC_NOINLINE NTSTATUS
PdoDispatchDefault(
    IN  PXENBUS_PDO Pdo,
    IN  PIRP        Irp
    )
{
    NTSTATUS        status;

    UNREFERENCED_PARAMETER(Pdo);

    status = Irp->IoStatus.Status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

NTSTATUS
PdoDispatch(
    IN  PXENBUS_PDO     Pdo,
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

VOID
PdoResume(
    IN  PXENBUS_PDO     Pdo
    )
{
    UNREFERENCED_PARAMETER(Pdo);
}

VOID
PdoSuspend(
    IN  PXENBUS_PDO     Pdo
    )
{
    UNREFERENCED_PARAMETER(Pdo);
}

NTSTATUS
PdoCreate(
    IN  PXENBUS_FDO     Fdo,
    IN  PANSI_STRING    Name
    )
{
    PDEVICE_OBJECT      PhysicalDeviceObject;
    PXENBUS_DX          Dx;
    PXENBUS_PDO         Pdo;
    NTSTATUS            status;

#pragma prefast(suppress:28197) // Possibly leaking memory 'PhysicalDeviceObject'
    status = IoCreateDevice(DriverObject,
                            sizeof(XENBUS_DX),
                            NULL,
                            FILE_DEVICE_UNKNOWN,
                            FILE_DEVICE_SECURE_OPEN | FILE_AUTOGENERATED_DEVICE_NAME,
                            FALSE,
                            &PhysicalDeviceObject);
    if (!NT_SUCCESS(status))
        goto fail1;

    Dx = (PXENBUS_DX)PhysicalDeviceObject->DeviceExtension;
    RtlZeroMemory(Dx, sizeof (XENBUS_DX));

    Dx->Type = PHYSICAL_DEVICE_OBJECT;
    Dx->DeviceObject = PhysicalDeviceObject;
    Dx->DevicePnpState = Present;

    Dx->SystemPowerState = PowerSystemShutdown;
    Dx->DevicePowerState = PowerDeviceD3;

    Pdo = __PdoAllocate(sizeof (XENBUS_PDO));

    status = STATUS_NO_MEMORY;
    if (Pdo == NULL)
        goto fail2;

    Pdo->Dx = Dx;

    status = BusInitialize(Pdo, &Pdo->BusInterface);
    if (!NT_SUCCESS(status))
        goto fail3;

    status = ThreadCreate(PdoSystemPower, Pdo, &Pdo->SystemPowerThread);
    if (!NT_SUCCESS(status))
        goto fail4;

    status = ThreadCreate(PdoDevicePower, Pdo, &Pdo->DevicePowerThread);
    if (!NT_SUCCESS(status))
        goto fail5;

    __PdoSetName(Pdo, Name);
    __PdoSetRevision(Pdo, Name);

    Info("%p (XENBUS\\CLASS_%s&REV_%02X#_)\n",
         PhysicalDeviceObject,
         __PdoGetName(Pdo),
         __PdoGetRevision(Pdo));

    Dx->Pdo = Pdo;
    PhysicalDeviceObject->Flags &= ~DO_DEVICE_INITIALIZING;

    __PdoLink(Pdo, Fdo);

    return STATUS_SUCCESS;

fail5:
    Error("fail5\n");

    ThreadAlert(Pdo->SystemPowerThread);
    ThreadJoin(Pdo->SystemPowerThread);
    Pdo->SystemPowerThread = NULL;

fail4:
    Error("fail4\n");

    BusTeardown(&Pdo->BusInterface);

fail3:
    Error("fail3\n");

    Pdo->Dx = NULL;

    ASSERT(IsZeroMemory(Pdo, sizeof (XENBUS_PDO)));
    __PdoFree(Pdo);

fail2:
    Error("fail2\n");

    IoDeleteDevice(PhysicalDeviceObject);

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

VOID
PdoDestroy(
    IN  PXENBUS_PDO Pdo
    )
{
    PXENBUS_DX      Dx = Pdo->Dx;
    PDEVICE_OBJECT  PhysicalDeviceObject = Dx->DeviceObject;

    ASSERT3U(__PdoGetDevicePnpState(Pdo), ==, Deleted);

    ASSERT(__PdoIsMissing(Pdo));
    Pdo->Missing = FALSE;

    __PdoUnlink(Pdo);

    Info("%p (XENBUS\\CLASS_%s&REV_%02X) (%s)\n",
         PhysicalDeviceObject,
         __PdoGetName(Pdo),
         __PdoGetRevision(Pdo),
         Pdo->Reason);
    Pdo->Reason = NULL;

    Dx->Pdo = NULL;

    Pdo->Revision = 0;

    ThreadAlert(Pdo->DevicePowerThread);
    ThreadJoin(Pdo->DevicePowerThread);
    Pdo->DevicePowerThread = NULL;
    
    ThreadAlert(Pdo->SystemPowerThread);
    ThreadJoin(Pdo->SystemPowerThread);
    Pdo->SystemPowerThread = NULL;

    BusTeardown(&Pdo->BusInterface);

    Pdo->Dx = NULL;

    ASSERT(IsZeroMemory(Pdo, sizeof (XENBUS_PDO)));
    __PdoFree(Pdo);

    IoDeleteDevice(PhysicalDeviceObject);
}
