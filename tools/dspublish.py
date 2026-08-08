# -*- coding: utf-8 -*-
"""배포본 폴더 규약 한 곳 (정본 문서: 저장소 루트 PACKAGING.md 1항).

    <저장소>\\Plugins_Publish\\<영문명>_<버전>.zip     <- **최신 배포본만** 여기
    <저장소>\\Old_Plugins_Publish\\<영문명>\\...zip     <- 이전 버전은 모드명 폴더로 이동

패키징 스크립트는 zip 을 쓰기 **직전에** archive_previous(NAME, out) 을 부른다.
그러면 Plugins_Publish 에는 언제나 모드당 zip 이 하나뿐이라, 사용자가 어느 게
최신인지 파일 이름을 뜯어보지 않아도 된다. 이전 판은 지우지 않고 **이동**한다 --
되돌릴 수 있어야 하기 때문이다.

폴더 이름은 `Plugins_Publish` / `Old_Plugins_Publish` 두 개다(둘 다 s 있음).
스크립트는 여기 상수만 쓰고 문자열을 다시 적지 않는다 -- 한 곳만 고치면 되게.

정리 도구로도 쓴다 (모드당 최신 하나만 남기고 나머지를 보관 폴더로):
    python tools/dspublish.py            <- 계획만 출력
    python tools/dspublish.py --apply    <- 실제 이동
"""
import os
import re
import shutil
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PUBLISH_DIR = os.path.join(ROOT, "Plugins_Publish")
OLD_DIR = os.path.join(ROOT, "Old_Plugins_Publish")


def _vkey(ver):
    """버전 문자열을 비교 가능한 튜플로. '0.21' -> (0, 21). 숫자가 없으면 (-1,)."""
    nums = [int(x) for x in re.findall(r"\d+", ver)]
    return tuple(nums) if nums else (-1,)


def _split(fn):
    """'DsAutoFood_1.2.zip' -> ('DsAutoFood', '1.2'). 형식이 아니면 (None, None)."""
    if not fn.lower().endswith(".zip"):
        return None, None
    stem = fn[:-4]
    if "_" not in stem:
        return None, None
    mod, ver = stem.split("_", 1)
    return mod, ver


def archive_previous(name, keep=None):
    """Plugins_Publish 의 <name>_*.zip 을 Old_Plugins_Publish/<name>/ 로 이동한다.

    keep: 남길 파일(경로든 파일명이든). 같은 버전을 다시 빌드할 때 자기 자신을
          보관 폴더로 치워 버리지 않게 하는 안전장치다.
    반환: 옮긴 파일명 목록.
    """
    os.makedirs(PUBLISH_DIR, exist_ok=True)
    keep = os.path.basename(keep) if keep else None
    moved = []
    for fn in sorted(os.listdir(PUBLISH_DIR)):
        mod, _ = _split(fn)
        if mod != name or fn == keep:
            continue
        dstdir = os.path.join(OLD_DIR, name)
        os.makedirs(dstdir, exist_ok=True)
        dst = os.path.join(dstdir, fn)
        if os.path.exists(dst):
            os.remove(dst)
        shutil.move(os.path.join(PUBLISH_DIR, fn), dst)
        moved.append(fn)
        print("  이전 판 보관: %s -> Old_Plugins_Publish/%s/" % (fn, name))
    return moved


def _cleanup(apply):
    """지금 Plugins_Publish 에 있는 것들을 모드별로 묶어, 최신 하나만 남긴다."""
    if not os.path.isdir(PUBLISH_DIR):
        sys.exit("배포본 폴더가 없다: %s" % PUBLISH_DIR)
    groups = {}
    other = []
    for fn in sorted(os.listdir(PUBLISH_DIR)):
        mod, ver = _split(fn)
        if mod:
            groups.setdefault(mod, []).append((ver, fn))
        elif fn.lower().endswith(".zip"):
            other.append(fn)

    for mod in sorted(groups):
        items = groups[mod]
        # 최신 = 버전이 큰 것. 버전이 같거나 못 읽으면 파일 시각으로 가른다.
        items.sort(key=lambda t: (_vkey(t[0]),
                                  os.path.getmtime(os.path.join(PUBLISH_DIR, t[1]))))
        newest = items[-1][1]
        olds = [fn for _, fn in items[:-1]]
        mark = "  (최신 하나뿐)" if not olds else ""
        print("[%s] 최신 %s%s" % (mod, newest, mark))
        if olds and not apply:
            for fn in olds:
                print("  옮길 것: %s -> Old_Plugins_Publish/%s/" % (fn, mod))
        elif olds:
            archive_previous(mod, keep=newest)

    if other:
        print("\n⚠ 이름 규약(<영문명>_<버전>.zip)에 안 맞는 파일 -- 손대지 않았다:")
        for fn in other:
            print("   %s" % fn)
    if not apply:
        print("\n(계획만 출력했다. 실제로 하려면 --apply)")


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8")
    _cleanup("--apply" in sys.argv)
