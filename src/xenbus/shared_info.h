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

#ifndef _XENBUS_SHARED_INFO_H
#define _XENBUS_SHARED_INFO_H

#include <ntddk.h>
#include <xen.h>
#include <shared_info_interface.h>

#include "fdo.h"

#define EVTCHN_PER_SELECTOR     (sizeof (ULONG_PTR) * 8)
#define EVTCHN_SELECTOR_COUNT   (RTL_FIELD_SIZE(shared_info_t, evtchn_pending) / sizeof (ULONG_PTR))

struct _XENBUS_SHARED_INFO_INTERFACE {
    PXENBUS_SHARED_INFO_OPERATIONS  Operations;
    PXENBUS_SHARED_INFO_CONTEXT     Context;
};

C_ASSERT(FIELD_OFFSET(XENBUS_SHARED_INFO_INTERFACE, Operations) == (ULONG_PTR)SHARED_INFO_OPERATIONS(NULL));
C_ASSERT(FIELD_OFFSET(XENBUS_SHARED_INFO_INTERFACE, Context) == (ULONG_PTR)SHARED_INFO_CONTEXT(NULL));

extern NTSTATUS
SharedInfoInitialize(
    IN  PXENBUS_FDO                     Fdo,
    OUT PXENBUS_SHARED_INFO_INTERFACE   Interface
    );

extern VOID
SharedInfoTeardown(
    IN OUT  PXENBUS_SHARED_INFO_INTERFACE   Interface
    );

#endif  // _XENBUS_SHARED_INFO_H

