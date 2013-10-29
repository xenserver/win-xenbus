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
#include <xen.h>
#include <util.h>

#include "hypercall.h"
#include "dbg_print.h"
#include "assert.h"

#define MAXIMUM_HYPERCALL_PFN_COUNT 2

#pragma code_seg("hypercall")
__declspec(allocate("hypercall"))
static UCHAR        __Section[(MAXIMUM_HYPERCALL_PFN_COUNT + 1) * PAGE_SIZE];

static ULONG        XenBaseLeaf = 0x40000000;

static USHORT       XenMajorVersion;
static USHORT       XenMinorVersion;

static PFN_NUMBER   HypercallPfn[MAXIMUM_HYPERCALL_PFN_COUNT];
static ULONG        HypercallPfnCount;

typedef UCHAR           HYPERCALL_GATE[32];
typedef HYPERCALL_GATE  *PHYPERCALL_GATE;

PHYPERCALL_GATE     Hypercall;

NTSTATUS
HypercallInitialize(
    VOID
    )
{
    ULONG       EAX = 'DEAD';
    ULONG       EBX = 'DEAD';
    ULONG       ECX = 'DEAD';
    ULONG       EDX = 'DEAD';
    ULONG       Index;
    ULONG       HypercallMsr;
    NTSTATUS    status;

    status = STATUS_UNSUCCESSFUL;
    for (;;) {
        CHAR    Signature[13] = {0};

        __CpuId(XenBaseLeaf, &EAX, &EBX, &ECX, &EDX);
        *((PULONG)(Signature + 0)) = EBX;
        *((PULONG)(Signature + 4)) = ECX;
        *((PULONG)(Signature + 8)) = EDX;

        if (strcmp(Signature, "XenVMMXenVMM") == 0 &&
            EAX >= XenBaseLeaf + 2)
            break;
            
        XenBaseLeaf += 0x100;
        
        if (XenBaseLeaf > 0x40000100)
            goto fail1;
    }

    __CpuId(XenBaseLeaf + 1, &EAX, NULL, NULL, NULL);
    XenMajorVersion = (USHORT)(EAX >> 16);
    XenMinorVersion = (USHORT)(EAX & 0xFFFF);

    Info("XEN %d.%d\n", XenMajorVersion, XenMinorVersion);
    Info("INTERFACE 0x%08x\n", __XEN_INTERFACE_VERSION__);

    if ((ULONG_PTR)__Section & (PAGE_SIZE - 1))
        Hypercall = (PVOID)(((ULONG_PTR)__Section + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1));
    else
        Hypercall = (PVOID)__Section;

    ASSERT3U(((ULONG_PTR)Hypercall & (PAGE_SIZE - 1)), ==, 0);

    for (Index = 0; Index < MAXIMUM_HYPERCALL_PFN_COUNT; Index++) {
        PHYSICAL_ADDRESS    PhysicalAddress;

        PhysicalAddress = MmGetPhysicalAddress((PUCHAR)Hypercall + (Index << PAGE_SHIFT));
        HypercallPfn[Index] = (PFN_NUMBER)(PhysicalAddress.QuadPart >> PAGE_SHIFT);
    }

    __CpuId(XenBaseLeaf + 2, &EAX, &EBX, NULL, NULL);
    HypercallPfnCount = EAX;
    ASSERT(HypercallPfnCount <= MAXIMUM_HYPERCALL_PFN_COUNT);
    HypercallMsr = EBX;

    for (Index = 0; Index < HypercallPfnCount; Index++) {
        Info("HypercallPfn[%d]: %p\n", Index, (PVOID)HypercallPfn[Index]);
        __writemsr(HypercallMsr, (ULONG64)HypercallPfn[Index] << PAGE_SHIFT);
    }

    return STATUS_SUCCESS;

fail1:
    Error("fail1 (%08x)", status);

    return status;
}

extern uintptr_t __stdcall hypercall_gate_2(uint32_t ord, uintptr_t arg1, uintptr_t arg2);

ULONG_PTR
__Hypercall2(
    ULONG       Ordinal,
    ULONG_PTR   Argument1,
    ULONG_PTR   Argument2
    )
{
    return hypercall_gate_2(Ordinal, Argument1, Argument2);
}

extern uintptr_t __stdcall hypercall_gate_3(uint32_t ord, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3);

ULONG_PTR
__Hypercall3(
    ULONG       Ordinal,
    ULONG_PTR   Argument1,
    ULONG_PTR   Argument2,
    ULONG_PTR   Argument3
    )
{
    return hypercall_gate_3(Ordinal, Argument1, Argument2, Argument3);
}

VOID
HypercallTeardown(
    VOID
    )
{
    ULONG   Index;

    Hypercall = NULL;

    for (Index = 0; Index < MAXIMUM_HYPERCALL_PFN_COUNT; Index++)
        HypercallPfn[Index] = 0;

    HypercallPfnCount = 0;
}
