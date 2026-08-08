"""UE4SS.dll 의 export 목록에서 링크용 .def 파일을 만든다.

입력: research/dumps/ue4ss_exports.txt  (w64devkit objdump -p 출력 저장본.
      다시 뽑으려면: objdump -p UE4SS.dll > research/dumps/ue4ss_exports.txt)
출력: mods/DsCppProbe/UE4SS.def

이 .def 를 MSVC lib.exe 에 넣으면 게임에 설치된 바로 그 UE4SS.dll 을 향한
임포트 라이브러리(UE4SS.lib)가 나온다. 스톡 릴리스의 .lib 을 쓰지 않는
이유: 게임의 UE4SS 는 실험판 커밋(c838a8ac)이라 릴리스와 export 목록이
다를 수 있고, 실물에서 뽑으면 그 불일치가 원천적으로 없다.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "research" / "dumps" / "ue4ss_exports.txt"
DST = ROOT / "mods" / "DsCppProbe" / "UE4SS.def"

# objdump 의 [Ordinal/Name Pointer] Table 줄:
#   "\t[  44] +base[  45]  002c ??0CppUserModBase@RC@@QEAA@XZ"
LINE = re.compile(r"^\s*\[\s*\d+\]\s+\+base\[\s*\d+\]\s+[0-9a-fA-F]+\s+(\S+)\s*$")

def main() -> int:
    names = []
    for line in SRC.read_text(encoding="utf-8", errors="replace").splitlines():
        m = LINE.match(line)
        if m:
            names.append(m.group(1))
    if len(names) < 1000:
        print(f"의심스럽게 적다: export {len(names)}개 -- 입력 파일을 확인하라")
        return 1
    body = "LIBRARY UE4SS.dll\nEXPORTS\n" + "\n".join(f"    {n}" for n in names) + "\n"
    DST.write_text(body, encoding="ascii")
    print(f"{DST} <- export {len(names)}개")
    return 0

if __name__ == "__main__":
    sys.exit(main())
