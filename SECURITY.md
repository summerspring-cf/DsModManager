# Security review notes

This document is written for reviewers. It states plainly what the mod does at runtime, why
the release archive looks alarming to an automated scanner, and how to verify every binary in
it against this source tree.

Everything below can be checked against `mods/DsCppModManager/main.cpp` — the mod is a single
translation unit with no obfuscation, no packing, and no generated or pre-built code.

---

## 0. Read this first: **I did not write UE4SS**

By size, **98.6 % of the release archive is not my code.** It is UE4SS, a third-party
framework I bundled as an installer convenience — nothing more.

| | Size | Share | Author |
|---|---|---|---|
| `DsCppModManager/` — **the actual mod** | ~240 KB | **1.4 %** | Me. Built from `main.cpp` in this repository. |
| `UE4SS/` — the bundled loader | ~16.6 MB | **98.6 %** | **[RE-UE4SS](https://github.com/UE4SS-RE/RE-UE4SS)**, Copyright (c) 2022 Narknon, MIT License. **Unmodified, byte-for-byte official build.** |

**Why it is in the archive at all.** UE4SS is a prerequisite: this mod cannot load without it.
Most players of this game have never installed it and do not have a `ue4ss` folder at all, and
installing it by hand is the step people get wrong — the files go next to the game executable,
not in a mods folder. So the installer fills it in for them:

- If `…\DS\Binaries\Win64\ue4ss\` **does not exist**, `install.bat` unpacks the bundled copy there.
- If it **already exists**, `install.bat` leaves it completely alone and installs only my mod.
  It never overwrites, upgrades, or downgrades an existing UE4SS.

**I claim no authorship of it, and no rights over it.** It is redistributed under its own MIT
License, whose full text ships as `ue4ss/LICENSE` inside every archive, and this repository's
"all rights reserved" notice explicitly does not apply to `vendor/ue4ss/`. Nothing about it was
altered — §5 gives SHA-256 hashes so you can verify it against the upstream release yourself.

If your scanner flagged this upload, the bundled UE4SS is the most likely reason, and
§1 explains exactly which of its properties look alarming and why.

---

## 1. Why the archive trips automated scanners

Four things in the release archive match malware heuristics. All four are inherent to how
Unreal Engine modding works on Windows.

| What the scanner sees | What it actually is |
|---|---|
| `dwmapi.dll` placed next to the game executable | The **UE4SS proxy loader**. Textbook DLL search-order hijacking as a pattern — and that is exactly, and openly, how UE4SS is designed to load. Unmodified official file. |
| `UE4SS.dll`, 15.8 MB, unsigned | [RE-UE4SS](https://github.com/UE4SS-RE/RE-UE4SS), a widely used Unreal Engine scripting framework. Unmodified official build. Hashes in §5. |
| `dlls/main.dll` — an unsigned DLL loaded into a game process that hooks an engine function | This mod. It registers a pre-callback on `UObject::ProcessEvent` **through UE4SS's public C++ mod API** (`UnrealInitCallback` / `ProcessEvent` pre-hook). It does not patch code itself. |
| `install.bat` writing into `C:\Program Files (x86)\Steam\...` | The installer. It copies the mod folder into the game's UE4SS `Mods` directory. Nothing is written outside the game's own installation. |

None of these involve persistence, privilege escalation, injection into other processes, or
any network activity.

---

## 2. What the mod does **not** do

Each of these is a `grep` away in `main.cpp`:

- **No network code of any kind.** No sockets, no WinINet/WinHTTP, no `URLDownloadToFile`,
  no HTTP client. Nothing is uploaded, downloaded, or phoned home. There is no telemetry,
  no update check, and no analytics.
- **No registry access.** No `RegOpenKey`, no `RegQueryValue`, no `HKEY_*`.
- **No manual code patching or injection.** No `VirtualProtect`, `WriteProcessMemory`,
  `LoadLibrary`, `GetProcAddress`, `CreateRemoteThread`, or `SetWindowsHookEx`. All hooking
  goes through UE4SS's mod API, inside the game's own process.
- **No persistence.** Nothing is installed as a service, scheduled task, autorun entry, or
  startup item. The mod only exists while the game is running.
- **No obfuscation, packing, or anti-analysis.** One readable `main.cpp`, `/MD /O2`, no
  packer, no encrypted blobs, no runtime string decryption.
- **No writes outside the game folder.** See §4.

---

## 3. Full disclosure of the two behaviours that deserve explanation

### 3.1 Keyboard polling — for key binding only

The mod polls keyboard state with `GetAsyncKeyState`. Two separate uses; the second is the one
that needs explaining.

**Continuous (always running):** exactly two keys, for the panel's own UI —
`VK_LBUTTON` (mouse click edges) and `VK_ESCAPE` (close the panel). The game's own input
pipeline does not deliver these to a mod reliably, so edges are latched on UE4SS's update
thread and consumed by the game-thread pump.

**Full key scan (gated):** when the user clicks a `type=key` option, the panel switches to
"press a key" and a flag is set. **Only while that flag is set** does the loop below run:

```cpp
// main.cpp, CppUserModBase::on_update()
if (g_keyCapture.load(...) && g_capturedVk.load(...) == 0)
{
    for (int vk = 0x01; vk <= 0xFE; ++vk)
    {
        if (vk == VK_LBUTTON || vk == VK_ESCAPE) continue;
        if (GetAsyncKeyState(vk) & 0x8000) { g_capturedVk.store(vk); break; }
    }
}
```

It stops at the **first** key pressed, stores that single virtual-key code, and clears the
flag. The captured value is the binding the user just chose; it is written to that mod's
`dsoptions.txt` as an integer (e.g. `togglekey=118` for F7). No sequence is recorded, no
keystroke text is stored anywhere, and — as stated in §2 — there is no code that could send
anything off the machine.

This is the same interaction as the game's own key-rebinding screen.

### 3.2 Exactly one process is created — Windows' own `tar.exe`

The manager can auto-extract a `.zip` **that the user themselves placed** in its `plugins`
folder, as a convenience. That is the only `CreateProcess` in the codebase:

```cpp
// main.cpp — runTarExtract()
wchar_t tar[MAX_PATH];
GetSystemDirectoryW(tar, MAX_PATH);          // resolved from System32, NOT from PATH
lstrcatW(tar, L"\\tar.exe");
if (!pathExistsW(tar)) { /* give up, log, do nothing */ }
swprintf(cmd, ..., L"\"%s\" -xf \"%s\" -C \"%s\"", tar, zipPath, destDir);
CreateProcessW(nullptr, cmd, ..., CREATE_NO_WINDOW, ...);
WaitForSingleObject(pi.hProcess, 30000);     // 30s cap, then abandoned
```

Points a reviewer will want:

- The executable is **`%SystemRoot%\System32\tar.exe`**, resolved through
  `GetSystemDirectoryW` — deliberately **not** through `PATH`, so a planted `tar.exe`
  earlier in `PATH` cannot be picked up.
- Source and destination are both inside the manager's own `plugins` folder.
- If `tar.exe` does not exist (older Windows), it logs and does nothing.
- Nothing else is ever executed. `ShellExecuteW` appears once more, only to open the
  `plugins` folder in Explorer when the user clicks the "폴더 바로가기" (Open folder) button.

---

## 4. Everything the mod writes to disk

All paths are inside the game installation.

| Path | What | Why |
|---|---|---|
| `…\ue4ss\Mods\DsCppModManager\dlls\cppmm_log.txt` | Text log | Diagnostics |
| `…\ue4ss\Mods\DsCppModManager\plugins\<mod>\dsoptions.txt` | `key=value` lines | The user's option values |
| `…\ue4ss\Mods\DsCppModManager\plugins\<mod>\dsruntime.txt` | `1` or `0` | Runtime on/off flag mods poll |
| `…\ue4ss\Mods\<mod>\enabled.txt` | Empty file | UE4SS's own "start this mod" marker |
| `…\ue4ss\Mods\DsCppModManager\dsorder.txt`, `bootstate.txt`, `safemode_last.txt` | Text | Mod order, last-launch fingerprint, safe-mode record |
| `…\ue4ss\Mods\<mod>\` | **Directory junction** | See below |
| `…\DS\Content\Paks\LogicMods\*.pak` (or `~mods\`) | **Hard links** | See below |

**Junctions and hard links instead of copies.** Enabling a mod used to copy its folder into
`Mods\`, which meant two copies to keep in sync. Since v0.26 the manager creates a *directory
junction* pointing at the single copy under `plugins\`; disabling removes it with
`RemoveDirectoryW` only (never a recursive delete, which would destroy the target). `.pak`
content mods are linked into the game's `Paks` folder with `CreateHardLinkW` and unlinked on
disable. Both are ordinary Win32 filesystem features and both stay within the game folder.

---

## 5. Verifying the release binaries

### Where to get the archive

The Nexus Mods copy is quarantined, so it cannot be downloaded there. Two other sources serve
the identical archive, neither of which needs an account:

| Source | |
|---|---|
| **GitHub Releases** | <https://github.com/summerspring-cf/DsModManager-Release/releases/tag/v0.40> — canonical (release archives live in a separate repository) |
| **Hangul Patch Studio** | <https://hangulpatchstudio.com/g/%EB%93%9C%EB%9E%98%EA%B3%A4%EC%86%8C%EB%93%9C-%EB%AA%A8%EB%93%9C%EB%A7%A4%EB%8B%88%EC%A0%80> — my own distribution page. The page text is Korean, but the download is a direct link and needs no login or registration. |

Verify whichever you download against the hashes below; if they ever disagree, the GitHub
Release is authoritative.

**All-in-one — `DsCppModManager_0.40.zip`** (UE4SS bundled):

| File | Size (bytes) | SHA-256 |
|---|---|---|
| *(the archive itself)* | 7,372,473 | `845ff7f735a6e6c226d8d8490645225064e3a36768fb2c67f730d79de5209da0` |
| `DsCppModManager/dlls/main.dll` | 276,992 | `c169def1555d2f57f8c65ddeea2523422c9ae7e71f0e6c4dfd3e3591ac7acfea` |
| `UE4SS/ue4ss/UE4SS.dll` | 16,519,168 | `d0107f63e567313cb6a15c505b5db2bdba38130964a04e019bda7611c6178022` |
| `UE4SS/dwmapi.dll` | 71,680 | `cfbd121b9e464b3ff35baba0f065d860aaffa7eb90f703748cd8e5b7730fa97e` |

**No-UE4SS — `DsCppModManager-NoUE4SS_0.40.zip`** (contains no UE4SS; 100 % my code):

| File | Size (bytes) | SHA-256 |
|---|---|---|
| *(the archive itself)* | 192,410 | `156f23fbde6200c0c70ee9197f59bd05f974d6ad4e777821df67103eb0247569` |
| `DsCppModManager/dlls/main.dll` | 276,992 | `c169def1555d2f57f8c65ddeea2523422c9ae7e71f0e6c4dfd3e3591ac7acfea` |

The bundled UE4SS source archive in this repository:

| File | Size (bytes) | SHA-256 |
|---|---|---|
| `vendor/ue4ss/UE4SS_experimental.zip` | 7,181,826 | `1a60989c29f94791f880f6e46130a7121533ffde753482572cba4a2e69f5fed9` |

That archive is the **unmodified official RE-UE4SS build**, experimental channel, commit
`c838a8acaade1a0f860bdf249f039e58f4e10088`. The two DLL hashes above are the files as they
come out of it — you can verify by extracting the vendored zip and hashing directly, without
running anything.

`main.dll` is produced entirely from `mods/DsCppModManager/main.cpp` by the command line in
[README § Building](README.md#building). MSVC embeds timestamps, so a rebuild will not be
bit-identical; it will be the same size to within a few bytes and functionally identical.

---

## 6. What the mod is for, in one paragraph

Players of this game accumulate mods that each ship their own `config.ini`, and there is no
in-game way to turn one off or change a setting without alt-tabbing and editing text files.
This mod adds one panel to the title screen that lists every mod it finds, lets you toggle
and reorder them, and — for mods that opt in with a small `dsplugin.ini` manifest — renders
their settings as real UI controls (toggles, dropdowns, key binds, colour pickers, sliders).
It reads and writes small text files next to each mod, and does nothing else.

---

## Contact

Issues on this repository, or the mod's Nexus Mods comments section.
