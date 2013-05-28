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
#include "log.h"
#include "assert.h"

static FORCEINLINE LONG_PTR
EventChannelOp(
    IN  ULONG   Command,
    IN  PVOID   Argument
    )
{
    return Hypercall2(LONG_PTR, event_channel_op, Command, Argument);
}

__checkReturn
XEN_API
NTSTATUS
EventChannelSend(
    IN  evtchn_port_t   LocalPort
    )
{
    struct evtchn_send  op;
    LONG_PTR            rc;
    NTSTATUS            status;

    op.port = LocalPort;

    rc = EventChannelOp(EVTCHNOP_send, &op);

    if (rc < 0) {
        ERRNO_TO_STATUS(-rc, status);
        goto fail1;
    }

    return STATUS_SUCCESS;

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

__checkReturn
XEN_API
NTSTATUS
EventChannelAllocateUnbound(
    IN  domid_t                 Domain,
    OUT evtchn_port_t           *LocalPort
    )
{
    struct evtchn_alloc_unbound op;
    LONG_PTR                    rc;
    NTSTATUS                    status;

    op.dom = DOMID_SELF;
    op.remote_dom = Domain;

    rc = EventChannelOp(EVTCHNOP_alloc_unbound, &op);

    if (rc < 0) {
        ERRNO_TO_STATUS(-rc, status);
        goto fail1;
    }

    *LocalPort = op.port;
    
    return STATUS_SUCCESS;

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

__checkReturn
XEN_API
NTSTATUS
EventChannelBindInterDomain(
    IN  domid_t                     RemoteDomain,
    IN  evtchn_port_t               RemotePort,
    OUT evtchn_port_t               *LocalPort
    )
{
    struct evtchn_bind_interdomain  op;
    LONG_PTR                        rc;
    NTSTATUS                        status;

    op.remote_dom = RemoteDomain,
    op.remote_port = RemotePort;

    rc = EventChannelOp(EVTCHNOP_bind_interdomain, &op);

    if (rc < 0) {
        ERRNO_TO_STATUS(-rc, status);
        goto fail1;
    }

    *LocalPort = op.local_port;
    
    return STATUS_SUCCESS;

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

__checkReturn
XEN_API
NTSTATUS
EventChannelBindVirq(
    IN  uint32_t            Virq,
    OUT evtchn_port_t       *LocalPort
    )
{
    struct evtchn_bind_virq op;
    LONG_PTR                rc;
    NTSTATUS                status;

    op.virq = Virq;
    op.vcpu = 0;

    rc = EventChannelOp(EVTCHNOP_bind_virq, &op);

    if (rc < 0) {
        ERRNO_TO_STATUS(-rc, status);
        goto fail1;
    }

    *LocalPort = op.port;
    
    return STATUS_SUCCESS;

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

__checkReturn
XEN_API
NTSTATUS
EventChannelClose(
    IN  evtchn_port_t   LocalPort
    )
{
    struct evtchn_close op;
    LONG_PTR            rc;
    NTSTATUS            status;

    op.port = LocalPort;

    rc = EventChannelOp(EVTCHNOP_close, &op);

    if (rc < 0) {
        ERRNO_TO_STATUS(-rc, status);
        goto fail1;
    }

    return STATUS_SUCCESS;

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}
