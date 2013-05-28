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

#define INITGUID

#include <windows.h>
#include <setupapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <strsafe.h>
#include <malloc.h>

#include <version.h>

__user_code;

#define MAXIMUM_BUFFER_SIZE 1024

static VOID
#pragma prefast(suppress:6262) // Function uses '1036' bytes of stack: exceeds /analyze:stacksize'1024'
__Log(
    IN  const CHAR  *Format,
    IN  ...
    )
{
    TCHAR               Buffer[MAXIMUM_BUFFER_SIZE];
    va_list             Arguments;
    size_t              Length;
    SP_LOG_TOKEN        LogToken;
    DWORD               Category;
    DWORD               Flags;
    HRESULT             Result;

    va_start(Arguments, Format);
    Result = StringCchVPrintf(Buffer, MAXIMUM_BUFFER_SIZE, Format, Arguments);
    va_end(Arguments);

    if (Result != S_OK && Result != STRSAFE_E_INSUFFICIENT_BUFFER)
        return;

    Result = StringCchLength(Buffer, MAXIMUM_BUFFER_SIZE, &Length);
    if (Result != S_OK)
        return;

    LogToken = SetupGetThreadLogToken();
    Category = TXTLOG_VENDOR;
    Flags = TXTLOG_DETAILS;

    SetupWriteTextLog(LogToken, Category, Flags, Buffer);
    Length = __min(MAXIMUM_BUFFER_SIZE - 1, Length + 2);

    __analysis_assume(Length < MAXIMUM_BUFFER_SIZE);
    __analysis_assume(Length >= 2);
    Buffer[Length] = '\0';
    Buffer[Length - 1] = '\n';
    Buffer[Length - 2] = '\r';

    OutputDebugString(Buffer);
}

#define Log(_Format, ...) \
        __Log(__MODULE__ "|" __FUNCTION__ ": " _Format, __VA_ARGS__)

static PTCHAR
GetErrorMessage(
    IN  DWORD   Error
    )
{
    PTCHAR      Message;
    ULONG       Index;

    FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER | 
                  FORMAT_MESSAGE_FROM_SYSTEM |
                  FORMAT_MESSAGE_IGNORE_INSERTS,
                  NULL,
                  Error,
                  MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                  (LPTSTR)&Message,
                  0,
                  NULL);

    for (Index = 0; Message[Index] != '\0'; Index++) {
        if (Message[Index] == '\r' || Message[Index] == '\n') {
            Message[Index] = '\0';
            break;
        }
    }

    return Message;
}

static const CHAR *
FunctionName(
    IN  DI_FUNCTION Function
    )
{
#define _NAME(_Function)        \
        case DIF_ ## _Function: \
            return #_Function;

    switch (Function) {
    _NAME(INSTALLDEVICE);
    _NAME(REMOVE);
    _NAME(SELECTDEVICE);
    _NAME(ASSIGNRESOURCES);
    _NAME(PROPERTIES);
    _NAME(FIRSTTIMESETUP);
    _NAME(FOUNDDEVICE);
    _NAME(SELECTCLASSDRIVERS);
    _NAME(VALIDATECLASSDRIVERS);
    _NAME(INSTALLCLASSDRIVERS);
    _NAME(CALCDISKSPACE);
    _NAME(DESTROYPRIVATEDATA);
    _NAME(VALIDATEDRIVER);
    _NAME(MOVEDEVICE);
    _NAME(DETECT);
    _NAME(INSTALLWIZARD);
    _NAME(DESTROYWIZARDDATA);
    _NAME(PROPERTYCHANGE);
    _NAME(ENABLECLASS);
    _NAME(DETECTVERIFY);
    _NAME(INSTALLDEVICEFILES);
    _NAME(ALLOW_INSTALL);
    _NAME(SELECTBESTCOMPATDRV);
    _NAME(REGISTERDEVICE);
    _NAME(NEWDEVICEWIZARD_PRESELECT);
    _NAME(NEWDEVICEWIZARD_SELECT);
    _NAME(NEWDEVICEWIZARD_PREANALYZE);
    _NAME(NEWDEVICEWIZARD_POSTANALYZE);
    _NAME(NEWDEVICEWIZARD_FINISHINSTALL);
    _NAME(INSTALLINTERFACES);
    _NAME(DETECTCANCEL);
    _NAME(REGISTER_COINSTALLERS);
    _NAME(ADDPROPERTYPAGE_ADVANCED);
    _NAME(ADDPROPERTYPAGE_BASIC);
    _NAME(TROUBLESHOOTER);
    _NAME(POWERMESSAGEWAKE);
    default:
        break;
    }

    return "UNKNOWN";

#undef  _NAME
}

static BOOLEAN
InstallFilter(
    IN  const GUID  *Guid,
    IN  PTCHAR      Filter
    )
{
    HRESULT         Error;
    DWORD           Type;
    DWORD           OldLength;
    DWORD           NewLength;
    PTCHAR          UpperFilters;
    ULONG           Offset;

    if (!SetupDiGetClassRegistryProperty(Guid,
                                         SPCRP_UPPERFILTERS,
                                         &Type,
                                         NULL,
                                         0,
                                         &OldLength,
                                         NULL,
                                         NULL)) {
        Error = GetLastError();

        if (Error == ERROR_INVALID_DATA) {
            Type = REG_MULTI_SZ;
            OldLength = sizeof (TCHAR);
        } else if (Error != ERROR_INSUFFICIENT_BUFFER) {
            goto fail1;
        }
    }

    if (Type != REG_MULTI_SZ) {
        SetLastError(ERROR_BAD_FORMAT);
        goto fail2;
    }

    NewLength = OldLength + (DWORD)((strlen(Filter) + 1) * sizeof (TCHAR));

    UpperFilters = malloc(NewLength);
    if (UpperFilters == NULL)
        goto fail3;

    memset(UpperFilters, 0, NewLength);

    Offset = 0;
    if (OldLength != sizeof (TCHAR)) {
        if (!SetupDiGetClassRegistryProperty(Guid,
                                             SPCRP_UPPERFILTERS,
                                             &Type,
                                             (PBYTE)UpperFilters,
                                             OldLength,
                                             NULL,
                                             NULL,
                                             NULL))
            goto fail4;

        while (UpperFilters[Offset] != '\0') {
            ULONG   FilterLength;

            FilterLength = (ULONG)strlen(&UpperFilters[Offset]) / sizeof (TCHAR);

            if (_stricmp(&UpperFilters[Offset], Filter) == 0) {
                Log("%s already present", Filter);
                goto done;
            }

            Offset += FilterLength + 1;
        }
    }

    memmove(&UpperFilters[Offset], Filter, strlen(Filter));
    Log("added %s", Filter);

    if (!SetupDiSetClassRegistryProperty(Guid,
                                         SPCRP_UPPERFILTERS,
                                         (PBYTE)UpperFilters,
                                         NewLength,
                                         NULL,
                                         NULL))
        goto fail5;

done:
    free(UpperFilters);

    return TRUE;

fail5:
fail4:
    free(UpperFilters);

fail3:
fail2:
fail1:
    return FALSE;
}

static BOOLEAN
RemoveFilter(
    IN  const GUID  *Guid,
    IN  PTCHAR      Filter
    )
{
    DWORD           Type;
    DWORD           OldLength;
    DWORD           NewLength;
    PTCHAR          UpperFilters;
    ULONG           Offset;
    ULONG           FilterLength;

    if (!SetupDiGetClassRegistryProperty(Guid,
                                         SPCRP_UPPERFILTERS,
                                         &Type,
                                         NULL,
                                         0,
                                         &OldLength,
                                         NULL,
                                         NULL)) {
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER)
            goto fail1;
    }

    if (Type != REG_MULTI_SZ) {
        SetLastError(ERROR_BAD_FORMAT);
        goto fail2;
    }

    UpperFilters = malloc(OldLength);
    if (UpperFilters == NULL)
        goto fail3;

    memset(UpperFilters, 0, OldLength);

    if (!SetupDiGetClassRegistryProperty(Guid,
                                         SPCRP_UPPERFILTERS,
                                         &Type,
                                         (PBYTE)UpperFilters,
                                         OldLength,
                                         NULL,
                                         NULL,
                                         NULL))
        goto fail4;

    Offset = 0;
    FilterLength = 0;
    while (UpperFilters[Offset] != '\0') {
        FilterLength = (ULONG)strlen(&UpperFilters[Offset]) / sizeof (TCHAR);

        if (_stricmp(&UpperFilters[Offset], Filter) == 0)
            goto remove;

        Offset += FilterLength + 1;
    }

    free(UpperFilters);
    goto done;

remove:
    NewLength = OldLength - ((FilterLength + 1) * sizeof (TCHAR));

    memmove(&UpperFilters[Offset],
            &UpperFilters[Offset + FilterLength + 1],
            (NewLength - Offset) * sizeof (TCHAR));

    Log("removed %s", Filter);

    if (!SetupDiSetClassRegistryProperty(Guid,
                                         SPCRP_UPPERFILTERS,
                                         (PBYTE)UpperFilters,
                                         NewLength,
                                         NULL,
                                         NULL))
        goto fail5;

    free(UpperFilters);

done:
    return TRUE;

fail5:
fail4:
    free(UpperFilters);

fail3:
fail2:
fail1:
    return FALSE;
}

static BOOLEAN
RequestReboot(
    IN  HDEVINFO            DeviceInfoSet,
    IN  PSP_DEVINFO_DATA    DeviceInfoData
    )
{
    SP_DEVINSTALL_PARAMS    DeviceInstallParams;

    DeviceInstallParams.cbSize = sizeof (DeviceInstallParams);

    if (!SetupDiGetDeviceInstallParams(DeviceInfoSet,
                                       DeviceInfoData,
                                       &DeviceInstallParams))
        goto fail1;

    DeviceInstallParams.Flags |= DI_NEEDREBOOT;

    Log("Flags = %08x", DeviceInstallParams.Flags);

    if (!SetupDiSetDeviceInstallParams(DeviceInfoSet,
                                       DeviceInfoData,
                                       &DeviceInstallParams))
        goto fail2;

    return TRUE;

fail2:
fail1:
    return FALSE;
}

// {4d36e97d-e325-11ce-bfc1-08002be10318}
DEFINE_GUID(GUID_SYSTEM_DEVICE, 
            0x4d36e97d,
            0xe325,
            0x11ce,
            0xbf,
            0xc1,
            0x08,
            0x00,
            0x2b,
            0xe1,
            0x03,
            0x18
            );

// {4d36e96a-e325-11ce-bfc1-08002be10318}
DEFINE_GUID(GUID_HDC_DEVICE, 
            0x4d36e96a,
            0xe325,
            0x11ce,
            0xbf,
            0xc1,
            0x08,
            0x00,
            0x2b,
            0xe1,
            0x03,
            0x18
            );

static FORCEINLINE HRESULT
__DifInstallPreProcess(
    IN  HDEVINFO                    DeviceInfoSet,
    IN  PSP_DEVINFO_DATA            DeviceInfoData,
    IN  PCOINSTALLER_CONTEXT_DATA   Context
    )
{
    UNREFERENCED_PARAMETER(DeviceInfoSet);
    UNREFERENCED_PARAMETER(DeviceInfoData);
    UNREFERENCED_PARAMETER(Context);

    Log("<===>");

    return ERROR_DI_POSTPROCESSING_REQUIRED; 
}

static FORCEINLINE HRESULT
__DifInstallPostProcess(
    IN  HDEVINFO                    DeviceInfoSet,
    IN  PSP_DEVINFO_DATA            DeviceInfoData,
    IN  PCOINSTALLER_CONTEXT_DATA   Context
    )
{
    HRESULT                         Error;
    BOOLEAN                         Success;

    Log("====>");

    Error = Context->InstallResult;
    if (Error != NO_ERROR) {
        SetLastError(Error);
        goto fail1;
    }

    Success = InstallFilter(&GUID_SYSTEM_DEVICE, "XENFILT");
    if (!Success)
        goto fail2;

    Success = InstallFilter(&GUID_HDC_DEVICE, "XENFILT");
    if (!Success)
        goto fail3;

    Success = RequestReboot(DeviceInfoSet, DeviceInfoData);
    if (!Success)
        goto fail4;

    Log("<====");

    return NO_ERROR;

fail4:
    Log("fail4");

fail3:
    Log("fail3");

fail2:
    Log("fail2");

fail1:
    Error = GetLastError();

    {
        PTCHAR  Message;

        Message = GetErrorMessage(Error);
        Log("fail1 (%s)", Message);
        LocalFree(Message);
    }

    return Error;
}

static DECLSPEC_NOINLINE HRESULT
DifInstall(
    IN  HDEVINFO                    DeviceInfoSet,
    IN  PSP_DEVINFO_DATA            DeviceInfoData,
    IN  PCOINSTALLER_CONTEXT_DATA   Context
    )
{
    SP_DEVINSTALL_PARAMS            DeviceInstallParams;
    HRESULT                         Error;

    DeviceInstallParams.cbSize = sizeof (DeviceInstallParams);

    if (!SetupDiGetDeviceInstallParams(DeviceInfoSet,
                                       DeviceInfoData,
                                       &DeviceInstallParams))
        goto fail1;

    Log("Flags = %08x", DeviceInstallParams.Flags);

    Error = (!Context->PostProcessing) ?
            __DifInstallPreProcess(DeviceInfoSet, DeviceInfoData, Context) :
            __DifInstallPostProcess(DeviceInfoSet, DeviceInfoData, Context);

    return Error;

fail1:
    Error = GetLastError();

    {
        PTCHAR  Message;

        Message = GetErrorMessage(Error);
        Log("fail1 (%s)", Message);
        LocalFree(Message);
    }

    return Error;
}

static FORCEINLINE HRESULT
__DifRemovePreProcess(
    IN  HDEVINFO                    DeviceInfoSet,
    IN  PSP_DEVINFO_DATA            DeviceInfoData,
    IN  PCOINSTALLER_CONTEXT_DATA   Context
    )
{
    UNREFERENCED_PARAMETER(DeviceInfoSet);
    UNREFERENCED_PARAMETER(DeviceInfoData);
    UNREFERENCED_PARAMETER(Context);

    Log("<===>");

    return ERROR_DI_POSTPROCESSING_REQUIRED; 
}

static FORCEINLINE HRESULT
__DifRemovePostProcess(
    IN  HDEVINFO                    DeviceInfoSet,
    IN  PSP_DEVINFO_DATA            DeviceInfoData,
    IN  PCOINSTALLER_CONTEXT_DATA   Context
    )
{
    HRESULT                         Error;
    BOOLEAN                         Success;

    Error = Context->InstallResult;
    if (Error != NO_ERROR) {
        SetLastError(Error);
        goto fail1;
    }

    Success = RemoveFilter(&GUID_HDC_DEVICE, "XENFILT");
    if (!Success)
        goto fail2;

    Success = RemoveFilter(&GUID_SYSTEM_DEVICE, "XENFILT");
    if (!Success)
        goto fail3;

    Success = RequestReboot(DeviceInfoSet, DeviceInfoData);
    if (!Success)
        goto fail4;

    return NO_ERROR;

fail4:
fail3:
fail2:
fail1:
    Error = GetLastError();

    {
        PTCHAR  Message;

        Message = GetErrorMessage(Error);
        Log("fail1 (%s)", Message);
        LocalFree(Message);
    }

    return Error;
}

static DECLSPEC_NOINLINE HRESULT
DifRemove(
    IN  HDEVINFO                    DeviceInfoSet,
    IN  PSP_DEVINFO_DATA            DeviceInfoData,
    IN  PCOINSTALLER_CONTEXT_DATA   Context
    )
{
    SP_DEVINSTALL_PARAMS            DeviceInstallParams;
    HRESULT                         Error;

    DeviceInstallParams.cbSize = sizeof (DeviceInstallParams);

    if (!SetupDiGetDeviceInstallParams(DeviceInfoSet,
                                       DeviceInfoData,
                                       &DeviceInstallParams))
        goto fail1;

    Log("Flags = %08x", DeviceInstallParams.Flags);

    Error = (!Context->PostProcessing) ?
            __DifRemovePreProcess(DeviceInfoSet, DeviceInfoData, Context) :
            __DifRemovePostProcess(DeviceInfoSet, DeviceInfoData, Context);

    return Error;

fail1:
    Error = GetLastError();

    {
        PTCHAR  Message;

        Message = GetErrorMessage(Error);
        Log("fail1 (%s)", Message);
        LocalFree(Message);
    }

    return Error;
}

DWORD CALLBACK
Entry(
    IN  DI_FUNCTION                 Function,
    IN  HDEVINFO                    DeviceInfoSet,
    IN  PSP_DEVINFO_DATA            DeviceInfoData,
    IN  PCOINSTALLER_CONTEXT_DATA   Context
    )
{
    HRESULT                         Error;

    Log("%s (%s) ===>",
        MAJOR_VERSION_STR "." MINOR_VERSION_STR "." MICRO_VERSION_STR "." BUILD_NUMBER_STR,
        DAY_STR "/" MONTH_STR "/" YEAR_STR);

    switch (Function) {
    case DIF_INSTALLDEVICE: {
        SP_DRVINFO_DATA         DriverInfoData;
        BOOLEAN                 DriverInfoAvailable;

        DriverInfoData.cbSize = sizeof (DriverInfoData);
        DriverInfoAvailable = SetupDiGetSelectedDriver(DeviceInfoSet,
                                                       DeviceInfoData,
                                                       &DriverInfoData) ?
                              TRUE :
                              FALSE;

        // If there is no driver information then the NULL driver is being
        // installed. Treat this as we would a DIF_REMOVE.
        Error = (DriverInfoAvailable) ?
                DifInstall(DeviceInfoSet, DeviceInfoData, Context) :
                DifRemove(DeviceInfoSet, DeviceInfoData, Context);
        break;
    }
    case DIF_REMOVE:
        Error = DifRemove(DeviceInfoSet, DeviceInfoData, Context);
        break;
    default:
        if (!Context->PostProcessing) {
            Log("%s PreProcessing",
                FunctionName(Function));

            Error = NO_ERROR;
        } else {
            Log("%s PostProcessing (%08x)",
                FunctionName(Function),
                Context->InstallResult);

            Error = Context->InstallResult;
        }

        break;
    }

    Log("%s (%s) <===",
        MAJOR_VERSION_STR "." MINOR_VERSION_STR "." MICRO_VERSION_STR "." BUILD_NUMBER_STR,
        DAY_STR "/" MONTH_STR "/" YEAR_STR);

    return (DWORD)Error;
}

DWORD CALLBACK
Version(
    IN  HWND        Window,
    IN  HINSTANCE   Module,
    IN  PTCHAR      Buffer,
    IN  INT         Reserved
    )
{
    UNREFERENCED_PARAMETER(Window);
    UNREFERENCED_PARAMETER(Module);
    UNREFERENCED_PARAMETER(Buffer);
    UNREFERENCED_PARAMETER(Reserved);

    Log("%s (%s)",
        MAJOR_VERSION_STR "." MINOR_VERSION_STR "." MICRO_VERSION_STR "." BUILD_NUMBER_STR,
        DAY_STR "/" MONTH_STR "/" YEAR_STR);

    return NO_ERROR;
}

static FORCEINLINE const CHAR *
__ReasonName(
    IN  DWORD       Reason
    )
{
#define _NAME(_Reason)          \
        case DLL_ ## _Reason:   \
            return #_Reason;

    switch (Reason) {
    _NAME(PROCESS_ATTACH);
    _NAME(PROCESS_DETACH);
    _NAME(THREAD_ATTACH);
    _NAME(THREAD_DETACH);
    default:
        break;
    }

    return "UNKNOWN";

#undef  _NAME
}

BOOL WINAPI
DllMain(
    IN  HINSTANCE   Module,
    IN  DWORD       Reason,
    IN  PVOID       Reserved
    )
{
    UNREFERENCED_PARAMETER(Module);
    UNREFERENCED_PARAMETER(Reserved);

    Log("%s (%s): %s",
        MAJOR_VERSION_STR "." MINOR_VERSION_STR "." MICRO_VERSION_STR "." BUILD_NUMBER_STR,
        DAY_STR "/" MONTH_STR "/" YEAR_STR,
        __ReasonName(Reason));

    return TRUE;
}
