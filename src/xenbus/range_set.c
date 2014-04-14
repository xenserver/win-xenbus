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

#include "range_set.h"
#include "dbg_print.h"
#include "assert.h"

#define RANGE_SET_AUDIT DBG

#define RANGE_SET_TAG   'GNAR'

typedef struct _RANGE {
    LIST_ENTRY  ListEntry;
    ULONGLONG   Start;
    ULONGLONG   End;
} RANGE, *PRANGE;

struct _XENBUS_RANGE_SET {
    LIST_ENTRY      List;
    PLIST_ENTRY     Cursor;
    PRANGE          Spare;
};

static FORCEINLINE PVOID
__RangeSetAllocate(
    IN  ULONG   Length
    )
{
    return __AllocateNonPagedPoolWithTag(Length, RANGE_SET_TAG);
}

static FORCEINLINE VOID
__RangeSetFree(
    IN  PVOID   Buffer
    )
{
    __FreePoolWithTag(Buffer, RANGE_SET_TAG);
}

#if RANGE_SET_AUDIT
static FORCEINLINE VOID
__RangeSetAudit(
    IN  PXENBUS_RANGE_SET   RangeSet
    )
{
    if (IsListEmpty(&RangeSet->List)) {
        ASSERT3P(RangeSet->Cursor, ==, &RangeSet->List);
    } else {
        BOOLEAN     FoundCursor;
        PLIST_ENTRY ListEntry;

        FoundCursor = FALSE;

        for (ListEntry = RangeSet->List.Flink;
             ListEntry != &RangeSet->List;
             ListEntry = ListEntry->Flink) {
            PRANGE Range;

            if (ListEntry == RangeSet->Cursor)
                FoundCursor = TRUE;

            Range = CONTAINING_RECORD(ListEntry, RANGE, ListEntry);

            ASSERT3U(Range->Start, <=, Range->End);

            if (ListEntry->Flink != &RangeSet->List) {
                PRANGE Next;

                Next = CONTAINING_RECORD(ListEntry->Flink, RANGE, ListEntry);

                ASSERT3U(Range->End, <, Next->Start - 1);
            }
        }

        ASSERT(FoundCursor);
    }
}
#endif

static FORCEINLINE VOID
__RangeSetRemove(
    IN  PXENBUS_RANGE_SET   RangeSet,
    IN  BOOLEAN             After
    )
{
    PLIST_ENTRY             Cursor;
    PRANGE                  Range;

    Cursor = RangeSet->Cursor;
    RangeSet->Cursor = (After) ? Cursor->Flink : Cursor->Blink;

    RemoveEntryList(Cursor);

    Range = CONTAINING_RECORD(Cursor, RANGE, ListEntry);

    if (RangeSet->Spare == NULL) {
        RtlZeroMemory(Range, sizeof (RANGE));
        RangeSet->Spare = Range;
    } else {
        __RangeSetFree(Range);
    }
}

static FORCEINLINE VOID
__RangeSetMergeBackwards(
    IN  PXENBUS_RANGE_SET   RangeSet
    )
{
    PLIST_ENTRY             Cursor;
    PRANGE                  Range;
    PRANGE                  Previous;

    Cursor = RangeSet->Cursor;
    if (Cursor->Blink == &RangeSet->List)
        return;

    Range = CONTAINING_RECORD(Cursor, RANGE, ListEntry);
    Previous = CONTAINING_RECORD(Cursor->Blink, RANGE, ListEntry);

    if (Previous->End != Range->Start - 1)  // Not touching
        return;

    Previous->End = Range->End;
    __RangeSetRemove(RangeSet, FALSE);
}

static FORCEINLINE VOID
__RangeSetMergeForwards(
    IN  PXENBUS_RANGE_SET   RangeSet
    )
{
    PLIST_ENTRY             Cursor;
    PRANGE                  Range;
    PRANGE                  Next;

    Cursor = RangeSet->Cursor;
    if (Cursor->Flink == &RangeSet->List)
        return;

    Range = CONTAINING_RECORD(Cursor, RANGE, ListEntry);
    Next = CONTAINING_RECORD(Cursor->Flink, RANGE, ListEntry);

    if (Next->Start != Range->End + 1)  // Not touching
        return;

    Next->Start = Range->Start;
    __RangeSetRemove(RangeSet, TRUE);
}

BOOLEAN
RangeSetIsEmpty(
    IN  PXENBUS_RANGE_SET   RangeSet
    )
{
    return IsListEmpty(&RangeSet->List);
}

ULONGLONG
RangeSetPop(
    IN  PXENBUS_RANGE_SET   RangeSet
    )
{
    PLIST_ENTRY             Cursor;
    PRANGE                  Range;
    ULONGLONG               Item;

    // Start at the head of the list
    Cursor = RangeSet->Cursor = RangeSet->List.Flink;
    ASSERT(Cursor != &RangeSet->List);

    Range = CONTAINING_RECORD(Cursor, RANGE, ListEntry);

    Item = Range->Start;

    if (Item == Range->End) { // Singleton
        __RangeSetRemove(RangeSet, TRUE);
    } else {
        Range->Start = Item + 1;
    }

#if RANGE_SET_AUDIT
    __RangeSetAudit(RangeSet);
#endif

    return Item;
}

static FORCEINLINE NTSTATUS
__RangeSetAdd(
    IN  PXENBUS_RANGE_SET   RangeSet,
    IN  ULONGLONG           Start,
    IN  ULONGLONG           End,
    IN  BOOLEAN             After
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

    PRANGE                  Range;
    PLIST_ENTRY             Cursor;
    NTSTATUS                status;

    if (RangeSet->Spare != NULL) {
        Range = RangeSet->Spare;
        RangeSet->Spare = NULL;
    } else {
        Range = __RangeSetAllocate(sizeof (RANGE));

        status = STATUS_NO_MEMORY;
        if (Range == NULL)
            goto fail1;
    }

    ASSERT(IsZeroMemory(Range, sizeof (RANGE)));

    Range->Start = Start;
    Range->End = End;

    Cursor = RangeSet->Cursor;

    if (After)
        INSERT_AFTER(Cursor, &Range->ListEntry);
    else
        INSERT_BEFORE(Cursor, &Range->ListEntry);

    RangeSet->Cursor = &Range->ListEntry;

    __RangeSetMergeBackwards(RangeSet);
    __RangeSetMergeForwards(RangeSet);

    return STATUS_SUCCESS;

fail1:
    Error("fail1 (%08x)\n", status);

    return status;

#undef  INSERT_AFTER
#undef  INSERT_BEFORE
}

NTSTATUS
RangeSetGet(
    IN  PXENBUS_RANGE_SET   RangeSet,
    IN  ULONGLONG           Item
    )
{
    PLIST_ENTRY             Cursor;
    PRANGE                  Range;
    NTSTATUS                status;

    Cursor = RangeSet->Cursor;
    ASSERT(Cursor != &RangeSet->List);

    Range = CONTAINING_RECORD(Cursor, RANGE, ListEntry);

    if (Item < Range->Start) {
        do {
            Cursor = Cursor->Blink;
            ASSERT(Cursor != &RangeSet->List);

            Range = CONTAINING_RECORD(Cursor, RANGE, ListEntry);
        } while (Item < Range->Start);

        RangeSet->Cursor = Cursor;

        if (Item == Range->Start && Item == Range->End) {   // Singleton
            __RangeSetRemove(RangeSet, TRUE);
            goto done;
        }
    } else if (Item > Range->End) {
        do {
            Cursor = Cursor->Flink;
            ASSERT(Cursor != &RangeSet->List);

            Range = CONTAINING_RECORD(Cursor, RANGE, ListEntry);
        } while (Item > Range->End);

        RangeSet->Cursor = Cursor;

        if (Item == Range->Start && Item == Range->End) {   // Singleton
            __RangeSetRemove(RangeSet, FALSE);
            goto done;
        }
    }

    ASSERT3U(Range->End, >, Range->Start);

    if (Item == Range->Start) {
        Range->Start = Item + 1;
        goto done;
    }

    ASSERT3U(Range->Start, <, Item);

    if (Item == Range->End) {
        Range->End = Item - 1;
        goto done;
    }

    ASSERT3U(Item, <, Range->End);

    // We need to split a range
    status = __RangeSetAdd(RangeSet, Item + 1, Range->End, TRUE);
    if (!NT_SUCCESS(status))
        goto fail1;

    Range->End = Item - 1;

done:
#if RANGE_SET_AUDIT
    __RangeSetAudit(RangeSet);
#endif

    return STATUS_SUCCESS;

fail1:
    Error("fail1 (%08x)\n", status);

#if RANGE_SET_AUDIT
    __RangeSetAudit(RangeSet);
#endif

    return status;    
}

static FORCEINLINE NTSTATUS
__RangeSetAddAfter(
    IN  PXENBUS_RANGE_SET   RangeSet,
    IN  ULONGLONG           Start,
    IN  ULONGLONG           End
    )
{
    PLIST_ENTRY             Cursor;
    PRANGE                  Range;
    NTSTATUS                status;

    Cursor = RangeSet->Cursor;
    ASSERT(Cursor != &RangeSet->List);

    Range = CONTAINING_RECORD(Cursor, RANGE, ListEntry);
    ASSERT3U(Start, >, Range->End);

    Cursor = Cursor->Flink;
    while (Cursor != &RangeSet->List) {
        Range = CONTAINING_RECORD(Cursor, RANGE, ListEntry);

        if (Start < Range->Start) {
            ASSERT(End < Range->Start);
            break;
        }

        Cursor = Cursor->Flink;
    }

    RangeSet->Cursor = Cursor;
    status = __RangeSetAdd(RangeSet, Start, End, FALSE);    
    if (!NT_SUCCESS(status))
        goto fail1;

    return STATUS_SUCCESS;

fail1:
    Error("fail1 (%08x)\n", status);

    return status;    
}

static FORCEINLINE NTSTATUS
__RangeSetAddBefore(
    IN  PXENBUS_RANGE_SET   RangeSet,
    IN  ULONGLONG           Start,
    IN  ULONGLONG           End
    )
{
    PLIST_ENTRY             Cursor;
    PRANGE                  Range;
    NTSTATUS                status;

    Cursor = RangeSet->Cursor;
    ASSERT(Cursor != &RangeSet->List);

    Range = CONTAINING_RECORD(Cursor, RANGE, ListEntry);
    ASSERT3U(End, <, Range->Start);

    Cursor = Cursor->Blink;
    while (Cursor != &RangeSet->List) {
        Range = CONTAINING_RECORD(Cursor, RANGE, ListEntry);

        if (End > Range->End) {
            ASSERT(Start > Range->End);
            break;
        }

        Cursor = Cursor->Blink;
    }

    RangeSet->Cursor = Cursor;
    status = __RangeSetAdd(RangeSet, Start, End, TRUE);    
    if (!NT_SUCCESS(status))
        goto fail1;

    return STATUS_SUCCESS;

fail1:
    Error("fail1 (%08x)\n", status);

    return status;    
}

NTSTATUS
RangeSetPut(
    IN  PXENBUS_RANGE_SET   RangeSet,
    IN  ULONGLONG           Start,
    IN  ULONGLONG           End
    )
{
    PLIST_ENTRY             Cursor;
    NTSTATUS                status;

    ASSERT3U(End, >=, Start);

    Cursor = RangeSet->Cursor;

    if (Cursor == &RangeSet->List) {
        status = __RangeSetAdd(RangeSet, Start, End, TRUE);
    } else {
        PRANGE  Range;

        Range = CONTAINING_RECORD(Cursor, RANGE, ListEntry);

        if (Start > Range->End) {
            status = __RangeSetAddAfter(RangeSet, Start, End);
        } else {
            ASSERT3U(End, <, Range->Start);
            status = __RangeSetAddBefore(RangeSet, Start, End);
        }
    }

#if RANGE_SET_AUDIT
    __RangeSetAudit(RangeSet);
#endif

    return status;
}

NTSTATUS
RangeSetInitialize(
    OUT PXENBUS_RANGE_SET   *RangeSet
    )
{
    NTSTATUS                status;

    *RangeSet = __RangeSetAllocate(sizeof (XENBUS_RANGE_SET));

    status = STATUS_NO_MEMORY;
    if (*RangeSet == NULL)
        goto fail1;

    InitializeListHead(&(*RangeSet)->List);
    (*RangeSet)->Cursor = &(*RangeSet)->List;

#if RANGE_SET_AUDIT
    __RangeSetAudit(*RangeSet);
#endif

    return STATUS_SUCCESS;

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

VOID
RangeSetTeardown(
    IN  PXENBUS_RANGE_SET   RangeSet
    )
{
    if (RangeSet->Spare != NULL) {
        __RangeSetFree(RangeSet->Spare);
        RangeSet->Spare = NULL;
    }
        
    ASSERT(IsListEmpty(&RangeSet->List));
    RtlZeroMemory(&RangeSet->List, sizeof (LIST_ENTRY));

    RangeSet->Cursor = NULL;

    ASSERT(IsZeroMemory(RangeSet, sizeof (XENBUS_RANGE_SET)));
    __RangeSetFree(RangeSet);
}

