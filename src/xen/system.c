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
#include "system.h"
#include "dbg_print.h"
#include "assert.h"

typedef struct _SYSTEM_CPU {
    ULONG   Index;
    CHAR    Manufacturer[13];
    UCHAR   ApicID;
} SYSTEM_CPU, *PSYSTEM_CPU;

typedef struct _SYSTEM_CONTEXT {
    LONG        References;
    SYSTEM_CPU  Cpu[MAXIMUM_PROCESSORS];
    PVOID       Handle;
} SYSTEM_CONTEXT, *PSYSTEM_CONTEXT;

static SYSTEM_CONTEXT   SystemContext;

static FORCEINLINE const CHAR *
__PlatformIdName(
    IN  ULONG   PlatformId
    )
{
#define PLATFORM_ID_NAME(_PlatformId)       \
        case VER_PLATFORM_ ## _PlatformId:  \
            return #_PlatformId;

    switch (PlatformId) {
    PLATFORM_ID_NAME(WIN32s);
    PLATFORM_ID_NAME(WIN32_WINDOWS);
    PLATFORM_ID_NAME(WIN32_NT);
    default:
        break;
    }

    return "UNKNOWN";
#undef  PLATFORM_ID_NAME
}

static FORCEINLINE const CHAR *
__SuiteName(
    IN  ULONG  SuiteBit
    )
{
#define SUITE_NAME(_Suite)          \
        case VER_SUITE_ ## _Suite:  \
            return #_Suite;

    switch (1 << SuiteBit) {
    SUITE_NAME(SMALLBUSINESS);
    SUITE_NAME(ENTERPRISE);
    SUITE_NAME(BACKOFFICE);
    SUITE_NAME(COMMUNICATIONS);
    SUITE_NAME(TERMINAL);
    SUITE_NAME(SMALLBUSINESS_RESTRICTED);
    SUITE_NAME(EMBEDDEDNT);
    SUITE_NAME(DATACENTER);
    SUITE_NAME(SINGLEUSERTS);
    SUITE_NAME(PERSONAL);
    SUITE_NAME(BLADE);
    SUITE_NAME(EMBEDDED_RESTRICTED);
    SUITE_NAME(SECURITY_APPLIANCE);
    SUITE_NAME(STORAGE_SERVER);
    SUITE_NAME(COMPUTE_SERVER);
    default:
        break;
    }

    return "UNKNOWN";
#undef  SUITE_NAME
}

static FORCEINLINE const CHAR *
__ProductTypeName(
    IN  UCHAR   ProductType
    )
{
#define PRODUCT_TYPE_NAME(_ProductType) \
        case VER_NT_ ## _ProductType:   \
            return #_ProductType;

        switch (ProductType) {
        PRODUCT_TYPE_NAME(WORKSTATION);
        PRODUCT_TYPE_NAME(DOMAIN_CONTROLLER);
        PRODUCT_TYPE_NAME(SERVER);
    default:
        break;
    }

    return "UNKNOWN";
#undef  PRODUCT_TYPE_NAME
}

static FORCEINLINE NTSTATUS
__SystemGetVersionInformation(
    VOID
    )
{
    RTL_OSVERSIONINFOEXW    VersionInformation;
    ULONG                   Bit;
    NTSTATUS                status;

    ASSERT3U(KeGetCurrentIrql(), ==, PASSIVE_LEVEL);

    RtlZeroMemory(&VersionInformation, sizeof (RTL_OSVERSIONINFOEXW));
    VersionInformation.dwOSVersionInfoSize = sizeof (RTL_OSVERSIONINFOEXW);

    status = RtlGetVersion((PRTL_OSVERSIONINFOW)&VersionInformation);
    if (!NT_SUCCESS(status))
        goto fail1;

#if defined(__i386__)
    Info("KERNEL: %d.%d (BUILD %d) PLATFORM %s\n",
         VersionInformation.dwMajorVersion,
         VersionInformation.dwMinorVersion,
         VersionInformation.dwBuildNumber,
         __PlatformIdName(VersionInformation.dwPlatformId));
#elif defined(__x86_64__)
    Info("KERNEL: %d.%d (BUILD %d) PLATFORM %s (x64)\n",
         VersionInformation.dwMajorVersion,
         VersionInformation.dwMinorVersion,
         VersionInformation.dwBuildNumber,
         __PlatformIdName(VersionInformation.dwPlatformId));
#else
#error 'Unrecognised architecture'
#endif    

    if (VersionInformation.wServicePackMajor != 0 ||
        VersionInformation.wServicePackMinor != 0)
        Info("SP: %d.%d (%s)\n",
             VersionInformation.wServicePackMajor,
             VersionInformation.wServicePackMinor,
             VersionInformation.szCSDVersion);

    Info("SUITES:\n");
    Bit = 0;
    while (VersionInformation.wSuiteMask != 0) {
        if (VersionInformation.wSuiteMask & 0x0001)
            Info("- %s\n", __SuiteName(Bit));

        VersionInformation.wSuiteMask >>= 1;
        Bit++;
    }

    Info("TYPE: %s\n", __ProductTypeName(VersionInformation.wProductType));

    return STATUS_SUCCESS;

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

static FORCEINLINE NTSTATUS
__SystemGetMemoryInformation(
    VOID
    )
{
    PHYSICAL_MEMORY_RANGE   *Range;
    ULONG                   Index;
    NTSTATUS                status;

    Range = MmGetPhysicalMemoryRanges();

    status = STATUS_UNSUCCESSFUL;
    if (Range == NULL)
        goto fail1;

    for (Index = 0;
         Range[Index].BaseAddress.QuadPart != 0 || Range[Index].NumberOfBytes.QuadPart != 0;
         Index++) {
        PHYSICAL_ADDRESS    Start;
        PHYSICAL_ADDRESS    End;

        Start.QuadPart = Range[Index].BaseAddress.QuadPart;
        End.QuadPart = Start.QuadPart + Range[Index].NumberOfBytes.QuadPart - 1;

        Info("RANGE[%u] %08x.%08x - %08x.%08x\n",
             Index,
             Start.HighPart, Start.LowPart,
             End.HighPart, End.LowPart);
    }

    ExFreePool(Range);

    return STATUS_SUCCESS;

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

KDEFERRED_ROUTINE   SystemCpuInformation;

VOID
SystemCpuInformation(
    IN  PKDPC   Dpc,
    IN  PVOID   Context,
    IN  PVOID   Argument1,
    IN  PVOID   Argument2
    )
{
    PSYSTEM_CPU Cpu = Context;
    PKSPIN_LOCK Lock = Argument1;
    PKEVENT     Event = Argument2;
    ULONG       EBX;
    ULONG       ECX;
    ULONG       EDX;

    UNREFERENCED_PARAMETER(Dpc);

    ASSERT(Cpu != NULL);
    ASSERT(Lock != NULL);
    ASSERT(Event != NULL);

    KeAcquireSpinLockAtDpcLevel(Lock);

    Info("====> (%u)\n", Cpu->Index);

    __CpuId(0, NULL, &EBX, &ECX, &EDX);

    RtlCopyMemory(&Cpu->Manufacturer[0], &EBX, sizeof (ULONG));
    RtlCopyMemory(&Cpu->Manufacturer[4], &EDX, sizeof (ULONG));
    RtlCopyMemory(&Cpu->Manufacturer[8], &ECX, sizeof (ULONG));

    Info("Manufacturer: %s\n", Cpu->Manufacturer);

    __CpuId(1, NULL, &EBX, NULL, NULL);

    Cpu->ApicID = EBX >> 24;

    Info("Local APIC ID: %02X\n", Cpu->ApicID);

    Info("<==== (%u)\n", Cpu->Index);

    KeReleaseSpinLockFromDpcLevel(Lock);
    KeSetEvent(Event, IO_NO_INCREMENT, FALSE);
}

static FORCEINLINE VOID
__SystemGetCpuInformation(
    VOID
    )
{
    PSYSTEM_CONTEXT     Context = &SystemContext;
    static KSPIN_LOCK   Lock;
    static KDPC         Dpc[MAXIMUM_PROCESSORS];
    static KEVENT       Event[MAXIMUM_PROCESSORS];
    PKEVENT             __Event[MAXIMUM_PROCESSORS];
    static KWAIT_BLOCK  WaitBlock[MAXIMUM_PROCESSORS];
    LONG                Index;

    KeInitializeSpinLock(&Lock);

    for (Index = 0; Index < KeNumberProcessors; Index++) {
        PSYSTEM_CPU Cpu = &Context->Cpu[Index];

        Cpu->Index = Index;
        KeInitializeDpc(&Dpc[Index],
                        SystemCpuInformation,
                        Cpu);
        KeSetTargetProcessorDpc(&Dpc[Index], (CCHAR)Index);
        KeSetImportanceDpc(&Dpc[Index], HighImportance);

        __Event[Index] = &Event[Index];
        KeInitializeEvent(__Event[Index], NotificationEvent, FALSE);

        KeInsertQueueDpc(&Dpc[Index], &Lock, __Event[Index]);
    }

    (VOID) KeWaitForMultipleObjects(KeNumberProcessors,
                                    __Event,
                                    WaitAll,
                                    Executive,
                                    KernelMode,
                                    FALSE,
                                    NULL,
                                    WaitBlock);
}

static FORCEINLINE NTSTATUS
__SystemGetStartOptions(
    VOID
    )
{
    UNICODE_STRING  Unicode;
    HANDLE          Key;
    PANSI_STRING    Ansi;
    NTSTATUS        status;

    RtlInitUnicodeString(&Unicode, L"\\Registry\\Machine\\SYSTEM\\CurrentControlSet\\Control");
    
    status = RegistryOpenKey(NULL, &Unicode, KEY_READ, &Key);
    if (!NT_SUCCESS(status))
        goto fail1;

    status = RegistryQuerySzValue(Key, "SystemStartOptions", &Ansi);
    if (!NT_SUCCESS(status))
        goto fail2;

    status = STATUS_UNSUCCESSFUL;
    if (Ansi[0].Buffer == NULL)
        goto fail3;

    Info("%Z\n", Ansi);

    RegistryFreeSzValue(Ansi);
    RegistryCloseKey(Key);

    return STATUS_SUCCESS;

fail3:
    Error("fail3\n");

    RegistryFreeSzValue(Ansi);

fail2:
    Error("fail2\n");

    RegistryCloseKey(Key);

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

static FORCEINLINE NTSTATUS
__SystemRegisterCallback(
    IN  PWCHAR              Name,
    IN  PCALLBACK_FUNCTION  Function,
    IN  PVOID               Argument,
    OUT PVOID               *Handle
    )
{
    UNICODE_STRING          Unicode;
    OBJECT_ATTRIBUTES       Attributes;
    PCALLBACK_OBJECT        Object;
    NTSTATUS                status;
    
    RtlInitUnicodeString(&Unicode, Name);

    InitializeObjectAttributes(&Attributes,
                               &Unicode,
                               OBJ_CASE_INSENSITIVE | OBJ_PERMANENT,
                               NULL,
                               NULL);

    status = ExCreateCallback(&Object,
                              &Attributes,
                              FALSE,
                              FALSE);
    if (!NT_SUCCESS(status))
        goto fail1;

    *Handle = ExRegisterCallback(Object,
                                 Function,
                                 Argument);

    status = STATUS_UNSUCCESSFUL;
    if (*Handle == NULL)
        goto fail2;

    ObDereferenceObject(Object);

    return STATUS_SUCCESS;

fail2:
    Error("fail2\n");

    ObDereferenceObject(Object);

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

static FORCEINLINE VOID
__SystemDeregisterCallback(
    IN  PVOID   Handle
    )
{
    ExUnregisterCallback(Handle);
}

CALLBACK_FUNCTION   SystemPowerStateCallback;

VOID
SystemPowerStateCallback(
    IN  PVOID   _Context,
    IN  PVOID   Argument1,
    IN  PVOID   Argument2
    )
{
    ULONG_PTR   Type = (ULONG_PTR)Argument1;
    ULONG_PTR   Value = (ULONG_PTR)Argument2;

    UNREFERENCED_PARAMETER(_Context);

    if (Type == PO_CB_SYSTEM_STATE_LOCK) {
        if (Value)
            Info("-> S0\n");
        else
            Info("<- S0\n");
    }
}

extern NTSTATUS
SystemInitialize(
    VOID
    )
{
    PSYSTEM_CONTEXT Context = &SystemContext;
    LONG            References;
    NTSTATUS        status;

    References = InterlockedIncrement(&Context->References);

    status = STATUS_OBJECTID_EXISTS;
    if (References != 1)
        goto fail1;

    status = __SystemGetStartOptions();
    if (!NT_SUCCESS(status))
        goto fail2;

    status = __SystemGetVersionInformation();
    if (!NT_SUCCESS(status))
        goto fail3;

    status = __SystemGetMemoryInformation();
    if (!NT_SUCCESS(status))
        goto fail4;

    __SystemGetCpuInformation();

    status = __SystemRegisterCallback(L"\\Callback\\PowerState",
                                      SystemPowerStateCallback,
                                      NULL,
                                      &Context->Handle);
    if (!NT_SUCCESS(status))
        goto fail5;

    return STATUS_SUCCESS;

fail5:
    Error("fail5\n");

fail4:
    Error("fail4\n");

fail3:
    Error("fail3\n");

fail2:
    Error("fail2\n");

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

extern VOID
SystemTeardown(
    VOID
    )
{
    PSYSTEM_CONTEXT Context = &SystemContext;

    __SystemDeregisterCallback(Context->Handle);
    Context->Handle = NULL;

    RtlZeroMemory(Context->Cpu, sizeof (SYSTEM_CPU) * MAXIMUM_PROCESSORS);

    (VOID) InterlockedDecrement(&Context->References);

    ASSERT(IsZeroMemory(Context, sizeof (SYSTEM_CONTEXT)));
}
