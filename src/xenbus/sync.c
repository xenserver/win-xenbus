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
#include <stdarg.h>
#include <xen.h>
#include <util.h>

#include "sync.h"
#include "dbg_print.h"
#include "assert.h"

// Routines to capture all CPUs in a spinning state with interrupts
// disabled (so that we remain in a known code context) and optionally
// execute a function on each CPU.
// These routines are used for suspend/resume and live snapshot.

// The general sequence of steps is follows:
//
// - SyncCaptureAndCall() is called on an arbitrary CPU
//
// - It raises to DISPATCH_LEVEL to avoid pre-emption and schedules
//   a DPC on each of the other CPUs.
//
// - It, and the DPC routines, all raise to HIGH_LEVEL, clear a
//   a bit corresponding to their CPU in a 'captured' mask, and then
//   spin waiting for the mask to become zero (i.e. all CPUs captured).
//
//   NOTE: There is a back-off in this spin. It is possible that CPU A
//         is waiting for an IPI to CPU B to complete, but CPU B is
//         spinning at HIGH_LEVEL. Thus CPU A will never make it to
//         HIGH_LEVEL. Thus if, while spinning at HIGH_LEVEL, we notice
//         that another CPU has not made it, we set our bit in the
//         'captured' mask again and briefly drop down to DISPATCH_LEVEL
//         before trying again. This should allow any pending IPI to
//         complete.
//
// - Once all CPUs are captured, each disables interrupts, executes an
//   optional function. All the DPC routined clear a bit corresponding
//   to their CPU in a 'completed' mask and then spin waiting for the
//   mask to become zero. The requesting CPU also executes the optional
//   function bit it then waits for the mask to only contain the bit
//   corresponding to its CPU and then returns from
//   SyncCaptureAndCall() at HIGH_LEVEL.
//
// - A subsequent call to SyncRelease() will clear the last
//   remaining bit (clearly it is necessarily executed on the same CPU
//   as SyncCaptureAndCall()) and thus allow all the DPC
//   routines to lower back to DISPATCH_LEVEL and complete.
//
// - SyncRelease() also lowers back to DISPATCH_LEVEL and then
//   back to the IRQL is was originally entered at.

typedef struct  _SYNC_CONTEXT {
    KDPC                Dpc[MAXIMUM_PROCESSORS];
    ULONG               Sequence;
    LONG                CompletionCount;
    BOOLEAN             DisableInterrupts[MAXIMUM_PROCESSORS];
    BOOLEAN             Exit[MAXIMUM_PROCESSORS];
} SYNC_CONTEXT, *PSYNC_CONTEXT;

static LONG SyncOwner = MAXIMUM_PROCESSORS;

static FORCEINLINE VOID
__SyncAcquire(
    IN  LONG    Cpu
    )
{
    LONG        Old;

    Old = InterlockedExchange(&SyncOwner, Cpu);
    ASSERT3U(Old, ==, MAXIMUM_PROCESSORS);
}

static FORCEINLINE VOID
__SyncRelease(
    IN  LONG    Cpu
    )
{
    LONG        Old;

    Old = InterlockedExchange(&SyncOwner, MAXIMUM_PROCESSORS);
    ASSERT3U(Old, ==, Cpu);
}

static SYNC_CONTEXT  SyncContext;

KDEFERRED_ROUTINE   SyncWorker;

#pragma intrinsic(_enable)
#pragma intrinsic(_disable)

VOID
#pragma prefast(suppress:28166) // Function does not restore IRQL
SyncWorker(
    IN  PKDPC   Dpc,
    IN  PVOID   Context,
    IN  PVOID   Argument1,
    IN  PVOID   Argument2
    )
{
    BOOLEAN     InterruptsDisabled;
    ULONG       Cpu;
    LONG        CpuCount;

    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(Argument1);
    UNREFERENCED_PARAMETER(Argument2);

    InterruptsDisabled = FALSE;
    Cpu = KeGetCurrentProcessorNumber();

    Trace("====> (%u)\n", Cpu);
    InterlockedIncrement(&SyncContext.CompletionCount);

    CpuCount = KeNumberProcessors;

    for (;;) {
        ULONG   Sequence;

        if (SyncContext.Exit[Cpu])
            break;

        if (SyncContext.DisableInterrupts[Cpu] == InterruptsDisabled) {
            SchedYield();
            KeMemoryBarrier();

            continue;
        }

        Sequence = SyncContext.Sequence;

        if (SyncContext.DisableInterrupts[Cpu]) {
            ULONG       Attempts;
            NTSTATUS    status;

            (VOID) KfRaiseIrql(HIGH_LEVEL);
            status = STATUS_SUCCESS;

            InterlockedIncrement(&SyncContext.CompletionCount);

            Attempts = 0;
            while (SyncContext.Sequence == Sequence &&
                   SyncContext.CompletionCount < CpuCount) {
                SchedYield();
                KeMemoryBarrier();

                if (++Attempts > 1000) {
                    LONG    Old;
                    LONG    New;

                    do {
                        Old = SyncContext.CompletionCount;
                        New = Old - 1;

                        if (Old == CpuCount)
                            break;
                    } while (InterlockedCompareExchange(&SyncContext.CompletionCount, New, Old) != Old);

                    if (Old < CpuCount) {
#pragma prefast(suppress:28138) // Use constant rather than variable
                        KeLowerIrql(DISPATCH_LEVEL);
                        status = STATUS_UNSUCCESSFUL;
                        break;
                    }
                }
            }
                    
            if (!NT_SUCCESS(status))
                continue;

            _disable();

            InterruptsDisabled = TRUE;
        } else {
            InterruptsDisabled = FALSE;

            _enable();

#pragma prefast(suppress:28138) // Use constant rather than variable
            KeLowerIrql(DISPATCH_LEVEL);

            InterlockedIncrement(&SyncContext.CompletionCount);

            while (SyncContext.Sequence == Sequence &&
                   SyncContext.CompletionCount < CpuCount) {
                SchedYield();
                KeMemoryBarrier();
            }

        }
    }

    Trace("<==== (%u)\n", Cpu);
    InterlockedIncrement(&SyncContext.CompletionCount);

    ASSERT(!InterruptsDisabled);
}

__drv_maxIRQL(DISPATCH_LEVEL)
__drv_raisesIRQL(DISPATCH_LEVEL)
VOID
SyncCapture(
    VOID
    )
{
    ULONG       Cpu;
    LONG        CpuCount;
    ULONG       Index;

    ASSERT3U(KeGetCurrentIrql(), ==, DISPATCH_LEVEL);

    Cpu = KeGetCurrentProcessorNumber();
    __SyncAcquire(Cpu);

    Trace("====> (%u)\n", Cpu);

    ASSERT(IsZeroMemory(&SyncContext, sizeof (SYNC_CONTEXT)));

    SyncContext.Sequence++;
    SyncContext.CompletionCount = 0;

    CpuCount = KeNumberProcessors;

    for (Index = 0; Index < (ULONG)CpuCount; Index++) {
        PKDPC   Dpc = &SyncContext.Dpc[Index];

        SyncContext.DisableInterrupts[Index] = FALSE;
        SyncContext.Exit[Index] = FALSE;

        if (Index == Cpu)
            continue;

        KeInitializeDpc(Dpc, SyncWorker, NULL);
        KeSetTargetProcessorDpc(Dpc, (CCHAR)Index);
        KeInsertQueueDpc(Dpc, NULL, NULL);
    }

    InterlockedIncrement(&SyncContext.CompletionCount);

    while (SyncContext.CompletionCount < CpuCount) {
        SchedYield();
        KeMemoryBarrier();
    }

    Trace("<==== (%u)\n", Cpu);
}

__drv_requiresIRQL(DISPATCH_LEVEL)
__drv_setsIRQL(HIGH_LEVEL)
VOID
SyncDisableInterrupts(
    VOID
    )
{
    LONG        CpuCount;
    ULONG       Index;
    ULONG       Attempts;
    NTSTATUS    status;

    Trace("====>\n");

    SyncContext.Sequence++;
    SyncContext.CompletionCount = 0;

    CpuCount = KeNumberProcessors;

    for (Index = 0; Index < (ULONG)CpuCount; Index++)
        SyncContext.DisableInterrupts[Index] = TRUE;

again:
    (VOID) KfRaiseIrql(HIGH_LEVEL);
    status = STATUS_SUCCESS;

    InterlockedIncrement(&SyncContext.CompletionCount);

    Attempts = 0;
    while (SyncContext.CompletionCount < CpuCount) {
        SchedYield();
        KeMemoryBarrier();

        if (++Attempts > 1000) {
            LONG    Old;
            LONG    New;

            do {
                Old = SyncContext.CompletionCount;
                New = Old - 1;

                if (Old == CpuCount)
                    break;
            } while (InterlockedCompareExchange(&SyncContext.CompletionCount, New, Old) != Old);

            if (Old < CpuCount) {
                LogPrintf(LOG_LEVEL_WARNING,
                          "SYNC: %d < %d\n",
                          Old,
                          CpuCount);

#pragma prefast(suppress:28138) // Use constant rather than variable
                KeLowerIrql(DISPATCH_LEVEL);
                status = STATUS_UNSUCCESSFUL;
                break;
            }
        }
    }
            
    if (!NT_SUCCESS(status))
        goto again;

    _disable();
}

__drv_requiresIRQL(HIGH_LEVEL)
__drv_setsIRQL(DISPATCH_LEVEL)
VOID
SyncEnableInterrupts(
    )
{
    KIRQL   Irql;
    LONG    CpuCount;
    ULONG   Index;

    _enable();

    Irql = KeGetCurrentIrql();
    ASSERT3U(Irql, ==, HIGH_LEVEL);

    SyncContext.Sequence++;
    SyncContext.CompletionCount = 0;

    CpuCount = KeNumberProcessors;

    for (Index = 0; Index < (ULONG)CpuCount; Index++)
        SyncContext.DisableInterrupts[Index] = FALSE;

    InterlockedIncrement(&SyncContext.CompletionCount);

    while (SyncContext.CompletionCount < CpuCount) {
        SchedYield();
        KeMemoryBarrier();
    }

#pragma prefast(suppress:28138) // Use constant rather than variable
    KeLowerIrql(DISPATCH_LEVEL);

    Trace("<====\n");
}

__drv_requiresIRQL(DISPATCH_LEVEL)
VOID
#pragma prefast(suppress:28167) // Function changes IRQL
SyncRelease(
    VOID
    )
{
    LONG        CpuCount;
    ULONG       Cpu;
    ULONG       Index;

    Trace("====>\n");

    SyncContext.Sequence++;
    SyncContext.CompletionCount = 0;

    CpuCount = KeNumberProcessors;

    for (Index = 0; Index < (ULONG)CpuCount; Index++)
        SyncContext.Exit[Index] = TRUE;

    InterlockedIncrement(&SyncContext.CompletionCount);

    while (SyncContext.CompletionCount < CpuCount) {
        SchedYield();
        KeMemoryBarrier();
    }

    RtlZeroMemory(&SyncContext, sizeof (SYNC_CONTEXT));

    Cpu = KeGetCurrentProcessorNumber();
    __SyncRelease(Cpu);

    Trace("<====\n");
}
