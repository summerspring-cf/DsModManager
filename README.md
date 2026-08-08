# DsModManager (DsCppModManager)

**DragonSword: Awakening** 의 타이틀 메뉴에서 모드를 켜고 끄고 설정하는 인게임 모드매니저.
UE4SS 위에서 도는 순수 C++ 모드다.

게임 설정창과 같은 위젯으로 만들어서 생김새와 조작감이 같고, **Lua 모드 · C++ 모드 ·
`.pak` 콘텐츠 모드**를 한 화면에서 함께 관리한다.

> 받아서 바로 쓰실 분은 소스를 받을 필요가 없다.
> [Releases](../../releases) 의 zip 을 풀고 `install.bat` 을 실행하면 끝이다.
> **UE4SS 가 함께 들어 있어 따로 받지 않아도 된다.**

---

## 문서

| 파일 | 대상 | 내용 |
|---|---|---|
| [MANUAL.md](mods/DsCppModManager/MANUAL.md) | **쓰는 사람** | 설치 · 화면 구성 · 조작 · 파일 위치 · 문제 해결 |
| [PLUGIN_GUIDE.md](mods/DsCppModManager/PLUGIN_GUIDE.md) | **모드 만드는 사람** | `dsplugin.ini` 계약, 옵션 컨트롤 8종, 값 읽는 법 |

## 모드 제작자에게

당신의 모드에 **`dsplugin.ini` 파일 하나**만 넣으면 매니저가 옵션 패널을 대신 그려 준다.
등록 함수 호출도, 링크할 API 도 없다 — 매니저가 폴더 생김새로 찾아낸다.

```ini
[plugin]
name=내 모드

[option:togglekey]
label=표시 토글키
type=key                     ; 사용자가 누른 키가 가상키 코드로 저장된다

[option:markercolor]
label=마커 색
type=color                   ; 팔레트 + 그라데이션 + #RRGGBB 입력
default=#FFD60A

[option:opacity]
label=오버레이 불투명도
type=slider
min=0
max=100
default=80
```

컨트롤 8종: `bool` 토글 · `int` 스테퍼 · 콤보박스(`choices=`) · `key` 키 지정 ·
`color` 색상 · `check` 체크박스 · `button` 실행 버튼 · `slider` 슬라이더.
모드당 옵션 16개까지, 자식 옵션(`parent=`) 가능.
자세한 것은 [PLUGIN_GUIDE.md](mods/DsCppModManager/PLUGIN_GUIDE.md).

---

## 저장소 구조

```
mods/DsCppModManager/     매니저 소스 (main.cpp 단일 파일 + ue4ss_abi.hpp)
  Assets/                 UI 이미지 (전부 코드로 그린 것 — 게임 파일이 아니다)
tools/                    빌드·패키징 스크립트
vendor/ue4ss/             배포에 동봉하는 UE4SS 원본 (손대지 않은 공식 zip)
```

경로가 `mods/…` 로 한 겹 들어가 있는 것은 원래 이 모드가 여러 모드를 담은 저장소에서
나왔기 때문이다. `build.bat` 과 `tools/package_cppmm.py` 가 이 상대 경로를 그대로
쓰므로 **폴더를 옮기지 말 것**.

### 빌드

```
mods\DsCppModManager\build.bat
```

- MSVC Build Tools 2022 (`vcvars64.bat` 경로가 `build.bat` 에 박혀 있다)
- **`/MD` Release 전용.** Debug STL 은 레이아웃이 달라 크래시한다
  (이유는 `ue4ss_abi.hpp` 머리 주석)
- `UE4SS.def` 는 커밋되어 있으므로 그대로 빌드된다.
  `UE4SS.lib` / `UE4SS.exp` 는 `build.bat` 이 만들어 낸다.
- ⚠ `tools/make_ue4ss_def.py` 는 `.def` 의 **출처를 남긴 도구**다. 실행하려면
  `objdump -p UE4SS.dll` 덤프가 필요하고 출력 경로도 이 저장소와 맞지 않는다.
  `.def` 가 이미 있으므로 부를 일이 없다.

### 배포본 만들기

```
python tools\package_cppmm.py
```

`Plugins_Publish/DsCppModManager_<버전>.zip` 이 나온다. 버전은 `main.cpp` 의
`ModVersion` 에서 읽는다. `vendor/ue4ss/` 의 UE4SS 와 생성한 `install.bat`,
`README.txt` 가 함께 담긴다.

---

## UE4SS 에 대하여

이 저장소와 배포본은 **UE4SS 를 동봉한다.**

| 항목 | 값 |
|---|---|
| 프로젝트 | [RE-UE4SS](https://github.com/UE4SS-RE/RE-UE4SS) — MIT License, Copyright (c) 2022 Narknon |
| 채널 · 커밋 | experimental, `c838a8acaade1a0f860bdf249f039e58f4e10088` |
| `ue4ss/UE4SS.dll` | 16,519,168 B |

**왜 하필 이 빌드인가**: 매니저 DLL 은 이 빌드의 export 목록에서 뽑은 `UE4SS.lib` 로
링크된다. 스톡 릴리스(3.0.1)와 export 가 달라, 다른 빌드 위에서는 모드 DLL 로드가
거부될 수 있다(게임은 정상 실행되고 모드만 안 뜬다). 실험 채널은 굴러가는 빌드라
지금 GitHub 에서 받아도 같은 커밋이 나온다는 보장이 없어, 검증된 이 판을 고정해 둔다.

자세한 근거는 [vendor/ue4ss/README.md](vendor/ue4ss/README.md).

---

## 라이선스

**All rights reserved.** 이 저장소의 코드·이미지·문서를 무단으로 재배포하거나
개조판을 배포하지 않는다. 개조·재배포·에셋 사용이 필요하면 먼저 문의할 것.

예외 둘:

1. **`mods/DsCppModManager/dsmm_options.lua` 는 자유롭게 복사해 써도 된다.**
   플러그인이 매니저의 설정값을 읽으라고 만든 헬퍼라, 자기 모드의 `Scripts\` 폴더에
   그대로 넣고 배포해도 좋다. 표기 의무 없음.
2. **`vendor/ue4ss/` 는 이 라이선스가 적용되지 않는다.** RE-UE4SS 의 MIT License 를
   따르며, 전문은 zip 안 `ue4ss/LICENSE` 에 들어 있다.

`Assets/` 의 UI 이미지는 게임 설정창을 보고 **코드로 새로 그린 것**이다.
게임 파일을 추출하거나 재배포하지 않는다.

---

made by **SummerSpring**
