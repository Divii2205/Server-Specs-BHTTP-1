@echo off
REM run_demo.bat - runs every test case against a server you started
REM yourself in another window:   bin\bserve.exe www 9000
REM
REM Each test prints what it is checking and the exit code it got.

setlocal
set BC=bin\bcurl.exe
set HOST=localhost:9000

echo ============================================================
echo  TEST 1  fetch a page that exists   (expect exit 0)
echo ============================================================
%BC% %HOST%/index.html
echo [exit code: %errorlevel%]
echo.

echo ============================================================
echo  TEST 2  fetch a page that does NOT exist   (expect exit 4)
echo ============================================================
%BC% %HOST%/missing.html
echo [exit code: %errorlevel%]
echo.

echo ============================================================
echo  TEST 3  three files over ONE connection   (expect exit 0)
echo ============================================================
%BC% %HOST%/index.html /about.html /notes.txt
echo [exit code: %errorlevel%]
echo.

echo ============================================================
echo  TEST 4  unknown frame type is skipped cleanly  (expect exit 0)
echo ============================================================
%BC% --send-unknown %HOST%/notes.txt
echo [exit code: %errorlevel%]
echo.

echo ============================================================
echo  TEST 5  malformed frame   (expect 400, exit 4)
echo ============================================================
%BC% --malformed %HOST%/index.html
echo [exit code: %errorlevel%]
echo.

echo ============================================================
echo  TEST 6  path that tries to escape the root  (expect 400, exit 4)
echo ============================================================
%BC% "%HOST%/../secret.txt"
echo [exit code: %errorlevel%]
echo.

echo All tests finished.
endlocal
