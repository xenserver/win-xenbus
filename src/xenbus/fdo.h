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

#ifndef _XENBUS_FDO_H
#define _XENBUS_FDO_H

#include <ntddk.h>

#include "driver.h"
#include "types.h"

typedef enum _XENBUS_RESOURCE_TYPE {
    MEMORY_RESOURCE = 0,
    INTERRUPT_RESOURCE,
    RESOURCE_COUNT
} XENBUS_RESOURCE_TYPE, *PXENBUS_RESOURCE_TYPE;

typedef struct _XENBUS_RESOURCE {
    CM_PARTIAL_RESOURCE_DESCRIPTOR Raw;
    CM_PARTIAL_RESOURCE_DESCRIPTOR Translated;
} XENBUS_RESOURCE, *PXENBUS_RESOURCE;

extern NTSTATUS
FdoCreate(
    IN  PDEVICE_OBJECT  PhysicalDeviceObject
    );

extern VOID
FdoDestroy(
    IN  PXENBUS_FDO Fdo
    );

extern NTSTATUS
FdoDelegateIrp(
    IN  PXENBUS_FDO     Fdo,
    IN  PIRP            Irp
    );

extern VOID
FdoAddPhysicalDeviceObject(
    IN  PXENBUS_FDO     Fdo,
    IN  PXENBUS_PDO     Pdo
    );

extern VOID
FdoRemovePhysicalDeviceObject(
    IN  PXENBUS_FDO     Fdo,
    IN  PXENBUS_PDO     Pdo
    );

extern VOID
FdoAcquireMutex(
    IN  PXENBUS_FDO     Fdo
    );

extern VOID
FdoReleaseMutex(
    IN  PXENBUS_FDO     Fdo
    );

extern PDEVICE_OBJECT
FdoGetPhysicalDeviceObject(
    IN  PXENBUS_FDO Fdo
    );

extern PDMA_ADAPTER
FdoGetDmaAdapter(
    IN  PXENBUS_FDO         Fdo,
    IN  PDEVICE_DESCRIPTION DeviceDescriptor,
    OUT PULONG              NumberOfMapRegisters
    );

extern PCHAR
FdoGetName(
    IN  PXENBUS_FDO Fdo
    );

extern PXENBUS_RESOURCE
FdoGetResource(
    IN  PXENBUS_FDO             Fdo,
    IN  XENBUS_RESOURCE_TYPE    Type
    );

extern PKINTERRUPT
FdoGetInterruptObject(
    IN  PXENBUS_FDO Fdo
    );

#include "suspend.h"

extern PXENBUS_SUSPEND_INTERFACE
FdoGetSuspendInterface(
    IN  PXENBUS_FDO     Fdo
    );

#include "shared_info.h"

extern PXENBUS_SHARED_INFO_INTERFACE
FdoGetSharedInfoInterface(
    IN  PXENBUS_FDO     Fdo
    );

#include "evtchn.h"

extern PXENBUS_EVTCHN_INTERFACE
FdoGetEvtchnInterface(
    IN  PXENBUS_FDO     Fdo
    );

#include "debug.h"

extern PXENBUS_DEBUG_INTERFACE
FdoGetDebugInterface(
    IN  PXENBUS_FDO     Fdo
    );

#include "store.h"

extern PXENBUS_STORE_INTERFACE
FdoGetStoreInterface(
    IN  PXENBUS_FDO     Fdo
    );

#include "gnttab.h"

extern PXENBUS_GNTTAB_INTERFACE
FdoGetGnttabInterface(
    IN  PXENBUS_FDO     Fdo
    );

extern NTSTATUS
FdoDispatch(
    IN  PXENBUS_FDO Fdo,
    IN  PIRP        Irp
    );

#endif  // _XENBUS_FDO_H
