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

#pragma warning(disable:4152)   // nonstandard extension, function/data pointer conversion in expression

#define XEN_API __declspec(dllexport)

#include <ntddk.h>
#include <stdlib.h>

#include "registry.h"
#include "log.h"
#include "assert.h"
#include "high.h"

#define LOG_BUFFER_SIZE 256

struct _LOG_DISPOSITION {
    LOG_LEVEL   Mask;
    VOID        (*Function)(PVOID, PCHAR, ULONG);
    PVOID       Argument;
};

#define LOG_MAXIMUM_DISPOSITION 8

typedef struct _LOG_CONTEXT {
    LONG            References;
    BOOLEAN         Enabled;
    CHAR            Buffer[LOG_BUFFER_SIZE];
    ULONG           Offset;
    LOG_DISPOSITION Disposition[LOG_MAXIMUM_DISPOSITION];
    HIGH_LOCK       Lock;
} LOG_CONTEXT, *PLOG_CONTEXT;

static LOG_CONTEXT  LogContext;

static FORCEINLINE
__drv_maxIRQL(HIGH_LEVEL)
__drv_raisesIRQL(HIGH_LEVEL)
__drv_savesIRQL
KIRQL
__LogAcquireBuffer(
    IN  PLOG_CONTEXT    Context
    )
{
    return __AcquireHighLock(&Context->Lock);
}

static DECLSPEC_NOINLINE VOID
__drv_maxIRQL(HIGH_LEVEL)
__drv_requiresIRQL(HIGH_LEVEL)
__LogReleaseBuffer(
    IN  PLOG_CONTEXT                Context,
    IN  LOG_LEVEL                   Level,
    IN  __drv_restoresIRQL KIRQL    Irql
    )
{
    ULONG                           Index;

    for (Index = 0; Index < LOG_MAXIMUM_DISPOSITION; Index++) {
        PLOG_DISPOSITION    Disposition = &Context->Disposition[Index];

        if (Level & Disposition->Mask)
            Disposition->Function(Disposition->Argument, Context->Buffer, Context->Offset);
    }

    RtlZeroMemory(Context->Buffer, Context->Offset);
    Context->Offset = 0;

    ReleaseHighLock(&Context->Lock, Irql);
}

static FORCEINLINE VOID
__LogPut(
    IN  PLOG_CONTEXT    Context,
    IN  CHAR            Character
    )
{
    ASSERT(Context->Offset < LOG_BUFFER_SIZE);

    Context->Buffer[Context->Offset++] = Character;
}

static DECLSPEC_NOINLINE PCHAR
LogFormatNumber(
    IN  PCHAR       Buffer,
    IN  ULONGLONG   Value,
    IN  UCHAR       Base,
    IN  BOOLEAN     UpperCase
    )
{
    ULONGLONG       Next = Value / Base;

    if (Next != 0)
        Buffer = LogFormatNumber(Buffer, Next, Base, UpperCase);

    Value %= Base;

    if (Value < 10)
        *Buffer++ = '0' + (CHAR)Value;
    else
        *Buffer++ = ((UpperCase) ? 'A' : 'a') + (CHAR)(Value - 10);

    *Buffer = '\0';

    return Buffer;
}

#define LOG_FORMAT_NUMBER(_Arguments, _Type, _Character, _Buffer)                               \
        do {                                                                                    \
            U ## _Type  _Value = va_arg((_Arguments), U ## _Type);                              \
            BOOLEAN     _UpperCase = FALSE;                                                     \
            UCHAR       _Base = 0;                                                              \
            ULONG       _Index = 0;                                                             \
                                                                                                \
            if ((_Character) == 'd' && (_Type)_Value < 0) {                                     \
                _Value = -((_Type)_Value);                                                      \
                (_Buffer)[_Index++] = '-';                                                      \
            }                                                                                   \
                                                                                                \
            switch (_Character) {                                                               \
            case 'o':                                                                           \
                _Base = 8;                                                                      \
                break;                                                                          \
                                                                                                \
            case 'd':                                                                           \
            case 'u':                                                                           \
                _Base = 10;                                                                     \
                break;                                                                          \
                                                                                                \
            case 'p':                                                                           \
            case 'X':                                                                           \
                _UpperCase = TRUE;                                                              \
                /* FALLTHRU */                                                                  \
                                                                                                \
            case 'x':                                                                           \
                _Base = 16;                                                                     \
                break;                                                                          \
            }                                                                                   \
                                                                                                \
            (VOID) LogFormatNumber(&(_Buffer)[_Index], (ULONGLONG)_Value, _Base, _UpperCase);   \
        } while (FALSE)

static DECLSPEC_NOINLINE VOID
LogWriteBuffer(
    IN  PLOG_CONTEXT    Context, 
    IN  LONG            Count,
    IN  const CHAR      *Format,
    IN  va_list         Arguments
    )
{
    CHAR                Character;

    while ((Character = *Format++) != '\0') {
        UCHAR   Pad = 0;
        UCHAR   Long = 0;
        BOOLEAN Wide = FALSE;
        BOOLEAN ZeroPrefix = FALSE;
        BOOLEAN OppositeJustification = FALSE;
        
        if (Character != '%') {
            __LogPut(Context, Character);
            goto loop;
        }

        Character = *Format++;
        ASSERT(Character != '\0');

        if (Character == '-') {
            OppositeJustification = TRUE;
            Character = *Format++;
            ASSERT(Character != '\0');
        }

        if (isdigit((unsigned char)Character)) {
            ZeroPrefix = (Character == '0') ? TRUE : FALSE;

            while (isdigit((unsigned char)Character)) {
                Pad = (Pad * 10) + (Character - '0');
                Character = *Format++;
                ASSERT(Character != '\0');
            }
        }

        while (Character == 'l') {
            Long++;
            Character = *Format++;
            ASSERT(Character == 'd' ||
                   Character == 'u' ||
                   Character == 'o' ||
                   Character == 'x' ||
                   Character == 'X' ||
                   Character == 'l');
        }
        ASSERT3U(Long, <=, 2);

        while (Character == 'w') {
            Wide = TRUE;
            Character = *Format++;
            ASSERT(Character == 'c' ||
                   Character == 's' ||
                   Character == 'Z');
        }

        switch (Character) {
        case 'c': {
            if (Wide) {
                WCHAR   Value;
                Value = va_arg(Arguments, WCHAR);

                __LogPut(Context, (CHAR)Value);
            } else { 
                CHAR    Value;

                Value = va_arg(Arguments, CHAR);

                __LogPut(Context, Value);
            }
            break;
        }
        case 'p':
            ZeroPrefix = TRUE;
            Pad = sizeof (ULONG_PTR) * 2;
            Long = sizeof (ULONG_PTR) / sizeof (ULONG);
            /* FALLTHRU */

        case 'd':
        case 'u':
        case 'o':
        case 'x':
        case 'X': {
            CHAR    Buffer[23]; // Enough for 8 bytes in octal plus the NUL terminator
            ULONG   Length;
            ULONG   Index;

            if (Long == 2)
                LOG_FORMAT_NUMBER(Arguments, LONGLONG, Character, Buffer);
            else
                LOG_FORMAT_NUMBER(Arguments, LONG, Character, Buffer);

            Length = (ULONG)strlen(Buffer);
            if (!OppositeJustification) {
                while (Pad > Length) {
                    __LogPut(Context, (ZeroPrefix) ? '0' : ' ');
                    --Pad;
                }
            }
            for (Index = 0; Index < Length; Index++)
                __LogPut(Context, Buffer[Index]);
            if (OppositeJustification) {
                while (Pad > Length) {
                    __LogPut(Context, ' ');
                    --Pad;
                }
            }

            break;
        }
        case 's': {
            if (Wide) {
                PWCHAR  Value = va_arg(Arguments, PWCHAR);
                ULONG   Length;
                ULONG   Index;

                if (Value == NULL)
                    Value = L"(null)";

                Length = (ULONG)wcslen(Value);

                if (OppositeJustification) {
                    while (Pad > Length) {
                        __LogPut(Context, ' ');
                        --Pad;
                    }
                }

                for (Index = 0; Index < Length; Index++)
                    __LogPut(Context, (CHAR)Value[Index]);

                if (!OppositeJustification) {
                    while (Pad > Length) {
                        __LogPut(Context, ' ');
                        --Pad;
                    }
                }
            } else {
                PCHAR   Value = va_arg(Arguments, PCHAR);
                ULONG   Length;
                ULONG   Index;

                if (Value == NULL)
                    Value = "(null)";

                Length = (ULONG)strlen(Value);

                if (OppositeJustification) {
                    while (Pad > Length) {
                        __LogPut(Context, ' ');
                        --Pad;
                    }
                }

                for (Index = 0; Index < Length; Index++)
                    __LogPut(Context, Value[Index]);

                if (!OppositeJustification) {
                    while (Pad > Length) {
                        __LogPut(Context, ' ');
                        --Pad;
                    }
                }
            }

            break;
        }
        case 'Z': {
            if (Wide) {
                PUNICODE_STRING Value = va_arg(Arguments, PUNICODE_STRING);
                PWCHAR          Buffer;
                ULONG           Length;
                ULONG           Index;

                if (Value == NULL) {
                    Buffer = L"(null)";
                    Length = sizeof ("(null)") - 1;
                } else {
                    Buffer = Value->Buffer;
                    Length = Value->Length / sizeof (WCHAR);
                }

                if (OppositeJustification) {
                    while (Pad > Length) {
                        __LogPut(Context, ' ');
                        --Pad;
                    }
                }

                for (Index = 0; Index < Length; Index++)
                    __LogPut(Context, (CHAR)Buffer[Index]);

                if (!OppositeJustification) {
                    while (Pad > Length) {
                        __LogPut(Context, ' ');
                        --Pad;
                    }
                }
            } else {
                PANSI_STRING Value = va_arg(Arguments, PANSI_STRING);
                PCHAR        Buffer;
                ULONG        Length;
                ULONG        Index;

                if (Value == NULL) {
                    Buffer = "(null)";
                    Length = sizeof ("(null)") - 1;
                } else {
                    Buffer = Value->Buffer;
                    Length = Value->Length / sizeof (CHAR);
                }

                if (OppositeJustification) {
                    while (Pad > Length) {
                        __LogPut(Context, ' ');
                        --Pad;
                    }
                }

                for (Index = 0; Index < Length; Index++)
                    __LogPut(Context, Buffer[Index]);

                if (!OppositeJustification) {
                    while (Pad > Length) {
                        __LogPut(Context, ' ');
                        --Pad;
                    }
                }
            }

            break;
        }
        default:
            __LogPut(Context, Character);
            break;
        }

loop:
        if (--Count == 0)
            break;
    }
}

XEN_API
VOID
LogCchVPrintf(
    IN  LOG_LEVEL   Level,
    IN  ULONG       Count,
    IN  const CHAR  *Format,
    IN  va_list     Arguments
    )
{
    PLOG_CONTEXT    Context = &LogContext;
    KIRQL           Irql;

    Irql = __LogAcquireBuffer(Context);

    LogWriteBuffer(Context,
                   __min(Count, LOG_BUFFER_SIZE),
                   Format,
                   Arguments);

    __LogReleaseBuffer(Context, Level, Irql);
}

XEN_API
VOID
LogVPrintf(
    IN  LOG_LEVEL   Level,
    IN  const CHAR  *Format,
    IN  va_list     Arguments
    )
{
    LogCchVPrintf(Level, LOG_BUFFER_SIZE, Format, Arguments);
}

XEN_API
VOID
LogCchPrintf(
    IN  LOG_LEVEL   Level,
    IN  ULONG       Count,
    IN  const CHAR  *Format,
    ...
    )
{
    va_list         Arguments;

    va_start(Arguments, Format);
    LogCchVPrintf(Level, Count, Format, Arguments);
    va_end(Arguments);
}

XEN_API
VOID
LogPrintf(
    IN  LOG_LEVEL   Level,
    IN  const CHAR  *Format,
    ...
    )
{
    va_list         Arguments;

    va_start(Arguments, Format);
    LogCchVPrintf(Level, LOG_BUFFER_SIZE, Format, Arguments);
    va_end(Arguments);
}

typedef VOID
(*DBG_PRINT_CALLBACK)(
    PANSI_STRING    Ansi,
    ULONG           ComponentId,
    ULONG           Level
    );

static DECLSPEC_NOINLINE VOID
LogDebugPrint(
    IN  PANSI_STRING    Ansi,
    IN  ULONG           ComponentId,
    IN  ULONG           Level
    )
{
    PLOG_CONTEXT        Context = &LogContext;
    KIRQL               Irql;
    ULONG               Index;

    if (Ansi->Length == 0 || Ansi->Buffer == NULL)
        return;

    // If this is not a debug build then apply an aggressive
    // filter to reduce the noise.
#if !DBG
    if (Ansi->Length < sizeof ("XEN"))
        return;

    if (Ansi->Buffer[0] != 'X' ||
        Ansi->Buffer[1] != 'E' ||
        Ansi->Buffer[2] != 'N')
        return;
#endif

    AcquireHighLock(&Context->Lock, &Irql);

    for (Index = 0; Index < LOG_MAXIMUM_DISPOSITION; Index++) {
        PLOG_DISPOSITION    Disposition = &Context->Disposition[Index];

        if ((1 << Level) & Disposition->Mask)
            Disposition->Function(Disposition->Argument, Ansi->Buffer, Ansi->Length);
    }

    ReleaseHighLock(&Context->Lock, Irql);
}

VOID
LogTeardown(
    VOID
    )
{
    PLOG_CONTEXT    Context = &LogContext;

    if (Context->Enabled) {
        (VOID) DbgSetDebugPrintCallback(LogDebugPrint, FALSE); 
        Context->Enabled = FALSE;
    }

    RtlZeroMemory(&Context->Lock, sizeof (HIGH_LOCK));

    (VOID) InterlockedDecrement(&Context->References);

    ASSERT(IsZeroMemory(Context, sizeof (LOG_CONTEXT)));
}

NTSTATUS
LogAddDisposition(
    IN  LOG_LEVEL           Mask,
    IN  VOID                (*Function)(PVOID, PCHAR, ULONG),
    IN  PVOID               Argument OPTIONAL,
    OUT PLOG_DISPOSITION    *Disposition
    )
{
    PLOG_CONTEXT            Context = &LogContext;
    KIRQL                   Irql;
    ULONG                   Index;
    NTSTATUS                status;

    status = STATUS_INVALID_PARAMETER;
    if (Mask == 0)
        goto fail1;

    AcquireHighLock(&Context->Lock, &Irql);

    status = STATUS_UNSUCCESSFUL;
    for (Index = 0; Index < LOG_MAXIMUM_DISPOSITION; Index++) {
        *Disposition = &Context->Disposition[Index];

        if ((*Disposition)->Mask == 0) {
            (*Disposition)->Mask = Mask;
            (*Disposition)->Function = Function;
            (*Disposition)->Argument = Argument;

            status = STATUS_SUCCESS;
            break;
        }
    }

    if (!NT_SUCCESS(status))
        goto fail2;

    ReleaseHighLock(&Context->Lock, Irql);

    return STATUS_SUCCESS;

fail2:
    Error("fail2\n");

    *Disposition = NULL;

    ReleaseHighLock(&Context->Lock, Irql);

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

extern VOID
LogRemoveDisposition(
    IN  PLOG_DISPOSITION    Disposition
    )
{
    PLOG_CONTEXT            Context = &LogContext;
    KIRQL                   Irql;
    ULONG                   Index;

    AcquireHighLock(&Context->Lock, &Irql);

    for (Index = 0; Index < LOG_MAXIMUM_DISPOSITION; Index++) {
        if (&Context->Disposition[Index] != Disposition)
            continue;

        RtlZeroMemory(&Context->Disposition[Index], sizeof (LOG_DISPOSITION));
    }

    ReleaseHighLock(&Context->Lock, Irql);
}

static FORCEINLINE BOOLEAN
__LogDbgPrintCallbackEnable(
    VOID
    )
{
    CHAR            Key[] = "XEN:DBG_PRINT=";
    PANSI_STRING    Option;
    PCHAR           Value;
    BOOLEAN         Enable;
    NTSTATUS        status;

    Enable = TRUE;

    status = RegistryQuerySystemStartOption(Key, &Option);
    if (!NT_SUCCESS(status))
        goto done;

    Value = Option->Buffer + sizeof (Key) - 1;

    if (strcmp(Value, "OFF") == 0)
        Enable = FALSE;

    RegistryFreeSzValue(Option);

done:
    return Enable;
}

NTSTATUS
LogInitialize(
    VOID)
{
    PLOG_CONTEXT    Context = &LogContext;
    ULONG           References;
    NTSTATUS        status;

    References = InterlockedIncrement(&Context->References);

    status = STATUS_OBJECTID_EXISTS;
    if (References != 1)
        goto fail1;

    InitializeHighLock(&Context->Lock);

    if (__LogDbgPrintCallbackEnable()) {
        status = DbgSetDebugPrintCallback(LogDebugPrint, TRUE);

        ASSERT(!Context->Enabled);
        Context->Enabled = NT_SUCCESS(status) ? TRUE : FALSE;
    }

    return STATUS_SUCCESS;

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}
