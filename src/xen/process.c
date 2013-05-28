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

#define XEN_API extern

#include <ntddk.h>
#include <xen.h>

#include "process.h"
#include "log.h"
#include "assert.h"

static VOID
ProcessNotify(
    IN  HANDLE          ParentId,
    IN  HANDLE          ProcessId,
    IN  BOOLEAN         Create
    )
{
    static LONG         HAP = -1;
    KIRQL               Irql;
    PHYSICAL_ADDRESS    Address;
    NTSTATUS            status;

    UNREFERENCED_PARAMETER(ParentId);
    UNREFERENCED_PARAMETER(ProcessId);

    if (Create)
        return;

    if (HAP > 0)    // Hardware Assisted Paging
        return;

    // Process destruction callbacks occur within the context of the
    // dying process so just read the current CR3 and notify Xen that
    // it's about to cease pointing at a page table hierarchy.
    // If the hypercall fails with EINVAL the either we're not an HVM
    // domain, which would be pretty miraculous, or HAP is turned on
    // in which case we need not tell Xen about CR3 invalidation.

    KeRaiseIrql(DISPATCH_LEVEL, &Irql);

    Address.QuadPart = __readcr3();
    status = HvmPagetableDying(Address);
    if (!NT_SUCCESS(status) && HAP < 0) {
        HAP = (status == STATUS_INVALID_PARAMETER) ? 1 : 0;

        Info("PAGING MODE: %s\n", (HAP > 0) ? "HAP" : "Shadow");
    }

    KeLowerIrql(Irql);
}

VOID
ProcessTeardown(
    VOID
    )
{
    (VOID) PsSetCreateProcessNotifyRoutine(ProcessNotify, TRUE);
}

NTSTATUS
ProcessInitialize(
    VOID)
{
    NTSTATUS    status;

    status = PsSetCreateProcessNotifyRoutine(ProcessNotify, FALSE);
    if (!NT_SUCCESS(status))
        goto fail1;

    return STATUS_SUCCESS;

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}
