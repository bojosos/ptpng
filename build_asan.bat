@echo off
rem build_asan.bat - AddressSanitizer build of ptpng + fuzz tool
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
if not exist build\asan mkdir build\asan

set CORE=/nologo /W3 /O1 /Zi /MT /DNDEBUG /D_CRT_SECURE_NO_WARNINGS /Iinclude /Isrc /fsanitize=address

cl %CORE% /c src\ptpng.c         /Fo"build\asan\ptpng.obj"         || exit /b 1
cl %CORE% /DPTPNG_HAVE_INFLATE_BMI2=1 /c src\ptpng_inflate.c /Fo"build\asan\ptpng_inflate.obj" || exit /b 1
cl %CORE% /arch:AVX2 /c src\ptpng_inflate_bmi2.c /Fo"build\asan\ptpng_inflate_bmi2.obj" || exit /b 1
cl %CORE% /c src\ptpng_inflate_fixed.c /Fo"build\asan\ptpng_inflate_fixed.obj" || exit /b 1
cl %CORE% /c src\ptpng_crc.c     /Fo"build\asan\ptpng_crc.obj"     || exit /b 1
cl %CORE% /c src\ptpng_crc_x86.c /Fo"build\asan\ptpng_crc_x86.obj" || exit /b 1
cl %CORE% /c src\ptpng_filters.c /Fo"build\asan\ptpng_filters.obj" || exit /b 1
cl %CORE% /arch:AVX2 /c src\ptpng_avx2.c /Fo"build\asan\ptpng_avx2.obj" || exit /b 1
cl %CORE% /c src\ptpng_encode.c /Fo"build\asan\ptpng_encode.obj" || exit /b 1
cl %CORE% /c src\ptpng_deflate.c /Fo"build\asan\ptpng_deflate.obj" || exit /b 1
cl %CORE% /arch:AVX2 /c src\ptpng_encode_avx2.c /Fo"build\asan\ptpng_encode_avx2.obj" || exit /b 1
set DECODE_OBJS=build\asan\ptpng.obj build\asan\ptpng_inflate.obj build\asan\ptpng_crc.obj build\asan\ptpng_crc_x86.obj build\asan\ptpng_filters.obj build\asan\ptpng_avx2.obj
set DECODE_OBJS=%DECODE_OBJS% build\asan\ptpng_inflate_fixed.obj build\asan\ptpng_inflate_bmi2.obj
set ENCODE_OBJS=%DECODE_OBJS% build\asan\ptpng_encode.obj build\asan\ptpng_deflate.obj build\asan\ptpng_encode_avx2.obj
cl %CORE% tools\encoder_test.c /link /nologo /out:build\asan\encoder_test.exe %ENCODE_OBJS% || exit /b 1
cl %CORE% tools\encoder_filters_test.c /link /nologo /out:build\asan\encoder_filters_test.exe %ENCODE_OBJS% || exit /b 1
cl %CORE% tools\deflate_test.c /link /nologo /out:build\asan\deflate_test.exe %ENCODE_OBJS% || exit /b 1
cl %CORE% tools\ptpng_tool.c /link /nologo /out:build\asan\ptpng_tool.exe %DECODE_OBJS% || exit /b 1
cl %CORE% tools\fuzz_png.c /link /nologo /out:build\asan\fuzz_png.exe %DECODE_OBJS% || exit /b 1

echo asan build OK
endlocal
