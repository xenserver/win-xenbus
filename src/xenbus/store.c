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
#include <stdarg.h>
#include <stdlib.h>
#include <xen.h>
#include <util.h>

#include "store.h"
#include "evtchn.h"
#include "fdo.h"
#include "dbg_print.h"
#include "assert.h"

#define STORE_TRANSACTION_MAGIC 'NART'

struct _XENBUS_STORE_TRANSACTION {
    LIST_ENTRY  ListEntry;
    ULONG       Magic;
    PVOID       Caller;
    uint32_t    Id;
    BOOLEAN     Active; // Must be tested at >= DISPATCH_LEVEL
};

#define STORE_WATCH_MAGIC 'CTAW'

struct _XENBUS_STORE_WATCH {
    LIST_ENTRY  ListEntry;
    ULONG       Magic;
    PVOID       Caller;
    USHORT      Id;
    PCHAR       Path;
    PKEVENT     Event;
    BOOLEAN     Active; // Must be tested at >= DISPATCH_LEVEL
};

typedef enum _STORE_REQUEST_STATE {
    REQUEST_INVALID = 0,
    REQUEST_PREPARED,
    REQUEST_SUBMITTED,
    REQUEST_PENDING,
    REQUEST_COMPLETED
} STORE_REQUEST_STATE, *PSTORE_REQUEST_STATE;

typedef struct _STORE_SEGMENT {
    PCHAR   Data;
    ULONG   Offset;
    ULONG   Length;
} STORE_SEGMENT, *PSTORE_SEGMENT;

enum {
    RESPONSE_HEADER_SEGMENT = 0,
    RESPONSE_PAYLOAD_SEGMENT,
    RESPONSE_SEGMENT_COUNT
};

typedef struct _STORE_RESPONSE {
    struct xsd_sockmsg  Header;
    CHAR                Data[XENSTORE_PAYLOAD_MAX];
    STORE_SEGMENT       Segment[RESPONSE_SEGMENT_COUNT];
    ULONG               Index;
} STORE_RESPONSE, *PSTORE_RESPONSE;

#define REQUEST_SEGMENT_COUNT   8

typedef struct _STORE_REQUEST {
    volatile STORE_REQUEST_STATE State;
    struct xsd_sockmsg  Header;
    STORE_SEGMENT       Segment[REQUEST_SEGMENT_COUNT];
    ULONG               Count;
    ULONG               Index;
    LIST_ENTRY          ListEntry;
    PSTORE_RESPONSE     Response;
} STORE_REQUEST, *PSTORE_REQUEST;

#define STORE_BUFFER_MAGIC 'FFUB'

typedef struct _STORE_BUFFER {
    LIST_ENTRY  ListEntry;
    ULONG       Magic;
    PVOID       Caller;
    CHAR        Data[1];
} STORE_BUFFER, *PSTORE_BUFFER;

struct _XENBUS_STORE_CONTEXT {
    LONG                                References;
    struct xenstore_domain_interface    *Shared;
    KSPIN_LOCK                          Lock;
    USHORT                              RequestId;
    LIST_ENTRY                          SubmittedList;
    LIST_ENTRY                          PendingList;
    LIST_ENTRY                          TransactionList;
    USHORT                              WatchId;
    LIST_ENTRY                          WatchList;
    LIST_ENTRY                          BufferList;
    KDPC                                Dpc;
    STORE_RESPONSE                      Response;
    PXENBUS_EVTCHN_INTERFACE            EvtchnInterface;
    PFN_NUMBER                          Pfn;
    PXENBUS_EVTCHN_DESCRIPTOR           Evtchn;
    PXENBUS_SUSPEND_INTERFACE           SuspendInterface;
    PXENBUS_DEBUG_INTERFACE             DebugInterface;
    PXENBUS_SUSPEND_CALLBACK            SuspendCallbackEarly;
    PXENBUS_SUSPEND_CALLBACK            SuspendCallbackLate;
    PXENBUS_DEBUG_CALLBACK              DebugCallback;
};

C_ASSERT(sizeof (struct xenstore_domain_interface) <= PAGE_SIZE);

#define STORE_TAG   'ROTS'

static FORCEINLINE PVOID
__StoreAllocate(
    IN  ULONG   Length
    )
{
    return __AllocateNonPagedPoolWithTag(Length, STORE_TAG);
}

static FORCEINLINE VOID
__StoreFree(
    IN  PVOID   Buffer
    )
{
    __FreePoolWithTag(Buffer, STORE_TAG);
}

static DECLSPEC_NOINLINE NTSTATUS
StorePrepareRequest(
    IN  PXENBUS_STORE_CONTEXT       Context,
    OUT PSTORE_REQUEST              Request,
    IN  PXENBUS_STORE_TRANSACTION   Transaction OPTIONAL,
    IN  enum xsd_sockmsg_type       Type,
    IN  ...
    )
{
    ULONG                           Id;
    KIRQL                           Irql;
    PSTORE_SEGMENT                  Segment;
    va_list                         Arguments;
    NTSTATUS                        status;

    ASSERT(IsZeroMemory(Request, sizeof (STORE_REQUEST)));

    if (Transaction != NULL) {
        status = STATUS_UNSUCCESSFUL;
        if (!Transaction->Active)
            goto fail1;

        Id = Transaction->Id;
    } else {
        Id = 0;
    }

    Request->Header.type = Type;
    Request->Header.tx_id = Id;
    Request->Header.len = 0;

    KeAcquireSpinLock(&Context->Lock, &Irql);
    Request->Header.req_id = Context->RequestId++;
    KeReleaseSpinLock(&Context->Lock, Irql);

    Request->Count = 0;
    Segment = &Request->Segment[Request->Count++];

    Segment->Data = (PCHAR)&Request->Header;
    Segment->Offset = 0;
    Segment->Length = sizeof (struct xsd_sockmsg);

    va_start(Arguments, Type);
    for (;;) {
        PCHAR   Data;
        ULONG   Length;

        Data = va_arg(Arguments, PCHAR);
        Length = va_arg(Arguments, ULONG);
        
        if (Data == NULL) {
            ASSERT3U(Length, ==, 0);
            break;
        }

        Segment = &Request->Segment[Request->Count++];
        ASSERT3U(Request->Count, <, REQUEST_SEGMENT_COUNT);

        Segment->Data = Data;
        Segment->Offset = 0;
        Segment->Length = Length;

        Request->Header.len += Segment->Length;
    }
    va_end(Arguments);

    Request->State = REQUEST_PREPARED;

    return STATUS_SUCCESS;

fail1:
    return status;
}

static FORCEINLINE ULONG
__StoreCopyToRing(
    IN  PXENBUS_STORE_CONTEXT           Context,
    IN  PCHAR                           Data,
    IN  ULONG                           Length
    )
{
    struct xenstore_domain_interface    *Shared;
    XENSTORE_RING_IDX                   cons;
    XENSTORE_RING_IDX                   prod;
    ULONG                               Offset;

    Shared = Context->Shared;

    KeMemoryBarrier();

    prod = Shared->req_prod;
    cons = Shared->req_cons;

    KeMemoryBarrier();

    Offset = 0;
    while (Length != 0) {
        ULONG   Available;
        ULONG   Index;
        ULONG   CopyLength;

        Available = cons + XENSTORE_RING_SIZE - prod;

        if (Available == 0)
            break;

        Index = MASK_XENSTORE_IDX(prod);

        CopyLength = __min(Length, Available);
        CopyLength = __min(CopyLength, XENSTORE_RING_SIZE - Index);

        RtlCopyMemory(&Shared->req[Index], Data + Offset, CopyLength);

        Offset += CopyLength;
        Length -= CopyLength;

        prod += CopyLength;
    }

    KeMemoryBarrier();

    Shared->req_prod = prod;

    KeMemoryBarrier();

    return Offset;    
}

static FORCEINLINE NTSTATUS
__StoreSendSegment(
    IN      PXENBUS_STORE_CONTEXT   Context,
    IN OUT  PSTORE_SEGMENT          Segment,
    IN OUT  PULONG                  Written
    )
{
    ULONG                           Copied;

    Copied = __StoreCopyToRing(Context,
                               Segment->Data + Segment->Offset,
                               Segment->Length - Segment->Offset);

    Segment->Offset += Copied;
    *Written += Copied;

    ASSERT3U(Segment->Offset, <=, Segment->Length);
    return (Segment->Offset == Segment->Length) ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
}

static VOID
StoreSendRequests(
    IN      PXENBUS_STORE_CONTEXT   Context,
    IN OUT  PULONG                  Written
    )
{
    if (IsListEmpty(&Context->SubmittedList))
        return;

    while (!IsListEmpty(&Context->SubmittedList)) {
        PLIST_ENTRY      ListEntry;
        PSTORE_REQUEST   Request;

        ListEntry = Context->SubmittedList.Flink;
        ASSERT3P(ListEntry, !=, &Context->SubmittedList);

        Request = CONTAINING_RECORD(ListEntry, STORE_REQUEST, ListEntry);

        ASSERT3U(Request->State, ==, REQUEST_SUBMITTED);

        while (Request->Index < Request->Count) {
            NTSTATUS    status;

            status = __StoreSendSegment(Context,
                                        &Request->Segment[Request->Index],
                                        Written);
            if (!NT_SUCCESS(status))
                break;

            Request->Index++;
        }

        if (Request->Index < Request->Count)
            break;

        ListEntry = RemoveHeadList(&Context->SubmittedList);
        ASSERT3P(ListEntry, ==, &Request->ListEntry);

        InsertTailList(&Context->PendingList, &Request->ListEntry);
        Request->State = REQUEST_PENDING;
    }
}

static FORCEINLINE ULONG
__StoreCopyFromRing(
    IN  PXENBUS_STORE_CONTEXT           Context,
    IN  PCHAR                           Data,
    IN  ULONG                           Length
    )
{
    struct xenstore_domain_interface    *Shared;
    XENSTORE_RING_IDX                   cons;
    XENSTORE_RING_IDX                   prod;
    ULONG                               Offset;

    Shared = Context->Shared;

    KeMemoryBarrier();

    cons = Shared->rsp_cons;
    prod = Shared->rsp_prod;

    KeMemoryBarrier();

    Offset = 0;
    while (Length != 0) {
        ULONG   Available;
        ULONG   Index;
        ULONG   CopyLength;

        Available = prod - cons;

        if (Available == 0)
            break;

        Index = MASK_XENSTORE_IDX(cons);

        CopyLength = __min(Length, Available);
        CopyLength = __min(CopyLength, XENSTORE_RING_SIZE - Index);

        RtlCopyMemory(Data + Offset, &Shared->rsp[Index], CopyLength);

        Offset += CopyLength;
        Length -= CopyLength;

        cons += CopyLength;
    }

    KeMemoryBarrier();

    Shared->rsp_cons = cons;

    KeMemoryBarrier();

    return Offset;    
}

static FORCEINLINE NTSTATUS
__StoreReceiveSegment(
    IN      PXENBUS_STORE_CONTEXT   Context,
    IN OUT  PSTORE_SEGMENT          Segment,
    IN OUT  PULONG                  Read
    )
{
    ULONG                           Copied;

    Copied = __StoreCopyFromRing(Context,
                                 Segment->Data + Segment->Offset,
                                 Segment->Length - Segment->Offset);

    Segment->Offset += Copied;
    *Read += Copied;

    ASSERT3U(Segment->Offset, <=, Segment->Length);
    return (Segment->Offset == Segment->Length) ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
}

static FORCEINLINE BOOLEAN
__StoreIgnoreHeaderType(
    IN  ULONG   Type
    )
{
    switch (Type) {
    case XS_DEBUG:
    case XS_GET_PERMS:
    case XS_INTRODUCE:
    case XS_RELEASE:
    case XS_GET_DOMAIN_PATH:
    case XS_MKDIR:
    case XS_SET_PERMS:
    case XS_IS_DOMAIN_INTRODUCED:
    case XS_RESUME:
    case XS_SET_TARGET:
    case XS_RESTRICT:
        return TRUE;
    default:
        return FALSE;
    }
}

static FORCEINLINE BOOLEAN
__StoreVerifyHeader(
    struct xsd_sockmsg  *Header
    )
{
    BOOLEAN             Valid;

    Valid = TRUE;

    if (Header->type != XS_DIRECTORY &&
        Header->type != XS_READ &&
        Header->type != XS_WATCH &&
        Header->type != XS_UNWATCH &&
        Header->type != XS_TRANSACTION_START &&
        Header->type != XS_TRANSACTION_END &&
        Header->type != XS_WRITE &&
        Header->type != XS_RM &&
        Header->type != XS_WATCH_EVENT &&
        Header->type != XS_ERROR &&
        !__StoreIgnoreHeaderType(Header->type)) {
        Error("UNRECOGNIZED TYPE 0x%08x\n", Header->type);
        Valid = FALSE;
    }

    if (Header->len >= XENSTORE_PAYLOAD_MAX) {
        Error("ILLEGAL LENGTH 0x%08x\n", Header->len);
        Valid = FALSE;
    }

    return Valid;    
}

static FORCEINLINE NTSTATUS
__StoreReceiveResponse(
    IN      PXENBUS_STORE_CONTEXT   Context,
    IN OUT  PULONG                  Read
    )
{
    PSTORE_RESPONSE                 Response = &Context->Response;
    NTSTATUS                        status;

    if (Response->Segment[RESPONSE_PAYLOAD_SEGMENT].Data != NULL)
        goto payload;

    status = __StoreReceiveSegment(Context, &Response->Segment[RESPONSE_HEADER_SEGMENT], Read);
    if (!NT_SUCCESS(status))
        goto done;

    ASSERT(__StoreVerifyHeader(&Response->Header));

    if (Response->Header.len == 0)
        goto done;

    Response->Segment[RESPONSE_PAYLOAD_SEGMENT].Length = Response->Header.len;
    Response->Segment[RESPONSE_PAYLOAD_SEGMENT].Data = Response->Data;

payload:
    status = __StoreReceiveSegment(Context, &Response->Segment[RESPONSE_PAYLOAD_SEGMENT], Read);

done:
    return status;    
}

static FORCEINLINE PSTORE_REQUEST
__StoreFindRequest(
    IN  PXENBUS_STORE_CONTEXT   Context,
    IN  uint32_t                req_id
    )
{
    PLIST_ENTRY                 ListEntry;
    PSTORE_REQUEST              Request;

    Request = NULL;
    for (ListEntry = Context->PendingList.Flink;
         ListEntry != &Context->PendingList;
         ListEntry = ListEntry->Flink) {

        Request = CONTAINING_RECORD(ListEntry, STORE_REQUEST, ListEntry);

        if (Request->Header.req_id == req_id)
            break;

        Request = NULL;
    }

    return Request;
}

static FORCEINLINE PXENBUS_STORE_WATCH
__StoreFindWatch(
    IN  PXENBUS_STORE_CONTEXT   Context,
    IN  USHORT                  Id
    )
{
    PLIST_ENTRY                 ListEntry;
    PXENBUS_STORE_WATCH         Watch;

    Watch = NULL;
    for (ListEntry = Context->WatchList.Flink;
         ListEntry != &Context->WatchList;
         ListEntry = ListEntry->Flink) {

        Watch = CONTAINING_RECORD(ListEntry, XENBUS_STORE_WATCH, ListEntry);

        if (Watch->Id == Id)
            break;

        Watch = NULL;
    }

    return Watch;
}

static FORCEINLINE USHORT
__StoreNextWatchId(
    IN  PXENBUS_STORE_CONTEXT   Context
    )
{
    USHORT                      Id;
    PXENBUS_STORE_WATCH         Watch;

    do {
        Id = Context->WatchId++;
        Watch = __StoreFindWatch(Context, Id);
    } while (Watch != NULL);

    return Id;
}

#if defined(__i386__)
#define TOKEN_LENGTH    (sizeof ("TOK|XXXXXXXX|XXXX"))
#elif defined(__x86_64__)
#define TOKEN_LENGTH    (sizeof ("TOK|XXXXXXXXXXXXXXXX|XXXX"))
#else
#error 'Unrecognised architecture'
#endif

static FORCEINLINE NTSTATUS
__StoreParseWatchEvent(
    IN  PCHAR       Data,
    IN  ULONG       Length,
    OUT PCHAR       *Path,
    OUT PVOID       *Caller,
    OUT PUSHORT     Id
    )
{
    PCHAR           End;

    *Path = Data;
    while (*Data != '\0' && Length != 0) {
        Data++;
        --Length;
    }

    if (Length != TOKEN_LENGTH + 1)
        goto fail1;

    // Skip over the NUL
    Data++;
    --Length;

    if (Data[Length - 1] != '\0')
        goto fail2;

    if (strncmp(Data, "TOK|", 4) != 0) {
        Warning("UNRECOGNIZED PRE-AMBLE: %02X%02X%02X%02X\n",
                Data[0],
                Data[1],
                Data[2],
                Data[3]);

        goto fail3;
    }

    Data += 4;
    *Caller = (PVOID)(ULONG_PTR)_strtoui64(Data, &End, 16);

    if (*End != '|')
        goto fail4;

    Data = End + 1;
    *Id = (USHORT)strtoul(Data, &End, 16);

    if (*End != '\0')
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
    Error("fail1\n");

    return STATUS_UNSUCCESSFUL;
}

static FORCEINLINE VOID
__StoreProcessWatchEvent(
    IN  PXENBUS_STORE_CONTEXT   Context
    )
{
    PSTORE_RESPONSE             Response;
    PCHAR                       Path;
    PVOID                       Caller;
    USHORT                      Id;
    PXENBUS_STORE_WATCH         Watch;
    NTSTATUS                    status;

    Response = &Context->Response;

    ASSERT3U(Response->Header.req_id, ==, 0);

    status = __StoreParseWatchEvent(Response->Segment[RESPONSE_PAYLOAD_SEGMENT].Data,
                                    Response->Segment[RESPONSE_PAYLOAD_SEGMENT].Length,
                                    &Path,
                                    &Caller,
                                    &Id);
    if (!NT_SUCCESS(status))
        return;

    Trace("%04x (%s)\n", Id, Path);

    Watch = __StoreFindWatch(Context, Id);

    if (Watch == NULL) {
        PCHAR       Name;
        ULONG_PTR   Offset;

        ModuleLookup((ULONG_PTR)Caller, &Name, &Offset);
        if (Name != NULL)
            Warning("SPURIOUS WATCH EVENT (%s) FOR %s + %p\n",
                    Path,
                    Name,
                    Offset);
        else
            Warning("SPURIOUS WATCH EVENT (%s) FOR %p\n",
                    Path,
                    Caller);

        return;
    }

    ASSERT3P(Caller, ==, Watch->Caller);

    if (Watch->Active)
        KeSetEvent(Watch->Event, 0, FALSE);
}

static FORCEINLINE VOID
__StoreResetResponse(
    IN  PXENBUS_STORE_CONTEXT   Context
    )
{
    PSTORE_RESPONSE             Response;
    PSTORE_SEGMENT              Segment;

    Response = &Context->Response;

    RtlZeroMemory(Response, sizeof (STORE_RESPONSE));

    Segment = &Response->Segment[RESPONSE_HEADER_SEGMENT];

    Segment->Data = (PCHAR)&Response->Header;
    Segment->Offset = 0;
    Segment->Length = sizeof (struct xsd_sockmsg);
}

static FORCEINLINE PSTORE_RESPONSE
__StoreCopyResponse(
    IN  PXENBUS_STORE_CONTEXT   Context
    )
{
    PSTORE_RESPONSE             Response;
    PSTORE_SEGMENT              Segment;
    NTSTATUS                    status;

    Response = __StoreAllocate(sizeof (STORE_RESPONSE));

    status = STATUS_NO_MEMORY;
    if (Response == NULL)
        goto fail1;

    *Response = Context->Response;

    Segment = &Response->Segment[RESPONSE_HEADER_SEGMENT];
    ASSERT3P(Segment->Data, ==, (PCHAR)&Context->Response.Header);
    Segment->Data = (PCHAR)&Response->Header;

    Segment = &Response->Segment[RESPONSE_PAYLOAD_SEGMENT];
    if (Segment->Length != 0) {
        ASSERT3P(Segment->Data, ==, Context->Response.Data);
        Segment->Data = Response->Data;
    } else {
        ASSERT3P(Segment->Data, ==, NULL);
    }

    return Response;

fail1:
    Error("fail1 (%08x)\n", status);

    return NULL;
}

static FORCEINLINE VOID
__StoreFreeResponse(
    IN  PSTORE_RESPONSE Response
    )
{
    __StoreFree(Response);    
}

static FORCEINLINE VOID
__StoreProcessResponse(
    IN  PXENBUS_STORE_CONTEXT   Context
    )
{
    PSTORE_RESPONSE             Response;
    PSTORE_REQUEST              Request;

    Response = &Context->Response;

    if (__StoreIgnoreHeaderType(Response->Header.type)) {
        Warning("IGNORING RESPONSE TYPE %08X\n", Response->Header.type);
        __StoreResetResponse(Context);
        return;
    }

    if (Response->Header.type == XS_WATCH_EVENT) {
        __StoreProcessWatchEvent(Context);
        __StoreResetResponse(Context);
        return;
    }

    Request = __StoreFindRequest(Context, Response->Header.req_id);
    if (Request == NULL) {
        Warning("SPURIOUS RESPONSE ID %08X\n", Response->Header.req_id);
        __StoreResetResponse(Context);
        return;
    }

    ASSERT3U(Request->State, ==, REQUEST_PENDING);

    RemoveEntryList(&Request->ListEntry);

    Request->Response = __StoreCopyResponse(Context);
    __StoreResetResponse(Context);

    Request->State = REQUEST_COMPLETED;

    KeMemoryBarrier();
}

static FORCEINLINE VOID
__StorePoll(
    IN  PXENBUS_STORE_CONTEXT   Context
    )
{
    ULONG                       Read;
    ULONG                       Written;
    NTSTATUS                    status;

    ASSERT3U(KeGetCurrentIrql(), ==, DISPATCH_LEVEL);

    do {
        Read = Written = 0;

        StoreSendRequests(Context, &Written);
        if (Written != 0)
            (VOID) EVTCHN(Send,
                          Context->EvtchnInterface,
                          Context->Evtchn);

        status = __StoreReceiveResponse(Context, &Read);
        if (NT_SUCCESS(status))
            __StoreProcessResponse(Context);

        if (Read != 0)
            (VOID) EVTCHN(Send,
                          Context->EvtchnInterface,
                          Context->Evtchn);

    } while (Written != 0 || Read != 0);
}

#pragma warning(push)
#pragma warning(disable:6011)   // dereferencing NULL pointer

KDEFERRED_ROUTINE   StoreDpc;

VOID
StoreDpc(
    IN  PKDPC               Dpc,
    IN  PVOID               _Context,
    IN  PVOID               Argument1,
    IN  PVOID               Argument2
    )
{
    PXENBUS_STORE_CONTEXT   Context = _Context;

    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(Argument1);
    UNREFERENCED_PARAMETER(Argument2);

    ASSERT(Context != NULL);

    KeAcquireSpinLockAtDpcLevel(&Context->Lock);
    __StorePoll(Context);
    KeReleaseSpinLockFromDpcLevel(&Context->Lock);
}

#pragma warning(pop)

static PSTORE_RESPONSE
StoreSubmitRequest(
    IN  PXENBUS_STORE_CONTEXT   Context,
    IN  PSTORE_REQUEST          Request
    )
{
    PSTORE_RESPONSE             Response;
    KIRQL                       Irql;

    ASSERT3U(Request->State, ==, REQUEST_PREPARED);

    // Make sure we don't suspend
    ASSERT3U(KeGetCurrentIrql(), <=, DISPATCH_LEVEL);
    KeRaiseIrql(DISPATCH_LEVEL, &Irql);

    KeAcquireSpinLockAtDpcLevel(&Context->Lock);

    InsertTailList(&Context->SubmittedList, &Request->ListEntry);
    Request->State = REQUEST_SUBMITTED;

    while (Request->State != REQUEST_COMPLETED) {
        __StorePoll(Context);
        SchedYield();
    }

    KeReleaseSpinLockFromDpcLevel(&Context->Lock);

    Response = Request->Response;
    ASSERT(Response == NULL ||
           Response->Header.type == XS_ERROR ||
           Response->Header.type == Request->Header.type);

    RtlZeroMemory(Request, sizeof(STORE_REQUEST));

    KeLowerIrql(Irql);

    return Response;
}

static FORCEINLINE NTSTATUS
__StoreCheckResponse(
    IN  PSTORE_RESPONSE Response
    )
{
    NTSTATUS            status;

    status = STATUS_UNSUCCESSFUL;
    if (Response->Header.type == XS_ERROR) {
        ULONG   Index;

        for (Index = 0;
             Index < sizeof (xsd_errors) / sizeof (xsd_errors[0]);
             Index++) {
            struct xsd_errors   *Entry = &xsd_errors[Index];
            PCHAR               Error = Response->Segment[RESPONSE_PAYLOAD_SEGMENT].Data;
            ULONG               Length = Response->Segment[RESPONSE_PAYLOAD_SEGMENT].Length;
            
            if (strncmp(Error, Entry->errstring, Length) == 0) {
                ERRNO_TO_STATUS(Entry->errnum, status);
                break;
            }
        }

        goto fail1;
    }

    return STATUS_SUCCESS;

fail1:
    return status;
}

static FORCEINLINE PSTORE_BUFFER
__StoreCopyPayload(
    IN  PXENBUS_STORE_CONTEXT   Context,
    IN  PSTORE_RESPONSE         Response,
    IN  PVOID                   Caller
    )
{
    PCHAR                       Data = Response->Segment[RESPONSE_PAYLOAD_SEGMENT].Data;
    ULONG                       Length = Response->Segment[RESPONSE_PAYLOAD_SEGMENT].Length;
    PSTORE_BUFFER               Buffer;
    KIRQL                       Irql;
    NTSTATUS                    status;

    Buffer = __StoreAllocate(FIELD_OFFSET(STORE_BUFFER, Data) +
                             Length +
                             (sizeof (CHAR) * 2));  // Double-NUL terminate

    status  = STATUS_NO_MEMORY;
    if (Buffer == NULL)
        goto fail1;

    Buffer->Magic = STORE_BUFFER_MAGIC;
    Buffer->Caller = Caller;

    RtlCopyMemory(Buffer->Data, Data, Length);

    KeAcquireSpinLock(&Context->Lock, &Irql);
    InsertTailList(&Context->BufferList, &Buffer->ListEntry);
    KeReleaseSpinLock(&Context->Lock, Irql);

    return Buffer;        

fail1:
    Error("fail1 (%08x)\n", status);

    return NULL;
}

static FORCEINLINE VOID
__StoreFreePayload(
    IN  PXENBUS_STORE_CONTEXT   Context,
    IN  PSTORE_BUFFER           Buffer
    )
{
    KIRQL                       Irql;

    ASSERT3U(Buffer->Magic, ==, STORE_BUFFER_MAGIC);

    KeAcquireSpinLock(&Context->Lock, &Irql);
    RemoveEntryList(&Buffer->ListEntry);
    KeReleaseSpinLock(&Context->Lock, Irql);

    __StoreFree(Buffer);
}

static VOID
StoreFree(
    IN  PXENBUS_STORE_CONTEXT   Context,
    IN  PCHAR                   Value
    )
{
    PSTORE_BUFFER               Buffer;

    Buffer = CONTAINING_RECORD(Value, STORE_BUFFER, Data);

    __StoreFreePayload(Context, Buffer);
}

extern USHORT
RtlCaptureStackBackTrace(
    __in        ULONG   FramesToSkip,
    __in        ULONG   FramesToCapture,
    __out       PVOID   *BackTrace,
    __out_opt   PULONG  BackTraceHash
    );

static NTSTATUS
StoreRead(
    IN  PXENBUS_STORE_CONTEXT       Context,
    IN  PXENBUS_STORE_TRANSACTION   Transaction OPTIONAL,
    IN  PCHAR                       Prefix OPTIONAL,
    IN  PCHAR                       Node,
    OUT PCHAR                       *Value
    )
{
    PVOID                           Caller;
    STORE_REQUEST                   Request;
    PSTORE_RESPONSE                 Response;
    PSTORE_BUFFER                   Buffer;
    NTSTATUS                        status;

    (VOID) RtlCaptureStackBackTrace(1, 1, &Caller, NULL);    

    RtlZeroMemory(&Request, sizeof (STORE_REQUEST));

    if (Prefix == NULL) {
        status = StorePrepareRequest(Context,
                                     &Request,
                                     Transaction,
                                     XS_READ,
                                     Node, strlen(Node),
                                     "", 1,
                                     NULL, 0);
    } else {
        status = StorePrepareRequest(Context,
                                     &Request,
                                     Transaction,
                                     XS_READ,
                                     Prefix, strlen(Prefix),
                                     "/", 1,
                                     Node, strlen(Node),
                                     "", 1,
                                     NULL, 0);
    }

    if (!NT_SUCCESS(status))
        goto fail1;

    Response = StoreSubmitRequest(Context, &Request);

    status = STATUS_NO_MEMORY;
    if (Response == NULL)
        goto fail2;

    status = __StoreCheckResponse(Response);
    if (!NT_SUCCESS(status))
        goto fail3;

    Buffer = __StoreCopyPayload(Context, Response, Caller);

    status = STATUS_NO_MEMORY;
    if (Buffer == NULL)
        goto fail4;

    __StoreFreeResponse(Response);
    ASSERT(IsZeroMemory(&Request, sizeof (STORE_REQUEST)));

    *Value = Buffer->Data;

    return STATUS_SUCCESS;

fail4:
fail3:
    __StoreFreeResponse(Response);

fail2:
fail1:
    ASSERT(IsZeroMemory(&Request, sizeof (STORE_REQUEST)));

    return status;
}

NTSTATUS
StoreWrite(
    IN  PXENBUS_STORE_CONTEXT       Context,
    IN  PXENBUS_STORE_TRANSACTION   Transaction OPTIONAL,
    IN  PCHAR                       Prefix OPTIONAL,
    IN  PCHAR                       Node,
    IN  PCHAR                       Value
    )
{
    STORE_REQUEST                   Request;
    PSTORE_RESPONSE                 Response;
    NTSTATUS                        status;

    RtlZeroMemory(&Request, sizeof (STORE_REQUEST));

    if (Prefix == NULL) {
        status = StorePrepareRequest(Context,
                                     &Request,
                                     Transaction,
                                     XS_WRITE,
                                     Node, strlen(Node),
                                     "", 1,
                                     Value, strlen(Value),
                                     NULL, 0);
    } else {
        status = StorePrepareRequest(Context,
                                     &Request,
                                     Transaction,
                                     XS_WRITE,
                                     Prefix, strlen(Prefix),
                                     "/", 1,
                                     Node, strlen(Node),
                                     "", 1,
                                     Value, strlen(Value),
                                     NULL, 0);
    }

    if (!NT_SUCCESS(status))
        goto fail1;

    Response = StoreSubmitRequest(Context, &Request);

    status = STATUS_NO_MEMORY;
    if (Response == NULL)
        goto fail2;

    status = __StoreCheckResponse(Response);
    if (!NT_SUCCESS(status))
        goto fail3;

    __StoreFreeResponse(Response);
    ASSERT(IsZeroMemory(&Request, sizeof (STORE_REQUEST)));

    return STATUS_SUCCESS;

fail3:
    __StoreFreeResponse(Response);

fail2:
fail1:
    ASSERT(IsZeroMemory(&Request, sizeof (STORE_REQUEST)));

    return status;
}

static FORCEINLINE NTSTATUS
__StoreVPrintf(
    IN  PXENBUS_STORE_CONTEXT       Context,
    IN  PXENBUS_STORE_TRANSACTION   Transaction OPTIONAL,
    IN  PCHAR                       Prefix OPTIONAL,
    IN  PCHAR                       Node,
    IN  const CHAR                  *Format,
    IN  va_list                     Arguments
    )
{
    PCHAR                           Buffer;
    ULONG                           Length;
    NTSTATUS                        status;

    Length = 32;
    for (;;) {
        Buffer = __StoreAllocate(Length);

        status = STATUS_NO_MEMORY;
        if (Buffer == NULL)
            goto fail1;

        status = RtlStringCbVPrintfA(Buffer,
                                     Length,
                                     Format,
                                     Arguments);
        if (NT_SUCCESS(status))
            break;

        if (status != STATUS_BUFFER_OVERFLOW)
            goto fail2;

        __StoreFree(Buffer);
        Length <<= 1;

        ASSERT3U(Length, <=, 1024);
    }

    status = StoreWrite(Context,
                        Transaction,
                        Prefix,
                        Node,
                        Buffer);
    if (!NT_SUCCESS(status))
        goto fail3;

    __StoreFree(Buffer);

    return STATUS_SUCCESS;

fail3:
fail2:
    __StoreFree(Buffer);

fail1:
    return status;
}

static NTSTATUS
StorePrintf(
    IN  PXENBUS_STORE_CONTEXT       Context,
    IN  PXENBUS_STORE_TRANSACTION   Transaction OPTIONAL,
    IN  PCHAR                       Prefix OPTIONAL,
    IN  PCHAR                       Node,
    IN  const CHAR                  *Format,
    ...
    )
{
    va_list                         Arguments;
    NTSTATUS                        status;

    va_start(Arguments, Format);
    status = __StoreVPrintf(Context,
                            Transaction,
                            Prefix,
                            Node,
                            Format,
                            Arguments);
    va_end(Arguments);

    return status;
}

static NTSTATUS
StoreRemove(
    IN  PXENBUS_STORE_CONTEXT       Context,
    IN  PXENBUS_STORE_TRANSACTION   Transaction OPTIONAL,
    IN  PCHAR                       Prefix OPTIONAL,
    IN  PCHAR                       Node
    )
{
    STORE_REQUEST                   Request;
    PSTORE_RESPONSE                 Response;
    NTSTATUS                        status;

    RtlZeroMemory(&Request, sizeof (STORE_REQUEST));

    if (Prefix == NULL) {
        status = StorePrepareRequest(Context,
                                     &Request,
                                     Transaction,
                                     XS_RM,
                                     Node, strlen(Node),
                                     "", 1,
                                     NULL, 0);
    } else {
        status = StorePrepareRequest(Context,
                                     &Request,
                                     Transaction,
                                     XS_RM,
                                     Prefix, strlen(Prefix),
                                     "/", 1,
                                     Node, strlen(Node),
                                     "", 1,
                                     NULL, 0);
    }

    if (!NT_SUCCESS(status))
        goto fail1;

    Response = StoreSubmitRequest(Context, &Request);

    status = STATUS_NO_MEMORY;
    if (Response == NULL)
        goto fail2;

    status = __StoreCheckResponse(Response);
    if (!NT_SUCCESS(status))
        goto fail3;

    __StoreFreeResponse(Response);
    ASSERT(IsZeroMemory(&Request, sizeof (STORE_REQUEST)));

    return STATUS_SUCCESS;

fail3:
    __StoreFreeResponse(Response);

fail2:
fail1:
    ASSERT(IsZeroMemory(&Request, sizeof (STORE_REQUEST)));

    return status;
}

static NTSTATUS
StoreDirectory(
    IN  PXENBUS_STORE_CONTEXT       Context,
    IN  PXENBUS_STORE_TRANSACTION   Transaction OPTIONAL,
    IN  PCHAR                       Prefix OPTIONAL,
    IN  PCHAR                       Node,
    OUT PCHAR                       *Value
    )
{
    PVOID                           Caller;
    STORE_REQUEST                   Request;
    PSTORE_RESPONSE                 Response;
    PSTORE_BUFFER                   Buffer;
    NTSTATUS                        status;

    (VOID) RtlCaptureStackBackTrace(1, 1, &Caller, NULL);    

    RtlZeroMemory(&Request, sizeof (STORE_REQUEST));

    if (Prefix == NULL) {
        status = StorePrepareRequest(Context,
                                     &Request,
                                     Transaction,
                                     XS_DIRECTORY,
                                     Node, strlen(Node),
                                     "", 1,
                                     NULL, 0);
    } else {
        status = StorePrepareRequest(Context,
                                     &Request,
                                     Transaction,
                                     XS_DIRECTORY,
                                     Prefix, strlen(Prefix),
                                     "/", 1,
                                     Node, strlen(Node),
                                     "", 1,
                                     NULL, 0);
    }

    if (!NT_SUCCESS(status))
        goto fail1;

    Response = StoreSubmitRequest(Context, &Request);

    status = STATUS_NO_MEMORY;
    if (Response == NULL)
        goto fail2;

    status = __StoreCheckResponse(Response);
    if (!NT_SUCCESS(status))
        goto fail3;

    Buffer = __StoreCopyPayload(Context, Response, Caller);

    status = STATUS_NO_MEMORY;
    if (Buffer == NULL)
        goto fail4;

    __StoreFreeResponse(Response);
    ASSERT(IsZeroMemory(&Request, sizeof (STORE_REQUEST)));

    *Value = Buffer->Data;

    return STATUS_SUCCESS;

fail4:
fail3:
    __StoreFreeResponse(Response);

fail2:
fail1:
    ASSERT(IsZeroMemory(&Request, sizeof (STORE_REQUEST)));

    return status;
}

static NTSTATUS
StoreTransactionStart(
    IN  PXENBUS_STORE_CONTEXT       Context,
    OUT PXENBUS_STORE_TRANSACTION   *Transaction
    )
{
    STORE_REQUEST                   Request;
    PSTORE_RESPONSE                 Response;
    KIRQL                           Irql;
    NTSTATUS                        status;

    *Transaction = __StoreAllocate(sizeof (XENBUS_STORE_TRANSACTION));

    status = STATUS_NO_MEMORY;
    if (*Transaction == NULL)
        goto fail1;

    (*Transaction)->Magic = STORE_TRANSACTION_MAGIC;
    (VOID) RtlCaptureStackBackTrace(1, 1, &(*Transaction)->Caller, NULL);    

    RtlZeroMemory(&Request, sizeof (STORE_REQUEST));

    status = StorePrepareRequest(Context,
                                 &Request,
                                 NULL,
                                 XS_TRANSACTION_START,
                                 "", 1,
                                 NULL, 0);
    ASSERT(NT_SUCCESS(status));

    Response = StoreSubmitRequest(Context, &Request);

    status = STATUS_NO_MEMORY;
    if (Response == NULL)
        goto fail2;

    status = __StoreCheckResponse(Response);
    if (!NT_SUCCESS(status))
        goto fail3;

    (*Transaction)->Id = (uint32_t)strtoul(Response->Segment[RESPONSE_PAYLOAD_SEGMENT].Data, NULL, 10);
    ASSERT((*Transaction)->Id != 0);

    __StoreFreeResponse(Response);
    ASSERT(IsZeroMemory(&Request, sizeof (STORE_REQUEST)));

    KeAcquireSpinLock(&Context->Lock, &Irql);
    (*Transaction)->Active = TRUE;
    InsertTailList(&Context->TransactionList, &(*Transaction)->ListEntry);
    KeReleaseSpinLock(&Context->Lock, Irql);

    return STATUS_SUCCESS;

fail3:
    Error("fail3\n");

    __StoreFreeResponse(Response);
    ASSERT(IsZeroMemory(&Request, sizeof (STORE_REQUEST)));

    (*Transaction)->Caller = NULL;
    (*Transaction)->Magic = 0;

    ASSERT(IsZeroMemory(*Transaction, sizeof (XENBUS_STORE_TRANSACTION)));
    __StoreFree(*Transaction);

fail2:
    Error("fail2\n");

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

static NTSTATUS
StoreTransactionEnd(
    IN  PXENBUS_STORE_CONTEXT       Context,
    IN  PXENBUS_STORE_TRANSACTION   Transaction,
    IN  BOOLEAN                     Commit
    )
{
    STORE_REQUEST                   Request;
    PSTORE_RESPONSE                 Response;
    KIRQL                           Irql;
    NTSTATUS                        status;

    ASSERT3U(Transaction->Magic, ==, STORE_TRANSACTION_MAGIC);

    KeAcquireSpinLock(&Context->Lock, &Irql);

    status = STATUS_RETRY;
    if (!Transaction->Active)
        goto done;

    KeReleaseSpinLock(&Context->Lock, Irql);

    RtlZeroMemory(&Request, sizeof (STORE_REQUEST));

    status = StorePrepareRequest(Context,
                                 &Request,
                                 Transaction,
                                 XS_TRANSACTION_END,
                                 (Commit) ? "T" : "F", 2,
                                 NULL, 0);
    ASSERT(NT_SUCCESS(status));

    Response = StoreSubmitRequest(Context, &Request);

    status = STATUS_NO_MEMORY;
    if (Response == NULL)
        goto fail1;

    status = __StoreCheckResponse(Response);
    if (!NT_SUCCESS(status) && status != STATUS_RETRY)
        goto fail2;

    __StoreFreeResponse(Response);
    ASSERT(IsZeroMemory(&Request, sizeof (STORE_REQUEST)));

    KeAcquireSpinLock(&Context->Lock, &Irql);
    Transaction->Active = FALSE;

done:
    RemoveEntryList(&Transaction->ListEntry);
    KeReleaseSpinLock(&Context->Lock, Irql);

    RtlZeroMemory(&Transaction->ListEntry, sizeof (LIST_ENTRY));

    Transaction->Id = 0;

    Transaction->Caller = NULL;
    Transaction->Magic = 0;

    ASSERT(IsZeroMemory(Transaction, sizeof (XENBUS_STORE_TRANSACTION)));
    __StoreFree(Transaction);

    return status;

fail2:
    ASSERT3U(status, !=, STATUS_RETRY);

    __StoreFreeResponse(Response);

fail1:
    ASSERT(IsZeroMemory(&Request, sizeof (STORE_REQUEST)));

    return status;
}

static NTSTATUS
StoreWatch(
    IN  PXENBUS_STORE_CONTEXT   Context,
    IN  PCHAR                   Prefix OPTIONAL,
    IN  PCHAR                   Node,
    IN  PKEVENT                 Event,
    OUT PXENBUS_STORE_WATCH     *Watch
    )
{
    ULONG                       Length;
    PCHAR                       Path;
    CHAR                        Token[TOKEN_LENGTH];
    STORE_REQUEST               Request;
    PSTORE_RESPONSE             Response;
    KIRQL                       Irql;
    NTSTATUS                    status;

    *Watch = __StoreAllocate(sizeof (XENBUS_STORE_WATCH));

    status = STATUS_NO_MEMORY;
    if (*Watch == NULL)
        goto fail1;

    (*Watch)->Magic = STORE_WATCH_MAGIC;
    (VOID) RtlCaptureStackBackTrace(1, 1, &(*Watch)->Caller, NULL);    

    if (Prefix == NULL)
        Length = (ULONG)strlen(Node) + sizeof (CHAR);
    else
        Length = (ULONG)strlen(Prefix) + 1 + (ULONG)strlen(Node) + sizeof (CHAR);

    Path = __StoreAllocate(Length);

    status = STATUS_NO_MEMORY;
    if (Path == NULL)
        goto fail2;

    status = (Prefix == NULL) ?
             RtlStringCbPrintfA(Path, Length, "%s", Node) :
             RtlStringCbPrintfA(Path, Length, "%s/%s", Prefix, Node);
    ASSERT(NT_SUCCESS(status));
    
    (*Watch)->Path = Path;
    (*Watch)->Event = Event;

    KeAcquireSpinLock(&Context->Lock, &Irql);
    (*Watch)->Id = __StoreNextWatchId(Context);
    (*Watch)->Active = TRUE;
    InsertTailList(&Context->WatchList, &(*Watch)->ListEntry);
    KeReleaseSpinLock(&Context->Lock, Irql);

    status = RtlStringCbPrintfA(Token,
                                sizeof (Token),
                                "TOK|%p|%04X",
                                (*Watch)->Caller,
                                (*Watch)->Id);
    ASSERT(NT_SUCCESS(status));
    ASSERT3U(strlen(Token), ==, TOKEN_LENGTH - 1);

    RtlZeroMemory(&Request, sizeof (STORE_REQUEST));

    status = StorePrepareRequest(Context,
                                 &Request,
                                 NULL,
                                 XS_WATCH,
                                 Path, strlen(Path),
                                 "", 1,
                                 Token, strlen(Token), 
                                 "", 1,
                                 NULL, 0);
    ASSERT(NT_SUCCESS(status));

    Response = StoreSubmitRequest(Context, &Request);

    status = STATUS_NO_MEMORY;
    if (Response == NULL)
        goto fail3;

    status = __StoreCheckResponse(Response);
    if (!NT_SUCCESS(status))
        goto fail4;

    __StoreFreeResponse(Response);
    ASSERT(IsZeroMemory(&Request, sizeof (STORE_REQUEST)));

    return STATUS_SUCCESS;

fail4:
    Error("fail4\n");

    __StoreFreeResponse(Response);

fail3:
    Error("fail3\n");

    ASSERT(IsZeroMemory(&Request, sizeof (STORE_REQUEST)));

    KeAcquireSpinLock(&Context->Lock, &Irql);
    (*Watch)->Active = FALSE;
    (*Watch)->Id = 0;
    RemoveEntryList(&(*Watch)->ListEntry);
    KeReleaseSpinLock(&Context->Lock, Irql);

    RtlZeroMemory(&(*Watch)->ListEntry, sizeof (LIST_ENTRY));

    (*Watch)->Event = NULL;
    (*Watch)->Path = NULL;

    __StoreFree(Path);

fail2:
    Error("fail2\n");

    (*Watch)->Caller = NULL;
    (*Watch)->Magic = 0;

    ASSERT(IsZeroMemory(*Watch, sizeof (XENBUS_STORE_WATCH)));
    __StoreFree(*Watch);

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

static NTSTATUS
StoreUnwatch(
    IN  PXENBUS_STORE_CONTEXT   Context,
    IN  PXENBUS_STORE_WATCH     Watch
    )
{
    PCHAR                       Path;
    CHAR                        Token[TOKEN_LENGTH];
    STORE_REQUEST               Request;
    PSTORE_RESPONSE             Response;
    KIRQL                       Irql;
    NTSTATUS                    status;

    ASSERT3U(Watch->Magic, ==, STORE_WATCH_MAGIC);

    Path = Watch->Path;

    KeAcquireSpinLock(&Context->Lock, &Irql);

    if (!Watch->Active)
        goto done;

    KeReleaseSpinLock(&Context->Lock, Irql);

    status = RtlStringCbPrintfA(Token,
                                sizeof (Token),
                                "TOK|%p|%04X",
                                Watch->Caller,
                                Watch->Id);
    ASSERT(NT_SUCCESS(status));
    ASSERT3U(strlen(Token), ==, TOKEN_LENGTH - 1);

    RtlZeroMemory(&Request, sizeof (STORE_REQUEST));

    status = StorePrepareRequest(Context,
                                 &Request,
                                 NULL,
                                 XS_UNWATCH,
                                 Path, strlen(Path),
                                 "", 1,
                                 Token, strlen(Token), 
                                 "", 1,
                                 NULL, 0);
    ASSERT(NT_SUCCESS(status));

    Response = StoreSubmitRequest(Context, &Request);

    status = STATUS_NO_MEMORY;
    if (Response == NULL)
        goto fail1;

    status = __StoreCheckResponse(Response);
    if (!NT_SUCCESS(status))
        goto fail2;

    __StoreFreeResponse(Response);
    ASSERT(IsZeroMemory(&Request, sizeof (STORE_REQUEST)));

    KeAcquireSpinLock(&Context->Lock, &Irql);
    Watch->Active = FALSE;

done:
    Watch->Id = 0;
    RemoveEntryList(&Watch->ListEntry);
    KeReleaseSpinLock(&Context->Lock, Irql);

    RtlZeroMemory(&Watch->ListEntry, sizeof (LIST_ENTRY));

    Watch->Event = NULL;
    Watch->Path = NULL;

    __StoreFree(Path);

    Watch->Caller = NULL;
    Watch->Magic = 0;

    ASSERT(IsZeroMemory(Watch, sizeof (XENBUS_STORE_WATCH)));
    __StoreFree(Watch);

    return STATUS_SUCCESS;

fail2:
    Error("fail2\n");

    __StoreFreeResponse(Response);

fail1:
    Error("fail1 (%08x)\n", status);

    ASSERT(IsZeroMemory(&Request, sizeof (STORE_REQUEST)));

    return status;
}

static VOID
StorePoll(
    IN  PXENBUS_STORE_CONTEXT   Context
    )
{
    KeAcquireSpinLockAtDpcLevel(&Context->Lock);
    __StorePoll(Context);
    KeReleaseSpinLockFromDpcLevel(&Context->Lock);
}

static VOID
StoreAcquire(
    IN  PXENBUS_STORE_CONTEXT   Context
    )
{
    InterlockedIncrement(&Context->References);
}

static VOID
StoreRelease(
    IN  PXENBUS_STORE_CONTEXT   Context
    )
{
    ASSERT(Context->References != 0);
    InterlockedDecrement(&Context->References);
}

#define STORE_OPERATION(_Type, _Name, _Arguments) \
        Store ## _Name,

static XENBUS_STORE_OPERATIONS  Operations = {
    DEFINE_STORE_OPERATIONS
};

#undef STORE_OPERATION

KSERVICE_ROUTINE StoreEvtchnCallback;

BOOLEAN
StoreEvtchnCallback(
    IN  PKINTERRUPT         InterruptObject,
    IN  PVOID               Argument
    )
{
    PXENBUS_STORE_CONTEXT   Context = Argument;

    UNREFERENCED_PARAMETER(InterruptObject);

    ASSERT(Context != NULL);

    KeInsertQueueDpc(&Context->Dpc, NULL, NULL);

    return TRUE;
}

static FORCEINLINE VOID
__StoreDisable(
    IN PXENBUS_STORE_CONTEXT    Context
    )
{
    EVTCHN(Close,
           Context->EvtchnInterface,
           Context->Evtchn);
    Context->Evtchn = NULL;
}

static FORCEINLINE VOID
__StoreEnable(
    IN PXENBUS_STORE_CONTEXT    Context
    )
{
    ULONG_PTR                   Port;
    BOOLEAN                     Pending;
    NTSTATUS                    status;

    status = HvmGetParam(HVM_PARAM_STORE_EVTCHN, &Port);
    ASSERT(NT_SUCCESS(status));

    Context->Evtchn = EVTCHN(Open,
                             Context->EvtchnInterface,
                             EVTCHN_FIXED,
                             StoreEvtchnCallback,
                             Context,
                             Port,
                             FALSE);
    ASSERT(Context->Evtchn != NULL);

    Pending = EVTCHN(Unmask,
                     Context->EvtchnInterface,
                     Context->Evtchn,
                     FALSE);
    if (Pending)
        EVTCHN(Trigger,
               Context->EvtchnInterface,
               Context->Evtchn);
}

static VOID
StoreSuspendCallbackEarly(
    IN  PVOID               Argument
    )
{
    PXENBUS_STORE_CONTEXT   Context = Argument;
    PLIST_ENTRY             ListEntry;
    PFN_NUMBER              Pfn;
    NTSTATUS                status;

    status = HvmGetParam(HVM_PARAM_STORE_PFN, &Pfn);
    ASSERT(NT_SUCCESS(status));
    ASSERT3U(Pfn, ==, Context->Pfn);

    for (ListEntry = Context->TransactionList.Flink;
         ListEntry != &(Context->TransactionList);
         ListEntry = ListEntry->Flink) {
        PXENBUS_STORE_TRANSACTION   Transaction;

        Transaction = CONTAINING_RECORD(ListEntry, XENBUS_STORE_TRANSACTION, ListEntry);

        Transaction->Active = FALSE;
    }

    for (ListEntry = Context->WatchList.Flink;
         ListEntry != &(Context->WatchList);
         ListEntry = ListEntry->Flink) {
        PXENBUS_STORE_WATCH Watch;

        Watch = CONTAINING_RECORD(ListEntry, XENBUS_STORE_WATCH, ListEntry);

        Watch->Active = FALSE;
    }
}

static VOID
StoreSuspendCallbackLate(
    IN  PVOID                           Argument
    )
{
    PXENBUS_STORE_CONTEXT               Context = Argument;
    struct xenstore_domain_interface    *Shared;
    PLIST_ENTRY                         ListEntry;
    KIRQL                               Irql;

    Shared = Context->Shared;

    KeAcquireSpinLock(&Context->Lock, &Irql);

    __StoreDisable(Context);
    __StoreResetResponse(Context);
    __StoreEnable(Context);

    for (ListEntry = Context->WatchList.Flink;
         ListEntry != &(Context->WatchList);
         ListEntry = ListEntry->Flink) {
        PXENBUS_STORE_WATCH Watch;

        Watch = CONTAINING_RECORD(ListEntry, XENBUS_STORE_WATCH, ListEntry);

        KeSetEvent(Watch->Event, 0, FALSE);
    }

    KeReleaseSpinLock(&Context->Lock, Irql);
}

static VOID
StoreDebugCallback(
    IN  PVOID                           Argument,
    IN  BOOLEAN                         Crashing
    )
{
    PXENBUS_STORE_CONTEXT               Context = Argument;

    DEBUG(Printf,
          Context->DebugInterface,
          Context->DebugCallback,
          "Pfn = %p\n",
          (PVOID)Context->Pfn);

    if (!Crashing) {
        struct xenstore_domain_interface    *Shared;

        Shared = Context->Shared;

        DEBUG(Printf,
              Context->DebugInterface,
              Context->DebugCallback,
              "req_cons = %08x req_prod = %08x\n",
              Shared->req_cons,
              Shared->req_prod);

        DEBUG(Printf,
              Context->DebugInterface,
              Context->DebugCallback,
              "rsp_cons = %08x rsp_prod = %08x\n",
              Shared->rsp_cons,
              Shared->rsp_prod);
    }

    if (!IsListEmpty(&Context->BufferList)) {
        PLIST_ENTRY ListEntry;

        DEBUG(Printf,
              Context->DebugInterface,
              Context->DebugCallback,
              "BUFFERS:\n");

        for (ListEntry = Context->BufferList.Flink;
             ListEntry != &(Context->BufferList);
             ListEntry = ListEntry->Flink) {
            PSTORE_BUFFER   Buffer;
            PCHAR           Name;
            ULONG_PTR       Offset;

            Buffer = CONTAINING_RECORD(ListEntry, STORE_BUFFER, ListEntry);

            ModuleLookup((ULONG_PTR)Buffer->Caller, &Name, &Offset);

            if (Name != NULL) {
                DEBUG(Printf,
                      Context->DebugInterface,
                      Context->DebugCallback,
                      "- (%p) %s + %p\n",
                      Buffer->Data,
                      Name,
                      (PVOID)Offset);
            } else {
                DEBUG(Printf,
                      Context->DebugInterface,
                      Context->DebugCallback,
                      "- (%p) %p\n",
                      Buffer->Data,
                      Buffer->Caller);
            }
        }
    }

    if (!IsListEmpty(&Context->WatchList)) {
        PLIST_ENTRY ListEntry;

        DEBUG(Printf,
              Context->DebugInterface,
              Context->DebugCallback,
              "WATCHES:\n");

        for (ListEntry = Context->WatchList.Flink;
             ListEntry != &(Context->WatchList);
             ListEntry = ListEntry->Flink) {
            PXENBUS_STORE_WATCH Watch;
            PCHAR               Name;
            ULONG_PTR           Offset;

            Watch = CONTAINING_RECORD(ListEntry, XENBUS_STORE_WATCH, ListEntry);

            ModuleLookup((ULONG_PTR)Watch->Caller, &Name, &Offset);

            if (Name != NULL) {
                DEBUG(Printf,
                      Context->DebugInterface,
                      Context->DebugCallback,
                      "- (%04X) ON %s BY %s + %p [%s]\n",
                      Watch->Id,
                      Watch->Path,
                      Name,
                      (PVOID)Offset,
                      (Watch->Active) ? "ACTIVE" : "EXPIRED");
            } else {
                DEBUG(Printf,
                      Context->DebugInterface,
                      Context->DebugCallback,
                      "- (%04X) ON %s BY %p [%s]\n",
                      Watch->Id,
                      Watch->Path,
                      (PVOID)Watch->Caller,
                      (Watch->Active) ? "ACTIVE" : "EXPIRED");
            }
        }
    }

    if (!IsListEmpty(&Context->TransactionList)) {
        PLIST_ENTRY ListEntry;

        DEBUG(Printf,
              Context->DebugInterface,
              Context->DebugCallback,
              "TRANSACTIONS:\n");

        for (ListEntry = Context->TransactionList.Flink;
             ListEntry != &(Context->TransactionList);
             ListEntry = ListEntry->Flink) {
            PXENBUS_STORE_TRANSACTION   Transaction;
            PCHAR                       Name;
            ULONG_PTR                   Offset;

            Transaction = CONTAINING_RECORD(ListEntry, XENBUS_STORE_TRANSACTION, ListEntry);

            ModuleLookup((ULONG_PTR)Transaction->Caller, &Name, &Offset);

            if (Name != NULL) {
                DEBUG(Printf,
                      Context->DebugInterface,
                      Context->DebugCallback,
                      "- (%08X) BY %s + %p [%s]\n",
                      Transaction->Id,
                      Name,
                      (PVOID)Offset,
                      (Transaction->Active) ? "ACTIVE" : "EXPIRED");
            } else {
                DEBUG(Printf,
                      Context->DebugInterface,
                      Context->DebugCallback,
                      "- (%04X) ON %s BY %p [%s]\n",
                      Transaction->Id,
                      (PVOID)Transaction->Caller,
                      (Transaction->Active) ? "ACTIVE" : "EXPIRED");
            }
        }
    }
}

NTSTATUS
StoreInitialize(
    IN  PXENBUS_FDO             Fdo,
    OUT PXENBUS_STORE_INTERFACE Interface
    )
{
    PXENBUS_STORE_CONTEXT       Context;
    PHYSICAL_ADDRESS            Address;
    NTSTATUS                    status;

    Trace("====>\n");

    Context = __StoreAllocate(sizeof (XENBUS_STORE_CONTEXT));

    status = STATUS_NO_MEMORY;
    if (Context == NULL)
        goto fail1;

    status = HvmGetParam(HVM_PARAM_STORE_PFN, &Context->Pfn);
    ASSERT(NT_SUCCESS(status));

    Address.QuadPart = (ULONGLONG)Context->Pfn << PAGE_SHIFT;
    Context->Shared = (struct xenstore_domain_interface *)MmMapIoSpace(Address,
                                                                       PAGE_SIZE,
                                                                       MmCached);
    status = STATUS_UNSUCCESSFUL;
    if (Context->Shared == NULL)
        goto fail2;

    Info("xenstore_domain_interface *: %p\n", Context->Shared);

    KeInitializeSpinLock(&Context->Lock);

    Context->RequestId = (USHORT)__rdtsc();
    InitializeListHead(&Context->SubmittedList);
    InitializeListHead(&Context->PendingList);

    InitializeListHead(&Context->TransactionList);

    Context->WatchId = (USHORT)(__rdtsc() >> 16);
    InitializeListHead(&Context->WatchList);

    InitializeListHead(&Context->BufferList);

    KeInitializeDpc(&Context->Dpc, StoreDpc, Context);

    Context->EvtchnInterface = FdoGetEvtchnInterface(Fdo);

    EVTCHN(Acquire, Context->EvtchnInterface);

    __StoreResetResponse(Context);
    __StoreEnable(Context);

    Context->SuspendInterface = FdoGetSuspendInterface(Fdo);

    SUSPEND(Acquire, Context->SuspendInterface);

    status = SUSPEND(Register,
                     Context->SuspendInterface,
                     SUSPEND_CALLBACK_EARLY,
                     StoreSuspendCallbackEarly,
                     Context,
                     &Context->SuspendCallbackEarly);
    if (!NT_SUCCESS(status))
        goto fail3;

    status = SUSPEND(Register,
                     Context->SuspendInterface,
                     SUSPEND_CALLBACK_LATE,
                     StoreSuspendCallbackLate,
                     Context,
                     &Context->SuspendCallbackLate);
    if (!NT_SUCCESS(status))
        goto fail4;

    Context->DebugInterface = FdoGetDebugInterface(Fdo);

    DEBUG(Acquire, Context->DebugInterface);

    status = DEBUG(Register,
                   Context->DebugInterface,
                   __MODULE__ "|STORE",
                   StoreDebugCallback,
                   Context,
                   &Context->DebugCallback);
    if (!NT_SUCCESS(status))
        goto fail5;

    Interface->Context = Context;
    Interface->Operations = &Operations;

    Trace("<====\n");

    return STATUS_SUCCESS;

fail5:
    Error("fail5\n");

    DEBUG(Release, Context->DebugInterface);
    Context->DebugInterface = NULL;

    SUSPEND(Deregister,
            Context->SuspendInterface,
            Context->SuspendCallbackLate);
    Context->SuspendCallbackLate = NULL;

fail4:
    Error("fail4\n");

    SUSPEND(Deregister,
            Context->SuspendInterface,
            Context->SuspendCallbackEarly);
    Context->SuspendCallbackEarly = NULL;

fail3:
    Error("fail3\n");

    SUSPEND(Release, Context->SuspendInterface);
    Context->SuspendInterface = NULL;

    __StoreDisable(Context);

    KeFlushQueuedDpcs();

    EVTCHN(Release, Context->EvtchnInterface);
    Context->EvtchnInterface = NULL;

    RtlZeroMemory(&Context->Response, sizeof (STORE_RESPONSE));

    RtlZeroMemory(&Context->BufferList, sizeof (LIST_ENTRY));

    RtlZeroMemory(&Context->WatchList, sizeof (LIST_ENTRY));

    Context->WatchId = 0;

    RtlZeroMemory(&Context->TransactionList, sizeof (LIST_ENTRY));

    RtlZeroMemory(&Context->PendingList, sizeof (LIST_ENTRY));

    RtlZeroMemory(&Context->SubmittedList, sizeof (LIST_ENTRY));

    Context->RequestId = 0;

    RtlZeroMemory(&Context->Lock, sizeof (KSPIN_LOCK));

    RtlZeroMemory(&Context->Dpc, sizeof (KDPC));

    MmUnmapIoSpace(Context->Shared, PAGE_SIZE);
    Context->Shared = NULL;

fail2:
    Error("fail2\n");

    ASSERT(IsZeroMemory(Context, sizeof (XENBUS_STORE_CONTEXT)));
    __StoreFree(Context);

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

VOID
StoreTeardown(
    IN OUT  PXENBUS_STORE_INTERFACE Interface
    )
{
    PXENBUS_STORE_CONTEXT           Context = Interface->Context;

    Trace("====>\n");

    if (!IsListEmpty(&Context->WatchList))
        BUG("OUTSTANDING WATCHES");

    if (!IsListEmpty(&Context->TransactionList))
        BUG("OUTSTANDING TRANSACTIONS");

    if (!IsListEmpty(&Context->BufferList))
        BUG("OUTSTANDING BUFFER");

    DEBUG(Deregister,
          Context->DebugInterface,
          Context->DebugCallback);
    Context->DebugCallback = NULL;

    DEBUG(Release, Context->DebugInterface);
    Context->DebugInterface = NULL;

    SUSPEND(Deregister,
            Context->SuspendInterface,
            Context->SuspendCallbackLate);
    Context->SuspendCallbackLate = NULL;

    SUSPEND(Deregister,
            Context->SuspendInterface,
            Context->SuspendCallbackEarly);
    Context->SuspendCallbackEarly = NULL;

    SUSPEND(Release, Context->SuspendInterface);
    Context->SuspendInterface = NULL;

    __StoreDisable(Context);

    KeFlushQueuedDpcs();

    EVTCHN(Release, Context->EvtchnInterface);
    Context->EvtchnInterface = NULL;

    RtlZeroMemory(&Context->Response, sizeof (STORE_RESPONSE));

    RtlZeroMemory(&Context->BufferList, sizeof (LIST_ENTRY));

    RtlZeroMemory(&Context->WatchList, sizeof (LIST_ENTRY));

    Context->WatchId = 0;

    RtlZeroMemory(&Context->TransactionList, sizeof (LIST_ENTRY));

    ASSERT(IsListEmpty(&Context->PendingList));
    RtlZeroMemory(&Context->PendingList, sizeof (LIST_ENTRY));

    ASSERT(IsListEmpty(&Context->SubmittedList));
    RtlZeroMemory(&Context->SubmittedList, sizeof (LIST_ENTRY));

    Context->RequestId = 0;

    RtlZeroMemory(&Context->Lock, sizeof (KSPIN_LOCK));

    RtlZeroMemory(&Context->Dpc, sizeof (KDPC));

    MmUnmapIoSpace(Context->Shared, PAGE_SIZE);
    Context->Shared = NULL;

    Context->Pfn = 0;

    ASSERT(IsZeroMemory(Context, sizeof (XENBUS_STORE_CONTEXT)));
    __StoreFree(Context);

    RtlZeroMemory(Interface, sizeof (XENBUS_STORE_INTERFACE));

    Trace("<====\n");
}
