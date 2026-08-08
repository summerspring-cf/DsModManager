# -*- coding: utf-8 -*-
"""
DsCppModManager 배포 패키지 생성기.

산출물: Plugins_Publish/DsCppModManager_<버전>.zip   (배포 규약: 저장소 루트 PACKAGING.md)
  DsCppModManager/            <- 게임 ue4ss/Mods/ 에 그대로 풀면 되는 폴더
    dlls/main.dll
    Assets/scrim_gradient.png
    plugins/                  <- 모드 투입 폴더 (빈 채로 포함)
    enabled.txt               <- 존재만으로 UE4SS 가 모드를 시작 (mods.txt 편집 불필요,
                                 이미 시작된 모드는 건너뛰므로 중복 시작 없음 -- UE4SSProgram.cpp:1191)
    README.txt                <- 설치/제거 안내 (한국어)
  UE4SS/                      <- 동봉한 UE4SS 원본 (vendor/ue4ss/ 의 공식 zip 그대로)
    dwmapi.dll                   install.bat 이 게임 Binaries\\Win64\\ 에 풀어 준다.
    ue4ss/UE4SS.dll              ⚠ **없는 파일만** 채운다 -- 이미 UE4SS 를 쓰는
    ue4ss/UE4SS-settings.ini        사용자의 파일은 하나도 덮어쓰지 않는다.
    ue4ss/LICENSE                (MIT 재배포 조건 = 이 전문 동봉)
    ue4ss/Mods/...               UE4SS 기본 모드 + 스톡 mods.txt
  install.bat                 <- 자동 설치 스크립트 (ASCII 전용 -- cp949 함정)

⚠ enabled.txt 를 담는 것은 **매니저 본체뿐**이다 (PACKAGING.md 4항의 유일한 예외).
   매니저는 켜 주는 주체라, 꺼진 채로 배포하면 사용자가 mods.txt 를 손으로 고쳐야 한다.
   플러그인 zip 에는 enabled.txt / dsruntime.txt / dsoptions.txt 를 절대 넣지 않는다.

⚠ zip 안의 `UE4SS/ue4ss/Mods/mods.txt` 는 PACKAGING.md 4항의 "mods.txt 금지" 와
   충돌하지 않는다. 그 규칙의 취지는 "사용자의 모드 목록을 덮어쓰지 마라" 이고,
   이 파일은 **UE4SS 자체가 없을 때만** 생성된다(install.bat 이 항목마다
   `if not exist` 로 막는다). 덮어쓸 사용자 파일이 존재하지 않는 경우뿐이다.

사용: python tools/package_cppmm.py
"""
import hashlib
import io
import os
import re
import sys
import zipfile

import dspublish  # 배포본 폴더 규약 (같은 tools 폴더)

sys.stdout.reconfigure(encoding="utf-8")

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MOD = os.path.join(ROOT, "mods", "DsCppModManager")
OUT_DIR = dspublish.PUBLISH_DIR   # PACKAGING.md 1항 (Plugins_Publish)

# 동봉할 UE4SS 원본. 손대지 않은 공식 experimental zip 이다 (vendor/ue4ss/README.md).
UE4SS_ZIP = os.path.join(ROOT, "vendor", "ue4ss", "UE4SS_experimental.zip")


def read_version():
    """버전 정본은 소스의 ModVersion 이다 (스크립트 상수가 아니라).
    손으로 적어 둔 상수는 반드시 코드와 어긋난다 -- STATUS 2026-08-05 DsAutoFood 절."""
    with open(os.path.join(MOD, "main.cpp"), encoding="utf-8") as f:
        m = re.search(r'ModVersion\s*=\s*L"([^"]+)"', f.read())
    if not m:
        sys.exit("[ERROR] ModVersion 을 main.cpp 에서 못 찾았다")
    return m.group(1)


VERSION = read_version()

# 이 패키지가 빌드/검증된 UE4SS (실험 채널, 커밋 c838a8ac). ABI 전사가 이 DLL 의
# 익스포트에 결합되어 있어, 다른 UE4SS 빌드에서는 DLL 로드가 거부될 수 있다.
UE4SS_SIZE = 16519168
UE4SS_SHA256 = "D0107F63E567313CB6A15C505B5DB2BDBA38130964A04E019BDA7611C6178022"
# 동봉본이 정말 그 빌드인지 압축 전에 대조한다. 엉뚱한 UE4SS 를 실어 보내면
# 사용자 화면에서는 "모드가 안 뜬다" 로만 나타나 원인 추적이 사실상 불가능하다.
UE4SS_DLL_ENTRY = "ue4ss/UE4SS.dll"


def read_ue4ss_payload():
    """공식 UE4SS zip 에서 담을 항목을 (배포본 경로, 바이트) 목록으로 읽는다.

    zip 을 풀지 않고 그대로 읽는 이유: 중간에 사람 손이 닿을 자리를 만들지
    않으려는 것이다. 동봉본은 공식 배포본과 바이트 단위로 같아야 한다."""
    if not os.path.exists(UE4SS_ZIP):
        sys.exit(f"[ERROR] UE4SS 동봉본이 없다: {UE4SS_ZIP}\n"
                 f"        vendor/ue4ss/README.md 참고 (실험 채널 c838a8ac).")
    items = []
    with zipfile.ZipFile(UE4SS_ZIP) as z:
        for info in z.infolist():
            if info.is_dir():
                continue
            items.append((f"UE4SS/{info.filename}", z.read(info.filename)))
    payload = dict(items)
    dll = payload.get(f"UE4SS/{UE4SS_DLL_ENTRY}")
    if dll is None:
        sys.exit(f"[ERROR] 동봉본에 {UE4SS_DLL_ENTRY} 이 없다: {UE4SS_ZIP}")
    got = hashlib.sha256(dll).hexdigest().upper()
    if len(dll) != UE4SS_SIZE or got != UE4SS_SHA256:
        sys.exit(f"[ERROR] 동봉할 UE4SS.dll 이 검증된 빌드가 아니다.\n"
                 f"        기대: {UE4SS_SIZE:,} B / {UE4SS_SHA256}\n"
                 f"        실제: {len(dll):,} B / {got}")
    return items

README = f"""DsCppModManager v{VERSION} — DragonSword: Awakening 용 C++ 모드매니저
=====================================================================

■ 전제 조건
  UE4SS 를 별도 설치/실행 한 적 없다면 이 모드에 함께 동봉되어 있으니,
  UE4SS 를 찾아 추가 설치/실행을 하지 않으셔도 됩니다.
  버전 업그레이드를 위해 install.bat 으로 업데이트할 때, UE4SS 가 이미
  설치되어 있으면 UE4SS 설치 과정은 건너뜁니다.

  동봉된 UE4SS: 실험 채널 빌드 커밋 c838a8ac
                UE4SS.dll {UE4SS_SIZE:,} 바이트
                SHA256 {UE4SS_SHA256}
  이 모드는 바로 그 빌드에서 빌드/검증되었습니다. 다른 버전의 UE4SS 위에서는
  모드 DLL 로드가 거부될 수 있습니다(게임은 정상 실행되고 모드만 꺼집니다).
  UE4SS 는 MIT 라이선스로 재배포됩니다 — 전문은 ue4ss\\LICENSE 에 있습니다.

  ※ 이미 다른 UE4SS 를 쓰고 계신 경우
    install.bat 은 기존 UE4SS 를 절대 덮어쓰지 않습니다. 설치는 되지만
    모드매니저가 뜨지 않는다면 UE4SS 버전이 달라서일 수 있습니다. 그때는
    ue4ss 폴더를 통째로 지우고 install.bat 을 다시 실행하세요.
    (다른 모드를 쓰고 계신다면 Mods 폴더를 먼저 백업하세요)

■ 설치 방법 1 — 자동 (권장)
  1) ★ 게임을 완전히 종료합니다. (실행 중이면 모드 파일이 잠겨 설치가 실패합니다)
  2) install.bat 을 더블클릭합니다.
     (기본 스팀 경로가 아니면: install.bat 아이콘 위로 게임 폴더를 드래그)
  3) 설치 완료 팝업이 나오면 끝. 게임을 실행하면 타이틀 메뉴에
     "모드매니저" 항목이 나타납니다.
  ※ Access denied 가 나오면 install.bat 을 우클릭 → 관리자 권한으로 실행.

■ 이전 버전에서 업데이트하기
  같은 방법으로 install.bat 을 실행하면 됩니다. 덮어쓰기 방식이라
  아래는 그대로 보존됩니다:
    - plugins 폴더에 넣어 둔 모드들
    - 각 모드의 설정값(dsoptions.txt)과 켬/끔 상태
    - 순서 탭에서 정한 모드 순서(dsorder.txt)
  설치 후 게임에서 [모드매니저 → 기본 → 모드 버전] 값이 이 배포판의 버전과
  같은지 확인하세요. 다르면 게임이 켜진 채로 설치해서 덮어쓰기가 실패한 것입니다.

■ 설치 방법 2 — 수동
  1) UE4SS 가 아직 없다면: UE4SS 폴더 **안의 내용물**(dwmapi.dll 과 ue4ss 폴더)을
     아래 위치에 복사합니다.
       <게임>\\DS\\Binaries\\Win64\\
     이미 UE4SS 를 쓰고 있다면 이 단계는 건너뜁니다(덮어쓰지 마세요).
  2) DsCppModManager 폴더를 통째로 아래 위치에 복사합니다:
       <게임>\\DS\\Binaries\\Win64\\ue4ss\\Mods\\DsCppModManager
  mods.txt 는 편집할 필요가 없습니다. 폴더 안의 enabled.txt 가 있으면
  UE4SS 가 자동으로 모드를 시작합니다.

■ 비활성화 / 제거
  - 잠시 끄기: Mods\\DsCppModManager\\enabled.txt 를 삭제
    (mods.txt 에 "DsCppModManager : 1" 항목이 있다면 그것도 0 으로)
  - 완전 제거: Mods\\DsCppModManager 폴더를 삭제

■ 플러그인(모드) 넣는 곳
  Mods\\DsCppModManager\\plugins\\<모드이름>\\
    ├─ Scripts\\main.lua   (Lua 모드)  또는
    └─ dlls\\main.dll      (C++ 모드)
  게임 안에서 [모드매니저 → 플러그인 → 폴더 바로가기] 로 열 수 있고,
  넣은 모드는 패널을 다시 열면 "모드선택" 에 표시됩니다.

■ 문서
  MANUAL.md       — 사용 설명서 (화면 구성 · 조작 · 파일 위치 · 문제 해결)
  PLUGIN_GUIDE.md — 모드 제작자용 연동 가이드 (dsplugin.ini 계약)
"""

INSTALL_BAT = r"""@echo off
rem ============================================================
rem  DsCppModManager __VERSION__ installer  (UE4SS 동봉판)
rem  * cp949(ANSI) + CRLF 로 저장된다 -- 한국어 Windows 콘솔/WSH 기준.
rem  * 결과는 항상 팝업으로 알린다. 콘솔이 순식간에 닫혀도 사용자가 볼 수 있게.
rem  * 실패 원인 1순위: zip 을 풀지 않고 zip 안에서 install.bat 을 더블클릭한 경우.
rem    (Windows 가 그 파일 하나만 임시폴더에 풀어 실행 -> 복사할 원본이 없다)
rem  * UE4SS 는 "없으면 넣고, 있으면 건드리지 않는다".
rem    복사는 전부 'if not exist' 로 한 항목씩 막는다. 남의 UE4SS 설정과
rem    모드 목록을 덮어쓰지 않는 것이 이 스크립트의 제1원칙이다.
rem ============================================================
setlocal EnableExtensions EnableDelayedExpansion
set "VER=__VERSION__"
set "SRC=%~dp0DsCppModManager"
set "UESRC=%~dp0UE4SS"
set "UESIZE=__UE4SS_SIZE__"
set "EXE=DSClient-Win64-Shipping.exe"
set "VBS=%TEMP%\dsmm_install_msg.vbs"

rem ---------- 0) 압축을 제대로 풀었는지 ----------
if not exist "%SRC%\dlls\main.dll" goto :no_src
if not exist "%UESRC%\ue4ss\UE4SS.dll" goto :no_src

rem ---------- 1) 게임 폴더 찾기 ----------
rem 기준은 UE4SS 가 아니라 **게임 실행 파일**이다. UE4SS.dll 을 기준으로 삼으면
rem UE4SS 미설치자에게 "게임을 못 찾았다" 는 엉뚱한 안내가 나가고, 시키는 대로
rem 게임 폴더를 드래그해도 같은 팝업이 반복된다 (실제 문의 사례).
set "GAME=%~1"
if "%GAME%"=="" set "GAME=C:\Program Files (x86)\Steam\steamapps\common\DragonSword  Awakening"
rem 드래그해 넣은 폴더가 게임 루트/Binaries/Win64/ue4ss 중 어디든 받아 준다.
rem ('if cond set ... & goto' 형태는 goto 가 무조건 실행될 위험이 있어 쓰지 않는다)
set "W64=%GAME%\DS\Binaries\Win64"
if exist "%W64%\%EXE%" goto :found
set "W64=%GAME%\Binaries\Win64"
if exist "%W64%\%EXE%" goto :found
set "W64=%GAME%\Win64"
if exist "%W64%\%EXE%" goto :found
set "W64=%GAME%"
if exist "%W64%\%EXE%" goto :found
rem ue4ss 폴더 자체를 끌어다 준 경우 -- 한 단계 위가 Win64 다.
for %%I in ("%GAME%\..") do set "W64=%%~fI"
if exist "%W64%\%EXE%" goto :found

rem 폴더를 끌어다 준 경우(인자 있음)에는 그 경로만 본다. 인자가 없으면
rem 다른 드라이브의 스팀 라이브러리까지 훑는다 -- 게임이 D:\ 등에 깔린 사람이
rem "설치가 안 된다" 고 하는 가장 흔한 이유다.
if not "%~1"=="" goto :no_game
set "FOUND="
for %%D in (C D E F G H I J K L) do call :probe "%%D:\SteamLibrary\steamapps\common\DragonSword  Awakening"
for %%D in (C D E F G H I J K L) do call :probe "%%D:\SteamLibrary\steamapps\common\DragonSword Awakening"
for %%D in (C D E F G H I J K L) do call :probe "%%D:\Steam\steamapps\common\DragonSword  Awakening"
for %%D in (C D E F G H I J K L) do call :probe "%%D:\Games\Steam\steamapps\common\DragonSword  Awakening"
for %%D in (C D E F G H I J K L) do call :probe "%%D:\Program Files (x86)\Steam\steamapps\common\DragonSword  Awakening"
if not defined FOUND goto :no_game
set "GAME=%FOUND%"
set "W64=%GAME%\DS\Binaries\Win64"
goto :found

:probe
if defined FOUND exit /b
if exist "%~1\DS\Binaries\Win64\DSClient-Win64-Shipping.exe" set "FOUND=%~1"
exit /b

:found
set "UE=%W64%\ue4ss"
set "DEST=%UE%\Mods\DsCppModManager"

rem ---------- 2) UE4SS: 없으면 넣고, 있으면 그대로 둔다 ----------
rem UE4SS 가 이미 있으면 **아무것도 하지 않는다**. 덮어쓰기만 피하는 정도로는
rem 부족하다 -- 없는 파일만 채우는 방식으로 짜 봤더니, 기본 모드를 지우고 쓰던
rem 사용자의 Mods 폴더에 모드 9개를 도로 밀어 넣었다(테스트 CASE B 실측).
rem 남의 UE4SS 는 손대지 않는 것이 맞다.
set "UEHAD=0"
if exist "%UE%\UE4SS.dll" set "UEHAD=1"
if "%UEHAD%"=="1" goto :ue_kept

rem 여기부터는 UE4SS 가 아예 없는 경우다. 그래도 한 항목씩 'if not exist' 로
rem 막는다 -- 설치가 반쯤 깨진 상태(폴더는 있고 DLL 은 없는)에서도 안전하게.
if not exist "%UE%\Mods" mkdir "%UE%\Mods" >nul 2>&1
if not exist "%W64%\dwmapi.dll" copy /y "%UESRC%\dwmapi.dll" "%W64%\" >nul 2>&1
if not exist "%UE%\UE4SS.dll" copy /y "%UESRC%\ue4ss\UE4SS.dll" "%UE%\" >nul 2>&1
if not exist "%UE%\UE4SS-settings.ini" copy /y "%UESRC%\ue4ss\UE4SS-settings.ini" "%UE%\" >nul 2>&1
if not exist "%UE%\LICENSE" copy /y "%UESRC%\ue4ss\LICENSE" "%UE%\" >nul 2>&1
if not exist "%UE%\Mods\mods.txt" copy /y "%UESRC%\ue4ss\Mods\mods.txt" "%UE%\Mods\" >nul 2>&1
if not exist "%UE%\Mods\mods.json" copy /y "%UESRC%\ue4ss\Mods\mods.json" "%UE%\Mods\" >nul 2>&1
for %%M in (BPML_GenericFunctions BPModLoaderMod CheatManagerEnablerMod ConsoleCommandsMod ConsoleEnablerMod Keybinds LineTraceMod SplitScreenMod shared) do if not exist "%UE%\Mods\%%M" xcopy /e /i /q /y "%UESRC%\ue4ss\Mods\%%M" "%UE%\Mods\%%M" >nul 2>&1

if not exist "%UE%\UE4SS.dll" goto :ue_failed
set "UESZ=0"
for %%A in ("%UE%\UE4SS.dll") do set "UESZ=%%~zA"
if not "!UESZ!"=="%UESIZE%" goto :ue_failed
set "UEMSG=UE4SS : 함께 설치했습니다 (동봉본)"
goto :ue_done

:ue_kept
rem 크기가 다르면 다른 빌드다. 그래도 덮어쓰지 않는다 -- 다른 모드가 그 위에서
rem 돌고 있을 수 있다. 대신 모드가 안 뜰 때 볼 안내를 팝업에 붙인다.
set "UESZ=0"
for %%A in ("%UE%\UE4SS.dll") do set "UESZ=%%~zA"
set "UEMSG=UE4SS : 이미 있어 그대로 두었습니다"
if not "!UESZ!"=="%UESIZE%" set "UEMSG=UE4SS : 다른 버전이 이미 있어 그대로 두었습니다. 모드매니저가 안 보이면 README 의 'UE4SS 버전' 항목을 참고하세요"

:ue_done

rem ---------- 3) 기존 설치 버전 ----------
set "OLDVER="
if exist "%DEST%\version.txt" set /p OLDVER=<"%DEST%\version.txt"
if not defined OLDVER if exist "%DEST%\dlls\main.dll" set "OLDVER=0.20 이하"

rem ---------- 4) 매니저 복사 ----------
echo 설치 위치: %DEST%
set "SRCSZ=0"
for %%A in ("%SRC%\dlls\main.dll") do set "SRCSZ=%%~zA"
xcopy /e /i /y "%SRC%" "%DEST%" >nul 2>&1
if errorlevel 1 goto :failed
if not exist "%DEST%\plugins" mkdir "%DEST%\plugins" >nul 2>&1

rem 복사가 조용히 건너뛰어졌는지 검증 (게임 실행 중이면 DLL 이 잠긴다)
set "DSTSZ=0"
for %%A in ("%DEST%\dlls\main.dll") do set "DSTSZ=%%~zA"
if not "%SRCSZ%"=="%DSTSZ%" goto :locked

rem 여기까지 왔으면 DLL 이 실제로 교체됐다. 그때만 버전 도장을 찍는다.
> "%DEST%\version.txt" echo %VER%

if defined OLDVER goto :ok_update
set "MSG=모드매니저 설치 완료[br][br]버전 : %VER%[br]위치 : %DEST%[br]!UEMSG![br][br]게임을 실행하면 타이틀 메뉴에 '모드매니저' 항목이 생깁니다."
set "ICON=64"
goto :say

:ok_update
set "MSG=모드매니저 업데이트 완료[br][br]버전 : %OLDVER%  ->  %VER%[br]위치 : %DEST%[br]!UEMSG![br][br]기존 플러그인/설정/모드 순서는 그대로 유지됩니다."
set "ICON=64"
goto :say

:no_src
set "MSG=설치 파일을 찾지 못했습니다.[br][br]zip 을 '압축 풀기' 한 다음,[br]풀린 폴더 안의 install.bat 을 실행해 주세요.[br][br]zip 을 열어서 그 안의 install.bat 을 바로 더블클릭하면[br]설치가 되지 않습니다."
set "ICON=16"
goto :say

:no_game
set "MSG=게임을 찾지 못했습니다.[br][br]찾아본 경로 : %GAME%[br][br]게임이 다른 드라이브에 있다면,[br]게임 폴더를 install.bat 아이콘 위로 끌어다 놓으세요.[br](DragonSword  Awakening 폴더)[br][br]찾는 기준은 DSClient-Win64-Shipping.exe 입니다.[br]UE4SS 는 없어도 됩니다. 이 설치본에 들어 있습니다."
set "ICON=16"
goto :say

:ue_failed
set "MSG=UE4SS 설치에 실패했습니다.[br][br]위치 : %UE%[br][br]1) 게임이 실행 중이면 완전히 종료 후 다시 시도[br]2) 그래도 안 되면 install.bat 우클릭 -[br]   '관리자 권한으로 실행'[br][br]모드매니저는 UE4SS 없이는 동작하지 않습니다."
set "ICON=16"
goto :say

:locked
set "MSG=업데이트가 적용되지 않았습니다. (이전 버전 그대로)[br][br]위치 : %DEST%[br][br]게임이 실행 중이면 모드 파일이 잠깁니다.[br]게임을 완전히 종료한 뒤 다시 실행해 주세요."
set "ICON=16"
goto :say

:failed
set "MSG=설치에 실패했습니다.[br][br]위치 : %DEST%[br][br]1) 게임이 실행 중이면 완전히 종료 후 다시 시도[br]2) 그래도 안 되면 install.bat 우클릭 -[br]   '관리자 권한으로 실행'"
set "ICON=16"
goto :say

rem ---------- 팝업 (WSH 가 막혀 있으면 콘솔 pause 로 폴백) ----------
:say
rem [주의] 반드시 !MSG! (지연 확장). %MSG% 로 쓰면 값 안의 '->' 의 '>' 가
rem        리다이렉션으로 해석돼 메시지가 파일로 새고 흐름이 깨진다(실측 사고).
echo.
echo !MSG:[br]= !
echo.
> "%VBS%" echo MsgBox Replace("!MSG!","[br]",vbCrLf), !ICON!, "DsCppModManager !VER!"
wscript "%VBS%" || pause
del "%VBS%" >nul 2>&1
if "!ICON!"=="16" exit /b 1
exit /b 0
""".replace("__VERSION__", VERSION).replace("__UE4SS_SIZE__", str(UE4SS_SIZE))


def check_clean(zip_path):
    """배포본에 사용자 상태가 섞여 들어가지 않았는지 확인한다 (PACKAGING.md 3·4항).

    ★ plugins 폴더는 **반드시 비어 있어야 한다.** 모드가 한 개라도 딸려 가면
      사용자가 고르지 않은 모드가 목록에 뜨고, 매니저가 관문 구실을 못 한다.
      개발 PC 의 plugins 에는 늘 시험용 모드가 들어 있어서, 언젠가 그걸 그대로
      담는 실수가 나온다 -- 그래서 문서가 아니라 검사로 막는다."""
    forbidden_names = ("dsruntime.txt", "dsoptions.txt", "dsorder.txt", "version.txt")
    bad = []
    with zipfile.ZipFile(zip_path) as z:
        for name in z.namelist():
            if name.startswith("DsCppModManager/plugins/") and name.rstrip("/") != "DsCppModManager/plugins":
                bad.append(f"plugins 안에 내용물: {name}")
            if os.path.basename(name).lower() in forbidden_names:
                bad.append(f"금지 파일: {name}")
    if bad:
        os.remove(zip_path)   # 잘못 만든 배포본은 남기지 않는다
        sys.exit("[ERROR] 배포 규약 위반 -- zip 을 지웠다:\n  " + "\n  ".join(bad))


def main():
    dll = os.path.join(MOD, "main.dll")
    png = os.path.join(MOD, "Assets", "scrim_gradient.png")
    for p in (dll, png):
        if not os.path.exists(p):
            print(f"[ERROR] 없음: {p}")
            sys.exit(1)
    ue4ss_payload = read_ue4ss_payload()   # 해시 대조 포함. 어긋나면 여기서 중단된다.
    os.makedirs(OUT_DIR, exist_ok=True)
    out = os.path.join(OUT_DIR, f"DsCppModManager_{VERSION}.zip")
    # 이전 버전 배포본은 Old_Plugins_Publish/<모드>/ 로 옮긴다 (PACKAGING.md 1항).
    # Plugins_Publish 에는 모드당 최신 하나만 남는다.
    dspublish.archive_previous(os.path.basename(out).split("_")[0], out)
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        z.write(dll, "DsCppModManager/dlls/main.dll")
        z.write(png, "DsCppModManager/Assets/scrim_gradient.png")
        # Assets 나머지 (토글 텍스처 등)
        assets = os.path.join(MOD, "Assets")
        for fn in sorted(os.listdir(assets)):
            if fn.lower().endswith(".png") and fn != "scrim_gradient.png":
                z.write(os.path.join(assets, fn), f"DsCppModManager/Assets/{fn}")
        # v0.10: 플러그인 제작자용 가이드 + 예제 + Lua 헬퍼
        z.write(os.path.join(MOD, "MANUAL.md"), "DsCppModManager/MANUAL.md")
        z.write(os.path.join(MOD, "PLUGIN_GUIDE.md"), "DsCppModManager/PLUGIN_GUIDE.md")
        z.write(os.path.join(MOD, "dsplugin.ini.example"), "DsCppModManager/dsplugin.ini.example")
        z.write(os.path.join(MOD, "dsmm_options.lua"), "DsCppModManager/dsmm_options.lua")
        z.writestr("DsCppModManager/plugins/", "")
        z.writestr("DsCppModManager/enabled.txt", "")
        # 동봉 UE4SS (공식 experimental zip 그대로). install.bat 이 없는 파일만 채운다.
        for name, data in ue4ss_payload:
            z.writestr(name, data)
        z.writestr("DsCppModManager/README.txt", "\ufeff" + README)  # 메모장용 BOM
        # ⚠ version.txt 는 zip 에 넣지 않는다. install.bat 이 **DLL 교체를 검증한 뒤**
        #    직접 쓴다 -- xcopy 로 딸려 보내면 DLL 이 잠겨 실패해도 버전만 새것으로
        #    남아 "0.21 인데 실제로는 0.17" 이라는 거짓 상태가 된다(실측 사고).
        # ⚠ 배치 파일은 **CRLF + cp949(ANSI)** 로 써야 한국어 Windows 의 콘솔과
        #   WSH(팝업)가 한글을 제대로 읽는다. UTF-8 로 쓰면 전부 깨진다.
        bat = INSTALL_BAT.replace("\r\n", "\n").replace("\n", "\r\n")
        try:
            bat_bytes = bat.encode("cp949")
        except UnicodeEncodeError as e:
            # cp949 에 없는 글자(⚠ ★ → 등)를 넣으면 사용자 화면에서 깨진다.
            # 조용히 대체하지 말고 배포를 멈춘다 -- PACKAGING.md 6항 정신.
            sys.exit(f"[ERROR] install.bat 에 cp949 로 못 쓰는 문자가 있다: {e}")
        z.writestr("install.bat", bat_bytes)
    check_clean(out)
    print(f"패키지 생성: {out} ({os.path.getsize(out):,} bytes)")
    # PACKAGING.md 6항 ⑤: 담긴 목록 출력 -- 금지 파일이 섞였는지 눈으로 보는 마지막 관문
    with zipfile.ZipFile(out) as z:
        for info in z.infolist():
            print(f"  {info.filename:<44} {info.file_size:7,d} B")


if __name__ == "__main__":
    main()
