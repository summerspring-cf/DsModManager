# vendor/ue4ss — 배포에 동봉하는 UE4SS 원본

`UE4SS_experimental.zip` 은 **손대지 않은 공식 배포본**이다. 여기서 압축을 풀거나
파일을 고치지 않는다. 패키징 스크립트가 zip 안에서 바로 읽어 배포본에 넣는다.

## 무엇인가

| 항목 | 값 |
|---|---|
| 프로젝트 | RE-UE4SS (UE4SS-RE/RE-UE4SS), MIT License, Copyright (c) 2022 Narknon |
| 채널 | experimental (실험) |
| 커밋 | `c838a8acaade1a0f860bdf249f039e58f4e10088` (2026-07-15) |
| zip SHA256 | `1a60989c29f94791f880f6e46130a7121533ffde753482572cba4a2e69f5fed9` (7,181,826 B) |
| `ue4ss/UE4SS.dll` | 16,519,168 B / SHA256 `d0107f63e567313cb6a15c505b5db2bdba38130964a04e019bda7611c6178022` |
| `dwmapi.dll` | 71,680 B / SHA256 `cfbd121b9e464b3ff35baba0f065d860aaffa7eb90f703748cd8e5b7730fa97e` |

## 왜 하필 이 빌드인가 (바꾸면 안 되는 이유)

우리 C++ 모드는 **이 DLL 의 export 목록에서 뽑은 `UE4SS.lib`** 로 링크된다
(`tools/make_ue4ss_def.py`). 스톡 릴리스(3.0.1)와 export 목록이 달라서, 다른 빌드
위에서는 모드 DLL 로드가 거부될 수 있다 — 게임은 정상 실행되고 모드만 안 뜬다.
그래서 "사용자가 알아서 받게" 두면 안 되고 검증된 이 판을 동봉한다.

실험 채널은 **굴러가는 빌드**라, 지금 GitHub 에서 받아도 c838a8ac 가 나온다는
보장이 없다. 이 파일이 그 커밋의 유일한 고정본이다. **지우지 말 것.**

## 재배포해도 되나

된다. MIT 라이선스가 재배포를 허용하고, 조건은 저작권/라이선스 전문 동봉뿐이다.
zip 안의 `ue4ss/LICENSE` 가 그 전문이고 배포본에 그대로 실린다.
같은 게임의 다른 모드(DSInstantDialogue)도 동일한 방식으로 UE4SS 를 동봉해 배포한다.

## 쓰는 곳

`tools/package_cppmm.py` 가 이 zip 을 열어 `UE4SS/` 접두어로 배포본에 옮겨 담고,
`install.bat` 이 게임의 `Binaries\Win64\` 에 **없는 파일만** 채워 넣는다.
이미 UE4SS 가 깔린 사용자의 파일은 하나도 건드리지 않는다.

## 원본 사본

`research/ue4ss_src/UE4SS_experimental.zip` 에 같은 파일이 있다(다운로드한 자리).
그쪽은 `.gitignore` 대상이라 새로 클론하면 사라진다 — 그래서 배포에 필요한
이 사본을 `vendor/` 에 따로 둔다.
