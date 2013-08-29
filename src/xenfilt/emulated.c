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

#include "registry.h"
#include "emulated.h"
#include "dbg_print.h"
#include "assert.h"

struct _XENFILT_EMULATED_CONTEXT {
    LONG                References;
};

#define EMULATED_TAG    'LUME'

static FORCEINLINE PVOID
__EmulatedAllocate(
    IN  ULONG   Length
    )
{
    return __AllocateNonPagedPoolWithTag(Length, EMULATED_TAG);
}

static FORCEINLINE VOID
__EmulatedFree(
    IN  PVOID   Buffer
    )
{
    __FreePoolWithTag(Buffer, EMULATED_TAG);
}

static BOOLEAN
EmulatedIsDevicePresent(
    IN  PXENFILT_EMULATED_CONTEXT   Context,
    IN  PCHAR                       DeviceID,
    IN  PCHAR                       InstanceID
    )
{
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(DeviceID);
    UNREFERENCED_PARAMETER(InstanceID);

    return TRUE;
}

static BOOLEAN
EmulatedIsDiskPresent(
    IN  PXENFILT_EMULATED_CONTEXT   Context,
    IN  ULONG                       Controller,
    IN  ULONG                       Target,
    IN  ULONG                       Lun
    )
{
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(Controller);
    UNREFERENCED_PARAMETER(Target);
    UNREFERENCED_PARAMETER(Lun);

    return TRUE;
}

static VOID
EmulatedAcquire(
    IN  PXENFILT_EMULATED_CONTEXT   Context
    )
{
    InterlockedIncrement(&Context->References);
}

static VOID
EmulatedRelease(
    IN  PXENFILT_EMULATED_CONTEXT   Context
    )
{
    ASSERT(Context->References != 0);
    InterlockedDecrement(&Context->References);
}

#define EMULATED_OPERATION(_Type, _Name, _Arguments) \
        Emulated ## _Name,

static XENFILT_EMULATED_OPERATIONS  Operations = {
    DEFINE_EMULATED_OPERATIONS
};

#undef EMULATED_OPERATION

NTSTATUS
EmulatedInitialize(
    OUT PXENFILT_EMULATED_INTERFACE Interface
    )
{
    PXENFILT_EMULATED_CONTEXT       Context;
    NTSTATUS                        status;

    Trace("====>\n");

    Context = __EmulatedAllocate(sizeof (XENFILT_EMULATED_CONTEXT));

    status = STATUS_NO_MEMORY;
    if (Context == NULL)
        goto fail1;

    Interface->Context = Context;
    Interface->Operations = &Operations;

    Trace("<====\n");

    return STATUS_SUCCESS;

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

VOID
EmulatedTeardown(
    IN OUT  PXENFILT_EMULATED_INTERFACE Interface
    )
{
    PXENFILT_EMULATED_CONTEXT           Context = Interface->Context;

    Trace("====>\n");

    ASSERT(IsZeroMemory(Context, sizeof (XENFILT_EMULATED_CONTEXT)));
    __EmulatedFree(Context);

    RtlZeroMemory(Interface, sizeof (XENFILT_EMULATED_INTERFACE));

    Trace("<====\n");
}
