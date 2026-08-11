# DsCppModManager

An **in-game mod manager** for *DragonSword: Awakening* (Unreal Engine 5.3.2, Steam).
It adds a **모드매니저 (Mod Manager)** entry to the title menu, opening a panel built from
the game's own settings widgets, where you can enable/disable mods, reorder them, and change
each mod's settings — without editing any config file.

It is a pure C++ UE4SS mod. It manages **Lua mods, C++ mods and `.pak` content mods** side by side.

> **Reviewing this repository for a security check?**
> Start with **[SECURITY.md](SECURITY.md)** — it explains exactly what the binary does at
> runtime, why the archive trips automated scanners, and lists SHA-256 hashes for every
> binary in the release. Build instructions are in [§ Building](#building) below.
>
> **Please note up front: I did not write UE4SS.** ~98.6 % of the release archive by size is
> [RE-UE4SS](https://github.com/UE4SS-RE/RE-UE4SS) (MIT, Copyright (c) 2022 Narknon), included
> **unmodified** and purely so that players who have no `ue4ss` folder do not have to install
> it by hand. `install.bat` unpacks it only when it is missing and never touches an existing
> installation. My own code is the ~240 KB `DsCppModManager/` folder — that is 1.4 % of the
> archive, and all of it is in this repository. See [SECURITY.md § 0](SECURITY.md).

---

## Downloads

Users do not need this repository. Grab the archive, extract it, and run `install.bat`.

Two builds are published for each release:

- **`DsCppModManager_<ver>.zip`** — all-in-one, **UE4SS bundled**. Nothing else to download.
- **`DsCppModManager-NoUE4SS_<ver>.zip`** — for players who already run UE4SS. Tiny; its
  `install.bat` stops with a message if UE4SS isn't installed yet.

| Source | Notes |
|---|---|
| **[GitHub Releases](https://github.com/summerspring-cf/DsModManager-Release/releases)** | Canonical, in the separate [release repository](https://github.com/summerspring-cf/DsModManager-Release). SHA-256 published in [SECURITY.md](SECURITY.md#5-verifying-the-release-binaries). |
| **[Hangul Patch Studio](https://hangulpatchstudio.com/g/%EB%93%9C%EB%9E%98%EA%B3%A4%EC%86%8C%EB%93%9C-%EB%AA%A8%EB%93%9C%EB%A7%A4%EB%8B%88%EC%A0%80)** | My own distribution page. Same archive. Korean-language page, direct download, **no account or registration needed**. |
| Nexus Mods | Currently unavailable — the upload is under automated quarantine, which is what this repository was published to help resolve. |

---

## Documentation

| File | Audience | Contents |
|---|---|---|
| [MANUAL.md](mods/DsCppModManager/MANUAL.md) | Users | Install, screen layout, controls, file locations, troubleshooting |
| [PLUGIN_GUIDE.md](mods/DsCppModManager/PLUGIN_GUIDE.md) | Mod authors | The `dsplugin.ini` contract, the 8 option controls, reading values back |
| [SECURITY.md](SECURITY.md) | Reviewers | Runtime behaviour, syscall surface, hashes, provenance |

*(MANUAL.md and PLUGIN_GUIDE.md are written in Korean — they ship inside the release archive
for end users. This README and SECURITY.md are in English.)*

---

## Repository layout

```
mods/DsCppModManager/     The mod itself
    main.cpp              Entire mod, single translation unit (~296 KB)
    ue4ss_abi.hpp         Hand-transcribed UE4SS ABI (see below)
    UE4SS.def             Export list used to link against UE4SS.dll  (committed — see note)
    build.bat             The whole build
    Assets/*.png          UI images, 11 files, all drawn procedurally (not game files)
    MANUAL.md             End-user manual (Korean)
    PLUGIN_GUIDE.md       Mod-author guide (Korean)
    dsplugin.ini.example  Example manifest for plugin authors
    dsmm_options.lua      Optional helper Lua mod authors may copy into their own mod
tools/
    package_cppmm.py      Builds the release archive
    make_ue4ss_def.py     Provenance of UE4SS.def (see § UE4SS.def)
    dspublish.py          Release-folder conventions (imported by package_cppmm.py)
vendor/ue4ss/
    UE4SS_experimental.zip   Unmodified official UE4SS build, bundled into releases
    README.md                Which build, why that one, hashes
```

The `mods/…` nesting exists because this mod was extracted from a multi-mod repository.
`build.bat` and `tools/package_cppmm.py` use these relative paths, so **do not move the folders**.

---

## Building

There are **no external dependencies, no package manager, and no network access** during the
build. Everything needed is in this repository.

### Prerequisites

| | |
|---|---|
| OS | Windows 10 / 11, x64 |
| Compiler | **Visual Studio Build Tools 2022** with the *Desktop development with C++* workload (MSVC v143 + Windows SDK) |
| Python | 3.x — **only** for `tools/package_cppmm.py`. Not needed to build the DLL. |

### Build

```bat
mods\DsCppModManager\build.bat
```

That is the entire build. It takes about a minute.

`build.bat` calls `vcvars64.bat` from a hard-coded Build Tools path:

```
C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat
```

If you installed Visual Studio Community/Professional instead, either edit that one line, or
skip it by opening an **"x64 Native Tools Command Prompt for VS 2022"** and running the two
commands below by hand:

```bat
cd mods\DsCppModManager

rem 1) import library, generated from the committed export list
lib /nologo /def:UE4SS.def /machine:x64 /out:UE4SS.lib

rem 2) the mod
cl /nologo /std:c++latest /utf-8 /EHsc /MD /O2 /W3 /D NDEBUG /LD ^
   main.cpp /Fe:main.dll /link UE4SS.lib user32.lib shell32.lib
```

**Output:** `mods\DsCppModManager\main.dll`, about 224 KB. That is the file shipped as
`DsCppModManager/dlls/main.dll` inside the release archive.

### Build flags — why these exactly

- **`/MD` Release only.** UE4SS is built against the release CRT. Linking the debug CRT gives
  different `std::string` / `std::vector` layouts, and objects crossing the ABI boundary then
  corrupt memory. This is not a preference; a `/MDd` build crashes on load.
- **`/std:c++latest`** — the transcribed ABI header uses recent language features.
- **`/utf-8`** — the source contains Korean string literals and comments.
- No LTCG, no `/GL`, no custom sections, no packing, no obfuscation.

### About `UE4SS.def` (a generated file that is committed on purpose)

The mod links against UE4SS's exported C++ API. There is no import library in the UE4SS
release, so one is generated from the DLL's export table:

```
UE4SS.dll  --(objdump -p)-->  export list  -->  UE4SS.def  --(lib.exe)-->  UE4SS.lib
```

`UE4SS.def` is a plain-text list of exported symbol names (~357 KB). It is **committed** so
that a fresh clone builds with nothing but MSVC. `tools/make_ue4ss_def.py` documents where it
came from, but it needs an `objdump -p` dump that is not in this repository, and its output
path targets a different mod in the original multi-mod repo — you do not need to run it.

`UE4SS.lib` and `UE4SS.exp` are regenerated by `build.bat` and are not committed.

### Reproducibility

MSVC output is **not** bit-for-bit reproducible (embedded timestamps and paths), so your
`main.dll` will not hash-match the released one. It should be the same size (±a few bytes) and
functionally identical. If you need byte-level assurance, the released DLL's SHA-256 is in
[SECURITY.md](SECURITY.md), and the source is 100% of what goes into it — there is no
pre-built code, no linked static library other than the Windows SDK and `UE4SS.lib` generated
above.

### Producing the release archive

```bat
python tools\package_cppmm.py
```

Writes `Plugins_Publish\DsCppModManager_<version>.zip`. The version is read from `ModVersion`
in `main.cpp`. The script assembles: the built `main.dll`, `Assets/`, the two Korean docs, the
generated `README.txt` and `install.bat`, and the contents of `vendor/ue4ss/UE4SS_experimental.zip`
under a `UE4SS/` prefix.

---

## About the bundled UE4SS

Releases of this mod **include UE4SS**.

| | |
|---|---|
| Project | [RE-UE4SS](https://github.com/UE4SS-RE/RE-UE4SS) — MIT License, Copyright (c) 2022 Narknon |
| Channel / commit | `experimental`, `c838a8acaade1a0f860bdf249f039e58f4e10088` |
| `ue4ss/UE4SS.dll` | 16,519,168 bytes |
| `dwmapi.dll` | 71,680 bytes (UE4SS's proxy loader) |

The file in `vendor/ue4ss/` is the **unmodified official archive**, byte-for-byte. Hashes are
in [vendor/ue4ss/README.md](vendor/ue4ss/README.md) and [SECURITY.md](SECURITY.md).

**Why pin this specific build:** the mod DLL is linked against the export table of *this*
UE4SS build. The stock 3.0.1 release exports a different set, and on a mismatched build the
mod DLL is simply refused — the game still launches normally, the mod just never appears.
The experimental channel is a moving target, so downloading it today is not guaranteed to
produce commit `c838a8ac`. Pinning it is the only way to make the release reliably work.

UE4SS is redistributed under the MIT License; its full licence text ships as `ue4ss/LICENSE`
inside every release archive.

---

## For mod authors

Drop a single `dsplugin.ini` next to your mod and the manager builds your settings panel.
There is no registration call and no API to link against — the manager discovers your mod
from its folder shape (`Scripts\main.lua` for Lua, `dlls\main.dll` for C++).

```ini
[plugin]
name=My Mod

[option:togglekey]
label=Toggle key
type=key                     ; user clicks, presses a key, you get the VK code

[option:markercolor]
label=Marker colour
type=color                   ; palette + gradient + #RRGGBB typing; you get 0xRRGGBB
default=#FFD60A

[option:opacity]
label=Overlay opacity
type=slider
min=0
max=100
default=80
```

Eight control types — `bool` toggle, `int` stepper, combo box (`choices=`), `key` bind,
`color` picker, `check` box, `button`, `slider`. Up to 16 options per mod, with nested
child options (`parent=`). Values land in `dsoptions.txt` next to your mod as `key=value`
lines. Full contract: [PLUGIN_GUIDE.md](mods/DsCppModManager/PLUGIN_GUIDE.md).

---

## Licence

**All rights reserved.** Do not redistribute this code, these images, or these documents, and
do not publish modified builds. Ask first if you need to modify, redistribute, or reuse assets.

Two exceptions:

1. **`mods/DsCppModManager/dsmm_options.lua` may be copied freely.** It exists so plugins can
   read the manager's settings; copy it into your own mod's `Scripts\` folder and ship it.
   No attribution required.
2. **`vendor/ue4ss/` is not covered by this licence.** It is RE-UE4SS under the MIT License;
   the full text is `ue4ss/LICENSE` inside the archive.

The images in `Assets/` were drawn from scratch in code to match the game's settings UI.
No game files are extracted or redistributed.

---

# 한국어

DragonSword: Awakening 의 타이틀 메뉴에서 모드를 켜고 끄고 설정하는 **인게임 모드매니저**.
UE4SS 위에서 도는 순수 C++ 모드이며, **Lua · C++ · `.pak` 콘텐츠 모드**를 한 화면에서
함께 관리한다.

받아서 쓰실 분은 소스가 필요 없다 —
[Releases](https://github.com/summerspring-cf/DsModManager/releases) 의 zip 을 풀고
`install.bat` 을 실행하면 끝이다. **UE4SS 가 함께 들어 있어 따로 받지 않아도 된다.**

## 문서

| 파일 | 대상 |
|---|---|
| [MANUAL.md](mods/DsCppModManager/MANUAL.md) | 쓰는 사람 — 설치 · 화면 구성 · 조작 · 문제 해결 |
| [PLUGIN_GUIDE.md](mods/DsCppModManager/PLUGIN_GUIDE.md) | 모드 만드는 사람 — `dsplugin.ini` 계약 |
| [SECURITY.md](SECURITY.md) | 보안 검토용 (영문) — 런타임 동작 · 호출하는 API · 해시 |

## 빌드

```
mods\DsCppModManager\build.bat
```

- MSVC Build Tools 2022 (`vcvars64.bat` 경로가 `build.bat` 에 박혀 있다)
- **`/MD` Release 전용** — Debug STL 은 레이아웃이 달라 로드 즉시 크래시한다
  (이유는 `ue4ss_abi.hpp` 머리 주석)
- `UE4SS.def` 는 커밋되어 있으므로 새 클론에서 그대로 빌드된다.
  `UE4SS.lib` / `UE4SS.exp` 는 `build.bat` 이 만든다
- 외부 의존성 · 패키지 매니저 · 네트워크 접근 **없음**

배포본 만들기:

```
python tools\package_cppmm.py
```

`Plugins_Publish/DsCppModManager_<버전>.zip` 이 나온다. 버전은 `main.cpp` 의 `ModVersion`
에서 읽는다.

## 라이선스

**All rights reserved.** 무단 재배포·개조판 배포 금지. 필요하면 먼저 문의할 것.

예외 둘: `dsmm_options.lua` 는 플러그인 제작에 자유롭게 복사 가능(표기 의무 없음),
`vendor/ue4ss/` 는 RE-UE4SS 의 MIT 라이선스를 따른다.

---

made by **SummerSpring**
