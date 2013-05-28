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
#include <ntstrsafe.h>
#include <stdlib.h>
#include <stdarg.h>
#include <xen.h>
#include <util.h>

#include <binding.h>

#include "unplug.h"
#include "log.h"
#include "assert.h"

typedef struct _UNPLUG_CONTEXT {
    LONG        References;
    KSPIN_LOCK  Lock;
} UNPLUG_CONTEXT, *PUNPLUG_CONTEXT;

UNPLUG_CONTEXT  UnplugContext;

static PVOID Port10 = ((PVOID)(ULONG_PTR)0x10);
static PVOID Port11 = ((PVOID)(ULONG_PTR)0x11);
static PVOID Port12 = ((PVOID)(ULONG_PTR)0x12);
static PVOID Port13 = ((PVOID)(ULONG_PTR)0x13);

typedef enum _UNPLUG_TYPE {
    UNPLUG_TYPE_INVALID = 0,
    UNPLUG_TYPE_IDE,
    UNPLUG_TYPE_NIC,
    UNPLUG_TYPE_COUNT
} UNPLUG_TYPE, *PUNPLUG_TYPE;

#define MAXIMUM_UNPLUG_INDEX    ((1 << (sizeof (UCHAR) * 8)) - 1)

static FORCEINLINE USHORT
__UnplugReadMagic(
    VOID
    )
{
    return READ_PORT_USHORT(Port10);
}

#define UNPLUG_MAGIC    0x49D2

static FORCEINLINE UCHAR
__UnplugReadVersion(
    VOID
    )
{
    return READ_PORT_UCHAR(Port12);
}

static FORCEINLINE VOID
__UnplugWriteVersion(
    IN  UCHAR   Version
    )
{
    // Careful here: we must speculatively set the version 2 unplug type to
    // an invalid value. This is because, if the unplug protocol is already set
    // to version 2 then writing the version will actually perform an unplug.
    // However, if the unplug type is invalid nothing will actually disappear!

    WRITE_PORT_UCHAR(Port11, (UCHAR)UNPLUG_TYPE_INVALID);
    WRITE_PORT_UCHAR(Port13, Version);
}

static FORCEINLINE UCHAR
__UnplugGetVersion(
    VOID
    )
{
    UCHAR   Version;

    __UnplugWriteVersion(2);
    Version = __UnplugReadVersion();

    return Version;
}

#define UNPLUG_PRODUCT_ID 3

static FORCEINLINE VOID
__UnplugWriteProductId(
    IN  USHORT  Id
    )
{
    WRITE_PORT_USHORT(Port12, Id);
}

#define UNPLUG_BUILD_NUMBER ((PCI_DEVICE_ID << 8) | PCI_REVISION)

static FORCEINLINE VOID
__UnplugWriteBuildNumber(
    IN  ULONG   BuildNumber
    )
{
    WRITE_PORT_ULONG(Port10, BuildNumber);
}

static FORCEINLINE VOID
__UnplugWriteUnplugCommand(
    IN  UCHAR   Type,
    IN  UCHAR   Index
    )
{
    WRITE_PORT_UCHAR(Port11, Type);
    WRITE_PORT_UCHAR(Port13, Index);
}

static FORCEINLINE NTSTATUS
__UnplugPrepare(
    VOID
    )
{
    USHORT      Magic;
    UCHAR       Version;
    NTSTATUS    status;

    Magic = __UnplugReadMagic();

    status = STATUS_NO_SUCH_DEVICE;
    if (Magic != UNPLUG_MAGIC)
        goto fail1;

    Version = __UnplugGetVersion();

    // We only support version 2 onwards

    status = STATUS_NOT_SUPPORTED;
    if (Version < 2)
        goto fail2;
        
    // Version 1 of the unplug protocol onwards allows for blacklisting of
    // drivers. This is done by modifying the returned magic number if the
    // drivers should not be used.

    __UnplugWriteProductId(UNPLUG_PRODUCT_ID);
    __UnplugWriteBuildNumber(UNPLUG_BUILD_NUMBER);

    Magic = __UnplugReadMagic();

    status = STATUS_INVALID_PARAMETER;
    if (Magic != UNPLUG_MAGIC)
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

XEN_API
NTSTATUS
UnplugReference(
    VOID
    )
{
    ULONG       References;
    KIRQL       Irql;
    NTSTATUS    status;

    Trace("====>\n");

    KeAcquireSpinLock(&UnplugContext.Lock, &Irql);

    References = UnplugContext.References++;

    if (References == 0) {
        status = __UnplugPrepare();
        if (!NT_SUCCESS(status))
            goto fail1;
    }

    KeReleaseSpinLock(&UnplugContext.Lock, Irql);

    Trace("<==== (%u)\n", References);

    return STATUS_SUCCESS;

fail1:
    Error("fail1 (%08x)\n", status);

    ASSERT(UnplugContext.References != 0);
    --UnplugContext.References;

    KeReleaseSpinLock(&UnplugContext.Lock, Irql);

    return status;
}

static BOOLEAN  UnplugEntry[UNPLUG_TYPE_COUNT][MAXIMUM_UNPLUG_INDEX + 1];

XEN_API
VOID
UnplugDereference(
    VOID
    )
{
    ULONG   References;
    KIRQL   Irql;

    Trace("====>\n");

    KeAcquireSpinLock(&UnplugContext.Lock, &Irql);

    References = UnplugContext.References;

    if (References == 0)
        goto done;

    --UnplugContext.References;

    if (References == 0)
        RtlZeroMemory(UnplugEntry, sizeof (BOOLEAN) * UNPLUG_TYPE_COUNT * (MAXIMUM_UNPLUG_INDEX + 1));

done:
    KeReleaseSpinLock(&UnplugContext.Lock, Irql);

    Trace("<==== (%u)\n", References);
}

XEN_API
NTSTATUS
UnplugDevice(
    PCHAR       Class,
    PCHAR       Device
    )
{
    KIRQL       Irql;
    UNPLUG_TYPE Type;
    UCHAR       Index;
    NTSTATUS    status;

    KeAcquireSpinLock(&UnplugContext.Lock, &Irql);

    ASSERT(UnplugContext.References != 0);

    Info("%s %s\n", Class, Device);

    status = STATUS_INVALID_PARAMETER;
    if (strcmp(Class, "VIF") == 0)
        Type = UNPLUG_TYPE_NIC;
    else if (strcmp(Class, "VBD") == 0)
        Type = UNPLUG_TYPE_IDE;
    else
        goto fail1;

    ASSERT3U(Type, <, UNPLUG_TYPE_COUNT);

    Index = (UCHAR)strtol(Device, NULL, 0);
    ASSERT3U(Index, <=, MAXIMUM_UNPLUG_INDEX);

    if (!UnplugEntry[Type][Index]) {
        UnplugEntry[Type][Index] = TRUE;
        __UnplugWriteUnplugCommand(Type, Index);
    }

    KeReleaseSpinLock(&UnplugContext.Lock, Irql);

    return STATUS_SUCCESS;

fail1:
    Error("fail1 (%08x)\n", status);

    KeReleaseSpinLock(&UnplugContext.Lock, Irql);

    return status;
}

XEN_API
VOID
UnplugReplay(
    VOID
    )
{
    KIRQL       Irql;
    UNPLUG_TYPE Type;
    ULONG       Index;
    NTSTATUS    status;

    KeAcquireSpinLock(&UnplugContext.Lock, &Irql);

    if (UnplugContext.References == 0)
        goto done;

    status = __UnplugPrepare();
    ASSERT(NT_SUCCESS(status));

    for (Type = 0; Type < UNPLUG_TYPE_COUNT; Type++) {
        if (Type == UNPLUG_TYPE_INVALID)
            continue;

        for (Index = 0; Index <= MAXIMUM_UNPLUG_INDEX; Index++) {
            if (UnplugEntry[Type][Index])
                __UnplugWriteUnplugCommand(Type, (UCHAR)Index);
        }
    }

done:
    KeReleaseSpinLock(&UnplugContext.Lock, Irql);
}

VOID
UnplugInitialize(
    VOID
    )
{
    ASSERT(IsZeroMemory(&UnplugContext, sizeof (UNPLUG_CONTEXT)));

    KeInitializeSpinLock(&UnplugContext.Lock);
}

VOID
UnplugTeardown(
    VOID
    )
{
    RtlZeroMemory(&UnplugContext.Lock, sizeof (KSPIN_LOCK));

    ASSERT(IsZeroMemory(&UnplugContext, sizeof (UNPLUG_CONTEXT)));
}
