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
#include <stdlib.h>
#include <xen.h>
#include <util.h>

#include "mutex.h"
#include "balloon.h"
#include "range_set.h"
#include "dbg_print.h"
#include "assert.h"

#define BALLOON_AUDIT   DBG

#define CSHORT_MAX          ((1 << (sizeof (CSHORT) * 8)) - 1)
#define MAX_PAGES_PER_MDL   ((CSHORT_MAX - sizeof(MDL)) / sizeof(PFN_NUMBER))

#define BALLOON_PFN_ARRAY_SIZE  (MAX_PAGES_PER_MDL)

#define BALLOON_TAG   'LLAB'

struct _XENBUS_BALLOON
{
    PKEVENT             LowMemoryEvent;
    HANDLE              LowMemoryHandle;
    MUTEX               Mutex;
    ULONGLONG           Size;
    PXENBUS_RANGE_SET   RangeSet;
    MDL                 Mdl;
    PFN_NUMBER          PfnArray[BALLOON_PFN_ARRAY_SIZE];
};

static FORCEINLINE PVOID
__BalloonAllocate(
    IN  ULONG   Length
    )
{
    return __AllocateNonPagedPoolWithTag(Length, BALLOON_TAG);
}

static FORCEINLINE VOID
__BalloonFree(
    IN  PVOID   Buffer
    )
{
    __FreePoolWithTag(Buffer, BALLOON_TAG);
}

#define SWAP_NODES(_PfnArray, _X, _Y)       \
    do {                                    \
        PFN_NUMBER  _Pfn = _PfnArray[(_Y)]; \
                                            \
        _PfnArray[_Y] = _PfnArray[_X];      \
        _PfnArray[_X] = _Pfn;               \
    } while (FALSE)

static FORCEINLINE VOID
__BalloonHeapPushDown(
    IN  PPFN_NUMBER Heap,
    IN  ULONG       Start,
    IN  ULONG       Count
    )
{
    ULONG           LeftChild;
    ULONG           RightChild;

again:
    LeftChild = Start * 2 + 1;
    RightChild = Start * 2 + 2;

    if (RightChild < Count) {
        ASSERT(Heap[LeftChild] != Heap[Start]);
        ASSERT(Heap[RightChild] != Heap[Start]);
        ASSERT(Heap[LeftChild] != Heap[RightChild]);

        if (Heap[LeftChild] < Heap[Start] &&
            Heap[RightChild] < Heap[Start])
            return;

        if (Heap[LeftChild] < Heap[Start] &&
            Heap[RightChild] > Heap[Start]) {
            SWAP_NODES(Heap, RightChild, Start);
            ASSERT(Heap[RightChild] < Heap[Start]);
            Start = RightChild;
            goto again;
        }

        if (Heap[RightChild] < Heap[Start] &&
            Heap[LeftChild] > Heap[Start]) {
            SWAP_NODES(Heap, LeftChild, Start);
            ASSERT(Heap[LeftChild] < Heap[Start]);
            Start = LeftChild;
            goto again;
        }

        // Heap[LeftChild] > Heap[Start] && Heap[RightChild] > Heap[Start]
        if (Heap[LeftChild] > Heap[RightChild]) {
            SWAP_NODES(Heap, LeftChild, Start);
            ASSERT(Heap[LeftChild] < Heap[Start]);
            Start = LeftChild;
        } else {
            SWAP_NODES(Heap, RightChild, Start);
            ASSERT(Heap[RightChild] < Heap[Start]);
            Start = RightChild;
        }

        goto again;
    }

    if (LeftChild < Count) {    // Only one child
        ASSERT(Heap[LeftChild] != Heap[Start]);
        if (Heap[LeftChild] < Heap[Start])
            return;

        SWAP_NODES(Heap, LeftChild, Start);
        ASSERT(Heap[LeftChild] < Heap[Start]);
        Start = LeftChild;
        goto again;
    }
}

// Turn an array of PFNs into a max heap (largest node at root)
static FORCEINLINE VOID
__BalloonCreateHeap(
    IN  PPFN_NUMBER PfnArray,
    IN  ULONG       Count
    )
{
    LONG            Index = (LONG)Count;

    while (--Index >= 0)
        __BalloonHeapPushDown(PfnArray, (ULONG)Index, Count);
}

#if BALLOON_AUDIT
static FORCEINLINE VOID
__BalloonAuditHeap(
    IN  PPFN_NUMBER         PfnArray,
    IN  ULONG               Count
    )
{
    ULONG                   Index;
    BOOLEAN                 Correct;

    Correct = TRUE;

    for (Index = 0; Index < Count / 2; Index++) {
        ULONG   LeftChild = Index * 2 + 1;
        ULONG   RightChild = Index * 2 + 2;

        if (LeftChild < Count) {
            if (PfnArray[Index] <= PfnArray[LeftChild]) {
                Trace("PFN[%d] (%p) <= PFN[%d] (%p)\n",
                      Index,
                      PfnArray[Index],
                      LeftChild,
                      PfnArray[LeftChild]);
                Correct = FALSE;
            }
        }
        if (RightChild < Count) {
            if (PfnArray[Index] <= PfnArray[RightChild]) {
                Trace("PFN[%d] (%p) <= PFN[%d] (%p)\n",
                      Index,
                      PfnArray[Index],
                      RightChild,
                      PfnArray[RightChild]);
                Correct = FALSE;
            }
        }
    }

    ASSERT(Correct);
}
#endif

static DECLSPEC_NOINLINE VOID
BalloonSortPfnArray(
    IN  PPFN_NUMBER PfnArray,
    IN  ULONG       Count
    )
{
    ULONG           Unsorted;
    ULONG           Index;

    // Heap sort to keep stack usage down
    __BalloonCreateHeap(PfnArray, Count);

#if BALLOON_AUDIT
    __BalloonAuditHeap(PfnArray, Count);
#endif

    for (Unsorted = Count; Unsorted != 0; --Unsorted) {
        SWAP_NODES(PfnArray, 0, Unsorted - 1);
        __BalloonHeapPushDown(PfnArray, 0, Unsorted - 1);

#if BALLOON_AUDIT
        __BalloonAuditHeap(PfnArray, Unsorted - 1);
#endif
    }

    for (Index = 0; Index < Count - 1; Index++)
        ASSERT3U(PfnArray[Index], <, PfnArray[Index + 1]);
}

static FORCEINLINE PMDL
__BalloonAllocatePagesForMdl(
    IN  ULONG       Count
    )
{
    LARGE_INTEGER   LowAddress;
    LARGE_INTEGER   HighAddress;
    LARGE_INTEGER   SkipBytes;
    SIZE_T          TotalBytes;
    PMDL            Mdl;

    LowAddress.QuadPart = 0ull;
    HighAddress.QuadPart = ~0ull;
    SkipBytes.QuadPart = 0ull;
    TotalBytes = (SIZE_T)Count << PAGE_SHIFT;
    
    Mdl = MmAllocatePagesForMdlEx(LowAddress,
                                  HighAddress,
                                  SkipBytes,
                                  TotalBytes,
                                  MmCached,
                                  MM_DONT_ZERO_ALLOCATION);
    if (Mdl == NULL)
        goto done;

    ASSERT((Mdl->MdlFlags & (MDL_MAPPED_TO_SYSTEM_VA |
                             MDL_PARTIAL_HAS_BEEN_MAPPED |
                             MDL_PARTIAL |
                             MDL_PARENT_MAPPED_SYSTEM_VA |
                             MDL_SOURCE_IS_NONPAGED_POOL |
                             MDL_IO_SPACE)) == 0);

done:
    return Mdl;
}

static FORCEINLINE VOID
__BalloonFreePagesFromMdl(
    IN  PMDL        Mdl,
    IN  BOOLEAN     Check
    )
{
    volatile UCHAR  *Mapping;
    ULONG           Index;

    if (!Check)
        goto done;

    // Sanity check:
    //
    // Make sure that things written to the page really do stick. 
    // If the page is still ballooned out at the hypervisor level
    // then writes will be discarded and reads will give back
    // all 1s.

    Mapping = MmMapLockedPagesSpecifyCache(Mdl,
                                           KernelMode,
                                           MmCached,
                                           NULL,
                                           FALSE,
                                           LowPagePriority);
    if (Mapping == NULL)
        // Windows couldn't map the memory. That's kind of sad, but not
        // really an error: it might be that we're very low on kernel
        // virtual address space.
        goto done;

    // Write and read the first byte in each page to make sure it's backed
    // by RAM.
    ASSERT((Mdl->ByteCount & (PAGE_SIZE - 1)) == 0);

    for (Index = 0; Index < Mdl->ByteCount >> PAGE_SHIFT; Index++)
        Mapping[Index << PAGE_SHIFT] = (UCHAR)Index;

    for (Index = 0; Index < Mdl->ByteCount >> PAGE_SHIFT; Index++)
        ASSERT3U(Mapping[Index << PAGE_SHIFT], ==, (UCHAR)Index);

    MmUnmapLockedPages((PVOID)Mapping, Mdl);

done:
    MmFreePagesFromMdl(Mdl);
}

#define MIN_PAGES_PER_S 10000ull

static FORCEINLINE ULONG
__BalloonAllocatePfnArray(
    IN      PXENBUS_BALLOON Balloon,
    IN      ULONG           Requested,
    IN OUT  PBOOLEAN        Slow
    )
{
    LARGE_INTEGER           Start;
    LARGE_INTEGER           End;
    ULONGLONG               TimeDelta;
    ULONGLONG               Rate;
    PMDL                    Mdl;
    PPFN_NUMBER             PfnArray;
    ULONG                   Count;

    ASSERT(Requested != 0);
    ASSERT3U(Requested, <=, BALLOON_PFN_ARRAY_SIZE);
    ASSERT(IsZeroMemory(Balloon->PfnArray, Requested * sizeof (PFN_NUMBER)));

    KeQuerySystemTime(&Start);
    Count = 0;

    Mdl = __BalloonAllocatePagesForMdl(Requested);
    if (Mdl == NULL)
        goto done;

    ASSERT(Mdl->ByteOffset == 0);
    ASSERT((Mdl->ByteCount & (PAGE_SIZE - 1)) == 0);
    ASSERT(Mdl->MdlFlags & MDL_PAGES_LOCKED);

    Count = Mdl->ByteCount >> PAGE_SHIFT;

    PfnArray = MmGetMdlPfnArray(Mdl);
    RtlCopyMemory(Balloon->PfnArray, PfnArray, Count * sizeof (PFN_NUMBER));

    BalloonSortPfnArray(Balloon->PfnArray, Count);

    ExFreePool(Mdl);

done:
    KeQuerySystemTime(&End);
    TimeDelta = __max(((End.QuadPart - Start.QuadPart) / 10000ull), 1);

    Rate = (ULONGLONG)(Count * 1000) / TimeDelta;
    *Slow = (Rate < MIN_PAGES_PER_S) ? TRUE : FALSE;

    Info("%u page(s) at %llu pages/s\n", Count, Rate);
    return Count;
}

static FORCEINLINE ULONG
__BalloonPopulatePhysmap(
    IN  ULONG       Requested,
    IN  PPFN_NUMBER PfnArray
    )
{
    LARGE_INTEGER           Start;
    LARGE_INTEGER           End;
    ULONGLONG               TimeDelta;
    ULONGLONG               Rate;
    ULONG                   Count;

    ASSERT(Requested != 0);

    KeQuerySystemTime(&Start);

    Count = MemoryPopulatePhysmap(Requested, PfnArray);

    KeQuerySystemTime(&End);
    TimeDelta = __max(((End.QuadPart - Start.QuadPart) / 10000ull), 1);

    Rate = (ULONGLONG)(Count * 1000) / TimeDelta;

    Info("%u page(s) at %llu pages/s\n", Count, Rate);
    return Count;
}

static FORCEINLINE ULONG
__BalloonPopulatePfnArray(
    IN      PXENBUS_BALLOON Balloon,
    IN      ULONG           Requested
    )
{
    LARGE_INTEGER           Start;
    LARGE_INTEGER           End;
    ULONGLONG               TimeDelta;
    ULONGLONG               Rate;
    ULONG                   Index;
    ULONG                   Count;

    ASSERT(Requested != 0);
    ASSERT3U(Requested, <=, BALLOON_PFN_ARRAY_SIZE);
    ASSERT(IsZeroMemory(Balloon->PfnArray, Requested * sizeof (PFN_NUMBER)));

    KeQuerySystemTime(&Start);

    for (Index = 0; Index < Requested; Index++) {
        LONGLONG    Pfn;
        NTSTATUS    status;

        status = RangeSetPop(Balloon->RangeSet, &Pfn);
        ASSERT(NT_SUCCESS(status));

        Balloon->PfnArray[Index] = (PFN_NUMBER)Pfn;
    }

    Count = __BalloonPopulatePhysmap(Requested, Balloon->PfnArray);

    Index = Count;
    while (Index < Requested) {
        NTSTATUS    status;

        status = RangeSetPut(Balloon->RangeSet,
                             (LONGLONG)Balloon->PfnArray[Index],
                             (LONGLONG)Balloon->PfnArray[Index]);
        ASSERT(NT_SUCCESS(status));

        Balloon->PfnArray[Index] = 0;
        Index++;
    }

    KeQuerySystemTime(&End);
    TimeDelta = __max(((End.QuadPart - Start.QuadPart) / 10000ull), 1);

    Rate = (ULONGLONG)(Count * 1000) / TimeDelta;

    Info("%u page(s) at %llu pages/s\n", Count, Rate);
    return Count;
}

static FORCEINLINE ULONG
__BalloonDecreaseReservation(
    IN  ULONG       Requested,
    IN  PPFN_NUMBER PfnArray
    )
{
    LARGE_INTEGER           Start;
    LARGE_INTEGER           End;
    ULONGLONG               TimeDelta;
    ULONGLONG               Rate;
    ULONG                   Count;

    ASSERT(Requested != 0);

    KeQuerySystemTime(&Start);

    Count = MemoryDecreaseReservation(Requested, PfnArray);

    KeQuerySystemTime(&End);
    TimeDelta = __max(((End.QuadPart - Start.QuadPart) / 10000ull), 1);

    Rate = (ULONGLONG)(Count * 1000) / TimeDelta;

    Info("%u page(s) at %llu pages/s\n", Count, Rate);
    return Count;
}

static FORCEINLINE ULONG
__BalloonReleasePfnArray(
    IN      PXENBUS_BALLOON Balloon,
    IN      ULONG           Requested
    )
{
    LARGE_INTEGER           Start;
    LARGE_INTEGER           End;
    ULONGLONG               TimeDelta;
    ULONGLONG               Rate;
    ULONG                   Index;
    ULONG                   Count;

    ASSERT3U(Requested, <=, BALLOON_PFN_ARRAY_SIZE);

    KeQuerySystemTime(&Start);
    Count = 0;

    if (Requested == 0)
        goto done;

    Index = 0;
    while (Index < Requested) {
        ULONG       Next = Index;
        NTSTATUS    status;

        while (Next + 1 < Requested) {
            ASSERT3U((ULONGLONG)Balloon->PfnArray[Next], <, (ULONGLONG)Balloon->PfnArray[Next + 1]);

            if ((ULONGLONG)Balloon->PfnArray[Next + 1] != (ULONGLONG)Balloon->PfnArray[Next] + 1)
                break;

            Next++;
        }

        status = RangeSetPut(Balloon->RangeSet,
                             (LONGLONG)Balloon->PfnArray[Index],
                             (LONGLONG)Balloon->PfnArray[Next]);
        if (!NT_SUCCESS(status))
            break;

        Index = Next + 1;
    }
    Requested = Index;

    Count = __BalloonDecreaseReservation(Requested, Balloon->PfnArray);

#pragma warning(push)
#pragma warning(disable:6386)

    RtlZeroMemory(Balloon->PfnArray, Count * sizeof (PFN_NUMBER));

#pragma warning(pop)

    for (Index = Count; Index < Requested; Index++) {
        NTSTATUS    status;

        status = RangeSetGet(Balloon->RangeSet, (LONGLONG)Balloon->PfnArray[Index]);
        ASSERT(NT_SUCCESS(status));

        Balloon->PfnArray[Index] = 0;
    }

done:
    ASSERT(IsZeroMemory(Balloon->PfnArray, Requested * sizeof (PFN_NUMBER)));

    KeQuerySystemTime(&End);
    TimeDelta = __max(((End.QuadPart - Start.QuadPart) / 10000ull), 1);

    Rate = (ULONGLONG)(Count * 1000) / TimeDelta;

    Info("%u page(s) at %llu pages/s\n", Count, Rate);
    return Count;
}

static FORCEINLINE ULONG
__BalloonFreePfnArray(
    IN      PXENBUS_BALLOON Balloon,
    IN      ULONG           Requested,
    IN      BOOLEAN         Check
    )
{
    LARGE_INTEGER           Start;
    LARGE_INTEGER           End;
    ULONGLONG               TimeDelta;
    ULONGLONG               Rate;
    ULONG                   Index;
    ULONG                   Count;
    PMDL                    Mdl;

    ASSERT3U(Requested, <=, BALLOON_PFN_ARRAY_SIZE);

    KeQuerySystemTime(&Start);
    Count = 0;

    if (Requested == 0)
        goto done;

    ASSERT(IsZeroMemory(&Balloon->Mdl, sizeof (MDL)));

    for (Index = 0; Index < Requested; Index++)
        ASSERT(Balloon->PfnArray[Index] != 0);

#pragma warning(push)
#pragma warning(disable:28145)

    Mdl = &Balloon->Mdl;
    Mdl->Next = NULL;
    Mdl->Size = (SHORT)(sizeof(MDL) + (sizeof(PFN_NUMBER) * Requested));
    Mdl->MdlFlags = MDL_PAGES_LOCKED;
    Mdl->Process = NULL;
    Mdl->MappedSystemVa = NULL;
    Mdl->StartVa = NULL;
    Mdl->ByteCount = Requested << PAGE_SHIFT;
    Mdl->ByteOffset = 0;

#pragma warning(pop)

    __BalloonFreePagesFromMdl(Mdl, Check);
    Count = Requested;

    RtlZeroMemory(&Balloon->Mdl, sizeof (MDL));

#pragma warning(push)
#pragma warning(disable:6386)

    RtlZeroMemory(Balloon->PfnArray, Count * sizeof (PFN_NUMBER));

#pragma warning(pop)

done:
    ASSERT(IsZeroMemory(Balloon->PfnArray, Requested * sizeof (PFN_NUMBER)));

    KeQuerySystemTime(&End);
    TimeDelta = __max(((End.QuadPart - Start.QuadPart) / 10000ull), 1);

    Rate = (ULONGLONG)(Count * 1000) / TimeDelta;

    Info("%u page(s) at %llu pages/s\n", Count, Rate);
    return Count;
}

static DECLSPEC_NOINLINE BOOLEAN
BalloonDeflate(
    IN  PXENBUS_BALLOON Balloon,
    IN  ULONGLONG       Requested
    )
{
    LARGE_INTEGER       Start;
    LARGE_INTEGER       End;
    BOOLEAN             Abort;
    ULONGLONG           Count;
    ULONGLONG           TimeDelta;

    Info("====> %llu page(s)\n", Requested);

    KeQuerySystemTime(&Start);

    Count = 0;
    Abort = FALSE;

    while (Count < Requested && !Abort) {
        ULONG   ThisTime = (ULONG)__min(Requested - Count, BALLOON_PFN_ARRAY_SIZE);
        ULONG   Populated;
        ULONG   Freed;

        Populated = __BalloonPopulatePfnArray(Balloon, ThisTime);
        if (Populated < ThisTime)
            Abort = TRUE;

        Freed = __BalloonFreePfnArray(Balloon, Populated, TRUE);
        ASSERT(Freed == Populated);

        Count += Freed;
    }

    KeQuerySystemTime(&End);

    TimeDelta = (End.QuadPart - Start.QuadPart) / 10000ull;

    Info("<==== %llu page(s) in %llums\n", Count, TimeDelta);
    Balloon->Size -= Count;

    return Abort;
}

static DECLSPEC_NOINLINE BOOLEAN
BalloonInflate(
    IN  PXENBUS_BALLOON Balloon,
    IN  ULONGLONG       Requested
    )
{
    LARGE_INTEGER       Start;
    LARGE_INTEGER       End;
    BOOLEAN             Abort;
    ULONGLONG           Count;
    ULONGLONG           TimeDelta;

    Info("====> %llu page(s)\n", Requested);

    KeQuerySystemTime(&Start);

    Count = 0;
    Abort = FALSE;

    while (Count < Requested && !Abort) {
        ULONG   ThisTime = (ULONG)__min(Requested - Count, BALLOON_PFN_ARRAY_SIZE);
        ULONG   Allocated;
        BOOLEAN Slow;
        ULONG   Released;

        Allocated = __BalloonAllocatePfnArray(Balloon, ThisTime, &Slow);
        if (Allocated < ThisTime || Slow)
            Abort = TRUE;

        Released = __BalloonReleasePfnArray(Balloon, Allocated);

        if (Released < Allocated) {
            ULONG   Freed;

            RtlMoveMemory(&(Balloon->PfnArray[0]),
                          &(Balloon->PfnArray[Released]),
                          (Allocated - Released) * sizeof (PFN_NUMBER));

            Freed = __BalloonFreePfnArray(Balloon, Allocated - Released, FALSE);
            ASSERT3U(Freed, ==, Allocated - Released);
        }

        if (Released == 0)
            Abort = TRUE;

        Count += Released;
    }

    KeQuerySystemTime(&End);

    TimeDelta = (End.QuadPart - Start.QuadPart) / 10000ull;

    Info("<==== %llu page(s) in %llums\n", Count, TimeDelta);
    Balloon->Size += Count;

    return Abort;
}

static FORCEINLINE BOOLEAN
__BalloonLowMemory(
    IN  PXENBUS_BALLOON Balloon
    )
{
    LARGE_INTEGER       Timeout;
    NTSTATUS            status;

    Timeout.QuadPart = 0;

    status = KeWaitForSingleObject(Balloon->LowMemoryEvent,
                                   Executive,
                                   KernelMode,
                                   FALSE,
                                   &Timeout);

    return (status == STATUS_SUCCESS) ? TRUE : FALSE;
}

BOOLEAN
BalloonAdjust(
    IN  PXENBUS_BALLOON Balloon,
    IN  ULONGLONG       Target,
    IN  BOOLEAN         AllowInflation,
    IN  BOOLEAN         AllowDeflation
    )
{
    BOOLEAN             Abort;

    ASSERT3U(KeGetCurrentIrql(), <, DISPATCH_LEVEL);

    Info("====> (%llu page(s))\n", Balloon->Size);

    Abort = FALSE;

    AcquireMutex(&Balloon->Mutex);

    for (;;) {
        if (Target > Balloon->Size)
            Abort = !AllowInflation || __BalloonLowMemory(Balloon) || BalloonInflate(Balloon, Target - Balloon->Size);
        else if (Target < Balloon->Size)
            Abort = !AllowDeflation || BalloonDeflate(Balloon, Balloon->Size - Target);

        if (Target == Balloon->Size || Abort)
            break;
    }

    ReleaseMutex(&Balloon->Mutex);

    Info("<==== (%llu page(s))\n", Balloon->Size);

    return !Abort;
}

ULONGLONG
BalloonGetSize(
    IN  PXENBUS_BALLOON Balloon
    )
{
    ULONGLONG           Size;

    AcquireMutex(&Balloon->Mutex);
    Size = Balloon->Size;
    ReleaseMutex(&Balloon->Mutex);

    return Size;
}

NTSTATUS
BalloonInitialize(
    OUT PXENBUS_BALLOON *Balloon
    )
{
    UNICODE_STRING      Unicode;
    NTSTATUS            status;

    *Balloon = __BalloonAllocate(sizeof (XENBUS_BALLOON));

    status = STATUS_NO_MEMORY;
    if (*Balloon == NULL)
        goto fail1;

    status = RangeSetInitialize(&(*Balloon)->RangeSet);
    if (!NT_SUCCESS(status))
        goto fail2;

    InitializeMutex(&(*Balloon)->Mutex);

    RtlInitUnicodeString(&Unicode, L"\\KernelObjects\\LowMemoryCondition");

    (*Balloon)->LowMemoryEvent = IoCreateNotificationEvent(&Unicode,
                                                           &(*Balloon)->LowMemoryHandle);

    status = STATUS_UNSUCCESSFUL;
    if ((*Balloon)->LowMemoryEvent == NULL)
        goto fail3;

    return STATUS_SUCCESS;

fail3:
    Error("fail3\n");

    RtlZeroMemory(&(*Balloon)->Mutex, sizeof (MUTEX));

fail2:
    Error("fail2\n");

    ASSERT(IsZeroMemory(*Balloon, sizeof (XENBUS_BALLOON)));
    __BalloonFree(*Balloon);

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

VOID
BalloonTeardown(
    IN  PXENBUS_BALLOON    Balloon
    )
{
    ZwClose(Balloon->LowMemoryHandle);
    Balloon->LowMemoryHandle = NULL;
    Balloon->LowMemoryEvent = NULL;

    RtlZeroMemory(&Balloon->Mutex, sizeof (MUTEX));

    RangeSetTeardown(Balloon->RangeSet);
    Balloon->RangeSet = NULL;

    ASSERT(IsZeroMemory(Balloon, sizeof (XENBUS_BALLOON)));
    __BalloonFree(Balloon);
}
