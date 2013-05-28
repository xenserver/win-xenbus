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

#ifndef _XENBUS_GNTTAB_H
#define _XENBUS_GNTTAB_H

#include <ntddk.h>
#include <xen.h>
#include <gnttab_interface.h>

#include "fdo.h"

struct _XENBUS_GNTTAB_INTERFACE {
    PXENBUS_GNTTAB_OPERATIONS   Operations;
    PXENBUS_GNTTAB_CONTEXT      Context;
};

C_ASSERT(FIELD_OFFSET(XENBUS_GNTTAB_INTERFACE, Operations) == (ULONG_PTR)GNTTAB_OPERATIONS(NULL));
C_ASSERT(FIELD_OFFSET(XENBUS_GNTTAB_INTERFACE, Context) == (ULONG_PTR)GNTTAB_CONTEXT(NULL));

extern NTSTATUS
GnttabInitialize(
    IN  PXENBUS_FDO                 Fdo,
    OUT PXENBUS_GNTTAB_INTERFACE    Interface
    );

extern VOID
GnttabTeardown(
    IN OUT  PXENBUS_GNTTAB_INTERFACE    Interface
    );

#endif  // _XENBUS_GNTTAB_H

