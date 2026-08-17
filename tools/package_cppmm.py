# -*- coding: utf-8 -*-
"""
DsCppModManager 배포 패키지 생성기.

산출물 (Plugins_Publish/ — 배포 규약: 저장소 루트 PACKAGING.md):
  ① DsCppModManager_<버전>.zip           <- UE4SS **동봉** (한글패치스튜디오/일반 배포)
  ② DsCppModManager-NoUE4SS_<버전>.zip    <- UE4SS **미동봉** (Nexus Mods 용)

동봉판 구조:
  DsCppModManager/            <- 게임 ue4ss/Mods/ 에 그대로 풀면 되는 폴더
    dlls/main.dll
    Assets/*.png
    plugins/                  <- 모드 투입 폴더 (빈 채로 포함)
    enabled.txt               <- 존재만으로 UE4SS 가 모드를 시작 (mods.txt 편집 불필요)
    README.txt                <- 설치/제거 안내 (한국어)
  UE4SS/                      <- 동봉한 UE4SS 원본 (vendor/ue4ss/ 의 공식 zip 그대로)
    dwmapi.dll                   install.bat 이 게임 Binaries\\Win64\\ 에 풀어 준다.
    ue4ss/...                    ⚠ **없는 파일만** 채운다.
  install.bat                 <- 자동 설치 (UE4SS 없으면 넣고, 있으면 건드리지 않음)

미동봉판 구조 (Nexus): 위에서 UE4SS/ 폴더만 빠지고, install.bat 은 UE4SS 가
  이미 설치돼 있어야 진행한다(없으면 UE4SS 를 먼저 설치하라고 안내). 매니저만 복사.

⚠ enabled.txt 를 담는 것은 **매니저 본체뿐**이다 (PACKAGING.md 4항의 유일한 예외).
⚠ zip 안의 UE4SS/ue4ss/Mods/mods.txt 는 install.bat 이 **UE4SS 자체가 없을 때만**
   생성하므로 "사용자 모드 목록 덮어쓰기 금지" 와 충돌하지 않는다.

사용: python tools/package_cppmm.py
"""
import hashlib
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
    """버전 정본은 소스의 ModVersion 이다 (스크립트 상수가 아니라)."""
    with open(os.path.join(MOD, "main.cpp"), encoding="utf-8") as f:
        m = re.search(r'ModVersion\s*=\s*L"([^"]+)"', f.read())
    if not m:
        sys.exit("[ERROR] ModVersion 을 main.cpp 에서 못 찾았다")
    return m.group(1)


VERSION = read_version()

# 이 패키지가 빌드/검증된 UE4SS (실험 채널, 커밋 c838a8ac).
UE4SS_SIZE = 16519168
UE4SS_SHA256 = "D0107F63E567313CB6A15C505B5DB2BDBA38130964A04E019BDA7611C6178022"
UE4SS_DLL_ENTRY = "ue4ss/UE4SS.dll"


def read_ue4ss_payload():
    """공식 UE4SS zip 에서 담을 항목을 (배포본 경로, 바이트) 목록으로 읽는다."""
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


# ------------------------------------------------------------------ README

README_BUNDLED = f"""DsCppModManager v{VERSION} — DragonSword: Awakening 용 C++ 모드매니저
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

■ 이 모드가 남기는 기록 (전부 게임 폴더 안에만, 밖으로 보내지 않습니다)
  Mods\\DsCppModManager\\dlls\\cppmm_log.txt   — 동작 기록
  Mods\\DsCppModManager\\blackbox*.txt         — 크래시가 어느 단계에서 났는지
  Mods\\DsCppModManager\\inputlog\\날짜.log     — 입력 진단 기록 (아래)

  입력 진단 기록은 "컨트롤러를 인식 못 한다" 같은 문제의 원인을 가리기 위한
  것으로, **타이틀 화면에 있을 때만** 남깁니다 (연결된 컨트롤러의 종류와 이름,
  누른 패드 버튼과 스틱 값). 게임 플레이 중에는 남기지 않고, 인터넷으로
  전송하지 않습니다.

  키보드는 **어떤 키를 눌렀는지 남기지 않습니다** -- "1초 동안 키 입력 N회"
  라는 횟수만 적습니다 (컨트롤러가 키보드로 전달되는지 판별하는 용도).
  ※ 타이틀에서 스팀 오버레이(Shift+Tab)를 열어 타자를 치면 그 횟수도 함께
    집계됩니다. 무엇을 쳤는지는 기록되지 않습니다.

  전부 남기지 않으려면 Mods 폴더의 DsCppModManager 안에 inputlog_off.txt 를
  만드세요 (다음 게임 실행부터 적용).

■ 문서
  MANUAL.md       — 사용 설명서 (화면 구성 · 조작 · 파일 위치 · 문제 해결)
  CHANGELOG.md    — 버전별 변경 내역
  PLUGIN_GUIDE.md — 모드 제작자용 연동 가이드 (dsplugin.ini 계약)
"""

README_UNBUNDLED = f"""DsCppModManager v{VERSION} — DragonSword: Awakening 용 C++ 모드매니저
=====================================================================
(UE4SS 미동봉판 — UE4SS 가 이미 설치되어 있어야 합니다)

■ 전제 조건 — UE4SS 필요
  이 배포본에는 UE4SS 가 **들어 있지 않습니다.** 게임에 UE4SS 가 이미 설치되어
  있어야 모드매니저가 동작합니다. UE4SS 가 없다면 먼저 설치하세요:
    UE4SS 프로젝트: https://github.com/UE4SS-RE/RE-UE4SS
  (UE4SS 를 함께 담은 올인원 배포본도 따로 있습니다 — UE4SS 설치가 번거로우면
   그쪽을 쓰세요.)

  ※ 이 모드는 UE4SS 실험 채널 빌드 커밋 c838a8ac
    (UE4SS.dll {UE4SS_SIZE:,} 바이트) 에서 빌드/검증되었습니다.
    다른 버전의 UE4SS 위에서는 모드 DLL 로드가 거부될 수 있습니다
    (게임은 정상 실행되고 모드만 꺼집니다). 모드매니저가 안 보이면
    UE4SS 버전이 원인일 수 있습니다.

■ 설치 방법 1 — 자동 (권장)
  1) ★ 게임을 완전히 종료합니다. (실행 중이면 모드 파일이 잠겨 설치가 실패합니다)
  2) install.bat 을 더블클릭합니다.
     (기본 스팀 경로가 아니면: install.bat 아이콘 위로 게임 폴더를 드래그)
  3) 설치 완료 팝업이 나오면 끝. 게임을 실행하면 타이틀 메뉴에
     "모드매니저" 항목이 나타납니다.
  ※ UE4SS 가 설치돼 있지 않으면 안내 팝업이 뜨고 설치가 중단됩니다.
     먼저 UE4SS 를 설치한 뒤 다시 실행하세요.
  ※ Access denied 가 나오면 install.bat 을 우클릭 → 관리자 권한으로 실행.

■ 이전 버전에서 업데이트하기
  같은 방법으로 install.bat 을 실행하면 됩니다. 덮어쓰기 방식이라
  plugins 폴더의 모드, 각 모드 설정(dsoptions.txt)·켬끔 상태, 순서(dsorder.txt)는
  그대로 보존됩니다.

■ 설치 방법 2 — 수동
  DsCppModManager 폴더를 통째로 아래 위치에 복사합니다:
    <게임>\\DS\\Binaries\\Win64\\ue4ss\\Mods\\DsCppModManager
  mods.txt 는 편집할 필요가 없습니다. 폴더 안의 enabled.txt 가 있으면
  UE4SS 가 자동으로 모드를 시작합니다.

■ 비활성화 / 제거
  - 잠시 끄기: Mods\\DsCppModManager\\enabled.txt 를 삭제
  - 완전 제거: Mods\\DsCppModManager 폴더를 삭제

■ 플러그인(모드) 넣는 곳
  Mods\\DsCppModManager\\plugins\\<모드이름>\\
    ├─ Scripts\\main.lua   (Lua 모드)  또는
    └─ dlls\\main.dll      (C++ 모드)

■ 이 모드가 남기는 기록 (전부 게임 폴더 안에만, 밖으로 보내지 않습니다)
  Mods\\DsCppModManager\\dlls\\cppmm_log.txt   — 동작 기록
  Mods\\DsCppModManager\\blackbox*.txt         — 크래시가 어느 단계에서 났는지
  Mods\\DsCppModManager\\inputlog\\날짜.log     — 입력 진단 기록 (아래)

  입력 진단 기록은 "컨트롤러를 인식 못 한다" 같은 문제의 원인을 가리기 위한
  것으로, **타이틀 화면에 있을 때만** 남깁니다 (연결된 컨트롤러의 종류와 이름,
  누른 패드 버튼과 스틱 값). 게임 플레이 중에는 남기지 않고, 인터넷으로
  전송하지 않습니다.

  키보드는 **어떤 키를 눌렀는지 남기지 않습니다** -- "1초 동안 키 입력 N회"
  라는 횟수만 적습니다 (컨트롤러가 키보드로 전달되는지 판별하는 용도).
  ※ 타이틀에서 스팀 오버레이(Shift+Tab)를 열어 타자를 치면 그 횟수도 함께
    집계됩니다. 무엇을 쳤는지는 기록되지 않습니다.

  전부 남기지 않으려면 Mods 폴더의 DsCppModManager 안에 inputlog_off.txt 를
  만드세요 (다음 게임 실행부터 적용).

■ 문서
  MANUAL.md       — 사용 설명서
  CHANGELOG.md    — 버전별 변경 내역
  PLUGIN_GUIDE.md — 모드 제작자용 연동 가이드 (dsplugin.ini 계약)
"""

# ------------------------------------------------------------------ install.bat

# 게임 폴더 찾기 블록 -- 두 install.bat 이 공유한다.
_FIND_GAME = r"""setlocal EnableExtensions EnableDelayedExpansion
set "VER=__VERSION__"
set "SRC=%~dp0DsCppModManager"
__UEVARS__
set "EXE=DSClient-Win64-Shipping.exe"
set "VBS=%TEMP%\dsmm_install_msg.vbs"

rem ---------- 0) 압축을 제대로 풀었는지 ----------
if not exist "%SRC%\dlls\main.dll" goto :no_src
__CHECK_UESRC__

rem ---------- 1) 게임 폴더 찾기 (기준 = 게임 실행 파일) ----------
set "GAME=%~1"
if "%GAME%"=="" set "GAME=C:\Program Files (x86)\Steam\steamapps\common\DragonSword  Awakening"
set "W64=%GAME%\DS\Binaries\Win64"
if exist "%W64%\%EXE%" goto :found
set "W64=%GAME%\Binaries\Win64"
if exist "%W64%\%EXE%" goto :found
set "W64=%GAME%\Win64"
if exist "%W64%\%EXE%" goto :found
set "W64=%GAME%"
if exist "%W64%\%EXE%" goto :found
for %%I in ("%GAME%\..") do set "W64=%%~fI"
if exist "%W64%\%EXE%" goto :found

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
"""

# 매니저 복사 + 결과 팝업 블록 -- 두 install.bat 이 공유한다.
_COPY_AND_SAY = r"""
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

set "DSTSZ=0"
for %%A in ("%DEST%\dlls\main.dll") do set "DSTSZ=%%~zA"
if not "%SRCSZ%"=="%DSTSZ%" goto :locked

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
set "MSG=게임을 찾지 못했습니다.[br][br]찾아본 경로 : %GAME%[br][br]게임이 다른 드라이브에 있다면,[br]게임 폴더를 install.bat 아이콘 위로 끌어다 놓으세요.[br](DragonSword  Awakening 폴더)[br][br]찾는 기준은 DSClient-Win64-Shipping.exe 입니다.__NOGAME_UE__"
set "ICON=16"
goto :say
__EXTRA_LABELS__
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
echo.
echo !MSG:[br]= !
echo.
> "%VBS%" echo MsgBox Replace("!MSG!","[br]",vbCrLf), !ICON!, "DsCppModManager !VER!"
wscript "%VBS%" || pause
del "%VBS%" >nul 2>&1
if "!ICON!"=="16" exit /b 1
exit /b 0
"""

# 동봉판: UE4SS 설치 블록.
_UE_INSTALL_BUNDLED = r"""
rem ---------- 2) UE4SS: 없으면 넣고, 있으면 그대로 둔다 ----------
set "UEHAD=0"
if exist "%UE%\UE4SS.dll" set "UEHAD=1"
if "%UEHAD%"=="1" goto :ue_kept

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
set "UESZ=0"
for %%A in ("%UE%\UE4SS.dll") do set "UESZ=%%~zA"
set "UEMSG=UE4SS : 이미 있어 그대로 두었습니다"
if not "!UESZ!"=="%UESIZE%" set "UEMSG=UE4SS : 다른 버전이 이미 있어 그대로 두었습니다. 모드매니저가 안 보이면 README 의 'UE4SS 버전' 항목을 참고하세요"

:ue_done
"""

# 동봉판: UE4SS 설치 실패 라벨.
_UE_LABELS_BUNDLED = r"""
:ue_failed
set "MSG=UE4SS 설치에 실패했습니다.[br][br]위치 : %UE%[br][br]1) 게임이 실행 중이면 완전히 종료 후 다시 시도[br]2) 그래도 안 되면 install.bat 우클릭 -[br]   '관리자 권한으로 실행'[br][br]모드매니저는 UE4SS 없이는 동작하지 않습니다."
set "ICON=16"
goto :say
"""

# 미동봉판: UE4SS 존재 확인 블록 (설치하지 않고, 없으면 안내).
_UE_CHECK_UNBUNDLED = r"""
rem ---------- 2) UE4SS 가 이미 있어야 한다 (이 배포본은 UE4SS 미동봉) ----------
if not exist "%UE%\UE4SS.dll" goto :no_ue4ss
set "UESZ=0"
for %%A in ("%UE%\UE4SS.dll") do set "UESZ=%%~zA"
set "UEMSG=UE4SS : 이미 설치된 것을 사용합니다"
if not "!UESZ!"=="%UESIZE%" set "UEMSG=UE4SS : 설치된 버전이 검증본과 다릅니다. 모드매니저가 안 보이면 README 의 'UE4SS 버전' 항목을 참고하세요"
"""

# 미동봉판: UE4SS 없음 라벨.
_UE_LABELS_UNBUNDLED = r"""
:no_ue4ss
set "MSG=UE4SS 가 설치되어 있지 않습니다.[br][br]위치 : %UE%[br][br]이 배포본에는 UE4SS 가 들어 있지 않습니다(미동봉판).[br]먼저 UE4SS 를 설치한 뒤 다시 실행해 주세요.[br]  UE4SS: github.com/UE4SS-RE/RE-UE4SS[br][br]UE4SS 설치가 번거로우면 UE4SS 동봉 올인원[br]배포본을 대신 사용하세요."
set "ICON=16"
goto :say
"""


def make_install_bat(bundled):
    body = "@echo off\r\n" if False else ""   # placeholder; header built below
    header = ("@echo off\n"
              "rem ============================================================\n"
              f"rem  DsCppModManager {VERSION} installer  "
              + ("(UE4SS 동봉판)\n" if bundled else "(UE4SS 미동봉판 -- Nexus)\n") +
              "rem  * cp949(ANSI) + CRLF 로 저장된다.\n"
              "rem  * 결과는 항상 팝업으로 알린다.\n"
              "rem  * UE4SS 는 " + ("없으면 넣고 있으면 건드리지 않는다.\n" if bundled
                                    else "이미 설치돼 있어야 한다(이 배포본엔 미동봉).\n") +
              "rem ============================================================\n")
    find = _FIND_GAME
    if bundled:
        find = find.replace("__UEVARS__", 'set "UESRC=%~dp0UE4SS"\nset "UESIZE=__UE4SS_SIZE__"')
        find = find.replace("__CHECK_UESRC__", r'if not exist "%UESRC%\ue4ss\UE4SS.dll" goto :no_src')
        ue_block = _UE_INSTALL_BUNDLED
        copy = _COPY_AND_SAY.replace("__EXTRA_LABELS__", _UE_LABELS_BUNDLED)
        copy = copy.replace("__NOGAME_UE__",
                            "[br]UE4SS 는 없어도 됩니다. 이 설치본에 들어 있습니다.")
    else:
        find = find.replace("__UEVARS__", 'set "UESIZE=__UE4SS_SIZE__"')
        find = find.replace("__CHECK_UESRC__", "")
        ue_block = _UE_CHECK_UNBUNDLED
        copy = _COPY_AND_SAY.replace("__EXTRA_LABELS__", _UE_LABELS_UNBUNDLED)
        copy = copy.replace("__NOGAME_UE__", "")
    text = header + find + ue_block + copy
    text = text.replace("__VERSION__", VERSION).replace("__UE4SS_SIZE__", str(UE4SS_SIZE))
    return text


def write_manager(z):
    """DsCppModManager/* (매니저 본체) — 두 배포본이 공유한다."""
    z.write(os.path.join(MOD, "main.dll"), "DsCppModManager/dlls/main.dll")
    assets = os.path.join(MOD, "Assets")
    for fn in sorted(os.listdir(assets)):
        if fn.lower().endswith(".png"):
            z.write(os.path.join(assets, fn), f"DsCppModManager/Assets/{fn}")
    z.write(os.path.join(MOD, "MANUAL.md"), "DsCppModManager/MANUAL.md")
    z.write(os.path.join(MOD, "CHANGELOG.md"), "DsCppModManager/CHANGELOG.md")
    z.write(os.path.join(MOD, "PLUGIN_GUIDE.md"), "DsCppModManager/PLUGIN_GUIDE.md")
    z.write(os.path.join(MOD, "dsplugin.ini.example"), "DsCppModManager/dsplugin.ini.example")
    z.write(os.path.join(MOD, "dsmm_options.lua"), "DsCppModManager/dsmm_options.lua")
    z.writestr("DsCppModManager/plugins/", "")
    z.writestr("DsCppModManager/enabled.txt", "")


def write_install_bat(z, bundled):
    bat = make_install_bat(bundled).replace("\r\n", "\n").replace("\n", "\r\n")
    try:
        bat_bytes = bat.encode("cp949")
    except UnicodeEncodeError as e:
        sys.exit(f"[ERROR] install.bat 에 cp949 로 못 쓰는 문자가 있다: {e}")
    z.writestr("install.bat", bat_bytes)


def check_clean(zip_path, bundled):
    """배포본에 사용자 상태가 섞이지 않았는지 (PACKAGING.md 3·4항). 미동봉판은
    UE4SS/ 가 **없어야** 한다."""
    forbidden_names = ("dsruntime.txt", "dsoptions.txt", "dsorder.txt", "version.txt")
    bad = []
    with zipfile.ZipFile(zip_path) as z:
        for name in z.namelist():
            if name.startswith("DsCppModManager/plugins/") and name.rstrip("/") != "DsCppModManager/plugins":
                bad.append(f"plugins 안에 내용물: {name}")
            if os.path.basename(name).lower() in forbidden_names:
                bad.append(f"금지 파일: {name}")
            if not bundled and name.startswith("UE4SS/"):
                bad.append(f"미동봉판에 UE4SS 포함: {name}")
    if bad:
        os.remove(zip_path)
        sys.exit("[ERROR] 배포 규약 위반 -- zip 을 지웠다:\n  " + "\n  ".join(bad))


def build(bundled, ue4ss_payload):
    name = "DsCppModManager" if bundled else "DsCppModManager-NoUE4SS"
    out = os.path.join(OUT_DIR, f"{name}_{VERSION}.zip")
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        write_manager(z)
        if bundled:
            for entry, data in ue4ss_payload:
                z.writestr(entry, data)
            z.writestr("DsCppModManager/README.txt", "\ufeff" + README_BUNDLED)
        else:
            z.writestr("DsCppModManager/README.txt", "\ufeff" + README_UNBUNDLED)
        write_install_bat(z, bundled)
    check_clean(out, bundled)
    print(f"패키지 생성: {out} ({os.path.getsize(out):,} bytes)")
    with zipfile.ZipFile(out) as z:
        n = len(z.namelist())
    print(f"  entries: {n}")
    return out


def main():
    dll = os.path.join(MOD, "main.dll")
    if not os.path.exists(dll):
        sys.exit(f"[ERROR] 없음: {dll}")
    ue4ss_payload = read_ue4ss_payload()   # 해시 대조 포함
    os.makedirs(OUT_DIR, exist_ok=True)
    # 이전 판(동봉/미동봉 모두)을 Old 로 치운 뒤 둘을 새로 만든다.
    dspublish.archive_previous("DsCppModManager")
    dspublish.archive_previous("DsCppModManager-NoUE4SS")
    build(True, ue4ss_payload)
    build(False, ue4ss_payload)


if __name__ == "__main__":
    main()
