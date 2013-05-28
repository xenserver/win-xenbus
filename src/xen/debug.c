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

#define XEN_API extern

#include <ntddk.h>
#include <xen.h>
#include <bugcodes.h>

#include "hypercall.h"
#include "module.h"
#include "debug.h"
#include "log.h"
#include "assert.h"

static KBUGCHECK_CALLBACK_RECORD DebugBugCheckCallbackRecord;

VOID
DebugTeardown(
    VOID
    )
{
    (VOID) KeDeregisterBugCheckCallback(&DebugBugCheckCallbackRecord);
}

static DECLSPEC_NOINLINE VOID
DebugDumpExceptionRecord(
    IN  PEXCEPTION_RECORD   Exception
    )
{
    __try {
        while (Exception != NULL) {
            ULONG   Index;

            LogQemuPrintf("%s|BUGCHECK: EXCEPTION (%p):\n", __MODULE__,
                          Exception);
            LogQemuPrintf("%s|BUGCHECK: - Code = %08X\n", __MODULE__,
                          Exception->ExceptionCode);
            LogQemuPrintf("%s|BUGCHECK: - Flags = %08X\n", __MODULE__,
                          Exception->ExceptionFlags);
            LogQemuPrintf("%s|BUGCHECK: - Address = %p\n", __MODULE__,
                          Exception->ExceptionAddress);

            for (Index = 0; Index < Exception->NumberParameters; Index++)
                LogQemuPrintf("%s|BUGCHECK: - Parameter[%u] = %p\n", __MODULE__,
                              Index,
                              (PVOID)Exception->ExceptionInformation[Index]);

            Exception = Exception->ExceptionRecord;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Nothing to to
    }
}

#pragma warning(push)
#pragma warning(disable:6262) // Uses more than 1024 bytes of stack

#if defined(__i386__)
static DECLSPEC_NOINLINE VOID
DebugDumpContext(
    IN  PCONTEXT    Context
    )
{
    __try {
        LogQemuPrintf("%s|BUGCHECK: CONTEXT (%p):\n", __MODULE__,
                      Context);
        LogQemuPrintf("%s|BUGCHECK: - GS = %p\n", __MODULE__,
                      (PVOID)Context->SegGs);
        LogQemuPrintf("%s|BUGCHECK: - FS = %p\n", __MODULE__,
                      (PVOID)Context->SegFs);
        LogQemuPrintf("%s|BUGCHECK: - ES = %p\n", __MODULE__,
                      (PVOID)Context->SegEs);
        LogQemuPrintf("%s|BUGCHECK: - DS = %p\n", __MODULE__,
                      (PVOID)Context->SegDs);
        LogQemuPrintf("%s|BUGCHECK: - SS = %p\n", __MODULE__,
                      (PVOID)Context->SegSs);
        LogQemuPrintf("%s|BUGCHECK: - CS = %p\n", __MODULE__,
                      (PVOID)Context->SegCs);

        LogQemuPrintf("%s|BUGCHECK: - EFLAGS = %p\n", __MODULE__,
                      (PVOID)Context->EFlags);

        LogQemuPrintf("%s|BUGCHECK: - EDI = %p\n", __MODULE__,
                      (PVOID)Context->Edi);
        LogQemuPrintf("%s|BUGCHECK: - ESI = %p\n", __MODULE__,
                      (PVOID)Context->Esi);
        LogQemuPrintf("%s|BUGCHECK: - EBX = %p\n", __MODULE__,
                      (PVOID)Context->Ebx);
        LogQemuPrintf("%s|BUGCHECK: - EDX = %p\n", __MODULE__,
                      (PVOID)Context->Edx);
        LogQemuPrintf("%s|BUGCHECK: - ECX = %p\n", __MODULE__,
                      (PVOID)Context->Ecx);
        LogQemuPrintf("%s|BUGCHECK: - EAX = %p\n", __MODULE__,
                      (PVOID)Context->Eax);
        LogQemuPrintf("%s|BUGCHECK: - EBP = %p\n", __MODULE__,
                      (PVOID)Context->Ebp);
        LogQemuPrintf("%s|BUGCHECK: - EIP = %p\n", __MODULE__,
                      (PVOID)Context->Eip);
        LogQemuPrintf("%s|BUGCHECK: - ESP = %p\n", __MODULE__,
                      (PVOID)Context->Esp);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Nothing to to
    }
}

static DECLSPEC_NOINLINE VOID
DebugStackDump(
    IN  PCONTEXT    Context
    )
{
#define PARAMETER_COUNT     3
#define MAXIMUM_ITERATIONS  20

    __try {
        ULONG_PTR   EBP;
        ULONG       Iteration;

        DebugDumpContext(Context);

        EBP = (ULONG_PTR)Context->Ebp;

        LogQemuPrintf("%s|BUGCHECK: STACK:\n", __MODULE__);

        for (Iteration = 0; Iteration < MAXIMUM_ITERATIONS; Iteration++) {
            ULONG_PTR   NextEBP;
            ULONG_PTR   EIP;
            ULONG_PTR   Parameter[PARAMETER_COUNT] = {0};
            ULONG       Index;
            PCHAR       Name;
            ULONG_PTR   Offset;

            NextEBP = *(PULONG_PTR)EBP;
            EIP = *(PULONG_PTR)(EBP + 4);

            if (EIP == 0)
                break;

            Index = 0;
            Offset = 8;
            for (;;) {
                if (EBP + Offset >= NextEBP)
                    break;

                if (Index == PARAMETER_COUNT)
                    break;

                Parameter[Index] = *(PULONG_PTR)(EBP + Offset);

                Index += 1;
                Offset += 4;
            }

            ModuleLookup(EIP, &Name, &Offset);

            if (Name != NULL)
                LogQemuPrintf("%s|BUGCHECK: %p: (%p %p %p) %s + %p\n", __MODULE__,
                              EBP,
                              (PVOID)Parameter[0],
                              (PVOID)Parameter[1],
                              (PVOID)Parameter[2],
                              Name,
                              (PVOID)Offset);
            else
                LogQemuPrintf("%s|BUGCHECK: %p: (%p %p %p) %p\n", __MODULE__,
                              EBP,
                              (PVOID)Parameter[0],
                              (PVOID)Parameter[1],
                              (PVOID)Parameter[2],
                              (PVOID)EIP);

            EBP = NextEBP;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // End of stack
    }

#undef  MAXIMUM_ITERATIONS
#undef  PARAMETER_COUNT
}
#elif defined(__x86_64__)
static DECLSPEC_NOINLINE VOID
DebugDumpContext(
    IN  PCONTEXT    Context
    )
{
    __try {
        LogQemuPrintf("%s|BUGCHECK: CONTEXT (%p):\n", __MODULE__,
                      Context);
        LogQemuPrintf("%s|BUGCHECK: - GS = %p\n", __MODULE__,
                      (PVOID)Context->SegGs);
        LogQemuPrintf("%s|BUGCHECK: - FS = %p\n", __MODULE__,
                      (PVOID)Context->SegFs);
        LogQemuPrintf("%s|BUGCHECK: - ES = %p\n", __MODULE__,
                      (PVOID)Context->SegEs);
        LogQemuPrintf("%s|BUGCHECK: - DS = %p\n", __MODULE__,
                      (PVOID)Context->SegDs);
        LogQemuPrintf("%s|BUGCHECK: - SS = %p\n", __MODULE__,
                      (PVOID)Context->SegSs);
        LogQemuPrintf("%s|BUGCHECK: - CS = %p\n", __MODULE__,
                      (PVOID)Context->SegCs);

        LogQemuPrintf("%s|BUGCHECK: - EFLAGS = %p\n", __MODULE__,
                      (PVOID)Context->EFlags);

        LogQemuPrintf("%s|BUGCHECK: - RDI = %p\n", __MODULE__,
                      (PVOID)Context->Rdi);
        LogQemuPrintf("%s|BUGCHECK: - RSI = %p\n", __MODULE__,
                      (PVOID)Context->Rsi);
        LogQemuPrintf("%s|BUGCHECK: - RBX = %p\n", __MODULE__,
                      (PVOID)Context->Rbx);
        LogQemuPrintf("%s|BUGCHECK: - RDX = %p\n", __MODULE__,
                      (PVOID)Context->Rdx);
        LogQemuPrintf("%s|BUGCHECK: - RCX = %p\n", __MODULE__,
                      (PVOID)Context->Rcx);
        LogQemuPrintf("%s|BUGCHECK: - RAX = %p\n", __MODULE__,
                      (PVOID)Context->Rax);
        LogQemuPrintf("%s|BUGCHECK: - RBP = %p\n", __MODULE__,
                      (PVOID)Context->Rbp);
        LogQemuPrintf("%s|BUGCHECK: - RIP = %p\n", __MODULE__,
                      (PVOID)Context->Rip);
        LogQemuPrintf("%s|BUGCHECK: - RSP = %p\n", __MODULE__,
                      (PVOID)Context->Rsp);
        LogQemuPrintf("%s|BUGCHECK: - R8 = %p\n", __MODULE__,
                      (PVOID)Context->R8);
        LogQemuPrintf("%s|BUGCHECK: - R9 = %p\n", __MODULE__,
                      (PVOID)Context->R9);
        LogQemuPrintf("%s|BUGCHECK: - R10 = %p\n", __MODULE__,
                      (PVOID)Context->R10);
        LogQemuPrintf("%s|BUGCHECK: - R11 = %p\n", __MODULE__,
                      (PVOID)Context->R11);
        LogQemuPrintf("%s|BUGCHECK: - R12 = %p\n", __MODULE__,
                      (PVOID)Context->R12);
        LogQemuPrintf("%s|BUGCHECK: - R13 = %p\n", __MODULE__,
                      (PVOID)Context->R13);
        LogQemuPrintf("%s|BUGCHECK: - R14 = %p\n", __MODULE__,
                      (PVOID)Context->R14);
        LogQemuPrintf("%s|BUGCHECK: - R15 = %p\n", __MODULE__,
                      (PVOID)Context->R15);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Nothing to to
    }
}

typedef struct _RUNTIME_FUNCTION {
    ULONG BeginAddress;
    ULONG EndAddress;
    ULONG UnwindData;
} RUNTIME_FUNCTION, *PRUNTIME_FUNCTION;

#define UNWIND_HISTORY_TABLE_SIZE 12

typedef struct _UNWIND_HISTORY_TABLE_ENTRY {
        ULONG64 ImageBase;
        PRUNTIME_FUNCTION FunctionEntry;
} UNWIND_HISTORY_TABLE_ENTRY, *PUNWIND_HISTORY_TABLE_ENTRY;

#define UNWIND_HISTORY_TABLE_NONE 0
#define UNWIND_HISTORY_TABLE_GLOBAL 1
#define UNWIND_HISTORY_TABLE_LOCAL 2

typedef struct _UNWIND_HISTORY_TABLE {
        ULONG Count;
        UCHAR Search;
        UCHAR RaiseStatusIndex;
        BOOLEAN Unwind;
        BOOLEAN Exception;
        ULONG64 LowAddress;
        ULONG64 HighAddress;
        UNWIND_HISTORY_TABLE_ENTRY Entry[UNWIND_HISTORY_TABLE_SIZE];
} UNWIND_HISTORY_TABLE, *PUNWIND_HISTORY_TABLE;

extern PRUNTIME_FUNCTION
RtlLookupFunctionEntry(
    __in ULONG64 ControlPc,
    __out PULONG64 ImageBase,
    __inout_opt PUNWIND_HISTORY_TABLE HistoryTable OPTIONAL
    );

#pragma prefast(suppress:28301) // No annotations
typedef EXCEPTION_DISPOSITION (*PEXCEPTION_ROUTINE) (
    __in struct _EXCEPTION_RECORD *ExceptionRecord,
    __in PVOID EstablisherFrame,
    __inout struct _CONTEXT *ContextRecord,
    __inout PVOID DispatcherContext
    );

#pragma warning(push)
#pragma warning(disable:4201) // nonstandard extension used : nameless struct/union

typedef struct _KNONVOLATILE_CONTEXT_POINTERS {
    union {
        PM128A FloatingContext[16];
        struct {
            PM128A Xmm0;
            PM128A Xmm1;
            PM128A Xmm2;
            PM128A Xmm3;
            PM128A Xmm4;
            PM128A Xmm5;
            PM128A Xmm6;
            PM128A Xmm7;
            PM128A Xmm8;
            PM128A Xmm9;
            PM128A Xmm10;
            PM128A Xmm11;
            PM128A Xmm12;
            PM128A Xmm13;
            PM128A Xmm14;
            PM128A Xmm15;
        };
    };

    union {
        PULONG64 IntegerContext[16];
        struct {
            PULONG64 Rax;
            PULONG64 Rcx;
            PULONG64 Rdx;
            PULONG64 Rbx;
            PULONG64 Rsp;
            PULONG64 Rbp;
            PULONG64 Rsi;
            PULONG64 Rdi;
            PULONG64 R8;
            PULONG64 R9;
            PULONG64 R10;
            PULONG64 R11;
            PULONG64 R12;
            PULONG64 R13;
            PULONG64 R14;
            PULONG64 R15;
        };
    };
} KNONVOLATILE_CONTEXT_POINTERS, *PKNONVOLATILE_CONTEXT_POINTERS;

#pragma warning(pop)

#define UNW_FLAG_NHANDLER   0
#define UNW_FLAG_EHANDLER   1
#define UNW_FLAG_UHANDLER   2

extern PEXCEPTION_ROUTINE
RtlVirtualUnwind(
    __in ULONG HandlerType,
    __in ULONG64 ImageBase,
    __in ULONG64 ControlPc,
    __in PRUNTIME_FUNCTION FunctionEntry,
    __inout PCONTEXT ContextRecord,
    __out PVOID *HandlerData,
    __out PULONG64 EstablisherFrame,
    __inout_opt PKNONVOLATILE_CONTEXT_POINTERS ContextPointers OPTIONAL
    );

static DECLSPEC_NOINLINE VOID
DebugStackDump(
    IN  PCONTEXT    Context
    )
{
#define PARAMETER_COUNT     4
#define MAXIMUM_ITERATIONS  20

    __try {
        ULONG   Iteration;

        DebugDumpContext(Context);

        LogQemuPrintf("%s|BUGCHECK: STACK:\n", __MODULE__);	

        for (Iteration = 0; Iteration < MAXIMUM_ITERATIONS; Iteration++) {
            PRUNTIME_FUNCTION   FunctionEntry;
            ULONG_PTR           ImageBase;
            ULONG_PTR           RIP;
            ULONG_PTR           RSP;
            ULONG_PTR           Parameter[PARAMETER_COUNT] = {0};
            ULONG               Index;
            PCHAR               Name;
            ULONG_PTR           Offset;

            if (Context->Rip == 0)
                break;

            FunctionEntry = RtlLookupFunctionEntry(Context->Rip,
                                                   &ImageBase,
                                                   NULL);

            if (FunctionEntry != NULL) {
                CONTEXT                         UnwindContext;
                ULONG64                         ControlPc;
                PVOID                           HandlerData;
                ULONG64                         EstablisherFrame;
                KNONVOLATILE_CONTEXT_POINTERS   ContextPointers;

                UnwindContext = *Context;
                ControlPc = Context->Rip;
                HandlerData = NULL;
                EstablisherFrame = 0;
                RtlZeroMemory(&ContextPointers, sizeof (KNONVOLATILE_CONTEXT_POINTERS));

                (VOID) RtlVirtualUnwind(UNW_FLAG_UHANDLER,
                                        ImageBase,
                                        ControlPc,
                                        FunctionEntry,
                                        &UnwindContext,
                                        &HandlerData,
                                        &EstablisherFrame,
                                        &ContextPointers);

                *Context = UnwindContext;
            } else {
                Context->Rip = *(PULONG64)(Context->Rsp);
                Context->Rsp += sizeof (ULONG64);
            }

            RSP = Context->Rsp;
            RIP = Context->Rip;

            Index = 0;
            Offset = 0;
            for (;;) {
                if (Index == PARAMETER_COUNT)
                    break;

                Parameter[Index] = *(PULONG64)(RSP + Offset);

                Index += 1;
                Offset += 8;
            }

            ModuleLookup(RIP, &Name, &Offset);

            if (Name != NULL)
                LogQemuPrintf("%s|BUGCHECK: %p: (%p %p %p %p) %s + %p\n", __MODULE__,
                              RSP,
                              (PVOID)Parameter[0],
                              (PVOID)Parameter[1],
                              (PVOID)Parameter[2],
                              (PVOID)Parameter[3],
                              Name,
                              (PVOID)Offset);
            else
                LogQemuPrintf("%s|BUGCHECK: %p: (%p %p %p %p) %p\n", __MODULE__,
                              RSP,
                              (PVOID)Parameter[0],
                              (PVOID)Parameter[1],
                              (PVOID)Parameter[2],
                              (PVOID)Parameter[3],
                              (PVOID)RIP);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Error of some kind
    }
}
#else
#error 'Unrecognised architecture'
#endif

extern VOID
RtlCaptureContext(
    __out PCONTEXT    Context
    );

static DECLSPEC_NOINLINE VOID
DebugIrqlNotLessOrEqual(
    IN  ULONG_PTR   Parameter1,
    IN  ULONG_PTR   Parameter2,
    IN  ULONG_PTR   Parameter3,
    IN  ULONG_PTR   Parameter4
    )
{
    __try {
        CONTEXT     Context;
        PVOID       Memory = (PVOID)Parameter1;
        KIRQL       Irql = (KIRQL)Parameter2;
        ULONG_PTR   Access = Parameter3;
        PVOID       Address = (PVOID)Parameter4;
        PCHAR       Name;
        ULONG_PTR   Offset;

        LogQemuPrintf("%s|BUGCHECK: MEMORY REFERENCED: %p\n", __MODULE__,
                      Memory);
        LogQemuPrintf("%s|BUGCHECK:              IRQL: %02x\n", __MODULE__,
                      Irql);
        LogQemuPrintf("%s|BUGCHECK:            ACCESS: %p\n", __MODULE__,
                      (PVOID)Access);

        ModuleLookup((ULONG_PTR)Address, &Name, &Offset);

        if (Name != NULL)
            LogQemuPrintf("%s|BUGCHECK:           ADDRESS: %s + %p\n", __MODULE__,
                          Name,
                          Offset);
        else
            LogQemuPrintf("%s|BUGCHECK:           ADDRESS: %p\n", __MODULE__,
                          Address);

        RtlCaptureContext(&Context);
        DebugStackDump(&Context);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Error of some kind
    }
}

static DECLSPEC_NOINLINE VOID
DebugDriverIrqlNotLessOrEqual(
    IN  ULONG_PTR   Parameter1,
    IN  ULONG_PTR   Parameter2,
    IN  ULONG_PTR   Parameter3,
    IN  ULONG_PTR   Parameter4
    )
{
    __try {
        CONTEXT     Context;
        PVOID       Memory = (PVOID)Parameter1;
        KIRQL       Irql = (KIRQL)Parameter2;
        ULONG_PTR   Access = Parameter3;
        PVOID       Address = (PVOID)Parameter4;
        PCHAR       Name;
        ULONG_PTR   Offset;

        LogQemuPrintf("%s|BUGCHECK: MEMORY REFERENCED: %p\n", __MODULE__,
                      Memory);
        LogQemuPrintf("%s|BUGCHECK:              IRQL: %02X\n", __MODULE__,
                      Irql);
        LogQemuPrintf("%s|BUGCHECK:            ACCESS: %p\n", __MODULE__,
                      (PVOID)Access);

        ModuleLookup((ULONG_PTR)Address, &Name, &Offset);

        if (Name != NULL)
            LogQemuPrintf("%s|BUGCHECK:           ADDRESS: %s + %p\n", __MODULE__,
                          Name,
                          Offset);
        else
            LogQemuPrintf("%s|BUGCHECK:           ADDRESS: %p\n", __MODULE__,
                          Address);

        RtlCaptureContext(&Context);
        DebugStackDump(&Context);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Error of some kind
    }
}

static DECLSPEC_NOINLINE VOID
DebugSystemServiceException(
    IN  ULONG_PTR   Parameter1,
    IN  ULONG_PTR   Parameter2,
    IN  ULONG_PTR   Parameter3,
    IN  ULONG_PTR   Parameter4
    )
{
    __try {
        PEXCEPTION_RECORD   Exception = (PEXCEPTION_RECORD)Parameter2;
        PCONTEXT            Context = (PCONTEXT)Parameter3;

        UNREFERENCED_PARAMETER(Parameter1);
        UNREFERENCED_PARAMETER(Parameter4);

        DebugDumpExceptionRecord(Exception);

        DebugStackDump(Context);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Error of some kind
    }
}

static DECLSPEC_NOINLINE VOID
DebugSystemThreadExceptionNotHandled(
    IN  ULONG_PTR   Parameter1,
    IN  ULONG_PTR   Parameter2,
    IN  ULONG_PTR   Parameter3,
    IN  ULONG_PTR   Parameter4
    )
{
    __try {
        ULONG               Code = (ULONG)Parameter1;
        PVOID               Address = (PVOID)Parameter2;
        PEXCEPTION_RECORD   Exception = (PEXCEPTION_RECORD)Parameter3;
        PCONTEXT            Context = (PCONTEXT)Parameter4;
        PCHAR               Name;
        ULONG_PTR           Offset;

        ModuleLookup((ULONG_PTR)Address, &Name, &Offset);

        if (Name != NULL)
            LogQemuPrintf("%s|BUGCHECK: %08X AT %s + %p\n", __MODULE__,
                          Code,
                          Name,
                          Offset);
        else
            LogQemuPrintf("%s|BUGCHECK: %08X AT %p\n", __MODULE__,
                          Code,
                          Name,
                          Address);

        DebugDumpExceptionRecord(Exception);

        DebugStackDump(Context);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Error of some kind
    }
}

static DECLSPEC_NOINLINE VOID
DebugKernelModeExceptionNotHandled(
    IN  ULONG_PTR   Parameter1,
    IN  ULONG_PTR   Parameter2,
    IN  ULONG_PTR   Parameter3,
    IN  ULONG_PTR   Parameter4
    )
{
    __try {
        CONTEXT     Context;
        ULONG       Code = (ULONG)Parameter1;
        PVOID       Address = (PVOID)Parameter2;
        PCHAR       Name;
        ULONG_PTR	Offset;

        UNREFERENCED_PARAMETER(Parameter3);
        UNREFERENCED_PARAMETER(Parameter4);

        ModuleLookup((ULONG_PTR)Address, &Name, &Offset);

        if (Name != NULL)
            LogQemuPrintf("%s|BUGCHECK: %08X AT %s + %p\n", __MODULE__,
                          Code,
                          Name,
                          Offset);
        else
            LogQemuPrintf("%s|BUGCHECK: %08X AT %p\n", __MODULE__,
                          Code,
                          Name,
                          Address);

        LogQemuPrintf("%s|BUGCHECK: - Code = %08X\n", __MODULE__,
                      Code);

        RtlCaptureContext(&Context);
        DebugStackDump(&Context);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Error of some kind
    }
}

static DECLSPEC_NOINLINE VOID
DebugCriticalObjectTermination(
    IN  ULONG_PTR   Parameter1,
    IN  ULONG_PTR   Parameter2,
    IN  ULONG_PTR   Parameter3,
    IN  ULONG_PTR   Parameter4
    )
{
    __try {
        ULONG       Type = (ULONG)Parameter1;
        PVOID	    Object = (PVOID)Parameter2;
        PCHAR	    Name = (PCHAR)Parameter3;
        PCHAR       Reason = (PCHAR)Parameter4;
        CONTEXT     Context;

        LogQemuPrintf("%s|BUGCHECK: Type = %08X\n", __MODULE__,
                      Type);
        LogQemuPrintf("%s|BUGCHECK: Object = %p\n", __MODULE__,
                      Object);
        LogQemuPrintf("%s|BUGCHECK: Name = %s\n", __MODULE__,
                      Name);
        LogQemuPrintf("%s|BUGCHECK: Reason = %s\n", __MODULE__,
                      Reason);

        RtlCaptureContext(&Context);
        DebugStackDump(&Context);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Error of some kind
    }
}

static DECLSPEC_NOINLINE VOID
DebugInaccessibleBootDevice(
    IN  ULONG_PTR   Parameter1,
    IN  ULONG_PTR   Parameter2,
    IN  ULONG_PTR   Parameter3,
    IN  ULONG_PTR   Parameter4
    )
{
    __try {
        PUNICODE_STRING Unicode = (PUNICODE_STRING)Parameter1;
        CONTEXT         Context;

        UNREFERENCED_PARAMETER(Parameter2);
        UNREFERENCED_PARAMETER(Parameter3);
        UNREFERENCED_PARAMETER(Parameter4);

        LogQemuPrintf("%s|BUGCHECK: %wZ\n", __MODULE__,
                      Unicode);

        RtlCaptureContext(&Context);
        DebugStackDump(&Context);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Error of some kind
    }
}

static DECLSPEC_NOINLINE VOID
DebugDriverPowerStateFailure(
    IN  ULONG_PTR       Parameter1,
    IN  ULONG_PTR       Parameter2,
    IN  ULONG_PTR       Parameter3,
    IN  ULONG_PTR       Parameter4
    )
{
    __try {
        ULONG_PTR       Code = Parameter1;

        UNREFERENCED_PARAMETER(Parameter3);

        LogQemuPrintf("%s|BUGCHECK: Code %08x\n", __MODULE__,
                      Code);

        switch (Code) {
        case 0x1: {
            PDEVICE_OBJECT  DeviceObject = (PDEVICE_OBJECT)Parameter2;

            LogQemuPrintf("%s|BUGCHECK: OUTSTANDING IRP (Device Object %p)\n", __MODULE__,
                          DeviceObject);

            break;
        }
        case 0x3: {
            PDEVICE_OBJECT      DeviceObject = (PDEVICE_OBJECT)Parameter2;
            PIRP                Irp = (PIRP)Parameter4;
            PIO_STACK_LOCATION  StackLocation;
            LONG                Index;

            LogQemuPrintf("%s|BUGCHECK: OUTSTANDING IRP %p (Device Object %p)\n", __MODULE__,
                          Irp,
                          DeviceObject);

            StackLocation = IoGetCurrentIrpStackLocation(Irp);

            LogQemuPrintf("%s|BUGCHECK: IRP STACK:\n", __MODULE__);	

            for (Index = 0; Index <= Irp->StackCount; Index++) {
                PCHAR       Name;
                ULONG_PTR   Offset;

                LogQemuPrintf("%s|BUGCHECK: [%c%u] %02x %02x %02x %02x\n", __MODULE__,
                              (Index == Irp->CurrentLocation) ? '>' : ' ',
                              Index,
                              StackLocation->MajorFunction,
                              StackLocation->MinorFunction,
                              StackLocation->Flags,
                              StackLocation->Control);

                ModuleLookup((ULONG_PTR)StackLocation->CompletionRoutine, &Name, &Offset);

                if (Name != NULL)
                    LogQemuPrintf("%s|BUGCHECK: [%c%u] CompletionRoutine = %s + %p\n", __MODULE__,
                                  (Index == Irp->CurrentLocation) ? '>' : ' ',
                                  Index,
                                  Name,
                                  (PVOID)Offset);
                else
                    LogQemuPrintf("%s|BUGCHECK: [%c%u] CompletionRoutine = %p\n", __MODULE__,
                                  (Index == Irp->CurrentLocation) ? '>' : ' ',
                                  Index,
                                  StackLocation->CompletionRoutine);

                LogQemuPrintf("%s|BUGCHECK: [%c%u] Context = %p\n", __MODULE__,
                              (Index == Irp->CurrentLocation) ? '>' : ' ',
                              Index,
                              StackLocation->Context);

                StackLocation++;
            } 

            break;
        }
        default:
            break;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Error of some kind
    }
}

static DECLSPEC_NOINLINE VOID
DebugAssertionFailure(
    IN  ULONG_PTR   Parameter1,
    IN  ULONG_PTR   Parameter2,
    IN  ULONG_PTR   Parameter3,
    IN  ULONG_PTR   Parameter4
    )
{
    __try {
        PCHAR       Text = (PCHAR)Parameter1;
        PCHAR       File = (PCHAR)Parameter2;
        ULONG       Line = (ULONG)Parameter3;
        CONTEXT     Context;

        UNREFERENCED_PARAMETER(Parameter4);

        LogQemuPrintf("%s|BUGCHECK: FILE: %s LINE: %u\n", __MODULE__,
                      File,
                      Line);
        LogQemuPrintf("%s|BUGCHECK: TEXT: %s\n", __MODULE__,
                      Text);

        RtlCaptureContext(&Context);
        DebugStackDump(&Context);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Error of some kind
    }
}

struct _BUG_CODE_ENTRY {
    ULONG       Code;
    const CHAR  *Name;
    VOID        (*Handler)(ULONG_PTR, ULONG_PTR, ULONG_PTR, ULONG_PTR);
};

#define DEFINE_HANDLER(_Code, _Function) \
        { (_Code), #_Code, (_Function) }

struct _BUG_CODE_ENTRY   BugCodeTable[] = {
    DEFINE_HANDLER(IRQL_NOT_LESS_OR_EQUAL, DebugIrqlNotLessOrEqual),
    DEFINE_HANDLER(DRIVER_IRQL_NOT_LESS_OR_EQUAL, DebugDriverIrqlNotLessOrEqual),
    DEFINE_HANDLER(SYSTEM_SERVICE_EXCEPTION, DebugSystemServiceException),
    DEFINE_HANDLER(SYSTEM_THREAD_EXCEPTION_NOT_HANDLED, DebugSystemThreadExceptionNotHandled),
    DEFINE_HANDLER(SYSTEM_THREAD_EXCEPTION_NOT_HANDLED_M, DebugSystemThreadExceptionNotHandled),
    DEFINE_HANDLER(KERNEL_MODE_EXCEPTION_NOT_HANDLED, DebugKernelModeExceptionNotHandled),
    DEFINE_HANDLER(KERNEL_MODE_EXCEPTION_NOT_HANDLED_M, DebugKernelModeExceptionNotHandled),
    DEFINE_HANDLER(CRITICAL_OBJECT_TERMINATION, DebugCriticalObjectTermination),
    DEFINE_HANDLER(INACCESSIBLE_BOOT_DEVICE, DebugInaccessibleBootDevice),
    DEFINE_HANDLER(DRIVER_POWER_STATE_FAILURE, DebugDriverPowerStateFailure),
    DEFINE_HANDLER(ASSERTION_FAILURE, DebugAssertionFailure),
    { 0, NULL, NULL }
};

static DECLSPEC_NOINLINE VOID
DebugDefaultHandler(
    VOID
    )
{
    __try {
        CONTEXT Context;

        RtlCaptureContext(&Context);
        DebugStackDump(&Context);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Error of some kind
    }
}

KBUGCHECK_CALLBACK_ROUTINE DebugBugCheckCallback;

VOID                     
DebugBugCheckCallback(
    IN  PVOID               Argument,
    IN  ULONG               Length
    )
{
    extern PULONG_PTR       KiBugCheckData;
    ULONG                   Code;
    ULONG_PTR               Parameter1;
    ULONG_PTR               Parameter2;
    ULONG_PTR               Parameter3;
    ULONG_PTR               Parameter4;
    struct _BUG_CODE_ENTRY  *Entry;

    UNREFERENCED_PARAMETER(Argument);
    UNREFERENCED_PARAMETER(Length);

    (VOID) SchedShutdownCode(SHUTDOWN_crash);

    LogQemuPrintf("%s|BUGCHECK: ====>\n", __MODULE__);

    Code = (ULONG)KiBugCheckData[0];
    Parameter1 = KiBugCheckData[1];
    Parameter2 = KiBugCheckData[2];
    Parameter3 = KiBugCheckData[3];
    Parameter4 = KiBugCheckData[4];

    for (Entry = BugCodeTable; Entry->Code != 0; Entry++) {
        if (Code == Entry->Code) {
            LogQemuPrintf("%s|BUGCHECK: %s: %p %p %p %p\n", __MODULE__,
                          Entry->Name,
                          (PVOID)Parameter1,
                          (PVOID)Parameter2,
                          (PVOID)Parameter3,
                          (PVOID)Parameter4);

            Entry->Handler(Parameter1,
                            Parameter2,
                            Parameter3,
                            Parameter4);

            goto done;
        }
    }

    LogQemuPrintf("%s|BUGCHECK: %08X: %p %p %p %p\n", __MODULE__,
                  Code,
                  (PVOID)Parameter1,
                  (PVOID)Parameter2,
                  (PVOID)Parameter3,
                  (PVOID)Parameter4);

    DebugDefaultHandler();

done:
    LogQemuPrintf("%s|BUGCHECK: <====\n", __MODULE__);
}

#pragma warning(pop)

NTSTATUS
DebugInitialize(
    VOID)
{
    NTSTATUS    status;

    KeInitializeCallbackRecord(&DebugBugCheckCallbackRecord);

    status = STATUS_UNSUCCESSFUL;
    if (!KeRegisterBugCheckCallback(&DebugBugCheckCallbackRecord,
                                    DebugBugCheckCallback,
                                    NULL,
                                    0,
                                    (PUCHAR)__MODULE__))
        goto fail1;

    Info("callback registered\n");

    return STATUS_SUCCESS;

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}
