@echo off
REM Build both programs on Windows with g++ (MSYS2 / MinGW-w64).
REM Run this from the project folder:   build.bat

if not exist bin mkdir bin

echo Building bserve.exe ...
g++ -std=c++17 -O2 -Wall -Wextra -Isrc -o bin\bserve.exe src\bserve.cpp src\frame.cpp -lws2_32
if errorlevel 1 goto failed

echo Building bcurl.exe ...
g++ -std=c++17 -O2 -Wall -Wextra -Isrc -o bin\bcurl.exe src\bcurl.cpp src\frame.cpp -lws2_32
if errorlevel 1 goto failed

echo.
echo Done. The programs are in the bin folder:
echo    bin\bserve.exe www 9000
echo    bin\bcurl.exe -v localhost:9000/index.html
goto :eof

:failed
echo.
echo BUILD FAILED. Check that g++ is on your PATH ^(try: g++ --version^).
exit /b 1
