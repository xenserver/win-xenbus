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

#ifndef  _XEN_HYPERCALL_H
#define  _XEN_HYPERCALL_H

#include <ntddk.h>
#include <xen-types.h>
#include <xen-warnings.h>
#include <xen/xen.h>

extern NTSTATUS
HypercallInitialize(
    VOID
    );

extern ULONG_PTR
__Hypercall2(
    ULONG       Ordinal,
    ULONG_PTR   Argument1,
    ULONG_PTR   Argument2
    );

#define Hypercall2(_Type, _Name, _Argument1, _Argument2) \
        ((_Type)__Hypercall2(__HYPERVISOR_##_Name,       \
                             (ULONG_PTR)(_Argument1),    \
                             (ULONG_PTR)(_Argument2)))

extern ULONG_PTR
__Hypercall3(
    ULONG       Ordinal,
    ULONG_PTR   Argument1,
    ULONG_PTR   Argument2,
    ULONG_PTR   Argument3
    );

#define Hypercall3(_Type, _Name, _Argument1, _Argument2, _Argument3)    \
        ((_Type)__Hypercall3(__HYPERVISOR_##_Name,                      \
                             (ULONG_PTR)(_Argument1),                   \
                             (ULONG_PTR)(_Argument2),                   \
                             (ULONG_PTR)(_Argument3)))

extern VOID
HypercallTeardown(
    VOID
    );

#endif  // _XEN_HYPERCALL_H
