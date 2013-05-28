call "%VS%\VC\vcvarsall.bat" x86
echo msbuild.exe "%SDV_PROJ%" /t:sdv /p:Inputs="%SDV_ARG%" /p:Configuration="Windows 8 Release" /p:Platform=x64
msbuild.exe "%SDV_PROJ%" /t:sdv /p:Inputs="%SDV_ARG%" /p:Configuration="Windows 8 Release" /p:Platform=x64
if errorlevel 1 goto error
exit 0

:error
exit 1
