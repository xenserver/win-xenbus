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

#include "dump.h"
#include "log.h"
#include "assert.h"

//
// Crash dumps via QEMU
//
// To enable you need to customize /opt/xensource/libexec/qemu-dm-wrapper in dom0 to add the following
// arguments to the qemu command line:
//
// -priv -dumpdir <directory> -dumpquota <quota>
//
// <directory> is best pointed at an empty directory. Files will be created with numeric names starting at 0.
// <quota> should be the total size in MB of all possible crash dump files. I.e. once sufficient crash dumps have
// occurred to fill this quota, no more will be allowed until sufficient space is cleared in <directory>. This is
// to prevent dump files (which can be very large) from overrunning dom0's filesystem.
//

static KBUGCHECK_REASON_CALLBACK_RECORD DumpBugCheckReasonCallbackRecord;

VOID
DumpTeardown(
    VOID
    )
{
    (VOID) KeDeregisterBugCheckReasonCallback(&DumpBugCheckReasonCallbackRecord);
}

static PVOID    PortEB = ((PVOID)(ULONG_PTR)0xeb);
static PVOID    PortEC = ((PVOID)(ULONG_PTR)0xec);

#define DUMP_VERSION        0x01

#define DUMP_IO_REGISTERED  0x00
#define DUMP_IO_OPEN        0x01
#define DUMP_IO_CLOSE       0x02

static KBUGCHECK_DUMP_IO_TYPE   DumpIoType = KbDumpIoInvalid;

static FORCEINLINE VOID
__DumpPortOpen(
    VOID
    )
{
    Info("====>\n");
    WRITE_PORT_UCHAR(PortEB, DUMP_IO_OPEN);   
    Info("<====\n");
}

static FORCEINLINE VOID
__DumpPortClose(
    VOID
    )
{
    Info("====>\n");
    WRITE_PORT_UCHAR(PortEB, DUMP_IO_CLOSE);   
    Info("<====\n");
}

#define IS_PAGE_ALIGNED(_Address)   (((ULONG_PTR)(_Address) & (PAGE_SIZE - 1)) == 0)

static VOID
DumpPortWrite(
    IN  ULONG64         Offset,
    IN  PVOID           Buffer,
    IN  ULONG           Length
    )
{
    PHYSICAL_ADDRESS    Address;

    ASSERT(Offset == (ULONG64)-1);
    ASSERT(IS_PAGE_ALIGNED(Buffer));
    ASSERT(IS_PAGE_ALIGNED(Length));

    //
    // Sometimes Windows passes us virtual addresses, sometimes it passes
    // physical addresses. It doesn't tell us which it's handing us, and
    // how this plays with PAE is anybody's guess.
    //
    Address = MmGetPhysicalAddress(Buffer);
    if (Address.QuadPart == 0)
        Address.QuadPart = (ULONG_PTR)Buffer;

    Address.QuadPart >>= PAGE_SHIFT;
    ASSERT3U(Address.HighPart, ==, 0);

    for (Length >>= PAGE_SHIFT; Length != 0; Length--)
        WRITE_PORT_ULONG(PortEC, Address.LowPart++);
}

static const CHAR *
DumpIoTypeName(
    IN  KBUGCHECK_DUMP_IO_TYPE  Type
    )
{
#define _IO_TYPE_NAME(_Type)    \
        case KbDumpIo ## _Type: \
            return #_Type;

    switch (Type) {
    _IO_TYPE_NAME(Invalid);
    _IO_TYPE_NAME(Header);
    _IO_TYPE_NAME(Body);
    _IO_TYPE_NAME(SecondaryData);
    _IO_TYPE_NAME(Complete);
    default:
        break;
    }

    return "UNKNOWN";

#undef  _IO_TYPE_NAME
}

KBUGCHECK_REASON_CALLBACK_ROUTINE   DumpBugCheckReasonCallback;

VOID
DumpBugCheckReasonCallback(
    IN  KBUGCHECK_CALLBACK_REASON           Reason,
    IN  PKBUGCHECK_REASON_CALLBACK_RECORD   Record,
    IN  OUT PVOID                           ReasonSpecificData,
    IN  ULONG                               ReasonSpecificDataLength 
    )
{   
    PKBUGCHECK_DUMP_IO                      DumpIo = (PKBUGCHECK_DUMP_IO)ReasonSpecificData;

    UNREFERENCED_PARAMETER(ReasonSpecificDataLength);
    
    ASSERT3U(Reason, ==, KbCallbackDumpIo);
    ASSERT3P(Record, ==, &DumpBugCheckReasonCallbackRecord);
    ASSERT(DumpIo != NULL);
    
    switch (DumpIo->Type) {
        case KbDumpIoHeader:
            ASSERT(DumpIoType == KbDumpIoInvalid ||
                   DumpIoType == KbDumpIoHeader);
            DumpIoType = KbDumpIoHeader;
                 
            __DumpPortOpen();

            DumpPortWrite(DumpIo->Offset,
                          DumpIo->Buffer,
                          DumpIo->BufferLength);
            break;

        case KbDumpIoBody:
            ASSERT(DumpIoType == KbDumpIoHeader ||
                   DumpIoType == KbDumpIoBody);
            DumpIoType = KbDumpIoBody;

            DumpPortWrite(DumpIo->Offset,
                          DumpIo->Buffer,
                          DumpIo->BufferLength);
            break;

        case KbDumpIoSecondaryData:
            ASSERT(DumpIoType == KbDumpIoBody ||
                   DumpIoType == KbDumpIoSecondaryData);
            DumpIoType = KbDumpIoSecondaryData;
                 
            DumpPortWrite(DumpIo->Offset,
                          DumpIo->Buffer,
                          DumpIo->BufferLength);
            break;

        case KbDumpIoComplete:
            ASSERT3U(DumpIoType, ==, KbDumpIoSecondaryData);
            DumpIoType = KbDumpIoComplete;
            
            __DumpPortClose();
            break;
        
        case KbDumpIoInvalid:
        default:
            ASSERT(FALSE);
            break;  
    }
}

NTSTATUS
DumpInitialize(
    VOID
    )
{
    UCHAR       Version;
    NTSTATUS    status;

    Version = READ_PORT_UCHAR(PortEB);

    if (Version != DUMP_VERSION)
        goto done;

    KeInitializeCallbackRecord(&DumpBugCheckReasonCallbackRecord);

    status = STATUS_UNSUCCESSFUL;
    if (!KeRegisterBugCheckReasonCallback(&DumpBugCheckReasonCallbackRecord,
                                          DumpBugCheckReasonCallback,
                                          KbCallbackDumpIo,
                                          (PUCHAR)__MODULE__))
        goto fail1;

    Info("callback registered\n");

done:
    return STATUS_SUCCESS;

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}
