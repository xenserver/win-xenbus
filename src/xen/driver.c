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

#define XEN_API __declspec(dllexport)

#include <ntddk.h>
#include <xen.h>

#include "hypercall.h"
#include "debug.h"
#include "dump.h"
#include "module.h"
#include "process.h"
#include "unplug.h"
#include "system.h"
#include "log.h"
#include "assert.h"
#include "version.h"

extern PULONG   InitSafeBootMode;

XEN_API
const CHAR *
XenVersion = MAJOR_VERSION_STR "." MINOR_VERSION_STR "." MICRO_VERSION_STR;

NTSTATUS
DllInitialize(
    IN  PUNICODE_STRING RegistryPath
    )
{
    NTSTATUS    status;

    UNREFERENCED_PARAMETER(RegistryPath);

    ExInitializeDriverRuntime(DrvRtPoolNxOptIn);

    LogInitialize();

    Info("%s (%s)\n",
         MAJOR_VERSION_STR "." MINOR_VERSION_STR "." MICRO_VERSION_STR "." BUILD_NUMBER_STR,
         DAY_STR "/" MONTH_STR "/" YEAR_STR);

    if (*InitSafeBootMode > 0)
        goto done;

    SystemGetInformation();

    status = HypercallInitialize();
    if (!NT_SUCCESS(status))
        goto fail1;

    status = DebugInitialize();
    if (!NT_SUCCESS(status))
        goto fail2;

    status = DumpInitialize();
    if (!NT_SUCCESS(status))
        goto fail3;

    status = ModuleInitialize();
    if (!NT_SUCCESS(status))
        goto fail4;

    status = ProcessInitialize();
    if (!NT_SUCCESS(status))
        goto fail5;

    UnplugInitialize();

done:
    return STATUS_SUCCESS;

fail5:
    Error("fail5\n");

    ModuleTeardown();

fail4:
    Error("fail4\n");

    DumpTeardown();

fail3:
    Error("fail3\n");

    DebugTeardown();

fail2:
    Error("fail2\n");

    HypercallTeardown();

fail1:
    Error("fail1 (%08x)", status);

    LogTeardown();

    return status;
}

NTSTATUS
DllUnload(
    VOID
    )
{
    Info("%s (%s)\n",
         MAJOR_VERSION_STR "." MINOR_VERSION_STR "." MICRO_VERSION_STR "." BUILD_NUMBER_STR,
         DAY_STR "/" MONTH_STR "/" YEAR_STR);

    if (*InitSafeBootMode > 0)
        goto done;

    UnplugTeardown();

    ProcessTeardown();

    ModuleTeardown();

    DumpTeardown();

    DebugTeardown();

    HypercallTeardown();

done:
    LogTeardown();

    return STATUS_SUCCESS;
}

DRIVER_INITIALIZE   DriverEntry;

NTSTATUS
DriverEntry(
    IN  PDRIVER_OBJECT  DriverObject,
    IN  PUNICODE_STRING RegistryPath
    )
{
    UNREFERENCED_PARAMETER(DriverObject);
    UNREFERENCED_PARAMETER(RegistryPath);

    return STATUS_SUCCESS;
}
