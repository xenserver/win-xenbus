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
#include <stdlib.h>
#include <stdarg.h>
#include <xen.h>
#include <util.h>
#include <version.h>

#include "driver.h"
#include "high.h"
#include "registry.h"
#include "unplug.h"
#include "dbg_print.h"
#include "assert.h"

#pragma warning(push)
#pragma warning(disable:28138)  // Constant argument should be variable

struct _XENFILT_UNPLUG_CONTEXT {
    LONG        References;
    HIGH_LOCK   Lock;
    BOOLEAN     BlackListed;
    BOOLEAN     UnpluggedDisks;
    BOOLEAN     UnpluggedNics;
    BOOLEAN     BootEmulated;
};

static XENFILT_UNPLUG_CONTEXT   UnplugContext;

static FORCEINLINE VOID
__UnplugGetFlags(
    IN  PXENFILT_UNPLUG_CONTEXT Context
    )
{
    HANDLE                      Key;
    DWORD                       Value;
    NTSTATUS                    status;

    Context->BootEmulated = FALSE;

    Key = DriverGetParametersKey();
    status = RegistryQueryDwordValue(Key,
                                     "BootEmulated",
                                     &Value);
    if (NT_SUCCESS(status)) {
        LogPrintf(LOG_LEVEL_WARNING,
                  "UNPLUG: BOOT_EMULATED %d\n",
                  Value);

        Context->BootEmulated = (Value == 1) ? TRUE : FALSE;
    }
}

static FORCEINLINE VOID
__UnplugDisksLocked(
    IN  PXENFILT_UNPLUG_CONTEXT Context
    )
{
    if (Context->BootEmulated) {
        WRITE_PORT_USHORT((PUSHORT)0x10, 0x0004);

        LogPrintf(LOG_LEVEL_WARNING, "UNPLUG: AUX DISKS\n");
    } else {
        WRITE_PORT_USHORT((PUSHORT)0x10, 0x0001);

        LogPrintf(LOG_LEVEL_WARNING, "UNPLUG: DISKS\n");
    }
}

static FORCEINLINE VOID
__UnplugNicsLocked(
    )
{
    WRITE_PORT_USHORT((PUSHORT)0x10, 0x0002);

    LogPrintf(LOG_LEVEL_WARNING, "UNPLUG: NICS\n");
}

static FORCEINLINE NTSTATUS
__UnplugPreamble(
    IN  PXENFILT_UNPLUG_CONTEXT Context,
    IN  BOOLEAN                 Locked
    )
{
    KIRQL                       Irql = PASSIVE_LEVEL;
    USHORT                      Magic;
    UCHAR                       Version;
    NTSTATUS                    status;

    if (!Locked)
        AcquireHighLock(&Context->Lock, &Irql);

    // See docs/misc/hvm-emulated-unplug.markdown for details of the
    // protocol in use here

    Magic = READ_PORT_USHORT((PUSHORT)0x10);
    
    if (Magic == 0xd249) {
        Context->BlackListed = TRUE;
        goto done;
    }

    status = STATUS_NOT_SUPPORTED;
    if (Magic != 0x49d2)
        goto fail1;

    Version = READ_PORT_UCHAR((PUCHAR)0x12);
    if (Version != 0) {
        WRITE_PORT_USHORT((PUSHORT)0x12, 0xFFFF);   // FIXME
        WRITE_PORT_ULONG((PULONG)0x10, 
                         (MAJOR_VERSION << 16) |
                         (MINOR_VERSION << 8) |
                         MICRO_VERSION);

        Magic = READ_PORT_USHORT((PUSHORT)0x10);
        if (Magic == 0xd249)
            Context->BlackListed = TRUE;
    }

done:
    LogPrintf(LOG_LEVEL_WARNING,
              "UNPLUG: PRE-AMBLE (DRIVERS %s)\n",
              (Context->BlackListed) ? "BLACKLISTED" : "NOT BLACKLISTED");

    if (!Locked)
        ReleaseHighLock(&Context->Lock, Irql);

    return STATUS_SUCCESS;

fail1:
    Error("fail1 (%08x)\n", status);

    if (!Locked)
        ReleaseHighLock(&Context->Lock, Irql);

    return status;
}

#define HKEY_LOCAL_MACHINE  "\\Registry\\Machine"
#define SERVICES_KEY        HKEY_LOCAL_MACHINE "\\SYSTEM\\CurrentControlSet\\Services"

static FORCEINLINE VOID
__UnplugDisks(
    IN  PXENFILT_UNPLUG_CONTEXT Context
    )
{
    HANDLE                      UnplugKey;
    PANSI_STRING                ServiceNames;
    ULONG                       Index;
    HANDLE                      ServiceKey;
    KIRQL                       Irql;
    NTSTATUS                    status;

    UnplugKey = DriverGetUnplugKey();

    ServiceKey = NULL;
    ServiceNames = NULL;

    status = RegistryQuerySzValue(UnplugKey,
                                  "DISKS",
                                  &ServiceNames);
    if (!NT_SUCCESS(status))
        goto done;

    for (Index = 0; ServiceNames[Index].Buffer != NULL; Index++) {
        PANSI_STRING    ServiceName = &ServiceNames[Index];
        CHAR            ServiceKeyName[sizeof (SERVICES_KEY "\\XXXXXXXX")];
        ULONG           Count;

        status = RtlStringCbPrintfA(ServiceKeyName,
                                    sizeof (ServiceKeyName),
                                    SERVICES_KEY "\\%Z",
                                    ServiceName);
        ASSERT(NT_SUCCESS(status));

        status = RegistryOpenSubKey(NULL,
                                    ServiceKeyName,
                                    KEY_READ,
                                    &ServiceKey);
        if (!NT_SUCCESS(status))
            goto done;

        status = RegistryQueryDwordValue(ServiceKey,
                                         "Count",
                                         &Count);
        if (!NT_SUCCESS(status))
            goto done;

        if (Count == 0)
            goto done;

        RegistryCloseKey(ServiceKey);
        ServiceKey = NULL;
    }

    AcquireHighLock(&Context->Lock, &Irql);

    ASSERT(!Context->UnpluggedDisks);

    __UnplugDisksLocked(Context);

    Context->UnpluggedDisks = TRUE;

    ReleaseHighLock(&Context->Lock, Irql);

done:
    if (ServiceKey != NULL)
        RegistryCloseKey(ServiceKey);

    if (ServiceNames != NULL)
        RegistryFreeSzValue(ServiceNames);
}

static FORCEINLINE VOID
__UnplugNics(
    IN  PXENFILT_UNPLUG_CONTEXT Context
    )
{
    HANDLE                      UnplugKey;
    PANSI_STRING                ServiceNames;
    ULONG                       Index;
    HANDLE                      ServiceKey;
    KIRQL                       Irql;
    NTSTATUS                    status;

    UnplugKey = DriverGetUnplugKey();

    ServiceKey = NULL;
    ServiceNames = NULL;

    status = RegistryQuerySzValue(UnplugKey,
                                  "NICS",
                                  &ServiceNames);
    if (!NT_SUCCESS(status))
        goto done;

    for (Index = 0; ServiceNames[Index].Buffer != NULL; Index++) {
        PANSI_STRING    ServiceName = &ServiceNames[Index];
        CHAR            ServiceKeyName[sizeof (SERVICES_KEY "\\XXXXXXXX")];
        ULONG           Count;

        status = RtlStringCbPrintfA(ServiceKeyName,
                                    sizeof (ServiceKeyName),
                                    SERVICES_KEY "\\%Z",
                                    ServiceName);
        ASSERT(NT_SUCCESS(status));

        status = RegistryOpenSubKey(NULL,
                                    ServiceKeyName,
                                    KEY_READ,
                                    &ServiceKey);
        if (!NT_SUCCESS(status))
            goto done;

        status = RegistryQueryDwordValue(ServiceKey,
                                         "Count",
                                         &Count);
        if (!NT_SUCCESS(status))
            goto done;

        if (Count == 0)
            goto done;

        RegistryCloseKey(ServiceKey);
        ServiceKey = NULL;
    }

    AcquireHighLock(&Context->Lock, &Irql);

    ASSERT(!Context->UnpluggedNics);

    __UnplugNicsLocked();

    Context->UnpluggedNics = TRUE;

    ReleaseHighLock(&Context->Lock, Irql);

done:
    if (ServiceKey != NULL)
        RegistryCloseKey(ServiceKey);

    if (ServiceNames != NULL)
        RegistryFreeSzValue(ServiceNames);
}

static VOID
UnplugReplay(
    IN  PXENFILT_UNPLUG_CONTEXT Context
    )
{
    KIRQL                       Irql;
    NTSTATUS                    status;

    AcquireHighLock(&Context->Lock, &Irql);

    status = __UnplugPreamble(Context, TRUE);
    ASSERT(NT_SUCCESS(status));

    if (Context->UnpluggedDisks) {
        __UnplugDisksLocked(Context);
    }

    if (Context->UnpluggedNics) {
        __UnplugNicsLocked();
    }
    
    ReleaseHighLock(&Context->Lock, Irql);
}

static VOID
UnplugAcquire(
    IN  PXENFILT_UNPLUG_CONTEXT Context
    )
{
    InterlockedIncrement(&Context->References);
}

static VOID
UnplugRelease(
    IN  PXENFILT_UNPLUG_CONTEXT Context
    )
{
    ASSERT(Context->References != 0);
    InterlockedDecrement(&Context->References);
}

#define UNPLUG_OPERATION(_Type, _Name, _Arguments) \
        Unplug ## _Name,

static XENFILT_UNPLUG_OPERATIONS  Operations = {
    DEFINE_UNPLUG_OPERATIONS
};

#undef UNPLUG_OPERATION

NTSTATUS
UnplugInitialize(
    OUT PXENFILT_UNPLUG_INTERFACE   Interface
    )
{
    PXENFILT_UNPLUG_CONTEXT         Context;
    ULONG                           References;
    NTSTATUS                        status;

    Trace("====>\n");

    Context = &UnplugContext;

    References = InterlockedIncrement(&Context->References);
    if (References > 1)
        goto done;

    InitializeHighLock(&Context->Lock);

    __UnplugGetFlags(Context);

    status = __UnplugPreamble(Context, FALSE);
    if (!NT_SUCCESS(status))
        goto fail1;

    __UnplugDisks(Context);
    __UnplugNics(Context);

done:
    Interface->Context = Context;
    Interface->Operations = &Operations;

    Trace("<====\n");

    return STATUS_SUCCESS;

fail1:
    Error("fail1 (%08x)\n", status);

    RtlZeroMemory(&Context->Lock, sizeof (HIGH_LOCK));
    Context->BootEmulated = FALSE;

    (VOID) InterlockedDecrement(&Context->References);

    return status;
}

VOID
UnplugTeardown(
    IN OUT  PXENFILT_UNPLUG_INTERFACE Interface
    )
{
    PXENFILT_UNPLUG_CONTEXT           Context = Interface->Context;
    ULONG                             References;

    ASSERT3P(Context, ==, &UnplugContext);

    Trace("====>\n");

    References = InterlockedDecrement(&Context->References);
    if (References > 0)
        goto done;

    Context->BlackListed = FALSE;
    Context->BootEmulated = FALSE;

    RtlZeroMemory(&Context->Lock, sizeof (HIGH_LOCK));

    ASSERT(IsZeroMemory(Context, sizeof (XENFILT_UNPLUG_CONTEXT)));

done:
    RtlZeroMemory(Interface, sizeof (XENFILT_UNPLUG_INTERFACE));

    Trace("<====\n");
}

#pragma warning(pop)
