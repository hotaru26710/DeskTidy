@echo off
REM ---------------------------------------------------------------------------
REM verify.bat -- DeskTidy one-shot acceptance: build + unit tests + E2E proof.
REM
REM NOTE: this file is deliberately ASCII-only. cmd.exe reads .bat in the OEM
REM codepage (GBK here), so UTF-8 Chinese comments break the parser.
REM
REM Three stages:
REM   1. Clean build of the main app (must be 0 warnings)
REM   2. Build + run CoreNames unit tests (21 cases)
REM   3. Build + run the E2E probe -- really moves files in an isolated dir,
REM      verifying collect / name-collision avoidance / undo
REM
REM Stage 3 moves real files under F:\QtProject\DeskTidy\_e2e_tmp, which is a
REM deliberately isolated sandbox. The real desktop is never touched.
REM ---------------------------------------------------------------------------

setlocal enabledelayedexpansion

set QT_DIR=C:\Qt\6.11.1\mingw_64
set MINGW_DIR=C:\Qt\Tools\mingw1310_64\bin
set PATH=%QT_DIR%\bin;%MINGW_DIR%;%PATH%

cd /d "%~dp0"

set FAILED=0

REM ---- Kill any running instance; a locked exe makes the link step fail ----
taskkill /IM DeskTidy.exe /F >nul 2>&1

echo ==========================================================
echo  1/14  Build main application
echo ==========================================================
if exist build-qmake rmdir /s /q build-qmake
mkdir build-qmake
cd build-qmake
qmake ..\DeskTidy.pro >nul
if errorlevel 1 (
    echo [ERROR] qmake failed
    set FAILED=1
    goto :summary
)
mingw32-make -j8 > build.log 2>&1
if errorlevel 1 (
    echo [ERROR] build failed, see build-qmake\build.log
    findstr /C:"error" build.log
    set FAILED=1
    goto :summary
)
findstr /C:"warning:" build.log >nul 2>&1
if not errorlevel 1 (
    echo [WARN] compile warnings present:
    findstr /C:"warning:" build.log
) else (
    echo   [OK] 0 warning / 0 error
)
cd ..

echo.
echo ==========================================================
echo  2/14  Unit tests (pure logic)
echo ==========================================================
set TESTOK=1

REM --- 2a: CoreNames pure functions ---
if exist tests\build-corelogic rmdir /s /q tests\build-corelogic
mkdir tests\build-corelogic
cd tests\build-corelogic
qmake ..\tests_corelogic.pro >nul
if errorlevel 1 (
    echo [ERROR] corelogic qmake failed
    set FAILED=1
    goto :summary
)
mingw32-make -j8 > testbuild.log 2>&1
if errorlevel 1 (
    echo [ERROR] corelogic build failed
    findstr /C:"error" testbuild.log
    set FAILED=1
    goto :summary
)
release\tst_corelogic.exe
if errorlevel 1 (set TESTOK=0)
cd ..\..

REM --- 2b: AppService + Settings (added in the floating-box work) ---
if exist tests\build-appservice rmdir /s /q tests\build-appservice
mkdir tests\build-appservice
cd tests\build-appservice
qmake ..\tests_appservice.pro >nul
if errorlevel 1 (
    echo [ERROR] appservice qmake failed
    set FAILED=1
    goto :summary
)
mingw32-make -j8 > testbuild.log 2>&1
if errorlevel 1 (
    echo [ERROR] appservice build failed
    findstr /C:"error" testbuild.log
    set FAILED=1
    goto :summary
)
release\tst_appservice.exe
if errorlevel 1 (set TESTOK=0)
cd ..\..

REM --- 2c: floating-window logic (settings key encoding, manager state machine) ---
if exist tests\build-floatinglogic rmdir /s /q tests\build-floatinglogic
mkdir tests\build-floatinglogic
cd tests\build-floatinglogic
qmake ..\tests_floatinglogic.pro >nul
if errorlevel 1 (
    echo [ERROR] floatinglogic qmake failed
    set FAILED=1
    goto :summary
)
mingw32-make -j8 > testbuild.log 2>&1
if errorlevel 1 (
    echo [ERROR] floatinglogic build failed
    findstr /C:"error" testbuild.log
    set FAILED=1
    goto :summary
)
release\tst_floatinglogic.exe
if errorlevel 1 (set TESTOK=0)
cd ..\..

if "%TESTOK%"=="1" (echo   [OK] unit tests passed) else (set FAILED=1)

echo.
echo ==========================================================
echo  3/14  End-to-end verification (real file moves, isolated)
echo ==========================================================
set E2E=F:\QtProject\DeskTidy\_e2e_tmp
if exist "%E2E%" rmdir /s /q "%E2E%"
mkdir "%E2E%\FakeDesktop"
mkdir "%E2E%\Boxes"
echo alpha>"%E2E%\FakeDesktop\report.docx"
echo beta>"%E2E%\FakeDesktop\data.txt"
echo gamma>"%E2E%\FakeDesktop\notes.md"
mkdir "%E2E%\FakeDesktop\photos"
echo inner>"%E2E%\FakeDesktop\photos\pic.txt"

if exist tools\build-qmake rmdir /s /q tools\build-qmake
mkdir tools\build-qmake
cd tools\build-qmake
qmake ..\e2e_probe.pro >nul
if errorlevel 1 (
    echo [ERROR] probe qmake failed
    set FAILED=1
    goto :summary
)
mingw32-make -j8 > probebuild.log 2>&1
if errorlevel 1 (
    echo [ERROR] probe build failed
    findstr /C:"error" probebuild.log
    set FAILED=1
    goto :summary
)
release\e2e_probe.exe
if errorlevel 1 (set FAILED=1) else (echo   [OK] end-to-end verification passed)
cd ..\..

echo.
echo ==========================================================
echo  4/14  Floating-box signal chain (GUI, no event loop)
echo ==========================================================
REM Verifies the wiring that manual testing finds hardest:
REM after collect/undo, does the floating window refresh ITSELF via signals?
if exist tools\build-floating rmdir /s /q tools\build-floating
mkdir tools\build-floating
cd tools\build-floating
qmake ..\e2e_floating.pro >nul
if errorlevel 1 (
    echo [ERROR] floating probe qmake failed
    set FAILED=1
    goto :summary
)
mingw32-make -j8 > floatbuild.log 2>&1
if errorlevel 1 (
    echo [ERROR] floating probe build failed
    findstr /C:"error" floatbuild.log
    set FAILED=1
    goto :summary
)
release\e2e_floating.exe
if errorlevel 1 (set FAILED=1) else (echo   [OK] floating signal chain passed)
cd ..\..

REM ---------------------------------------------------------------------------
REM 5/8  Fade animation timing (real QPropertyAnimation on a real window)
REM ---------------------------------------------------------------------------
echo.
echo ==========================================================
echo  5/14  Fade animation timing
echo ==========================================================
if exist tools\build-fade rmdir /s /q tools\build-fade
mkdir tools\build-fade
cd tools\build-fade
qmake ..\fade_diag.pro >nul
if errorlevel 1 (
    echo [ERROR] fade diag qmake failed
    set FAILED=1
    goto :summary
)
mingw32-make -j8 > fadebuild.log 2>&1
if errorlevel 1 (
    echo [ERROR] fade diag build failed
    findstr /C:"error" fadebuild.log
    set FAILED=1
    goto :summary
)
REM This one prints a timing curve and always exits 0 (it is a diagnostic,
REM not a pass/fail suite), so check its output for the failure marker instead.
REM The marker is plain ASCII on purpose -- a non-ASCII marker in a findstr
REM pattern gets mangled by the .bat codepage and stops matching.
release\fade_diag.exe > faderun.log 2>&1
findstr /C:"FAIL-MARKER" faderun.log >nul 2>&1
if not errorlevel 1 (
    echo [ERROR] fade animation did not settle at its target value
    findstr /C:"FAIL-MARKER" faderun.log
    set FAILED=1
    goto :summary
)
echo   [OK] fade animation settles correctly
cd ..\..

REM ---------------------------------------------------------------------------
REM 6/9  Delete-box probe (real file moves + recycle bin)
REM ---------------------------------------------------------------------------
echo.
echo ==========================================================
echo  6/14  Delete-box probe (real file moves)
echo ==========================================================
if exist tools\build-del rmdir /s /q tools\build-del
mkdir tools\build-del
cd tools\build-del
qmake ..\delete_probe.pro >nul
if errorlevel 1 (
    echo [ERROR] delete probe qmake failed
    set FAILED=1
    goto :summary
)
mingw32-make -j8 > delbuild.log 2>&1
if errorlevel 1 (
    echo [ERROR] delete probe build failed
    findstr /C:"error" delbuild.log
    set FAILED=1
    goto :summary
)
release\delete_probe.exe
if errorlevel 1 (set FAILED=1) else (echo   [OK] delete-box probe passed)
cd ..\..

REM ---------------------------------------------------------------------------
REM 7/9  View-reset guard probe (regression guard for a real fixed bug)
REM ---------------------------------------------------------------------------
echo.
echo ==========================================================
echo  7/14  View-reset guard (list <-> icon mode switching)
echo ==========================================================
if exist tools\build-viewreset rmdir /s /q tools\build-viewreset
mkdir tools\build-viewreset
cd tools\build-viewreset
qmake ..\viewreset_probe.pro >nul
if errorlevel 1 (
    echo [ERROR] viewreset qmake failed
    set FAILED=1
    goto :summary
)
mingw32-make -j8 > viewbuild.log 2>&1
if errorlevel 1 (
    echo [ERROR] viewreset build failed
    findstr /C:"error" viewbuild.log
    set FAILED=1
    goto :summary
)
release\viewreset_probe.exe
if errorlevel 1 (set FAILED=1) else (echo   [OK] view-reset guard passed)
cd ..\..

REM ---------------------------------------------------------------------------
REM 8/14  Rounded-corner mask verification
REM ---------------------------------------------------------------------------
echo.
echo ==========================================================
echo  8/14  Rounded corners (window mask)
echo ==========================================================
if exist tools\build-cornermask rmdir /s /q tools\build-cornermask
mkdir tools\build-cornermask
cd tools\build-cornermask
qmake ..\corner_mask.pro >nul
if errorlevel 1 (
    echo [ERROR] corner mask qmake failed
    set FAILED=1
    goto :summary
)
mingw32-make -j8 > cornerbuild.log 2>&1
if errorlevel 1 (
    echo [ERROR] corner mask build failed
    findstr /C:"error" cornerbuild.log
    set FAILED=1
    goto :summary
)
release\corner_mask.exe
if errorlevel 1 (set FAILED=1) else (echo   [OK] rounded corners verified)
cd ..\..

REM ---------------------------------------------------------------------------
REM 9/14  Roll-up / expand height animation
REM ---------------------------------------------------------------------------
echo.
echo ==========================================================
echo  9/14  Roll-up height animation
echo ==========================================================
if exist tools\build-rolldiag rmdir /s /q tools\build-rolldiag
mkdir tools\build-rolldiag
cd tools\build-rolldiag
qmake ..\rollup_diag.pro >nul
if errorlevel 1 (
    echo [ERROR] rollup diag qmake failed
    set FAILED=1
    goto :summary
)
mingw32-make -j8 > rollbuild.log 2>&1
if errorlevel 1 (
    echo [ERROR] rollup diag build failed
    findstr /C:"error" rollbuild.log
    set FAILED=1
    goto :summary
)
REM Stale config from an earlier run makes the probe start at the wrong height
REM and then report it as a failure. It uses its own org name, so clearing it
REM cannot touch the user's real DeskTidy settings.
if exist "%LOCALAPPDATA%\DeskTidyRollDiag" rmdir /s /q "%LOCALAPPDATA%\DeskTidyRollDiag"
release\rollup_diag.exe
if errorlevel 1 (set FAILED=1) else (echo   [OK] roll-up animation passed)
cd ..\..

REM ---------------------------------------------------------------------------
REM 10/14  Floating window push-down geometry (WindowLayout)
REM
REM Pure geometry, no widgets and no config: it links only windowlayout.cpp,
REM so it builds and runs in well under a second. It re-checks the same
REM constraints the C-group unit tests cover, but from the other direction --
REM instead of pinning each exact dy, it asserts whole-layout invariants
REM (nothing overlaps, nothing leaves the screen, nobody is pushed upward,
REM the anchor never pushes itself) across several realistic arrangements.
REM ---------------------------------------------------------------------------
echo.
echo ==========================================================
echo  10/14  Floating window push-down geometry
echo ==========================================================
if exist tools\build-windowlayout rmdir /s /q tools\build-windowlayout
mkdir tools\build-windowlayout
cd tools\build-windowlayout
qmake ..\windowlayout_diag.pro >nul
if errorlevel 1 (
    echo [ERROR] windowlayout diag qmake failed
    set FAILED=1
    goto :summary
)
mingw32-make -j8 > layoutbuild.log 2>&1
if errorlevel 1 (
    echo [ERROR] windowlayout diag build failed
    findstr /C:"error" layoutbuild.log
    set FAILED=1
    goto :summary
)
release\windowlayout_diag.exe
if errorlevel 1 (set FAILED=1) else (echo   [OK] push-down geometry passed)
cd ..\..

REM ---------------------------------------------------------------------------
REM 11/14  Hover auto-expand / auto-collapse
REM
REM Drives real QEnterEvent / QEvent::Leave through a real event loop and waits
REM out the real timer delays, which is the only way to observe this feature.
REM The probe owns its config (own org name) and clears rolledUp + geometry per
REM fixture -- without that, one fixture's leftover state silently turns the
REM next fixture's assertions into tautologies. That happened; see its comments.
REM ---------------------------------------------------------------------------
echo.
echo ==========================================================
echo  11/14  Hover auto-expand / auto-collapse
echo ==========================================================
if exist tools\build-hover rmdir /s /q tools\build-hover
mkdir tools\build-hover
cd tools\build-hover
qmake ..\hover_diag.pro >nul
if errorlevel 1 (
    echo [ERROR] hover diag qmake failed
    set FAILED=1
    goto :summary
)
mingw32-make -j8 > hoverbuild.log 2>&1
if errorlevel 1 (
    echo [ERROR] hover diag build failed
    findstr /C:"error" hoverbuild.log
    set FAILED=1
    goto :summary
)
release\hover_diag.exe
if errorlevel 1 (set FAILED=1) else (echo   [OK] hover expand/collapse passed)
cd ..\..

REM ---------------------------------------------------------------------------
REM 12/14  Push-down / slide animation / pinning
REM
REM The geometry itself (WindowLayout) is already covered by 64 unit tests.
REM What this stage checks is the WIRING around it: that the anchor's target
REM rect is the post-expansion one (not the 30px rolled-up strip), that the
REM pushed window actually SLIDES (sampled mid-frames, not just the end value),
REM that it slides back on collapse, and -- the easiest thing to get wrong --
REM that the temporary pushed position never reaches the config file.
REM ---------------------------------------------------------------------------
echo.
echo ==========================================================
echo  12/14  Push-down / slide animation / pinning
echo ==========================================================
if exist tools\build-pushdown rmdir /s /q tools\build-pushdown
mkdir tools\build-pushdown
cd tools\build-pushdown
qmake ..\pushdown_diag.pro >nul
if errorlevel 1 (
    echo [ERROR] pushdown diag qmake failed
    set FAILED=1
    goto :summary
)
mingw32-make -j8 > pushbuild.log 2>&1
if errorlevel 1 (
    echo [ERROR] pushdown diag build failed
    findstr /C:"error" pushbuild.log
    set FAILED=1
    goto :summary
)
release\pushdown_diag.exe
if errorlevel 1 (set FAILED=1) else (echo   [OK] push-down / pinning passed)
cd ..\..

REM ---------------------------------------------------------------------------
REM 13/14  Box item icons (no cross-contamination)
REM
REM Guards the bug the user hit: "every time I add something, the icons in the
REM box get scrambled".  Two causes were found and fixed:
REM   1) the icon cache was keyed by file SUFFIX, so same-suffix files borrowed
REM      each other's icons;
REM   2) QFileIconProvider hands back implicitly-shared QIcons on Windows, so
REM      every item ended up pointing at the FIRST file's icon.
REM Both are silent -- the list still shows *an* icon, just the wrong one -- so
REM this needs a probe that compares icon identity, not just presence.
REM ---------------------------------------------------------------------------
echo.
echo ==========================================================
echo  13/14  Box item icons
echo ==========================================================
if exist tools\build-iconprobe rmdir /s /q tools\build-iconprobe
mkdir tools\build-iconprobe
cd tools\build-iconprobe
qmake ..\icon_probe.pro >nul
if errorlevel 1 (
    echo [ERROR] icon probe qmake failed
    set FAILED=1
    goto :summary
)
mingw32-make -j8 > iconbuild.log 2>&1
if errorlevel 1 (
    echo [ERROR] icon probe build failed
    findstr /C:"error" iconbuild.log
    set FAILED=1
    goto :summary
)
release\icon_probe.exe
if errorlevel 1 (set FAILED=1) else (echo   [OK] item icons passed)
cd ..\..

REM ---------------------------------------------------------------------------
REM Cleanup: the deleteBox unit tests and the delete probe really move box
REM directories into the system recycle bin, and QTemporaryDir cannot
REM reclaim them once they are gone. Purge those leftovers so repeated runs
REM do not pile up junk in the user's bin.
REM
REM NOTE: this used to be an inline multi-line powershell -Command with caret
REM continuations. That form broke cmd's parser: the command contained
REM parentheses, which cmd treats as block delimiters, so it started
REM executing subsequent lines as commands -- the REM '-----' rules were
REM parsed as command names and it spun in an error loop. Hence the separate
REM .ps1 file: cmd now sees one simple command and parses nothing else.
REM ---------------------------------------------------------------------------
echo.
echo  Cleaning up recycle-bin leftovers from tests...
powershell -NoProfile -ExecutionPolicy Bypass -File "tools\cleanup-test-residue.ps1"

:summary
echo.
echo ==========================================================
if "%FAILED%"=="1" (
    echo  RESULT: FAILED -- see output above
    exit /b 1
) else (
    echo  RESULT: ALL PASSED
    echo  Artifact: build-qmake\release\DeskTidy.exe
    exit /b 0
)
