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

//
// This driver responds to the following system start options:
// (These can be set by bcdedit /set loadoptions <string>).
//
// /XEN:BALLOON=OFF
//
// The system balloon defaults to ON and is adjusted via the
// xenstore 'memory/static-max' and 'memory/target' values. If
// this option is present those values are ignored and the
// balloon remains inactive.
//

#include <ntddk.h>
#include <util.h>

#include "registry.h"
#include "fdo.h"
#include "pdo.h"
#include "driver.h"
#include "log.h"
#include "assert.h"
#include "version.h"

extern const CHAR   *XenVersion;
extern const CHAR   *XenPciVersion;

extern PULONG       InitSafeBootMode;

PDRIVER_OBJECT      DriverObject;

HANDLE              DriverServiceKey;

XENBUS_PARAMETERS   DriverParameters;

DRIVER_UNLOAD       DriverUnload;

VOID
DriverUnload(
    IN  PDRIVER_OBJECT  _DriverObject
    )
{
    ASSERT3P(_DriverObject, ==, DriverObject);

    Trace("====>\n");

    Info("%s (%s)\n",
         MAJOR_VERSION_STR "." MINOR_VERSION_STR "." MICRO_VERSION_STR "." BUILD_NUMBER_STR,
         DAY_STR "/" MONTH_STR "/" YEAR_STR);

    if (*InitSafeBootMode > 0)
        goto done;

    RegistryFreeSzValue(DriverParameters.SyntheticClasses);
    RegistryFreeSzValue(DriverParameters.SupportedClasses);

    RegistryCloseKey(DriverServiceKey);

    RegistryTeardown();

done:
    DriverObject = NULL;

    Trace("<====\n");
}

DRIVER_ADD_DEVICE   AddDevice;

NTSTATUS
#pragma prefast(suppress:28152) // Does not clear DO_DEVICE_INITIALIZING
AddDevice(
    IN  PDRIVER_OBJECT  _DriverObject,
    IN  PDEVICE_OBJECT  DeviceObject
    )
{
    NTSTATUS            status;

    ASSERT3P(_DriverObject, ==, DriverObject);

    status = FdoCreate(DeviceObject);
    if (!NT_SUCCESS(status))
        goto fail1;

    return STATUS_SUCCESS;

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

DRIVER_DISPATCH Dispatch;

NTSTATUS 
Dispatch(
    IN PDEVICE_OBJECT   DeviceObject,
    IN PIRP             Irp
    )
{
    PXENBUS_DX          Dx;
    NTSTATUS            status;

    Dx = (PXENBUS_DX)DeviceObject->DeviceExtension;
    ASSERT3P(Dx->DeviceObject, ==, DeviceObject);

    if (Dx->DevicePnpState == Deleted) {
        status = STATUS_NO_SUCH_DEVICE;

        Irp->IoStatus.Status = status;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        goto done;
    }

    status = STATUS_NOT_SUPPORTED;
    switch (Dx->Type) {
    case PHYSICAL_DEVICE_OBJECT: {
        PXENBUS_PDO Pdo = Dx->Pdo;

        status = PdoDispatch(Pdo, Irp);
        break;
    }
    case FUNCTION_DEVICE_OBJECT: {
        PXENBUS_FDO Fdo = Dx->Fdo;

        status = FdoDispatch(Fdo, Irp);
        break;
    }
    default:
        ASSERT(FALSE);
        break;
    }

done:
    return status;
}

DRIVER_INITIALIZE   DriverEntry;

NTSTATUS
DriverEntry(
    IN  PDRIVER_OBJECT  _DriverObject,
    IN  PUNICODE_STRING RegistryPath
    )
{
    HANDLE              ParametersKey;
    PANSI_STRING        Options;
    ULONG               Index;
    NTSTATUS            status;

    ASSERT3P(DriverObject, ==, NULL);

    ExInitializeDriverRuntime(DrvRtPoolNxOptIn);

    Trace("====>\n");

    Info("%s (%s)\n",
         MAJOR_VERSION_STR "." MINOR_VERSION_STR "." MICRO_VERSION_STR "." BUILD_NUMBER_STR,
         DAY_STR "/" MONTH_STR "/" YEAR_STR);

    DriverObject = _DriverObject;
    DriverObject->DriverUnload = DriverUnload;

    if (*InitSafeBootMode > 0)
        goto done;

    status = RegistryInitialize(RegistryPath);
    if (!NT_SUCCESS(status))
        goto fail1;

    status = RegistryOpenServiceKey(KEY_READ, &DriverServiceKey);
    if (!NT_SUCCESS(status))
        goto fail2;

    DriverParameters.SupportedClasses = NULL;
    DriverParameters.SyntheticClasses = NULL;
    DriverParameters.CreatePDOs = 1;
    DriverParameters.InterceptDmaAdapter = 0;

    status = RegistryOpenSubKey(DriverServiceKey, "Parameters", KEY_READ, &ParametersKey);
    if (NT_SUCCESS(status)) {
        PANSI_STRING    SupportedClasses;
        PANSI_STRING    SyntheticClasses;
        ULONG           CreatePDOs;
        ULONG           InterceptDmaAdapter;

        status = RegistryQuerySzValue(ParametersKey,
                                      "SupportedClasses",
                                      &SupportedClasses);
        if (NT_SUCCESS(status))
            DriverParameters.SupportedClasses = SupportedClasses;

        status = RegistryQuerySzValue(ParametersKey,
                                      "SyntheticClasses",
                                      &SyntheticClasses);
        if (NT_SUCCESS(status))
            DriverParameters.SyntheticClasses = SyntheticClasses;

        status = RegistryQueryDwordValue(ParametersKey,
                                         "CreatePDOs",
                                         &CreatePDOs);
        if (NT_SUCCESS(status))
            DriverParameters.CreatePDOs = CreatePDOs;

        status = RegistryQueryDwordValue(ParametersKey,
                                         "InterceptDmaAdapter",
                                         &InterceptDmaAdapter);
        if (NT_SUCCESS(status))
            DriverParameters.InterceptDmaAdapter = InterceptDmaAdapter;

        RegistryCloseKey(ParametersKey);
    }

    DriverParameters.Balloon = 1;

    status = RegistryQuerySystemStartOptions(&Options);
    if (NT_SUCCESS(status)) {
        const CHAR  Key[] = " XEN:BALLOON=";
        PCHAR       Value;

        Trace("Options = '%Z'\n", Options);

        Value = strstr(Options->Buffer, Key);
        if (Value != NULL) {
            Value += sizeof (Key) - 1;

            if (strcmp(Value, "OFF") == 0)
                DriverParameters.Balloon = 0;
        }

        RegistryFreeSzValue(Options);
    }

    DriverObject->DriverExtension->AddDevice = AddDevice;

    for (Index = 0; Index <= IRP_MJ_MAXIMUM_FUNCTION; Index++) {
#pragma prefast(suppress:28169) // No __drv_dispatchType annotation
#pragma prefast(suppress:28168) // No matching __drv_dispatchType annotation for IRP_MJ_CREATE
       DriverObject->MajorFunction[Index] = Dispatch;
    }

done:
    Trace("<====\n");

    return STATUS_SUCCESS;

fail2:
    Error("fail2\n");

    RegistryTeardown();

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}
