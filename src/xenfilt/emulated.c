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
#include "log.h"
#include "assert.h"

#define MAXIMUM_CLASS_NAME_LENGTH   32
#define MAXIMUM_DEVICE_NAME_LENGTH  32
#define MAXIMUM_ALIAS_LENGTH        128

typedef struct _EMULATED_DEVICE {
    CHAR    Class[MAXIMUM_CLASS_NAME_LENGTH];
    CHAR    Device[MAXIMUM_DEVICE_NAME_LENGTH];
    CHAR    Alias[MAXIMUM_ALIAS_LENGTH];
    BOOLEAN Present;
} EMULATED_DEVICE, *PEMULATED_DEVICE;

struct _XENFILT_EMULATED_CONTEXT {
    LONG                References;
    PEMULATED_DEVICE    Table;
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
EmulatedIsPresent(
    IN  PXENFILT_EMULATED_CONTEXT   Context,
    IN  PCHAR                       Class,
    IN  PCHAR                       Device
    )
{
    PEMULATED_DEVICE                Entry;
    BOOLEAN                         Present;

    Present = FALSE;

    if (Context->Table == NULL) {
        Warning("no table\n");
        goto done;
    }

    for (Entry = Context->Table; strlen(Entry->Alias) != 0; Entry++) {
        if (_stricmp(Entry->Class, Class) == 0 &&
            _stricmp(Entry->Device, Device) == 0)
            break;
    }

    if (strlen(Entry->Alias) == 0) {
        Info("%s %s NOT FOUND\n",
             Class,
             Device);
        goto done;
    }

    Present = Entry->Present;

    Info("%s %s %s\n",
         Entry->Class,
         Entry->Device,
         (Present) ? "PRESENT" : "NOT PRESENT");

done:
    return Present;
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
EmulatedUpdate(
    IN  PXENFILT_EMULATED_INTERFACE Interface,
    IN  PCHAR                       Alias
    )
{
    PXENFILT_EMULATED_CONTEXT       Context = Interface->Context;
    PEMULATED_DEVICE                Entry;
    HANDLE                          ServiceKey;
    HANDLE                          StatusKey;
    LONG                            Count;
    LONG                            Index;
    PANSI_STRING                    Old;
    PANSI_STRING                    New;
    ULONG                           Length;
    NTSTATUS                        status;

    if (Context->Table == NULL)
        goto done;

    for (Entry = Context->Table; strlen(Entry->Alias) != 0; Entry++) {
        if (strcmp(Entry->Alias, Alias) == 0)
            break;
    }

    if (strlen(Entry->Alias) == 0)
        goto done;

    Info("%s %s\n", Entry->Class, Entry->Device);

    Entry->Present = TRUE;

    status = RegistryOpenServiceKey(KEY_ALL_ACCESS, &ServiceKey);
    if (!NT_SUCCESS(status))
        goto fail1;

    status = RegistryOpenSubKey(ServiceKey, "Status", KEY_ALL_ACCESS, &StatusKey);
    if (!NT_SUCCESS(status))
        goto fail2;

    status = RegistryQuerySzValue(StatusKey, Entry->Class, &Old);
    if (!NT_SUCCESS(status))
        Old = NULL;

    Count = 0;
    for (Index = 0; Old != NULL && Old[Index].Buffer != NULL; Index++)
        Count++;

    New = __EmulatedAllocate(sizeof (ANSI_STRING) * (Count + 2));

    status = STATUS_NO_MEMORY;
    if (New == NULL)
        goto fail3;

    for (Index = 0; Index < Count; Index++) {
        Length = Old[Index].Length;

        New[Index].MaximumLength = (USHORT)Length + sizeof (CHAR);
        New[Index].Buffer = __EmulatedAllocate(New[Index].MaximumLength);

        status = STATUS_NO_MEMORY;
        if (New[Index].Buffer == NULL)
            goto fail4;

        RtlCopyMemory(New[Index].Buffer, Old[Index].Buffer, Length);
        New[Index].Length = (USHORT)Length;
    }

    Length = (ULONG)strlen(Entry->Device);

    New[Count].MaximumLength = (USHORT)Length + sizeof (CHAR);
    New[Count].Buffer = __EmulatedAllocate(New[Count].MaximumLength);

    status = STATUS_NO_MEMORY;
    if (New[Count].Buffer == NULL)
        goto fail5;

    RtlCopyMemory(New[Count].Buffer, Entry->Device, Length);
    New[Count].Length = (USHORT)Length;

    status = RegistryUpdateSzValue(StatusKey, Entry->Class, REG_MULTI_SZ, New);
    if (!NT_SUCCESS(status))
        goto fail6;

    RegistryFreeSzValue(Old);

    for (Index = 0; Index < Count + 1; Index++)
        __EmulatedFree(New[Index].Buffer);

    __EmulatedFree(New);

    RegistryCloseKey(StatusKey);

    RegistryCloseKey(ServiceKey);

done:
    return STATUS_SUCCESS;

fail6:
    Error("fail6\n");

    __EmulatedFree(New[Count].Buffer);

fail5:
    Error("fail5\n");

    Index = Count;

fail4:
    Error("fail4\n");

    while (--Index >= 0)
        __EmulatedFree(New[Index].Buffer);

    __EmulatedFree(New);

fail3:
    Error("fail3\n");

    RegistryFreeSzValue(Old);

    RegistryCloseKey(StatusKey);

fail2:
    Error("fail2\n");

    RegistryCloseKey(ServiceKey);

fail1:
    Error("fail1 (%08x)\n", status);

    return status;    
}

static NTSTATUS
EmulatedCountDevices(
    IN  PVOID   Context,
    IN  HANDLE  Key,
    IN  PCHAR   Name
    )
{
    PULONG      Count = Context;

    UNREFERENCED_PARAMETER(Key);
    UNREFERENCED_PARAMETER(Name);

    (*Count)++;

    return STATUS_SUCCESS;
}

static NTSTATUS
EmulatedCountClasses(
    IN  PVOID   Context,
    IN  HANDLE  Key,
    IN  PCHAR   Name
    )
{
    HANDLE      ClassKey;
    NTSTATUS    status;

    status = RegistryOpenSubKey(Key, Name, KEY_ALL_ACCESS, &ClassKey);
    if (!NT_SUCCESS(status))
        goto fail1;

    status = RegistryEnumerateValues(ClassKey, EmulatedCountDevices, Context);
    if (!NT_SUCCESS(status))
        goto fail2;

    RegistryCloseKey(ClassKey);

    return STATUS_SUCCESS;

fail2:
    RegistryCloseKey(ClassKey);
    
fail1:
    return status;
}

static NTSTATUS
EmulatedAddDevices(
    IN  PVOID           Context,
    IN  HANDLE          Key,
    IN  PCHAR           Name
    )
{
    PEMULATED_DEVICE    Entry = *(PEMULATED_DEVICE *)Context;
    PCHAR               Class = Entry->Class;
    PANSI_STRING        Alias;
    NTSTATUS            status;

    status = RtlStringCchPrintfA(Entry->Device,
                                 MAXIMUM_DEVICE_NAME_LENGTH,
                                 "%s",
                                 Name);
    ASSERT(NT_SUCCESS(status));

    status = RegistryQuerySzValue(Key, Name, &Alias);
    if (!NT_SUCCESS(status))
        goto fail1;

    status = RtlStringCchPrintfA(Entry->Alias,
                                 MAXIMUM_ALIAS_LENGTH,
                                 "%s",
                                 Alias[0].Buffer);
    ASSERT(NT_SUCCESS(status));

    RegistryFreeSzValue(Alias);

    Entry++;
    
    status = RtlStringCchPrintfA(Entry->Class,
                                 MAXIMUM_CLASS_NAME_LENGTH,
                                 "%s",
                                 Class);
    ASSERT(NT_SUCCESS(status));

    *(PEMULATED_DEVICE *)Context = Entry;

    return STATUS_SUCCESS;

fail1:
    return status;
}


static NTSTATUS
EmulatedAddClasses(
    IN  PVOID           Context,
    IN  HANDLE          Key,
    IN  PCHAR           Name
    )
{
    PEMULATED_DEVICE    Entry = *(PEMULATED_DEVICE *)Context;
    HANDLE              ClassKey;
    NTSTATUS            status;

    status = RtlStringCchPrintfA(Entry->Class,
                                 MAXIMUM_CLASS_NAME_LENGTH,
                                 "%s",
                                 Name);
    ASSERT(NT_SUCCESS(status));

    status = RegistryOpenSubKey(Key, Name, KEY_ALL_ACCESS, &ClassKey);
    if (!NT_SUCCESS(status))
        goto fail1;

    status = RegistryEnumerateValues(ClassKey, EmulatedAddDevices, &Entry);
    if (!NT_SUCCESS(status))
        goto fail2;

    RegistryCloseKey(ClassKey);

    *(PEMULATED_DEVICE *)Context = Entry;

    return STATUS_SUCCESS;

fail2:
    RegistryCloseKey(ClassKey);
    
fail1:
    return status;
}

static FORCEINLINE NTSTATUS
__EmulatedGetDeviceTable(
    IN  PXENFILT_EMULATED_CONTEXT   Context
    )
{
    HANDLE                          ServiceKey;
    HANDLE                          AliasesKey;
    ULONG                           Count;
    PEMULATED_DEVICE                Table;
    PEMULATED_DEVICE                Entry;
    ULONG                           Index;
    NTSTATUS                        status;

    status = RegistryOpenServiceKey(KEY_ALL_ACCESS, &ServiceKey);
    if (!NT_SUCCESS(status))
        goto fail1;

    status = RegistryOpenSubKey(ServiceKey, "Aliases", KEY_ALL_ACCESS, &AliasesKey);
    if (!NT_SUCCESS(status))
        goto fail2;

    Count = 0;

    status = RegistryEnumerateSubKeys(AliasesKey, EmulatedCountClasses, &Count);
    if (!NT_SUCCESS(status))
        goto fail3;

    Table = NULL;

    if (Count == 0)
        goto done;

    Table = __EmulatedAllocate(sizeof (EMULATED_DEVICE) * (Count + 1));

    status = STATUS_NO_MEMORY;
    if (Table == NULL)
        goto fail4;

    Entry = Table;

    status = RegistryEnumerateSubKeys(AliasesKey, EmulatedAddClasses, &Entry);
    if (!NT_SUCCESS(status))
        goto fail5;

    ASSERT3U((ULONG)(Entry - Table), ==, Count);
    RtlZeroMemory(Entry, sizeof (EMULATED_DEVICE));

    for (Index = 0; strlen(Table[Index].Alias) != 0; Index++) {
        Entry = &Table[Index];

        Info("[%u]: %s %s -> %s\n",
             Index,
             Entry->Class,
             Entry->Device,
             Entry->Alias);
    }

done:
    Context->Table = Table;

    RegistryCloseKey(AliasesKey);

    RegistryCloseKey(ServiceKey);

    return STATUS_SUCCESS;

fail5:
    Error("fail5\n");

    __EmulatedFree(Table);

fail4:
    Error("fail4\n");

fail3:
    Error("fail3\n");

    RegistryCloseKey(AliasesKey);

fail2:
    Error("fail2\n");

    RegistryCloseKey(ServiceKey);

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

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

    status = __EmulatedGetDeviceTable(Context);
    if (!NT_SUCCESS(status))
        goto fail2;

    Interface->Context = Context;
    Interface->Operations = &Operations;

    Trace("<====\n");

    return STATUS_SUCCESS;

fail2:
    Error("fail2\n");

    ASSERT(IsZeroMemory(Context, sizeof (XENFILT_EMULATED_CONTEXT)));
    __EmulatedFree(Context);

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

    if (Context->Table != NULL) {
        __EmulatedFree(Context->Table);
        Context->Table = NULL;
    }

    ASSERT(IsZeroMemory(Context, sizeof (XENFILT_EMULATED_CONTEXT)));
    __EmulatedFree(Context);

    RtlZeroMemory(Interface, sizeof (XENFILT_EMULATED_INTERFACE));

    Trace("<====\n");
}
