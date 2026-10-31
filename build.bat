@echo off
rem build.bat - MSVC build for ptpng (multi-arch objects + tools)
setlocal enabledelayedexpansion

call "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1

if not exist build mkdir build
if not exist build\obj mkdir build\obj

set CORE=/nologo /W3 /O2 /MT /DNDEBUG /D_CRT_SECURE_NO_WARNINGS /Iinclude /Isrc

rem --- core (scalar + SSE2 baseline) ---
cl %CORE% /c src\ptpng.c         /Fo"build\obj\ptpng.obj"         || exit /b 1
cl %CORE% /DPTPNG_HAVE_INFLATE_BMI2=1 /c src\ptpng_inflate.c /Fo"build\obj\ptpng_inflate.obj" || exit /b 1
cl %CORE% /arch:AVX2 /c src\ptpng_inflate_bmi2.c /Fo"build\obj\ptpng_inflate_bmi2.obj" || exit /b 1
cl %CORE% /c src\ptpng_inflate_fixed.c /Fo"build\obj\ptpng_inflate_fixed.obj" || exit /b 1
cl %CORE% /c src\ptpng_crc.c     /Fo"build\obj\ptpng_crc.obj"     || exit /b 1
cl %CORE% /c src\ptpng_crc_x86.c /Fo"build\obj\ptpng_crc_x86.obj" || exit /b 1
cl %CORE% /c src\ptpng_filters.c /Fo"build\obj\ptpng_filters.obj" || exit /b 1
cl %CORE% /c src\ptpng_encode.c /Fo"build\obj\ptpng_encode.obj" || exit /b 1
cl %CORE% /c src\ptpng_deflate.c /Fo"build\obj\ptpng_deflate.obj" || exit /b 1

rem --- AVX2 TU ---
cl %CORE% /arch:AVX2 /c src\ptpng_avx2.c /Fo"build\obj\ptpng_avx2.obj" || exit /b 1
cl %CORE% /arch:AVX2 /c src\ptpng_encode_avx2.c /Fo"build\obj\ptpng_encode_avx2.obj" || exit /b 1

rem --- tools ---
set OBJS=build\obj\ptpng.obj build\obj\ptpng_inflate.obj build\obj\ptpng_crc.obj build\obj\ptpng_filters.obj build\obj\ptpng_avx2.obj
set OBJS=%OBJS% build\obj\ptpng_encode.obj build\obj\ptpng_deflate.obj build\obj\ptpng_encode_avx2.obj
set OBJS=%OBJS% build\obj\ptpng_crc_x86.obj
set OBJS=%OBJS% build\obj\ptpng_inflate_fixed.obj build\obj\ptpng_inflate_bmi2.obj
cl %CORE% tools\ptpng_tool.c   /link /nologo /out:build\ptpng_tool.exe   %OBJS% || exit /b 1
cl %CORE% tools\inflate_test.c /link /nologo /out:build\inflate_test.exe %OBJS% || exit /b 1
cl %CORE% tools\filters_test.c /link /nologo /out:build\filters_test.exe %OBJS% || exit /b 1
cl %CORE% tools\sums_test.c    /link /nologo /out:build\sums_test.exe    %OBJS% || exit /b 1
cl %CORE% tools\encoder_test.c /link /nologo /out:build\encoder_test.exe %OBJS% || exit /b 1
cl %CORE% tools\encoder_filters_test.c /link /nologo /out:build\encoder_filters_test.exe %OBJS% || exit /b 1
cl %CORE% tools\deflate_test.c /link /nologo /out:build\deflate_test.exe %OBJS% || exit /b 1
cl %CORE% tools\inf_bench.c    /link /nologo /out:build\inf_bench.exe    %OBJS% || exit /b 1
cl %CORE% /Zi tools\pmp.c      /link /nologo /debug /out:build\pmp.exe   %OBJS% || exit /b 1

echo build OK
endlocal
