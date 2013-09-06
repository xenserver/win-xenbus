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
};

static XENFILT_UNPLUG_CONTEXT   UnplugContext;

static FORCEINLINE NTSTATUS
__UnplugPreamble(
    IN  PXENFILT_UNPLUG_CONTEXT Context
    )
{
    KIRQL                       Irql;
    USHORT                      Magic;
    UCHAR                       Version;
    NTSTATUS                    status;

    AcquireHighLock(&Context->Lock, &Irql);

    // See docs/misc/hvm-emulated-unplug.markdown for details of the
    // protocol in use here

    Magic = READ_PORT_USHORT((PUSHORT)0x10);

    status = STATUS_NOT_SUPPORTED;
    if (Magic != 0x49d2)
        goto fail1;

    Version = READ_PORT_UCHAR((PUCHAR)0x12);
    if (Version != 0) {
        WRITE_PORT_USHORT((PUSHORT)0x12, 0x0001);
        WRITE_PORT_ULONG((PULONG)0x10, 
                         (MAJOR_VERSION << 16) |
                         (MINOR_VERSION << 8) |
                         MICRO_VERSION);

        Magic = READ_PORT_USHORT((PUSHORT)0x10);
        if (Magic == 0xd249)
            Context->BlackListed = TRUE;
    }

    ReleaseHighLock(&Context->Lock, Irql);

    Info("DONE %s\n", (Context->BlackListed) ? "[BLACKLISTED]" : "");

    return STATUS_SUCCESS;

fail1:
    ReleaseHighLock(&Context->Lock, Irql);

    Error("fail1 (%08x)\n", status);

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
    PANSI_STRING                ServiceName;
    CHAR                        ServiceKeyName[sizeof (SERVICES_KEY "\\XXXXXXXX")];
    HANDLE                      ServiceKey;
    ULONG                       Count;
    KIRQL                       Irql;
    NTSTATUS                    status;

    UnplugKey = DriverGetUnplugKey();

    ServiceName = NULL;
    ServiceKey = NULL;

    status = RegistryQuerySzValue(UnplugKey,
                                  "DISKS",
                                  &ServiceName);
    if (!NT_SUCCESS(status)) {
        Info("NO PV SERVICE\n");
        goto done;
    }

    status = RtlStringCbPrintfA(ServiceKeyName,
                                sizeof (ServiceKeyName),
                                SERVICES_KEY "\\%Z",
                                ServiceName);
    ASSERT(NT_SUCCESS(status));

    status = RegistryOpenSubKey(NULL,
                                ServiceKeyName,
                                KEY_READ,
                                &ServiceKey);
    if (!NT_SUCCESS(status)) {
        Info("%Z: NO SERVICE KEY\n", ServiceName);
        goto done;
    }

    status = RegistryQueryDwordValue(ServiceKey,
                                     "Count",
                                     &Count);
    if (!NT_SUCCESS(status))
        Count = 0;

    if (Count == 0) {
        Info("%Z: NO SERVICE INSTANCES\n", ServiceName);
        goto done;
    }

    AcquireHighLock(&Context->Lock, &Irql);

    ASSERT(!Context->UnpluggedDisks);

    WRITE_PORT_USHORT((PUSHORT)0x10, 0x0001);
    Context->UnpluggedDisks = TRUE;

    ReleaseHighLock(&Context->Lock, Irql);

    Info("DONE\n");

done:
    if (ServiceKey != NULL)
        RegistryCloseKey(ServiceKey);

    if (ServiceName != NULL)
        RegistryFreeSzValue(ServiceName);
}

static FORCEINLINE VOID
__UnplugNics(
    IN  PXENFILT_UNPLUG_CONTEXT Context
    )
{
    HANDLE                      UnplugKey;
    PANSI_STRING                ServiceName;
    CHAR                        ServiceKeyName[sizeof (SERVICES_KEY "\\XXXXXXXX")];
    HANDLE                      ServiceKey;
    ULONG                       Count;
    KIRQL                       Irql;
    NTSTATUS                    status;

    UnplugKey = DriverGetUnplugKey();

    ServiceName = NULL;
    ServiceKey = NULL;

    status = RegistryQuerySzValue(UnplugKey,
                                  "NICS",
                                  &ServiceName);
    if (!NT_SUCCESS(status)) {
        Info("NO PV SERVICE\n");
        goto done;
    }

    status = RtlStringCbPrintfA(ServiceKeyName,
                                sizeof (ServiceKeyName),
                                SERVICES_KEY "\\%Z",
                                ServiceName);
    ASSERT(NT_SUCCESS(status));

    status = RegistryOpenSubKey(NULL,
                                ServiceKeyName,
                                KEY_READ,
                                &ServiceKey);
    if (!NT_SUCCESS(status)) {
        Info("%Z: NO SERVICE KEY\n", ServiceName);
        goto done;
    }

    status = RegistryQueryDwordValue(ServiceKey,
                                     "Count",
                                     &Count);
    if (!NT_SUCCESS(status))
        Count = 0;

    if (Count == 0) {
        Info("%Z: NO SERVICE INSTANCES\n", ServiceName);
        goto done;
    }

    AcquireHighLock(&Context->Lock, &Irql);

    ASSERT(!Context->UnpluggedNics);

    WRITE_PORT_USHORT((PUSHORT)0x10, 0x0002);
    Context->UnpluggedNics = TRUE;

    ReleaseHighLock(&Context->Lock, Irql);

    Info("DONE\n");

done:
    if (ServiceKey != NULL)
        RegistryCloseKey(ServiceKey);

    if (ServiceName != NULL)
        RegistryFreeSzValue(ServiceName);
}

static VOID
UnplugReplay(
    IN  PXENFILT_UNPLUG_CONTEXT Context
    )
{
    KIRQL                       Irql;

    AcquireHighLock(&Context->Lock, &Irql);

    if (Context->UnpluggedDisks)
        WRITE_PORT_USHORT((PUSHORT)0x10, 0x0001);

    if (Context->UnpluggedNics)
        WRITE_PORT_USHORT((PUSHORT)0x10, 0x0002);
    
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

    status = __UnplugPreamble(Context);
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

    RtlZeroMemory(&Context->Lock, sizeof (HIGH_LOCK));

    ASSERT(IsZeroMemory(Context, sizeof (XENFILT_UNPLUG_CONTEXT)));

    Info("DONE\n");

done:
    RtlZeroMemory(Interface, sizeof (XENFILT_UNPLUG_INTERFACE));

    Trace("<====\n");
}

#pragma warning(pop)
