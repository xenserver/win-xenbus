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

#define XEN_API   __declspec(dllexport)

#include <ntddk.h>
#include <ntstrsafe.h>
#include <aux_klib.h>
#include <xen.h>
#include <util.h>

#include "high.h"
#include "module.h"
#include "log.h"
#include "assert.h"

#define MODULE_TAG   'UDOM'

typedef struct _MODULE {
    LIST_ENTRY  ListEntry;
    ULONG_PTR   Start;
    ULONG_PTR   End;
    CHAR        Name[AUX_KLIB_MODULE_PATH_LEN];
} MODULE, *PMODULE;

LIST_ENTRY  ModuleList;
PLIST_ENTRY ModuleCursor = &ModuleList;
HIGH_LOCK   ModuleLock;

static FORCEINLINE PVOID
__ModuleAllocate(
    IN  ULONG   Length
    )
{
    return __AllocateNonPagedPoolWithTag(Length, MODULE_TAG);
}

static FORCEINLINE VOID
__ModuleFree(
    IN  PVOID   Buffer
    )
{
    __FreePoolWithTag(Buffer, MODULE_TAG);
}

static FORCEINLINE VOID
__ModuleAudit(
    VOID
    )
{
    if (!IsListEmpty(&ModuleList)) {
        PLIST_ENTRY ListEntry;
        BOOLEAN     FoundCursor;

        FoundCursor = FALSE;

        for (ListEntry = ModuleList.Flink;
             ListEntry != &ModuleList;
             ListEntry = ListEntry->Flink) {
            PMODULE Module;

            if (ListEntry == ModuleCursor)
                FoundCursor = TRUE;

            Module = CONTAINING_RECORD(ListEntry, MODULE, ListEntry);

            ASSERT(Module->Start < Module->End);

            if (ListEntry->Flink != &ModuleList) {
                PMODULE Next;

                Next = CONTAINING_RECORD(ListEntry->Flink, MODULE, ListEntry);

                ASSERT(Module->End < Next->Start);
            }
        }

        ASSERT(FoundCursor);
    }
}

static FORCEINLINE VOID
__ModuleSearchForwards(
    IN  ULONG_PTR   Address
    )
{
    while (ModuleCursor != &ModuleList) {
        PMODULE Module;

        Module = CONTAINING_RECORD(ModuleCursor, MODULE, ListEntry);

        if (Address <= Module->End)
            break;

        ModuleCursor = ModuleCursor->Flink;
    }
}

static FORCEINLINE VOID
__ModuleSearchBackwards(
    IN  ULONG_PTR   Address
    )
{
    while (ModuleCursor != &ModuleList) {
        PMODULE Module;

        Module = CONTAINING_RECORD(ModuleCursor, MODULE, ListEntry);

        if (Address >= Module->Start)
            break;

        ModuleCursor = ModuleCursor->Blink;
    }
}

static FORCEINLINE NTSTATUS
__ModuleAdd(
    IN  PCHAR           Name,
    IN  ULONG_PTR       Start,
    IN  ULONG_PTR       Size
    )
{
#define INSERT_AFTER(_Cursor, _New)             \
        do {                                    \
            (_New)->Flink = (_Cursor)->Flink;   \
            (_Cursor)->Flink->Blink = (_New);   \
                                                \
            (_Cursor)->Flink = (_New);          \
            (_New)->Blink = (_Cursor);          \
        } while (FALSE)

#define INSERT_BEFORE(_Cursor, _New)            \
        do {                                    \
            (_New)->Blink = (_Cursor)->Blink;   \
            (_Cursor)->Blink->Flink = (_New);   \
                                                \
            (_Cursor)->Blink = (_New);          \
            (_New)->Flink = (_Cursor);          \
        } while (FALSE)

    PMODULE             New;
    ULONG               Index;
    PMODULE             Module;
    KIRQL               Irql;
    LIST_ENTRY          List;
    BOOLEAN             After;
    NTSTATUS            status;

    New = __ModuleAllocate(sizeof (MODULE));

    status = STATUS_NO_MEMORY;
    if (New == NULL)
        goto fail1;

    for (Index = 0; Index < AUX_KLIB_MODULE_PATH_LEN; Index++) {
        if (Name[Index] == '\0')
            break;

        New->Name[Index] = (CHAR)tolower(Name[Index]);
    }

    New->Start = Start;
    New->End = Start + Size - 1;

    Info("ADDING: (%p - %p) %s\n",
         (PVOID)New->Start,
         (PVOID)New->End,
         New->Name);

    InitializeListHead(&List);

    AcquireHighLock(&ModuleLock, &Irql);

again:
    After = TRUE;

    if (ModuleCursor == &ModuleList) {
        ASSERT(IsListEmpty(&ModuleList));
        goto done;
    }

    Module = CONTAINING_RECORD(ModuleCursor, MODULE, ListEntry);

    if (New->Start > Module->End) {
        __ModuleSearchForwards(New->Start);

        After = FALSE;

        if (ModuleCursor == &ModuleList)    // End of list
            goto done;

        Module = CONTAINING_RECORD(ModuleCursor, MODULE, ListEntry);

        if (New->End >= Module->Start) {    // Overlap
            PLIST_ENTRY Cursor = ModuleCursor->Blink;

            RemoveEntryList(ModuleCursor);
            InsertTailList(&List, &Module->ListEntry);

            ModuleCursor = Cursor;
            goto again;
        }
    } else if (New->End < Module->Start) {
        __ModuleSearchBackwards(New->End);

        After = TRUE;

        if (ModuleCursor == &ModuleList)    // Start of list
            goto done;

        Module = CONTAINING_RECORD(ModuleCursor, MODULE, ListEntry);

        if (New->Start <= Module->End) {    // Overlap
            PLIST_ENTRY Cursor = ModuleCursor->Flink;

            RemoveEntryList(ModuleCursor);
            InsertTailList(&List, &Module->ListEntry);

            ModuleCursor = Cursor;
            goto again;
        }
    } else {
        PLIST_ENTRY Cursor;
        
        Cursor = (ModuleCursor->Flink != &ModuleList) ?
                 ModuleCursor->Flink :
                 ModuleCursor->Blink;

        RemoveEntryList(ModuleCursor);
        InsertTailList(&List, &Module->ListEntry);

        ModuleCursor = Cursor;
        goto again;
    }

done:
    if (After)
        INSERT_AFTER(ModuleCursor, &New->ListEntry);
    else
        INSERT_BEFORE(ModuleCursor, &New->ListEntry);

    ModuleCursor = &New->ListEntry;

    ReleaseHighLock(&ModuleLock, Irql);

    while (!IsListEmpty(&List)) {
        PLIST_ENTRY     ListEntry;

        ListEntry = RemoveHeadList(&List);
        ASSERT(ListEntry != &List);

        Module = CONTAINING_RECORD(ListEntry, MODULE, ListEntry);

        Info("REMOVED: (%p - %p) %s\n",
             (PVOID)Module->Start,
             (PVOID)Module->End,
             Module->Name);

        __ModuleFree(Module);
    }

    __ModuleAudit();

    return STATUS_SUCCESS;

fail1:
    Error("fail1 (%08x)\n", status);

    return status;

#undef  INSERT_AFTER
#undef  INSERT_BEFORE
}

static VOID
ModuleLoad(
    IN  PUNICODE_STRING FullImageName,
    IN  HANDLE          ProcessId,
    IN  PIMAGE_INFO     ImageInfo
    )
{
    ANSI_STRING         Ansi;
    PCHAR               Buffer;
    PCHAR               Name;
    NTSTATUS            status;

    UNREFERENCED_PARAMETER(ProcessId);

    ASSERT3U(KeGetCurrentIrql(), ==, PASSIVE_LEVEL);

    if (!ImageInfo->SystemModeImage)
        return;

    status = RtlUnicodeStringToAnsiString(&Ansi, FullImageName, TRUE);
    if (!NT_SUCCESS(status))
        goto fail1;

    Buffer = __ModuleAllocate(Ansi.Length + sizeof (CHAR));

    status = STATUS_NO_MEMORY;
    if (Buffer == NULL)
        goto fail2;

    RtlCopyMemory(Buffer, Ansi.Buffer, Ansi.Length);

    Name = strrchr((const CHAR *)Buffer, '\\');
    Name = (Name == NULL) ? Buffer : (Name + 1);

    status = __ModuleAdd(Name,
                         (ULONG_PTR)ImageInfo->ImageBase,
                         (ULONG_PTR)ImageInfo->ImageSize);
    if (!NT_SUCCESS(status))
        goto fail3;

    __ModuleFree(Buffer);

    RtlFreeAnsiString(&Ansi);

    return;

fail3:
    Error("fail3\n");

    __ModuleFree(Buffer);

fail2:
    Error("fail2\n");

    RtlFreeAnsiString(&Ansi);

fail1:
    Error("fail1 (%08x)\n", status);
}

XEN_API
VOID
ModuleLookup(
    IN  ULONG_PTR   Address,
    OUT PCHAR       *Name,
    OUT PULONG_PTR  Offset
    )
{
    PLIST_ENTRY     ListEntry;
    KIRQL           Irql;

    *Name = NULL;
    *Offset = 0;

    AcquireHighLock(&ModuleLock, &Irql);

    for (ListEntry = ModuleList.Flink;
         ListEntry != &ModuleList;
         ListEntry = ListEntry->Flink) {
        PMODULE Module;

        Module = CONTAINING_RECORD(ListEntry, MODULE, ListEntry);

        if (Address >= Module->Start &&
            Address <= Module->End) {
            *Name = Module->Name;
            *Offset = Address - Module->Start;
            break;
        }
    }

    ReleaseHighLock(&ModuleLock, Irql);
}

VOID
ModuleTeardown(
    VOID
    )
{
    (VOID) PsRemoveLoadImageNotifyRoutine(ModuleLoad);

    while (!IsListEmpty(&ModuleList)) {
        PLIST_ENTRY ListEntry;
        PMODULE     Module;

        ListEntry = RemoveHeadList(&ModuleList);
        ASSERT(ListEntry != &ModuleList);

        Module = CONTAINING_RECORD(ListEntry, MODULE, ListEntry);
        __ModuleFree(Module);
    }

    RtlZeroMemory(&ModuleList, sizeof (LIST_ENTRY));
}

NTSTATUS
ModuleInitialize(
    VOID)
{
    ULONG                       BufferSize;
    ULONG                       Count;
    PAUX_MODULE_EXTENDED_INFO   QueryInfo;
    ULONG                       Index;
    NTSTATUS                    status;

    InitializeHighLock(&ModuleLock);

    (VOID) AuxKlibInitialize();

    status = AuxKlibQueryModuleInformation(&BufferSize,
                                           sizeof (AUX_MODULE_EXTENDED_INFO),
                                           NULL);
    if (!NT_SUCCESS(status))
        goto fail1;

    status = STATUS_UNSUCCESSFUL;
    if (BufferSize == 0)
        goto fail2;

    Count = BufferSize / sizeof (AUX_MODULE_EXTENDED_INFO);
    QueryInfo = __ModuleAllocate(sizeof (AUX_MODULE_EXTENDED_INFO) * Count);

    status = STATUS_NO_MEMORY;
    if (QueryInfo == NULL)
        goto fail3;

    status = AuxKlibQueryModuleInformation(&BufferSize,
                                           sizeof (AUX_MODULE_EXTENDED_INFO),
                                           QueryInfo);
    if (!NT_SUCCESS(status))
        goto fail4;

    InitializeListHead(&ModuleList);

    for (Index = 0; Index < Count; Index++) {
        PCHAR   Name;

        Name = strrchr((const CHAR *)QueryInfo[Index].FullPathName, '\\');
        Name = (Name == NULL) ? (PCHAR)QueryInfo[Index].FullPathName : (Name + 1);

        status = __ModuleAdd(Name,
                             (ULONG_PTR)QueryInfo[Index].BasicInfo.ImageBase,
                             (ULONG_PTR)QueryInfo[Index].ImageSize);
        if (!NT_SUCCESS(status))
            goto fail5;
    }

    status = PsSetLoadImageNotifyRoutine(ModuleLoad);
    if (!NT_SUCCESS(status))
        goto fail6;

    __ModuleFree(QueryInfo);

    __ModuleAudit();

    return STATUS_SUCCESS;

fail6:
    Error("fail6\n");

fail5:
    Error("fail5\n");

    while (!IsListEmpty(&ModuleList)) {
        PLIST_ENTRY ListEntry;
        PMODULE     Module;

        ListEntry = RemoveHeadList(&ModuleList);
        ASSERT(ListEntry != &ModuleList);

        Module = CONTAINING_RECORD(ListEntry, MODULE, ListEntry);
        __ModuleFree(Module);
    }

    RtlZeroMemory(&ModuleList, sizeof (LIST_ENTRY));

fail4:
    Error("fail4\n");

    __ModuleFree(QueryInfo);

fail3:
    Error("fail3\n");

fail2:
    Error("fail2\n");

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}
