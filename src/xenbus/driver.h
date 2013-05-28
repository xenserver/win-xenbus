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

#ifndef _XENBUS_DRIVER_H
#define _XENBUS_DRIVER_H

typedef struct _XENBUS_FDO      XENBUS_FDO, *PXENBUS_FDO;
typedef struct _XENBUS_PDO      XENBUS_PDO, *PXENBUS_PDO;

#include "pdo.h"
#include "fdo.h"

extern PDRIVER_OBJECT   DriverObject;

typedef struct _XENBUS_PARAMETERS {
    PANSI_STRING    SupportedClasses;
    PANSI_STRING    SyntheticClasses;
    ULONG           CreatePDOs;
    ULONG           Balloon;
    ULONG           InterceptDmaAdapter;
} XENBUS_PARAMETERS, *PXENBUS_PARAMETERS;

extern XENBUS_PARAMETERS    DriverParameters;

#define MAX_DEVICE_ID_LEN   200

#pragma warning(push)
#pragma warning(disable:4201) // nonstandard extension used : nameless struct/union

typedef struct _XENBUS_DX {
    PDEVICE_OBJECT      DeviceObject;
    DEVICE_OBJECT_TYPE  Type;

    DEVICE_PNP_STATE    DevicePnpState;
    DEVICE_PNP_STATE    PreviousDevicePnpState;

    SYSTEM_POWER_STATE  SystemPowerState;
    DEVICE_POWER_STATE  DevicePowerState;

    CHAR                Name[MAX_DEVICE_ID_LEN];

    LIST_ENTRY          ListEntry;

    union {
        PXENBUS_FDO     Fdo;
        PXENBUS_PDO     Pdo;
    };
} XENBUS_DX, *PXENBUS_DX;

#pragma warning(pop)

#endif  // _XENBUS_DRIVER_H
