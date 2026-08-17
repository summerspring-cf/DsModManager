/*
  DsCppModManager v0.1 -- C++ 전용 모드매니저 1단계(실험):
  타이틀 메뉴에 "모드매니저C++" 항목을 삽입한다.

  목표(마일스톤): 메인 메뉴의 설정(Option)과 종료(Exit) 사이에
  "모드매니저C++" 항목이 보이는 것. 클릭 동작/패널은 2단계.

  구현 = Lua DsModManager 의 injectMenu 체인(umg.lua:2626-2764)을
  UObject::ProcessEvent 수동 호출로 전사한 것:
    1) FindAllOf("DUWG_TitleMenu_C") -> GetFullName 에 "/Game/" 이 없는
       것만 런타임 인스턴스 (템플릿 함정, umg.lua:195-226)
    2) "Exit" 항목의 GetParent() = VerticalBox_BotButtons (컨테이너)
    3) WidgetBlueprintLibrary CDO :Create(pc, 라이브GetClass, pc) 로 클론
       (StaticConstructObject 는 WidgetTree 가 null 이라 금지 -- umg.lua:18)
    4) TitleMenuType(바이트 enum, 네이티브 오프셋 0x338 실측) = 100 을
       AddChild **전에** 기록 -- 신품 클론은 0(=이어하기)이라 그대로 두면
       클릭 시 게임이 시작된다. 100 = enum 범위 밖 = BP 스위치 무동작.
    5) TitleText(DTextBlock) :SetText(FText) -- AddChild 전/후 2회
       (Construct 가 TitleMenuType 기준으로 라벨을 다시 쓰기 때문)
    6) AddChild(클론) -> RemoveChild(Exit) -> AddChild(Exit) 재부착으로
       "설정과 종료 사이" 순서 확보
    7) 마감: ImgSelected 는 항상 Visible(0), OverlaySelected 는 Collapsed(1),
       RenderOpacity 1.0, 형제 항목의 가시성 복사, 행 자체 SetVisibility(0)

  스레드 규율:
  - C++ on_update 는 게임 스레드가 아니다(UE4SS-UpdateThread 실측,
    MEMORY/fps 분석). 모든 UMG 접근은 ProcessEvent pre-hook 콜백 안에서
    IsInGameThread() 확인 후에만 한다.
  - 콜백 재진입: 우리가 부르는 ProcessEvent 도 훅을 다시 발화시키므로
    thread_local t_busy 로 차단.
  - 파일 로그는 짧은 append 이고 실패 스팸은 횟수 제한(D2 함정 예방).

  안전장치:
  - 모든 ProcessEvent / 원시 메모리 접근은 SEH 가드로 감싼다. AV 가 나면
    크래시 대신 로그를 남기고 영구 중단(g_hardFail)한다.
  - 모든 UFunction 은 호출 전에 GetParmsSize() 를 실측값과 대조한다.
    (파라미터 오프셋 가정이 어긋나는 빌드에서는 호출 자체를 포기)
  - 프로퍼티 오프셋은 하드코딩하지 않고 GetPropertyByNameInChain 으로
    런타임 해석한다(BP 추가 프로퍼티 ImgSelected 포함).

  출력: cppmm_log.txt (이 DLL 과 같은 폴더, append)
*/

#include "ue4ss_abi.hpp"

#include <windows.h>
#include <shellapi.h>
#include <xinput.h>
#pragma comment(lib, "xinput.lib")   // v0.40(pad): 정적 링크 -- LoadLibrary 금지(SECURITY.md 계약)
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")    // v0.50: 입력 진단 -- 레거시 조이스틱 API(XInput 밖 장치 식별)
#include <stdio.h>
#include <stdarg.h>

#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

using RC::Unreal::FProperty;
using RC::Unreal::UClass;
using RC::Unreal::UFunction;
using RC::Unreal::UObject;
namespace UOG = RC::Unreal::UObjectGlobals;

static HMODULE g_self = nullptr;

// ---------------------------------------------------------------- 로그 유틸

static void modulePath(wchar_t* out, const wchar_t* filename)
{
    out[0] = 0;
    HMODULE self = g_self;
    if (!self)
    {
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&modulePath), &self);
    }
    DWORD n = GetModuleFileNameW(self, out, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
    {
        out[0] = 0;
        return;
    }
    int cut = (int)n;
    while (cut > 0 && out[cut - 1] != L'\\') cut--;
    out[cut] = 0;
    lstrcatW(out, filename);
}

static void appendFile(const wchar_t* filename, const char* data, DWORD len)
{
    wchar_t path[MAX_PATH];
    modulePath(path, filename);
    if (!path[0]) return;
    HANDLE h = CreateFileW(path, FILE_APPEND_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD written;
    WriteFile(h, data, len, &written, nullptr);
    CloseHandle(h);
}

static void logf(const char* fmt, ...)
{
    char msg[768];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    SYSTEMTIME t;
    GetLocalTime(&t);
    char line[1024];
    int len = snprintf(line, sizeof(line),
                       "[DsCppMM] %04d-%02d-%02d %02d:%02d:%02d.%03d %s\r\n",
                       t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond,
                       t.wMilliseconds, msg);
    if (len > 0) appendFile(L"cppmm_log.txt", line, (DWORD)len);
}

static void utf8(const std::wstring& w, std::string& out)
{
    if (w.empty()) { out.clear(); return; }
    int need = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    out.resize(need);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), out.data(), need, nullptr, nullptr);
}

static std::string u8(const std::wstring& w)
{
    std::string s;
    utf8(w, s);
    return s;
}

// ------------------------------------------------ SEH 가드 (POD 인자 전용)
// C2712 규칙: __try 를 담는 함수에는 unwind 가 필요한 C++ 객체를 두지 않는다.

static bool peGuard(UObject* ctx, UFunction* fn, void* parms)
{
    __try
    {
        ctx->ProcessEvent(fn, parms);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

static bool readPtrGuard(void* base, int off, void** out)
{
    __try
    {
        *out = *reinterpret_cast<void**>(reinterpret_cast<char*>(base) + off);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

static bool readByteGuard(void* base, int off, unsigned char* out)
{
    __try
    {
        *out = *reinterpret_cast<unsigned char*>(reinterpret_cast<char*>(base) + off);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

static bool writeByteGuard(void* base, int off, unsigned char v)
{
    __try
    {
        *reinterpret_cast<unsigned char*>(reinterpret_cast<char*>(base) + off) = v;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

static bool writePtrGuard(void* base, int off, void* v)
{
    __try
    {
        *reinterpret_cast<void**>(reinterpret_cast<char*>(base) + off) = v;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

static bool writeIntGuard(void* base, int off, int v)
{
    __try
    {
        *reinterpret_cast<int*>(reinterpret_cast<char*>(base) + off) = v;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

static bool readBytesGuard(void* base, int off, void* out, int n)
{
    __try
    {
        memcpy(out, reinterpret_cast<char*>(base) + off, (size_t)n);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

/*
  리플렉션 "조회" SEH 썽크 -- v0.2 리뷰 확정 결함의 핵심 수정.
  peGuard 는 ProcessEvent 호출만 지켰는데, 죽은(해제된) 객체에 대한 첫 역참조는
  GetFunctionByNameInChain / GetPropertyByNameInChain 안에서 터진다(월드 전환 GC).
  /EHsc 의 catch(...)는 SEH AV 를 못 잡으므로 __try 로 직접 잡는다.
  (포인터 인자/반환뿐이라 C2712 에 안 걸린다. MSVC C++ 예외도 SEH 코드
   0xE06D7363 이라 함께 잡힌다 -- 어느 쪽이든 "죽은 객체 취급"이 정답.)
  반환: 0=정상, -2=SEH 폴트(죽은 객체 추정).
*/
static int sehGetFn(RC::Unreal::UObject* o, const wchar_t* name, RC::Unreal::UFunction** out)
{
    __try
    {
        *out = o->GetFunctionByNameInChain(name);
        return 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return -2;
    }
}

static int sehGetProp(RC::Unreal::UObject* o, const wchar_t* name, RC::Unreal::FProperty** out)
{
    __try
    {
        *out = o->GetPropertyByNameInChain(name);
        return 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return -2;
    }
}

static int sehPropOffSize(RC::Unreal::FProperty* p, int* off, int* sz)
{
    __try
    {
        *off = p->GetOffset_Internal();
        *sz = p->GetSize();
        return 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return -2;
    }
}

// v0.40 9차: 엔진 할당자 브리지 -- 미검증 익스포트 첫 호출은 반드시 SEH 뒤에서
// (전사 규율: 링크 성공은 ABI 증명이지 호출 안전 증명이 아니다)
static int sehEngineMalloc(unsigned long long n, void** out)
{
    __try
    {
        *out = RC::Unreal::FMemory::Malloc(n, 0);
        return 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return -2;
    }
}

static int sehEngineFree(void* p)
{
    __try
    {
        RC::Unreal::FMemory::Free(p);
        return 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return -2;
    }
}

// ---------------------------------------------------------------- 상태

static std::mutex g_mx;  // g_fnClass 보호 (콜백은 게임/로딩 스레드에서 온다)
static std::unordered_map<void*, unsigned char> g_fnClass;  // 1=타이틀메뉴 관련, 2=무관

static std::atomic<uint64_t> g_relevantHits{0};
static std::atomic<uint64_t> g_attempts{0};
static std::atomic<uint64_t> g_softRetries{0};
static std::atomic<void*> g_doneBox{nullptr};   // 삽입된 VerticalBox (로그/진단용, 판정에 쓰지 않음)
static std::atomic<void*> g_myClone{nullptr};   // 내 클론 (역참조 금지 -- FindAllOf 목록 멤버십으로만 생존 확인)
static std::atomic<bool> g_hardFail{false};     // 구조적 실패 -> 영구 중단
static std::atomic<int> g_cloneOrphans{0};      // 만들었지만 부착 실패한 클론 수 (누적 방지 안전판)
static std::atomic<int> g_anchorFails{0};       // Create 전 단계 연속 실패 (무한 재시도+로그 스팸 방지 래치)
static ULONGLONG g_lastTryMs = 0;               // 게임 스레드에서만 접근
static thread_local bool t_busy = false;        // 재진입 가드

// v0.40: 표시 언어 -- 0=한국어 1=English. -1=미확정(ensureLang 이 결정한다).
// 우선순위: dslang.txt(사용자 선택) > 게임 설정 LanguageText(0=한국어) > Windows UI 언어.
static int g_lang = -1;
static const wchar_t* TR(const wchar_t* ko, const wchar_t* en) { return g_lang == 1 ? en : ko; }
static const wchar_t* trLabel() { return TR(L"모드매니저", L"ModManager"); }  // 메뉴/패널 제목 (EN 도 붙여쓰기 -- 사용자 지정)
static void* g_langHs = nullptr;   // v0.40: [기본] 언어 콤보 알약 (히트 대상)
static void* g_langTx = nullptr;
static wchar_t g_langChoices[2][24] = {L"한국어(Korean)", L"English"};
static const wchar_t* const MOD_VER_W = L"v0.50";
static void* g_padIcon = nullptr;     // 11b: 클론 항목의 패드 Y 아이콘 위젯(SizeBox).
                                      // 패드 사용 중에만 보인다(키퍼가 가시성 토글).
static void* g_popupPadIcon = nullptr;  // v0.50: 팝업 확인버튼 (A) 아이콘 (패드 시만)
static void* g_padIconSpacer = nullptr; // v0.50b: Y아이콘 간격자(언어별 폭 갱신용)
// v0.50b: 게임 네이티브 확정 편승 -- 클론이 맨 아래일 때 게임/Slate 내비가 클론을
// 실제로 선택하고(라이브 확정 00:24), A/클릭 확정은 DTitleMenuUserWidget 의
// OnClickedButton(PE 실측)으로 온다. 포인터 비교로 잡아 패널을 연다.
static std::atomic<void*> g_fnItemClicked{nullptr};
static std::atomic<unsigned long long> g_cloneClickedMs{0};

/*
  ⚠ IsInGameThread() 크래시 실측 (2026-08-04 14:19, 콜스택 판독):
  이 설치본에서 RC::Unreal::IsInGameThread() 첫 호출이 C++ 예외를 던졌고
  (UE4SS 내부 전역/AOB 의존 추정), on_update 에 처리기가 없어 terminate ->
  "Abort signal received" 로 게임이 즉사했다. 미검증 익스포트는 전부 예외
  방벽 뒤에서만 부른다.
  고장(항상 예외)으로 판정되면 true 를 돌려 cls 게이트에 위임한다 --
  cls==1 은 "타이틀 UI 위젯 함수가 지금 이 스레드에서 실행 중"이라는 뜻이고
  UMG/Slate 함수는 엔진 계약상 게임 스레드에서만 돈다.
*/
// ---- v0.2 상태: 호버/클릭 펌프 + 패널 -------------------------------------
// 원칙: 아래 상태는 전부 게임 스레드에서만 쓰고 읽는다(펌프/삽입 모두 게임
// 스레드). atomic 인 것만 콜백 게이트에서 타 스레드가 읽는다.
static std::atomic<unsigned long> g_gameThreadId{0};        // INJECT_OK 순간 캡처(cls==1 = UI 함수 실행 중 = 게임 스레드)
static std::atomic<unsigned long long> g_lastTitleMs{0};    // cls==1 마지막 관측 시각 = 타이틀 메뉴 생존 신호
static ULONGLONG g_lastPumpMs = 0;
static ULONGLONG g_lastLiveScanMs = 0;
static bool g_lastHover = false;
static bool g_panelOpen = false;
static void* g_panel = nullptr;                    // 패널 호스트 UUserWidget (게임 스레드 전용)
static void* g_hsX[3] = {nullptr, nullptr, nullptr};  // X 버튼 히트스팟 후보(박스/보더/글자 -- 단독 Border 는 IsHovered 안 잡히는 실측 함정)
static void* g_hsBtn[3] = {nullptr, nullptr, nullptr};  // '폴더 바로가기' 버튼 히트스팟 후보

// v0.10: 플러그인이 dsplugin.ini 로 선언하는 옵션 (v0.16: 모드당 최대 16개)
struct PlgOpt
{
    char key[32];        // dsoptions.txt 의 키 (ASCII)
    wchar_t label[48];   // UI 라벨 (UTF-8 -> wide)
    // 0=bool 1=int(+choices 면 콤보) 2=key(VK) 3=color(0xRRGGBB)
    // 4=check(0/1) 5=button(누른 횟수) 6=slider(0~1000)          [v0.29]
    int type;
    int minV, maxV, step;
    int val;             // 현재값 (bool 은 0/1)
    // v0.13: 조건부(자식) 옵션 -- parent 키의 값이 조건을 만족할 때만 표시
    char parent[32];     // 부모 옵션 키 (빈 문자열 = 항상 표시)
    int parentValue;     // parent_value= 지정값
    bool hasParentValue; // false 면 "부모 != 0" 이 조건
    // v0.15: 콤보박스 -- choices= 가 있으면 스테퍼 대신 드롭다운. 값 = 0기준 인덱스
    int choiceN;             // 0 = 콤보 아님
    wchar_t choices[10][24]; // 항목 라벨 (UTF-8 -> wide)
    // UI 히트스팟 (패널 수명 동안만)
    void* boolOff;
    void* boolOn;
    void* boolOffText;
    void* boolOnText;
    void* hsDec;
    void* hsInc;
    void* valText;
    void* comboHs;   // 닫힘 콤보 알약 (히트 대상 Border)
    void* comboTx;   // 닫힘 콤보 값 텍스트
    void* swatch;      // v0.28: color 옵션의 색 견본 Border
    void* sliderFill;  // v0.29: 슬라이더 '채워진' 구간 SizeBox (폭으로 손잡이를 옮긴다)
    void* sliderRest;  // v0.29: '남은' 구간 SizeBox
    wchar_t btnCap[24];// v0.29: button= 캡션 (없으면 '실행')
    // v0.50: config.ini 브리지 -- 값 저장이 dsoptions 가 아니라 config.ini 로 감
    bool iniBacked;      // true = 이 옵션은 config.ini 에서 왔다
    bool iniHeader;      // true = 섹션 제목/안내 행 (type 7, 컨트롤 없음)
    char iniSection[24]; // config.ini 섹션명 (제자리 저장에 사용)
    int  iniOrigVal;     // v0.50: 로드 당시 값 -- 저장은 바뀐 키만(남의 줄 보존)
};

/* ======================= v0.28: 키 바인딩 · 색상 =========================
   모드가 config.ini 로 새던 두 가지를 패널에서 다룬다.
   - type=key   : 값 = 가상키코드(VK, 0=없음). 클릭 -> "키를 누르세요" -> 다음 입력을 저장.
                  Delete = 해제, ESC = 취소 (게임 키 변경 UI 관례).
   - type=color : 값 = 0xRRGGBB. 클릭 -> 팔레트/그라데이션/HEX 입력 창.
   저장 형식은 기존과 같은 정수라 dsoptions.txt 파서를 건드리지 않는다.
*/
// VK -> 사람이 읽는 이름. 확장키는 스캔코드에 확장 비트를 얹어야 이름이 맞다.
static void keyName(int vk, wchar_t* out, int cap)
{
    out[0] = 0;
    if (vk <= 0) { lstrcpynW(out, TR(L"(없음)", L"(none)"), cap); return; }
    if (vk == VK_LBUTTON) { lstrcpynW(out, TR(L"마우스 좌클릭", L"Left mouse button"), cap); return; }
    if (vk == VK_RBUTTON) { lstrcpynW(out, TR(L"마우스 우클릭", L"Right mouse button"), cap); return; }
    if (vk == VK_MBUTTON) { lstrcpynW(out, TR(L"마우스 휠클릭", L"Middle mouse button"), cap); return; }
    UINT sc = MapVirtualKeyW((UINT)vk, MAPVK_VK_TO_VSC);
    switch (vk)
    {
        case VK_INSERT: case VK_DELETE: case VK_HOME: case VK_END:
        case VK_PRIOR: case VK_NEXT: case VK_LEFT: case VK_RIGHT:
        case VK_UP: case VK_DOWN: case VK_DIVIDE: case VK_NUMLOCK:
            sc |= 0x100;  // 확장키
            break;
        default: break;
    }
    if (!sc || !GetKeyNameTextW((LONG)(sc << 16), out, cap) || !out[0])
        swprintf(out, cap, TR(L"키 %d", L"Key %d"), vk);
}

// 매니페스트에 F7 처럼 이름으로 적을 수 있게 (숫자도 허용)
static int keyFromName(const std::string& s)
{
    if (s.empty()) return 0;
    if (s[0] >= '0' && s[0] <= '9' && s.find_first_not_of("0123456789") == std::string::npos)
        return atoi(s.c_str());
    std::string u;
    for (char c : s) if (c != ' ') u += (char)toupper((unsigned char)c);
    if (u.size() >= 2 && u[0] == 'F')
    {
        int n = atoi(u.c_str() + 1);
        if (n >= 1 && n <= 24) return VK_F1 + n - 1;
    }
    if (u.size() == 1 && ((u[0] >= 'A' && u[0] <= 'Z') || (u[0] >= '0' && u[0] <= '9'))) return u[0];
    struct { const char* n; int vk; } T[] = {
        {"SPACE", VK_SPACE}, {"TAB", VK_TAB}, {"ENTER", VK_RETURN}, {"RETURN", VK_RETURN},
        {"SHIFT", VK_SHIFT}, {"CTRL", VK_CONTROL}, {"CONTROL", VK_CONTROL}, {"ALT", VK_MENU},
        {"INSERT", VK_INSERT}, {"DELETE", VK_DELETE}, {"HOME", VK_HOME}, {"END", VK_END},
        {"PAGEUP", VK_PRIOR}, {"PAGEDOWN", VK_NEXT}, {"LEFT", VK_LEFT}, {"RIGHT", VK_RIGHT},
        {"UP", VK_UP}, {"DOWN", VK_DOWN}, {"BACKSPACE", VK_BACK},
    };
    for (auto& t : T) if (u == t.n) return t.vk;
    return 0;
}

static int colorFromText(const std::string& s)
{
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '#')) ++i;
    if (i >= s.size()) return -1;
    // 6자리 16진수면 색, 아니면 10진수로
    std::string h = s.substr(i);
    if (h.size() >= 6 && h.find_first_not_of("0123456789abcdefABCDEF") >= 6)
        return (int)strtol(h.substr(0, 6).c_str(), nullptr, 16);
    return atoi(h.c_str());
}

static void hsvToRgb(float h, float s, float v, int* r, int* g, int* b)
{
    float c = v * s, x = c * (1 - fabsf(fmodf(h * 6.0f, 2.0f) - 1)), m = v - c;
    float rr = 0, gg = 0, bb = 0;
    int seg = (int)(h * 6.0f) % 6;
    if (seg == 0) { rr = c; gg = x; }
    else if (seg == 1) { rr = x; gg = c; }
    else if (seg == 2) { gg = c; bb = x; }
    else if (seg == 3) { gg = x; bb = c; }
    else if (seg == 4) { rr = x; bb = c; }
    else { rr = c; bb = x; }
    *r = (int)((rr + m) * 255 + 0.5f);
    *g = (int)((gg + m) * 255 + 0.5f);
    *b = (int)((bb + m) * 255 + 0.5f);
}

// v0.50: 옵션 배열 상한. dsplugin.ini 계약은 여전히 16개(문서화된 값)지만,
// config.ini 브리지는 섹션 제목행 + 키를 전부 노출하므로 배열은 넉넉히 잡는다.
// 정적 메모리 비용뿐(PlgRow ~55KB x 50 = ~2.8MB, 전부 static).
#define MM_MAX_OPT 64
// v0.6: 플러그인 토글 행 상태 (패널 수명 동안만 유효, 게임 스레드 전용)
struct PlgRow
{
    wchar_t name[64];   // 모드 폴더명 = **설치 이름** (Mods\<name>, dsorder 키)
    wchar_t rel[192];   // v0.20: plugins\ 기준 상대 경로 (중첩 배포판이면 여러 단계).
                        // 원본(plugins) 쪽 파일은 전부 이걸로 찾는다. 비어 있으면 name.
    wchar_t label[64];  // v0.11: 매니페스트 [plugin] name= 표시 이름 (없으면 빈 문자열 -> 폴더명 표시)
    void* offPill;  // '끄기' 알약 Border (vis 0 = 히트 대상)
    void* onPill;   // '켜기' 알약 Border
    void* offText;
    void* onText;
    bool on;
    bool pak;            // v0.27: pak(콘텐츠) 모드
    char pakTarget[16];  // dsplugin.ini pak_target= : 기본(빈값)=~mods(에셋) / logicmods=BP모드
    char rtkey[32];  // v0.10: 매니페스트 runtime_key (없으면 빈 문자열 -> 내장표)
    int optN;
    void* expandHs;   // v0.16: 옵션 '펼치기/접기' 행 히트스팟 (접힘 UI 없으면 null)
    bool iniBridge;   // v0.50: config.ini 브리지 모드 (저장이 config.ini 로 감)
    PlgOpt opt[MM_MAX_OPT];  // v0.16: 4->16, v0.50: 브리지 위해 MM_MAX_OPT
};
// v0.19.1: 표시 한도 8 -> 32. 옛 8 은 행이 단순하던 v0.5 시절의 임의값이었고
// (기술적 근거 없음), 실사용에서 정확히 8개에 도달했다. 초과분은 로그만 남기고
// 조용히 사라지므로 "넣었는데 안 보인다"의 두 번째 원인이 될 자리였다.
// 비용은 정적 메모리뿐(PlgRow ~12KB x 32 = ~400KB, 전부 static 이라 스택 무관).
#define MM_MAX_PLUGINS 50
static PlgRow g_plg[MM_MAX_PLUGINS];
static int g_plgN = 0;

// v0.7: 입력 래치 -- on_update(5ms, 항상 돎)가 엣지를 감지해 시각을 기록하고,
// 게임 스레드 펌프가 소비한다. PE 트래픽 기근으로 클릭을 놓치던 지연의 해결책.
static std::atomic<unsigned long long> g_pendClickMs{0};
static std::atomic<unsigned long long> g_pendEscMs{0};

// v0.8: 펌프 박동 계측 -- "버튼이 늦게 먹는" 문제의 실체(펌프 굶주림)를
// 로그로 증명/반증하기 위한 텔레메트리. 하트비트마다 출력 후 리셋.
static std::atomic<unsigned long long> g_pumpTicks{0};
static std::atomic<unsigned long long> g_pumpMaxGapMs{0};
static ULONGLONG g_lastPumpTickMs = 0;  // 게임 스레드 전용
// v0.8.2: 펄스 자체의 박동(콜백에서 context==클론 관측) -- 펄스 정지 vs
// 펌프 게이트 문제를 구분하는 결정적 계측
static std::atomic<unsigned long long> g_pulseTicks{0};
static std::atomic<unsigned long long> g_pulseMaxGapMs{0};
static std::atomic<unsigned long long> g_lastPulseMs{0};

// v0.7: 재시작 안내 팝업 상태 (게임 스레드 전용)
static void* g_popup = nullptr;
static void* g_hsPop[2] = {nullptr, nullptr};  // 확인/닫기 히트스팟
static bool g_popupOpen = false;

// v0.15: 콤보박스 드롭다운 상태 (게임 스레드 전용).
// 드롭다운 = 별도 뷰포트 호스트(ZOrder 1100) -- 패널 레이아웃을 밀지 않고
// 닫힘 알약 바로 아래에 겹쳐 뜬다(설정창 언어 콤보 실측 재현).
static bool g_comboOpen = false;
static void* g_comboHost = nullptr;
static int g_comboN = 0;
static void* g_comboItemHs[10];  // 항목 하이라이트 Border (히트+페인트 대상)
static void* g_comboItemTx[10];  // 항목 라벨
static void* g_comboItemMk[10];  // 우측 ◆ 마커 (호버 항목에만 표시)
static int g_comboHover = -1;
static int g_comboRow = -1;      // 콤보 소유자: -1 = 언어(매니저 자체), else g_plg 인덱스
static int g_comboOpt = 0;
static void* g_comboValTx = nullptr;  // 닫힘 알약의 값 텍스트 (선택 반영)
static void* g_comboAnchorPill = nullptr;  // 열림 동안 포커스 외곽선으로 바꾼 알약

// v0.28: 키 캡처 -- 켜지면 on_update 가 전체 가상키를 훑어 첫 입력을 잡는다.
// 평상시엔 스캔하지 않는다(매 틱 254회 조회는 낭비).
static std::atomic<bool> g_keyCapture{false};
static std::atomic<int> g_capturedVk{0};
static int g_keyCapRow = -1, g_keyCapOpt = -1;
// v0.28: 색상 선택창 상태
static bool g_colorOpen = false;
static void* g_colorHost = nullptr;
static int g_colorRow = -1, g_colorOpt = -1;
static void* g_colorSV = nullptr;      // 채도/명도 사각형 (히트 대상)
static void* g_colorHue = nullptr;     // 색조 띠 (히트 대상)
static void* g_colorSVBase = nullptr;  // 사각형 바탕(순색 틴트)
static void* g_colorPreview = nullptr; // 미리보기 견본
static void* g_colorHexTx = nullptr;   // #RRGGBB 표시
static void* g_colorSwatchHs[24];      // 프리셋 팔레트 히트스팟
static int g_colorSwatchRgb[24];
static int g_colorSwatchN = 0;
static float g_colorH = 0, g_colorS = 0, g_colorV = 1;  // 현재 HSV
static wchar_t g_hexBuf[8];            // HEX 타이핑 버퍼
static int g_hexLen = 0;

// v0.28: [기본] 탭 테스트 행 (세션 한정 값 -- 파일 계약과 무관한 UI 시연용)

// v0.16: 탭 + 순서(드래그 재배치) 상태 (게임 스레드 전용, 패널 수명 한정 포인터)
// ==================== v0.40(pad): 게임패드 ====================
// on_update(5ms)가 XInput 을 폴링해 엣지를 래치하고, 게임 스레드 펌프가 소비한다.
// B 버튼은 비트 없이 ESC 래치(g_pendEscMs)로 흘린다 -- 패널/콤보/색상창/팝업의
// ESC 경로 전부가 공짜로 패드 대응이 된다.
#define PAD_UP    0x01u
#define PAD_DOWN  0x02u
#define PAD_LEFT  0x04u
#define PAD_RIGHT 0x08u
#define PAD_A     0x10u
#define PAD_LB    0x20u
#define PAD_RB    0x40u
#define PAD_Y     0x80u
static std::atomic<unsigned> g_padEdges{0};
static std::atomic<ULONGLONG> g_padEdgeMs{0};   // 마지막 래치 시각 (신선도 400ms)
static std::atomic<bool> g_padPresent{false};
static std::atomic<ULONGLONG> g_lastPadNavMs{0};   // 마지막 방향 입력 시각 (메뉴 워프 게이트)
// 메뉴 선택 관측: 게임이 항목 선택을 옮길 때마다 BP_OnItemSelectionChanged(bool)
// 를 부른다(타이틀 PE 카탈로그 실측). 전역 프리훅이 포인터 비교로 잡아 기록한다.
static std::atomic<void*> g_fnSelChanged{nullptr};
static std::atomic<void*> g_fnFocusRecv{nullptr};   // 진단: 클론 포커스 수신 증명용
static std::atomic<void*> g_selEvtItem{nullptr};
static std::atomic<unsigned char> g_selEvtOn{0};
static std::atomic<void*> g_menuOption{nullptr};   // '설정' 항목 (클론 원형 sample)
static std::atomic<void*> g_menuExit{nullptr};     // '나가기' 항목
// v0.50: 위치 원복 -- 클론을 설정<->나가기 사이로. Exit 를 remove+readd 하면 게임이
// 로컬라이즈한 Exit 라벨이 위젯 기본값으로 되돌아간다(영어판 "나가기"·한국판 "Quit"
// 실측). 그래서 게임 로컬라이즈가 "안정"(라벨 무변화 1.2초)된 뒤에 재정렬하고, 그
// 순간의 라벨을 캡처해 복원한다. 이후 언어 전환은 게임이 제 항목을 재로컬라이즈하므로
// (Continue/Settings 가 전환 따라가는 실측) 1회 복원으로 충분하다.
static bool g_reorderPending = false;
static unsigned char g_reorderLastExit[24] = {0};
static bool g_reorderHaveLast = false;
static ULONGLONG g_reorderStableSince = 0;
// v0.50: 정렬 전 은신 -- 클론이 잠깐 맨 아래에 보였다가 위로 점프하는 증상 제거.
// finishClone 이 Collapsed(1) 로 숨기고, 재정렬 완료/실패/시간초과 때 Visible(0) 복귀.
// 시간초과는 벽시계가 아니라 **게이트(팝업·패널 닫힘)가 열린 틱 수**로 센다 --
// 라이브 실측(20:46): 안전모드 팝업을 읽는 2분 동안 벽시계가 다 흘러, 닫자마자
// 즉시 타임아웃 -> 맨 아래 표시됐다. 틱 카운트는 팝업이 열려 있으면 안 는다.
static int g_reorderOpenTicks = 0;
// v0.50(리뷰 C-2): RemoveChild(Exit) 성공 후 AddChild 가 실패하면 Exit(나가기)가
// 메뉴에서 떨어진 채 남는다 -- 붙을 때까지 틱마다 재시도하는 수리 모드.
static bool g_reorderRepair = false;
static unsigned char g_reorderSavedLabel[24] = {0};  // 떼기 직전 캡처한 Exit 라벨
static bool g_reorderHaveSaved = false;
// 패널 패드 내비게이션: openPanel 이 행을 만들며 등록하는 목록
struct NavItem
{
    int kind;        // NAVK_*
    void* outline;   // 선택 테두리 Border (평소 투명 -- 크림색이 켜지면 선택)
    void* rowBox;    // 스크롤 가시성 판정용
    int row, opt;    // g_plg / g_ord 인덱스 (해당할 때)
};
#define NAVK_FOLDER 0
#define NAVK_LANG   1
#define NAVK_MOD    2
#define NAVK_OPT    3
#define NAVK_FOLD   4
#define NAVK_ORD    5
static NavItem g_nav[600];
static int g_navN = 0;
static int g_navSel = -1;       // -1 = 패드 미사용 (테두리 없음)
static int g_padOrdLift = -1;   // 순서 탭: 집어든 행 (-1 = 없음)
static void* g_lastRowOutline = nullptr;   // addRow 가 만든 마지막 테두리 (등록용)
static void* g_lastRowBox = nullptr;
static std::atomic<int> g_inputMode{0};    // 0=키보드/마우스 1=패드. 마지막으로 쓴 장치.
                                           // 패드 UI(칩·힌트·선택 테두리)는 1일 때만 보인다.
static int g_comboPadSel = -1;             // 드롭다운 안 패드 선택 (-1 = 없음)
static void* g_chipBox[2] = {nullptr, nullptr};   // LB/RB 칩 SizeBox (표시 토글)
static void* g_padHintBox = nullptr;       // 하단 (A)선택 (B)닫기 힌트 컨테이너
static void* g_pcPanel = nullptr;          // 패널을 연 PlayerController (입력모드 복구용)
static bool g_navWired = false;            // v0.40 5차: 게임 메뉴 내비 링에 클론을 끼웠는가
static std::atomic<bool> g_padBHeld{false}; // v0.40 8차: 패드 B 물리 상태 (복구 지연용)
static bool g_restoreInputPending = false;  // v0.40 8차: B/ESC 를 놓으면 입력모드를 돌려준다
// v0.40 9차: 뿌리 레이어(DLayerTitleGame_C). 패드 선택은 뿌리가 내부 인덱스로
// 항목 목록(ListTitleMenuBtn TArray)을 순회하는 구조다 -- 포커스 아님(조사 확정,
// STATUS 9차 절). 배열에 클론을 편입하는 것이 진입의 정공 경로. 게임 스레드 전용.
static void* g_root = nullptr;              // DLayerTitleGame_C 인스턴스
static int g_rootArrOff = -1;               // ListTitleMenuBtn 오프셋 (런타임 해석)
static int g_rootSnapOff = -1;              // 계측 스냅샷 시작(Load_Anim 끝) 오프셋
static bool g_rootAdopted = false;          // 클론이 게임 항목 배열에 들어가 있는가
static int g_memSelfTest = 0;               // 0=미시도 1=통과 -1=실패(재할당 영구 봉인)
static std::atomic<void*> g_fnKeyDown{nullptr};   // 진단: UserWidget:OnKeyDown UFunction
static bool g_menuCloneSel = false;         // 9차b: 메뉴 클론 선택 상태 (게임 스레드 전용)
static int g_menuGen = 0;                   // 9차b: 메뉴 세대 -- cloneLost 마다 +1,
                                            // 폴링의 지역 정적 잔존(오발/허위 diff)을 리셋
static std::atomic<bool> g_padDirHeld{false};  // 10차: 방향(D패드) 물리 상태 (복구 지연용)
static bool g_restoreWaitDir = false;          // 10차: 지연 복구가 방향키 뗌도 기다리는가
static bool g_vstop = false;                   // 10차: 가상 정지 -- 클론을 '선택'으로 표시하고
                                               // 게임 입력을 UIOnly 로 동결한 상태 (편입 폴백)
static void* g_vstopGameSel = nullptr;         // 동결 순간 게임이 선택 중이던 항목 (복원용)
static const bool g_adoptEnabled = false;      // v0.50f: 편입(배열 부트스트랩/append) 홀드
                                               // -- 사용자 지시(2026-08-12). 라이브 5회
                                               // 시도에도 부팅 경로 순회에 반영 안 됨.
                                               // 재개 시 이 스위치만 켜면 전부 살아난다.
static const bool g_vstopEnabled = false;      // 10차f: 동결 방식 봉인 -- 게임과 경합해
                                               // 폭주(연속 자동 이동) 실측. 내비 그래프
                                               // 편입으로 전환한다. 코드는 참고용으로 유지.
static void* g_navObj = nullptr;               // 10차f: UDWidgetNavigation 인스턴스
static void* g_navGraph[4] = {};               // RoutingTable 값 (방향별 DWidgetGraph)
static int g_navGraphKey[4] = {};
static int g_navGraphN = 0;

static void navAdd(int kind, int row, int opt)
{
    if (g_navN >= 600) return;
    g_nav[g_navN].kind = kind;
    g_nav[g_navN].outline = g_lastRowOutline;
    g_nav[g_navN].rowBox = g_lastRowBox;
    g_nav[g_navN].row = row;
    g_nav[g_navN].opt = opt;
    ++g_navN;
}

static int g_activeTab = 0;                    // 0=모드 1=순서 (세션 동안 유지)
static void* g_hsTab[2] = {nullptr, nullptr};  // 탭 셀 히트스팟
struct OrderRow
{
    wchar_t name[64];   // 폴더명 (dsorder.txt 의 키) -- 내용물: 재배열 시 이동
    wchar_t label[64];  // 표시명 (없으면 폴더명 복사)
    void* band;         // 행 밴드 Border -- 슬롯: 위젯은 고정, 내용만 옮긴다
    void* text;         // 라벨 TextBlock
};
static OrderRow g_ord[MM_MAX_PLUGINS];
static int g_ordN = 0;
static int g_dragIdx = -1;       // 드래그 중인 행 (-1 = 없음)
static bool g_dragMoved = false;
static std::atomic<bool> g_lmbHeld{false};  // on_update 5ms 갱신 -- 드래그 유지 판정
// 뗌(버튼 up) 엣지 시각. 유지 상태만 보면 펌프가 굶은 사이의 "뗌 -> 재누름"을
// 못 보고 드래그가 이어져 엉뚱한 위치에 저장된다(리뷰 지적). 드래그 시작 때
// 이 값을 스냅샷해 두고, 값이 바뀌었으면 그 사이 뗐다는 뜻이라 즉시 종료한다.
static std::atomic<unsigned long long> g_lmbUpMs{0};
static unsigned long long g_dragUpSnap = 0;  // 게임 스레드 전용
// v0.29: 슬라이더 -- 게임 설정창 실측(트랙 540 / 손잡이 28 / 막대 8, 비율 유지)
static const float SLD_TRACK = 540.0f, SLD_KNOB = 28.0f, SLD_BAR = 8.0f;
static void* g_sldHs = nullptr;              // 잡고 있는 슬라이더의 히트 Border
static int g_sldRow = -1, g_sldOpt = -1;     // row<0 = 기본 탭 테스트 행
static unsigned long long g_sldUpSnap = 0;   // 펌프가 굶어 놓친 뗌 감지용

// v0.17: 뗌 엣지 래치 (누름과 별개로 소비) -- "누르는 순간"이 아니라 "눌렀다 뗐을
// 때"만 실행해야 하는 컨트롤(펼치기/접기)에 쓴다. 누른 채 끌어 스크롤하려던
// 동작이 곧바로 버튼으로 먹히던 문제의 해결책.
static std::atomic<unsigned long long> g_pendUpMs{0};

// v0.17: 스크롤 위치 사수 -- 패널 재구축(펼치기/옵션 변경)은 위젯을 새로 만들기
// 때문에 스크롤이 맨 위로 돌아간다. 재구축 전 오프셋을 기억했다가 새 패널에
// 되돌린다. 레이아웃이 잡히기 전엔 ScrollBox 가 값을 물지 않으므로(0 으로 클램프)
// 한 번 쏘고 끝내지 않고 목표에 닿을 때까지 짧게 재시도한다.
static void* g_scrollBox = nullptr;
static float g_pendScroll = -1.0f;
static ULONGLONG g_pendScrollUntilMs = 0;

// v0.18: 스크롤 UX -- 휠/드래그 스크롤 + 스크롤바 자동 숨김(1초)
static void* g_scrimW = nullptr;          // 패널 루트 스크림 = 뷰포트 사각형 조회원
static void* g_comboScrollBox = nullptr;  // 드롭다운 내부 ScrollBox (스크롤 필요할 때만)
// 자동 숨김: 오프셋이 바뀌면(휠 포함) 활동으로 보고 1초 뒤 숨긴다
static float g_sbLastOff[2] = {-1.0f, -1.0f};   // [0]=패널 [1]=콤보
static ULONGLONG g_sbHideAt[2] = {0, 0};
static bool g_sbShown[2] = {false, false};
// 드래그 스크롤 (내가 직접 구현 -- 이 빌드의 Slate 는 좌클릭 드래그 스크롤을 안 준다)
static void* g_dsBox = nullptr;      // 드래그 대상 ScrollBox
static int g_dsWhich = 0;            // 0=패널 1=콤보
static double g_dsStartY = 0;
static float g_dsStartOff = 0;
static bool g_dsActive = false;      // 임계값 초과 = 스크롤로 확정(= 클릭 취소)
// 클릭 대기: 누른 대상을 기억해 두고 **뗄 때 같은 대상 위에 있으면** 실행한다.
// 누른 채 끌면 스크롤이므로 클릭을 취소한다. (v0.17 펼치기 규약을 전체로 확장)
enum ArmKind
{
    ARM_NONE = 0, ARM_MOD_OFF, ARM_MOD_ON, ARM_OPT_OFF, ARM_OPT_ON,
    ARM_DEC, ARM_INC, ARM_COMBO_OPEN, ARM_FOLDER, ARM_FOLD, ARM_COMBO_ITEM,
    ARM_KEYBIND, ARM_COLOR_OPEN, ARM_CHECK, ARM_BUTTON, ARM_LANG
};
static int g_armKind = ARM_NONE;
static void* g_armHs = nullptr;
static int g_armRow = -1, g_armOpt = -1, g_armItem = -1;

static void clearArm()
{
    g_armKind = ARM_NONE;
    g_armHs = nullptr;
    g_armRow = g_armOpt = g_armItem = -1;
}

static void clearDragScroll()
{
    g_dsBox = nullptr;
    g_dsActive = false;
    g_dsWhich = 0;
}

// v0.7: 이번 세션에 UE4SS 가 실제 시작한 모드 스냅샷 (UE4SS.log 파싱, 패널 열 때 갱신)
// v0.50: 32 -> 96 (UE4SS 내장 ~10개와 상한을 나눠 쓰므로 플러그인 몫이 모자랐다
// -- 넘치면 sessionLoaded 오판 = '재시작 필요' 배지·팝업 오발)
static wchar_t g_loaded[96][64];
static int g_loadedN = 0;
static std::vector<void*> g_siblings;              // 런타임 메뉴 항목 캐시 (clearOthers 용)
static bool g_reflFault = false;                   // 이번 틱에 리플렉션 SEH 폴트 발생 (게임 스레드 전용)

static std::atomic<int> g_gtState{-1};  // -1 미확인 / 0 고장 / 1 정상
static bool gtGate()
{
    if (g_gtState.load(std::memory_order_relaxed) == 0) return true;
    try
    {
        bool r = RC::Unreal::IsInGameThread();
        g_gtState.store(1, std::memory_order_relaxed);
        return r;
    }
    catch (...)
    {
        g_gtState.store(0, std::memory_order_relaxed);
        logf("WARN IsInGameThread() C++ 예외 (실측 재현) -- 이후 cls 게이트만 사용");
        return true;
    }
}

// ---------------------------------------------------------------- 리플렉션 헬퍼
// 전부 게임 스레드(콜백 안)에서만 호출된다.

static bool wcontains(const std::wstring& s, const wchar_t* needle)
{
    return s.find(needle) != std::wstring::npos;
}

static UFunction* fnOf(UObject* o, const wchar_t* name, const char* tag)
{
    UFunction* fn = nullptr;
    if (sehGetFn(o, name, &fn) != 0)
    {
        logf("FAIL %s: '%s' 조회 중 SEH -- 죽은 객체 추정", tag, u8(name).c_str());
        g_reflFault = true;
        return nullptr;
    }
    if (!fn) logf("FAIL %s: UFunction '%s' 없음", tag, u8(name).c_str());
    return fn;
}

// ParmsSize 가 기대와 다르면 파라미터 오프셋 가정이 무효 -> 호출 포기.
// structural=true(핵심 단계)일 때만 영구 중단 플래그를 세운다 --
// 화장 단계(가시성/불투명도)의 불일치가 재삽입까지 막으면 안 된다.
static bool parmsExact(UFunction* fn, int expect, const char* tag, bool structural = true)
{
    int actual = (int)fn->GetParmsSize();
    if (actual != expect)
    {
        logf("%s %s: ParmsSize=%d (기대 %d) -- 레이아웃 가정 무효, 호출 포기",
             structural ? "FAIL" : "WARN", tag, actual, expect);
        if (structural) g_hardFail = true;
        return false;
    }
    return true;
}

struct PB
{
    alignas(16) unsigned char b[128];  // SetFont(FSlateFontInfo)=88B 까지 수용
    PB() { memset(b, 0, sizeof(b)); }
};

// 프로퍼티 오프셋 런타임 해석 (BP 추가 프로퍼티 포함)
static int propOffset(UObject* o, const wchar_t* name, int expectSize, const char* tag)
{
    FProperty* p = nullptr;
    if (sehGetProp(o, name, &p) != 0)
    {
        logf("FAIL %s: '%s' 프로퍼티 조회 중 SEH -- 죽은 객체 추정", tag, u8(name).c_str());
        g_reflFault = true;
        return -1;
    }
    if (!p)
    {
        logf("WARN %s: 프로퍼티 '%s' 없음", tag, u8(name).c_str());
        return -1;
    }
    int off = 0, sz = 0;
    if (sehPropOffSize(p, &off, &sz) != 0)
    {
        logf("FAIL %s: '%s' 오프셋 조회 중 SEH", tag, u8(name).c_str());
        g_reflFault = true;
        return -1;
    }
    if (off <= 0 || off > 0x8000)
    {
        logf("WARN %s: '%s' 오프셋 의심값 0x%X -- 사용 포기", tag, u8(name).c_str(), off);
        return -1;
    }
    if (expectSize > 0 && sz != expectSize)
    {
        logf("WARN %s: '%s' 크기 %d (기대 %d) -- 사용 포기", tag, u8(name).c_str(), sz, expectSize);
        return -1;
    }
    return off;
}

static UObject* readObjProp(UObject* o, const wchar_t* name, const char* tag)
{
    int off = propOffset(o, name, 8, tag);
    if (off < 0) return nullptr;
    void* v = nullptr;
    if (!readPtrGuard(o, off, &v))
    {
        logf("FAIL %s: '%s' 읽기 AV (off=0x%X)", tag, u8(name).c_str(), off);
        return nullptr;
    }
    return reinterpret_cast<UObject*>(v);
}

static bool setVisibility(UObject* widget, unsigned char vis, const char* tag)
{
    UFunction* fn = fnOf(widget, L"SetVisibility", tag);
    if (!fn || !parmsExact(fn, 1, tag, false)) return false;
    PB pb;
    pb.b[0] = vis;
    if (!peGuard(widget, fn, pb.b))
    {
        logf("FAIL %s: SetVisibility(%d) SEH 예외", tag, (int)vis);
        return false;
    }
    return true;
}

static int getVisibility(UObject* widget, const char* tag)
{
    UFunction* fn = fnOf(widget, L"GetVisibility", tag);
    if (!fn || !parmsExact(fn, 1, tag, false)) return -1;
    if ((int)fn->GetReturnValueOffset() != 0)
    {
        logf("WARN %s: GetVisibility retOff=%d (기대 0)", tag, (int)fn->GetReturnValueOffset());
        return -1;
    }
    PB pb;
    if (!peGuard(widget, fn, pb.b))
    {
        logf("FAIL %s: GetVisibility SEH 예외", tag);
        return -1;
    }
    return (int)pb.b[0];
}

static bool setRenderOpacity(UObject* widget, float v, const char* tag)
{
    UFunction* fn = fnOf(widget, L"SetRenderOpacity", tag);
    if (!fn || !parmsExact(fn, 4, tag, false)) return false;
    PB pb;
    memcpy(pb.b, &v, 4);
    if (!peGuard(widget, fn, pb.b))
    {
        logf("FAIL %s: SetRenderOpacity SEH 예외", tag);
        return false;
    }
    return true;
}

static float getRenderOpacity(UObject* widget, const char* tag, bool* ok)
{
    *ok = false;
    UFunction* fn = fnOf(widget, L"GetRenderOpacity", tag);
    if (!fn || !parmsExact(fn, 4, tag, false)) return 0.f;
    if ((int)fn->GetReturnValueOffset() != 0) return 0.f;
    PB pb;
    if (!peGuard(widget, fn, pb.b)) return 0.f;
    float v;
    memcpy(&v, pb.b, 4);
    *ok = true;
    return v;
}

// 라벨 설정: TitleText(DTextBlock) :SetText(FText 24바이트)
// FText 는 생성만 하고 파괴하지 않는다(의도적 미세 누수 -- ue4ss_abi.hpp 주석).
static bool setLabel(UObject* clone, const char* phase, bool strict = true)
{
    UObject* tt = readObjProp(clone, L"TitleText", "setLabel");
    if (!tt)
    {
        if (strict) logf("WARN setLabel(%s): TitleText 없음 -- 라벨 생략", phase);
        return false;
    }
    UFunction* fn = fnOf(tt, L"SetText", "setLabel");
    if (!fn || !parmsExact(fn, 24, "setLabel.SetText")) return false;
    if (RC::Unreal::FText::StaticSize() != 24)
    {
        if (!strict) return false;
        logf("FAIL setLabel: FText::StaticSize()=%d (기대 24) -- 전사 무효", RC::Unreal::FText::StaticSize());
        g_hardFail = true;
        return false;
    }
    RC::Unreal::FText txt(trLabel());
    PB pb;
    memcpy(pb.b, &txt, 24);
    if (!peGuard(tt, fn, pb.b))
    {
        if (!strict) return false;   // 키퍼(2초 주기)의 일시 SEH 로 hardFail 을 박지 않는다
        logf("FAIL setLabel(%s): SetText SEH 예외", phase);
        g_hardFail = true;
        return false;
    }
    if (strict) logf("OK setLabel(%s)", phase);
    return true;
}

// ---------------------------------------------------------------- 삽입 본체

// 소프트 실패(메뉴가 아직 준비 안 됨)는 스로틀로 재시도, 스팸은 횟수 제한
static void softRetry(const char* why)
{
    uint64_t n = ++g_softRetries;
    if (n <= 10 || n % 100 == 0)
        logf("RETRY(#%llu): %s", (unsigned long long)n, why);
}

static bool finishClone(UObject* box, UObject* exitW, UObject* sample, UObject* clone);

// Create 전(前) 단계의 실패: 구조적일 가능성이 높지만 일시적일 수도 있어
// 즉시 영구 중단하지 않고, 연속 10회면 래치를 건다 (D2 로그 스팸 함정 예방).
static void anchorFail(const char* what)
{
    int n = ++g_anchorFails;
    logf("FAIL %s (연속 %d/10)", what, n);
    if (n >= 10)
    {
        g_hardFail = true;
        logf("MM_RESULT: FAIL 준비 단계 실패 반복 -- 영구 중단");
    }
}

// ======================= v0.2: 헬퍼 =======================================

static UObject* findObj(const wchar_t* path, const char* tag)
{
    UObject* o = UOG::StaticFindObject_InternalSlow(nullptr, nullptr, path, false);
    if (!o) logf("WARN %s: StaticFindObject '%s' 실패", tag, u8(path).c_str());
    return o;
}

// 구조체/스칼라 인자 1개짜리 UFunction 호출 (바이트 그대로 복사). 화장 단계 전용.
static bool callBytes(UObject* obj, const wchar_t* fnName, const void* data, int size, const char* tag)
{
    UFunction* fn = fnOf(obj, fnName, tag);
    if (!fn || !parmsExact(fn, size, tag, false)) return false;
    if (size > (int)sizeof(PB::b)) { logf("WARN %s: %d바이트는 버퍼 초과", tag, size); return false; }
    PB pb;
    if (size > 0) memcpy(pb.b, data, (size_t)size);
    if (!peGuard(obj, fn, pb.b))
    {
        logf("FAIL %s: %s SEH 예외", tag, u8(fnName).c_str());
        return false;
    }
    return true;
}

struct LinColor { float r, g, b, a; };

// 0xRRGGBB -> UMG 색 (이 빌드의 브러시는 sRGB 값을 그대로 받는다 -- 토글/밴드와 동일 규약)
static LinColor rgbToLin(int rgb)
{
    LinColor c;
    c.r = ((rgb >> 16) & 0xFF) / 255.0f;
    c.g = ((rgb >> 8) & 0xFF) / 255.0f;
    c.b = (rgb & 0xFF) / 255.0f;
    c.a = 1.0f;
    return c;
}

struct FStringRaw { const wchar_t* data; int num; int max; };  // FString 16B 실측 레이아웃

// <Mods>\DsCppModManager\ 루트 절대경로 (dlls\ 의 부모)
static void modRootPath(wchar_t* out)
{
    modulePath(out, L"");
    size_t n = wcslen(out);
    const wchar_t* suffix = L"dlls\\";
    size_t sn = wcslen(suffix);
    if (n >= sn && _wcsicmp(out + n - sn, suffix) == 0) out[n - sn] = 0;
}

// <Mods>\DsCppModManager\Assets\<file> 절대경로 (dlls\ 의 형제 폴더)
static void assetPath(wchar_t* out, const wchar_t* file)
{
    modRootPath(out);
    lstrcatW(out, L"Assets\\");
    lstrcatW(out, file);
}

// 플러그인 폴더(<Mods>\DsCppModManager\plugins) 를 만들고 탐색기로 연다.
// '폴더 바로가기' 버튼의 동작. ShellExecuteW 는 비동기 실행이라 게임 스레드 안전.
static void openPluginsFolder()
{
    static wchar_t dir[MAX_PATH * 2];
    modRootPath(dir);
    lstrcatW(dir, L"plugins");
    CreateDirectoryW(dir, nullptr);  // 이미 있으면 무해
    HINSTANCE rc = ShellExecuteW(nullptr, L"open", dir, nullptr, nullptr, SW_SHOWNORMAL);
    logf("plugins 폴더 열기: %s (rc=%lld)", u8(dir).c_str(), (long long)(INT_PTR)rc);
}

// ---- v0.6: 플러그인 켬/끔의 파일 계층 ---------------------------------------
// 로드는 UE4SS 가 게임 시작 시 Mods\ 를 스캔해서 한다. 따라서:
//   켬 = Mods\<이름> 이 없으면 plugins 에서 복사 + enabled.txt 생성
//        (enabled.txt 는 mods.txt 값과 무관하게 모드를 시작시킨다 -- UE4SSProgram.cpp:1155)
//   끔 = enabled.txt 삭제 + mods.txt 의 "<이름> : 1" 을 0 으로 (있을 때만, 최초 1회 백업)

// 경로에서 상위로 n 단계 (끝 역슬래시 유지)
static void upDirs(wchar_t* p, int n)
{
    for (int k = 0; k < n; ++k)
    {
        size_t len = wcslen(p);
        if (len && p[len - 1] == L'\\') p[--len] = 0;
        while (len && p[len - 1] != L'\\') p[--len] = 0;
    }
}

static void gameModsRoot(wchar_t* out)  // "<...>\ue4ss\Mods" + 끝 역슬래시
{
    modRootPath(out);  // "...\Mods\DsCppModManager" + 끝 역슬래시
    size_t n = wcslen(out);
    if (n && out[n - 1] == L'\\') out[--n] = 0;
    while (n && out[n - 1] != L'\\') out[--n] = 0;
}

// plugins\<상대경로> 절대 경로 만들기 -- 원본(plugins) 쪽 파일 접근의 단일 경로
// 생성기. rel 은 중첩 배포판이면 여러 단계일 수 있다(v0.20 깊이 탐색).
static void pluginSrcPath(wchar_t* out, const wchar_t* rel)
{
    modRootPath(out);
    lstrcatW(out, L"plugins\\");
    lstrcatW(out, rel);
}

static bool pathExistsW(const wchar_t* p)
{
    return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES;
}

/* ============ v0.50: 입력 진단 로그 (inputlog\YYYYMMDD.log) ==================
   "모드매니저를 깔면 스팀 컨트롤러를 인식 못 한다" 제보(2026-08-16) 조사용.
   매니저 코드에는 게임의 패드 인식을 끊을 경로가 없다(XInput '읽기'만 하고
   XInputEnable·입력 훅·RawInput 등록이 없다) -- 그래서 **무엇이 실제로 들어오고
   있는지**를 기록해 판단 근거를 만든다.

   파일 이름 규칙 (사용자 지시): 세션 시작 때 그날 날짜로 정하고 **그대로 고정**.
   자정을 넘겨도 같은 파일에 이어 쓰고(한 세션 = 한 파일), 게임을 껐다 새로 켜면
   그때 날짜로 새 파일. 같은 날 여러 번 켜면 같은 파일에 이어 붙는다.

   남기는 것: 전경 여부 · XInput 슬롯/능력(가상패드 식별) · 버튼/스틱/트리거 변화 ·
   패킷 번호(장치가 살아 있는지) · 키보드 키 · 마우스 버튼 · 레거시 조이스틱 목록
   (장치 이름이 나온다 -- 스팀 입력이 XInput 밖으로 내보내는 구성이면 여기 잡힌다).
   부담 방지: 타이틀 화면에서만 · 변화가 있을 때만 · 세션당 줄 수 상한.
   끄려면 매니저 폴더에 inputlog_off.txt 를 만든다. */
static bool g_inLogInit = false;
static bool g_inLogOff = false;
static wchar_t g_inLogFile[MAX_PATH * 2] = {0};
static int g_inLogN = 0;
static const int IN_LOG_MAX = 20000;   // 세션 상한 (약 1~2MB)
// 리뷰 D1: 키 '이름'을 남기는 것은 inputlog_keys.txt 가 있을 때만 (옵트인).
// 기본은 초당 **횟수만** 집계한다 -- "키보드 신호가 오고 있다"는 진단에는 그것으로
// 충분하고, 스팀 오버레이 채팅 같은 내용이 남을 여지가 사라진다.
static bool g_inLogKeyNames = false;
static int g_inLogKeyN = 0;                 // 리뷰 D7: 키 예산을 따로 -- 패드 진단이
static const int IN_LOG_KEY_MAX = 4000;     // 키 폭주에 밀려 끊기지 않게

static void inputLogOpen()
{
    if (g_inLogInit) return;
    g_inLogInit = true;
    wchar_t p[MAX_PATH * 2];
    modRootPath(p);
    // 리뷰 D4: modulePath 실패(경로 260자 초과 등)면 빈 문자열이 온다 -- 그대로 쓰면
    // 게임 프로세스의 **현재 작업 디렉터리**에 폴더를 만들고 로그를 흘린다.
    // "전부 게임 폴더 안에만" 약속을 깨므로 그때는 아예 기록하지 않는다.
    if (!p[0])
    {
        g_inLogOff = true;
        logf("inputlog: 모듈 경로 확인 실패 -- 입력 진단 기록을 하지 않는다");
        return;
    }
    lstrcatW(p, L"inputlog_off.txt");
    if (pathExistsW(p))
    {
        g_inLogOff = true;
        logf("inputlog: inputlog_off.txt 가 있어 입력 진단 기록을 하지 않는다");
        return;
    }
    // 리뷰 D1: 키 '이름'까지 남기는 것은 옵트인. 기본은 익명 집계(횟수)만 --
    // 스팀 오버레이(Shift+Tab)는 별도 창을 만들지 않아 게임이 계속 전경이고,
    // GetAsyncKeyState 는 물리 상태라 오버레이 채팅 타자까지 읽힌다.
    modRootPath(p);
    lstrcatW(p, L"inputlog_keys.txt");
    g_inLogKeyNames = pathExistsW(p);
    modRootPath(p);
    lstrcatW(p, L"inputlog");
    CreateDirectoryW(p, nullptr);
    SYSTEMTIME t;
    GetLocalTime(&t);
    swprintf(g_inLogFile, MAX_PATH * 2, L"%s\\%04u%02u%02u.log", p,
             (unsigned)t.wYear, (unsigned)t.wMonth, (unsigned)t.wDay);
    // 리뷰 D3-①: 같은 날 재실행마다 이어 붙으므로 파일이 무한정 커질 수 있다.
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (GetFileAttributesExW(g_inLogFile, GetFileExInfoStandard, &fa))
    {
        ULONGLONG sz = ((ULONGLONG)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
        if (sz > 8ull * 1024 * 1024)
        {
            g_inLogOff = true;
            logf("inputlog: 오늘 파일이 이미 %llu MB -- 이번 실행은 기록하지 않는다",
                 (unsigned long long)(sz / (1024 * 1024)));
            return;
        }
    }
    // 리뷰 D3-②: 보존 기간 -- 14일 넘은 날짜 파일은 지운다 (무한 축적 방지)
    {
        wchar_t pat[MAX_PATH * 2];
        swprintf(pat, MAX_PATH * 2, L"%s\\*.log", p);
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW(pat, &fd);
        if (h != INVALID_HANDLE_VALUE)
        {
            FILETIME ftNow;
            GetSystemTimeAsFileTime(&ftNow);
            ULONGLONG now100 = ((ULONGLONG)ftNow.dwHighDateTime << 32) | ftNow.dwLowDateTime;
            const ULONGLONG keep = 14ull * 24 * 60 * 60 * 10000000ull;
            int cut = 0;
            do
            {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                ULONGLONG w = ((ULONGLONG)fd.ftLastWriteTime.dwHighDateTime << 32) |
                              fd.ftLastWriteTime.dwLowDateTime;
                if (!w || now100 <= w || now100 - w <= keep) continue;
                wchar_t old[MAX_PATH * 2];
                swprintf(old, MAX_PATH * 2, L"%s\\%s", p, fd.cFileName);
                if (DeleteFileW(old)) ++cut;
            } while (FindNextFileW(h, &fd));
            FindClose(h);
            if (cut) logf("inputlog: 14일 지난 기록 %d개 정리", cut);
        }
    }
    logf("inputlog: 입력 진단 기록 -> %s (세션 시작 날짜로 고정, 키 이름 기록=%d)",
         u8(g_inLogFile).c_str(), (int)g_inLogKeyNames);
}

// ⚠ UpdateThread 전용 (on_update). 다른 스레드에서 부르지 말 것 -- 카운터가 평범한 int 다.
static void inputLog(const char* fmt, ...)
{
    if (g_inLogOff) return;
    if (!g_inLogInit) inputLogOpen();
    if (g_inLogOff || !g_inLogFile[0]) return;
    if (g_inLogN >= IN_LOG_MAX)
    {
        if (g_inLogN == IN_LOG_MAX)
        {
            ++g_inLogN;   // 한 번만 알린다
            logf("inputlog: 세션 상한(%d줄) 도달 -- 이후 기록 생략", IN_LOG_MAX);
        }
        return;
    }
    ++g_inLogN;
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    SYSTEMTIME t;
    GetLocalTime(&t);
    char line[640];
    int n = snprintf(line, sizeof(line), "%02u:%02u:%02u.%03u %s\r\n",
                     (unsigned)t.wHour, (unsigned)t.wMinute, (unsigned)t.wSecond,
                     (unsigned)t.wMilliseconds, msg);
    if (n <= 0) return;
    if (n > (int)sizeof(line) - 1) n = (int)sizeof(line) - 1;   // snprintf 는 '원래 길이'를 준다
    HANDLE h = CreateFileW(g_inLogFile, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD wr = 0;
    WriteFile(h, line, (DWORD)n, &wr, nullptr);
    CloseHandle(h);
}

/* ====== v0.50: 보조 XInput -- **게임과 같은 눈으로** 패드를 본다 ==============
   실측(2026-08-16, 리포터 크래시 덤프의 모듈 목록):
     게임 = C:\Windows\System32\**XINPUT1_3.dll**  (exe 문자열에도 XINPUT1_3.dll)
     매니저 = C:\Windows\System32\**XINPUT1_4.dll** (정적 링크 xinput.lib)
   한 프로세스에 두 개가 나란히 올라와 있다. 스팀 입력(Steam Input)이 컨트롤러를
   에뮬레이트해 주는 경로가 **게임이 쓰는 DLL 쪽에만** 걸려 있으면, 게임은 패드가
   되는데 매니저만 "장치 없음"이 된다 — 제보 문장과 정확히 같은 그림이다.
   그래서 기본(1_4)이 아무것도 못 보면 **게임이 이미 로드해 둔** XInput 으로도
   물어본다. 어느 쪽이 답했는지는 로그에 남긴다.
   ⚠ SECURITY.md 계약: LoadLibrary 를 쓰지 않는다 — `GetModuleHandleW` 는 **이미
   로드된** 모듈만 집는다(새 DLL 을 불러오지 않는다). 없으면 그냥 포기한다.
   ⚠ 미검증 함수 포인터의 첫 호출이므로 SEH 뒤에서 부른다(전사 규율). */
typedef DWORD(WINAPI* PFN_XInputGetState)(DWORD, XINPUT_STATE*);
static PFN_XInputGetState g_xiAlt = nullptr;
static const wchar_t* g_xiAltName = nullptr;
static bool g_xiAltTried = false;

static void xiAltResolve()
{
    if (g_xiAltTried) return;
    g_xiAltTried = true;
    static const wchar_t* const CANDS[] = {
        L"XINPUT1_3.dll", L"XINPUT9_1_0.dll", L"XINPUT1_2.dll", L"XINPUT1_1.dll"
    };
    for (const wchar_t* nm : CANDS)
    {
        HMODULE h = GetModuleHandleW(nm);   // 이미 로드된 것만 (LoadLibrary 아님)
        if (!h) continue;
        FARPROC p = GetProcAddress(h, "XInputGetState");
        // 이름이 없으면 서수 100 = XInputGetStateEx (문서 외, 가이드 버튼까지 준다.
        // 구조체 레이아웃은 XINPUT_STATE 와 같다 -- 게임들이 흔히 쓰는 경로)
        if (!p) p = GetProcAddress(h, (LPCSTR)(uintptr_t)100);
        if (!p) continue;
        g_xiAlt = (PFN_XInputGetState)p;
        g_xiAltName = nm;
        logf("pad: 보조 XInput 확보 -- %s (게임이 이미 로드해 둔 것)", u8(nm).c_str());
        return;
    }
    logf("pad: 보조 XInput 없음 (XINPUT1_3/9_1_0 미로드)");
}

// ⚠ 오브젝트 소멸자가 없는 함수여야 __try 가 성립한다(C2712)
static DWORD xiAltGetState(DWORD idx, XINPUT_STATE* st)
{
    if (!g_xiAlt) return ERROR_DEVICE_NOT_CONNECTED;
    __try
    {
        return g_xiAlt(idx, st);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        g_xiAlt = nullptr;   // 한 번이라도 튀면 영구 포기
        return ERROR_DEVICE_NOT_CONNECTED;
    }
}

// 레거시 조이스틱 목록 (XInput 에 안 잡히는 장치를 이름으로 식별). 반환 = 발견 수.
static int inputLogJoysticks()
{
    // 리뷰 D5: 꺼져 있으면 열거 자체를 하지 않는다 ("끄면 아무것도 안 한다" 계약)
    if (!g_inLogInit) inputLogOpen();
    if (g_inLogOff) return 0;
    UINT nd = joyGetNumDevs();
    if (nd > 8) nd = 8;   // 열거 비용 상한
    int found = 0;
    for (UINT i = 0; i < nd; ++i)
    {
        JOYINFOEX ji;
        memset(&ji, 0, sizeof(ji));
        ji.dwSize = sizeof(ji);
        ji.dwFlags = JOY_RETURNBUTTONS | JOY_RETURNX | JOY_RETURNY;
        if (joyGetPosEx(i, &ji) != JOYERR_NOERROR) continue;
        ++found;
        JOYCAPSW jc;
        memset(&jc, 0, sizeof(jc));
        if (joyGetDevCapsW(i, &jc, sizeof(jc)) == JOYERR_NOERROR)
        {   // szPname 은 드라이버가 종단 NUL 없이 32자를 채울 수 있다 -- 길이를 못박는다
            std::wstring nm(jc.szPname, wcsnlen(jc.szPname, 32));
            inputLog("joy[%u]: '%s' 버튼=%u 축=%u (VID=%04X PID=%04X)", i,
                     u8(nm).c_str(), jc.wNumButtons, jc.wNumAxes, jc.wMid, jc.wPid);
        }
        else
            inputLog("joy[%u]: 연결됨 (이름 조회 실패)", i);
    }
    if (!found) inputLog("joy: 레거시 조이스틱 API 에도 장치 없음 (검사 %u개)", nd);
    return found;
}

/* v0.50: 환경 지문 -- 세션당 1회, 전부 **읽기 전용**. 이 네 덩어리면 "스팀 컨트롤러가
   왜 안 보이나"가 사용자 로그 하나로 갈린다 (조사 2026-08-17 권고).
   ① 스팀 오버레이 주입 여부 -- 없으면 스팀을 거치지 않은 실행이거나 오버레이 꺼짐.
      Windows 에서 에뮬 패드는 **오버레이가 만든다**. 없으면 패드도 없다(원인 확정).
   ② SteamAppId 등 환경변수 -- 스팀이 띄운 프로세스인지.
   ③ SteamVirtualGamepadInfo -- 스팀이 이 프로세스에 광고하는 가상 패드 목록
      (슬롯/이름/VID/PID 가 그대로 들어 있다. SDL 이 실제로 읽는 경로).
   ④ 로드된 XInput 모듈 + **훅 지문** -- 함수 앞 바이트가 E9/FF25(점프)면 스팀이
      훅한 것, 원본 프롤로그면 미훅. "우리 1_4 는 미훅인데 게임 1_3 은 훅됨"이
      보이면 원인 확정. */
static std::string readFileA(const wchar_t* path);   // 정의는 아래 (파일 순서 때문)

static void inputLogEnvFingerprint()
{
    // ① 오버레이
    {
        static const wchar_t* const OV[] = {L"GameOverlayRenderer64.dll", L"GameOverlayRenderer.dll"};
        bool any = false;
        for (const wchar_t* n : OV)
            if (GetModuleHandleW(n)) { any = true; inputLog("환경: 스팀 오버레이 주입됨 (%s)", u8(n).c_str()); }
        if (!any)
            inputLog("환경: ⚠ 스팀 오버레이가 주입되지 않았다 -- 스팀을 거치지 않고 실행했거나 "
                     "오버레이가 꺼져 있다. 이 경우 스팀 컨트롤러는 어떤 게임에도 패드로 보이지 않는다");
    }
    // ② 스팀 환경변수
    {
        static const wchar_t* const EV[] = {L"SteamAppId", L"SteamGameId", L"SteamOverlayGameId"};
        wchar_t buf[128];
        bool any = false;
        for (const wchar_t* n : EV)
            if (GetEnvironmentVariableW(n, buf, 128))
            { any = true; inputLog("환경: %s=%s", u8(n).c_str(), u8(buf).c_str()); }
        if (!any) inputLog("환경: ⚠ 스팀 환경변수 없음 -- 스팀이 띄운 프로세스가 아니다");
    }
    // ③ 스팀 가상 패드 광고 파일
    {
        wchar_t p[MAX_PATH * 2];
        DWORD n = GetEnvironmentVariableW(L"SteamVirtualGamepadInfo", p, MAX_PATH * 2);
        if (n && n < MAX_PATH * 2)
        {
            inputLog("환경: SteamVirtualGamepadInfo=%s", u8(p).c_str());
            std::string d = readFileA(p);
            if (d.empty()) inputLog("환경: 가상패드 목록 파일이 비었거나 못 읽음");
            else
            {
                size_t pos = 0;
                int lines = 0;
                while (pos < d.size() && lines < 40)
                {
                    size_t eol = d.find('\n', pos);
                    if (eol == std::string::npos) eol = d.size();
                    std::string ln = d.substr(pos, eol - pos);
                    pos = eol + 1;
                    while (!ln.empty() && (ln.back() == '\r' || ln.back() == ' ')) ln.pop_back();
                    if (ln.empty()) continue;
                    ++lines;
                    inputLog("  가상패드| %.200s", ln.c_str());
                }
            }
        }
        else inputLog("환경: SteamVirtualGamepadInfo 없음 (스팀이 가상 패드를 광고하지 않는다)");
    }
    // ④ XInput 모듈 + 훅 지문
    {
        static const wchar_t* const XI[] = {
            L"XINPUT1_4.dll", L"XINPUT1_3.dll", L"XINPUT9_1_0.dll", L"XINPUT1_2.dll", L"XINPUT1_1.dll"
        };
        for (const wchar_t* n : XI)
        {
            HMODULE h = GetModuleHandleW(n);
            if (!h) { inputLog("환경: %s 미로드", u8(n).c_str()); continue; }
            FARPROC p = GetProcAddress(h, "XInputGetState");
            if (!p) p = GetProcAddress(h, (LPCSTR)(uintptr_t)2);
            if (!p) { inputLog("환경: %s 로드됨 (XInputGetState 없음)", u8(n).c_str()); continue; }
            unsigned char b[8] = {0};
            memcpy(b, (const void*)p, 8);   // 코드 페이지 읽기 (실행 가능 = 읽기 가능)
            bool hooked = (b[0] == 0xE9) || (b[0] == 0xFF && b[1] == 0x25) ||
                          (b[0] == 0xEB) || (b[0] == 0x48 && b[1] == 0xB8 && b[2] == 0x00);
            inputLog("환경: %s 로드됨 진입부=%02X %02X %02X %02X %02X %02X %02X %02X -> %s",
                     u8(n).c_str(), b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
                     hooked ? "훅 걸림(스팀 등이 가로챔)" : "원본(훅 없음)");
        }
    }
}

/* 전경/타이틀 전환 + 키보드·마우스 입력 기록.
   ⚠ 기록 범위를 **타이틀 화면 + 게임 창이 앞에 있을 때**로 못박는다:
   인게임 조작이나 다른 창에 치는 타자는 남기지 않는다(그럴 이유도 없고, 남기면
   안 된다). 타이틀에는 글자 입력 칸이 없어 개인적인 내용이 찍힐 여지도 없다.
   남기는 것은 키 '이름'뿐이고, 매니저 폴더의 로그 파일 밖으로 나가지 않는다. */
static void inputDiagTick(bool fgOurs, bool atTitle)
{
    static bool s_first = true, s_lastFg = false, s_lastTitle = false;
    static ULONGLONG s_lastKbMs = 0;
    static unsigned char s_kbPrev[256] = {0};
    if (g_inLogOff) return;
    ULONGLONG now = GetTickCount64();
    if (atTitle != s_lastTitle)
    {
        s_lastTitle = atTitle;
        if (atTitle)
        {
            if (s_first)
            {
                s_first = false;
                inputLog("================ 세션 시작 (모드매니저 %s) ================",
                         u8(MOD_VER_W).c_str());
                inputLog("기록 범위: 타이틀 화면 + 게임 창이 앞에 있을 때만. 끄려면 매니저 "
                         "폴더에 inputlog_off.txt 를 만드세요.");
                inputLogEnvFingerprint();   // v0.50: 세션 1회 환경 지문 (읽기 전용)
                inputLogJoysticks();
            }
            inputLog("--- 타이틀 진입 ---");
        }
        else inputLog("--- 타이틀 이탈 (게임 로딩/종료) ---");
        memset(s_kbPrev, 0, sizeof(s_kbPrev));   // 경계에서 유령 엣지 방지
    }
    if (!atTitle) return;
    if (fgOurs != s_lastFg)
    {
        s_lastFg = fgOurs;
        inputLog("창 포커스: %s%s", fgOurs ? "게임 앞으로" : "게임 뒤로",
                 fgOurs ? "" : " (이 동안의 입력은 기록하지 않음)");
        if (!fgOurs) memset(s_kbPrev, 0, sizeof(s_kbPrev));
    }
    if (!fgOurs) return;
    if (g_inLogKeyN >= IN_LOG_KEY_MAX) return;   // 리뷰 D7: 키 예산 소진 -- 패드 진단은 계속
    if (now - s_lastKbMs < 50) return;   // 50ms 간격 (전체 가상키 훑기 비용 억제)
    s_lastKbMs = now;
    static ULONGLONG s_aggUntil = 0;
    static int s_aggDown = 0, s_aggUp = 0;
    for (int vk = 0x01; vk <= 0xFE; ++vk)
    {
        bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
        if (down == (s_kbPrev[vk] != 0)) continue;
        s_kbPrev[vk] = down ? 1 : 0;
        if (g_inLogKeyNames)
        {   // 옵트인: 키 이름까지 (깊은 진단용 -- inputlog_keys.txt)
            ++g_inLogKeyN;
            wchar_t kn[64];
            keyName(vk, kn, 64);
            inputLog("%s vk=0x%02X (%s)", down ? "키 누름" : "키 뗌 ", vk, u8(kn).c_str());
        }
        else
        {   // 기본: 무엇을 눌렀는지는 남기지 않고 **횟수만** 1초 단위로 모아 적는다.
            // "키보드 신호가 오고 있다"(스팀 입력이 패드를 키보드로 바꾸는 구성)를
            // 판별하는 데는 이것으로 충분하다.
            if (down) ++s_aggDown; else ++s_aggUp;
            if (!s_aggUntil) s_aggUntil = now + 1000;
        }
    }
    if (s_aggUntil && now >= s_aggUntil)
    {
        if (s_aggDown || s_aggUp)
        {
            ++g_inLogKeyN;
            inputLog("키보드 입력 감지: 누름 %d회 뗌 %d회 (1초) -- 어떤 키인지는 남기지 "
                     "않습니다. 필요하면 inputlog_keys.txt 를 만드세요", s_aggDown, s_aggUp);
        }
        s_aggDown = s_aggUp = 0;
        s_aggUntil = 0;
    }
}

static bool copyDirRecursive(const wchar_t* src, const wchar_t* dst)
{
    CreateDirectoryW(dst, nullptr);
    wchar_t pat[MAX_PATH * 2];
    swprintf(pat, MAX_PATH * 2, L"%s\\*", src);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool ok = true;
    do
    {
        if (fd.cFileName[0] == L'.' &&
            (!fd.cFileName[1] || (fd.cFileName[1] == L'.' && !fd.cFileName[2]))) continue;
        // 정션/심링크는 따라가지 않는다 -- 조상을 가리키면 무한 재귀 = 스택 오버플로(리뷰 확정)
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
        wchar_t s[MAX_PATH * 2], d[MAX_PATH * 2];
        swprintf(s, MAX_PATH * 2, L"%s\\%s", src, fd.cFileName);
        swprintf(d, MAX_PATH * 2, L"%s\\%s", dst, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ok = copyDirRecursive(s, d) && ok;
        else ok = (CopyFileW(s, d, FALSE) != 0) && ok;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return ok;
}

// mods.txt 파싱 공용: name 항목 줄을 찾아 (없으면 -1) 값 문자('0'/'1') 위치를 돌려준다
static int modsTxtFindValue(const std::string& data, const std::string& nameA)
{
    size_t pos = 0;
    if (data.size() >= 3 && data.compare(0, 3, "\xEF\xBB\xBF") == 0) pos = 3;  // UTF-8 BOM 스킵
    while (pos < data.size())
    {
        size_t eol = data.find('\n', pos);
        if (eol == std::string::npos) eol = data.size();
        size_t s = pos;
        while (s < eol && (data[s] == ' ' || data[s] == '\t')) ++s;
        if (data.compare(s, nameA.size(), nameA) == 0)
        {
            size_t p = s + nameA.size();
            while (p < eol && (data[p] == ' ' || data[p] == '\t')) ++p;
            if (p < eol && data[p] == ':')
            {
                ++p;
                while (p < eol && (data[p] == ' ' || data[p] == '\t')) ++p;
                if (p < eol && (data[p] == '0' || data[p] == '1')) return (int)p;
            }
        }
        pos = eol + 1;
    }
    return -1;
}

static std::string readFileA(const wchar_t* path)
{
    std::string out;
    // FILE_SHARE_WRITE 필수: UE4SS.log 처럼 다른 프로세스가 쓰기로 잡고 있는
    // 파일은 우리 쪽이 상대의 쓰기를 허용해야 열린다 (라이브 실패 실측)
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return out;
    DWORD sz = GetFileSize(h, nullptr);
    if (sz && sz != INVALID_FILE_SIZE)
    {
        out.resize(sz);
        DWORD rd = 0;
        if (!ReadFile(h, out.data(), sz, &rd, nullptr)) out.clear();
        else out.resize(rd);
    }
    CloseHandle(h);
    return out;
}

static bool modsTxtEnabled(const wchar_t* name)
{
    wchar_t mt[MAX_PATH * 2];
    gameModsRoot(mt);
    lstrcatW(mt, L"mods.txt");
    std::string data = readFileA(mt);
    if (data.empty()) return false;
    int vp = modsTxtFindValue(data, u8(name));
    return vp >= 0 && data[vp] == '1';
}

static void modsTxtDisable(const wchar_t* name)
{
    wchar_t mt[MAX_PATH * 2];
    gameModsRoot(mt);
    lstrcatW(mt, L"mods.txt");
    std::string data = readFileA(mt);
    if (data.empty()) return;
    int vp = modsTxtFindValue(data, u8(name));
    if (vp < 0 || data[vp] != '1') return;
    wchar_t bak[MAX_PATH * 2];
    gameModsRoot(bak);
    lstrcatW(bak, L"mods.txt.cppmm.bak");
    CopyFileW(mt, bak, TRUE);  // 최초 1회만 (이미 있으면 실패 = 의도)
    data[vp] = '0';
    HANDLE h = CreateFileW(mt, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { logf("FAIL mods.txt 쓰기 실패"); return; }
    DWORD wr = 0;
    WriteFile(h, data.data(), (DWORD)data.size(), &wr, nullptr);
    CloseHandle(h);
    logf("mods.txt: '%s : 1' -> 0", u8(name).c_str());
}

// v0.7: 자가 펄스 -- K2_SetTimer 로 클론의 무해한 게터를 20ms 루프 호출하게 등록.
// 타이머 콜백은 게임 스레드에서 ProcessEvent 를 타므로 우리 전역 훅이 매번 깨어난다
// = 모달 중 PE 트래픽 기근(클릭 1초+ 지연의 원인) 해소. 대상 오브젝트(클론)가
// 월드와 함께 죽으면 타이머도 자동 소멸 -> 재삽입 때마다 재등록하면 된다.
static void startPulseTimer(RC::Unreal::UObject* clone)
{
    RC::Unreal::UObject* ksl = findObj(L"/Script/Engine.Default__KismetSystemLibrary", "pulse");
    if (!ksl) return;
    RC::Unreal::UFunction* fn = fnOf(ksl, L"K2_SetTimer", "pulse");
    if (!fn) return;
    int ps = (int)fn->GetParmsSize();
    // UE 버전에 따라 40(구형) 또는 48(+InitialStartDelay/Variance). 필드 오프셋은
    // 앞쪽(0/8/24/28)이 동일하고 추가 float 들은 0 이면 무해하다.
    if (ps < 33 || ps > 96)
    {
        logf("WARN pulse: K2_SetTimer parms=%d -- 생략 (전역 PE 편승만 사용)", ps);
        return;
    }
    // ⚠ 리뷰 확정: K2_SetTimer 는 대상 함수의 ParmsSize>0 이면 조용히 거부한다
    // (반환값도 ParmsSize 에 포함 -- GetVisibility(ret 1B)는 탈락). 진짜 0-parm
    // 인 UWidget:ForceLayoutPrepass(void, 인자 없음)를 쓰고, 사전 검증한다.
    static const wchar_t* const FN = L"ForceLayoutPrepass";
    UFunction* tf = fnOf(clone, FN, "pulse.target");
    if (!tf || (int)tf->GetParmsSize() != 0)
    {
        logf("WARN pulse: 대상 함수 parms=%d (0 필요) -- 생략", tf ? (int)tf->GetParmsSize() : -1);
        return;
    }
    PB pb;
    memcpy(pb.b + 0, &clone, 8);
    FStringRaw fs{FN, (int)wcslen(FN) + 1, (int)wcslen(FN) + 1};
    memcpy(pb.b + 8, &fs, 16);
    float t = 0.02f;
    memcpy(pb.b + 24, &t, 4);
    pb.b[28] = 1;  // bLooping
    if (!peGuard(ksl, fn, pb.b))
    {
        logf("WARN pulse: K2_SetTimer SEH -- 생략");
        return;
    }
    unsigned long long handle = 0;
    memcpy(&handle, pb.b + (int)fn->GetReturnValueOffset(), 8);
    if (handle == 0) logf("WARN pulse: 핸들 0 = 등록 거부됨 (전역 PE 편승만 사용)");
    else logf("pulse: 20ms 게임스레드 펄스 등록 OK (handle=%llu, parms=%d)", handle, ps);
}

// v0.7: 이번 세션에 실제 시작된 모드 스냅샷 -- UE4SS.log 에서
// "Starting Lua mod 'X'" / "Starting C++ mod 'X'" / "Mod 'X' has enabled.txt" 를 긁는다.
// 모드 내부 협조가 필요 없어 서드파티 모드에도 정확하다.
static void loadSessionMods()
{
    g_loadedN = 0;
    wchar_t lp[MAX_PATH * 2];
    gameModsRoot(lp);  // "...\ue4ss\Mods" 에서 한 단계 더 올라가면 ue4ss 루트
    size_t n = wcslen(lp);
    if (n && lp[n - 1] == L'\\') lp[--n] = 0;
    while (n && lp[n - 1] != L'\\') lp[--n] = 0;
    lstrcatW(lp, L"UE4SS.log");
    std::string log = readFileA(lp);
    if (log.empty())
    {
        logf("WARN 세션 스냅샷: UE4SS.log 읽기 실패 (%s)", u8(lp).c_str());
        return;
    }
    const char* pats[3] = {"Starting Lua mod '", "Starting C++ mod '", "Mod '"};
    for (int pi = 0; pi < 3; ++pi)
    {
        size_t pos = 0;
        while (g_loadedN < 96)
        {
            pos = log.find(pats[pi], pos);
            if (pos == std::string::npos) break;
            pos += strlen(pats[pi]);
            size_t end = log.find('\'', pos);
            if (end == std::string::npos) break;
            if (end - pos >= 63)  // 다른 모드의 임의 로그 줄 -- 이 항목만 건너뛴다
            {
                pos = end;
                continue;
            }
            // "Mod 'X'" 패턴은 enabled.txt 줄만 인정
            if (pi == 2 && log.compare(end, 17, "' has enabled.txt") != 0)
            {
                pos = end;
                continue;
            }
            std::string nameA = log.substr(pos, end - pos);
            wchar_t nameW[64];
            int wn = MultiByteToWideChar(CP_UTF8, 0, nameA.c_str(), -1, nameW, 64);
            if (wn > 0)
            {
                bool dup = false;
                for (int i = 0; i < g_loadedN; ++i)
                    if (_wcsicmp(g_loaded[i], nameW) == 0) { dup = true; break; }
                if (!dup) lstrcpynW(g_loaded[g_loadedN++], nameW, 64);
            }
            pos = end;
        }
    }
    logf("세션 스냅샷: 시작된 모드 %d개", g_loadedN);
}

static bool sessionLoaded(const wchar_t* name)
{
    for (int i = 0; i < g_loadedN; ++i)
        if (_wcsicmp(g_loaded[i], name) == 0) return true;
    return false;
}

// ---- v0.8: 런타임 협조 계약 동기화 ---------------------------------------
// 로드 계층(enabled.txt/mods.txt)과 별개로, 이미 로드된 모드들은
// probe/mods_enabled.txt 를 ~1Hz 폴링해 스스로 활동을 켜고 끈다(협조 계약).
// Lua 매니저 은퇴 후 이 파일의 관리 주체가 없어져 "매니저는 켜짐인데 모드는
// 잠듦" 불일치가 생겼다. 게임 시작 시 1회 + 토글 때마다 여기서 정합시킨다.
// (폴더명→런타임 키 대응은 당분간 내장 표 -- 추후 dsplugin.ini 매니페스트로 확장)
struct RtKey
{
    const wchar_t* folder;
    const char* key;
};
static const RtKey RT_KEYS[] = {
    {L"DsChestFinder", "chestfinder"},
    {L"DsDPSGauge", "dpsgauge"},
    {L"DsAutoSweep", "autosweep"},
    {L"DsPauseGuard", "pauseguard"},
};
// 협조 계약 파일은 각 Lua 모드에 절대경로로 하드코딩돼 있다 (개발 머신 전용)
static const wchar_t* const RT_FILE_W = L"F:/AI_Project/ETC/GAME_MOD/DragonSword/probe/mods_enabled.txt";

static bool modFileEnabled(const wchar_t* name)  // 로드 계층 기준 켜짐 여부
{
    wchar_t en[MAX_PATH * 2];
    gameModsRoot(en);
    lstrcatW(en, name);
    lstrcatW(en, L"\\enabled.txt");
    return pathExistsW(en) || modsTxtEnabled(name);
}

// mods_enabled.txt 의 key=0|1 줄을 갱신(없으면 추가). 모르는 키/줄은 보존한다.
static void rtSetKey(const char* key, bool on)
{
    std::string data = readFileA(RT_FILE_W);
    std::string want = std::string(key) + "=" + (on ? "1" : "0");
    std::string out;
    bool found = false;
    size_t pos = 0;
    while (pos < data.size())
    {
        size_t eol = data.find('\n', pos);
        size_t len = (eol == std::string::npos ? data.size() : eol) - pos;
        std::string line = data.substr(pos, len);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        size_t s = 0;
        while (s < line.size() && (line[s] == ' ' || line[s] == '\t')) ++s;
        bool match = line.compare(s, strlen(key), key) == 0;
        if (match)
        {
            size_t p = s + strlen(key);
            while (p < line.size() && (line[p] == ' ' || line[p] == '\t')) ++p;
            match = p < line.size() && line[p] == '=';
        }
        if (match && !found)
        {
            out += want;
            found = true;
        }
        else if (!line.empty() && !match) out += line;  // 매치된 중복 줄은 버린다 (Lua 는 마지막 줄 우선)
        if (!line.empty() || eol != std::string::npos) out += "\n";
        if (eol == std::string::npos) break;
        pos = eol + 1;
    }
    if (!found) out += want + "\n";
    HANDLE h = INVALID_HANDLE_VALUE;
    for (int tries = 0; tries < 3 && h == INVALID_HANDLE_VALUE; ++tries)
    {
        if (tries) Sleep(5);  // Lua 폴링 리더와의 공유 위반(~µs 창) 재시도
        h = CreateFileW(RT_FILE_W, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    }
    if (h == INVALID_HANDLE_VALUE)
    {
        logf("WARN rtSetKey: mods_enabled.txt 쓰기 실패 (%s)", key);
        return;
    }
    DWORD wr = 0;
    WriteFile(h, out.data(), (DWORD)out.size(), &wr, nullptr);
    CloseHandle(h);
}

static const char* rtKeyOf(const wchar_t* folder)
{
    for (const RtKey& k : RT_KEYS)
        if (_wcsicmp(k.folder, folder) == 0) return k.key;
    return nullptr;
}

// ---- v0.10: 플러그인 옵션 계약 (dsplugin.ini / dsoptions.txt / dsruntime.txt) ----
// "public 함수" 대신 파일 계약이 API 다: Lua 모드도 C++ 모드도, 링크 없이
// 자기 폴더의 파일만 읽으면 된다(서드파티 안전 -- 경로 하드코딩 불필요).
// 스키마와 소비 방법은 배포 zip 의 PLUGIN_GUIDE.md 가 정본.

// 아주 소박한 INI 스캐너: [section] 안의 key=value (앞뒤 공백/CR 무시, UTF-8)
static std::string iniValue(const std::string& data, const char* section, const char* key)
{
    std::string sec = std::string("[") + section + "]";
    size_t pos = 0;
    bool inSec = false;
    while (pos < data.size())
    {
        size_t eol = data.find('\n', pos);
        if (eol == std::string::npos) eol = data.size();
        size_t s = pos, e = eol;
        while (s < e && (data[s] == ' ' || data[s] == '\t')) ++s;
        while (e > s && (data[e - 1] == '\r' || data[e - 1] == ' ' || data[e - 1] == '\t')) --e;
        if (e > s)
        {
            if (data[s] == '[')
                inSec = (e - s == sec.size()) && data.compare(s, sec.size(), sec) == 0;
            else if (inSec)
            {
                size_t eq = data.find('=', s);
                if (eq != std::string::npos && eq < e)
                {
                    size_t ke = eq;
                    while (ke > s && (data[ke - 1] == ' ' || data[ke - 1] == '\t')) --ke;
                    if (ke - s == strlen(key) && _strnicmp(data.c_str() + s, key, ke - s) == 0)
                    {
                        size_t vs = eq + 1;
                        while (vs < e && (data[vs] == ' ' || data[vs] == '\t')) ++vs;
                        return data.substr(vs, e - vs);
                    }
                }
            }
        }
        pos = eol + 1;
    }
    return std::string();
}

static void utf8ToW(const std::string& s, wchar_t* out, int cap)
{
    out[0] = 0;
    if (!s.empty()) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, out, cap);
    out[cap - 1] = 0;
}

// plugins\<이름>\dsplugin.ini 를 읽어 rtkey/옵션 선언을 채운다
// v0.40: 언어 변형 키 -- 영어 모드면 "<키>_en" 을 먼저 찾고 없으면 원본을 쓴다.
// (AutoFood 세션 요청 2026-08-11: 모드가 두 언어 라벨을 매니페스트에 같이 실을 수
//  있게. 매니저가 패널을 지을 때 g_lang 에 맞는 쪽을 고르므로 같은 프레임에 바뀐다
//  -- 모드 쪽 1초 폴링 갈아끼우기의 '한 박자 지연'이 사라진다.)
// ⚠ choices_en 은 원본과 항목 수·순서가 같아야 한다 (저장값 = 0기준 인덱스).
static std::string iniValueLang(const std::string& ini, const char* sec, const char* key)
{
    if (g_lang == 1)
    {
        std::string k = std::string(key) + "_en";
        std::string v = iniValue(ini, sec, k.c_str());
        if (!v.empty()) return v;
    }
    return iniValue(ini, sec, key);
}

// ======================= v0.50: config.ini 브리지 =========================
// 서드파티 모드가 dsplugin.ini 옵션을 하나도 선언하지 않았지만 config.ini 로
// 설정을 받는 경우(예: 대화 초상화 오버레이), 그 config.ini 값을 이 패널에서
// 바로 편집하게 한다. 사용자 결정(2026-08-12): "있는 것 전부 노출".
//   [섹션] 줄  = 안내/제목 행(type 7, 컨트롤 없음)
//   값 0/1     = 토글, 정수 = 스테퍼, 그 외(실수/문자열) = 읽기 전용 표시(type 7)
//   저장       = config.ini 제자리(값 토막만 교체, 주석/순서/나머지 전부 보존)
// dsplugin.ini 옵션이 하나라도 있으면 브리지하지 않는다(모드 계약 우선).
// config.ini 위치: 플러그인 폴더 루트 우선, 없으면 Scripts\ 하위.
static bool iniBridgeFilePath(PlgRow& r, wchar_t* out)
{
    const wchar_t* base = r.rel[0] ? r.rel : r.name;
    const wchar_t* subs[] = { L"\\config.ini", L"\\Scripts\\config.ini", L"\\scripts\\config.ini" };
    for (const wchar_t* sub : subs)
    {
        pluginSrcPath(out, base);
        lstrcatW(out, sub);
        if (pathExistsW(out)) return true;
    }
    return false;
}

// 정수 리터럴인가 (앞뒤 공백 제거된 문자열, 부호 허용)
static bool isIntLiteral(const std::string& v)
{
    if (v.empty()) return false;
    size_t i = 0;
    if (v[i] == '+' || v[i] == '-') ++i;
    if (i >= v.size()) return false;
    for (; i < v.size(); ++i) if (v[i] < '0' || v[i] > '9') return false;
    return true;
}

// v0.50: 값이 int32 로 '왕복'되는 표준 정수인가. 왕복이 안 되면(앞자리 0, 선행 +,
// int32 범위 초과) atoi/"%d" 저장이 원문을 바꿔버리므로(007->7, 4294967295 오버플로,
// SteamID64 손상) 편집 대상에서 빼고 표시 전용(type 7)으로 돌린다. 리뷰 확정 2건의 뿌리.
static bool parseInt32Exact(const std::string& v, int* out)
{
    if (!isIntLiteral(v)) return false;
    char* end = nullptr;
    long long ll = strtoll(v.c_str(), &end, 10);
    if (!end || *end != 0) return false;
    if (ll < -2147483647LL - 1 || ll > 2147483647LL) return false;
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", (int)ll);
    if (v != buf) return false;   // 표준형이 아니면(앞0·+부호 등) 거부
    *out = (int)ll;
    return true;
}

// config.ini 제자리 저장 -- 값 토막만 교체(키/주석/순서/나머지 보존). true=교체함.
static bool iniReplaceValue(std::string& data, const char* section, const char* key, const std::string& newVal)
{
    std::string sec = std::string("[") + section + "]";
    size_t pos = 0;
    bool inSec = false;
    while (pos < data.size())
    {
        size_t eol = data.find('\n', pos);
        size_t lineEnd = (eol == std::string::npos) ? data.size() : eol;
        size_t s = pos, e = lineEnd;
        while (s < e && (data[s] == ' ' || data[s] == '\t')) ++s;
        size_t te = e;
        while (te > s && (data[te - 1] == '\r' || data[te - 1] == ' ' || data[te - 1] == '\t')) --te;
        if (te > s)
        {
            if (data[s] == '[')
                inSec = (te - s == sec.size()) && data.compare(s, sec.size(), sec) == 0;
            else if (inSec && data[s] != ';' && data[s] != '#')
            {
                size_t eq = data.find('=', s);
                if (eq != std::string::npos && eq < te)
                {
                    size_t ke = eq;
                    while (ke > s && (data[ke - 1] == ' ' || data[ke - 1] == '\t')) --ke;
                    if (ke - s == strlen(key) && _strnicmp(data.c_str() + s, key, ke - s) == 0)
                    {
                        size_t vs = eq + 1;
                        while (vs < te && (data[vs] == ' ' || data[vs] == '\t')) ++vs;
                        data.replace(vs, te - vs, newVal);
                        return true;
                    }
                }
            }
        }
        if (eol == std::string::npos) break;
        pos = eol + 1;
    }
    return false;
}

static void saveIniBridge(PlgRow& r)
{
    wchar_t p[MAX_PATH * 2];
    if (!iniBridgeFilePath(r, p)) return;
    std::string data = readFileA(p);
    if (data.empty()) return;
    bool bom = (data.size() >= 3 && data.compare(0, 3, "\xEF\xBB\xBF") == 0);
    std::string body = bom ? data.substr(3) : data;
    int changed = 0;
    for (int i = 0; i < r.optN; ++i)
    {
        PlgOpt& o = r.opt[i];
        if (!o.iniBacked || o.iniHeader) continue;
        if (o.type != 0 && o.type != 1) continue;   // 편집 가능한 것만(표시전용 제외)
        if (o.val == o.iniOrigVal) continue;         // v0.50: 사용자가 바꾼 키만 기록 -- 남의 줄 보존
        char buf[24];
        snprintf(buf, sizeof(buf), "%d", o.val);
        if (iniReplaceValue(body, o.iniSection, o.key, buf)) { ++changed; o.iniOrigVal = o.val; }
    }
    if (changed == 0) return;
    std::string outp = bom ? (std::string("\xEF\xBB\xBF") + body) : body;
    HANDLE h = CreateFileW(p, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { logf("WARN iniBridge save: 열기 실패"); return; }
    DWORD wr = 0;
    WriteFile(h, outp.data(), (DWORD)outp.size(), &wr, nullptr);
    CloseHandle(h);
    logf("iniBridge '%s': config.ini 저장 (%d개 갱신)", u8(r.name).c_str(), changed);
}

// config.ini 를 읽어 옵션 행을 채운다 (loadManifest 에서 optN==0 일 때만 호출).
static void loadIniBridge(PlgRow& r)
{
    wchar_t p[MAX_PATH * 2];
    if (!iniBridgeFilePath(r, p)) return;
    std::string data = readFileA(p);
    if (data.empty()) return;
    if (data.size() >= 3 && data.compare(0, 3, "\xEF\xBB\xBF") == 0) data.erase(0, 3);

    int firstKey = r.optN;   // 안내 행 전에 실제 키 개수 판정용
    // 안내 행 (항상 첫 줄)
    {
        PlgOpt& o = r.opt[r.optN];
        memset(&o, 0, sizeof(o));
        o.type = 7; o.iniBacked = true; o.iniHeader = true;
        lstrcpynW(o.label, TR(L"config.ini \u00b7 \uac12 \ubcc0\uacbd \ud6c4 \ubaa8\ub4dc \ub9ac\ub85c\ub4dc/\uc7ac\uc2dc\uc791 \ud544\uc694",
                              L"config.ini \u00b7 reload or restart the mod after changes"), 48);
        ++r.optN;
    }

    std::string curSec;
    int realKeys = 0;
    size_t pos = 0;
    while (pos < data.size() && r.optN < MM_MAX_OPT)
    {
        size_t eol = data.find('\n', pos);
        if (eol == std::string::npos) eol = data.size();
        size_t s = pos, e = eol;
        while (s < e && (data[s] == ' ' || data[s] == '\t')) ++s;
        while (e > s && (data[e - 1] == '\r' || data[e - 1] == ' ' || data[e - 1] == '\t')) --e;
        pos = eol + 1;
        if (e <= s) continue;                 // 빈 줄
        char c0 = data[s];
        if (c0 == ';' || c0 == '#') continue; // 주석
        if (c0 == '[')
        {
            size_t rb = data.find(']', s);
            if (rb == std::string::npos || rb >= e) continue;
            curSec = data.substr(s + 1, rb - s - 1);
            if (curSec.empty() || curSec.size() >= 24) { curSec.clear(); continue; }
            PlgOpt& o = r.opt[r.optN];
            memset(&o, 0, sizeof(o));
            o.type = 7; o.iniBacked = true; o.iniHeader = true;
            strncpy_s(o.iniSection, curSec.c_str(), _TRUNCATE);
            std::string lab = "[" + curSec + "]";
            utf8ToW(lab, o.label, 48);
            ++r.optN;
            continue;
        }
        // key=value
        size_t eq = data.find('=', s);
        if (eq == std::string::npos || eq >= e) continue;
        size_t ke = eq;
        while (ke > s && (data[ke - 1] == ' ' || data[ke - 1] == '\t')) --ke;
        std::string key = data.substr(s, ke - s);
        size_t vs = eq + 1;
        while (vs < e && (data[vs] == ' ' || data[vs] == '\t')) ++vs;
        std::string val = data.substr(vs, e - vs);
        if (key.empty() || key.size() >= 32 || curSec.empty()) continue;

        PlgOpt& o = r.opt[r.optN];
        memset(&o, 0, sizeof(o));
        o.iniBacked = true;
        strncpy_s(o.key, key.c_str(), _TRUNCATE);
        strncpy_s(o.iniSection, curSec.c_str(), _TRUNCATE);
        int iv = 0;
        if (val == "0" || val == "1")
        {
            o.type = 0; o.minV = 0; o.maxV = 1; o.step = 1; o.val = (val == "1") ? 1 : 0;
            o.iniOrigVal = o.val;
            utf8ToW(key, o.label, 48);
        }
        else if (parseInt32Exact(val, &iv))   // v0.50: 왕복되는 표준 int32 만 편집 대상
        {
            o.type = 1; o.val = iv; o.iniOrigVal = iv;
            int a = iv < 0 ? -iv : iv;
            o.step = (a >= 1000) ? 25 : (a >= 100 ? 5 : 1);   // 큰 좌표는 큰 폭으로
            o.minV = -100000000; o.maxV = 100000000;          // 오버플로 안전한 넉넉한 범위
            utf8ToW(key, o.label, 48);
        }
        else
        {
            // 실수/문자열/비표준 정수(큰 수·앞0·부호) = 읽기 전용 표시 (라벨에 값 포함)
            o.type = 7; o.iniHeader = false;
            std::string lab = key + " = " + val;
            utf8ToW(lab, o.label, 48);
        }
        ++realKeys;
        ++r.optN;
    }
    if (realKeys == 0) { r.optN = firstKey; return; }   // 키가 없으면 안내/섹션행도 걷어낸다
    if (r.optN >= MM_MAX_OPT && pos < data.size())      // v0.50: 상한에 걸려 뒷줄이 잘렸다 -- '넣었는데 안 보인다' 방지
        logf("WARN iniBridge '%s': config.ini 항목이 상한(%d) 초과 -- 뒷부분 잘림", u8(r.name).c_str(), MM_MAX_OPT);
    r.iniBridge = true;
    logf("iniBridge '%s': config.ini -> 옵션 %d개 (키 %d)", u8(r.name).c_str(), r.optN - firstKey, realKeys);
}

static void loadManifest(PlgRow& r)
{
    r.rtkey[0] = 0;
    r.optN = 0;
    r.iniBridge = false;  // v0.50
    wchar_t mp[MAX_PATH * 2];
    pluginSrcPath(mp, r.rel[0] ? r.rel : r.name);
    lstrcatW(mp, L"\\dsplugin.ini");
    std::string ini = readFileA(mp);
    if (ini.empty()) { loadIniBridge(r); return; }  // v0.50: config.ini 브리지
    if (ini.size() >= 3 && ini.compare(0, 3, "\xEF\xBB\xBF") == 0) ini.erase(0, 3);

    std::string rk = iniValue(ini, "plugin", "runtime_key");
    if (!rk.empty() && rk.size() < 31)
    {
        strncpy_s(r.rtkey, rk.c_str(), _TRUNCATE);
    }
    // v0.11: 표시 이름 -- [plugin] name=한글이름 (UTF-8). 파일 계층은 계속
    // 폴더명(r.name)을 쓰고, 이 값은 오직 패널 표기에만 쓴다.
    std::string nm = iniValueLang(ini, "plugin", "name");
    if (!nm.empty()) utf8ToW(nm, r.label, 64);
    // v0.27: pak 모드가 어디로 가야 하는지 (블루프린트=logicmods 기본, 에셋교체=paks)
    std::string pt = iniValue(ini, "plugin", "pak_target");
    if (!pt.empty() && pt.size() < 15) strncpy_s(r.pakTarget, pt.c_str(), _TRUNCATE);
    // 옵션: [option:<key>] 섹션을 ini 본문에서 직접 나열 (v0.16: 캡 4 -> 16)
    size_t pos = 0;
    while (r.optN < 16)
    {
        pos = ini.find("[option:", pos);
        if (pos == std::string::npos) break;
        size_t ks = pos + 8;
        size_t ke = ini.find(']', ks);
        if (ke == std::string::npos || ke - ks >= 31) break;
        std::string key = ini.substr(ks, ke - ks);
        std::string secName = "option:" + key;
        PlgOpt& o = r.opt[r.optN];
        memset(&o, 0, sizeof(o));
        strncpy_s(o.key, key.c_str(), _TRUNCATE);
        std::string type = iniValue(ini, secName.c_str(), "type");
        if (_stricmp(type.c_str(), "int") == 0) o.type = 1;
        else if (_stricmp(type.c_str(), "key") == 0) o.type = 2;       // v0.28
        else if (_stricmp(type.c_str(), "color") == 0) o.type = 3;     // v0.28
        else if (_stricmp(type.c_str(), "check") == 0) o.type = 4;     // v0.29
        else if (_stricmp(type.c_str(), "button") == 0) o.type = 5;    // v0.29
        else if (_stricmp(type.c_str(), "slider") == 0) o.type = 6;    // v0.29
        else o.type = 0;
        std::string bcap = iniValueLang(ini, secName.c_str(), "button");
        if (!bcap.empty()) utf8ToW(bcap, o.btnCap, 24);
        std::string lbl = iniValueLang(ini, secName.c_str(), "label");
        utf8ToW(lbl.empty() ? key : lbl, o.label, 48);
        o.minV = atoi(iniValue(ini, secName.c_str(), "min").c_str());
        o.maxV = atoi(iniValue(ini, secName.c_str(), "max").c_str());
        o.step = atoi(iniValue(ini, secName.c_str(), "step").c_str());
        if (o.step <= 0) o.step = 1;
        if (o.maxV <= o.minV) { o.minV = 0; o.maxV = o.type ? 100 : 1; }
        // v0.15: choices=a,b,c -- 있으면 콤보박스 (값 = 0기준 인덱스, min/max 무시)
        std::string ch = iniValueLang(ini, secName.c_str(), "choices");
        if (!ch.empty())
        {
            size_t cp = 0;
            while (o.choiceN < 10 && cp <= ch.size())
            {
                size_t ce = ch.find(',', cp);
                if (ce == std::string::npos) ce = ch.size();
                std::string item = ch.substr(cp, ce - cp);
                while (!item.empty() && (item.front() == ' ' || item.front() == '\t')) item.erase(0, 1);
                while (!item.empty() && (item.back() == ' ' || item.back() == '\t' || item.back() == '\r')) item.pop_back();
                if (!item.empty()) utf8ToW(item, o.choices[o.choiceN++], 24);
                cp = ce + 1;
            }
            if (o.choiceN >= 2)
            {
                o.type = 1;  // 저장/클램프 경로는 int 와 동일
                o.minV = 0;
                o.maxV = o.choiceN - 1;
                o.step = 1;
            }
            else o.choiceN = 0;  // 항목 1개 이하 = 콤보 무의미 -> 일반 컨트롤
        }
        if (o.type == 6)   // v0.29: 슬라이더 -- 허용 범위는 0~1000 으로 못박는다
        {
            if (o.minV < 0) o.minV = 0;
            if (o.maxV > 1000) o.maxV = 1000;
            if (o.maxV <= o.minV) { o.minV = 0; o.maxV = 1000; }
        }
        else if (o.type == 4 || o.type == 5) { o.minV = 0; o.maxV = (o.type == 4) ? 1 : 0x7FFFFFF; }
        std::string defs = iniValue(ini, secName.c_str(), "default");
        if (o.type == 2)          // v0.28: key -- 'F7' 같은 이름도 받는다
        {
            o.minV = 0;
            o.maxV = 255;
            o.val = keyFromName(defs);
        }
        else if (o.type == 3)     // v0.28: color -- '#RRGGBB' 또는 10진수
        {
            o.minV = 0;
            o.maxV = 0xFFFFFF;
            int c = colorFromText(defs);
            o.val = (c < 0) ? 0xFFFFFF : c;
        }
        else o.val = atoi(defs.c_str());
        if (o.val < o.minV) o.val = o.minV;
        if (o.val > o.maxV) o.val = o.maxV;
        // v0.13: 자식 옵션 조건
        std::string par = iniValue(ini, secName.c_str(), "parent");
        if (!par.empty() && par.size() < 31) strncpy_s(o.parent, par.c_str(), _TRUNCATE);
        std::string pv = iniValue(ini, secName.c_str(), "parent_value");
        o.hasParentValue = !pv.empty();
        if (o.hasParentValue) o.parentValue = atoi(pv.c_str());
        ++r.optN;
        pos = ke;
    }
    if (r.optN == 0) loadIniBridge(r);  // v0.50: 옵션 미선언 + config.ini 있으면 브리지
    logf("manifest '%s': 표시명=%s rtkey=%s 옵션 %d개", u8(r.name).c_str(),
         r.label[0] ? u8(r.label).c_str() : "(폴더명)",
         r.rtkey[0] ? r.rtkey : "(없음)", r.optN);
}

// dsoptions.txt (key=value) 읽기/쓰기 -- Mods\<이름>\ 과 plugins\<상대경로>\ 양쪽
static void optionsFilePath(wchar_t* out, const wchar_t* name, const wchar_t* rel, bool installed)
{
    if (installed)
    {
        gameModsRoot(out);
        lstrcatW(out, name);
    }
    else pluginSrcPath(out, rel && rel[0] ? rel : name);
    lstrcatW(out, L"\\dsoptions.txt");
}

static void loadOptionValues(PlgRow& r)
{
    if (r.iniBridge) return;   // v0.50: 값은 config.ini 에서 이미 읽음
    wchar_t p[MAX_PATH * 2];
    optionsFilePath(p, r.name, r.rel, true);
    std::string data = readFileA(p);
    if (data.empty())
    {
        optionsFilePath(p, r.name, r.rel, false);
        data = readFileA(p);
    }
    if (data.empty()) return;
    // 단순 파서: 줄 단위 key=value
    size_t pos = 0;
    while (pos < data.size())
    {
        size_t eol = data.find('\n', pos);
        if (eol == std::string::npos) eol = data.size();
        size_t eq = data.find('=', pos);
        if (eq != std::string::npos && eq < eol)
        {
            std::string k = data.substr(pos, eq - pos);
            while (!k.empty() && (k.back() == ' ' || k.back() == '\t')) k.pop_back();
            for (int i = 0; i < r.optN; ++i)
            {
                if (_stricmp(k.c_str(), r.opt[i].key) == 0)
                {
                    int v = atoi(data.c_str() + eq + 1);
                    if (v < r.opt[i].minV) v = r.opt[i].minV;
                    if (v > r.opt[i].maxV) v = r.opt[i].maxV;
                    r.opt[i].val = v;
                }
            }
        }
        pos = eol + 1;
    }
}

static void saveOptionValues(PlgRow& r)
{
    if (r.iniBridge) { saveIniBridge(r); return; }   // v0.50: config.ini 제자리 저장
    std::string out;
    for (int i = 0; i < r.optN; ++i)
    {
        char line[64];
        snprintf(line, sizeof(line), "%s=%d\n", r.opt[i].key, r.opt[i].val);
        out += line;
    }
    for (int side = 0; side < 2; ++side)
    {
        wchar_t p[MAX_PATH * 2];
        optionsFilePath(p, r.name, r.rel, side == 0);
        HANDLE h = CreateFileW(p, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;  // 미설치(Mods 없음) 쪽은 조용히 생략
        DWORD wr = 0;
        WriteFile(h, out.data(), (DWORD)out.size(), &wr, nullptr);
        CloseHandle(h);
    }
    logf("options '%s': 저장 (%d개)", u8(r.name).c_str(), r.optN);
}

// ======================= v0.16: 표시 순서 (dsorder.txt) ====================
// 순서 탭에서 드래그로 정한 표시 순서. 파일 = 폴더명 UTF-8 한 줄씩.
// 파일에 있는 모드는 그 순서대로 앞에, 없는 모드는 발견 순서 그대로 뒤에 붙는다.

struct PlgEnt
{
    wchar_t name[64];   // 모드 폴더명(설치 이름)
    wchar_t rel[192];   // plugins\ 기준 상대 경로 (v0.20 깊이 탐색)
    bool lua;
    bool cpp;
    bool pak;           // v0.27: .pak 을 담은 폴더 = 콘텐츠(pak) 모드
};

/* ======================= v0.27: pak 모드 =================================
  UE4SS 모드와 로드 경로가 완전히 다르다.
    UE4SS 모드 : Mods\<이름>\Scripts|dlls        -> UE4SS 가 로드
    pak 모드   : Content\Paks\LogicMods\*.pak    -> BPModLoaderMod 가 마운트
                 Content\Paks\~mods\*.pak        -> 엔진이 마운트(에셋 교체용)
  이 게임 실측: IoStore 미사용(.utoc/.ucas 없음), 서명(.sig) 없음,
  LogicMods 폴더 이미 존재, BPModLoaderMod 켜져 있음 -- pak 모드가 성립하는 환경이다.

  켤 때 **사본을 만들지 않는다**(v0.26 원칙). 폴더가 아니라 파일이므로 정션 대신
  **하드링크**(CreateHardLinkW)를 쓴다 -- 같은 볼륨의 한 파일에 이름을 하나 더 다는 것.
  끌 때는 그 이름만 지운다(DeleteFileW). 원본은 plugins 에 그대로 남는다.
  ⚠ 지우기 전에 **파일 인덱스로 동일 파일임을 확인**한다. 사용자가 손으로 넣어 둔
  동명이인 pak 을 실수로 지우지 않기 위해서다.
*/
static const wchar_t* const PAK_EXTS[] = {L".pak", L".ucas", L".utoc", L".sig"};

static bool isPakFileName(const wchar_t* n)
{
    size_t len = wcslen(n);
    for (const wchar_t* e : PAK_EXTS)
    {
        size_t el = wcslen(e);
        if (len > el && _wcsicmp(n + len - el, e) == 0) return true;
    }
    return false;
}

// <게임>\DS\Content\Paks\<sub>\  (sub 이 비면 Paks 자체)
static void contentPaksPath(wchar_t* out, const wchar_t* sub)
{
    gameModsRoot(out);   // <ue4ss>/Mods/ 까지
    upDirs(out, 4);      // 네 단계 위 = DS 폴더
    lstrcatW(out, L"Content\\Paks\\");
    if (sub && sub[0])
    {
        lstrcatW(out, sub);
        lstrcatW(out, L"\\");
    }
}

// ---------------- v0.40: 표시 언어 결정/저장 (dslang.txt) ----------------
// ① dslang.txt = 사용자가 콤보로 고른 값(언제나 이긴다) ② 게임 설정
// GameUserSettings.ini 의 LanguageText(실측: 한국어 클라 = 0) ③ Windows UI 언어
static void langFilePath(wchar_t* out)
{
    modRootPath(out);
    lstrcatW(out, L"dslang.txt");
}

static int readGameLangText()   // GameUserSettings.ini 의 LanguageText. 없으면 -1
{
    // v0.40 6차: mtime 캐시 -- 라벨 키퍼가 150ms 로 빨라져(전환 공란 최소화) 파일을
    // 매번 파싱하지 않는다. 쓰기 시각이 그대로면 stat 한 번으로 캐시값을 돌려준다.
    static FILETIME s_mt = {0, 0};
    static int s_val = -1;
    wchar_t p[MAX_PATH * 2];
    gameModsRoot(p);
    upDirs(p, 4);   // = DS 폴더
    lstrcatW(p, L"Saved\\Config\\Windows\\GameUserSettings.ini");
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExW(p, GetFileExInfoStandard, &fad)) return -1;
    if (s_val >= 0 &&
        fad.ftLastWriteTime.dwLowDateTime == s_mt.dwLowDateTime &&
        fad.ftLastWriteTime.dwHighDateTime == s_mt.dwHighDateTime)
        return s_val;
    // 게임이 쓰기 핸들을 쥐고 있을 수 있어 공유 플래그를 넉넉히 준다
    HANDLE h = CreateFileW(p, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return -1;
    static char ini[64 * 1024];
    DWORD rd = 0;
    ReadFile(h, ini, sizeof(ini) - 1, &rd, nullptr);
    CloseHandle(h);
    ini[rd] = 0;
    const char* f = strstr(ini, "LanguageText=");
    if (!f) return -1;
    s_mt = fad.ftLastWriteTime;
    s_val = atoi(f + 13);
    return s_val;
}

static void writeLangBroadcast()
{
    // v0.40: 방송 -- 매니저의 현재 언어("ko"/"en"). 플러그인이 자기 문구 언어를
    // 매니저와 맞추는 용도다. 경로: <모드 폴더>\..\DsCppModManager\dsmmlang.txt
    wchar_t p[MAX_PATH * 2];
    modRootPath(p);
    lstrcatW(p, L"dsmmlang.txt");
    HANDLE h = CreateFileW(p, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    const char* v = (g_lang == 1) ? "en" : "ko";
    DWORD wr = 0;
    WriteFile(h, v, 2, &wr, nullptr);
    CloseHandle(h);
}

static void applyLang(int l)
{
    g_lang = (l == 1) ? 1 : 0;
    writeLangBroadcast();
}

static void saveLang()
{
    // v0.40 핀 형식: "ko 0" -- 뒤 숫자는 저장 순간의 게임 LanguageText.
    // 게임 언어가 그대로인 동안만 핀이 유효하고, 게임 언어를 바꾸면 자동 추적으로
    // 돌아간다 (실측 요구: 게임을 영어로 바꾸면 매니저도 English 가 되어야 한다).
    wchar_t p[MAX_PATH * 2];
    langFilePath(p);
    HANDLE h = CreateFileW(p, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    char buf[32];
    int n = snprintf(buf, sizeof(buf), "%s %d\n", (g_lang == 1) ? "en" : "ko", readGameLangText());
    DWORD wr = 0;
    if (n > 0) WriteFile(h, buf, (DWORD)n, &wr, nullptr);
    CloseHandle(h);
}

static int resolveLang(int lt)   // lt = readGameLangText() 결과. 핀 유효하면 핀, 아니면 자동
{
    // v0.40: lt < 0 은 "지금은 알 수 없음"(게임이 파일을 쓰는 중 등)이지
    // "언어가 다르다"가 아니다 -- 핀은 유지하고, 자동도 현상 유지한다.
    // (리뷰 확정: 2초 키퍼가 저장 순간을 밟으면 언어가 한 틱 튀고 방송까지 튀었다)
    wchar_t p[MAX_PATH * 2];
    langFilePath(p);
    HANDLE h = CreateFileW(p, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE)
    {
        char buf[32] = {0};
        DWORD rd = 0;
        ReadFile(h, buf, sizeof(buf) - 1, &rd, nullptr);
        CloseHandle(h);
        int pinned = (buf[0] == 'e') ? 1 : (buf[0] == 'k') ? 0 : -1;
        const char* sp = strchr(buf, ' ');
        if (pinned >= 0 && sp)
        {
            int rec = atoi(sp + 1);
            // 모르거나(lt<0), 핀 자체가 모름 상태에서 저장됐거나(rec<0), 일치하면 유지
            if (lt < 0 || rec < 0 || rec == lt) return pinned;
        }
    }
    if (lt >= 0) return (lt == 0) ? 0 : 1;   // 실측: 한국어 클라 = 0
    if (g_lang >= 0) return g_lang;          // 모름 -> 현상 유지
    return (PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_KOREAN) ? 0 : 1;
}

static void ensureLang()
{
    if (g_lang >= 0) return;
    applyLang(resolveLang(readGameLangText()));
    logf("lang: 결정 -> %s", g_lang ? "en" : "ko");
}

static bool recheckLang(int lt)   // 키퍼가 부른다. 언어가 바뀌었으면 true
{
    int want = resolveLang(lt);
    if (want == g_lang) return false;
    applyLang(want);
    logf("lang: 게임 언어 변경 감지 -> %s", g_lang ? "en" : "ko");
    return true;
}

// 폴더 안에 .pak 이 하나라도 있는가
static bool folderHasPak(const wchar_t* dir)
{
    // v0.40: IoStore 전용 모드(.utoc/.ucas 만, .pak 없음)도 발견한다
    static const wchar_t* const PAT[2] = {L"%s\\*.pak", L"%s\\*.utoc"};
    for (int i = 0; i < 2; ++i)
    {
        wchar_t pat[MAX_PATH * 2];
        swprintf(pat, MAX_PATH * 2, PAT[i], dir);
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW(pat, &fd);
        if (h != INVALID_HANDLE_VALUE)
        {
            FindClose(h);
            return true;
        }
    }
    return false;
}

// v0.40: plugins 루트에 pak 계열 '파일'을 그냥 넣은 경우 (실측 보고: 인식 안 됨)
// -- 발견 규칙은 폴더 단위이므로 <파일이름>\ 폴더를 만들어 감싸준다.
//    같은 이름의 .ucas/.utoc/.sig 짝도 zip 자동 해제와 같은 3초 스윕에서 함께 옮겨진다.
static void wrapLoosePaks()
{
    wchar_t pat[MAX_PATH * 2];
    modRootPath(pat);
    lstrcatW(pat, L"plugins\\*.*");
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do
    {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (!isPakFileName(fd.cFileName)) continue;
        wchar_t stem[80];
        lstrcpynW(stem, fd.cFileName, 80);
        wchar_t* dot = wcsrchr(stem, L'.');
        if (dot) *dot = 0;
        if (!stem[0]) continue;
        wchar_t dir[MAX_PATH * 2];
        pluginSrcPath(dir, stem);
        CreateDirectoryW(dir, nullptr);
        wchar_t srcp[MAX_PATH * 2], dstp[MAX_PATH * 2];
        pluginSrcPath(srcp, fd.cFileName);
        swprintf(dstp, MAX_PATH * 2, L"%s\\%s", dir, fd.cFileName);
        if (MoveFileW(srcp, dstp))
            logf("pak: 낱개 파일 '%s' -> '%s\\' 폴더로 감쌈 (pak 모드로 인식)",
                 u8(fd.cFileName).c_str(), u8(stem).c_str());
        else
            logf("WARN pak: '%s' 감싸기 실패 (err=%lu)", u8(fd.cFileName).c_str(), GetLastError());
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// 두 경로가 같은 실체(하드링크)인가 -- 파일 인덱스 비교
static bool sameFileIdentity(const wchar_t* a, const wchar_t* b)
{
    BY_HANDLE_FILE_INFORMATION ia, ib;
    HANDLE ha = CreateFileW(a, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (ha == INVALID_HANDLE_VALUE) return false;
    HANDLE hb = CreateFileW(b, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hb == INVALID_HANDLE_VALUE) { CloseHandle(ha); return false; }
    bool ok = GetFileInformationByHandle(ha, &ia) && GetFileInformationByHandle(hb, &ib) &&
              ia.dwVolumeSerialNumber == ib.dwVolumeSerialNumber &&
              ia.nFileIndexHigh == ib.nFileIndexHigh && ia.nFileIndexLow == ib.nFileIndexLow;
    CloseHandle(ha);
    CloseHandle(hb);
    return ok;
}


static void applySavedOrder(PlgEnt* e, int n)
{
    wchar_t p[MAX_PATH * 2];
    modRootPath(p);
    lstrcatW(p, L"dsorder.txt");
    std::string d = readFileA(p);
    if (d.empty()) return;
    if (d.size() >= 3 && d.compare(0, 3, "\xEF\xBB\xBF") == 0) d.erase(0, 3);
    int outIdx = 0;
    size_t pos = 0;
    while (pos < d.size() && outIdx < n)
    {
        size_t eol = d.find('\n', pos);
        if (eol == std::string::npos) eol = d.size();
        std::string line = d.substr(pos, eol - pos);
        pos = eol + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.erase(0, 1);
        if (line.empty()) continue;
        wchar_t nameW[64];
        utf8ToW(line, nameW, 64);
        for (int i = outIdx; i < n; ++i)
        {
            if (_wcsicmp(e[i].name, nameW) != 0) continue;
            PlgEnt t = e[i];  // 안정 회전 -- 사이 항목들의 상대 순서 유지
            for (int j = i; j > outIdx; --j) e[j] = e[j - 1];
            e[outIdx++] = t;
            break;
        }
    }
}

static void saveOrderFile()
{
    std::string out;
    for (int i = 0; i < g_ordN; ++i) out += u8(g_ord[i].name) + "\n";
    wchar_t p[MAX_PATH * 2];
    modRootPath(p);
    lstrcatW(p, L"dsorder.txt");
    HANDLE h = CreateFileW(p, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
    {
        logf("WARN order: dsorder.txt 쓰기 실패");
        return;
    }
    DWORD wr = 0;
    WriteFile(h, out.data(), (DWORD)out.size(), &wr, nullptr);
    CloseHandle(h);
    logf("order: 순서 저장 (%d개)", g_ordN);
}

// v0.20: 목록에 올리면 안 되는 이름들.
//  - UE4SS 기본 내장 모드: "UE4SS 포함판" 배포판을 통째로 넣으면 딸려 들어온다.
//  - v0.22: 매니저 자신. 사용자가 매니저 zip 을 plugins 에 넣는 일이 실제로 있었고
//    (자동 해제까지 붙으면 더 흔해진다) 그대로 두면 자기 자신을 플러그인으로 띄운다.
static bool isExcludedModName(const wchar_t* n)
{
    static const wchar_t* const B[] = {
        L"BPML_GenericFunctions", L"BPModLoaderMod", L"CheatManagerEnablerMod",
        L"ConsoleCommandsMod", L"ConsoleEnablerMod", L"Keybinds", L"LineTraceMod",
        L"SplitScreenMod", L"jsbLuaProfilerMod", L"shared", L"UE4SS_Signatures",
        L"DsCppModManager",
    };
    for (const wchar_t* b : B)
        if (_wcsicmp(n, b) == 0) return true;
    return false;
}

// ======================= v0.22: plugins 안의 zip 자동 해제 ==================
// 사용자가 배포 zip 을 풀지 않고 그대로 넣는 일이 잦다. UE4SS 는 폴더에서만 모드를
// 읽으므로(zip 은 못 읽는다) 매니저가 대신 풀어 준다.
// ⚠ 해제는 **UpdateThread 에서만** 한다 -- 게임 스레드에서 외부 프로세스를 기다리면
//   그동안 화면이 멎는다. Windows 내장 tar.exe(bsdtar, zip 지원)를 **절대 경로**로
//   부른다: PATH 하이재킹 방지(TRAPS 의 유닉스 find 사고와 같은 이유).
static wchar_t g_zipTried[32][80];
static int g_zipTriedN = 0;

static bool zipAlreadyTriedName(const wchar_t* name)
{
    for (int i = 0; i < g_zipTriedN; ++i)
        if (_wcsicmp(g_zipTried[i], name) == 0) return true;
    return false;
}

static bool runTarExtract(const wchar_t* zipPath, const wchar_t* destDir)
{
    wchar_t tar[MAX_PATH];
    UINT n = GetSystemDirectoryW(tar, MAX_PATH);
    if (!n || n >= MAX_PATH - 16) return false;
    lstrcatW(tar, L"\\tar.exe");
    if (!pathExistsW(tar))
    {
        logf("WARN zip: tar.exe 없음(구버전 Windows) -- 압축은 수동으로 풀어야 합니다");
        return false;
    }
    static wchar_t cmd[MAX_PATH * 5];
    swprintf(cmd, MAX_PATH * 5, L"\"%s\" -xf \"%s\" -C \"%s\"", tar, zipPath, destDir);
    STARTUPINFOW si;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi;
    memset(&pi, 0, sizeof(pi));
    if (!CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &si, &pi))
    {
        logf("FAIL zip: tar 실행 실패 (err=%lu)", GetLastError());
        return false;
    }
    DWORD code = 1;
    if (WaitForSingleObject(pi.hProcess, 30000) == WAIT_OBJECT_0)
        GetExitCodeProcess(pi.hProcess, &code);
    else
    {
        logf("WARN zip: tar 30초 초과 -- 중단");
        TerminateProcess(pi.hProcess, 1);
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return code == 0;
}

static void extractPluginZips()
{
    wchar_t pat[MAX_PATH * 2];
    modRootPath(pat);
    lstrcatW(pat, L"plugins\\*.zip");
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do
    {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        wchar_t stem[80];
        lstrcpynW(stem, fd.cFileName, 80);
        size_t sl = wcslen(stem);
        if (sl > 4 && _wcsicmp(stem + sl - 4, L".zip") == 0) stem[sl - 4] = 0;
        if (!stem[0]) continue;
        wchar_t dest[MAX_PATH * 2];
        pluginSrcPath(dest, stem);
        if (pathExistsW(dest)) continue;             // 이미 풀려 있다 (매 스캔의 정상 경로)
        if (zipAlreadyTriedName(fd.cFileName)) continue;  // 실패한 zip 을 매번 다시 풀지 않는다
        if (g_zipTriedN < 32) lstrcpynW(g_zipTried[g_zipTriedN++], fd.cFileName, 80);
        ULONGLONG sz = ((ULONGLONG)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        if (sz > 200ull * 1024 * 1024)
        {
            logf("WARN zip: '%s' 200MB 초과 -- 자동 해제 생략", u8(fd.cFileName).c_str());
            continue;
        }
        wchar_t zipPath[MAX_PATH * 2];
        pluginSrcPath(zipPath, fd.cFileName);
        if (!CreateDirectoryW(dest, nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
        {
            logf("FAIL zip: '%s' 폴더 생성 실패", u8(stem).c_str());
            continue;
        }
        bool ok = runTarExtract(zipPath, dest);
        logf("zip: '%s' 자동 해제 %s", u8(fd.cFileName).c_str(), ok ? "성공" : "실패");
        if (!ok) RemoveDirectoryW(dest);  // 빈 폴더를 남기면 다음에 '이미 풀림' 으로 오인된다
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// v0.20: 깊이 탐색 -- 모더가 어떤 형태로 압축을 풀었든 찾아낸다.
// 판정은 그대로 "Scripts\main.lua 또는 dlls\main.dll 을 품은 폴더 = 모드 폴더".
// 그 폴더를 찾으면 **그 안으로는 더 들어가지 않는다**(모드 안의 예제/백업 폴더가
// 또 잡히는 것을 막는다). Scripts/dlls 자체도 내려가지 않는다.
//
// v0.20.1 깊이 산정 근거 (배포판 형태별 실제 깊이):
//   모드 단독판                                        1
//   Win64 기준     (ue4ss\Mods\<모드>)                 4
//   Binaries 기준  (Win64\ue4ss\Mods\<모드>)           5
//   게임 루트 기준 (DS\Binaries\Win64\ue4ss\Mods\<모드>) 7
//   게임 폴더째 압축 (<게임명>\DS\...\Mods\<모드>)        8
// 그래서 8단계 + 여유 2 = 10. 대신 무한 탐색이 되지 않게 **디렉터리 예산**으로
// 최악을 묶는다(엉뚱한 대용량 폴더를 넣어도 게임이 멎지 않게).
#define MM_SCAN_MAX_DEPTH 10
static bool g_scanCutWarned = false;

static void scanPluginDir(const wchar_t* base, const wchar_t* rel, int depth,
                          PlgEnt* ents, int cap, int* n, int* budget)
{
    if (*n >= cap) return;
    if (depth > MM_SCAN_MAX_DEPTH || *budget <= 0)
    {
        if (!g_scanCutWarned)
        {
            g_scanCutWarned = true;
            logf("WARN panel: 탐색 중단 (깊이 %d 초과 또는 폴더 예산 소진) -- '%s' 이하 생략",
                 MM_SCAN_MAX_DEPTH, u8(rel).c_str());
        }
        return;
    }
    --(*budget);
    wchar_t dir[MAX_PATH * 2];
    if (rel[0]) swprintf(dir, MAX_PATH * 2, L"%s\\%s", base, rel);
    else lstrcpynW(dir, base, MAX_PATH * 2);
    wchar_t pat[MAX_PATH * 2];
    swprintf(pat, MAX_PATH * 2, L"%s\\*", dir);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do
    {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (fd.cFileName[0] == L'.') continue;
        if (_wcsicmp(fd.cFileName, L"Scripts") == 0 || _wcsicmp(fd.cFileName, L"dlls") == 0) continue;
        wchar_t childRel[192];
        if (rel[0]) swprintf(childRel, 192, L"%s\\%s", rel, fd.cFileName);
        else lstrcpynW(childRel, fd.cFileName, 192);
        wchar_t probe[MAX_PATH * 2];
        swprintf(probe, MAX_PATH * 2, L"%s\\%s\\Scripts\\main.lua", dir, fd.cFileName);
        bool luaMod = GetFileAttributesW(probe) != INVALID_FILE_ATTRIBUTES;
        swprintf(probe, MAX_PATH * 2, L"%s\\%s\\dlls\\main.dll", dir, fd.cFileName);
        bool cppMod = GetFileAttributesW(probe) != INVALID_FILE_ATTRIBUTES;
        // v0.27: .pak 을 담은 폴더도 모드다 (콘텐츠/블루프린트 모드)
        bool pakMod = false;
        if (!luaMod && !cppMod)
        {
            swprintf(probe, MAX_PATH * 2, L"%s\\%s", dir, fd.cFileName);
            pakMod = folderHasPak(probe);
        }
        if (!luaMod && !cppMod && !pakMod)
        {
            scanPluginDir(base, childRel, depth + 1, ents, cap, n, budget);  // 한 겹 더
            continue;
        }
        if (isExcludedModName(fd.cFileName))
        {
            logf("panel: '%s' 는 목록 제외 대상(UE4SS 기본 모드/매니저 자신)", u8(fd.cFileName).c_str());
            continue;
        }
        bool dup = false;  // 같은 이름이 여러 곳에 있으면 먼저 찾은 것만
        for (int i = 0; i < *n; ++i)
            if (_wcsicmp(ents[i].name, fd.cFileName) == 0) { dup = true; break; }
        if (dup)
        {
            logf("panel: '%s' 중복 발견(%s) -- 건너뜀", u8(fd.cFileName).c_str(), u8(childRel).c_str());
            continue;
        }
        if (*n >= cap)
        {
            logf("panel: 플러그인 %d개 초과 -- 이후 생략", cap);
            break;
        }
        lstrcpynW(ents[*n].name, fd.cFileName, 64);
        lstrcpynW(ents[*n].rel, childRel, 192);
        ents[*n].lua = luaMod;
        ents[*n].cpp = cppMod;
        ents[*n].pak = pakMod;
        if (_wcsicmp(childRel, fd.cFileName) != 0)
            logf("panel: '%s' 중첩 발견 -> %s", u8(fd.cFileName).c_str(), u8(childRel).c_str());
        ++(*n);
        // 모드 폴더 안으로는 더 들어가지 않는다
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// 플러그인 수집 + 저장 순서 적용 (모드 탭 목록/순서 탭/알림 스캔 공용)
static int collectPlugins(PlgEnt* ents, int cap)
{
    wchar_t pdir[MAX_PATH * 2];
    modRootPath(pdir);
    lstrcatW(pdir, L"plugins");
    int n = 0;
    int budget = 3000;  // 열어 볼 폴더 수 상한 (최악 보호 -- 정상 설치는 수십 개)
    g_scanCutWarned = false;
    scanPluginDir(pdir, L"", 0, ents, cap, &n, &budget);
    applySavedOrder(ents, n);
    return n;
}

// v0.20.1: 알림 폴링용 캐시. checkModNotifications 는 1초마다 도는데 깊이 탐색을
// 매번 돌리면 게임 스레드에서 디스크를 계속 긁는다. 목록은 5초에 한 번만 갱신하고
// (모드 추가는 그 정도 지연을 감당할 수 있다) dsnotify 파일 확인만 매초 한다.
static PlgEnt g_scanCache[MM_MAX_PLUGINS];
static int g_scanCacheN = 0;
static ULONGLONG g_scanCacheMs = 0;

static int collectPluginsCached(ULONGLONG now)
{
    if (g_scanCacheMs == 0 || now - g_scanCacheMs >= 5000)
    {
        g_scanCacheMs = now;
        g_scanCacheN = collectPlugins(g_scanCache, MM_MAX_PLUGINS);
    }
    return g_scanCacheN;
}

// dsruntime.txt: 범용 런타임 켬끔 신호 (서드파티용 -- 자기 폴더만 읽으면 됨)
static void writeRuntimeFlag(const wchar_t* name, const wchar_t* rel, bool on)
{
    for (int side = 0; side < 2; ++side)
    {
        wchar_t p[MAX_PATH * 2];
        if (side == 0)
        {
            gameModsRoot(p);
            lstrcatW(p, name);
        }
        else pluginSrcPath(p, rel && rel[0] ? rel : name);
        lstrcatW(p, L"\\dsruntime.txt");
        HANDLE h = CreateFileW(p, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;
        const char* v = on ? "1" : "0";
        DWORD wr = 0;
        WriteFile(h, v, 1, &wr, nullptr);
        CloseHandle(h);
    }
}

// 게임 시작 시 1회: 로드 계층(매니저가 보여주는 상태)을 런타임 계약에 반영
static void reconcileRuntimeContract(const char* why)
{
    for (const RtKey& k : RT_KEYS)
    {
        bool on = modFileEnabled(k.folder);
        rtSetKey(k.key, on);
        logf("정합(%s): %s -> %s=%d", why, u8(k.folder).c_str(), k.key, (int)on);
    }
}

/* ======================= v0.26: 사본 대신 정션 ============================

  왜 바꾸나 (사용자 요구): "Mods 에 사본을 만들지 마라. plugins 에 들어온 것은
  plugins 에서만 관리해라." 옛 방식은 켤 때 plugins -> Mods 로 **복사**했다.
  그 결과 (1) 같은 모드가 두 벌 존재해 설정이 어느 쪽인지 헷갈리고,
  (2) 끄거나 매니저를 지워도 Mods 쪽 사본이 남아 계속 로드됐다(실측 사고).

  UE4SS 소스 확인 (research/ue4ss_src, UE4SSProgram.cpp:1284·1567):
    directory_iterator + is_directory() 로 Mods\ 를 훑고, <모드>\scripts 또는
    <모드>\dlls 존재를 보고 등록한 뒤, <모드>\enabled.txt 가 있으면 시작한다.
  is_directory() 는 재해석 지점(정션)을 **따라간다**. 그래서 Mods\<이름> 을
  plugins\<이름> 을 가리키는 **디렉터리 정션**으로 만들면 사본 없이 그대로 로드된다.
  실측: 관리자 권한 없이 생성됨, 읽기/쓰기 모두 원본으로 통함,
  ⚠ 삭제는 반드시 RemoveDirectory(정션만 제거, 원본 무사) -- 재귀 삭제 금지.
*/
#ifndef FSCTL_SET_REPARSE_POINT
#define FSCTL_SET_REPARSE_POINT 0x000900A4
#endif
#ifndef IO_REPARSE_TAG_MOUNT_POINT
#define IO_REPARSE_TAG_MOUNT_POINT 0xA0000003L
#endif

static bool isReparsePoint(const wchar_t* p)
{
    DWORD a = GetFileAttributesW(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

// link 를 target 으로 가리키는 디렉터리 정션 생성 (관리자 권한 불필요, 같은 볼륨)
static bool createJunction(const wchar_t* link, const wchar_t* target)
{
    if (!CreateDirectoryW(link, nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return false;
    HANDLE h = CreateFileW(link, GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (h == INVALID_HANDLE_VALUE) { RemoveDirectoryW(link); return false; }

    wchar_t subst[MAX_PATH * 2];
    swprintf(subst, MAX_PATH * 2, L"\\??\\%s", target);
    size_t sl = wcslen(subst), pl = wcslen(target);

    struct Buf
    {
        ULONG tag;
        USHORT dataLen;
        USHORT reserved;
        USHORT substOff, substLen, printOff, printLen;
        wchar_t path[MAX_PATH * 4];
    } b;
    memset(&b, 0, sizeof(b));
    b.tag = IO_REPARSE_TAG_MOUNT_POINT;
    b.substOff = 0;
    b.substLen = (USHORT)(sl * 2);
    b.printOff = (USHORT)(sl * 2 + 2);
    b.printLen = (USHORT)(pl * 2);
    memcpy(b.path, subst, sl * 2 + 2);
    memcpy((char*)b.path + b.printOff, target, pl * 2 + 2);
    b.dataLen = (USHORT)(8 + b.printOff + pl * 2 + 2);

    DWORD ret = 0;
    BOOL ok = DeviceIoControl(h, FSCTL_SET_REPARSE_POINT, &b,
                              (DWORD)(b.dataLen + 8), nullptr, 0, &ret, nullptr);
    DWORD err = GetLastError();
    CloseHandle(h);
    if (!ok)
    {
        RemoveDirectoryW(link);
        logf("FAIL 정션 생성 (err=%lu) %s", err, u8(link).c_str());
        return false;
    }
    return true;
}

// 실행 진입점(Mods\<이름>) 정리. 정션이면 링크만 지운다 -- 원본은 절대 건드리지 않는다.
// 진짜 폴더(옛 사본)면 남긴다: 남의 데이터를 말없이 지우지 않는다.
static void removeModEntry(const wchar_t* name)
{
    wchar_t dst[MAX_PATH * 2];
    gameModsRoot(dst);
    lstrcatW(dst, name);
    if (!pathExistsW(dst)) return;
    if (isReparsePoint(dst))
    {
        if (RemoveDirectoryW(dst)) logf("plugin '%s': 정션 제거", u8(name).c_str());
        else logf("WARN plugin '%s': 정션 제거 실패 (err=%lu)", u8(name).c_str(), GetLastError());
    }
    else
    {
        logf("WARN plugin '%s': Mods 에 **실제 폴더**가 있다(옛 방식 사본). "
             "지우지 않고 둔다 -- 매니저 밖에서 로드될 수 있으니 직접 확인할 것",
             u8(name).c_str());
    }
}

/* ================= v0.50(TODO 13): pak 링크 장부 (paklinks.txt) =================
   실측(넥서스 리포트 2026-08-16): 모드를 업데이트하면 plugins 원본의 파일 실체가
   바뀌어, ~mods 의 옛 하드링크가 sameFileIdentity 검증(현재 원본과 대조)에 떨어지고
   "사용자 파일 보호" 분기로 영영 남는다. 실체 대조만으로는 "우리가 건 링크"를
   업데이트 후에 증명할 수 없다 -- 그래서 걸 때 장부에 적고, 지울 때 장부를 믿는다.
   형식: <sub>\<파일명>|<크기>|<소유모드폴더명>  (UTF-8 한 줄씩, 관리자 루트에 저장)
   ⚠ 장부 삭제 검증: 실체 대조 실패 시 "장부에 있고 + 크기 일치"면 우리 잔재로 본다.
   (사용자가 우리 링크를 지우고 같은 이름·같은 크기의 파일을 손수 둔 경우만 오판
   가능 -- 내용이 같은 pak 이므로 고유 데이터 손실은 없다. 잔여 위험으로 문서화.) */
static void pakManifestPath(wchar_t* out)
{
    modRootPath(out);
    lstrcatW(out, L"paklinks.txt");
}

static ULONGLONG fileSizeOfW(const wchar_t* p)
{
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExW(p, GetFileExInfoStandard, &fa)) return 0;
    return ((ULONGLONG)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
}

static std::string pakKey(const wchar_t* sub, const wchar_t* file)
{
    return u8(sub) + "\\" + u8(file);
}

// owner 가 null 이면 그 키를 지우고, 아니면 갱신/추가한다 (파일 통째 재작성).
static void pakManifestPut(const wchar_t* sub, const wchar_t* file, ULONGLONG size,
                           const wchar_t* owner)
{
    wchar_t p[MAX_PATH * 2];
    pakManifestPath(p);
    std::string data = readFileA(p);
    std::string key = pakKey(sub, file);
    std::string out;
    size_t pos = 0;
    while (pos < data.size())
    {
        size_t eol = data.find('\n', pos);
        if (eol == std::string::npos) eol = data.size();
        std::string ln = data.substr(pos, eol - pos);
        while (!ln.empty() && (ln.back() == '\r' || ln.back() == ' ')) ln.pop_back();
        size_t bar = ln.find('|');
        bool same = bar != std::string::npos && bar == key.size() &&
                    _strnicmp(ln.c_str(), key.c_str(), bar) == 0;
        if (!ln.empty() && !same) out += ln + "\n";
        pos = eol + 1;
    }
    if (owner)
    {
        // 리뷰 D2: 고정 버퍼 금지 -- 한글 파일명은 UTF-8 로 최대 ~780바이트라
        // snprintf 가 잘라먹으면 항목이 조용히 유실된다(파서가 못 읽는 줄이 됨).
        char szbuf[24];
        snprintf(szbuf, sizeof(szbuf), "%llu", (unsigned long long)size);
        out += key + "|" + szbuf + "|" + u8(owner) + "\n";
    }
    HANDLE h = CreateFileW(p, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD wr = 0;
    WriteFile(h, out.data(), (DWORD)out.size(), &wr, nullptr);
    CloseHandle(h);
}

// 장부에서 키 조회. 반환 true = 있음 (size/owner 채움).
static bool pakManifestFind(const wchar_t* sub, const wchar_t* file,
                            ULONGLONG* sizeOut, std::string* ownerOut)
{
    wchar_t p[MAX_PATH * 2];
    pakManifestPath(p);
    std::string data = readFileA(p);
    std::string key = pakKey(sub, file);
    size_t pos = 0;
    while (pos < data.size())
    {
        size_t eol = data.find('\n', pos);
        if (eol == std::string::npos) eol = data.size();
        std::string ln = data.substr(pos, eol - pos);
        while (!ln.empty() && (ln.back() == '\r' || ln.back() == ' ')) ln.pop_back();
        size_t b1 = ln.find('|');
        if (b1 != std::string::npos && b1 == key.size() &&
            _strnicmp(ln.c_str(), key.c_str(), b1) == 0)
        {
            size_t b2 = ln.find('|', b1 + 1);
            if (b2 != std::string::npos)
            {
                if (sizeOut) *sizeOut = _strtoui64(ln.c_str() + b1 + 1, nullptr, 10);
                if (ownerOut) *ownerOut = ln.substr(b2 + 1);
                return true;
            }
        }
        pos = eol + 1;
    }
    return false;
}

// v0.27: pak 모드 켜기/끄기 -- plugins\<rel>\*.pak 을 Content\Paks\<타깃>\ 에
// 하드링크로 걸었다 뗀다. 반환 = 처리한 파일 수.
// v0.50(TODO 13): owner = 장부에 적을 소유모드 폴더명. 걸면 기록, 지우면 말소.
static int linkPakFiles(const wchar_t* rel, const wchar_t* targetSub, bool on,
                        const wchar_t* owner)
{
    wchar_t src[MAX_PATH * 2];
    pluginSrcPath(src, rel);
    wchar_t dstDir[MAX_PATH * 2];
    contentPaksPath(dstDir, targetSub);
    if (on && !pathExistsW(dstDir) && !CreateDirectoryW(dstDir, nullptr))
    {
        logf("FAIL pak: 대상 폴더 생성 실패 %s", u8(dstDir).c_str());
        return 0;
    }
    wchar_t pat[MAX_PATH * 2];
    swprintf(pat, MAX_PATH * 2, L"%s\\*", src);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int done = 0;
    do
    {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (!isPakFileName(fd.cFileName)) continue;
        wchar_t s[MAX_PATH * 2], d[MAX_PATH * 2];
        swprintf(s, MAX_PATH * 2, L"%s\\%s", src, fd.cFileName);
        swprintf(d, MAX_PATH * 2, L"%s%s", dstDir, fd.cFileName);
        if (on)
        {
            if (pathExistsW(d))
            {
                if (sameFileIdentity(s, d))
                {   // 이미 걸려 있다 -- 장부에 없으면 입양 (구판에서 건 링크의 이행)
                    ++done;
                    if (!pakManifestFind(targetSub, fd.cFileName, nullptr, nullptr))
                        pakManifestPut(targetSub, fd.cFileName, fileSizeOfW(d), owner);
                    continue;
                }
                // v0.50(TODO 13): 실체가 다르다 = 모드 업데이트로 원본이 바뀐 경우가
                // 대부분 -- 장부에 있고 + 크기 일치 + **소유자가 이 모드**일 때만
                // 우리 잔재로 보고 걷어낸 뒤 새 원본으로 다시 건다.
                // 리뷰 D1: 소유자 검증 없이는 같은 파일명을 쓰는 **다른 모드의 산
                // 링크**를 지워버린다 (넥서스 pak 이름 충돌은 실재).
                ULONGLONG msz = 0;
                std::string mOwn;
                if (pakManifestFind(targetSub, fd.cFileName, &msz, &mOwn) &&
                    msz != 0 && msz == fileSizeOfW(d) &&
                    _stricmp(mOwn.c_str(), u8(owner).c_str()) == 0)
                {
                    if (DeleteFileW(d))
                    {
                        logf("pak: '%s' 구판 잔재 교체 (장부 검증)", u8(fd.cFileName).c_str());
                        if (CreateHardLinkW(d, s, nullptr))
                        {
                            ++done;
                            pakManifestPut(targetSub, fd.cFileName, fileSizeOfW(s), owner);
                        }
                        else
                        {
                            pakManifestPut(targetSub, fd.cFileName, 0, nullptr);
                            logf("FAIL pak: 재링크 실패 %s (err=%lu)", u8(fd.cFileName).c_str(), GetLastError());
                        }
                    }
                    else
                        logf("FAIL pak: 구판 잔재 삭제 실패 %s (err=%lu) -- 다음 부팅 초입에 재시도",
                             u8(fd.cFileName).c_str(), GetLastError());
                    continue;
                }
                logf("WARN pak: '%s' 가 이미 있는데 다른 파일이다 -- 건드리지 않는다",
                     u8(fd.cFileName).c_str());
                continue;
            }
            if (CreateHardLinkW(d, s, nullptr))
            {
                ++done;
                pakManifestPut(targetSub, fd.cFileName, fileSizeOfW(s), owner);  // v0.50: 장부 기록
            }
            else logf("FAIL pak: 하드링크 실패 %s (err=%lu)", u8(fd.cFileName).c_str(), GetLastError());
        }
        else
        {
            if (!pathExistsW(d))
            {
                pakManifestPut(targetSub, fd.cFileName, 0, nullptr);  // 이미 없음 -- 장부만 정리
                continue;
            }
            // ⚠ 우리 파일과 같은 실체일 때만 지운다 (사용자가 손수 넣은 것 보호).
            // v0.50(TODO 13): 실체가 달라도 "장부에 있고 + 크기 일치 + 소유자가
            // 이 모드"면 우리 잔재(업데이트로 원본이 바뀐 경우)로 보고 지운다.
            // 리뷰 D1: 소유자 검증 필수 -- 이름 충돌한 다른 모드의 산 링크 보호.
            bool ours = sameFileIdentity(s, d);
            if (!ours)
            {
                ULONGLONG msz = 0;
                std::string mOwn;
                ours = pakManifestFind(targetSub, fd.cFileName, &msz, &mOwn) &&
                       msz != 0 && msz == fileSizeOfW(d) &&
                       _stricmp(mOwn.c_str(), u8(owner).c_str()) == 0;
            }
            if (!ours)
            {
                logf("WARN pak: '%s' 는 우리가 건 링크가 아니다 -- 그대로 둔다",
                     u8(fd.cFileName).c_str());
                continue;
            }
            if (DeleteFileW(d))
            {
                ++done;
                pakManifestPut(targetSub, fd.cFileName, 0, nullptr);  // v0.50: 장부 말소
            }
            else logf("FAIL pak unlink: %s (err=%lu) -- 엔진이 이미 연 파일은 지워지지 않는다. "
                      "다음 부팅 초입(earlyGuard)에 재시도된다",
                      u8(fd.cFileName).c_str(), GetLastError());
            // ⚠ 실측 2026-08-13: 안전모드가 '껐다'던 pak 이 공유 위반으로 안 지워져
            // ~mods 에 남아 매 부팅 마운트되고 있었다 (조용한 실패 금지).
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return done;
}

// 토글 실제 적용 (파일 계층). 반환: 성공 여부와 무관하게 로그로 남긴다.
static void applyPluginState(PlgRow& r)
{
    wchar_t dst[MAX_PATH * 2];
    gameModsRoot(dst);
    lstrcatW(dst, r.name);
    wchar_t en[MAX_PATH * 2];
    swprintf(en, MAX_PATH * 2, L"%s\\enabled.txt", dst);
    // v0.27: pak 모드는 UE4SS 가 아니라 엔진이 마운트한다 -- Mods\ 진입점이 없고
    // Content\Paks\<타깃>\ 에 하드링크를 걸었다 뗀다. 상태는 plugins 쪽에 남긴다.
    if (r.pak)
    {
        // v0.40 5차: 기본은 ~mods (엔진이 에셋 교체용으로 마운트, 안전). LogicMods 는
        // BPModLoaderMod 가 '블루프린트 모드'로 로드하려 들어 에셋 팩이 크래시했다
        // (실측: _P 캐릭터 팩을 LogicMods 에 넣고 재시작 -> 크래시). 명시적으로
        // pak_target=logicmods 라고 적은 BP 모드만 그쪽으로 보낸다.
        bool toLogic = r.pakTarget[0] && _stricmp(r.pakTarget, "logicmods") == 0;
        const wchar_t* sub = toLogic ? L"LogicMods" : L"~mods";
        // 리뷰(v0.50): pak_target 을 바꾼 모드의 **반대쪽** 잔류 링크를 항상 걷는다.
        // 안 걷으면 이중 마운트가 영구화되고, LogicMods 방향 잔류는 BPModLoaderMod
        // 가 에셋 pak 을 BP 모드로 로드하는 실측 크래시를 재현한다. 없으면 no-op.
        linkPakFiles(r.rel[0] ? r.rel : r.name, toLogic ? L"~mods" : L"LogicMods", false, r.name);
        int nf = linkPakFiles(r.rel[0] ? r.rel : r.name, sub, r.on, r.name);
        wchar_t pen[MAX_PATH * 2];
        pluginSrcPath(pen, r.rel[0] ? r.rel : r.name);
        lstrcatW(pen, L"\\enabled.txt");
        if (r.on)
        {
            HANDLE h = CreateFileW(pen, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                   FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        }
        else DeleteFileW(pen);
        logf("plugin '%s': pak 모드 %s -- %s 에 파일 %d개 %s (게임 재시작 후 반영)",
             u8(r.name).c_str(), r.on ? "켬" : "끔", u8(sub).c_str(), nf,
             r.on ? "연결" : "해제");
        writeRuntimeFlag(r.name, r.rel, r.on);
        return;
    }
    if (r.on)
    {
        // v0.26: 사본을 만들지 않는다. Mods\<이름> 을 plugins\<상대경로> 로 가는
        // 정션으로 만든다 -- 모드는 plugins 한 곳에만 존재한다.
        if (!pathExistsW(dst))
        {
            wchar_t src[MAX_PATH * 2];
            pluginSrcPath(src, r.rel[0] ? r.rel : r.name);
            if (createJunction(dst, src))
                logf("plugin '%s': 정션 연결 %s -> %s (사본 없음)", u8(r.name).c_str(),
                     u8(dst).c_str(), u8(src).c_str());
            else
            {
                // 정션이 안 되는 환경(다른 볼륨/파일시스템)이면 켤 수 없다.
                // 조용히 사본을 만들지 않는다 -- 그건 이 버전이 없애려는 바로 그 문제다.
                logf("FAIL plugin '%s': 정션 생성 실패 -- 켤 수 없다", u8(r.name).c_str());
                r.on = false;
                return;
            }
        }
        else if (!isReparsePoint(dst))
            logf("WARN plugin '%s': Mods 에 옛 방식 사본(실제 폴더)이 이미 있다 -- 그대로 쓴다",
                 u8(r.name).c_str());
        HANDLE h = CreateFileW(en, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE)
        {
            CloseHandle(h);
            logf("plugin '%s': 켬 (enabled.txt 생성) -- 게임 재시작 후 로드", u8(r.name).c_str());
        }
        else logf("FAIL plugin '%s': enabled.txt 생성 실패", u8(r.name).c_str());
    }
    else
    {
        DeleteFileW(en);
        modsTxtDisable(r.name);
        removeModEntry(r.name);   // v0.26: 정션도 걷어낸다 -- 끄면 흔적이 남지 않는다
        logf("plugin '%s': 끔 (enabled.txt 삭제 + 진입점 제거)", u8(r.name).c_str());
    }
    // v0.8: 런타임 계약도 즉시 동기화 -- 이미 로드된 협조 모드는 ~1초 내 반응.
    // v0.10: 매니페스트 runtime_key 가 있으면 그것이 우선(내장표는 폴백),
    // 범용 신호 dsruntime.txt 도 항상 기록(서드파티 계약).
    const char* k = r.rtkey[0] ? r.rtkey : rtKeyOf(r.name);
    if (k)
    {
        rtSetKey(k, r.on);
        logf("plugin '%s': 런타임 키 %s=%d 동기화 (로드된 모드는 즉시 반영)",
             u8(r.name).c_str(), k, (int)r.on);
    }
    writeRuntimeFlag(r.name, r.rel, r.on);
}

/* v0.50: 진입점 정합 — "켜짐인데 재시작해도 안 뜨는" 문제의 진짜 뿌리 (넥서스 리포트 1번)

   UE4SS 는 `Mods\` 를 훑어 모드를 찾는다. 우리 방식에서 `Mods\<이름>` 은 plugins 쪽을
   가리키는 **정션**이고, 그 정션을 만드는 코드는 지금까지 **토글 한 곳뿐**이었다.
   그런데 서드파티 배포 zip 은 enabled.txt 를 담은 채 오는 경우가 많다 — 그런 모드를
   plugins 에 넣으면 패널은 켜짐으로 보이지만(plugins 쪽 enabled.txt 가 있으니)
   **진입점이 없어 UE4SS 는 그 모드의 존재조차 모른다.** 그대로 재시작해도 안 뜨고,
   패널에서 껐다 켜야(=정션 생성) 비로소 다음 재시작에 로드됐다. 리포터가 겪은 그것.

   여기서 켜짐인데 진입점이 없는 모드에 정션을 만들어 둔다 -> **이제는 그냥 재시작하면
   된다.** (이번 실행에는 못 싣는다 — UE4SS 는 부팅 때 이미 목록을 다 만들었다.
   그래서 '재시작 필요' 배지가 함께 필요하다.)
   ⚠ 반드시 bootGuard(안전모드) **뒤에** 부를 것 — 방금 끈 모드를 되살리면 안 된다. */
static void reconcileEntryPoints(const char* why)
{
    static PlgEnt ents[MM_MAX_PLUGINS];   // 게임/업데이트 스레드 전용 (스택 절약)
    int n = collectPlugins(ents, MM_MAX_PLUGINS);
    int made = 0;
    for (int i = 0; i < n; ++i)
    {
        if (ents[i].pak) continue;   // pak 은 진입점이 없다 (Content\Paks 하드링크)
        const wchar_t* rel = ents[i].rel[0] ? ents[i].rel : ents[i].name;
        wchar_t pen[MAX_PATH * 2];
        pluginSrcPath(pen, rel);
        lstrcatW(pen, L"\\enabled.txt");
        if (!pathExistsW(pen) && !modsTxtEnabled(ents[i].name)) continue;   // 꺼진 모드
        wchar_t dst[MAX_PATH * 2];
        gameModsRoot(dst);
        lstrcatW(dst, ents[i].name);
        if (pathExistsW(dst)) continue;   // 진입점 이미 있음 (정션이든 옛 사본이든)
        wchar_t src[MAX_PATH * 2];
        pluginSrcPath(src, rel);
        if (createJunction(dst, src))
        {
            ++made;
            logf("진입점 정합(%s): '%s' 켜짐인데 진입점이 없어 정션 생성 -- 다음 재시작에 로드",
                 why, u8(ents[i].name).c_str());
        }
        else
            logf("WARN 진입점 정합(%s): '%s' 정션 생성 실패 (err=%lu)", why,
                 u8(ents[i].name).c_str(), GetLastError());
    }
    if (made) logf("진입점 정합(%s): %d개 복구", why, made);
}

/* ======================= v0.23: 부팅 안전장치 (안전 모드) ==================

  왜 필요한가 (2026-08-06 실측 사고): 게임이 패치되자 하드코딩 주소를 쓰던 모드가
  **메인 메뉴에 닿기도 전에** 크래시를 냈다. 패널로 모드를 끄려면 메뉴에 들어가야
  하는데 그 전에 죽으니 손쓸 방법이 없는 잠김 상태가 된다.

  두 겹으로 막는다:
   - 다음 실행 보장: 로드 계층(enabled.txt)을 내려 둔다. 협조 여부와 무관하게
     UE4SS 가 아예 안 불러온다. **이것이 확실한 안전망이다.**
   - 이번 실행 최선: 런타임 계층(dsruntime.txt=0)도 같이 쓴다. 폴링하는 모드는
     위험한 일을 하기 전에 멎는다. 첫 틱이 사고 모드 시작보다 3.2초 빨랐다(실측).

  ⚠ 정직한 한계: **크래시가 모드 탓인지 덤프로 증명할 수 없다.** 오늘 사고도 UE
  콜스택에 모드 흔적이 전혀 없었다(게임 코드 프레임만). 그래서 "증명" 대신
  "직전 실행이 비정상 종료됐고 모드가 켜져 있었다" 는 사실만 근거로 삼고,
  판단은 사람에게 넘긴다(팝업 + 되돌릴 목록 기록).
*/
static ULONGLONG g_bootPrevLaunch = 0;
static std::atomic<bool> g_safeModePending{false};
static wchar_t g_safeModeText[768];   // v0.50: 끈 모드 목록 + 블랙박스 판정까지 (480 -> 768)
// v0.50: 팝업에 '실제로 끈 모드 목록'을 보여준다 -- 실측 2026-08-13: pak 모드가
// 크래시 원인이었는데 팝업은 '마지막 로거'(DsAutoFood)를 지목했다. pak 모드는
// 로그를 안 쓰므로 그 휴리스틱에 절대 못 걸린다 -- 목록이 정답, 로거는 참고.
static wchar_t g_safeModeList[320];
static bool g_safeModeHadPak = false;
static int g_safeModeListN = 0;
static wchar_t g_safeModePakList[200];  // v0.50: 블랙박스 '로딩 중 사망' 판정 때 지목할 pak 들

static ULONGLONG ftToU64(const FILETIME& ft)
{
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return u.QuadPart;
}

static ULONGLONG nowFileTime()
{
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return ftToU64(ft);
}

// 게임 실행파일(<Win64>\*-Win64-Shipping.exe)의 크기와 수정시각.
// 이름을 하드코딩하지 않는다 -- 패치로 바뀌어도 찾을 수 있게 패턴으로 잡는다.
static bool gameExeInfo(ULONGLONG* size, ULONGLONG* mtime)
{
    wchar_t dir[MAX_PATH * 2];
    gameModsRoot(dir);   // <ue4ss>/Mods/ 까지
    upDirs(dir, 2);      // 두 단계 위 = Win64 폴더
    wchar_t pat[MAX_PATH * 2];
    swprintf(pat, MAX_PATH * 2, L"%s*-Win64-Shipping.exe", dir);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    *size = ((ULONGLONG)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
    *mtime = ftToU64(fd.ftLastWriteTime);
    FindClose(h);
    return true;
}

// 지정 시각 이후에 생긴 크래시 흔적이 있는가 (UE4SS 덤프 + UE 크래시 리포트).
static bool crashArtifactNewerThan(ULONGLONG t, wchar_t* whatOut, int cap)
{
    if (!t) return false;
    bool found = false;
    for (int src = 0; src < 2 && !found; ++src)
    {
        wchar_t pat[MAX_PATH * 2];
        if (src == 0)
        {
            gameModsRoot(pat);
            upDirs(pat, 1);                       // 한 단계 위 = ue4ss 폴더
            lstrcatW(pat, L"crash_*.dmp");
        }
        else
        {
            gameModsRoot(pat);
            upDirs(pat, 4);                       // 네 단계 위 = DS 폴더
            lstrcatW(pat, L"Saved\\Crashes\\*");
        }
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW(pat, &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do
        {
            if (fd.cFileName[0] == L'.') continue;
            if (ftToU64(fd.ftLastWriteTime) > t)
            {
                found = true;
                lstrcpynW(whatOut, fd.cFileName, cap);
                break;
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return found;
}

static void bootStatePath(wchar_t* out)
{
    modRootPath(out);
    lstrcatW(out, L"bootstate.txt");
}

static void writeBootState(ULONGLONG launch, ULONGLONG exeSize, ULONGLONG exeMtime)
{
    char buf[256];
    int n = snprintf(buf, sizeof(buf), "launch=%llu\nexe_size=%llu\nexe_mtime=%llu\nmmver=%s\n",
                     (unsigned long long)launch, (unsigned long long)exeSize,
                     (unsigned long long)exeMtime, u8(MOD_VER_W).c_str());
    if (n <= 0) return;
    wchar_t p[MAX_PATH * 2];
    bootStatePath(p);
    HANDLE h = CreateFileW(p, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD wr = 0;
    WriteFile(h, buf, (DWORD)n, &wr, nullptr);
    CloseHandle(h);
}

static ULONGLONG bootStateGet(const std::string& data, const char* key)
{
    std::string k = std::string(key) + "=";
    size_t p = data.find(k);
    if (p == std::string::npos) return 0;
    return _strtoui64(data.c_str() + p + k.size(), nullptr, 10);
}

// 모든 플러그인을 강제로 내린다 (로드 계층 + 런타임 계층). 되돌릴 목록을 남긴다.
static int forceAllPluginsOff(const wchar_t* reason)
{
    static PlgEnt ents[MM_MAX_PLUGINS];
    int n = collectPlugins(ents, MM_MAX_PLUGINS);
    std::string list;
    int off = 0;
    g_safeModeList[0] = 0;      // v0.50: 팝업용 표시 목록 리셋
    g_safeModeHadPak = false;
    g_safeModeListN = 0;
    g_safeModePakList[0] = 0;
    for (int i = 0; i < n; ++i)
    {
        wchar_t en[MAX_PATH * 2];
        gameModsRoot(en);
        lstrcatW(en, ents[i].name);
        lstrcatW(en, L"\\enabled.txt");
        wchar_t pen[MAX_PATH * 2];   // v0.27: 정본은 plugins 쪽 (pak 모드는 여기만 있다)
        pluginSrcPath(pen, ents[i].rel[0] ? ents[i].rel : ents[i].name);
        lstrcatW(pen, L"\\enabled.txt");
        // v0.50(리뷰 D1): mods.txt "<이름> : 1" 로만 켜진 모드도 '켜져 있었다'로 센다
        // -- 패널 r.on 과 동일한 3중 판정. 빠뜨리면 조용히 꺼놓고 목록/팝업에서 누락.
        bool was = pathExistsW(en) || pathExistsW(pen) || modsTxtEnabled(ents[i].name);
        if (was) { DeleteFileW(en); DeleteFileW(pen); }
        modsTxtDisable(ents[i].name);
        removeModEntry(ents[i].name);  // v0.26: 정션 진입점까지 걷어낸다
        if (ents[i].pak)               // v0.27: pak 링크도 뗀다 (양쪽 타깃 모두)
        {
            linkPakFiles(ents[i].rel[0] ? ents[i].rel : ents[i].name, L"LogicMods", false, ents[i].name);
            linkPakFiles(ents[i].rel[0] ? ents[i].rel : ents[i].name, L"~mods", false, ents[i].name);
        }
        writeRuntimeFlag(ents[i].name, ents[i].rel, false);  // 이번 실행 최선 (협조 모드는 즉시 멎는다)
        if (const char* k = rtKeyOf(ents[i].name)) rtSetKey(k, false);
        if (was)
        {
            ++off;
            list += u8(ents[i].name) + "\n";
            // v0.50: 팝업용 표시 목록 -- 표시명(dsplugin.ini name=) + pak 표기
            if (ents[i].pak) g_safeModeHadPak = true;
            wchar_t disp[64];
            disp[0] = 0;
            {
                wchar_t mp[MAX_PATH * 2];
                pluginSrcPath(mp, ents[i].rel[0] ? ents[i].rel : ents[i].name);
                lstrcatW(mp, L"\\dsplugin.ini");
                std::string ini = readFileA(mp);
                if (!ini.empty())
                {
                    if (ini.size() >= 3 && ini.compare(0, 3, "\xEF\xBB\xBF") == 0) ini.erase(0, 3);
                    std::string nm = iniValueLang(ini, "plugin", "name");
                    if (!nm.empty()) utf8ToW(nm, disp, 64);
                }
            }
            if (!disp[0]) lstrcpynW(disp, ents[i].name, 64);
            if (ents[i].pak)
            {   // v0.50: 블랙박스가 '로딩 중 사망'을 판정하면 이 목록으로 pak 을 지목
                size_t pu = wcslen(g_safeModePakList);
                if (pu < 130)
                    swprintf(g_safeModePakList + pu, 200 - pu, L"%s『%s』",
                             pu ? L", " : L"", disp);
            }
            size_t used = wcslen(g_safeModeList);
            if (used < 220)
            {
                swprintf(g_safeModeList + used, 320 - used, L"%s『%s』%s",
                         used ? L"\n" : L"", disp, ents[i].pak ? L" [pak]" : L"");
                ++g_safeModeListN;
            }
        }
    }
    if (off > g_safeModeListN && g_safeModeListN > 0)
    {   // 목록 칸이 모자라 잘렸다 -- 나머지는 개수로
        size_t used = wcslen(g_safeModeList);
        swprintf(g_safeModeList + used, 320 - used,
                 TR(L"\n외 %d개", L"\nand %d more"), off - g_safeModeListN);
    }
    if (off)
    {
        wchar_t p[MAX_PATH * 2];
        modRootPath(p);
        lstrcatW(p, L"safemode_last.txt");
        std::string head = std::string("# 안전 모드로 꺼진 모드 (사유: ") + u8(reason) +
                           ")\n# 매니저 패널에서 다시 켤 수 있습니다.\n";
        head += list;
        HANDLE h = CreateFileW(p, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE)
        {
            DWORD wr = 0;
            WriteFile(h, head.data(), (DWORD)head.size(), &wr, nullptr);
            CloseHandle(h);
        }
    }
    logf("안전모드(%s): 플러그인 %d개 강제 끔 (검사 %d개)", u8(reason).c_str(), off, n);
    return off;
}

/* ======================= v0.50: 부트 블랙박스 =========================
   세션 진행 단계를 파일에 즉시 기록해, 크래시 후 "어디서 죽었나"를 판정한다
   (사용자 제안 2026-08-13). 기록 파일 = Mods\DsCppModManager\blackbox.txt,
   부팅 때 _prev 로 회전. 단계: ctor -> engine-init -> title <-> title-lost.
   + modstart 줄(UE4SS 로그에서 "Starting ... mod" 수집 = 정밀 브래킷)
   + lastlog.txt (UE4SS 로그 꼬리 4KB, 5초 주기 = 사망 직전 기록. 최대 5초 공백)
   분석(bootGuard)의 용의자 우선순위 (사용자 규칙):
   ① 모드 시작 단계 사망 -> 마지막 modstart 를 이름으로 지목
   ② 게임 로딩 진입 중 사망 -> (이름을 못 대므로) 이때만 pak 을 지목
   ③ 그 외 -> 기존 '마지막 기록 모드' 참고 표시 */
static char g_bbLastPhase[24] = {0};

static void blackboxPath(wchar_t* out, bool prev)
{
    modRootPath(out);
    lstrcatW(out, prev ? L"blackbox_prev.txt" : L"blackbox.txt");
}

static void blackboxAppend(const char* text)
{
    wchar_t p[MAX_PATH * 2];
    blackboxPath(p, false);
    // 리뷰 F6: 게임 스레드(phase)와 UpdateThread(modstart)가 겹칠 수 있다 --
    // FILE_SHARE_WRITE 로 열어야 한쪽이 공유 위반으로 조용히 유실되지 않는다
    // (FILE_APPEND_DATA 쓰기는 원자적 append 라 줄이 통째로 섞일 뿐 깨지지 않는다).
    HANDLE h = CreateFileW(p, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    char line[320];
    int n = snprintf(line, sizeof(line), "%02u:%02u:%02u.%03u %s\r\n",
                     st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, text);
    // 리뷰 F4: snprintf 는 잘렸을 때 '원래 길이'를 반환한다 -- 그대로 WriteFile 에
    // 넘기면 스택 밖을 읽는다(OOB). 버퍼 한도로 클램프.
    if (n > (int)sizeof(line) - 1) n = (int)sizeof(line) - 1;
    DWORD wr = 0;
    if (n > 0) WriteFile(h, line, (DWORD)n, &wr, nullptr);
    CloseHandle(h);
}

// 같은 단계 연속 기록은 생략 (게임/UE4SS 스레드 양쪽에서 불려도 무해 -- 최악은 중복 한 줄)
static void blackboxPhase(const char* ph)
{
    if (strcmp(g_bbLastPhase, ph) == 0) return;
    strncpy_s(g_bbLastPhase, ph, _TRUNCATE);
    char line[64];
    snprintf(line, sizeof(line), "phase %s", ph);
    blackboxAppend(line);
}

// 생성자 1회: 직전 세션 기록을 _prev 로 보존하고 새로 시작. lastlog 도 함께 보존
// (새 세션의 첫 스냅샷이 죽은 세션의 마지막 기록을 덮으면 안 된다).
static void blackboxRotate()
{
    wchar_t cur[MAX_PATH * 2], prv[MAX_PATH * 2];
    blackboxPath(cur, false);
    blackboxPath(prv, true);
    MoveFileExW(cur, prv, MOVEFILE_REPLACE_EXISTING);
    modRootPath(cur);
    lstrcatW(cur, L"lastlog.txt");
    modRootPath(prv);
    lstrcatW(prv, L"lastlog_prev.txt");
    MoveFileExW(cur, prv, MOVEFILE_REPLACE_EXISTING);
    char boot[80];
    snprintf(boot, sizeof(boot), "boot %s", u8(MOD_VER_W).c_str());
    blackboxAppend(boot);
    blackboxPhase("ctor");
}

// UE4SS 루트 (= Mods\ 의 부모) -- UE4SS.log 가 있는 곳
static void ue4ssRootPath(wchar_t* out)
{
    gameModsRoot(out);              // "...\ue4ss\Mods\"
    size_t len = wcslen(out);
    if (len > 5) out[len - 5] = 0;  // "Mods\" 걷어내기
}

// on_update 5초 주기 (UpdateThread, 파일 I/O 전용):
// 1) 타이틀 확인 전 -- UE4SS 로그에서 "Starting <종류> mod '<이름>'" 줄을 수집해
//    새 것만 blackbox 에 modstart 로 기록 (정밀 브래킷: 어느 모드까지 시작됐나)
// 2) 항상 -- 로그 꼬리 4KB 를 lastlog.txt 로 스냅샷 (사망 직전 기록, 5초 공백 한계)
static void blackboxTick(ULONGLONG now)
{
    static ULONGLONG s_last = 0;
    if (s_last && now - s_last < 5000) return;
    s_last = now;
    static ULONGLONG s_first = 0;
    if (!s_first) s_first = now;
    wchar_t lp[MAX_PATH * 2];
    ue4ssRootPath(lp);
    lstrcatW(lp, L"UE4SS.log");
    static bool s_bracketDone = false;
    static int s_seen = 0;
    if (!s_bracketDone && (strcmp(g_bbLastPhase, "title") == 0 || now - s_first > 90000))
        s_bracketDone = true;   // 타이틀 도달 = 모드 시작 전부 완료 (또는 90초 상한)
    if (!s_bracketDone)
    {
        // 줄 단위로 두 형식을 다 잡는다 (라이브 실측 20:58 -- 매니저 관리 모드는
        // 전부 enabled.txt 계층이라 "Mod 'X' has enabled.txt, starting mod." 형식):
        //   ① "Starting <종류> mod '<이름>'"          (mods.txt 계층 = UE4SS 내장들)
        //   ② "Mod '<이름>' has enabled.txt, starting mod."  (유저 모드 전부)
        // 줄 순서 = 시간 순서 -- '마지막으로 시작된 모드' 판정의 근거.
        std::string log = readFileA(lp);   // 부팅 초의 로그는 작다
        size_t pos = 0;
        int idx = 0;
        while (pos < log.size())
        {
            size_t eol = log.find('\n', pos);
            if (eol == std::string::npos) eol = log.size();
            std::string ln = log.substr(pos, eol - pos);
            std::string entry;
            size_t a = ln.find("Starting ");
            if (a != std::string::npos)
            {
                size_t k = ln.find(" mod '", a);
                // 리뷰 F3: 종류 토막은 "Lua"/"C++" 뿐 -- 길이 1~16 밖이면 위조 줄
                if (k != std::string::npos && k > a + 9 && k - (a + 9) <= 16)
                {
                    size_t ns = k + 6;
                    size_t ne = ln.find('\'', ns);
                    if (ne != std::string::npos && ne - ns < 100)
                        entry = "modstart " + ln.substr(a + 9, k - (a + 9)) +
                                " " + ln.substr(ns, ne - ns);
                }
            }
            if (entry.empty())
            {
                size_t m = ln.find("Mod '");
                if (m != std::string::npos)
                {
                    size_t ns = m + 5;
                    size_t ne = ln.find('\'', ns);
                    if (ne != std::string::npos && ne - ns < 100 &&
                        ln.compare(ne, 31, "' has enabled.txt, starting mod") == 0)
                        entry = "modstart enabled " + ln.substr(ns, ne - ns);
                }
            }
            if (!entry.empty())
            {
                ++idx;
                if (idx > s_seen)
                {
                    s_seen = idx;
                    blackboxAppend(entry.c_str());
                }
            }
            pos = eol + 1;
        }
    }
    HANDLE h = CreateFileW(lp, GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER sz;
    if (GetFileSizeEx(h, &sz) && sz.QuadPart > 0)
    {
        const DWORD WANT = 4096;
        DWORD take = (sz.QuadPart < (LONGLONG)WANT) ? (DWORD)sz.QuadPart : WANT;
        LARGE_INTEGER ps;
        ps.QuadPart = sz.QuadPart - take;
        SetFilePointerEx(h, ps, nullptr, FILE_BEGIN);
        std::string buf;
        buf.resize(take);
        DWORD got = 0;
        if (take && ReadFile(h, &buf[0], take, &got, nullptr) && got)
        {
            wchar_t sp[MAX_PATH * 2];
            modRootPath(sp);
            lstrcatW(sp, L"lastlog.txt");
            HANDLE o = CreateFileW(sp, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                   CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (o != INVALID_HANDLE_VALUE)
            {
                DWORD wr = 0;
                WriteFile(o, buf.data(), got, &wr, nullptr);
                CloseHandle(o);
            }
        }
    }
    CloseHandle(h);
}

/* v0.50: 초입 안전장치 (생성자 시점, 순수 파일 I/O 전용)
   UE4SS 는 C++ 모드를 엔진 PreInit 보다 먼저 로드한다 (실측 2026-08-13: 콘솔 생성
   +10ms 에 'Starting C++ mod', 엔진 pak 마운트는 그보다 한참 뒤). 이 틈에:
   1) 직전 크래시/게임 패치 감지 시 -- 켜진 pak 의 링크를 **엔진이 열기 전에** 뗀다.
      첫 틱(bootGuard) 시점엔 엔진이 pak 을 이미 열고 있어 DeleteFileW 가 공유 위반으로
      실패한다 (실측: '껐다'던 pak 이 ~mods 에 남아 매 부팅 마운트 -- 크래시 반복의 원인).
      끄기 본조치(enabled.txt·팝업·bootstate 갱신)는 종전대로 bootGuard 가 한다.
   2) 항상 -- 꺼진 pak 모드의 잔류 링크 청소 (1의 과거 실패분 자가 치유).
   ⚠ 이 함수에서 UE API 호출 절대 금지 (엔진 미초기화 상태). bootstate 를 다시 쓰지
   않는다 (bootGuard 의 감지 조건을 소모하면 안 된다). */
static void earlyBootGuard()
{
    wchar_t p[MAX_PATH * 2];
    bootStatePath(p);
    std::string prev = readFileA(p);
    ULONGLONG prevLaunch = bootStateGet(prev, "launch");
    ULONGLONG prevSize = bootStateGet(prev, "exe_size");
    ULONGLONG prevMtime = bootStateGet(prev, "exe_mtime");
    ULONGLONG size = 0, mtime = 0;
    bool haveExe = gameExeInfo(&size, &mtime);
    bool updated = haveExe && prevSize && (size != prevSize || mtime != prevMtime);
    wchar_t crashName[128] = {0};
    bool crashed = prevLaunch != 0 && crashArtifactNewerThan(prevLaunch, crashName, 128);

    static PlgEnt ents[MM_MAX_PLUGINS];
    int n = collectPlugins(ents, MM_MAX_PLUGINS);
    int cut = 0, swept = 0;
    for (int i = 0; i < n; ++i)
    {
        if (!ents[i].pak) continue;
        const wchar_t* rel = ents[i].rel[0] ? ents[i].rel : ents[i].name;
        wchar_t pen[MAX_PATH * 2];
        pluginSrcPath(pen, rel);
        lstrcatW(pen, L"\\enabled.txt");
        wchar_t en[MAX_PATH * 2];
        gameModsRoot(en);
        lstrcatW(en, ents[i].name);
        lstrcatW(en, L"\\enabled.txt");
        bool on = pathExistsW(pen) || pathExistsW(en) || modsTxtEnabled(ents[i].name);
        if (on && (updated || crashed))
        {   // 위험 감지 -- 링크만 선제 해제 (identity 검증은 linkPakFiles 내부에서)
            int k = linkPakFiles(rel, L"LogicMods", false, ents[i].name) +
                    linkPakFiles(rel, L"~mods", false, ents[i].name);
            if (k)
            {
                cut += k;
                logf("earlyGuard: pak '%s' 링크 %d개 선제 해제 (%s)", u8(ents[i].name).c_str(),
                     k, updated ? "게임 패치" : "직전 크래시");
            }
        }
        else if (!on)
        {   // 잔류 링크 청소 -- 과거 첫 틱 삭제 실패(공유 위반)의 자가 치유
            int k = linkPakFiles(rel, L"LogicMods", false, ents[i].name) +
                    linkPakFiles(rel, L"~mods", false, ents[i].name);
            if (k)
            {
                swept += k;
                logf("earlyGuard: 꺼진 pak '%s' 의 잔류 링크 %d개 청소", u8(ents[i].name).c_str(), k);
            }
        }
    }
    // v0.50(TODO 13): 장부 스윕 -- 원본 순회로는 못 잡는 잔재(업데이트로 파일명/실체가
    // 바뀐 옛 링크)를 장부 기준으로 걷는다. 엔진이 pak 을 열기 전이라 삭제가 성립한다.
    {
        wchar_t mp[MAX_PATH * 2];
        pakManifestPath(mp);
        std::string mdata = readFileA(mp);
        int stale = 0;
        size_t pos = 0;
        while (pos < mdata.size())
        {
            size_t eol = mdata.find('\n', pos);
            if (eol == std::string::npos) eol = mdata.size();
            std::string ln = mdata.substr(pos, eol - pos);
            pos = eol + 1;
            while (!ln.empty() && (ln.back() == '\r' || ln.back() == ' ')) ln.pop_back();
            if (ln.empty()) continue;
            size_t b1 = ln.find('|');
            size_t b2 = (b1 == std::string::npos) ? std::string::npos : ln.find('|', b1 + 1);
            if (b2 == std::string::npos) continue;
            std::string key = ln.substr(0, b1);           // "<sub>\<파일>"
            ULONGLONG msz = _strtoui64(ln.c_str() + b1 + 1, nullptr, 10);
            std::string ownerU = ln.substr(b2 + 1);
            size_t sl = key.find('\\');
            if (sl == std::string::npos) continue;
            std::string subU = key.substr(0, sl);
            std::string fileU = key.substr(sl + 1);
            // 손편집 대비 가드: sub 화이트리스트 + 파일명에 경로 문자 금지
            if (subU != "~mods" && subU != "LogicMods") continue;
            if (fileU.empty() || fileU.find('\\') != std::string::npos ||
                fileU.find('/') != std::string::npos || fileU.find("..") != std::string::npos)
                continue;
            wchar_t subW[16], fileW[MAX_PATH], ownerW[64];
            utf8ToW(subU, subW, 16);
            utf8ToW(fileU, fileW, MAX_PATH);
            utf8ToW(ownerU, ownerW, 64);
            wchar_t tgt[MAX_PATH * 2];
            contentPaksPath(tgt, subW);
            lstrcatW(tgt, fileW);
            if (!pathExistsW(tgt))
            {   // 파일이 이미 없다 -- 장부만 정리
                pakManifestPut(subW, fileW, 0, nullptr);
                continue;
            }
            // 소유 모드의 현재 상태·원본과 대조
            bool ownerFound = false, ownerOn = false, live = false, srcHas = false;
            wchar_t src2[MAX_PATH * 2] = {0};
            for (int i = 0; i < n; ++i)
            {
                if (_wcsicmp(ents[i].name, ownerW) != 0) continue;
                ownerFound = true;
                const wchar_t* rel2 = ents[i].rel[0] ? ents[i].rel : ents[i].name;
                wchar_t pen2[MAX_PATH * 2];
                pluginSrcPath(pen2, rel2);
                lstrcatW(pen2, L"\\enabled.txt");
                wchar_t en2[MAX_PATH * 2];
                gameModsRoot(en2);
                lstrcatW(en2, ents[i].name);
                lstrcatW(en2, L"\\enabled.txt");
                ownerOn = pathExistsW(pen2) || pathExistsW(en2) || modsTxtEnabled(ents[i].name);
                pluginSrcPath(src2, rel2);
                lstrcatW(src2, L"\\");
                lstrcatW(src2, fileW);
                srcHas = pathExistsW(src2);
                live = srcHas && sameFileIdentity(src2, tgt);  // 현역 링크인가
                break;
            }
            // 리뷰 D3: '스캔에 없음'과 '모드가 진짜 없음'을 구분한다 -- 스캔이
            // 상한(50)에 걸렸거나, 폴더가 실제로 남아 있으면 판정 불가 = 건드리지 않는다.
            if (!ownerFound)
            {
                if (n >= MM_MAX_PLUGINS) continue;
                wchar_t ownDir[MAX_PATH * 2];
                pluginSrcPath(ownDir, ownerW);
                if (pathExistsW(ownDir)) continue;
                // 폴더도 없다 = 모드 삭제됨 -- 아래 크기 검증 후 청소
            }
            if (ownerOn && live) continue;   // 켜져 있고 현역 -- 정상
            // 꺼졌거나, 켜져 있어도 원본과 실체가 다르면(업데이트 잔재) 걷는다.
            // 검증: 크기가 장부와 일치할 때만 (사용자 파일 보호 잔여 규칙).
            if (msz != 0 && msz == fileSizeOfW(tgt) && DeleteFileW(tgt))
            {
                pakManifestPut(subW, fileW, 0, nullptr);
                // 리뷰 D4: 켜진 모드의 같은 이름 새 원본이 있으면 그 자리에서 재링크
                // -- 안 하면 켜진 모드가 pak 없이 부팅한다 (토글 전까지 안 걸림).
                if (ownerOn && srcHas && CreateHardLinkW(tgt, src2, nullptr))
                {
                    pakManifestPut(subW, fileW, fileSizeOfW(src2), ownerW);
                    logf("earlyGuard: 켜진 pak 구판 잔재 교체(재링크) %s\\%s (소유: %s)",
                         subU.c_str(), fileU.c_str(), ownerU.c_str());
                }
                else
                {
                    ++stale;
                    logf("earlyGuard: 장부 잔재 청소 %s\\%s (소유: %s)", subU.c_str(),
                         fileU.c_str(), ownerU.c_str());
                }
            }
        }
        swept += stale;
    }
    if (cut || swept || crashed || updated)
        logf("earlyGuard: 선제해제 %d 잔류청소 %d (크래시=%d 패치=%d)", cut, swept,
             (int)crashed, (int)updated);
}

/* v0.24: '다잉 메시지' -- 크래시 직전에 **마지막으로 기록을 남긴 모드**를 찾는다.

   근거(2026-08-06 실측): 크래시 14:11:31.8 직전, autofood.txt 가 14:11:29 로
   유일하게 늦게 기록됐다(다른 모드는 전부 14:11:25). 범인을 정확히 가리켰다.

   ⚠ 한계를 분명히 한다: 이것은 **정황이지 증거가 아니다.** 자주 기록하는 모드가
   항상 '마지막' 이 될 수 있다. 그래서 팝업에도 "확정 원인이 아니다" 를 함께 띄우고,
   전체 순위는 로그에만 남긴다. 판단은 사람이 한다.
*/
struct Suspect
{
    wchar_t name[64];
    wchar_t label[64];
    ULONGLONG when;
    wchar_t lastLine[160];
};

// 매니저가 쓰는 계약 파일은 제외한다 -- 우리 손이 닿은 시각이 모드의 활동으로 오인된다.
static bool isContractFile(const wchar_t* n)
{
    static const wchar_t* const F[] = {
        L"dsoptions.txt", L"dsruntime.txt", L"enabled.txt", L"dsnotify.txt",
        L"version.txt", L"bootstate.txt", L"safemode_last.txt",
    };
    for (const wchar_t* f : F)
        if (_wcsicmp(n, f) == 0) return true;
    size_t len = wcslen(n);           // *_cmd.txt = 사람이 쓰는 명령 파일
    return len > 8 && _wcsicmp(n + len - 8, L"_cmd.txt") == 0;
}

// 파일 끝에서 마지막 비어 있지 않은 줄 (UTF-8 로그 가정)
static void readTailLine(const wchar_t* path, wchar_t* out, int cap)
{
    out[0] = 0;
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER sz;
    if (GetFileSizeEx(h, &sz) && sz.QuadPart > 0)
    {
        const DWORD WANT = 2048;
        DWORD take = (sz.QuadPart < (LONGLONG)WANT) ? (DWORD)sz.QuadPart : WANT;
        LARGE_INTEGER pos;
        pos.QuadPart = sz.QuadPart - take;
        SetFilePointerEx(h, pos, nullptr, FILE_BEGIN);
        std::string buf;
        buf.resize(take);
        DWORD got = 0;
        if (ReadFile(h, &buf[0], take, &got, nullptr) && got)
        {
            buf.resize(got);
            while (!buf.empty() && (buf.back() == '\n' || buf.back() == '\r' || buf.back() == ' '))
                buf.pop_back();
            size_t nl = buf.find_last_of("\r\n");
            std::string line = (nl == std::string::npos) ? buf : buf.substr(nl + 1);
            if (line.size() > 150) line.resize(150);
            utf8ToW(line, out, cap);
        }
    }
    CloseHandle(h);
}

// since 이후에 기록된 모드 로그 중 가장 늦은 것을 찾는다.
static bool findCrashSuspect(ULONGLONG since, Suspect* out)
{
    if (!since) return false;
    static PlgEnt ents[MM_MAX_PLUGINS];
    int n = collectPlugins(ents, MM_MAX_PLUGINS);
    Suspect best;
    memset(&best, 0, sizeof(best));
    std::string rank;
    for (int i = 0; i < n; ++i)
    {
        ULONGLONG modBest = 0;
        wchar_t modBestPath[MAX_PATH * 2] = {0};
        // v0.26: 로그의 실제 거처는 plugins 쪽이다(정션이라 모드가 거기에 쓴다).
        // 옛 방식 사본이 남은 사람도 있으니 Mods 쪽도 함께 본다.
        for (int sub = 0; sub < 4; ++sub)
        {
            wchar_t pat[MAX_PATH * 2];
            if (sub < 2) pluginSrcPath(pat, ents[i].rel[0] ? ents[i].rel : ents[i].name);
            else
            {
                gameModsRoot(pat);
                lstrcatW(pat, ents[i].name);
            }
            lstrcatW(pat, (sub % 2) == 0 ? L"\\*.txt" : L"\\dlls\\*.txt");
            WIN32_FIND_DATAW fd;
            HANDLE h = FindFirstFileW(pat, &fd);
            if (h == INVALID_HANDLE_VALUE) continue;
            do
            {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                if (isContractFile(fd.cFileName)) continue;
                ULONGLONG t = ftToU64(fd.ftLastWriteTime);
                if (t <= since || t <= modBest) continue;
                modBest = t;
                gameModsRoot(modBestPath);
                lstrcatW(modBestPath, ents[i].name);
                lstrcatW(modBestPath, sub == 0 ? L"\\" : L"\\dlls\\");
                lstrcatW(modBestPath, fd.cFileName);
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        if (!modBest) continue;
        char line[128];
        snprintf(line, sizeof(line), "  %s (%llu)\n", u8(ents[i].name).c_str(),
                 (unsigned long long)modBest);
        rank += line;
        if (modBest > best.when)
        {
            memset(&best, 0, sizeof(best));
            best.when = modBest;
            lstrcpynW(best.name, ents[i].name, 64);
            readTailLine(modBestPath, best.lastLine, 160);
            wchar_t ini[MAX_PATH * 2];
            pluginSrcPath(ini, ents[i].rel[0] ? ents[i].rel : ents[i].name);
            lstrcatW(ini, L"\\dsplugin.ini");
            std::string d = readFileA(ini);
            if (!d.empty())
            {
                if (d.size() >= 3 && d.compare(0, 3, "\xEF\xBB\xBF") == 0) d.erase(0, 3);
                std::string nm = iniValueLang(d, "plugin", "name");
                if (!nm.empty()) utf8ToW(nm, best.label, 64);
            }
        }
    }
    if (!best.when) return false;
    logf("다잉 메시지 후보 (직전 실행 이후 기록한 모드):\n%s", rank.c_str());
    *out = best;
    return true;
}

// FILETIME -> "HH:MM:SS" (로컬)
static void ftToClock(ULONGLONG t, wchar_t* out, int cap)
{
    out[0] = 0;
    FILETIME ft, lf;
    ULARGE_INTEGER u;
    u.QuadPart = t;
    ft.dwLowDateTime = u.LowPart;
    ft.dwHighDateTime = u.HighPart;
    SYSTEMTIME st;
    if (FileTimeToLocalFileTime(&ft, &lf) && FileTimeToSystemTime(&lf, &st))
        swprintf(out, cap, L"%02d:%02d:%02d", st.wHour, st.wMinute, st.wSecond);
}

// 게임 시작 직후 1회 (UpdateThread, 파일 I/O 뿐이라 안전).
static void bootGuard()
{
    ensureLang();   // v0.40: 안전모드 팝업 문구의 언어를 먼저 확정한다
    wchar_t p[MAX_PATH * 2];
    bootStatePath(p);
    std::string prev = readFileA(p);
    ULONGLONG prevLaunch = bootStateGet(prev, "launch");
    ULONGLONG prevSize = bootStateGet(prev, "exe_size");
    ULONGLONG prevMtime = bootStateGet(prev, "exe_mtime");
    g_bootPrevLaunch = prevLaunch;
    // v0.40: 매니저 자체 업데이트는 게임 패치와 구분해 기록만 한다 (조치·팝업 없음)
    {
        size_t mp = prev.find("mmver=");
        if (mp != std::string::npos)
        {
            size_t e = prev.find('\n', mp);
            std::string pv = prev.substr(mp + 6, (e == std::string::npos ? prev.size() : e) - mp - 6);
            std::string cv = u8(MOD_VER_W);
            if (!pv.empty() && pv != cv)
                logf("bootGuard: 모드매니저 업데이트 (%s -> %s) -- 게임 패치와 무관, 조치 없음",
                     pv.c_str(), cv.c_str());
        }
    }

    ULONGLONG size = 0, mtime = 0;
    bool haveExe = gameExeInfo(&size, &mtime);

    bool updated = haveExe && prevSize && (size != prevSize || mtime != prevMtime);
    wchar_t crashName[128] = {0};
    bool crashed = crashArtifactNewerThan(prevLaunch, crashName, 128);

    logf("bootGuard: 이전실행=%llu 게임패치=%d 직전크래시=%d(%s) exe=%llu/%llu",
         (unsigned long long)prevLaunch, (int)updated, (int)crashed,
         crashName[0] ? u8(crashName).c_str() : "-",
         (unsigned long long)size, (unsigned long long)mtime);

    // 이번 실행 시각을 먼저 기록한다 -- 지금부터 생기는 덤프가 '이번 실행의 크래시'다.
    writeBootState(nowFileTime(), size, mtime);

    if (!updated && !crashed) return;
    const wchar_t* reason = updated ? L"게임 업데이트 감지" : L"직전 실행 비정상 종료";
    int off = forceAllPluginsOff(reason);
    if (off <= 0)
    {
        logf("bootGuard: 켜져 있던 플러그인이 없어 조치 없음");
        return;
    }
    // v0.50: 팝업의 주인공은 **실제로 끈 모드 목록**이다 (safemode_last 와 같은 내용).
    // '마지막 로거'는 참고 정보로 강등 -- pak 모드는 로그가 없어 그 휴리스틱에 절대
    // 못 걸리는데, 실측(2026-08-13)에서 pak 이 범인인 크래시에 DsAutoFood 가
    // 지목되는 오해를 낳았다.
    const wchar_t* pakNote = g_safeModeHadPak
        ? TR(L"\n\n※ 게임 업데이트 직후에는 pak(외형·콘텐츠) 모드가 크래시 원인인 경우가 많습니다.",
             L"\n\n* Right after a game update, pak (appearance/content) mods are a common crash cause.")
        : L"";
    if (updated)
    {
        swprintf(g_safeModeText, 768,
                 TR(L"게임이 업데이트되어 안전을 위해 다음 모드 %d개를 껐습니다:\n%s%s\n\n"
                    L"모드가 새 버전에 맞는지 확인한 뒤 매니저에서 다시 켜 주세요.",
                    L"The game was updated, so these %d mod(s) were switched off for safety:\n%s%s\n\n"
                    L"Check that your mods match the new version, then re-enable them here."),
                 off, g_safeModeList, pakNote);
    }
    else
    {
        // v0.50: 블랙박스 판독 -- 용의자 우선순위 (사용자 규칙 2026-08-13):
        // ① 모드 시작 단계 사망 = 마지막 modstart 이름 지목
        // ② 게임 로딩 진입 중(title-lost) 사망 = 이름을 못 대므로 이때만 pak 지목
        // ③ 그 외 = 기존 '마지막 기록 모드' 참고
        wchar_t verdict[400] = {0};   // 리뷰 F5: EN pak 목록 최악 ~311 -- 280 은 잘렸다
        bool vNamed = false;          // 용의자를 이름으로 댔는가 (댔으면 아래 '참고' 생략)
        {
            wchar_t bp[MAX_PATH * 2];
            blackboxPath(bp, true);   // 생성자에서 _prev 로 회전해 둔 죽은 세션 기록
            std::string bb = readFileA(bp);
            if (!bb.empty())
            {
                std::string lastPhase, lastStart;
                size_t bpos = 0;
                while (bpos < bb.size())
                {
                    size_t eol = bb.find('\n', bpos);
                    if (eol == std::string::npos) eol = bb.size();
                    std::string ln = bb.substr(bpos, eol - bpos);
                    while (!ln.empty() && (ln.back() == '\r' || ln.back() == ' ')) ln.pop_back();
                    size_t sp1 = ln.find(' ');   // "HH:MM:SS.mmm " 접두 제거
                    std::string body = (sp1 == std::string::npos) ? ln : ln.substr(sp1 + 1);
                    if (body.compare(0, 6, "phase ") == 0) lastPhase = body.substr(6);
                    else if (body.compare(0, 9, "modstart ") == 0) lastStart = body.substr(9);
                    bpos = eol + 1;
                }
                // 리뷰 F1: title-lost 는 '로딩 진입'이지만 인게임 내내 유지된다 --
                // 사망 시각(lastlog_prev mtime, 5초 스냅샷이라 ±5초)과 title-lost
                // 시각(blackbox_prev mtime = 마지막 append)을 비교해, 5분 넘게
                // 지났으면 '로딩 중'이 아니라 '플레이 중' 사망으로 판정한다.
                bool inGame = false;
                if (lastPhase == "title-lost")
                {
                    ULONGLONG bbM = 0, llM = 0;
                    WIN32_FILE_ATTRIBUTE_DATA fa;
                    if (GetFileAttributesExW(bp, GetFileExInfoStandard, &fa))
                        bbM = ((ULONGLONG)fa.ftLastWriteTime.dwHighDateTime << 32) |
                              fa.ftLastWriteTime.dwLowDateTime;
                    wchar_t lp2[MAX_PATH * 2];
                    modRootPath(lp2);
                    lstrcatW(lp2, L"lastlog_prev.txt");
                    if (GetFileAttributesExW(lp2, GetFileExInfoStandard, &fa))
                        llM = ((ULONGLONG)fa.ftLastWriteTime.dwHighDateTime << 32) |
                              fa.ftLastWriteTime.dwLowDateTime;
                    if (bbM && llM && llM > bbM && (llM - bbM) > 300ULL * 10000000ULL)
                        inGame = true;   // FILETIME 10^7 틱 = 1초, 300초 초과 = 플레이 중
                }
                logf("블랙박스: 마지막 단계='%s' 마지막 시작='%s' inGame=%d",
                     lastPhase.c_str(), lastStart.c_str(), (int)inGame);
                if (lastPhase == "ctor" || lastPhase == "engine-init")
                {
                    if (!lastStart.empty())
                    {
                        wchar_t wn[128];
                        utf8ToW(lastStart, wn, 128);
                        // 리뷰 F2: 수집이 5초 주기라 '그 다음 모드'가 범인일 수도 있다
                        swprintf(verdict, 400,
                                 TR(L"\n\n블랙박스: 부팅(모드 시작) 단계에서 죽었습니다.\n"
                                    L"마지막으로 시작이 기록된 모드: 『%s』 (유력)\n"
                                    L"※ 기록이 5초 주기라 그 다음 모드가 시작 직후 죽었을 수도 있습니다.",
                                    L"\n\nBlack box: died during startup (mod loading).\n"
                                    L"Last mod recorded as starting: '%s' (likely)\n"
                                    L"* Logging is 5s-periodic -- the NEXT mod to start may be the one."), wn);
                        vNamed = true;
                    }
                    else if (g_safeModePakList[0])
                    {   // 라이브 실측(20:44): pak 은 엔진 초기화 때 마운트·초기 로드된다 --
                        // 모드 시작 전 사망 + pak 켜져 있었음 = pak 이 첫 번째 용의자.
                        // (사용자 규칙: 블랙박스가 이름을 못 대면 pak 을 지목한다)
                        swprintf(verdict, 400,
                                 TR(L"\n\n블랙박스: 게임 부팅(엔진 초기화·pak 마운트) 중에 죽었습니다.\n"
                                    L"이 단계 크래시는 pak(외형·콘텐츠) 모드가 흔한 원인입니다 -- 유력: %s",
                                    L"\n\nBlack box: died during engine boot (pak mounting/early load).\n"
                                    L"pak (appearance/content) mods are a common cause here -- likely: %s"),
                                 g_safeModePakList);
                        vNamed = true;
                    }
                    else
                        swprintf(verdict, 400,
                                 TR(L"\n\n블랙박스: 부팅 극초기 또는 모드 시작 기록이 수집되기 전에 죽었습니다.",
                                    L"\n\nBlack box: died very early in boot, or before mod-start logging began."));
                }
                else if (lastPhase == "title-lost" && !inGame)
                {
                    if (g_safeModePakList[0])
                    {
                        swprintf(verdict, 400,
                                 TR(L"\n\n블랙박스: 게임 시작/로딩 중에 죽었습니다.\n"
                                    L"이때 죽으면 대개 pak(외형·콘텐츠) 모드입니다 -- 유력: %s",
                                    L"\n\nBlack box: died while loading into the game.\n"
                                    L"This usually points to pak (appearance/content) mods -- likely: %s"),
                                 g_safeModePakList);
                        vNamed = true;
                    }
                    else
                        swprintf(verdict, 400,
                                 TR(L"\n\n블랙박스: 게임 시작/로딩 중에 죽었습니다.",
                                    L"\n\nBlack box: died while loading into the game."));
                }
                else if (lastPhase == "title-lost")   // inGame -- 이름 못 댐, 아래 '참고'가 이어진다
                    swprintf(verdict, 400,
                             TR(L"\n\n블랙박스: 게임 플레이 중에 죽었습니다 (로딩 단계 아님).",
                                L"\n\nBlack box: died during gameplay (not while loading)."));
                // lastPhase == "title": 타이틀 화면 사망 -- 이름 못 댐, 아래 참고로
            }
        }
        // v0.24 '마지막으로 기록을 남긴 모드'는 **로그에만** 남긴다 (사용자 지시
        // 2026-08-13: 지금은 기록을 남기는 모드가 자작 모드뿐이라, 팝업에 넣으면
        // 항상 자작 모드가 뒤집어쓴다 -- 생태계가 생기기 전엔 팝업 금지).
        (void)vNamed;
        Suspect s;
        if (findCrashSuspect(prevLaunch, &s))
        {
            wchar_t clock[16] = {0};
            ftToClock(s.when, clock, 16);
            logf("다잉 메시지: '%s' 가 %s 에 마지막 기록 -- \"%s\" (팝업 미표시)",
                 u8(s.name).c_str(), u8(clock).c_str(), u8(s.lastLine).c_str());
        }
        swprintf(g_safeModeText, 768,
                 TR(L"직전 실행이 비정상 종료되어 다음 모드 %d개를 껐습니다:\n%s%s",
                    L"The previous run ended abnormally; these %d mod(s) were switched off:\n%s%s"),
                 off, g_safeModeList,
                 verdict[0] ? verdict : pakNote);
    }
    g_safeModePending.store(true, std::memory_order_relaxed);
}

static bool setBrushColor(UObject* border, LinColor c, const char* tag)
{
    return callBytes(border, L"SetBrushColor", &c, 16, tag);
}

// TextBlock 은 FSlateColor(20B = FLinearColor + ColorUseRule 바이트 0) -- umg.lua 실측 규약
static bool setTextColor(UObject* tb, LinColor c, const char* tag)
{
    unsigned char buf[20] = {};
    memcpy(buf, &c, 16);
    buf[16] = 0;  // UseColor_Specified
    return callBytes(tb, L"SetColorAndOpacity", buf, 20, tag);
}

// 패널용 SetText (setLabel 과 달리 실패해도 영구 중단 없음)
static bool setTextOn(UObject* tb, const wchar_t* s, const char* tag)
{
    UFunction* fn = fnOf(tb, L"SetText", tag);
    if (!fn || !parmsExact(fn, 24, tag, false)) return false;
    if (RC::Unreal::FText::StaticSize() != 24) return false;
    RC::Unreal::FText txt(s);  // 의도적 미세 누수 (ue4ss_abi.hpp 규칙)
    PB pb;
    memcpy(pb.b, &txt, 24);
    if (!peGuard(tb, fn, pb.b)) { logf("FAIL %s: SetText SEH", tag); return false; }
    return true;
}

/*
  v0.9: 클릭 히트 판정 -- IsHovered 는 마우스가 "움직일 때만" 갱신돼서, 방금 뜬
  패널 위에서 가만히 누르면 false 를 준다(Lua 시절 실측 함정, 로그에서 클릭
  4~5회 소비로 재확인). 그래서 좌표 산술 판정을 1차로 쓴다.

  실측 사양(덤프+역어셈블):
  - FGeometry = 56B, 전부 float. 유효 필드는 Size(0x00) + AccumulatedRenderTransform
    (0x1C..0x28) + Translation(0x2C,0x30). ⚠ AbsolutePosition/Position(0x0C~0x18)은
    기본 생성자가 안 채워 쓰레기가 남을 수 있으니 쓰지 않는다.
  - UWidget:GetCachedGeometry parms 56, 반환 @0x00
  - USlateBlueprintLibrary:IsUnderLocation(Geometry@0x00, AbsoluteCoord@0x38(FVector2D
    =double 2개)) -> bool@0x48, parms 80, static(CDO 호출)
  - UWidgetLayoutLibrary:GetMousePositionOnPlatform() -> FVector2D@0x00, parms 16
    (절대 데스크톱 좌표 = Slate 절대 좌표계와 동일 기준)
*/
static UObject* g_sbl = nullptr;   // SlateBlueprintLibrary CDO (패널 수명 동안 캐시)
static UObject* g_wll = nullptr;   // WidgetLayoutLibrary CDO
static double g_mouseX = 0, g_mouseY = 0;
static bool g_mouseOk = false;

// 클릭 시점에 1회: 마우스 절대 좌표 확보.
// ⚠ 라이브 실측(22:32): 엔진 GetMousePositionOnPlatform 값이 위젯 기하와 같은
// 좌표계다(그 값 1260,1162 가 행 사각형 913..1675 x 1161..1235 안에 정확히 들어감).
// 반면 Win32 ScreenToClient(GetForegroundWindow) 는 Y 가 ~195px 어긋났다 --
// 창 클라이언트 원점이 다르다. 그래서 엔진 값이 정본, Win32 는 폴백이다.
static void sampleMouse()
{
    g_mouseOk = false;
    if (g_wll)
    {
        UFunction* fn = fnOf(g_wll, L"GetMousePositionOnPlatform", "mouse");
        if (fn && (int)fn->GetParmsSize() >= 16)
        {
            PB pb;
            if (peGuard(g_wll, fn, pb.b))
            {
                int ro = (int)fn->GetReturnValueOffset();
                memcpy(&g_mouseX, pb.b + ro, 8);
                memcpy(&g_mouseY, pb.b + ro + 8, 8);
                g_mouseOk = true;
                return;
            }
        }
    }
    POINT pt;  // 폴백: 화면 절대 좌표 (창 변환 없이 -- 전체화면이면 동일 기준)
    if (GetCursorPos(&pt))
    {
        g_mouseX = (double)pt.x;
        g_mouseY = (double)pt.y;
        g_mouseOk = true;
    }
}

// 좌표 산술 판정: -1 불가 / 0 밖 / 1 안.
// 두 경로를 함께 쓴다: ① 엔진 IsUnderLocation ② 직접 사각형 계산(역행렬).
// 둘 중 하나라도 참이면 히트 -- 좌표계/구현 차이에 대한 이중 안전망이다.
// 클릭 순간에만 호출되므로(호버 폴링은 isHoveredSlate) 상세 로그를 남긴다.
static int hitByGeometry(UObject* w, const char* tag)
{
    if (!g_mouseOk) return -1;
    UFunction* fg = fnOf(w, L"GetCachedGeometry", tag);
    if (!fg) return -1;
    if ((int)fg->GetParmsSize() < 56 || (int)fg->GetReturnValueOffset() != 0)
    {
        logf("WARN %s: GetCachedGeometry parms=%d ret=%d -- 좌표 판정 생략", tag,
             (int)fg->GetParmsSize(), (int)fg->GetReturnValueOffset());
        return -1;
    }
    PB geo;
    if (!peGuard(w, fg, geo.b)) return -1;
    float sx = 0, sy = 0, m00 = 0, m01 = 0, m10 = 0, m11 = 0, tx = 0, ty = 0;
    memcpy(&sx, geo.b + 0x00, 4);
    memcpy(&sy, geo.b + 0x04, 4);
    memcpy(&m00, geo.b + 0x1C, 4);
    memcpy(&m01, geo.b + 0x20, 4);
    memcpy(&m10, geo.b + 0x24, 4);
    memcpy(&m11, geo.b + 0x28, 4);
    memcpy(&tx, geo.b + 0x2C, 4);
    memcpy(&ty, geo.b + 0x30, 4);

    // ② 직접 계산: 로컬(0,0)/(sx,sy) 를 절대 좌표로 변환해 사각형 판정
    //    Abs = L.x*[M00,M01] + L.y*[M10,M11] + T   (실측 역어셈블 공식)
    int manual = -1;
    if (sx > 0.f && sy > 0.f)
    {
        double cx[4], cy[4];
        const float lx[4] = {0, sx, 0, sx};
        const float ly[4] = {0, 0, sy, sy};
        for (int i = 0; i < 4; ++i)
        {
            cx[i] = (double)lx[i] * m00 + (double)ly[i] * m10 + tx;
            cy[i] = (double)lx[i] * m01 + (double)ly[i] * m11 + ty;
        }
        double x0 = cx[0], x1 = cx[0], y0 = cy[0], y1 = cy[0];
        for (int i = 1; i < 4; ++i)
        {
            if (cx[i] < x0) x0 = cx[i];
            if (cx[i] > x1) x1 = cx[i];
            if (cy[i] < y0) y0 = cy[i];
            if (cy[i] > y1) y1 = cy[i];
        }
        manual = (g_mouseX >= x0 && g_mouseX <= x1 && g_mouseY >= y0 && g_mouseY <= y1) ? 1 : 0;
        logf("hit(%s): rect=(%.0f,%.0f)-(%.0f,%.0f) mouse=(%.0f,%.0f) 직접=%d "
             "size=%.0fx%.0f M=[%.2f %.2f %.2f %.2f] T=(%.0f,%.0f)",
             tag, x0, y0, x1, y1, g_mouseX, g_mouseY, manual, sx, sy, m00, m01, m10, m11, tx, ty);
    }
    else logf("hit(%s): size=0 (레이아웃 전) -- IsHovered 폴백", tag);

    // ① 엔진 함수 (실측 ParmsSize=73: Geo 56 + FVector2D 16 + bool 1)
    int engine = -1;
    if (g_sbl)
    {
        UFunction* fu = fnOf(g_sbl, L"IsUnderLocation", tag);
        if (fu)
        {
            int ps = (int)fu->GetParmsSize();
            int ro = (int)fu->GetReturnValueOffset();
            if (ps >= 73 && ps <= 96 && ro >= 72 && ro < ps)
            {
                PB pb;
                memcpy(pb.b + 0, geo.b, 56);
                memcpy(pb.b + 56, &g_mouseX, 8);
                memcpy(pb.b + 64, &g_mouseY, 8);
                if (peGuard(g_sbl, fu, pb.b)) engine = pb.b[ro] ? 1 : 0;
            }
            else logf("WARN %s: IsUnderLocation parms=%d ret=%d", tag, ps, ro);
        }
    }
    if (engine >= 0 && manual >= 0 && engine != manual)
        logf("hit(%s): 엔진=%d 직접=%d 불일치 -- OR 채택", tag, engine, manual);

    if (engine == 1 || manual == 1) return 1;
    if (engine == 0 || manual == 0) return 0;
    return -1;
}

// v0.15: 위젯의 절대 화면 사각형 (hitByGeometry 의 직접 계산 경로만, 무로그).
// 콤보 드롭다운의 앵커 배치와 30Hz 호버 페인팅에 쓴다 -- 매 틱 로그 금지.
static bool widgetRectAbs(UObject* w, double* ox0, double* oy0, double* ox1, double* oy1)
{
    UFunction* fg = fnOf(w, L"GetCachedGeometry", "rectq");
    if (!fg || (int)fg->GetParmsSize() < 56 || (int)fg->GetReturnValueOffset() != 0) return false;
    PB geo;
    if (!peGuard(w, fg, geo.b)) return false;
    float sx = 0, sy = 0, m00 = 0, m01 = 0, m10 = 0, m11 = 0, tx = 0, ty = 0;
    memcpy(&sx, geo.b + 0x00, 4);
    memcpy(&sy, geo.b + 0x04, 4);
    memcpy(&m00, geo.b + 0x1C, 4);
    memcpy(&m01, geo.b + 0x20, 4);
    memcpy(&m10, geo.b + 0x24, 4);
    memcpy(&m11, geo.b + 0x28, 4);
    memcpy(&tx, geo.b + 0x2C, 4);
    memcpy(&ty, geo.b + 0x30, 4);
    if (sx <= 0.f || sy <= 0.f) return false;  // 레이아웃 전
    const float lx[4] = {0, sx, 0, sx};
    const float ly[4] = {0, 0, sy, sy};
    double x0 = 0, x1 = 0, y0 = 0, y1 = 0;
    for (int i = 0; i < 4; ++i)
    {
        double cx = (double)lx[i] * m00 + (double)ly[i] * m10 + tx;
        double cy = (double)lx[i] * m01 + (double)ly[i] * m11 + ty;
        if (i == 0) { x0 = x1 = cx; y0 = y1 = cy; }
        else
        {
            if (cx < x0) x0 = cx;
            if (cx > x1) x1 = cx;
            if (cy < y0) y0 = cy;
            if (cy > y1) y1 = cy;
        }
    }
    *ox0 = x0; *oy0 = y0; *ox1 = x1; *oy1 = y1;
    return true;
}

// 무로그 좌표 히트: -1 판정불가 / 0 밖 / 1 안. sampleMouse() 선행 필요.
static int hitPtQuiet(UObject* w)
{
    if (!g_mouseOk) return -1;
    double x0, y0, x1, y1;
    if (!widgetRectAbs(w, &x0, &y0, &x1, &y1)) return -1;
    return (g_mouseX >= x0 && g_mouseX <= x1 && g_mouseY >= y0 && g_mouseY <= y1) ? 1 : 0;
}

// v0.29: 슬라이더 시각 갱신 -- 채움/남은 칸의 폭만 바꾼다.
// 손잡이는 두 칸 사이에 끼어 있으므로 겹침(오버레이) 없이 저절로 밀린다.
static void paintSlider(void* fill, void* rest, void* tx, int val, int mn, int mx)
{
    float usable = SLD_TRACK - SLD_KNOB;
    float t = (mx > mn) ? (float)(val - mn) / (float)(mx - mn) : 0.0f;
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    float fw = usable * t, rw = usable - fw;
    if (fill) callBytes(reinterpret_cast<UObject*>(fill), L"SetWidthOverride", &fw, 4, "sld.fill");
    if (rest) callBytes(reinterpret_cast<UObject*>(rest), L"SetWidthOverride", &rw, 4, "sld.rest");
    if (tx)
    {
        wchar_t b[16];
        swprintf(b, 16, L"%d", val);
        setTextOn(reinterpret_cast<UObject*>(tx), b, "sld.val");
    }
}

// 커서 X -> 값. 손잡이 '중심'이 커서를 따라오도록 양끝에서 반지름만큼 안쪽으로 편다.
// 화면 배율은 모르지만 SLD_KNOB/SLD_TRACK 비율은 배율과 무관하다 -- 실측 폭에 곱한다.
// -1 = 사각형을 못 재서 판정 불가 (슬라이더 값은 항상 0 이상이라 sentinel 로 안전).
static int sliderValueAt(void* hs, int mn, int mx)
{
    double x0, y0, x1, y1;
    if (!hs || !widgetRectAbs(reinterpret_cast<UObject*>(hs), &x0, &y0, &x1, &y1)) return -1;
    double half = (x1 - x0) * (SLD_KNOB / (2.0 * SLD_TRACK));
    double a = x0 + half, b = x1 - half;
    if (b <= a) return mn;
    double t = (g_mouseX - a) / (b - a);
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    return mn + (int)((double)(mx - mn) * t + 0.5);
}

// 히트 판정: 산술(1차) -> IsHovered(2차 보조). -1 실패 / 0 아님 / 1 히트.
static int isHovered(UObject* w, const char* tag)
{
    int g = hitByGeometry(w, tag);
    if (g >= 0) return g;
    UFunction* fn = fnOf(w, L"IsHovered", tag);
    if (!fn || !parmsExact(fn, 1, tag, false)) return -1;
    if ((int)fn->GetReturnValueOffset() != 0) return -1;
    PB pb;
    if (!peGuard(w, fn, pb.b)) { logf("FAIL %s: IsHovered SEH", tag); return -1; }
    return pb.b[0] ? 1 : 0;
}

// 메뉴 항목 호버(연속 폴링)는 Slate 의 hover 를 그대로 쓴다 -- 하이라이트는
// 마우스 이동과 동기여야 자연스럽고, 산술 판정은 클릭 순간에만 필요하다.
static int isHoveredSlate(UObject* w, const char* tag)
{
    UFunction* fn = fnOf(w, L"IsHovered", tag);
    if (!fn || !parmsExact(fn, 1, tag, false)) return -1;
    if ((int)fn->GetReturnValueOffset() != 0) return -1;
    PB pb;
    if (!peGuard(w, fn, pb.b)) { logf("FAIL %s: IsHovered SEH", tag); return -1; }
    return pb.b[0] ? 1 : 0;
}

// AddChild(parms16) -> 슬롯 반환 (null = 실패)
static UObject* addChildTo(UObject* panel, UObject* child, const char* tag)
{
    UFunction* fn = fnOf(panel, L"AddChild", tag);
    if (!fn || !parmsExact(fn, 16, tag, false)) return nullptr;
    PB pb;
    memcpy(pb.b, &child, 8);
    if (!peGuard(panel, fn, pb.b)) { logf("FAIL %s: AddChild SEH", tag); return nullptr; }
    void* slot = nullptr;
    memcpy(&slot, pb.b + (int)fn->GetReturnValueOffset(), 8);
    if (!slot) logf("WARN %s: AddChild slot=null", tag);
    return reinterpret_cast<UObject*>(slot);
}

// 슬롯 정렬 강제 -- 이 빌드의 컨테이너는 기본으로 자식을 늘리지 않는다(실측).
// h/v: 0=Fill 1=Left/Top 2=Center 3=Right/Bottom, -1=건드리지 않음
static void slotAlign(UObject* slot, int h, int v, const char* tag)
{
    if (!slot) return;
    unsigned char b;
    if (h >= 0) { b = (unsigned char)h; callBytes(slot, L"SetHorizontalAlignment", &b, 1, tag); }
    if (v >= 0) { b = (unsigned char)v; callBytes(slot, L"SetVerticalAlignment", &b, 1, tag); }
}

static void slotPad(UObject* slot, float l, float t, float r, float b, const char* tag)
{
    if (!slot) return;
    float m[4] = {l, t, r, b};
    callBytes(slot, L"SetPadding", m, 16, tag);
}

// HBox 슬롯 가로 채움 (FSlateChildSize {Value=1.0f, SizeRule=1(Fill)} = 8B)
static void slotFillWidth(UObject* slot, const char* tag)
{
    if (!slot) return;
    unsigned char sz[8] = {0x00, 0x00, 0x80, 0x3F, 0x01, 0x00, 0x00, 0x00};
    callBytes(slot, L"SetSize", sz, 8, tag);
}

// v0.6: 토글 페인팅 -- 선택 알약 = 브러시 틴트 흰색, 비선택 = 틴트 알파 0
// (브러시만 투명해지고 자식 텍스트와 히트테스트는 그대로 유지된다)
static void paintToggle(PlgRow& r)
{
    const LinColor selText = {0.97f, 0.97f, 0.98f, 1.0f};
    const LinColor dimText = {0.52f, 0.55f, 0.57f, 1.0f};
    if (r.offPill)
        setBrushColor(reinterpret_cast<UObject*>(r.offPill),
                      r.on ? LinColor{1, 1, 1, 0} : LinColor{1, 1, 1, 1}, "tgl.off");
    if (r.onPill)
        setBrushColor(reinterpret_cast<UObject*>(r.onPill),
                      r.on ? LinColor{1, 1, 1, 1} : LinColor{1, 1, 1, 0}, "tgl.on");
    if (r.offText)
        setTextColor(reinterpret_cast<UObject*>(r.offText), r.on ? dimText : selText, "tgl.off");
    if (r.onText)
        setTextColor(reinterpret_cast<UObject*>(r.onText), r.on ? selText : dimText, "tgl.on");
}

// v0.10: bool 옵션의 토글 페인팅 (int 값 텍스트는 클릭 시 직접 갱신)
static void paintOpt(PlgOpt& o)
{
    if (o.type != 0) return;
    const LinColor selText = {0.97f, 0.97f, 0.98f, 1.0f};
    const LinColor dimText = {0.52f, 0.55f, 0.57f, 1.0f};
    bool on = o.val != 0;
    if (o.boolOff)
        setBrushColor(reinterpret_cast<UObject*>(o.boolOff),
                      on ? LinColor{1, 1, 1, 0} : LinColor{1, 1, 1, 1}, "opt.off");
    if (o.boolOn)
        setBrushColor(reinterpret_cast<UObject*>(o.boolOn),
                      on ? LinColor{1, 1, 1, 1} : LinColor{1, 1, 1, 0}, "opt.on");
    if (o.boolOffText)
        setTextColor(reinterpret_cast<UObject*>(o.boolOffText), on ? dimText : selText, "opt.off");
    if (o.boolOnText)
        setTextColor(reinterpret_cast<UObject*>(o.boolOnText), on ? selText : dimText, "opt.on");
}

// v0.13: 조건부 옵션 가시성 -- parent 체인을 따라가며 판정 (깊이 4 제한)
static int optIndexOf(PlgRow& r, const char* key)
{
    for (int i = 0; i < r.optN; ++i)
        if (_stricmp(r.opt[i].key, key) == 0) return i;
    return -1;
}

static bool optVisible(PlgRow& r, int oi, int depth = 0)
{
    PlgOpt& o = r.opt[oi];
    if (!o.parent[0] || depth > 4) return true;
    int pi = optIndexOf(r, o.parent);
    if (pi < 0 || pi == oi) return true;  // 부모 키 오기재 -> 그냥 표시 (조용한 실종 방지)
    bool cond = o.hasParentValue ? (r.opt[pi].val == o.parentValue) : (r.opt[pi].val != 0);
    return cond && optVisible(r, pi, depth + 1);
}

static int optDepth(PlgRow& r, int oi, int depth = 0)
{
    PlgOpt& o = r.opt[oi];
    if (!o.parent[0] || depth > 4)
        return (o.iniBacked && !o.iniHeader) ? depth + 1 : depth;  // v0.50: ini 자식 한 단 들여씀
    int pi = optIndexOf(r, o.parent);
    if (pi < 0 || pi == oi) return depth;
    return optDepth(r, pi, depth + 1);
}

static bool optHasChildren(PlgRow& r, int oi)
{
    for (int i = 0; i < r.optN; ++i)
        if (i != oi && _stricmp(r.opt[i].parent, r.opt[oi].key) == 0) return true;
    return false;
}

// v0.16: 옵션 접힘 UI 의 '펼침' 상태 -- 패널 재구축(needReopen)을 넘어 세션 동안
// 유지해야 하므로 PlgRow(패널 수명) 밖의 폴더명 키 목록으로 기억한다.
static wchar_t g_expandedMods[MM_MAX_PLUGINS][64];
static int g_expandedN = 0;

static bool isExpanded(const wchar_t* name)
{
    for (int i = 0; i < g_expandedN; ++i)
        if (_wcsicmp(g_expandedMods[i], name) == 0) return true;
    return false;
}

static void setExpanded(const wchar_t* name, bool on)
{
    for (int i = 0; i < g_expandedN; ++i)
    {
        if (_wcsicmp(g_expandedMods[i], name) != 0) continue;
        if (!on) lstrcpynW(g_expandedMods[i], g_expandedMods[--g_expandedN], 64);
        return;
    }
    if (on && g_expandedN < MM_MAX_PLUGINS) lstrcpynW(g_expandedMods[g_expandedN++], name, 64);
}

// ======================= v0.2: 메뉴 항목 호버 ==============================

// Lua setSelected 실측 규약: ImgSelected 는 항상 0(Visible), OverlaySelected 만
// 4(호버 켬, SelfHitTestInvisible) / 1(끔, Collapsed) 로 토글.
static void setSelectedRow(UObject* row, bool on)
{
    if (UObject* img = readObjProp(row, L"ImgSelected", "hover"))
        setVisibility(img, 0, "hover.img");
    if (UObject* ovl = readObjProp(row, L"OverlaySelected", "hover"))
        setVisibility(ovl, on ? 4 : 1, "hover.ovl");
}

// 호버 켤 때 형제 항목들의 하이라이트 해제 (게임이 하나 남겨두는 경우 방지).
// 마우스가 진짜 항목으로 돌아가면 게임이 스스로 다시 켠다 -- 싸울 일 없음(실측).
static void clearOthers(UObject* mine)
{
    for (void* s : g_siblings)
    {
        if (!s || s == (void*)mine) continue;
        UObject* o = reinterpret_cast<UObject*>(s);
        if (UObject* ovl = readObjProp(o, L"OverlaySelected", "clear"))
            setVisibility(ovl, 1, "clear.ovl");
    }
}

// 사운드 후보 호출 (시각 효과 없음이 실측 확인된 best-effort -- Lua 와 동일)
static void pokeSounds(UObject* row, bool on)
{
    UFunction* fn = fnOf(row, L"BP_OnItemSelectionChanged", "sound");
    if (fn && (int)fn->GetParmsSize() == 1)
    {
        PB pb;
        pb.b[0] = on ? 1 : 0;
        peGuard(row, fn, pb.b);
    }
    if (UObject* btn = readObjProp(row, L"TitleMenuBtn", "sound"))
    {
        UFunction* ev = fnOf(btn, on ? L"OnHoveredExpandEvent" : L"OnUnHoveredExpandEvent", "sound");
        if (ev && (int)ev->GetParmsSize() == 0)
        {
            PB pb;
            peGuard(btn, ev, pb.b);
        }
    }
}

// v0.40 10차: 가상 정지 해제 -- 클론 표시를 내리고 게임이 선택 중이던 항목을
// 복원한다. 입력 복구는 쥔 키(B/ESC/방향)를 놓은 뒤로 지연 -- 눌린 채 돌려주면
// 게임이 그 키를 받아 오동작한다(B = 타이틀에서 게임 종료, 실측 사고).
static void exitVstop(UObject* clone, bool waitDirRelease)
{
    g_vstop = false;
    setSelectedRow(clone, false);
    pokeSounds(clone, false);
    if (g_vstopGameSel)
    {   // 동결 중엔 게임이 못 그리므로 이 항목은 살아 있다(타이틀 파괴 시 cloneLost 경로)
        UObject* gs = reinterpret_cast<UObject*>(g_vstopGameSel);
        if (UObject* ovl = readObjProp(gs, L"OverlaySelected", "vstop"))
            setVisibility(ovl, 4, "vstop.ovl");
        pokeSounds(gs, true);
        g_vstopGameSel = nullptr;
    }
    g_restoreInputPending = true;
    g_restoreWaitDir = waitDirRelease;
    logf("vstop: 해제 (%s)", waitDirRelease ? "방향 이동" : "취소");
}

// ======================= v0.2: 상태 리셋 ===================================

static void closePanel(const char* why);  // 아래 패널 절에 정의
static bool panelInputMode(bool uiOnly, UObject* focusW = nullptr);  // 아래 정의 (기본값은 여기만)
                                                                      // 반환 = 전환 호출 실제 성공
static int adoptAndAlign(UObject* clone, UObject* box, UObject* exitW);  // v0.40 10차: 아래 정의
static bool ensureRoot();  // v0.50c: 아래 정의 (키퍼 재선정 잇기)
static bool cloneInAnyRootArray(UObject* clone);  // v0.50e: 상시 감시용 (아래 정의)
static void probeNavGraph(UObject* root);   // v0.40 10차g: 내비 그래프 재탐색용

static void cloneLost(const char* why)
{

    logf("클론 소실/리셋 (%s) -- 상태 초기화", why);
    // ⚠ 리뷰 확정: 패널을 참조만 버리면 스크림이 클릭을 영원히 먹는 소프트락.
    // 가드된 닫기를 "시도"한다 -- 진짜 죽은 패널이면 SEH 가 잡고 참조만 버려진다.
    if (g_panel || g_panelOpen) closePanel("cloneLost 연쇄 정리");
    if (g_vstop || g_restoreInputPending)
    {   // 10차b: 가상 정지/지연 복구가 잠근 게임 입력을 반드시 돌려준다(소프트락
        // 방지). 클론 사후에는 펌프의 지연 복구 지점에 도달하지 못하므로(mc 게이트)
        // 여기서 즉시 복구가 차악. g_vstopGameSel 은 역참조 금지(TRAPS X).
        g_vstop = false;
        g_vstopGameSel = nullptr;
        g_restoreInputPending = false;
        g_restoreWaitDir = false;
        panelInputMode(false);
    }
    // v0.40 9차b: 편입 철거 -- 리플렉션 TArray 는 GC 강참조라 옛 클론이 배열에
    // 남으면 영원히 listed(재삽입 봉쇄) + 세대마다 누적된다(리뷰 확정). 뿌리
    // 생존을 FindAllOf 멤버십으로 먼저 확인하고(죽은 객체 무접촉), 포인터 "값"
    // 비교만으로 제거한다 -- 클론/뿌리 역참조 없음, 읽고 쓰기는 전부 SEH 가드.
    // 뿌리가 unlisted 면 배열도 함께 죽었으므로 손대지 않는다(TRAPS X).
    if (g_rootAdopted && g_root && g_rootArrOff >= 0)
    {
        void* myc = g_myClone.load();
        bool rootAlive = false;
        std::vector<UObject*> lay;
        UOG::FindAllOf(L"DLayerTitleGame_C", lay);
        for (UObject* o : lay)
            if ((void*)o == g_root) { rootAlive = true; break; }
        if (rootAlive && myc)
        {
            struct { void* data; int num; int max; } a{};
            if (readBytesGuard(g_root, g_rootArrOff, &a, 16) &&
                a.data && a.num > 0 && a.num <= 16)
            {
                int w = 0;
                bool okRW = true;
                for (int r = 0; r < a.num && okRW; ++r)
                {
                    void* e = nullptr;
                    okRW = readPtrGuard(a.data, r * 8, &e);
                    if (okRW && e != myc)
                    {
                        if (w != r) okRW = writePtrGuard(a.data, w * 8, e);
                        ++w;
                    }
                }
                if (okRW && w != a.num)
                {
                    writeIntGuard(g_root, g_rootArrOff + 8, w);
                    logf("adopt: 배열에서 클론 철거 (num %d->%d)", a.num, w);
                }
            }
        }
    }
    g_myClone = nullptr;
    g_doneBox = nullptr;
    g_lastHover = false;
    g_padIcon = nullptr;     // 11b: 클론과 함께 죽는다 (죽은 위젯 토글 금지)
    g_padIconSpacer = nullptr;
    g_reorderPending = false;   // v0.50: 위치 원복 예약도 메뉴와 함께 취소
    g_reorderRepair = false;    // v0.50(C-2): Exit 수리 모드도 메뉴와 함께 리셋
    g_reorderHaveSaved = false;
    g_reorderOpenTicks = 0;
    blackboxPhase("title-lost");  // v0.50: 블랙박스 -- 게임시작/이어하기 로딩 진입(추정)
    g_siblings.clear();
    g_root = nullptr;        // 뿌리/편입 상태는 메뉴와 함께 죽는다
    g_rootArrOff = -1;
    g_rootSnapOff = -1;
    g_rootAdopted = false;
    g_navObj = nullptr;      // 10차f: 내비 기계 포인터도 메뉴와 함께 죽는다
    g_navGraphN = 0;
    g_menuCloneSel = false;
    ++g_menuGen;             // 폴링의 지역 정적(s_prevIdx/s_snap)도 이 번호로 리셋된다
}

static bool cloneStillListed(void* mc)
{
    std::vector<UObject*> all;
    UOG::FindAllOf(L"DUWG_TitleMenu_C", all);
    for (UObject* o : all)
        if ((void*)o == mc) return true;
    return false;
}

// ======================= v0.2: 패널 ========================================

static void closePopup(const char* why);  // v0.7 팝업 절에 정의
static void closeCombo(const char* why);  // v0.15 콤보 절에 정의
static void closeColor(const char* why);  // v0.28 색상 절에 정의

// v0.17: ScrollBox 오프셋 읽기/쓰기 (UScrollBox::Get/SetScrollOffset, parms 4 실측 대조)
static float readScrollOffset(void* sb)
{
    if (!sb) return -1.0f;
    UObject* w = reinterpret_cast<UObject*>(sb);
    UFunction* fn = fnOf(w, L"GetScrollOffset", "scroll.get");
    if (!fn || (int)fn->GetParmsSize() != 4 || (int)fn->GetReturnValueOffset() != 0) return -1.0f;
    PB pb;
    if (!peGuard(w, fn, pb.b)) return -1.0f;
    float v = 0;
    memcpy(&v, pb.b, 4);
    return v;
}

static void writeScrollOffset(void* sb, float v)
{
    if (!sb) return;
    callBytes(reinterpret_cast<UObject*>(sb), L"SetScrollOffset", &v, 4, "scroll.set");
}

// 재구축 직전 호출: 현재 스크롤 위치를 기억해 둔다 (실패하면 조용히 포기 = 맨 위)
static void captureScroll()
{
    float v = readScrollOffset(g_scrollBox);
    g_pendScroll = v > 0.5f ? v : -1.0f;  // 맨 위였으면 되돌릴 것도 없다
}

// v0.18: 스크롤바 표시/숨김. UScrollBox 의 함수명이 빌드마다 대소문자가 갈려서
// 두 철자를 다 시도하고, 없으면 프로퍼티(ESlateVisibility 바이트)를 직접 쓴다
// (생성 직후 = Slate 빌드 전이면 그것만으로도 먹는다).
//
// ⚠ v0.18.1 실측: 숨김을 **Collapsed(1)로 하면 내용이 좌우로 밀린다.**
// SScrollBox 는 스크롤바를 콘텐츠 **옆 슬롯**에 두므로, Collapsed 는 그 폭을
// 회수했다가 표시될 때 다시 뺏는다 = 스크롤할 때마다 UI 가 흔들린다.
// **Hidden(2)** 은 "그리지 않되 자리는 그대로" 라서 레이아웃이 절대 변하지 않는다.
// (자체 오버레이 스크롤바는 Overlay 위젯 추가가 이 빌드에서 전멸한 전례가 있어
//  선택하지 않았다 -- TRAPS/CLAUDE 의 UMG 함정)
static void setScrollbarShown(void* sb, bool on)
{
    if (!sb) return;
    UObject* w = reinterpret_cast<UObject*>(sb);
    const wchar_t* cands[2] = {L"SetScrollbarVisibility", L"SetScrollBarVisibility"};
    unsigned char v = on ? 0 : 2;  // ESlateVisibility 0=Visible 2=Hidden(자리 유지)
    for (int i = 0; i < 2; ++i)
    {
        UFunction* fn = fnOf(w, cands[i], "sb.vis");
        if (!fn || (int)fn->GetParmsSize() != 1) continue;
        PB pb;
        pb.b[0] = v;
        if (peGuard(w, fn, pb.b)) return;
    }
    int off = propOffset(w, L"ScrollBarVisibility", 1, "sb.visProp");
    if (off >= 0) writeByteGuard(w, off, v);
}

// 오프셋이 스스로 바뀌었으면(= 사용자가 휠을 굴렸으면) 활동으로 보고 스크롤바를
// 띄운다. 우리가 드래그로 바꾼 경우도 force 로 같은 경로를 탄다. 1초 뒤 숨김.
static void tickScrollbar(int which, void* sb, ULONGLONG now, bool force)
{
    if (!sb)
    {
        g_sbShown[which] = false;
        g_sbLastOff[which] = -1.0f;
        return;
    }
    float cur = readScrollOffset(sb);
    bool moved = force || (g_sbLastOff[which] >= 0.0f && cur >= 0.0f &&
                           (cur > g_sbLastOff[which] + 0.5f || cur < g_sbLastOff[which] - 0.5f));
    g_sbLastOff[which] = cur;
    if (moved)
    {
        g_sbHideAt[which] = now + 1000;
        if (!g_sbShown[which])
        {
            setScrollbarShown(sb, true);
            g_sbShown[which] = true;
        }
    }
    else if (g_sbShown[which] && now > g_sbHideAt[which])
    {
        setScrollbarShown(sb, false);
        g_sbShown[which] = false;
    }
}

// v0.40: 패널이 열린 동안 게임(타이틀 메뉴)이 패드 입력을 처리하지 못하게 막는다.
// 실측 사고: 패널 조작 중에도 게임이 패드를 받아, 내부 선택이 '나가기'면 A 가
// 게임을 조용히 종료시켰다(크래시 아님 -- 정상 DETACH). B 는 설정창을 열었다.
static bool panelInputMode(bool uiOnly, UObject* focusW)
{   // 반환 = SetInputMode 호출이 실제로 나갔는가. vstop 은 실패 시 진입을 포기해야
    // 한다(동결 안 됐는데 동결 전제로 굴면 게임과 이중 실행 -- 리뷰 확정).
    UObject* pc = reinterpret_cast<UObject*>(g_pcPanel);
    if (!pc) return false;
    UObject* lib = findObj(L"/Script/UMG.Default__WidgetBlueprintLibrary", "inputmode");
    if (!lib) return false;
    UFunction* fn = fnOf(lib, uiOnly ? L"SetInputMode_UIOnlyEx" : L"SetInputMode_GameAndUIEx", "inputmode");
    if (!fn) return false;
    int ps = (int)fn->GetParmsSize();
    if (ps < 18 || ps > 32)
    {
        logf("WARN inputmode: parmSize %d 예상 밖 -- 생략", ps);
        return false;
    }
    PB pb;
    memset(pb.b, 0, 64);
    memcpy(pb.b + 0, &pc, 8);
    void* w = uiOnly ? (focusW ? (void*)focusW : g_panel) : nullptr;   // UIOnly = 포커스 대상
    memcpy(pb.b + 8, &w, 8);
    pb.b[16] = 0;                            // EMouseLockMode::DoNotLock
    pb.b[17] = uiOnly ? 1 : 0;               // UIOnly: bFlushInput=1 / GameAndUI: bHideCursor=0
    if (!peGuard(lib, fn, pb.b))
    {
        logf("WARN inputmode: SEH");
        return false;
    }
    logf("inputmode: %s (parms=%d)", uiOnly ? "UIOnly(패널 포커스)" : "GameAndUI(복구)", ps);
    return true;
}

// v0.40 5차: 게임 메뉴 내비게이션 링에 클론을 실제로 끼운다.
// UWidget::SetNavigationRuleExplicit(EUINavigation Dir, UWidget* To) 를 써서
//   설정.Down = 클론 / 클론.Up = 설정 · 클론.Down = 나가기 / 나가기.Up = 클론
// 으로 재배선한다. 그러면 게임의 패드 내비가 우리를 네이티브로 선택하고, 우리는
// '클론이 선택됨 + A' 만 잡으면 된다 (가상 포커스·커서 워프 폐기).
// ⚠ 게임이 UMG 내비가 아니라 자체 입력 핸들러를 쓰면 무동작(무해) -- 그 경우
//    클론이 선택되지 않아 로그로 드러난다. EUINavigation: Up=2 Down=3 (실측 enum).
static bool navRule(UObject* w, int dir, UObject* to)
{
    if (!w || !to) return false;
    UFunction* fn = fnOf(w, L"SetNavigationRuleExplicit", "navrule");
    if (!fn) return false;
    int ps = (int)fn->GetParmsSize();
    if (ps < 12 || ps > 24) { logf("WARN navrule: parmSize %d 예상 밖", ps); return false; }
    PB pb;
    memset(pb.b, 0, 64);
    pb.b[0] = (unsigned char)dir;     // EUINavigation (uint8)
    memcpy(pb.b + 8, &to, 8);         // UWidget* (8정렬)
    if (!peGuard(w, fn, pb.b)) { logf("WARN navrule: SEH"); return false; }
    return true;
}

static void wireNav(UObject* clone, UObject* settings, UObject* exitW, bool quiet = false)
{
    // v0.40 7차: 메뉴는 포커스 구동이다(실측 menufn: OnFocusReceived/OnKeyDown).
    // 이 빌드엔 SetIsFocusable UFunction 이 없다(6차 FAIL 스팸) -- bIsFocusable
    // 프로퍼티를 직접 1 로 쓴다. before 값이 0 이면 "클론만 포커스 불가"가 원인 확정.
    {
        int off = propOffset(clone, L"bIsFocusable", 1, "navwire");
        if (off >= 0)
        {
            unsigned char before = 255;
            readByteGuard(clone, off, &before);
            writeByteGuard(clone, off, 1);
            if (!quiet) logf("navwire: bIsFocusable off=0x%X %d -> 1", off, (int)before);
        }
        else if (!quiet) logf("navwire: bIsFocusable 프로퍼티 없음");
    }
    /* ★ v0.50 (2026-08-17): **게임 위젯의 방향 이동 규칙을 덮어쓰던 코드를 걷어냈다.**

       무엇을 했었나: `나가기.Down = 우리 클론`, `게임시작.Up = 우리 클론`(항목과
       내부 버튼 양쪽)을 `SetNavigationRuleExplicit` 로 게임 위젯에 박았다.

       왜 지운다:
       1. **이득이 0 이다.** 패드 진입은 전용 Y 버튼으로 확정됐고(v0.40 11차),
          순회 편입(adopt)은 봉인돼 있다(`g_adoptEnabled=false`). 이 배선이
          작동한다는 증거는 끝내 없었다.
       2. **배치와 어긋난다.** 이 규칙들은 v0.50b '클론 맨 아래' 기준인데, 지금
          클론은 설정↔나가기 사이다. 즉 게임 메뉴에 **틀린 이동 규칙**을 박고 있었다.
       3. **위험이 실재한다.** 클론은 재정렬이 끝날 때까지 Collapsed 이고(은신),
          재정렬이 실패/시간초과할 수도 있다. 접힌 위젯으로 가는 규칙은 **막다른 길**이
          된다. 게다가 게임시작 위젯을 못 찾으면(`play=0` 실측) `나가기.Down` 만
          걸려 **되돌아올 랩이 없다**.
       → "모드매니저를 깔면 컨트롤러로 메뉴 이동이 안 된다"는 제보를 만들 수 있는
          **유일한 경로**였다. 남의 위젯은 건드리지 않는다.

       남기는 것: 우리 클론(과 그 버튼)의 `bIsFocusable` 뿐 -- 우리 위젯에만 닿고
       게임 순회를 바꾸지 않는다. */
    g_navWired = false;
    if (UObject* cBtn = readObjProp(clone, L"TitleMenuBtn", "navbtn"))
    {
        int off = propOffset(cBtn, L"bIsFocusable", 1, "navbtn");
        if (off >= 0)
        {
            unsigned char before = 255;
            readByteGuard(cBtn, off, &before);
            writeByteGuard(cBtn, off, 1);
            if (!quiet) logf("navbtn: 클론버튼 bIsFocusable off=0x%X %d -> 1", off, (int)before);
        }
    }
    else if (!quiet) logf("navbtn: 클론 TitleMenuBtn 없음");
    (void)settings;
    (void)exitW;
    if (!quiet) logf("navwire: 게임 위젯 내비 규칙은 건드리지 않는다 (v0.50 제거)");
}

static void closePanel(const char* why)
{
    if (g_popupOpen || g_popup) closePopup("패널 닫힘 연쇄");
    if (g_comboOpen || g_comboHost) closeCombo("패널 닫힘 연쇄");
    if (g_colorOpen || g_colorHost) closeColor("패널 닫힘 연쇄");   // v0.28
    g_keyCapture.store(false, std::memory_order_relaxed);
    g_keyCapRow = g_keyCapOpt = -1;
    g_panelOpen = false;
    for (int i = 0; i < 3; ++i) g_hsX[i] = nullptr;
    for (int i = 0; i < 3; ++i) g_hsBtn[i] = nullptr;
    g_hsTab[0] = g_hsTab[1] = nullptr;  // v0.16 탭/순서 상태 무효화
    g_ordN = 0;
    g_dragIdx = -1;
    g_dragMoved = false;
    g_scrollBox = nullptr;  // v0.17
    g_scrimW = nullptr;  // v0.18
    g_sbShown[0] = false;
    g_sbLastOff[0] = -1.0f;
    clearArm();
    clearDragScroll();
    g_sldHs = nullptr;          // v0.29: 잡고 있던 슬라이더 놓기
    g_sldRow = g_sldOpt = -1;
    g_langHs = g_langTx = nullptr;   // v0.40: 언어 콤보
    g_navN = 0;                      // v0.40(pad): 내비 목록 무효화 (선택 인덱스는 유지)
    g_padOrdLift = -1;
    g_padEdges.store(0, std::memory_order_relaxed);
    // v0.40 8차: B/ESC 를 아직 쥔 채 입력을 돌려주면 게임이 그 눌림을 받아
    // 타이틀에서 게임을 꺼버린다(실측: B 로 패널 닫기 = 게임 종료). 놓을 때까지 지연.
    if (g_padBHeld.load(std::memory_order_relaxed) || (GetAsyncKeyState(VK_ESCAPE) & 0x8000))
        g_restoreInputPending = true;
    else
        panelInputMode(false);
    // ⚠ g_pcPanel 은 지연 복구(panelInputMode)가 쓴다 -- 여기서 지우면 복구가
    // PC 를 못 찾아 UIOnly 잠금이 풀리지 않는다. 재open/메뉴 vfocus 가 갱신한다.
    g_chipBox[0] = g_chipBox[1] = nullptr;
    g_padHintBox = nullptr;
    g_plgN = 0;  // 토글 행 포인터 일괄 무효화
    void* p = g_panel;
    g_panel = nullptr;
    if (p)
    {
        // 닫기 = 파괴 (Lua 규약: 숨기지 않는다). 죽은 패널이어도 조회/호출이
        // 전부 SEH 가드라 시도 자체는 안전하다.
        UObject* host = reinterpret_cast<UObject*>(p);
        UFunction* fn = fnOf(host, L"RemoveFromParent", "closePanel");
        if (fn && (int)fn->GetParmsSize() == 0)
        {
            PB pb;
            if (!peGuard(host, fn, pb.b)) logf("WARN closePanel: RemoveFromParent SEH");
        }
    }
    logf("panel: 닫힘 (%s)", why);
}

// 설정창 스타일 패널. Lua buildPanel(umg.lua:1982-2302) 체인의 C++ 전사.
// 평면 위젯 생성은 StaticConstructObject(금지 구조체) 대신
// GameplayStatics:SpawnObject(Class, Outer) ProcessEvent 호출 (parmSize 24 실측).
static bool openPanel(UObject* clone)
{
    logf("panel: open 시작");
    loadSessionMods();  // v0.7: 재시작 필요 판정용 세션 스냅샷
    // v0.50: 게임 실행 중에 넣은 모드(켜진 채 배포된 zip 포함)의 진입점을 만들어 둔다.
    // 이번 실행에는 못 싣지만, 이제 껐다 켤 필요 없이 **재시작만 하면** 로드된다.
    reconcileEntryPoints("패널 열기");

    UObject* pc = UOG::FindFirstOf(L"PlayerController");
    g_pcPanel = (void*)pc;               // v0.40: 입력모드 전환/복구용
    recheckLang(readGameLangText());     // v0.40: 설정에서 언어를 바꾼 직후 바로 들어와도 맞춘다
    UObject* lib = findObj(L"/Script/UMG.Default__WidgetBlueprintLibrary", "panel");
    UObject* gs = findObj(L"/Script/Engine.Default__GameplayStatics", "panel");
    UObject* hostCls = findObj(L"/Script/UMG.UserWidget", "panel");
    UObject* bCls = findObj(L"/Script/UMG.Border", "panel");
    UObject* tCls = findObj(L"/Script/UMG.TextBlock", "panel");
    UObject* vCls = findObj(L"/Script/UMG.VerticalBox", "panel");
    UObject* hCls = findObj(L"/Script/UMG.HorizontalBox", "panel");
    UObject* sCls = findObj(L"/Script/UMG.SizeBox", "panel");
    if (!lib || !gs || !hostCls || !bCls || !tCls || !vCls || !hCls || !sCls) return false;

    // 호스트 UserWidget = 엔진 /Script/UMG.UserWidget 을 Create (Lua 실증 경로;
    // StaticConstructObject 로 만들면 WidgetTree 가 null -- 금지)
    UFunction* fnCreate = fnOf(lib, L"Create", "panel.create");
    if (!fnCreate || !parmsExact(fnCreate, 32, "panel.Create", false)) return false;
    UObject* ctx = pc ? pc : clone;
    PB pbC;
    memcpy(pbC.b + 0, &ctx, 8);
    memcpy(pbC.b + 8, &hostCls, 8);
    memcpy(pbC.b + 16, &pc, 8);
    if (!peGuard(lib, fnCreate, pbC.b)) { logf("FAIL panel: Create SEH"); return false; }
    void* hostRaw = nullptr;
    memcpy(&hostRaw, pbC.b + (int)fnCreate->GetReturnValueOffset(), 8);
    UObject* host = reinterpret_cast<UObject*>(hostRaw);
    if (!host) { logf("FAIL panel: Create null"); return false; }

    UObject* tree = readObjProp(host, L"WidgetTree", "panel.tree");
    if (!tree) { logf("FAIL panel: WidgetTree null"); return false; }

    // 평면 위젯 공장
    UFunction* fnSpawn = fnOf(gs, L"SpawnObject", "panel.spawn");
    if (!fnSpawn || !parmsExact(fnSpawn, 24, "panel.SpawnObject", false)) return false;
    auto spawn = [&](UObject* cls, const char* tag) -> UObject* {
        PB pb;
        memcpy(pb.b + 0, &cls, 8);
        memcpy(pb.b + 8, &tree, 8);
        if (!peGuard(gs, fnSpawn, pb.b)) { logf("FAIL %s: SpawnObject SEH", tag); return nullptr; }
        void* w = nullptr;
        memcpy(&w, pb.b + (int)fnSpawn->GetReturnValueOffset(), 8);
        if (!w) logf("FAIL %s: SpawnObject null", tag);
        return reinterpret_cast<UObject*>(w);
    };

    // 스크림(루트 Border) -- ⚠ 루트는 반드시 Border (Overlay 교체/추가 = 실측 전멸)
    UObject* scrim = spawn(bCls, "panel.scrim");
    if (!scrim) return false;
    {
        int off = propOffset(tree, L"RootWidget", 8, "panel.root");
        if (off < 0) { logf("FAIL panel: RootWidget 해석 불가"); return false; }
        if (!writePtrGuard(tree, off, scrim)) { logf("FAIL panel: RootWidget 쓰기 AV"); return false; }
        logf("panel: RootWidget=scrim (off=0x%X)", off);
    }
    // 텍스처 임포트 공용 (그라데이션/토글) -- ChestFinder 실증 경로.
    // ImportFileAsTexture2D parms 32, Border 판 SetBrushFromTexture parms 8 (덤프 실측)
    UObject* krl = findObj(L"/Script/Engine.Default__KismetRenderingLibrary", "panel.krl");
    UFunction* fnImp = krl ? fnOf(krl, L"ImportFileAsTexture2D", "panel.import") : nullptr;
    if (fnImp && !parmsExact(fnImp, 32, "panel.Import", false)) fnImp = nullptr;
    auto importTex = [&](const wchar_t* file, const char* tag) -> UObject* {
        if (!fnImp) return nullptr;
        static wchar_t tp[MAX_PATH * 2];
        assetPath(tp, file);
        PB pb;
        UObject* c3 = pc ? pc : clone;
        memcpy(pb.b + 0, &c3, 8);
        FStringRaw fs{tp, (int)wcslen(tp) + 1, (int)wcslen(tp) + 1};
        memcpy(pb.b + 8, &fs, 16);
        if (!peGuard(krl, fnImp, pb.b))
        {
            logf("FAIL %s: import SEH", tag);
            return nullptr;
        }
        void* tex = nullptr;
        memcpy(&tex, pb.b + (int)fnImp->GetReturnValueOffset(), 8);
        if (!tex) logf("WARN %s: 텍스처 임포트 실패 (%s)", tag, u8(tp).c_str());
        return reinterpret_cast<UObject*>(tex);
    };

    // 그라데이션: 설정창 배경 실측 램프 텍스처. Lua 의 5회 실패는 밴드/Overlay 방식,
    // 텍스처 경로는 미시도였고 v0.3 에서 라이브 실증됨.
    bool gradientOn = false;
    if (UObject* tex = importTex(L"scrim_gradient.png", "panel.grad"))
    {
        if (callBytes(scrim, L"SetBrushFromTexture", &tex, 8, "panel.brushTex"))
        {
            setBrushColor(scrim, {1, 1, 1, 1}, "panel.tint");  // 색/알파는 텍스처가 전담
            gradientOn = true;
        }
    }
    if (!gradientOn)  // 폴백: 실측 램프의 중간값 플랫
        setBrushColor(scrim, {0.020f, 0.024f, 0.030f, 0.96f}, "panel.scrimFlat");
    {
        float pad[4] = {96, 46, 58, 60};
        callBytes(scrim, L"SetPadding", pad, 16, "panel.scrimPad");
    }
    setVisibility(scrim, 0, "panel.scrim");  // 0=Visible = 클릭 흡수 -- 모달의 핵심
    g_scrimW = scrim;  // v0.18: 전체화면 루트 = 뷰포트 사각형의 유일한 실측원

    // 내용 컬럼
    UObject* vb = spawn(vCls, "panel.vb");
    if (!vb) return false;
    setVisibility(vb, 4, "panel.vb");
    slotAlign(addChildTo(scrim, vb, "panel.vb"), 0, 0, "panel.vb");

    // 폰트: 클론 항목의 TitleText 에서 통째로 빌림 (Lua 규약 -- 크기 지정 없음)
    unsigned char font[96];
    bool haveFont = false;
    if (UObject* tt = readObjProp(clone, L"TitleText", "panel.font"))
    {
        int foff = propOffset(tt, L"Font", 88, "panel.font");
        if (foff >= 0 && readBytesGuard(tt, foff, font, 88)) haveFont = true;
    }
    float baseFontSize = 0.f;
    if (haveFont) memcpy(&baseFontSize, font + 0x48, 4);
    logf("panel: font %s (base Size=%.1f)", haveFont ? "빌림(88B)" : "실패 -- 엔진 기본", baseFontSize);
    // FSlateFontInfo.Size = float @0x48 (덤프 실측). 우리 복사본만 패치하므로 안전.
    // 설정창 실측 비율: 제목 1.15 · 탭/라벨 1.0 · 섹션 0.78 · 알약/버튼 0.9 · 힌트 0.8
    auto applyFontScaled = [&](UObject* tb, float scale) {
        if (!haveFont) return;
        unsigned char fb[88];
        memcpy(fb, font, 88);
        if (scale != 1.0f)
        {
            float s;
            memcpy(&s, fb + 0x48, 4);
            s *= scale;
            memcpy(fb + 0x48, &s, 4);
        }
        callBytes(tb, L"SetFont", fb, 88, "panel.font");
    };

    // 제목줄: [모드매니저C++ ....................... X]
    UObject* row = spawn(hCls, "panel.row");
    if (!row) return false;
    setVisibility(row, 4, "panel.row");
    UObject* rowSlot = addChildTo(vb, row, "panel.row");
    slotAlign(rowSlot, 0, -1, "panel.row");

    UObject* title = spawn(tCls, "panel.title");
    if (title)
    {
        setVisibility(title, 4, "panel.title");
        setTextOn(title, trLabel(), "panel.title");
        setTextColor(title, {0.97f, 0.97f, 0.98f, 1.0f}, "panel.title");
        applyFontScaled(title, 1.15f);
        UObject* s = addChildTo(row, title, "panel.title");
        slotAlign(s, -1, 2, "panel.title");
        slotFillWidth(s, "panel.title");
    }

    // X 버튼: 게임의 진짜 닫기 아이콘(DToolBar_C 의 CloseButton) 복제 -- Lua 라이브 실증 체인.
    // 실측: 설정창 X = 71x72px 중심(2476,71). 52px 박스 + RenderScale 1.86(=67/36) 이면
    // 시각 ~67px, 현 레이아웃의 X 중심 (2475,71) = 이미 일치라 배치는 그대로 둔다.
    UObject* xClose = nullptr;  // 최종 히트스팟 (복제 CloseButton 또는 폴백 Border)
    UObject* xbox = spawn(sCls, "panel.xbox");
    if (xbox)
    {
        float f52 = 52.0f;
        callBytes(xbox, L"SetWidthOverride", &f52, 4, "panel.xbox");
        callBytes(xbox, L"SetHeightOverride", &f52, 4, "panel.xbox");
        setVisibility(xbox, 4, "panel.xbox");

        // 1) 툴바 클래스: 기록된 에셋 경로(STATUS 실측) -> 라이브 인스턴스 폴백
        UObject* tbCls = findObj(L"/Game/UI/Layer/DToolBar.DToolBar_C", "panel.tbCls");
        if (!tbCls)
        {
            std::vector<UObject*> tbs;
            UOG::FindAllOf(L"DToolBar_C", tbs);
            for (UObject* t : tbs)
            {
                if (!t) continue;
                std::wstring full = t->GetFullName(nullptr);
                if (wcontains(full, L"/Game/") || wcontains(full, L"Default__")) continue;
                tbCls = reinterpret_cast<UObject*>(t->GetClassPrivate());
                break;
            }
        }
        if (tbCls)
        {
            PB pb;
            UObject* c2 = pc ? pc : clone;
            memcpy(pb.b + 0, &c2, 8);
            memcpy(pb.b + 8, &tbCls, 8);
            memcpy(pb.b + 16, &pc, 8);
            UObject* tb = nullptr;
            if (peGuard(lib, fnCreate, pb.b))
            {
                void* raw = nullptr;
                memcpy(&raw, pb.b + (int)fnCreate->GetReturnValueOffset(), 8);
                tb = reinterpret_cast<UObject*>(raw);
            }
            if (tb)
            {
                // CloseButton 은 네이티브 프로퍼티(UDToolbar @0x358, 덤프 실측) --
                // Lua 의 재귀 트리 탐색이 필요 없다.
                if (UObject* cb = readObjProp(tb, L"CloseButton", "panel.closeBtn"))
                {
                    // 접힌 툴바를 트리에 남겨 X 의 소유 트리 생존 보장 (Lua 실측 규약)
                    setVisibility(tb, 1, "panel.tbar");
                    addChildTo(vb, tb, "panel.tbar");
                    UFunction* rm = fnOf(cb, L"RemoveFromParent", "panel.xdetach");
                    if (rm && (int)rm->GetParmsSize() == 0)
                    {
                        PB p2;
                        peGuard(cb, rm, p2.b);
                    }
                    addChildTo(xbox, cb, "panel.xattach");
                    double sc[2] = {1.86, 1.86};  // FVector2D = double 2개 (UE5.3 실측 16B)
                    callBytes(cb, L"SetRenderScale", sc, 16, "panel.xscale");
                    setVisibility(cb, 0, "panel.x");
                    unsigned char on1 = 1;
                    callBytes(cb, L"SetIsEnabled", &on1, 1, "panel.xenable");
                    setRenderOpacity(cb, 1.0f, "panel.x");
                    xClose = cb;
                    logf("panel: 게임 CloseButton 복제 성공");
                }
            }
            if (!xClose) logf("WARN panel: 툴바 복제 실패 -- 글리프 X 폴백");
        }

        // 2) 폴백: 손그림 'X' 글리프 (v0.2 방식)
        if (!xClose)
        {
            UObject* xb = spawn(bCls, "panel.xborder");
            UObject* xt = xb ? spawn(tCls, "panel.xtext") : nullptr;
            if (xb && xt)
            {
                setBrushColor(xb, {1, 1, 1, 0.055f}, "panel.xborder");
                setVisibility(xb, 0, "panel.xborder");
                setTextOn(xt, L"X", "panel.xtext");
                setTextColor(xt, {0.97f, 0.97f, 0.98f, 1.0f}, "panel.xtext");
                applyFontScaled(xt, 1.0f);
                setVisibility(xt, 4, "panel.xtext");
                slotAlign(addChildTo(xbox, xb, "panel.xb"), 0, 0, "panel.xb");
                slotAlign(addChildTo(xb, xt, "panel.xt"), 2, 2, "panel.xt");
                xClose = xb;
            }
        }

        UObject* s = addChildTo(row, xbox, "panel.xslot");
        slotAlign(s, -1, 2, "panel.xslot");
        slotPad(s, 16, 0, 0, 0, "panel.xslot");
        g_hsX[0] = xbox;
        g_hsX[1] = xClose;
        g_hsX[2] = nullptr;
    }
    if (!xClose) logf("WARN panel: X 버튼 생성 실패 -- ESC 로만 닫기 가능");

    // ================= UI 공사 (v0.4) -- 전부 설정창 픽셀 실측 기반 =================
    // 탭 셀 591px · 골드 밑줄 6px sRGB{248,238,192} · 행 밴드 h105 흰α0.07 ·
    // 악센트 14px 흰α0.20 · 버전 알약 656x62 · 보기형 버튼 683x80 외곽선 {145,157,156}

    const LinColor GOLD = {0.93f, 0.85f, 0.53f, 1.0f};
    // v0.40: 선택 테두리 텍스처 -- 가운데가 투명이라 외곽선만 그려진다
    // (Border 색만 켜면 행 전체가 칠해진다 -- 실측 지적으로 교체)
    UObject* texSelFrame = importTex(L"select_frame.png", "panel.texSelFrame");

    // ---- 탭 줄: [모드][순서] (v0.16 -- 활성 = 골드 글자 + 6px 골드 밑줄,
    //      비활성 = 흐린 글자 + 2px 희미선. 밑줄들은 바닥 정렬로 이어진다) ----
    const wchar_t* const TAB_NAMES[2] = {TR(L"모드", L"Mods"), TR(L"순서", L"Order")};  // v0.40: static 금지(언어 전환)
    if (UObject* tabRow = spawn(hCls, "panel.tabRow"))
    {
        setVisibility(tabRow, 4, "panel.tabRow");
        // v0.40(pad): LB/RB 칩 -- 게임 설정창처럼 탭 양 옆. 아이콘 텍스처를 쓰고,
        // 패드를 쓸 때만 보인다 (키/마 입력이 오면 즉시 숨김 -- 설정창 실측).
        auto padChip = [&](const wchar_t* texName, int idx) {
            UObject* cbox = spawn(sCls, "panel.padChip");
            if (!cbox) return;
            float cw = 64.0f, ch2 = 44.0f;
            callBytes(cbox, L"SetWidthOverride", &cw, 4, "panel.padChip");
            callBytes(cbox, L"SetHeightOverride", &ch2, 4, "panel.padChip");
            // Hidden(2) = 자리 유지 -- Collapsed(1)로 접으면 탭 줄이 옆으로 민다 (실측 보고)
            setVisibility(cbox, g_inputMode.load(std::memory_order_relaxed) == 1 ? 4 : 2, "panel.padChip");
            UObject* ctex = importTex(texName, "panel.padChip");
            if (UObject* cb = spawn(bCls, "panel.padChipB"))
            {
                if (ctex) callBytes(cb, L"SetBrushFromTexture", &ctex, 8, "panel.padChipB");
                setBrushColor(cb, {1, 1, 1, 1}, "panel.padChipB");
                setVisibility(cb, 4, "panel.padChipB");
                slotAlign(addChildTo(cbox, cb, "panel.padChip"), 0, 0, "panel.padChip");
            }
            slotAlign(addChildTo(tabRow, cbox, "panel.padChip"), -1, 2, "panel.padChip");
            g_chipBox[idx] = cbox;
        };
        padChip(L"pad_lb.png", 0);
        for (int t = 0; t < 2; ++t)
        {
            UObject* tabBox = spawn(sCls, "panel.tab");
            if (!tabBox) continue;
            float fw = 591.0f;
            callBytes(tabBox, L"SetWidthOverride", &fw, 4, "panel.tab");
            setVisibility(tabBox, 4, "panel.tab");
            if (UObject* tabTxt = spawn(tCls, "panel.tabTxt"))
            {
                setVisibility(tabTxt, 4, "panel.tabTxt");
                setTextOn(tabTxt, TAB_NAMES[t], "panel.tabTxt");
                setTextColor(tabTxt, t == g_activeTab ? GOLD : LinColor{0.42f, 0.46f, 0.50f, 1.0f},
                             "panel.tabTxt");
                applyFontScaled(tabTxt, 1.0f);
                slotAlign(addChildTo(tabBox, tabTxt, "panel.tabTxt"), 2, 3, "panel.tabTxt");
            }
            slotAlign(addChildTo(tabRow, tabBox, "panel.tab"), -1, 3, "panel.tab");
            g_hsTab[t] = tabBox;
        }
        padChip(L"pad_rb.png", 1);
        UObject* s = addChildTo(vb, tabRow, "panel.tabRow");
        slotAlign(s, 1, -1, "panel.tabRow");
        slotPad(s, 0, 44, 0, 0, "panel.tabRow");
    }
    if (UObject* lineRow = spawn(hCls, "panel.tabLine"))
    {
        setVisibility(lineRow, 4, "panel.tabLine");
        for (int t = 0; t < 2; ++t)
        {
            if (UObject* segBox = spawn(sCls, "panel.tabSeg"))
            {
                float fw = 591.0f, fh = t == g_activeTab ? 6.0f : 2.0f;
                callBytes(segBox, L"SetWidthOverride", &fw, 4, "panel.tabSeg");
                callBytes(segBox, L"SetHeightOverride", &fh, 4, "panel.tabSeg");
                setVisibility(segBox, 4, "panel.tabSeg");
                if (UObject* seg = spawn(bCls, "panel.tabSegF"))
                {
                    setBrushColor(seg, t == g_activeTab ? GOLD : LinColor{1, 1, 1, 0.16f},
                                  "panel.tabSegF");
                    setVisibility(seg, 4, "panel.tabSegF");
                    slotAlign(addChildTo(segBox, seg, "panel.tabSeg"), 0, 0, "panel.tabSeg");
                }
                slotAlign(addChildTo(lineRow, segBox, "panel.tabSeg"), -1, 3, "panel.tabSeg");
            }
        }
        if (UObject* faintBox = spawn(sCls, "panel.faint"))
        {
            float fh = 2.0f;
            callBytes(faintBox, L"SetHeightOverride", &fh, 4, "panel.faint");
            setVisibility(faintBox, 4, "panel.faint");
            if (UObject* faint = spawn(bCls, "panel.faintLine"))
            {
                setBrushColor(faint, {1, 1, 1, 0.16f}, "panel.faintLine");
                setVisibility(faint, 4, "panel.faintLine");
                slotAlign(addChildTo(faintBox, faint, "panel.faint"), 0, 0, "panel.faint");
            }
            UObject* fs2 = addChildTo(lineRow, faintBox, "panel.faint");
            slotAlign(fs2, -1, 3, "panel.faint");
            slotFillWidth(fs2, "panel.faint");
        }
        UObject* s = addChildTo(vb, lineRow, "panel.tabLine");
        slotAlign(s, 0, -1, "panel.tabLine");
        slotPad(s, 0, 20, 0, 0, "panel.tabLine");
    }

    // ---- v0.12: 스크롤 영역 (설정창 그래픽 탭 방식) ----
    // 제목/탭/하단 힌트는 고정, 그 사이 콘텐츠만 ScrollBox 안에서 휠/스크롤바로
    // 이동한다. ScrollBox 는 휠 입력을 받아야 하므로 Visible(0) -- 빈 영역 클릭은
    // 어차피 스크림이 먹던 것이라 모달 동작 불변.
    UObject* vbC = vb;  // 콘텐츠 부착 대상 (스크롤 생성 실패 시 기존 vb 폴백)
    if (UObject* sbCls = findObj(L"/Script/UMG.ScrollBox", "panel.sb"))
    {
        UObject* scroll = spawn(sbCls, "panel.scroll");
        UObject* vbS = scroll ? spawn(vCls, "panel.vbS") : nullptr;
        if (scroll && vbS)
        {
            setVisibility(scroll, 0, "panel.scroll");
            setVisibility(vbS, 4, "panel.vbS");
            slotAlign(addChildTo(scroll, vbS, "panel.vbS"), 0, -1, "panel.vbS");
            UObject* s = addChildTo(vb, scroll, "panel.scroll");
            slotAlign(s, 0, -1, "panel.scroll");
            slotFillWidth(s, "panel.scroll");  // VerticalBoxSlot SetSize(Fill) = 남은 세로 전부
            vbC = vbS;
            g_scrollBox = scroll;  // v0.17: 재구축 시 위치 사수용
            setScrollbarShown(scroll, false);  // v0.18: 평상시 숨김 (스크롤할 때만 등장)
            g_sbShown[0] = false;
            g_sbLastOff[0] = -1.0f;
            logf("panel: ScrollBox 도입 (콘텐츠 스크롤 가능)");
        }
        else logf("WARN panel: ScrollBox 생성 실패 -- 스크롤 없이 표시");
    }

    // ---- 섹션 라벨 / 행 밴드 공용 람다 ----
    const LinColor C_SEC = {0.22f, 0.25f, 0.29f, 1.0f};
    auto addSection = [&](const wchar_t* txt, const char* tag) {
        if (UObject* t = spawn(tCls, tag))
        {
            setVisibility(t, 4, tag);
            setTextOn(t, txt, tag);
            setTextColor(t, C_SEC, tag);
            applyFontScaled(t, 0.78f);
            slotPad(addChildTo(vbC, t, tag), 0, 64, 0, 0, tag);
        }
    };
    // 행 밴드 생성 -> 오른쪽 컨트롤을 넣을 HBox 반환 (라벨까지 채워짐)
    auto addRow = [&](const wchar_t* label, const char* tag) -> UObject* {
        UObject* rowBox = spawn(sCls, tag);
        if (!rowBox) return nullptr;
        float fh = 105.0f;
        callBytes(rowBox, L"SetHeightOverride", &fh, 4, tag);
        setVisibility(rowBox, 4, tag);
        UObject* band = spawn(bCls, tag);
        if (!band) return nullptr;
        setBrushColor(band, {1, 1, 1, 0.07f}, tag);
        setVisibility(band, 4, tag);
        {
            float m[4] = {0, 0, 74, 0};  // 알약~밴드 우측단 간격 (실측)
            callBytes(band, L"SetPadding", m, 16, tag);
        }
        // v0.40(pad): 선택 테두리 -- 게임 설정창의 크림색 외곽선(실측 pad.png).
        // 평소엔 투명(알파 0)이고, 패드 선택이 오면 크림색이 켜진다. 두께 = 패딩 3.
        UObject* selOut = spawn(bCls, tag);
        if (selOut)
        {
            if (texSelFrame) callBytes(selOut, L"SetBrushFromTexture", &texSelFrame, 8, tag);
        setBrushColor(selOut, {0.95f, 0.92f, 0.80f, 0.0f}, tag);
            setVisibility(selOut, 4, tag);
            float sm[4] = {3, 3, 3, 3};
            callBytes(selOut, L"SetPadding", sm, 16, tag);
            slotAlign(addChildTo(rowBox, selOut, tag), 0, 0, tag);
            slotAlign(addChildTo(selOut, band, tag), 0, 0, tag);
        }
        else slotAlign(addChildTo(rowBox, band, tag), 0, 0, tag);
        g_lastRowOutline = selOut;
        g_lastRowBox = rowBox;
        UObject* hb = spawn(hCls, tag);
        if (!hb) return nullptr;
        setVisibility(hb, 4, tag);
        slotAlign(addChildTo(band, hb, tag), 0, 0, tag);
        if (UObject* accBox = spawn(sCls, tag))
        {
            float fw = 14.0f;  // 좌측 악센트 바 (실측 14px)
            callBytes(accBox, L"SetWidthOverride", &fw, 4, tag);
            setVisibility(accBox, 4, tag);
            if (UObject* acc = spawn(bCls, tag))
            {
                setBrushColor(acc, {1, 1, 1, 0.20f}, tag);
                setVisibility(acc, 4, tag);
                slotAlign(addChildTo(accBox, acc, tag), 0, 0, tag);
            }
            slotAlign(addChildTo(hb, accBox, tag), -1, 0, tag);
        }
        if (UObject* lbl = spawn(tCls, tag))
        {
            setVisibility(lbl, 4, tag);
            setTextOn(lbl, label, tag);
            setTextColor(lbl, {0.93f, 0.95f, 0.96f, 1.0f}, tag);
            applyFontScaled(lbl, 1.0f);
            UObject* ls = addChildTo(hb, lbl, tag);
            slotAlign(ls, -1, 2, tag);
            slotPad(ls, 30, 0, 0, 0, tag);
            slotFillWidth(ls, tag);
        }
        UObject* s = addChildTo(vbC, rowBox, tag);
        slotAlign(s, 0, -1, tag);
        slotPad(s, 0, 20, 38, 0, tag);  // 밴드 우측단을 설정창과 정렬 (실측 38)
        return hb;
    };

    // ==================== 탭별 콘텐츠 (v0.16) ====================
    g_navN = 0;   // v0.40(pad): 행을 다시 만들며 다시 등록한다
    if (g_activeTab == 0)
    {
    // ---- v0.15: 닫힘 콤보 알약 공용 빌더 ----
    // 설정창 언어 콤보 실측: 675x76, 어두운 채움 {41,50,55} + 희미 이중선(텍스처),
    // 값 텍스트 좌측 정렬 + 우측 ▼ 크림 {253,245,207}. 히트 대상 = 알약 Border.
    UObject* texCombo = importTex(L"combo_pill.png", "panel.texCombo");
    // v0.28: showArrow=false 면 ▼ 를 빼고(키/색상은 목록이 아니다),
    //        oSwatch 를 주면 왼쪽에 색 견본 사각형을 단다.
    auto makeCombo = [&](UObject* hb, const wchar_t* curLabel, void** oHs, void** oTx,
                         bool showArrow = true, void** oSwatch = nullptr) -> bool {
        if (!hb) return false;
        UObject* box = spawn(sCls, "panel.cbo");
        UObject* pill = box ? spawn(bCls, "panel.cboPill") : nullptr;
        UObject* hbx = pill ? spawn(hCls, "panel.cboRow") : nullptr;
        if (!box || !pill || !hbx) return false;
        float fw = 675.0f, fh = 76.0f;
        callBytes(box, L"SetWidthOverride", &fw, 4, "panel.cbo");
        callBytes(box, L"SetHeightOverride", &fh, 4, "panel.cbo");
        setVisibility(box, 4, "panel.cbo");
        if (texCombo)
        {
            callBytes(pill, L"SetBrushFromTexture", &texCombo, 8, "panel.cboPill");
            setBrushColor(pill, {1, 1, 1, 1}, "panel.cboPill");
        }
        else setBrushColor(pill, {0.026f, 0.033f, 0.038f, 0.95f}, "panel.cboPill");
        setVisibility(pill, 0, "panel.cboPill");  // 히트 대상
        setVisibility(hbx, 4, "panel.cboRow");
        slotAlign(addChildTo(box, pill, "panel.cbo"), 0, 0, "panel.cbo");
        slotAlign(addChildTo(pill, hbx, "panel.cbo"), 0, 0, "panel.cbo");
        if (oSwatch)   // v0.28: 색 견본 (알약 왼쪽)
        {
            *oSwatch = nullptr;
            if (UObject* swBox = spawn(sCls, "panel.cboSw"))
            {
                float sw = 48.0f, sh = 48.0f;
                callBytes(swBox, L"SetWidthOverride", &sw, 4, "panel.cboSw");
                callBytes(swBox, L"SetHeightOverride", &sh, 4, "panel.cboSw");
                setVisibility(swBox, 4, "panel.cboSw");
                if (UObject* swb = spawn(bCls, "panel.cboSwB"))
                {
                    setVisibility(swb, 4, "panel.cboSwB");
                    slotAlign(addChildTo(swBox, swb, "panel.cboSw"), 0, 0, "panel.cboSw");
                    *oSwatch = swb;
                }
                UObject* ss = addChildTo(hbx, swBox, "panel.cboSw");
                slotAlign(ss, -1, 2, "panel.cboSw");
                slotPad(ss, 24, 0, 0, 0, "panel.cboSw");
            }
        }
        if (UObject* vt = spawn(tCls, "panel.cboVal"))
        {
            setVisibility(vt, 4, "panel.cboVal");
            setTextOn(vt, curLabel, "panel.cboVal");
            setTextColor(vt, {0.97f, 0.97f, 0.98f, 1.0f}, "panel.cboVal");
            applyFontScaled(vt, 0.9f);
            UObject* vs = addChildTo(hbx, vt, "panel.cboVal");
            slotAlign(vs, -1, 2, "panel.cboVal");
            slotPad(vs, oSwatch ? 18.0f : 36.0f, 0, 0, 0, "panel.cboVal");
            slotFillWidth(vs, "panel.cboVal");
            *oTx = vt;
        }
        if (showArrow) if (UObject* ar = spawn(tCls, "panel.cboArw"))
        {
            setVisibility(ar, 4, "panel.cboArw");
            setTextOn(ar, L"▼", "panel.cboArw");
            setTextColor(ar, {0.984f, 0.921f, 0.63f, 1.0f}, "panel.cboArw");
            // 실측(2.png): ▼ 높이 = 알약의 15% (13/84), 종횡 ~1.7 (게임 삼각형이 넓적)
            applyFontScaled(ar, 0.42f);
            double sc[2] = {1.5, 1.0};
            callBytes(ar, L"SetRenderScale", sc, 16, "panel.cboArw");
            UObject* as = addChildTo(hbx, ar, "panel.cboArw");
            slotAlign(as, -1, 2, "panel.cboArw");
            slotPad(as, 0, 0, 30, 0, "panel.cboArw");
        }
        slotAlign(addChildTo(hb, box, "panel.cbo"), -1, 2, "panel.cbo");
        *oHs = pill;
        return true;
    };

    // v0.29: 체크박스 -- 설정창 알약과 같은 계열(어두운 채움+크림 테두리 / 카키 채움+체크)
    UObject* texCheckOff = importTex(L"check_off.png", "panel.texChkOff");
    UObject* texCheckOn = importTex(L"check_on.png", "panel.texChkOn");
    auto makeCheck = [&](UObject* hb, bool on, void** oHs) -> bool {
        if (!hb) return false;
        UObject* box = spawn(sCls, "panel.chk");
        UObject* b = box ? spawn(bCls, "panel.chkB") : nullptr;
        if (!box || !b) return false;
        float w = 44.0f;
        callBytes(box, L"SetWidthOverride", &w, 4, "panel.chk");
        callBytes(box, L"SetHeightOverride", &w, 4, "panel.chk");
        setVisibility(box, 4, "panel.chk");
        UObject* t = on ? texCheckOn : texCheckOff;
        if (t) callBytes(b, L"SetBrushFromTexture", &t, 8, "panel.chkB");
        setBrushColor(b, {1, 1, 1, 1}, "panel.chkB");
        setVisibility(b, 0, "panel.chkB");   // 히트 대상
        slotAlign(addChildTo(box, b, "panel.chk"), 0, 0, "panel.chk");
        UObject* s2 = addChildTo(hb, box, "panel.chk");
        slotAlign(s2, -1, 2, "panel.chk");
        slotPad(s2, 0, 0, 24, 0, "panel.chk");
        *oHs = b;
        return true;
    };

    // v0.29: 실행 버튼 -- '폴더 바로가기' 와 같은 생김새(외곽선+채움+가운데 글자)
    auto makeButton = [&](UObject* hb, const wchar_t* cap, void** oHs) -> bool {
        if (!hb) return false;
        UObject* box = spawn(sCls, "panel.btn2");
        if (!box) return false;
        float fw = 340.0f, fh = 72.0f;
        callBytes(box, L"SetWidthOverride", &fw, 4, "panel.btn2");
        callBytes(box, L"SetHeightOverride", &fh, 4, "panel.btn2");
        setVisibility(box, 4, "panel.btn2");
        UObject* outline = spawn(bCls, "panel.btn2o");
        UObject* fill2 = outline ? spawn(bCls, "panel.btn2f") : nullptr;
        UObject* bt = fill2 ? spawn(tCls, "panel.btn2t") : nullptr;
        if (!outline || !fill2 || !bt) return false;
        setBrushColor(outline, {0.285f, 0.336f, 0.333f, 0.95f}, "panel.btn2o");
        setVisibility(outline, 0, "panel.btn2o");   // 히트 대상
        { float m[4] = {3, 3, 3, 3}; callBytes(outline, L"SetPadding", m, 16, "panel.btn2o"); }
        setBrushColor(fill2, {0.010f, 0.014f, 0.016f, 0.55f}, "panel.btn2f");
        setVisibility(fill2, 4, "panel.btn2f");
        setTextOn(bt, cap, "panel.btn2t");
        setTextColor(bt, {0.97f, 0.97f, 0.98f, 1.0f}, "panel.btn2t");
        applyFontScaled(bt, 0.9f);
        setVisibility(bt, 4, "panel.btn2t");
        slotAlign(addChildTo(box, outline, "panel.btn2"), 0, 0, "panel.btn2");
        slotAlign(addChildTo(outline, fill2, "panel.btn2"), 0, 0, "panel.btn2");
        slotAlign(addChildTo(fill2, bt, "panel.btn2"), 2, 2, "panel.btn2");
        slotAlign(addChildTo(hb, box, "panel.btn2"), -1, 2, "panel.btn2");
        *oHs = outline;
        return true;
    };

    // v0.29: 슬라이더 -- 게임 설정창 실측(채움 #FFF7D1 / 트랙 {47,53,58} /
    //        막대 8px / 손잡이 28px 원). [채움][손잡이][남은] 3칸 HBox 로 만들고
    //        양끝 칸의 폭만 바꿔 손잡이를 움직인다(오버레이 없이).
    UObject* texHandle = importTex(L"slider_handle.png", "panel.texHandle");
    auto makeSlider = [&](UObject* hb, int val, int mn, int mx,
                          void** oHs, void** oFill, void** oRest, void** oTx) -> bool {
        if (!hb) return false;
        float usable = SLD_TRACK - SLD_KNOB;
        float t = (mx > mn) ? (float)(val - mn) / (float)(mx - mn) : 0.0f;
        if (t < 0) t = 0;
        if (t > 1) t = 1;
        float fw = usable * t, rw = usable - fw;
        UObject* box = spawn(sCls, "panel.sld");
        UObject* hit = box ? spawn(bCls, "panel.sldHit") : nullptr;
        UObject* row = hit ? spawn(hCls, "panel.sldRow") : nullptr;
        if (!box || !hit || !row) return false;
        float bw = SLD_TRACK, bh = 40.0f;
        callBytes(box, L"SetWidthOverride", &bw, 4, "panel.sld");
        callBytes(box, L"SetHeightOverride", &bh, 4, "panel.sld");
        setVisibility(box, 4, "panel.sld");
        setBrushColor(hit, {1, 1, 1, 0}, "panel.sldHit");   // 투명하지만 히트 대상
        setVisibility(hit, 0, "panel.sldHit");
        slotAlign(addChildTo(box, hit, "panel.sld"), 0, 0, "panel.sld");
        setVisibility(row, 4, "panel.sldRow");
        slotAlign(addChildTo(hit, row, "panel.sld"), 0, 0, "panel.sld");
        auto seg = [&](float w, LinColor c) -> UObject* {
            UObject* sb = spawn(sCls, "panel.sldSeg");
            UObject* b = sb ? spawn(bCls, "panel.sldSegB") : nullptr;
            if (!sb || !b) return nullptr;
            float h = SLD_BAR;
            callBytes(sb, L"SetWidthOverride", &w, 4, "panel.sldSeg");
            callBytes(sb, L"SetHeightOverride", &h, 4, "panel.sldSeg");
            setVisibility(sb, 4, "panel.sldSeg");
            setBrushColor(b, c, "panel.sldSegB");
            setVisibility(b, 4, "panel.sldSegB");
            slotAlign(addChildTo(sb, b, "panel.sldSeg"), 0, 0, "panel.sldSeg");
            slotAlign(addChildTo(row, sb, "panel.sldRow"), -1, 2, "panel.sldRow");
            return sb;
        };
        UObject* fillSeg = seg(fw, {1.0f, 0.969f, 0.82f, 1.0f});     // 실측 #FFF7D1
        UObject* knobBox = spawn(sCls, "panel.sldKnob");
        if (knobBox)
        {
            float k = SLD_KNOB;
            callBytes(knobBox, L"SetWidthOverride", &k, 4, "panel.sldKnob");
            callBytes(knobBox, L"SetHeightOverride", &k, 4, "panel.sldKnob");
            setVisibility(knobBox, 4, "panel.sldKnob");
            if (UObject* kb = spawn(bCls, "panel.sldKnobB"))
            {
                if (texHandle) callBytes(kb, L"SetBrushFromTexture", &texHandle, 8, "panel.sldKnobB");
                setBrushColor(kb, {1, 1, 1, 1}, "panel.sldKnobB");
                setVisibility(kb, 4, "panel.sldKnobB");
                slotAlign(addChildTo(knobBox, kb, "panel.sldKnob"), 0, 0, "panel.sldKnob");
            }
            slotAlign(addChildTo(row, knobBox, "panel.sldRow"), -1, 2, "panel.sldRow");
        }
        UObject* restSeg = seg(rw, {0.184f, 0.208f, 0.227f, 1.0f});  // 실측 {47,53,58}
        slotAlign(addChildTo(hb, box, "panel.sld"), -1, 2, "panel.sld");
        if (UObject* vbox = spawn(sCls, "panel.sldVal"))
        {
            float w2 = 120.0f, h2 = 40.0f;
            callBytes(vbox, L"SetWidthOverride", &w2, 4, "panel.sldVal");
            callBytes(vbox, L"SetHeightOverride", &h2, 4, "panel.sldVal");
            setVisibility(vbox, 4, "panel.sldVal");
            if (UObject* vt = spawn(tCls, "panel.sldValT"))
            {
                wchar_t buf[16];
                swprintf(buf, 16, L"%d", val);
                setVisibility(vt, 4, "panel.sldValT");
                setTextOn(vt, buf, "panel.sldValT");
                setTextColor(vt, {0.97f, 0.97f, 0.98f, 1.0f}, "panel.sldValT");
                applyFontScaled(vt, 1.0f);
                slotAlign(addChildTo(vbox, vt, "panel.sldVal"), 2, 2, "panel.sldVal");
                *oTx = vt;
            }
            slotAlign(addChildTo(hb, vbox, "panel.sldVal"), -1, 2, "panel.sldVal");
        }
        *oHs = hit;
        *oFill = fillSeg;
        *oRest = restSeg;
        return fillSeg && restSeg;
    };

    // ---- 기본: 모드 버전 [vX.Y 알약] ----
    addSection(TR(L"기본", L"General"), "panel.sec1");
    if (UObject* hb = addRow(TR(L"모드 버전", L"Version"), "panel.rowVer"))
    {
        if (UObject* pill = spawn(sCls, "panel.verPill"))
        {
            float fw = 656.0f, fh = 62.0f;
            callBytes(pill, L"SetWidthOverride", &fw, 4, "panel.verPill");
            callBytes(pill, L"SetHeightOverride", &fh, 4, "panel.verPill");
            setVisibility(pill, 4, "panel.verPill");
            if (UObject* fill = spawn(bCls, "panel.verFill"))
            {
                setBrushColor(fill, {0.008f, 0.010f, 0.012f, 0.78f}, "panel.verFill");
                setVisibility(fill, 4, "panel.verFill");
                slotAlign(addChildTo(pill, fill, "panel.verFill"), 0, 0, "panel.verFill");
                if (UObject* t = spawn(tCls, "panel.verTxt"))
                {
                    setVisibility(t, 4, "panel.verTxt");
                    setTextOn(t, MOD_VER_W, "panel.verTxt");
                    setTextColor(t, {0.97f, 0.97f, 0.98f, 1.0f}, "panel.verTxt");
                    applyFontScaled(t, 0.9f);
                    slotAlign(addChildTo(fill, t, "panel.verTxt"), 2, 2, "panel.verTxt");
                }
            }
            slotAlign(addChildTo(hb, pill, "panel.verPill"), -1, 2, "panel.verPill");
        }
    }

    // ---- 기본 (계속): 플러그인 [폴더 바로가기] (크레딧|보기 스타일, 클릭=폴더 열기) ----
    if (UObject* hb = addRow(TR(L"플러그인", L"Plugins"), "panel.rowPlg"))
    {
        if (UObject* obox = spawn(sCls, "panel.btnBox"))
        {
            float fw = 683.0f, fh = 80.0f;
            callBytes(obox, L"SetWidthOverride", &fw, 4, "panel.btnBox");
            callBytes(obox, L"SetHeightOverride", &fh, 4, "panel.btnBox");
            setVisibility(obox, 4, "panel.btnBox");
            UObject* outline = spawn(bCls, "panel.btnOutline");
            UObject* fill2 = outline ? spawn(bCls, "panel.btnFill") : nullptr;
            UObject* bt = fill2 ? spawn(tCls, "panel.btnTxt") : nullptr;
            if (outline && fill2 && bt)
            {
                setBrushColor(outline, {0.285f, 0.336f, 0.333f, 0.95f}, "panel.btnOutline");
                setVisibility(outline, 0, "panel.btnOutline");  // 히트테스트 대상
                float m[4] = {3, 3, 3, 3};                      // 외곽선 두께
                callBytes(outline, L"SetPadding", m, 16, "panel.btnOutline");
                setBrushColor(fill2, {0.010f, 0.014f, 0.016f, 0.55f}, "panel.btnFill");
                setVisibility(fill2, 4, "panel.btnFill");
                setTextOn(bt, TR(L"폴더 바로가기", L"Open folder"), "panel.btnTxt");
                setTextColor(bt, {0.97f, 0.97f, 0.98f, 1.0f}, "panel.btnTxt");
                applyFontScaled(bt, 0.9f);
                setVisibility(bt, 4, "panel.btnTxt");
                slotAlign(addChildTo(obox, outline, "panel.btn"), 0, 0, "panel.btn");
                slotAlign(addChildTo(outline, fill2, "panel.btn"), 0, 0, "panel.btn");
                slotAlign(addChildTo(fill2, bt, "panel.btn"), 2, 2, "panel.btn");
                g_hsBtn[0] = obox;
                g_hsBtn[1] = outline;
                g_hsBtn[2] = bt;
            }
            slotAlign(addChildTo(hb, obox, "panel.btnBox"), -1, 2, "panel.btnBox");
        }
        navAdd(NAVK_FOLDER, -1, -1);   // v0.40(pad)
    }

    // ---- 기본 (계속): 언어(Language) -- 매니저 자체 UI 언어 [v0.40] ----
    g_langHs = g_langTx = nullptr;
    if (UObject* hb = addRow(TR(L"언어(Language)", L"Language"), "panel.rowLang"))
    {
        makeCombo(hb, g_langChoices[g_lang == 1 ? 1 : 0], &g_langHs, &g_langTx);
        navAdd(NAVK_LANG, -1, -1);   // v0.40(pad)
    }

    // ---- 모드선택: plugins 폴더 스캔 -> 인식된 모드 행 (v0.5: 표시만, 토글은 다음 단계) ----
    // 인식 규칙 = UE4SS 표준 모드 폴더 구조: <이름>\Scripts\main.lua (Lua) 또는
    // <이름>\dlls\main.dll (C++). 등록 함수 호출 같은 능동 절차는 없다 -- 존재만으로 발견.
    addSection(TR(L"모드선택", L"Mods"), "panel.sec2");
    {
        // 토글 텍스처 (설정창 끄기/켜기 픽셀 실측: 컨테이너 678x76 r20, 알약 332x66 r26)
        UObject* texBg = importTex(L"toggle_bg.png", "panel.texBg");
        UObject* texSel = importTex(L"toggle_sel.png", "panel.texSel");

        // 켜기/끄기 토글 컨트롤 공용 빌더 (모드 행 + bool 옵션 행에서 재사용)
        auto makeToggle = [&](UObject* hb, void** oOffPill, void** oOnPill,
                              void** oOffText, void** oOnText) -> bool {
            if (!texBg || !texSel || !hb) return false;
            UObject* ctl = spawn(sCls, "panel.tglBox");
            UObject* bgB = ctl ? spawn(bCls, "panel.tglBg") : nullptr;
            UObject* hbx = bgB ? spawn(hCls, "panel.tglRow") : nullptr;
            if (!ctl || !bgB || !hbx) return false;
            float fw = 678.0f, fh = 76.0f;
            callBytes(ctl, L"SetWidthOverride", &fw, 4, "panel.tgl");
            callBytes(ctl, L"SetHeightOverride", &fh, 4, "panel.tgl");
            setVisibility(ctl, 4, "panel.tgl");
            callBytes(bgB, L"SetBrushFromTexture", &texBg, 8, "panel.tglBg");
            setBrushColor(bgB, {1, 1, 1, 1}, "panel.tglBg");
            setVisibility(bgB, 4, "panel.tglBg");
            setVisibility(hbx, 4, "panel.tglRow");
            slotAlign(addChildTo(ctl, bgB, "panel.tgl"), 0, 0, "panel.tgl");
            slotAlign(addChildTo(bgB, hbx, "panel.tgl"), 0, 0, "panel.tgl");
            const wchar_t* caps[2] = {TR(L"끄기", L"Off"), TR(L"켜기", L"On")};
            void* pills[2] = {nullptr, nullptr};
            void* texts[2] = {nullptr, nullptr};
            for (int k = 0; k < 2; ++k)
            {
                UObject* cell = spawn(sCls, "panel.tglCell");
                UObject* pill = cell ? spawn(bCls, "panel.tglPill") : nullptr;
                UObject* txt = pill ? spawn(tCls, "panel.tglTxt") : nullptr;
                if (!cell || !pill || !txt) break;
                float cw = 332.0f, chh = 66.0f;
                callBytes(cell, L"SetWidthOverride", &cw, 4, "panel.tglCell");
                callBytes(cell, L"SetHeightOverride", &chh, 4, "panel.tglCell");
                setVisibility(cell, 4, "panel.tglCell");
                callBytes(pill, L"SetBrushFromTexture", &texSel, 8, "panel.tglPill");
                setVisibility(pill, 0, "panel.tglPill");
                setTextOn(txt, caps[k], "panel.tglTxt");
                applyFontScaled(txt, 0.9f);
                setVisibility(txt, 4, "panel.tglTxt");
                slotAlign(addChildTo(cell, pill, "panel.tglCell"), 0, 0, "panel.tglCell");
                slotAlign(addChildTo(pill, txt, "panel.tglPill"), 2, 2, "panel.tglPill");
                UObject* cs = addChildTo(hbx, cell, "panel.tglRow");
                slotAlign(cs, -1, 2, "panel.tglRow");
                slotPad(cs, k == 0 ? 4.0f : 6.0f, 0, 0, 0, "panel.tglRow");
                pills[k] = pill;
                texts[k] = txt;
            }
            if (!pills[0] || !pills[1]) return false;
            slotAlign(addChildTo(hb, ctl, "panel.tglBox"), -1, 2, "panel.tglBox");
            *oOffPill = pills[0];
            *oOnPill = pills[1];
            *oOffText = texts[0];
            *oOnText = texts[1];
            return true;
        };

        // v0.16: 발견 -> dsorder.txt 저장 순서 적용 -> 행 구축 (수집기는 순서 탭과 공유)
        static PlgEnt ents[MM_MAX_PLUGINS];  // 게임 스레드 전용 -- 스택 절약
        int entN = collectPlugins(ents, MM_MAX_PLUGINS);
        int shown = 0;
        g_plgN = 0;
        {
            for (int k = 0; k < entN; ++k)
            {
                bool luaMod = ents[k].lua;
                bool cppMod = ents[k].cpp;
                ++shown;
                // ⚠ v0.11: 매니페스트를 **행을 만들기 전에** 읽는다. 표시 이름
                // (label)이 addRow 의 인자라, 예전처럼 나중에 읽으면 이미 폴더
                // 이름으로 그려진 뒤여서 반영할 방법이 없다.
                PlgRow& r = g_plg[g_plgN];
                memset(&r, 0, sizeof(r));
                lstrcpynW(r.name, ents[k].name, 64);
                lstrcpynW(r.rel, ents[k].rel, 192);  // v0.20: 중첩 배포판 대응
                r.pak = ents[k].pak;                 // v0.27
                loadManifest(r);       // v0.10: dsplugin.ini (옵션 선언 + runtime_key + label)
                loadOptionValues(r);   // dsoptions.txt 저장값 반영
                {   // 초기 상태: enabled.txt (plugins 쪽이 정본 -- 정션이라 같은 파일이고,
                    // pak 모드는 Mods 진입점이 아예 없다) 또는 mods.txt "<이름> : 1"
                    wchar_t pen[MAX_PATH * 2];
                    pluginSrcPath(pen, r.rel[0] ? r.rel : r.name);
                    lstrcatW(pen, L"\\enabled.txt");
                    wchar_t en[MAX_PATH * 2];
                    gameModsRoot(en);
                    lstrcatW(en, r.name);
                    lstrcatW(en, L"\\enabled.txt");
                    r.on = pathExistsW(pen) || pathExistsW(en) || modsTxtEnabled(r.name);
                }
                if (UObject* hb2 = addRow(r.label[0] ? r.label : r.name, "panel.rowMod"))
                {
                    // v0.50(TODO 11): "켜짐인데 이번 세션 미로드" 배지 -- 서드파티 zip 이
                    // enabled.txt 를 담은 채 오면 켜짐으로 보여도 실제로는 안 돌고 있다
                    // (UE4SS 는 부팅 때만 시작). 넥서스 리포트의 "켜짐인데 적용 안 됨" 해소.
                    // pak 은 세션 로드 개념이 없어 제외. 배치는 실측 검증된 modKind 와 동일.
                    if ((luaMod || cppMod) && r.on && !sessionLoaded(r.name))
                    {
                        if (UObject* nb = spawn(tCls, "panel.modKind"))
                        {
                            setVisibility(nb, 4, "panel.modKind");
                            setTextOn(nb, TR(L"재시작 필요", L"needs restart"), "panel.modKind");
                            setTextColor(nb, {0.95f, 0.65f, 0.25f, 1.0f}, "panel.modKind");
                            applyFontScaled(nb, 0.8f);
                            UObject* ns = addChildTo(hb2, nb, "panel.modKind");
                            slotAlign(ns, -1, 2, "panel.modKind");
                            slotPad(ns, 0, 0, 20, 0, "panel.modKind");
                        }
                    }
                    {   // v0.40: 모드 종류 (Lua / C++ / pak) -- 라벨과 토글 사이 (실측 mod1.png)
                        const wchar_t* kindTxt = ents[k].pak ? L"pak" : (cppMod ? L"C++" : L"Lua");
                        if (UObject* kt = spawn(tCls, "panel.modKind"))
                        {
                            setVisibility(kt, 4, "panel.modKind");
                            setTextOn(kt, kindTxt, "panel.modKind");
                            setTextColor(kt, {0.52f, 0.57f, 0.62f, 1.0f}, "panel.modKind");
                            applyFontScaled(kt, 0.8f);
                            UObject* ks = addChildTo(hb2, kt, "panel.modKind");
                            slotAlign(ks, -1, 2, "panel.modKind");
                            slotPad(ks, 0, 0, 28, 0, "panel.modKind");
                        }
                    }
                    bool built = makeToggle(hb2, &r.offPill, &r.onPill, &r.offText, &r.onText);
                    if (built)
                    {
                        navAdd(NAVK_MOD, g_plgN, -1);   // v0.40(pad): built 확정 뒤 등록 -- 실패 행이
                                                        // 다음 모드의 슬롯을 가리키는 별칭 사고 방지
                        paintToggle(r);
                        // ---- 켜진 모드의 옵션 서브행 (들여쓴 라벨 + 토글/스테퍼) ----
                        if (r.on)
                        {
                            // v0.16: 보이는 옵션이 FOLD_OVER 초과면 앞 FOLD_SHOW 개만
                            // 그리고 '펼치기' 행을 붙인다 (펼침 상태는 세션 유지).
                            // v0.17: 요청대로 8개째부터 접힘 (7개까지는 전부 표시)
                            const int FOLD_OVER = 7, FOLD_SHOW = 7;
                            int visTotal = 0;
                            for (int oi = 0; oi < r.optN; ++oi)
                                if (optVisible(r, oi)) ++visTotal;
                            bool folded = visTotal > FOLD_OVER && !isExpanded(r.name);
                            int builtOpt = 0;
                            for (int oi = 0; oi < r.optN; ++oi)
                            {
                                PlgOpt& o = r.opt[oi];
                                if (!optVisible(r, oi)) continue;  // v0.13: 부모 조건 미충족 자식은 숨김
                                if (folded && builtOpt >= FOLD_SHOW) break;  // 접힌 나머지는 UI 없음(널 포인터)
                                ++builtOpt;
                                UObject* rowBox = spawn(sCls, "panel.optRow");
                                if (!rowBox) break;
                                float fh = 88.0f;
                                callBytes(rowBox, L"SetHeightOverride", &fh, 4, "panel.optRow");
                                setVisibility(rowBox, 4, "panel.optRow");
                                UObject* band = spawn(bCls, "panel.optBand");
                                if (!band) break;
                                setBrushColor(band, {1, 1, 1, 0.035f}, "panel.optBand");
                                setVisibility(band, 4, "panel.optBand");
                                {
                                    float m[4] = {0, 0, 74, 0};
                                    callBytes(band, L"SetPadding", m, 16, "panel.optBand");
                                }
                                {   // v0.40(pad): 선택 테두리
                                    UObject* selOut = spawn(bCls, "panel.optBand");
                                    if (selOut)
                                    {
                                        if (texSelFrame) callBytes(selOut, L"SetBrushFromTexture", &texSelFrame, 8, "panel.optBand");
            setBrushColor(selOut, {0.95f, 0.92f, 0.80f, 0.0f}, "panel.optBand");
                                        setVisibility(selOut, 4, "panel.optBand");
                                        float sm[4] = {3, 3, 3, 3};
                                        callBytes(selOut, L"SetPadding", sm, 16, "panel.optBand");
                                        slotAlign(addChildTo(rowBox, selOut, "panel.optBand"), 0, 0, "panel.optBand");
                                        slotAlign(addChildTo(selOut, band, "panel.optBand"), 0, 0, "panel.optBand");
                                    }
                                    else slotAlign(addChildTo(rowBox, band, "panel.optBand"), 0, 0, "panel.optBand");
                                    g_lastRowOutline = selOut;
                                    g_lastRowBox = rowBox;
                                    if (o.type != 7) navAdd(NAVK_OPT, g_plgN, oi);  // v0.50: ini 표시행 제외
                                }
                                UObject* hbO = spawn(hCls, "panel.optHb");
                                if (!hbO) break;
                                setVisibility(hbO, 4, "panel.optHb");
                                slotAlign(addChildTo(band, hbO, "panel.optHb"), 0, 0, "panel.optHb");
                                if (UObject* lbl = spawn(tCls, "panel.optLbl"))
                                {
                                    setVisibility(lbl, 4, "panel.optLbl");
                                    setTextOn(lbl, o.label, "panel.optLbl");
                                    setTextColor(lbl, {0.62f, 0.66f, 0.70f, 1.0f}, "panel.optLbl");
                                    if (o.type == 7 && o.iniHeader)  // v0.50: 섹션/안내 행은 밝게
                                        setTextColor(lbl, {0.82f, 0.86f, 0.90f, 1.0f}, "panel.optLbl");
                                    applyFontScaled(lbl, 0.85f);
                                    UObject* ls = addChildTo(hbO, lbl, "panel.optLbl");
                                    slotAlign(ls, -1, 2, "panel.optLbl");
                                    // 들여쓰기: 기본 84 + 자식 단계당 40 (계층 시각화)
                                    slotPad(ls, 84.0f + 40.0f * optDepth(r, oi), 0, 0, 0, "panel.optLbl");
                                    slotFillWidth(ls, "panel.optLbl");
                                }
                                if (o.type == 4)
                                {
                                    // v0.29: 체크박스 (값 0/1)
                                    makeCheck(hbO, o.val != 0, &o.comboHs);
                                }
                                else if (o.type == 5)
                                {
                                    // v0.29: 실행 버튼 -- 값은 '누른 횟수'. 모드가
                                    // 값 증가를 보고 한 번 일하고 끝낸다.
                                    makeButton(hbO, o.btnCap[0] ? o.btnCap : TR(L"실행", L"Run"), &o.comboHs);
                                }
                                else if (o.type == 6)
                                {
                                    // v0.29: 슬라이더 (0~1000). 누른 순간부터 값이 따라온다.
                                    makeSlider(hbO, o.val, o.minV, o.maxV, &o.comboHs,
                                               &o.sliderFill, &o.sliderRest, &o.valText);
                                }
                                else if (o.type == 2)
                                {
                                    // v0.28: 키 바인딩 -- 알약에 키 이름, 클릭하면 캡처
                                    wchar_t kn[64];
                                    keyName(o.val, kn, 64);
                                    makeCombo(hbO, kn, &o.comboHs, &o.comboTx, false);
                                }
                                else if (o.type == 3)
                                {
                                    // v0.28: 색상 -- 알약에 #RRGGBB + 색 견본
                                    wchar_t hx[16];
                                    swprintf(hx, 16, L"#%06X", o.val & 0xFFFFFF);
                                    makeCombo(hbO, hx, &o.comboHs, &o.comboTx, false, &o.swatch);
                                    if (o.swatch)
                                        setBrushColor(reinterpret_cast<UObject*>(o.swatch),
                                                      rgbToLin(o.val), "opt.swatch");
                                }
                                else if (o.choiceN >= 2)
                                {
                                    // v0.15: choices= 선언 옵션 = 콤보박스 (값 = 인덱스)
                                    int ci = (o.val >= 0 && o.val < o.choiceN) ? o.val : 0;
                                    makeCombo(hbO, o.choices[ci], &o.comboHs, &o.comboTx);
                                }
                                else if (o.type == 0)
                                {
                                    makeToggle(hbO, &o.boolOff, &o.boolOn, &o.boolOffText, &o.boolOnText);
                                }
                                else if (o.type == 7)
                                {
                                    // v0.50: ini 표시 전용(헤더/구분/읽기전용) -- 컨트롤 없음
                                }
                                else
                                {
                                    // 스테퍼: [◀][ 값 ][▶]
                                    auto stepBtn = [&](const wchar_t* glyph) -> void* {
                                        UObject* box = spawn(sCls, "panel.step");
                                        UObject* bd = box ? spawn(bCls, "panel.stepB") : nullptr;
                                        UObject* tx = bd ? spawn(tCls, "panel.stepT") : nullptr;
                                        if (!box || !bd || !tx) return nullptr;
                                        float w2 = 84.0f, h2 = 64.0f;
                                        callBytes(box, L"SetWidthOverride", &w2, 4, "panel.step");
                                        callBytes(box, L"SetHeightOverride", &h2, 4, "panel.step");
                                        setVisibility(box, 4, "panel.step");
                                        setBrushColor(bd, {1, 1, 1, 0.10f}, "panel.stepB");
                                        setVisibility(bd, 0, "panel.stepB");  // 히트 대상
                                        setTextOn(tx, glyph, "panel.stepT");
                                        setTextColor(tx, {0.93f, 0.95f, 0.96f, 1.0f}, "panel.stepT");
                                        applyFontScaled(tx, 0.85f);
                                        setVisibility(tx, 4, "panel.stepT");
                                        slotAlign(addChildTo(box, bd, "panel.step"), 0, 0, "panel.step");
                                        slotAlign(addChildTo(bd, tx, "panel.step"), 2, 2, "panel.step");
                                        UObject* s2 = addChildTo(hbO, box, "panel.step");
                                        slotAlign(s2, -1, 2, "panel.step");
                                        slotPad(s2, 8, 0, 0, 0, "panel.step");
                                        return bd;
                                    };
                                    o.hsDec = stepBtn(L"◀");
                                    if (UObject* vbox = spawn(sCls, "panel.stepVal"))
                                    {
                                        float w2 = 200.0f, h2 = 64.0f;
                                        callBytes(vbox, L"SetWidthOverride", &w2, 4, "panel.stepVal");
                                        callBytes(vbox, L"SetHeightOverride", &h2, 4, "panel.stepVal");
                                        setVisibility(vbox, 4, "panel.stepVal");
                                        if (UObject* vt = spawn(tCls, "panel.stepVT"))
                                        {
                                            wchar_t buf[16];
                                            swprintf(buf, 16, L"%d", o.val);
                                            setVisibility(vt, 4, "panel.stepVT");
                                            setTextOn(vt, buf, "panel.stepVT");
                                            setTextColor(vt, {0.97f, 0.97f, 0.98f, 1.0f}, "panel.stepVT");
                                            applyFontScaled(vt, 0.9f);
                                            slotAlign(addChildTo(vbox, vt, "panel.stepVal"), 2, 2, "panel.stepVal");
                                            o.valText = vt;
                                        }
                                        UObject* s2 = addChildTo(hbO, vbox, "panel.stepVal");
                                        slotAlign(s2, -1, 2, "panel.stepVal");
                                    }
                                    o.hsInc = stepBtn(L"▶");
                                }
                                UObject* s = addChildTo(vbC, rowBox, "panel.optRow");
                                slotAlign(s, 0, -1, "panel.optRow");
                                slotPad(s, 0, 8, 38, 0, "panel.optRow");
                            }
                            if (visTotal > FOLD_OVER)
                            {
                                // 펼치기/접기 행 (옵션 밴드보다 얕은 톤, 가운데 글자)
                                UObject* rowBox = spawn(sCls, "panel.foldRow");
                                UObject* band = rowBox ? spawn(bCls, "panel.foldBand") : nullptr;
                                UObject* ftxt = band ? spawn(tCls, "panel.foldTxt") : nullptr;
                                if (rowBox && band && ftxt)
                                {
                                    float fh = 72.0f;
                                    callBytes(rowBox, L"SetHeightOverride", &fh, 4, "panel.foldRow");
                                    setVisibility(rowBox, 4, "panel.foldRow");
                                    setBrushColor(band, {1, 1, 1, 0.05f}, "panel.foldBand");
                                    setVisibility(band, 0, "panel.foldBand");  // 히트 대상
                                    {
                                        float m[4] = {0, 0, 74, 0};
                                        callBytes(band, L"SetPadding", m, 16, "panel.foldBand");
                                    }
                                    {   // v0.40(pad): 선택 테두리
                                        UObject* selOut = spawn(bCls, "panel.foldBand");
                                        if (selOut)
                                        {
                                            if (texSelFrame) callBytes(selOut, L"SetBrushFromTexture", &texSelFrame, 8, "panel.foldBand");
            setBrushColor(selOut, {0.95f, 0.92f, 0.80f, 0.0f}, "panel.foldBand");
                                            setVisibility(selOut, 4, "panel.foldBand");
                                            float sm[4] = {3, 3, 3, 3};
                                            callBytes(selOut, L"SetPadding", sm, 16, "panel.foldBand");
                                            slotAlign(addChildTo(rowBox, selOut, "panel.foldBand"), 0, 0, "panel.foldBand");
                                            slotAlign(addChildTo(selOut, band, "panel.foldBand"), 0, 0, "panel.foldBand");
                                        }
                                        else slotAlign(addChildTo(rowBox, band, "panel.foldBand"), 0, 0, "panel.foldBand");
                                        g_lastRowOutline = selOut;
                                        g_lastRowBox = rowBox;
                                        navAdd(NAVK_FOLD, g_plgN, -1);
                                    }
                                    wchar_t cap[64];
                                    if (folded)
                                        swprintf(cap, 64, TR(L"펼치기 ▼   (옵션 %d개 더)", L"Expand ▼   (%d more options)"), visTotal - FOLD_SHOW);
                                    else
                                        lstrcpynW(cap, TR(L"접기 ▲", L"Collapse ▲"), 64);
                                    setTextOn(ftxt, cap, "panel.foldTxt");
                                    setTextColor(ftxt, {0.70f, 0.74f, 0.78f, 1.0f}, "panel.foldTxt");
                                    applyFontScaled(ftxt, 0.8f);
                                    setVisibility(ftxt, 4, "panel.foldTxt");
                                    slotAlign(addChildTo(band, ftxt, "panel.foldTxt"), 2, 2, "panel.foldTxt");
                                    UObject* s = addChildTo(vbC, rowBox, "panel.foldRow");
                                    slotAlign(s, 0, -1, "panel.foldRow");
                                    slotPad(s, 0, 8, 38, 0, "panel.foldRow");
                                    r.expandHs = band;
                                }
                            }
                            for (int oi = 0; oi < r.optN; ++oi) paintOpt(r.opt[oi]);
                        }
                        ++g_plgN;
                    }
                    else if (UObject* st = spawn(tCls, "panel.modState"))  // 폴백: v0.5 상태 텍스트
                    {
                        setVisibility(st, 4, "panel.modState");
                        setTextOn(st, ents[k].pak ? TR(L"pak 모드 인식됨", L"pak mod detected") : (luaMod ? TR(L"Lua 모드 인식됨", L"Lua mod detected") : TR(L"C++ 모드 인식됨", L"C++ mod detected")), "panel.modState");
                        setTextColor(st, {0.52f, 0.57f, 0.62f, 1.0f}, "panel.modState");
                        applyFontScaled(st, 0.8f);
                        slotAlign(addChildTo(hb2, st, "panel.modState"), -1, 2, "panel.modState");
                    }
                }
                logf("panel: 플러그인 '%s' (lua=%d cpp=%d pak=%d)", u8(ents[k].name).c_str(), (int)luaMod, (int)cppMod, (int)ents[k].pak);
            }
        }
        if (shown == 0)
        {
            if (UObject* empty = spawn(tCls, "panel.empty"))
            {
                setVisibility(empty, 4, "panel.empty");
                setTextOn(empty, TR(L"plugins 폴더에 모드 폴더를 넣으면 여기에 표시됩니다.  (모드폴더\\Scripts\\main.lua 또는 모드폴더\\dlls\\main.dll)",
                              L"Put mod folders into the plugins folder and they appear here.  (ModFolder\\Scripts\\main.lua or ModFolder\\dlls\\main.dll)"), "panel.empty");
                setTextColor(empty, {0.52f, 0.57f, 0.62f, 1.0f}, "panel.empty");
                applyFontScaled(empty, 0.8f);
                slotPad(addChildTo(vbC, empty, "panel.empty"), 8, 20, 0, 0, "panel.empty");
            }
        }
    }
    }
    else
    {
        // ==================== 순서 탭 (v0.16) ====================
        // 모드 행을 누른 채 끌면 실시간 재배열, 놓으면 dsorder.txt 저장.
        // 위젯 행(밴드/라벨)은 고정 슬롯이고 드래그는 내용(name/label)만 옮긴다 --
        // 위젯 재부착 없이 setTextOn 재페인트라 레이아웃/포인터 수명 문제가 없다.
        addSection(TR(L"모드 순서", L"Mod order"), "panel.secOrd");
        if (UObject* tip = spawn(tCls, "panel.ordTip"))
        {
            setVisibility(tip, 4, "panel.ordTip");
            setTextOn(tip, TR(L"행을 누른 채 위아래로 끌면 순서가 바뀌고, 놓으면 저장됩니다.",
                              L"Hold a row and drag up or down to reorder; release to save."), "panel.ordTip");
            setTextColor(tip, {0.52f, 0.57f, 0.62f, 1.0f}, "panel.ordTip");
            applyFontScaled(tip, 0.8f);
            slotPad(addChildTo(vbC, tip, "panel.ordTip"), 8, 16, 0, 0, "panel.ordTip");
        }
        static PlgEnt ents[MM_MAX_PLUGINS];  // 게임 스레드 전용 -- 스택 절약
        int entN = collectPlugins(ents, MM_MAX_PLUGINS);
        g_ordN = 0;
        g_dragIdx = -1;
        g_dragMoved = false;
        for (int k = 0; k < entN; ++k)
        {
            OrderRow& orow = g_ord[g_ordN];
            memset(&orow, 0, sizeof(orow));
            lstrcpynW(orow.name, ents[k].name, 64);
            {   // 표시명: 매니페스트 [plugin] name= 만 가볍게 읽는다 (옵션 파싱 불필요)
                wchar_t mp[MAX_PATH * 2];
                pluginSrcPath(mp, ents[k].rel[0] ? ents[k].rel : ents[k].name);
                lstrcatW(mp, L"\\dsplugin.ini");
                std::string ini = readFileA(mp);
                if (!ini.empty())
                {
                    if (ini.size() >= 3 && ini.compare(0, 3, "\xEF\xBB\xBF") == 0) ini.erase(0, 3);
                    std::string nm = iniValueLang(ini, "plugin", "name");
                    if (!nm.empty()) utf8ToW(nm, orow.label, 64);
                }
            }
            if (!orow.label[0]) lstrcpynW(orow.label, orow.name, 64);
            // 행: 밴드(히트/하이라이트) + ≡ 핸들 + 라벨
            UObject* rowBox = spawn(sCls, "panel.ordRow");
            if (!rowBox) break;
            float fh = 96.0f;
            callBytes(rowBox, L"SetHeightOverride", &fh, 4, "panel.ordRow");
            setVisibility(rowBox, 4, "panel.ordRow");
            UObject* band = spawn(bCls, "panel.ordBand");
            if (!band) break;
            setBrushColor(band, {1, 1, 1, 0.07f}, "panel.ordBand");
            setVisibility(band, 0, "panel.ordBand");  // 히트 대상 (잡기/통과 판정)
            {
                float m[4] = {0, 0, 74, 0};
                callBytes(band, L"SetPadding", m, 16, "panel.ordBand");
            }
            {   // v0.40(pad): 선택 테두리
                UObject* selOut = spawn(bCls, "panel.ordBand");
                if (selOut)
                {
                    if (texSelFrame) callBytes(selOut, L"SetBrushFromTexture", &texSelFrame, 8, "panel.ordBand");
            setBrushColor(selOut, {0.95f, 0.92f, 0.80f, 0.0f}, "panel.ordBand");
                    setVisibility(selOut, 4, "panel.ordBand");
                    float sm[4] = {3, 3, 3, 3};
                    callBytes(selOut, L"SetPadding", sm, 16, "panel.ordBand");
                    slotAlign(addChildTo(rowBox, selOut, "panel.ordBand"), 0, 0, "panel.ordBand");
                    slotAlign(addChildTo(selOut, band, "panel.ordBand"), 0, 0, "panel.ordBand");
                }
                else slotAlign(addChildTo(rowBox, band, "panel.ordBand"), 0, 0, "panel.ordBand");
                g_lastRowOutline = selOut;
                g_lastRowBox = rowBox;
                navAdd(NAVK_ORD, g_ordN, -1);
            }
            UObject* hbO = spawn(hCls, "panel.ordHb");
            if (!hbO) break;
            setVisibility(hbO, 4, "panel.ordHb");
            slotAlign(addChildTo(band, hbO, "panel.ordHb"), 0, 0, "panel.ordHb");
            if (UObject* grip = spawn(tCls, "panel.ordGrip"))
            {
                setVisibility(grip, 4, "panel.ordGrip");
                setTextOn(grip, L"≡", "panel.ordGrip");
                setTextColor(grip, {0.52f, 0.57f, 0.62f, 1.0f}, "panel.ordGrip");
                applyFontScaled(grip, 1.0f);
                UObject* gs2 = addChildTo(hbO, grip, "panel.ordGrip");
                slotAlign(gs2, -1, 2, "panel.ordGrip");
                slotPad(gs2, 34, 0, 0, 0, "panel.ordGrip");
            }
            if (UObject* lbl = spawn(tCls, "panel.ordLbl"))
            {
                setVisibility(lbl, 4, "panel.ordLbl");
                setTextOn(lbl, orow.label, "panel.ordLbl");
                setTextColor(lbl, {0.93f, 0.95f, 0.96f, 1.0f}, "panel.ordLbl");
                applyFontScaled(lbl, 1.0f);
                UObject* ls = addChildTo(hbO, lbl, "panel.ordLbl");
                slotAlign(ls, -1, 2, "panel.ordLbl");
                slotPad(ls, 26, 0, 0, 0, "panel.ordLbl");
                slotFillWidth(ls, "panel.ordLbl");
                orow.text = lbl;
            }
            {   // v0.40: 모드 종류 (Lua / C++ / pak) -- 행 우측단 (실측 mod2.png)
                const wchar_t* kindTxt = ents[k].pak ? L"pak" : (ents[k].cpp ? L"C++" : L"Lua");
                if (UObject* kt = spawn(tCls, "panel.ordKind"))
                {
                    setVisibility(kt, 4, "panel.ordKind");
                    setTextOn(kt, kindTxt, "panel.ordKind");
                    setTextColor(kt, {0.52f, 0.57f, 0.62f, 1.0f}, "panel.ordKind");
                    applyFontScaled(kt, 0.8f);
                    UObject* ks = addChildTo(hbO, kt, "panel.ordKind");
                    slotAlign(ks, -1, 2, "panel.ordKind");
                    slotPad(ks, 0, 0, 40, 0, "panel.ordKind");
                }
            }
            orow.band = band;
            UObject* s = addChildTo(vbC, rowBox, "panel.ordRow");
            slotAlign(s, 0, -1, "panel.ordRow");
            slotPad(s, 0, 14, 38, 0, "panel.ordRow");
            ++g_ordN;
        }
        if (g_ordN == 0)
        {
            if (UObject* empty = spawn(tCls, "panel.ordEmpty"))
            {
                setVisibility(empty, 4, "panel.ordEmpty");
                setTextOn(empty, TR(L"정렬할 모드가 없습니다. plugins 폴더에 모드를 넣으면 여기서 순서를 정할 수 있습니다.",
                                L"Nothing to sort. Put mods into the plugins folder and order them here."), "panel.ordEmpty");
                setTextColor(empty, {0.52f, 0.57f, 0.62f, 1.0f}, "panel.ordEmpty");
                applyFontScaled(empty, 0.8f);
                slotPad(addChildTo(vbC, empty, "panel.ordEmpty"), 8, 20, 0, 0, "panel.ordEmpty");
            }
        }
        logf("panel: 순서 탭 %d행", g_ordN);
    }

    // 하단 고정 줄: 좌측 제작자명 · 우측 조작 힌트
    if (UObject* bottom = spawn(hCls, "panel.bottom"))
    {
        setVisibility(bottom, 4, "panel.bottom");
        if (UObject* credit = spawn(tCls, "panel.credit"))
        {
            setVisibility(credit, 4, "panel.credit");
            setTextOn(credit, L"made by SummerSpring", "panel.credit");
            setTextColor(credit, {0.52f, 0.57f, 0.62f, 1.0f}, "panel.credit");
            applyFontScaled(credit, 0.8f);
            UObject* cs = addChildTo(bottom, credit, "panel.credit");
            slotAlign(cs, -1, 2, "panel.credit");
            slotFillWidth(cs, "panel.credit");  // 좌측 붙임 + 남은 폭 차지 -> 힌트가 우측 끝으로
        }
        // v0.40: 우측 = 패드 힌트 (아이콘 + 글자). 패드를 쓸 때만 보인다.
        // (ESC/버전 문구는 사용자 요청으로 제거 -- 버전은 [기본] 탭 '모드 버전' 행에 있다)
        if (UObject* ph = spawn(hCls, "panel.padHint"))
        {
            setVisibility(ph, g_inputMode.load(std::memory_order_relaxed) == 1 ? 4 : 1, "panel.padHint");
            auto hintIcon = [&](const wchar_t* texName, const wchar_t* cap) {
                UObject* ib = spawn(sCls, "panel.phIco");
                if (ib)
                {
                    float iw = 40.0f, ih = 40.0f;
                    callBytes(ib, L"SetWidthOverride", &iw, 4, "panel.phIco");
                    callBytes(ib, L"SetHeightOverride", &ih, 4, "panel.phIco");
                    setVisibility(ib, 4, "panel.phIco");
                    UObject* htex = importTex(texName, "panel.phIco");
                    if (UObject* im2 = spawn(bCls, "panel.phIcoB"))
                    {
                        if (htex) callBytes(im2, L"SetBrushFromTexture", &htex, 8, "panel.phIcoB");
                        setBrushColor(im2, {1, 1, 1, 1}, "panel.phIcoB");
                        setVisibility(im2, 4, "panel.phIcoB");
                        slotAlign(addChildTo(ib, im2, "panel.phIco"), 0, 0, "panel.phIco");
                    }
                    UObject* is2 = addChildTo(ph, ib, "panel.phIco");
                    slotAlign(is2, -1, 2, "panel.phIco");
                    slotPad(is2, 26, 0, 8, 0, "panel.phIco");
                }
                if (UObject* tx = spawn(tCls, "panel.phTxt"))
                {
                    setVisibility(tx, 4, "panel.phTxt");
                    setTextOn(tx, cap, "panel.phTxt");
                    setTextColor(tx, {0.85f, 0.88f, 0.90f, 1.0f}, "panel.phTxt");
                    applyFontScaled(tx, 0.8f);
                    slotAlign(addChildTo(ph, tx, "panel.phTxt"), -1, 2, "panel.phTxt");
                }
            };
            hintIcon(L"pad_a.png", TR(L"선택", L"Select"));
            hintIcon(L"pad_b.png", TR(L"닫기", L"Close"));
            slotAlign(addChildTo(bottom, ph, "panel.padHint"), -1, 2, "panel.padHint");
            g_padHintBox = ph;
        }
        UObject* s = addChildTo(vb, bottom, "panel.bottom");
        slotAlign(s, 0, -1, "panel.bottom");
        slotPad(s, 0, 24, 0, 0, "panel.bottom");
    }

    // 표시: 호스트는 0(클릭 감시자 있음 -- 우리 펌프), 뷰포트 ZOrder 1000
    panelInputMode(true);   // v0.40: 게임(메뉴)의 패드 처리 차단 -- '나가기' 오발 방지
    g_restoreInputPending = false;   // 다시 열렸으면 지연 복구는 무효
    g_restoreWaitDir = false;
    setVisibility(host, 0, "panel.host");
    int z = 1000;
    if (!callBytes(host, L"AddToViewport", &z, 4, "panel.viewport"))
    {
        logf("FAIL panel: AddToViewport 실패 -- 고아 호스트는 GC 에 맡김");
        g_restoreInputPending = true;    // 위에서 건 UIOnly 가 동결로 남지 않게(리뷰)
        g_restoreWaitDir = false;
        for (int i = 0; i < 3; ++i) g_hsX[i] = nullptr;
        for (int i = 0; i < 3; ++i) g_hsBtn[i] = nullptr;
        g_plgN = 0;
        return false;
    }

    g_panel = (void*)host;
    g_panelOpen = true;
    // v0.17: 재구축 전 스크롤 위치 되돌리기 -- 지금은 레이아웃 전이라 값이 안 물 수
    // 있어, 여기서 한 번 쏘고 펌프가 짧게 재시도한다(닿으면 즉시 중단).
    if (g_pendScroll >= 0.0f && g_scrollBox)
    {
        writeScrollOffset(g_scrollBox, g_pendScroll);
        g_pendScrollUntilMs = GetTickCount64() + 900;
        logf("panel: 스크롤 위치 복원 시도 (%.0f)", g_pendScroll);
    }
    else g_pendScroll = -1.0f;
    setSelectedRow(clone, false);
    g_lastHover = false;
    // v0.8: 펄스 재무장 -- 패널 열림 = PE 기근 구간의 시작이므로, 같은 델리게이트
    // K2_SetTimer 는 기존 타이머를 리셋만 한다(중복 누적 없음). 어떤 이유로
    // 펄스가 죽어 있었어도 여기서 되살아난다.
    startPulseTimer(clone);
    startPulseTimer(host);  // v0.8.2: 독립 보험 펄스 (다른 오브젝트 = 다른 델리게이트)
    logf("MM panel: OPEN 성공 host=%p", (void*)host);
    return true;
}

// ======================= v0.7: 재시작 안내 팝업 ============================

static void closePopup(const char* why)
{
    g_popupOpen = false;
    g_hsPop[0] = g_hsPop[1] = nullptr;
    g_popupPadIcon = nullptr;   // v0.50: 팝업과 함께 죽는다
    void* p = g_popup;
    g_popup = nullptr;
    if (p)
    {
        UObject* w = reinterpret_cast<UObject*>(p);
        UFunction* fn = fnOf(w, L"RemoveFromParent", "closePopup");
        if (fn && (int)fn->GetParmsSize() == 0)
        {
            PB pb;
            if (!peGuard(w, fn, pb.b)) logf("WARN closePopup: SEH");
        }
    }
    logf("popup: 닫힘 (%s)", why);
}

// 게임 범용 확인 팝업(DPopupDefault_C, 덤프 실측: TitleText@0x488 DescriptionText@0x490
// Button_Confirm@0x458 등) 복제를 1차 시도. 팝업 BP 는 요청 시 로드라 타이틀에서
// 클래스가 없을 수 있고, 그때는 설정창 스타일 원시 팝업으로 폴백한다.
// v0.14: desc 로 본문 교체 가능 (모드발 dsnotify 신호의 사용자 정의 메시지)
static bool showRestartPopup(const wchar_t* desc = nullptr)
{
    if (g_popupOpen) return true;
    g_popupPadIcon = nullptr;   // v0.50 리뷰: 실패 경로가 스테일 포인터를 남기지 않게
    const wchar_t* descText = desc ? desc : TR(L"모드 적용을 위해 게임 재시작이 필요합니다.",
                                               L"A game restart is needed to apply the mod.");
    // v0.25: 띄운 문구를 그대로 로그에 남긴다 -- 사용자가 팝업을 닫아 버려도
    // 무슨 일이 있었는지 cppmm_log.txt 만 보면 재구성된다.
    logf("popup 요청: \"%s\"", u8(descText).c_str());
    UObject* mc = reinterpret_cast<UObject*>(g_myClone.load(std::memory_order_relaxed));
    UObject* pc = UOG::FindFirstOf(L"PlayerController");
    UObject* lib = findObj(L"/Script/UMG.Default__WidgetBlueprintLibrary", "popup");
    if (!lib || (!pc && !mc)) return false;
    UFunction* fnCreate = fnOf(lib, L"Create", "popup.create");
    if (!fnCreate || !parmsExact(fnCreate, 32, "popup.Create", false)) return false;
    UObject* ctx = pc ? pc : mc;

    auto createW = [&](UObject* cls) -> UObject* {
        PB pb;
        memcpy(pb.b + 0, &ctx, 8);
        memcpy(pb.b + 8, &cls, 8);
        memcpy(pb.b + 16, &pc, 8);
        if (!peGuard(lib, fnCreate, pb.b)) return nullptr;
        void* w = nullptr;
        memcpy(&w, pb.b + (int)fnCreate->GetReturnValueOffset(), 8);
        return reinterpret_cast<UObject*>(w);
    };

    // ---- 1차: 게임 팝업 복제 ----
    if (UObject* popCls = findObj(L"/Game/UI/Popup/DPopupDefault.DPopupDefault_C", "popup.cls"))
    {
        if (UObject* pop = createW(popCls))
        {
            UObject* tt = readObjProp(pop, L"TitleText", "popup");
            UObject* dt = readObjProp(pop, L"DescriptionText", "popup");
            if (tt) setTextOn(tt, TR(L"모드 매니저", L"ModManager"), "popup.title");
            if (dt)
            {
                setTextOn(dt, descText, "popup.desc");
                // v0.25: 게임 팝업의 본문도 감싸 준다 (긴 문구가 상자 밖으로 나가던 문제)
                unsigned char wrapOn = 1;
                callBytes(dt, L"SetAutoWrapText", &wrapOn, 1, "popup.descWrap");
            }
            if (UObject* bc = readObjProp(pop, L"Button_Cancel", "popup"))
                setVisibility(bc, 1, "popup.cancel");
            if (UObject* cbp = readObjProp(pop, L"CheckBoxPanel", "popup"))
                setVisibility(cbp, 1, "popup.check");
            UObject* ok = readObjProp(pop, L"Button_Confirm", "popup");
            UObject* xb = readObjProp(pop, L"CloseButton", "popup");
            unsigned char b1 = 1;
            if (ok)
            {
                setVisibility(ok, 0, "popup.ok");
                callBytes(ok, L"SetIsEnabled", &b1, 1, "popup.ok");
            }
            if (xb)
            {
                setVisibility(xb, 0, "popup.x");
                callBytes(xb, L"SetIsEnabled", &b1, 1, "popup.x");
            }
            int z = 1100;
            if ((ok || xb) && callBytes(pop, L"AddToViewport", &z, 4, "popup.viewport"))
            {
                // Construct 가 텍스트를 되돌릴 수 있다 (메뉴 클론 실측 규약) -- 후처리 재설정
                if (tt) setTextOn(tt, TR(L"모드 매니저", L"ModManager"), "popup.title2");
                if (dt) setTextOn(dt, descText, "popup.desc2");
                g_popup = (void*)pop;
                g_hsPop[0] = ok;
                g_hsPop[1] = xb;
                g_popupOpen = true;
                if (mc) startPulseTimer(mc);  // v0.8: 펄스 재무장
                logf("popup: 게임 DPopupDefault 복제 표시 성공");
                return true;
            }
        }
        logf("WARN popup: DPopupDefault 복제 실패 -- 원시 폴백");
    }
    else logf("popup: DPopupDefault_C 미로드 -- 원시 폴백");

    // ---- 2차: 원시 폴백 (설정창 스타일 근사, 중앙 정렬 = BorderSlot 센터) ----
    UObject* gs = findObj(L"/Script/Engine.Default__GameplayStatics", "popup");
    UObject* hostCls = findObj(L"/Script/UMG.UserWidget", "popup");
    UObject* bCls = findObj(L"/Script/UMG.Border", "popup");
    UObject* tCls = findObj(L"/Script/UMG.TextBlock", "popup");
    UObject* sCls = findObj(L"/Script/UMG.SizeBox", "popup");
    UObject* vCls = findObj(L"/Script/UMG.VerticalBox", "popup");
    UObject* hCls = findObj(L"/Script/UMG.HorizontalBox", "popup");
    if (!gs || !hostCls || !bCls || !tCls || !sCls || !vCls) return false;
    UObject* host = createW(hostCls);
    if (!host) return false;
    UObject* tree = readObjProp(host, L"WidgetTree", "popup.tree");
    if (!tree) return false;
    UFunction* fnSpawn = fnOf(gs, L"SpawnObject", "popup.spawn");
    if (!fnSpawn || !parmsExact(fnSpawn, 24, "popup.spawn", false)) return false;
    auto spawn2 = [&](UObject* cls) -> UObject* {
        PB pb;
        memcpy(pb.b + 0, &cls, 8);
        memcpy(pb.b + 8, &tree, 8);
        if (!peGuard(gs, fnSpawn, pb.b)) return nullptr;
        void* w = nullptr;
        memcpy(&w, pb.b + (int)fnSpawn->GetReturnValueOffset(), 8);
        return reinterpret_cast<UObject*>(w);
    };
    // v0.50: 확인 버튼에 붙일 (A) 아이콘 텍스처
    UObject* padATex = nullptr;
    {
        UObject* krl = findObj(L"/Script/Engine.Default__KismetRenderingLibrary", "popup");
        UFunction* fnImpA = krl ? fnOf(krl, L"ImportFileAsTexture2D", "popup") : nullptr;
        if (fnImpA && (int)fnImpA->GetParmsSize() == 32)
        {
            static wchar_t tpA[MAX_PATH * 2];
            assetPath(tpA, L"pad_a.png");
            PB pb;
            UObject* c3 = mc ? mc : host;
            memcpy(pb.b + 0, &c3, 8);
            FStringRaw fs{tpA, (int)wcslen(tpA) + 1, (int)wcslen(tpA) + 1};
            memcpy(pb.b + 8, &fs, 16);
            if (peGuard(krl, fnImpA, pb.b))
                memcpy(&padATex, pb.b + (int)fnImpA->GetReturnValueOffset(), 8);
        }
    }

    UObject* dim = spawn2(bCls);
    if (!dim) return false;
    {
        int off = propOffset(tree, L"RootWidget", 8, "popup.root");
        if (off < 0 || !writePtrGuard(tree, off, dim)) return false;
    }
    setBrushColor(dim, {0, 0, 0, 0.45f}, "popup.dim");
    setVisibility(dim, 0, "popup.dim");  // 뒤 클릭 흡수 (모달)

    // 폰트 차용 (메뉴 클론의 TitleText)
    unsigned char font[96];
    bool haveFont = false;
    if (mc)
        if (UObject* mt = readObjProp(mc, L"TitleText", "popup.font"))
        {
            int foff = propOffset(mt, L"Font", 88, "popup.font");
            if (foff >= 0 && readBytesGuard(mt, foff, font, 88)) haveFont = true;
        }
    auto pfont = [&](UObject* tb, float scale) {
        if (!haveFont) return;
        unsigned char fb[88];
        memcpy(fb, font, 88);
        if (scale != 1.0f)
        {
            float s;
            memcpy(&s, fb + 0x48, 4);
            s *= scale;
            memcpy(fb + 0x48, &s, 4);
        }
        callBytes(tb, L"SetFont", fb, 88, "popup.font");
    };

    UObject* boxSize = spawn2(sCls);
    UObject* box = boxSize ? spawn2(bCls) : nullptr;
    UObject* pvb = box ? spawn2(vCls) : nullptr;
    if (!boxSize || !box || !pvb) return false;
    // v0.25: 폭만 고정하고 **높이는 내용에 맞춰 자란다**. 예전엔 높이도 380 으로
    // 못박아, 문구가 길면 상자 밖으로 삐져나왔다(사용자 스크린샷 실측).
    // 최소 높이만 줘서 짧은 문구에서도 대화상자 꼴을 유지한다.
    float bw = 960.0f, bhMin = 320.0f;
    callBytes(boxSize, L"SetWidthOverride", &bw, 4, "popup.box");
    callBytes(boxSize, L"SetMinDesiredHeight", &bhMin, 4, "popup.box");
    setVisibility(boxSize, 4, "popup.box");
    slotAlign(addChildTo(dim, boxSize, "popup.box"), 2, 2, "popup.box");  // 중앙 정렬
    setBrushColor(box, {0.012f, 0.015f, 0.018f, 0.97f}, "popup.boxBg");
    setVisibility(box, 4, "popup.boxBg");
    {
        float m[4] = {48, 36, 48, 36};
        callBytes(box, L"SetPadding", m, 16, "popup.boxBg");
    }
    slotAlign(addChildTo(boxSize, box, "popup.boxBg"), 0, 0, "popup.boxBg");
    setVisibility(pvb, 4, "popup.vb");
    slotAlign(addChildTo(box, pvb, "popup.vb"), 0, 0, "popup.vb");

    if (UObject* t = spawn2(tCls))
    {
        setVisibility(t, 4, "popup.t");
        setTextOn(t, TR(L"모드 매니저", L"ModManager"), "popup.t");
        setTextColor(t, {0.97f, 0.97f, 0.98f, 1.0f}, "popup.t");
        pfont(t, 1.05f);
        addChildTo(pvb, t, "popup.t");
    }
    if (UObject* dv = spawn2(sCls))
    {
        float f2 = 2.0f;
        callBytes(dv, L"SetHeightOverride", &f2, 4, "popup.dv");
        setVisibility(dv, 4, "popup.dv");
        if (UObject* dl = spawn2(bCls))
        {
            setBrushColor(dl, {1, 1, 1, 0.16f}, "popup.dv");
            setVisibility(dl, 4, "popup.dv");
            slotAlign(addChildTo(dv, dl, "popup.dv"), 0, 0, "popup.dv");
        }
        UObject* s = addChildTo(pvb, dv, "popup.dv");
        slotAlign(s, 0, -1, "popup.dv");
        slotPad(s, 0, 18, 0, 0, "popup.dv");
    }
    if (UObject* d = spawn2(tCls))
    {
        setVisibility(d, 4, "popup.d");
        setTextOn(d, descText, "popup.d");
        setTextColor(d, {0.93f, 0.95f, 0.96f, 1.0f}, "popup.d");
        pfont(d, 0.9f);
        // v0.25: 자동 줄바꿈. ⚠ 슬롯을 Fill(0) 로 둬야 감쌀 폭이 정해진다 --
        // 가운데 정렬(2)이면 TextBlock 이 원하는 만큼 늘어나 줄바꿈이 안 걸린다.
        unsigned char wrapOn = 1;
        callBytes(d, L"SetAutoWrapText", &wrapOn, 1, "popup.dWrap");
        unsigned char just = 1;  // ETextJustify: 0=Left 1=Center 2=Right
        callBytes(d, L"SetJustification", &just, 1, "popup.dJust");
        UObject* s = addChildTo(pvb, d, "popup.d");
        slotAlign(s, 0, -1, "popup.d");
        slotPad(s, 0, 44, 0, 0, "popup.d");
    }
    UObject* okOutline = nullptr;
    if (UObject* obox = spawn2(sCls))
    {
        float ow = 300.0f, oh = 84.0f;
        callBytes(obox, L"SetWidthOverride", &ow, 4, "popup.ok");
        callBytes(obox, L"SetHeightOverride", &oh, 4, "popup.ok");
        setVisibility(obox, 4, "popup.ok");
        UObject* outline = spawn2(bCls);
        UObject* fill = outline ? spawn2(bCls) : nullptr;
        UObject* bt = fill ? spawn2(tCls) : nullptr;
        if (outline && fill && bt)
        {
            setBrushColor(outline, {0.285f, 0.336f, 0.333f, 0.95f}, "popup.ok");
            setVisibility(outline, 0, "popup.ok");  // 히트 대상
            float m[4] = {3, 3, 3, 3};
            callBytes(outline, L"SetPadding", m, 16, "popup.ok");
            setBrushColor(fill, {0.010f, 0.014f, 0.016f, 0.55f}, "popup.ok");
            setVisibility(fill, 4, "popup.ok");
            setTextOn(bt, TR(L"확인", L"OK"), "popup.ok");
            setTextColor(bt, {0.97f, 0.97f, 0.98f, 1.0f}, "popup.ok");
            pfont(bt, 0.9f);
            setVisibility(bt, 4, "popup.ok");
            slotAlign(addChildTo(obox, outline, "popup.ok"), 0, 0, "popup.ok");
            slotAlign(addChildTo(outline, fill, "popup.ok"), 0, 0, "popup.ok");
            // v0.50: 확인 텍스트 옆 (A) 아이콘 -- 패드 사용 시만 표시(패드로도 닫힘 안내)
            UObject* okhb = hCls ? spawn2(hCls) : nullptr;
            if (okhb)
            {
                slotAlign(addChildTo(okhb, bt, "popup.ok"), -1, 2, "popup.ok");
                if (UObject* ab = spawn2(sCls))
                {
                    float aw = 40.0f;
                    callBytes(ab, L"SetWidthOverride", &aw, 4, "popup.ok");
                    callBytes(ab, L"SetHeightOverride", &aw, 4, "popup.ok");
                    if (UObject* ai = spawn2(bCls))
                    {
                        if (padATex) callBytes(ai, L"SetBrushFromTexture", &padATex, 8, "popup.ok");
                        setBrushColor(ai, {1, 1, 1, 1}, "popup.ok");
                        setVisibility(ai, 4, "popup.ok");
                        slotAlign(addChildTo(ab, ai, "popup.ok"), 0, 0, "popup.ok");
                    }
                    bool po = g_inputMode.load(std::memory_order_relaxed) == 1 &&
                              g_padPresent.load(std::memory_order_relaxed);
                    setVisibility(ab, po ? 4 : 1, "popup.ok");
                    g_popupPadIcon = (void*)ab;
                    UObject* asl = addChildTo(okhb, ab, "popup.ok");
                    slotAlign(asl, -1, 2, "popup.ok");
                    slotPad(asl, 10, 0, 0, 0, "popup.ok");
                }
                slotAlign(addChildTo(fill, okhb, "popup.ok"), 2, 2, "popup.ok");
            }
            else slotAlign(addChildTo(fill, bt, "popup.ok"), 2, 2, "popup.ok");
            okOutline = outline;
        }
        UObject* s = addChildTo(pvb, obox, "popup.ok");
        slotAlign(s, 2, -1, "popup.ok");
        slotPad(s, 0, 40, 0, 0, "popup.ok");
    }

    setVisibility(host, 0, "popup.host");
    int z = 1100;
    if (!callBytes(host, L"AddToViewport", &z, 4, "popup.viewport")) return false;
    g_popup = (void*)host;
    g_hsPop[0] = okOutline;
    g_hsPop[1] = nullptr;
    g_popupOpen = true;
    if (mc) startPulseTimer(mc);  // v0.8: 펄스 재무장
    logf("popup: 원시 폴백 표시 (DPopupDefault 미로드)");
    return true;
}

// ======================= v0.15: 콤보 드롭다운 ==============================
// 설정창 언어 콤보 픽셀 실측 재현: 폭 = 닫힘 알약과 동일(675), 항목 피치 88
// (높이 82), 패널 배경 sRGB{39,45,41} 불투명, 호버 항목 = 카키 {148,145,111}
// + 어두운 글자 + 우측 ◆ 마커, 평상시 글자 {164,177,175}.
// 드롭다운은 별도 뷰포트 호스트(ZOrder 1100) -- 앵커(닫힘 알약)의 절대 사각형을
// 루트 Border 패딩으로 옮겨 바로 아래 겹쳐 띄운다(패널 레이아웃 안 밀림).
// 루트가 전화면 클릭을 흡수하므로 바깥 클릭 = 닫기가 자연히 성립한다.

// v0.15.1: 닫힘 알약의 브러시 교체 -- 게임 실측 2상태 재현.
// 평상시 combo_pill.png(희미 이중선), 드롭다운 열림 combo_pill_focus.png(흰 광륜).
static void setComboPillTex(void* pill, const wchar_t* file)
{
    if (!pill) return;
    UObject* pc = UOG::FindFirstOf(L"PlayerController");
    UObject* mc = reinterpret_cast<UObject*>(g_myClone.load(std::memory_order_relaxed));
    UObject* ctx = pc ? pc : mc;
    UObject* krl = findObj(L"/Script/Engine.Default__KismetRenderingLibrary", "combo.tex");
    if (!ctx || !krl) return;
    UFunction* fnImp = fnOf(krl, L"ImportFileAsTexture2D", "combo.tex");
    if (!fnImp || !parmsExact(fnImp, 32, "combo.tex", false)) return;
    static wchar_t tp[MAX_PATH * 2];
    assetPath(tp, file);
    PB pb;
    memcpy(pb.b + 0, &ctx, 8);
    FStringRaw fs{tp, (int)wcslen(tp) + 1, (int)wcslen(tp) + 1};
    memcpy(pb.b + 8, &fs, 16);
    if (!peGuard(krl, fnImp, pb.b)) return;
    void* tex = nullptr;
    memcpy(&tex, pb.b + (int)fnImp->GetReturnValueOffset(), 8);
    if (!tex) return;
    callBytes(reinterpret_cast<UObject*>(pill), L"SetBrushFromTexture", &tex, 8, "combo.tex");
}

static void closeCombo(const char* why)
{
    if (!g_comboOpen && !g_comboHost) return;
    if (g_comboAnchorPill)
    {
        setComboPillTex(g_comboAnchorPill, L"combo_pill.png");  // 포커스 외곽선 해제
        g_comboAnchorPill = nullptr;
    }
    g_comboOpen = false;
    g_comboN = 0;
    g_comboHover = -1;
    g_comboPadSel = -1;   // v0.40(pad)
    g_comboRow = -1;
    g_comboOpt = 0;
    g_comboValTx = nullptr;
    g_comboScrollBox = nullptr;  // v0.18
    g_sbShown[1] = false;
    g_sbLastOff[1] = -1.0f;
    if (g_dsWhich == 1) clearDragScroll();
    if (g_armKind == ARM_COMBO_ITEM) clearArm();
    for (int i = 0; i < 10; ++i)
        g_comboItemHs[i] = g_comboItemTx[i] = g_comboItemMk[i] = nullptr;
    void* p = g_comboHost;
    g_comboHost = nullptr;
    if (p)
    {
        UObject* w = reinterpret_cast<UObject*>(p);
        UFunction* fn = fnOf(w, L"RemoveFromParent", "closeCombo");
        if (fn && (int)fn->GetParmsSize() == 0)
        {
            PB pb;
            if (!peGuard(w, fn, pb.b)) logf("WARN closeCombo: RemoveFromParent SEH");
        }
    }
    logf("combo: 닫힘 (%s)", why);
}

// 항목 호버 페인팅 -- 하이라이트/글자색/◆ 마커를 한 번에 (i<0 은 무시)
static void paintComboItem(int i, bool on)
{
    if (i < 0 || i >= g_comboN) return;
    const LinColor KHAKI = {0.297f, 0.285f, 0.163f, 1.0f};   // 실측 {148,145,111}
    const LinColor TXT_N = {0.372f, 0.44f, 0.428f, 1.0f};    // 실측 {164,177,175}
    const LinColor TXT_H = {0.04f, 0.045f, 0.035f, 1.0f};    // 카키 위 어두운 글자
    if (g_comboItemHs[i])
        setBrushColor(reinterpret_cast<UObject*>(g_comboItemHs[i]),
                      on ? KHAKI : LinColor{1, 1, 1, 0}, "combo.hl");
    if (g_comboItemTx[i])
        setTextColor(reinterpret_cast<UObject*>(g_comboItemTx[i]), on ? TXT_H : TXT_N, "combo.tx");
    if (g_comboItemMk[i])
        setTextColor(reinterpret_cast<UObject*>(g_comboItemMk[i]),
                     on ? LinColor{1, 1, 1, 1} : LinColor{1, 1, 1, 0}, "combo.mk");
}

// 드롭다운 열기. labels = 항목 라벨 배열(최대 10), row/opt = 소유자
// (row -1 = [기본] 테스트 데모), valTx = 닫힘 알약의 값 텍스트(선택 반영용).
static bool openCombo(UObject* anchor, const wchar_t labels[][24], int n, int cur,
                      int row, int opt, void* valTx)
{
    (void)cur;  // 게임 실측: 열림 직후엔 아무 항목도 하이라이트 없음 (호버에만 반응)
    if (g_comboOpen) closeCombo("재열림");
    if (!anchor || n < 2) return false;
    if (n > 10) n = 10;
    double ax0, ay0, ax1, ay1;
    if (!widgetRectAbs(anchor, &ax0, &ay0, &ax1, &ay1))
    {
        logf("WARN combo: 앵커 사각형 판정 불가 -- 드롭다운 생략");
        return false;
    }

    UObject* mc = reinterpret_cast<UObject*>(g_myClone.load(std::memory_order_relaxed));
    UObject* pc = UOG::FindFirstOf(L"PlayerController");
    UObject* lib = findObj(L"/Script/UMG.Default__WidgetBlueprintLibrary", "combo");
    UObject* gs = findObj(L"/Script/Engine.Default__GameplayStatics", "combo");
    UObject* hostCls = findObj(L"/Script/UMG.UserWidget", "combo");
    UObject* bCls = findObj(L"/Script/UMG.Border", "combo");
    UObject* tCls = findObj(L"/Script/UMG.TextBlock", "combo");
    UObject* sCls = findObj(L"/Script/UMG.SizeBox", "combo");
    UObject* vCls = findObj(L"/Script/UMG.VerticalBox", "combo");
    UObject* hCls = findObj(L"/Script/UMG.HorizontalBox", "combo");
    UObject* sbCls = findObj(L"/Script/UMG.ScrollBox", "combo");
    if (!lib || !gs || !hostCls || !bCls || !tCls || !sCls || !vCls || !hCls) return false;
    if (!pc && !mc) return false;
    UFunction* fnCreate = fnOf(lib, L"Create", "combo.create");
    if (!fnCreate || !parmsExact(fnCreate, 32, "combo.create", false)) return false;
    UObject* ctx = pc ? pc : mc;
    PB pbC;
    memcpy(pbC.b + 0, &ctx, 8);
    memcpy(pbC.b + 8, &hostCls, 8);
    memcpy(pbC.b + 16, &pc, 8);
    if (!peGuard(lib, fnCreate, pbC.b)) return false;
    void* hostRaw = nullptr;
    memcpy(&hostRaw, pbC.b + (int)fnCreate->GetReturnValueOffset(), 8);
    UObject* host = reinterpret_cast<UObject*>(hostRaw);
    if (!host) return false;
    UObject* tree = readObjProp(host, L"WidgetTree", "combo.tree");
    if (!tree) return false;
    UFunction* fnSpawn = fnOf(gs, L"SpawnObject", "combo.spawn");
    if (!fnSpawn || !parmsExact(fnSpawn, 24, "combo.spawn", false)) return false;
    auto spawn = [&](UObject* cls, const char* tag) -> UObject* {
        PB pb;
        memcpy(pb.b + 0, &cls, 8);
        memcpy(pb.b + 8, &tree, 8);
        if (!peGuard(gs, fnSpawn, pb.b)) { logf("FAIL %s: SpawnObject SEH", tag); return nullptr; }
        void* w = nullptr;
        memcpy(&w, pb.b + (int)fnSpawn->GetReturnValueOffset(), 8);
        return reinterpret_cast<UObject*>(w);
    };

    // 루트: 전화면 투명 Border -- Visible = 바깥 클릭 흡수, 패딩 = 앵커 아래 배치.
    // 위젯 레이아웃 단위 == 절대 픽셀은 실측 확정(패널 678px 토글 = 설정창 678px,
    // 히트 사각형 좌표 = 마우스 절대 좌표).
    UObject* root = spawn(bCls, "combo.root");
    if (!root) return false;
    {
        int off = propOffset(tree, L"RootWidget", 8, "combo.root");
        if (off < 0 || !writePtrGuard(tree, off, root)) return false;
    }
    setBrushColor(root, {0, 0, 0, 0.0f}, "combo.root");
    setVisibility(root, 0, "combo.root");
    // 배치(SetPadding)는 목록 높이가 정해진 뒤에 한다 -- 아래로 펼칠지 위로 펼칠지가
    // 높이에 달렸다(v0.18).

    // 폰트 차용 (메뉴 클론 TitleText -- 패널/팝업과 동일 규약)
    unsigned char font[96];
    bool haveFont = false;
    if (mc)
        if (UObject* mt = readObjProp(mc, L"TitleText", "combo.font"))
        {
            int foff = propOffset(mt, L"Font", 88, "combo.font");
            if (foff >= 0 && readBytesGuard(mt, foff, font, 88)) haveFont = true;
        }
    auto cfont = [&](UObject* tb, float scale) {
        if (!haveFont) return;
        unsigned char fb[88];
        memcpy(fb, font, 88);
        if (scale != 1.0f)
        {
            float s;
            memcpy(&s, fb + 0x48, 4);
            s *= scale;
            memcpy(fb + 0x48, &s, 4);
        }
        callBytes(tb, L"SetFont", fb, 88, "combo.font");
    };

    const float itemH = 82.0f;  // 실측 피치 88 - 행간 (하이라이트 79 근사)
    float dropW = (float)(ax1 - ax0);
    if (dropW < 200.f) dropW = 675.0f;

    // v0.18: 화면 밖으로 나가지 않게 **아래/위 중 넓은 쪽**에 펼친다.
    // 실측 사고: 마지막 모드의 콤보는 아래 공간이 없어 목록 절반이 화면 밖이었고
    // 스크롤조차 닿지 않았다(패널 자체가 화면 밖이라). 뷰포트 사각형은 패널 루트
    // 스크림(전체화면)의 기하로 잰다 -- 이 빌드는 위젯 크기 조회가 0 이라 그것만이
    // 같은 좌표계의 실측원이다.
    double vpTop = 0, vpBot = 0;
    {
        double sx0, sy0, sx1, sy1;
        if (g_scrimW && widgetRectAbs(reinterpret_cast<UObject*>(g_scrimW), &sx0, &sy0, &sx1, &sy1) &&
            sy1 - sy0 > 200.0)
        {
            vpTop = sy0;
            vpBot = sy1;
        }
        else
        {   // 폴백: 앵커 주변으로 넉넉히 -- 최소한 아래로 무한정 뻗지는 않게
            vpTop = ay0 - 900.0;
            vpBot = ay1 + 900.0;
            logf("WARN combo: 뷰포트 실측 실패 -- 앵커 기준 추정");
        }
    }
    const float MARGIN = 12.0f;
    double spaceBelow = vpBot - ay1 - MARGIN * 2;
    double spaceAbove = ay0 - vpTop - MARGIN * 2;
    bool below = spaceBelow >= spaceAbove;
    double space = below ? spaceBelow : spaceAbove;
    if (space < itemH * 2) space = itemH * 2;  // 최소 2행은 확보(그래도 스크롤은 된다)

    int fitN = (int)((space - 12.0) / itemH);
    if (fitN < 2) fitN = 2;
    int visN = n < fitN ? n : fitN;   // 들어가면 전부, 아니면 들어가는 만큼 + 스크롤
    float dropH = visN * itemH + 12.0f;

    // 확정된 높이로 위치 결정: 아래면 알약 바로 밑, 위면 알약 위로 dropH 만큼
    {
        double top = below ? (ay1 + MARGIN * 0.5) : (ay0 - MARGIN * 0.5 - dropH);
        if (top < vpTop + 4.0) top = vpTop + 4.0;
        float pad[4] = {(float)ax0, (float)top, 0, 0};
        callBytes(root, L"SetPadding", pad, 16, "combo.rootPad");
    }

    UObject* box = spawn(sCls, "combo.box");
    if (!box) return false;
    callBytes(box, L"SetWidthOverride", &dropW, 4, "combo.box");
    callBytes(box, L"SetHeightOverride", &dropH, 4, "combo.box");
    setVisibility(box, 4, "combo.box");
    slotAlign(addChildTo(root, box, "combo.box"), 1, 1, "combo.box");  // 좌상단 고정

    // 패널 = 외곽선 Border + 채움 Border (실측 색, 텍스처 스트레치 왜곡 없는 평면)
    UObject* outline = spawn(bCls, "combo.outline");
    UObject* fill = outline ? spawn(bCls, "combo.fill") : nullptr;
    if (!outline || !fill) return false;
    setBrushColor(outline, {1, 1, 1, 0.10f}, "combo.outline");
    setVisibility(outline, 0, "combo.outline");  // 드롭다운 내부 빈틈 클릭 흡수
    {
        float m[4] = {2, 2, 2, 2};
        callBytes(outline, L"SetPadding", m, 16, "combo.outline");
    }
    setBrushColor(fill, {0.021f, 0.028f, 0.023f, 0.985f}, "combo.fill");  // 실측 {39,45,41}
    setVisibility(fill, 4, "combo.fill");
    {
        float m[4] = {4, 4, 4, 4};
        callBytes(fill, L"SetPadding", m, 16, "combo.fill");
    }
    slotAlign(addChildTo(box, outline, "combo.outline"), 0, 0, "combo.outline");
    slotAlign(addChildTo(outline, fill, "combo.fill"), 0, 0, "combo.fill");

    // 항목 부착 대상 -- 7개 초과면 ScrollBox (휠 스크롤, v0.12 실증 경로)
    UObject* itemsVb = spawn(vCls, "combo.vb");
    if (!itemsVb) return false;
    setVisibility(itemsVb, 4, "combo.vb");
    UObject* itemsHost = fill;
    g_comboScrollBox = nullptr;
    if (n > visN && sbCls)
    {
        if (UObject* scroll = spawn(sbCls, "combo.scroll"))
        {
            setVisibility(scroll, 0, "combo.scroll");  // 휠 입력 수신 대상
            slotAlign(addChildTo(fill, scroll, "combo.scroll"), 0, 0, "combo.scroll");
            itemsHost = scroll;
            g_comboScrollBox = scroll;               // v0.18: 휠/드래그 스크롤 + 자동 숨김
            setScrollbarShown(scroll, false);
            g_sbShown[1] = false;
            g_sbLastOff[1] = -1.0f;
        }
    }
    slotAlign(addChildTo(itemsHost, itemsVb, "combo.vb"), 0, itemsHost == fill ? 0 : -1, "combo.vb");

    int built = 0;
    for (int i = 0; i < n; ++i)
    {
        UObject* cell = spawn(sCls, "combo.cell");
        UObject* hl = cell ? spawn(bCls, "combo.hl") : nullptr;
        UObject* hbI = hl ? spawn(hCls, "combo.hb") : nullptr;
        if (!cell || !hl || !hbI) break;
        float ih = itemH;
        callBytes(cell, L"SetHeightOverride", &ih, 4, "combo.cell");
        setVisibility(cell, 4, "combo.cell");
        setBrushColor(hl, {1, 1, 1, 0}, "combo.hl");  // 평상시 투명 -- 호버 때 카키
        setVisibility(hl, 0, "combo.hl");             // 히트 대상
        setVisibility(hbI, 4, "combo.hb");
        slotAlign(addChildTo(cell, hl, "combo.cell"), 0, 0, "combo.cell");
        slotAlign(addChildTo(hl, hbI, "combo.hl"), 0, 0, "combo.hl");
        UObject* tx = spawn(tCls, "combo.tx");
        if (tx)
        {
            setVisibility(tx, 4, "combo.tx");
            setTextOn(tx, labels[i], "combo.tx");
            setTextColor(tx, {0.372f, 0.44f, 0.428f, 1.0f}, "combo.tx");
            cfont(tx, 0.9f);
            UObject* ts = addChildTo(hbI, tx, "combo.tx");
            slotAlign(ts, -1, 2, "combo.tx");
            slotPad(ts, 30, 0, 0, 0, "combo.tx");
            slotFillWidth(ts, "combo.tx");
        }
        UObject* mk = spawn(tCls, "combo.mk");
        if (mk)
        {
            setVisibility(mk, 4, "combo.mk");
            setTextOn(mk, L"◆", "combo.mk");
            setTextColor(mk, {1, 1, 1, 0}, "combo.mk");  // 호버 항목에서만 알파 1
            cfont(mk, 0.7f);
            UObject* ms = addChildTo(hbI, mk, "combo.mk");
            slotAlign(ms, -1, 2, "combo.mk");
            slotPad(ms, 0, 0, 24, 0, "combo.mk");
        }
        UObject* cs = addChildTo(itemsVb, cell, "combo.cell");
        slotAlign(cs, 0, -1, "combo.cell");
        g_comboItemHs[built] = hl;
        g_comboItemTx[built] = tx;
        g_comboItemMk[built] = mk;
        ++built;
    }
    if (built < 2)
    {
        logf("WARN combo: 항목 구축 %d개 -- 중단 (고아 호스트는 GC 에 맡김)", built);
        return false;  // g_comboOpen=false 상태 유지라 배열 잔존 포인터는 읽히지 않는다
    }

    setVisibility(host, 0, "combo.host");
    int z = 1100;
    if (!callBytes(host, L"AddToViewport", &z, 4, "combo.viewport")) return false;
    g_comboHost = (void*)host;
    g_comboOpen = true;
    g_comboN = built;
    g_comboHover = -1;
    g_comboRow = row;
    g_comboOpt = opt;
    g_comboValTx = valTx;
    g_comboAnchorPill = (void*)anchor;
    setComboPillTex(g_comboAnchorPill, L"combo_pill_focus.png");  // 게임 포커스 외곽선
    if (mc) startPulseTimer(mc);  // 모달 구간 펄스 재무장 (팝업 규약)
    logf("combo: OPEN 앵커=(%.0f,%.0f)-(%.0f,%.0f) 항목 %d개 표시 %d 방향=%s "
         "뷰포트=%.0f~%.0f 스크롤=%s",
         ax0, ay0, ax1, ay1, built, visN, below ? "아래" : "위", vpTop, vpBot,
         g_comboScrollBox ? "예" : "아니오");
    return true;
}

// v0.28: 패널 람다 밖(색상창 등)에서 쓰는 폰트 -- 메뉴 클론의 TitleText 에서 차용해 캐시
static unsigned char g_borrowedFont[96];
static bool g_haveBorrowedFont = false;

static void applyBorrowedFont(UObject* tb, float scale)
{
    if (!g_haveBorrowedFont)
    {
        UObject* mc = reinterpret_cast<UObject*>(g_myClone.load(std::memory_order_relaxed));
        if (!mc) return;
        UObject* tt = readObjProp(mc, L"TitleText", "font.borrow");
        if (!tt) return;
        int foff = propOffset(tt, L"Font", 88, "font.borrow");
        if (foff < 0 || !readBytesGuard(tt, foff, g_borrowedFont, 88)) return;
        g_haveBorrowedFont = true;
    }
    unsigned char fb[88];
    memcpy(fb, g_borrowedFont, 88);
    if (scale != 1.0f)
    {
        float sz;
        memcpy(&sz, fb + 0x48, 4);
        sz *= scale;
        memcpy(fb + 0x48, &sz, 4);
    }
    callBytes(tb, L"SetFont", fb, 88, "font.borrow");
}

// ======================= v0.28: 색상 선택창 ================================
// 콤보 드롭다운과 같은 골격(별도 뷰포트 호스트 + 앵커 아래 배치 + 바깥 클릭 닫기).
// 안에는 ① 채도/명도 사각형 ② 색조 띠 ③ 프리셋 팔레트 ④ HEX 표시가 들어간다.

static void closeColor(const char* why)
{
    if (!g_colorOpen && !g_colorHost) return;
    if (g_comboAnchorPill)
    {
        setComboPillTex(g_comboAnchorPill, L"combo_pill.png");
        g_comboAnchorPill = nullptr;
    }
    g_colorOpen = false;
    g_colorRow = g_colorOpt = -1;
    g_colorSV = g_colorHue = g_colorSVBase = g_colorPreview = g_colorHexTx = nullptr;
    g_colorSwatchN = 0;
    g_hexLen = 0;
    g_hexBuf[0] = 0;
    void* p = g_colorHost;
    g_colorHost = nullptr;
    if (p)
    {
        UObject* w = reinterpret_cast<UObject*>(p);
        UFunction* fn = fnOf(w, L"RemoveFromParent", "closeColor");
        if (fn && (int)fn->GetParmsSize() == 0) { PB pb; peGuard(w, fn, pb.b); }
    }
    logf("color: 닫힘 (%s)", why);
}

static int currentColorRgb()
{
    int r, g, b;
    hsvToRgb(g_colorH, g_colorS, g_colorV, &r, &g, &b);
    return (r << 16) | (g << 8) | b;
}

// 현재 HSV 를 창(미리보기·HEX·사각형 바탕)과 옵션 값에 반영
static void paintColorPicker()
{
    int rgb = currentColorRgb();
    if (g_colorSVBase)
    {
        int r, g, b;
        hsvToRgb(g_colorH, 1.0f, 1.0f, &r, &g, &b);
        setBrushColor(reinterpret_cast<UObject*>(g_colorSVBase),
                      rgbToLin((r << 16) | (g << 8) | b), "color.base");
    }
    if (g_colorPreview)
        setBrushColor(reinterpret_cast<UObject*>(g_colorPreview), rgbToLin(rgb), "color.prev");
    if (g_colorHexTx)
    {
        wchar_t hx[24];
        if (g_hexLen > 0) swprintf(hx, 24, L"#%s_", g_hexBuf);
        else swprintf(hx, 24, L"#%06X", rgb);
        setTextOn(reinterpret_cast<UObject*>(g_colorHexTx), hx, "color.hex");
    }
}

static bool openColorPicker(UObject* anchor, int startRgb, int row, int opt)
{
    if (g_colorOpen) closeColor("재열림");
    if (!anchor) return false;
    double ax0, ay0, ax1, ay1;
    if (!widgetRectAbs(anchor, &ax0, &ay0, &ax1, &ay1)) return false;

    UObject* mc = reinterpret_cast<UObject*>(g_myClone.load(std::memory_order_relaxed));
    UObject* pc = UOG::FindFirstOf(L"PlayerController");
    UObject* lib = findObj(L"/Script/UMG.Default__WidgetBlueprintLibrary", "color");
    UObject* gs = findObj(L"/Script/Engine.Default__GameplayStatics", "color");
    UObject* hostCls = findObj(L"/Script/UMG.UserWidget", "color");
    UObject* bCls = findObj(L"/Script/UMG.Border", "color");
    UObject* tCls = findObj(L"/Script/UMG.TextBlock", "color");
    UObject* sCls = findObj(L"/Script/UMG.SizeBox", "color");
    UObject* vCls = findObj(L"/Script/UMG.VerticalBox", "color");
    UObject* hCls = findObj(L"/Script/UMG.HorizontalBox", "color");
    if (!lib || !gs || !hostCls || !bCls || !tCls || !sCls || !vCls || !hCls) return false;
    if (!pc && !mc) return false;
    UFunction* fnCreate = fnOf(lib, L"Create", "color.create");
    if (!fnCreate || !parmsExact(fnCreate, 32, "color.create", false)) return false;
    UObject* ctx = pc ? pc : mc;
    PB pbC;
    memcpy(pbC.b + 0, &ctx, 8);
    memcpy(pbC.b + 8, &hostCls, 8);
    memcpy(pbC.b + 16, &pc, 8);
    if (!peGuard(lib, fnCreate, pbC.b)) return false;
    void* raw = nullptr;
    memcpy(&raw, pbC.b + (int)fnCreate->GetReturnValueOffset(), 8);
    UObject* host = reinterpret_cast<UObject*>(raw);
    if (!host) return false;
    UObject* tree = readObjProp(host, L"WidgetTree", "color.tree");
    if (!tree) return false;
    UFunction* fnSpawn = fnOf(gs, L"SpawnObject", "color.spawn");
    if (!fnSpawn || !parmsExact(fnSpawn, 24, "color.spawn", false)) return false;
    auto spawn = [&](UObject* cls) -> UObject* {
        PB pb;
        memcpy(pb.b + 0, &cls, 8);
        memcpy(pb.b + 8, &tree, 8);
        if (!peGuard(gs, fnSpawn, pb.b)) return nullptr;
        void* w = nullptr;
        memcpy(&w, pb.b + (int)fnSpawn->GetReturnValueOffset(), 8);
        return reinterpret_cast<UObject*>(w);
    };
    UObject* krl = findObj(L"/Script/Engine.Default__KismetRenderingLibrary", "color.krl");
    UFunction* fnImp = krl ? fnOf(krl, L"ImportFileAsTexture2D", "color.tex") : nullptr;
    if (fnImp && !parmsExact(fnImp, 32, "color.tex", false)) fnImp = nullptr;
    auto tex = [&](const wchar_t* file) -> UObject* {
        if (!fnImp) return nullptr;
        static wchar_t tp[MAX_PATH * 2];
        assetPath(tp, file);
        PB pb;
        memcpy(pb.b + 0, &ctx, 8);
        FStringRaw fs{tp, (int)wcslen(tp) + 1, (int)wcslen(tp) + 1};
        memcpy(pb.b + 8, &fs, 16);
        if (!peGuard(krl, fnImp, pb.b)) return nullptr;
        void* t = nullptr;
        memcpy(&t, pb.b + (int)fnImp->GetReturnValueOffset(), 8);
        return reinterpret_cast<UObject*>(t);
    };

    UObject* root = spawn(bCls);
    if (!root) return false;
    {
        int off = propOffset(tree, L"RootWidget", 8, "color.root");
        if (off < 0 || !writePtrGuard(tree, off, root)) return false;
    }
    setBrushColor(root, {0, 0, 0, 0.0f}, "color.root");
    setVisibility(root, 0, "color.root");

    const float PW = 520.0f, PH = 500.0f;
    double vpTop = ay0 - 900, vpBot = ay1 + 900;
    {
        double sx0, sy0, sx1, sy1;
        if (g_scrimW && widgetRectAbs(reinterpret_cast<UObject*>(g_scrimW), &sx0, &sy0, &sx1, &sy1) &&
            sy1 - sy0 > 200.0) { vpTop = sy0; vpBot = sy1; }
    }
    double top = (ay1 + PH + 12 <= vpBot) ? (ay1 + 6) : (ay0 - 6 - PH);
    if (top < vpTop + 4) top = vpTop + 4;
    {
        float pad[4] = {(float)ax0, (float)top, 0, 0};
        callBytes(root, L"SetPadding", pad, 16, "color.rootPad");
    }

    UObject* box = spawn(sCls);
    if (!box) return false;
    float bw = PW, bh = PH;
    callBytes(box, L"SetWidthOverride", &bw, 4, "color.box");
    callBytes(box, L"SetHeightOverride", &bh, 4, "color.box");
    setVisibility(box, 4, "color.box");
    slotAlign(addChildTo(root, box, "color.box"), 1, 1, "color.box");
    UObject* outline = spawn(bCls);
    UObject* fill = outline ? spawn(bCls) : nullptr;
    UObject* vb = fill ? spawn(vCls) : nullptr;
    if (!outline || !fill || !vb) return false;
    setBrushColor(outline, {1, 1, 1, 0.10f}, "color.outline");
    setVisibility(outline, 0, "color.outline");
    { float m[4] = {2, 2, 2, 2}; callBytes(outline, L"SetPadding", m, 16, "color.outline"); }
    setBrushColor(fill, {0.021f, 0.028f, 0.023f, 0.985f}, "color.fill");
    setVisibility(fill, 4, "color.fill");
    { float m[4] = {16, 16, 16, 16}; callBytes(fill, L"SetPadding", m, 16, "color.fill"); }
    slotAlign(addChildTo(box, outline, "color.outline"), 0, 0, "color.outline");
    slotAlign(addChildTo(outline, fill, "color.fill"), 0, 0, "color.fill");
    setVisibility(vb, 4, "color.vb");
    slotAlign(addChildTo(fill, vb, "color.vb"), 0, -1, "color.vb");

    // ① 채도/명도 사각형 (바탕=순색 틴트, 흰색/검정 그라데이션 두 겹)
    if (UObject* svBox = spawn(sCls))
    {
        float w = PW - 32, h = 220.0f;
        callBytes(svBox, L"SetWidthOverride", &w, 4, "color.sv");
        callBytes(svBox, L"SetHeightOverride", &h, 4, "color.sv");
        setVisibility(svBox, 4, "color.sv");
        UObject* base = spawn(bCls);
        if (base)
        {
            setVisibility(base, 0, "color.svBase");   // 히트 대상
            slotAlign(addChildTo(svBox, base, "color.sv"), 0, 0, "color.sv");
            g_colorSVBase = base;
            g_colorSV = base;
            if (UObject* sat = spawn(bCls))
            {
                if (UObject* t = tex(L"color_sat.png"))
                    callBytes(sat, L"SetBrushFromTexture", &t, 8, "color.sat");
                setBrushColor(sat, {1, 1, 1, 1}, "color.sat");
                setVisibility(sat, 4, "color.sat");
                slotAlign(addChildTo(base, sat, "color.sat"), 0, 0, "color.sat");
                if (UObject* val = spawn(bCls))
                {
                    if (UObject* t = tex(L"color_val.png"))
                        callBytes(val, L"SetBrushFromTexture", &t, 8, "color.val");
                    setBrushColor(val, {1, 1, 1, 1}, "color.val");
                    setVisibility(val, 4, "color.val");
                    slotAlign(addChildTo(sat, val, "color.val"), 0, 0, "color.val");
                }
            }
        }
        UObject* s = addChildTo(vb, svBox, "color.sv");
        slotAlign(s, 0, -1, "color.sv");
    }
    // ② 색조 띠
    if (UObject* hueBox = spawn(sCls))
    {
        float w = PW - 32, h = 34.0f;
        callBytes(hueBox, L"SetWidthOverride", &w, 4, "color.hue");
        callBytes(hueBox, L"SetHeightOverride", &h, 4, "color.hue");
        setVisibility(hueBox, 4, "color.hue");
        if (UObject* hb2 = spawn(bCls))
        {
            if (UObject* t = tex(L"color_hue.png"))
                callBytes(hb2, L"SetBrushFromTexture", &t, 8, "color.hueTex");
            setBrushColor(hb2, {1, 1, 1, 1}, "color.hueTex");
            setVisibility(hb2, 0, "color.hueTex");   // 히트 대상
            slotAlign(addChildTo(hueBox, hb2, "color.hue"), 0, 0, "color.hue");
            g_colorHue = hb2;
        }
        UObject* s = addChildTo(vb, hueBox, "color.hue");
        slotAlign(s, 0, -1, "color.hue");
        slotPad(s, 0, 10, 0, 0, "color.hue");
    }
    // ③ 프리셋 팔레트 (자주 쓰는 12색)
    static const int PRESET[12] = {
        0xFFFFFF, 0xC0C0C0, 0x808080, 0x000000, 0xFF3B30, 0xFF9500,
        0xFFD60A, 0x34C759, 0x00C7BE, 0x0A84FF, 0x5E5CE6, 0xFF2D55};
    g_colorSwatchN = 0;
    if (UObject* row = spawn(hCls))
    {
        setVisibility(row, 4, "color.pal");
        for (int i = 0; i < 12; ++i)
        {
            UObject* cell = spawn(sCls);
            UObject* sw = cell ? spawn(bCls) : nullptr;
            if (!cell || !sw) break;
            float w = 36.0f, h = 36.0f;
            callBytes(cell, L"SetWidthOverride", &w, 4, "color.pal");
            callBytes(cell, L"SetHeightOverride", &h, 4, "color.pal");
            setVisibility(cell, 4, "color.pal");
            setBrushColor(sw, rgbToLin(PRESET[i]), "color.pal");
            setVisibility(sw, 0, "color.pal");
            slotAlign(addChildTo(cell, sw, "color.pal"), 0, 0, "color.pal");
            UObject* cs = addChildTo(row, cell, "color.pal");
            slotAlign(cs, -1, 2, "color.pal");
            slotPad(cs, i ? 4.0f : 0.0f, 0, 0, 0, "color.pal");
            g_colorSwatchHs[g_colorSwatchN] = sw;
            g_colorSwatchRgb[g_colorSwatchN] = PRESET[i];
            ++g_colorSwatchN;
        }
        UObject* s = addChildTo(vb, row, "color.pal");
        slotAlign(s, 1, -1, "color.pal");
        slotPad(s, 0, 12, 0, 0, "color.pal");
    }
    // ④ 미리보기 + HEX
    if (UObject* row = spawn(hCls))
    {
        setVisibility(row, 4, "color.foot");
        if (UObject* pvBox = spawn(sCls))
        {
            float w = 56.0f, h = 40.0f;
            callBytes(pvBox, L"SetWidthOverride", &w, 4, "color.prev");
            callBytes(pvBox, L"SetHeightOverride", &h, 4, "color.prev");
            setVisibility(pvBox, 4, "color.prev");
            if (UObject* pv = spawn(bCls))
            {
                setVisibility(pv, 4, "color.prev");
                slotAlign(addChildTo(pvBox, pv, "color.prev"), 0, 0, "color.prev");
                g_colorPreview = pv;
            }
            slotAlign(addChildTo(row, pvBox, "color.prev"), -1, 2, "color.prev");
        }
        if (UObject* hx = spawn(tCls))
        {
            setVisibility(hx, 4, "color.hex");
            setTextColor(hx, {0.97f, 0.97f, 0.98f, 1.0f}, "color.hex");
            applyBorrowedFont(hx, 0.95f);
            UObject* s2 = addChildTo(row, hx, "color.hex");
            slotAlign(s2, -1, 2, "color.hex");
            slotPad(s2, 16, 0, 0, 0, "color.hex");
            // ⚠ Fill 을 주지 않는다. 한 줄에 안내 문구까지 넣었더니 자동 슬롯이 폭을
            //   다 먹어 Fill 슬롯이 0 으로 눌렸고, HEX 글자가 옆 문구 위로 흘러넘쳤다
            //   (UMG TextBlock 은 기본적으로 슬롯을 넘어가도 잘리지 않는다).
            g_colorHexTx = hx;
        }
        UObject* s = addChildTo(vb, row, "color.foot");
        slotAlign(s, 0, -1, "color.foot");
        slotPad(s, 0, 12, 0, 0, "color.foot");
    }
    // 안내는 **아랫줄**로 분리 -- 같은 줄에 두면 폭 경쟁이 난다
    if (UObject* tip = spawn(tCls))
    {
        setVisibility(tip, 4, "color.tip");
        setTextOn(tip, TR(L"0-9 A-F 로 직접 입력  ·  Backspace 지우기  ·  ESC 닫기",
                          L"Type 0-9 A-F directly  ·  Backspace to erase  ·  ESC to close"), "color.tip");
        setTextColor(tip, {0.52f, 0.57f, 0.62f, 1.0f}, "color.tip");
        applyBorrowedFont(tip, 0.7f);
        UObject* s = addChildTo(vb, tip, "color.tip");
        slotAlign(s, 1, -1, "color.tip");
        slotPad(s, 2, 8, 0, 0, "color.tip");
    }

    setVisibility(host, 0, "color.host");
    int z = 1100;
    if (!callBytes(host, L"AddToViewport", &z, 4, "color.viewport")) return false;
    g_colorHost = (void*)host;
    g_colorOpen = true;
    g_colorRow = row;
    g_colorOpt = opt;
    g_hexLen = 0;
    g_hexBuf[0] = 0;
    // 시작 색을 HSV 로 (역변환)
    {
        float r = ((startRgb >> 16) & 0xFF) / 255.0f, g2 = ((startRgb >> 8) & 0xFF) / 255.0f,
              b = (startRgb & 0xFF) / 255.0f;
        float mx = r > g2 ? (r > b ? r : b) : (g2 > b ? g2 : b);
        float mn = r < g2 ? (r < b ? r : b) : (g2 < b ? g2 : b);
        float d = mx - mn;
        g_colorV = mx;
        g_colorS = mx > 0 ? d / mx : 0;
        if (d <= 0.0001f) g_colorH = 0;
        else if (mx == r) g_colorH = fmodf((g2 - b) / d / 6.0f + 1.0f, 1.0f);
        else if (mx == g2) g_colorH = ((b - r) / d + 2.0f) / 6.0f;
        else g_colorH = ((r - g2) / d + 4.0f) / 6.0f;
    }
    paintColorPicker();
    g_comboAnchorPill = (void*)anchor;
    setComboPillTex(g_comboAnchorPill, L"combo_pill_focus.png");
    if (mc) startPulseTimer(mc);
    logf("color: OPEN 시작색=#%06X", startRgb & 0xFFFFFF);
    return true;
}

// ======================= v0.14: 모드발 재시작 안내 신호 =====================
// 판단은 모드가, 표시는 매니저가: 모드가 자기 폴더에 dsnotify.txt 를 쓰면
// (내용 = "restart" 또는 사용자 정의 메시지) 매니저가 1초 주기로 감지해
// 재시작 팝업을 띄우고 파일을 지운다(1회성 ack). 팝업이 떠 있는 동안 온
// 신호는 파일이 남아 다음 스캔에서 처리된다.
static ULONGLONG g_lastNotifyScanMs = 0;

static void checkModNotifications(ULONGLONG now)
{
    if (g_popupOpen) return;
    if (now - g_lastNotifyScanMs < 1000) return;
    g_lastNotifyScanMs = now;
    // v0.20: 발견은 공용 수집기(깊이 탐색)를 그대로 쓰되, 목록은 5초 캐시다
    // (매초 재귀 탐색 = 게임 스레드에서 디스크 긁기). 파일 확인은 매초 한다.
    PlgEnt* ents = g_scanCache;
    int entN = collectPluginsCached(now);
    for (int k = 0; k < entN; ++k)
    {
        const wchar_t* relPath = ents[k].rel[0] ? ents[k].rel : ents[k].name;
        // 실행 사본(Mods) 우선, plugins 쪽도 허용
        wchar_t nf[MAX_PATH * 2];
        gameModsRoot(nf);
        lstrcatW(nf, ents[k].name);
        lstrcatW(nf, L"\\dsnotify.txt");
        if (!pathExistsW(nf))
        {
            pluginSrcPath(nf, relPath);
            lstrcatW(nf, L"\\dsnotify.txt");
            if (!pathExistsW(nf)) continue;
        }
        std::string msgA = readFileA(nf);
        // 양쪽 사본 모두 청소 (ack)
        wchar_t nf2[MAX_PATH * 2];
        gameModsRoot(nf2);
        lstrcatW(nf2, ents[k].name);
        lstrcatW(nf2, L"\\dsnotify.txt");
        DeleteFileW(nf2);
        pluginSrcPath(nf2, relPath);
        lstrcatW(nf2, L"\\dsnotify.txt");
        DeleteFileW(nf2);

        if (msgA.size() >= 3 && msgA.compare(0, 3, "\xEF\xBB\xBF") == 0) msgA.erase(0, 3);
        while (!msgA.empty() && (msgA.back() == '\n' || msgA.back() == '\r' || msgA.back() == ' '))
            msgA.pop_back();
        // 표시명(dsplugin.ini name=) -- 기본 문구와 발신자 표기 양쪽에 쓴다
        wchar_t disp[64];
        disp[0] = 0;
        {
            wchar_t ini[MAX_PATH * 2];
            pluginSrcPath(ini, relPath);
            lstrcatW(ini, L"\\dsplugin.ini");
            std::string d = readFileA(ini);
            if (!d.empty())
            {
                if (d.size() >= 3 && d.compare(0, 3, "\xEF\xBB\xBF") == 0) d.erase(0, 3);
                std::string nm = iniValueLang(d, "plugin", "name");
                if (!nm.empty()) utf8ToW(nm, disp, 64);
            }
        }
        static wchar_t msgW[512];
        if (msgA.empty() || _stricmp(msgA.c_str(), "restart") == 0)
        {
            swprintf(msgW, 512, TR(L"『%s』 변경 사항은 게임 재시작 후 적용됩니다.",
                               L"Changes to '%s' take effect after the game restarts."),
                     disp[0] ? disp : ents[k].name);
        }
        else
        {
            // v0.40: 발신자를 밝힌다 -- 발신자 없는 문구는 매니저/게임의 말처럼
            // 읽힌다 (실측 2026-08-10: AutoFood 의 "게임이 업데이트되어..." 를
            // 매니저/게임 탓으로 오인). 모드가 보낸 문구 앞에 이름을 붙인다.
            // ★ v0.50 실측 2026-08-17: 상한이 **200바이트**여서 한글(3바이트/자)은
            // 66자에서 잘렸다 -- AutoFood 안내가 "...모드 업데이트" 에서 끊겨 표시됨.
            // 팝업은 자동 줄바꿈·높이 조절이 되므로(v0.25) 넉넉히 준다.
            // ⚠ 바이트로 자르면 다중바이트 문자 중간이 잘려 깨진다 -- **문자 경계**에서 자른다.
            const size_t LIMIT = 600;
            if (msgA.size() > LIMIT)
            {
                size_t cut = LIMIT;
                while (cut > 0 && ((unsigned char)msgA[cut] & 0xC0) == 0x80) --cut;  // 이어바이트면 뒤로
                msgA.resize(cut);
                msgA += "...";
            }
            wchar_t body[320];
            utf8ToW(msgA, body, 320);
            swprintf(msgW, 512, TR(L"『%s』 모드의 안내:\n%s", L"From mod '%s':\n%s"),
                     disp[0] ? disp : ents[k].name, body);
        }
        logf("notify: '%s' 재시작 안내 신호 수신", u8(ents[k].name).c_str());
        showRestartPopup(msgW);
        return;  // 한 번에 하나 -- 다음 신호는 다음 스캔에서
    }
}

// ---------------- v0.40: 재구축 = "새로 그린 뒤 옛것 제거" ----------------
// closePanel -> openPanel 순서는 옛 패널이 사라진 뒤 새 패널이 완성될 때까지
// (openPanel 이 무겁다) 화면이 1초쯤 비었다(실측 보고). 옛 패널을 산 채로 두고
// 새 패널을 위에 얹은 다음 걷어내면 공백이 없다.
static void rebuildPanel(UObject* clone, const char* why, bool keepScroll = true)
{
    if (keepScroll) captureScroll();
    UObject* oldHost = reinterpret_cast<UObject*>(g_panel);
    g_panel = nullptr;               // closePanel 이 옛 패널을 지우지 않게
    closePanel(why);                 // 상태 청소 (콤보/색상/팝업 연쇄 닫기 포함)
    openPanel(clone);                // 새 패널이 옛 패널 위에 얹힌다
    if (oldHost)
    {
        UFunction* fn = fnOf(oldHost, L"RemoveFromParent", "rebuild.old");
        if (fn && (int)fn->GetParmsSize() == 0)
        {
            PB pb;
            if (!peGuard(oldHost, fn, pb.b)) logf("WARN rebuild: 옛 패널 제거 SEH");
        }
    }
}

// ---------------- v0.40(pad): 패널 내비게이션 ----------------
static void navPaintSel(int oldSel, int newSel)
{
    if (oldSel >= 0 && oldSel < g_navN && g_nav[oldSel].outline)
        setBrushColor(reinterpret_cast<UObject*>(g_nav[oldSel].outline),
                      {1, 1, 1, 0}, "nav-sel");
    // 켜는 쪽은 패드 모드에서만 -- 키/마 사용 중엔 패드 선택 표시를 숨긴다 (설정창 실측)
    if (newSel >= 0 && newSel < g_navN && g_nav[newSel].outline &&
        g_inputMode.load(std::memory_order_relaxed) == 1)
        setBrushColor(reinterpret_cast<UObject*>(g_nav[newSel].outline),
                      {1, 1, 1, 1}, "nav-sel");
}

/* ★ v0.50 실측 2026-08-17 (라이브): 패널을 새로 연 직후 패드로 쭉 내리면 화면 밖
   행부터 **스크롤이 안 따라가** 선택이 보이지 않는 곳으로 사라졌다. 마우스로 한 번
   굴린 뒤에는 정상 동작.
   원인: `widgetRectAbs` 는 `GetCachedGeometry` 를 쓰는데, Slate 는 **그려진 적 없는
   위젯의 캐시 크기를 0 으로 준다**(뷰포트 밖은 컬링돼 페인트를 안 탄다). 그래서
   `sx<=0` 로 **false** 가 나고 이 함수가 통째로 조기 반환했다 = 스크롤 없음.
   마우스로 굴리면 그 행들이 그려져 캐시가 생기고, 그 뒤로는 잘 됐던 것.
   해법: 목표 행의 좌표를 못 얻으면 **이동 방향으로 한 칸 밀어** 배치를 유도한다.
   한 번 그려지고 나면 아래 정밀 경로가 이어받는다. 마지막 행처럼 뒤이은 이동이
   없는 경우를 위해 호출자가 잠깐 재확인(settle)한다. */
static ULONGLONG g_navSettleUntil = 0;   // 이 시각까지 정밀 재확인 (패드 모드에서만)

static void navEnsureVisible(int sel, int dir = 0)
{
    if (g_pendScroll >= 0.0f) return;   // v0.40: 복원이 진행 중이면 양보
    if (sel < 0 || sel >= g_navN || !g_scrollBox) return;
    float cur = readScrollOffset(g_scrollBox);
    if (cur < 0) cur = 0;
    double rx0, ry0, rx1, ry1, sx0, sy0, sx1, sy1;
    bool haveRow = g_nav[sel].rowBox &&
                   widgetRectAbs(reinterpret_cast<UObject*>(g_nav[sel].rowBox), &rx0, &ry0, &rx1, &ry1);
    if (!haveRow)
    {
        // 아직 한 번도 그려지지 않은 행 -- 위치를 알 수 없다. 방향으로 한 칸 민다.
        if (!dir) return;
        float want = cur + (dir > 0 ? 96.0f : -96.0f);
        if (want < 0) want = 0;
        if (want != cur) writeScrollOffset(g_scrollBox, want);
        return;
    }
    if (!widgetRectAbs(reinterpret_cast<UObject*>(g_scrollBox), &sx0, &sy0, &sx1, &sy1)) return;
    float want = cur;
    if (ry0 < sy0) want = cur - (float)(sy0 - ry0) - 20.0f;
    else if (ry1 > sy1) want = cur + (float)(ry1 - sy1) + 20.0f;
    if (want < 0) want = 0;
    if (want != cur) writeScrollOffset(g_scrollBox, want);
}

static void navAfterRebuild()
{
    if (g_navSel >= g_navN) g_navSel = g_navN - 1;
    navPaintSel(-1, g_navSel);
    // v0.40: 스크롤 따라가기는 패드 모드에서만 -- 마우스 재구축은 복원(g_pendScroll)
    // 이 자리를 지키는데, 여기서 또 스크롤하면 복원과 싸운다
    if (g_inputMode.load(std::memory_order_relaxed) == 1) navEnsureVisible(g_navSel);
}

// v0.40(pad): 드롭다운에서 패드 A 로 고른 항목 적용. true = 패널 재구축(틱 종료).
static bool comboApplyIdx(int idx, UObject* clone)
{
    bool reopenAfter = false;
    if (g_comboRow < 0)   // 언어 콤보 (매니저 자체)
    {
        int want = (idx == 1) ? 1 : 0;
        if (want != g_lang)
        {
            applyLang(want);
            saveLang();
            logf("lang: 사용자 선택(패드) -> %s", g_lang ? "en" : "ko");
            setLabel(clone, "lang");
            reopenAfter = true;
        }
    }
    else if (g_comboRow >= 0 && g_comboRow < g_plgN)
    {
        PlgRow& r = g_plg[g_comboRow];
        if (g_comboOpt >= 0 && g_comboOpt < r.optN)
        {
            PlgOpt& o = r.opt[g_comboOpt];
            if (idx >= 0 && idx < o.choiceN && o.val != idx)
            {
                o.val = idx;
                if (g_comboValTx)
                    setTextOn(reinterpret_cast<UObject*>(g_comboValTx), o.choices[idx], "pad-combo");
                saveOptionValues(r);
                if (optHasChildren(r, g_comboOpt)) reopenAfter = true;
                logf("combo(패드): '%s' %s = %d", u8(r.name).c_str(), o.key, idx);
            }
        }
    }
    closeCombo("선택(패드)");
    if (reopenAfter)
    {
        rebuildPanel(clone, "옵션 갱신(콤보)");
        navAfterRebuild();
        return true;
    }
    return false;
}

// 패널 열림 중 패드 입력 소비. true = 패널을 재구축했다(이번 틱 종료).
// 규약: 마우스와 별개 경로다 -- 무장(눌렀다 떼기)을 흉내내지 않고 즉시 실행한다.
static bool padPanelInput(unsigned pe, UObject* clone)
{
    if (pe & (PAD_LB | PAD_RB))   // 탭 전환 (게임 설정창과 동일)
    {
        int want = (pe & PAD_LB) ? 0 : 1;
        if (want != g_activeTab)
        {
            if (g_padOrdLift >= 0)   // 집어든 이동은 저장하고 탭을 바꾼다
            {
                g_padOrdLift = -1;
                saveOrderFile();
                logf("pad: 순서 저장(탭 전환)");
            }
            g_activeTab = want;
            g_padOrdLift = -1;
            logf("pad: 탭 전환 -> %s", want == 0 ? "모드" : "순서");
            rebuildPanel(clone, "패드 탭 전환", false);
            navAfterRebuild();
            return true;
        }
    }
    if (g_navN <= 0) return false;
    if (pe & (PAD_UP | PAD_DOWN))
    {
        int dir = (pe & PAD_DOWN) ? 1 : -1;
        // 순서 탭에서 집어든 상태: 위/아래 = 이웃 행과 자리 교환
        if (g_activeTab == 1 && g_padOrdLift >= 0 && g_padOrdLift < g_ordN)
        {
            int a = g_padOrdLift, b2 = a + dir;
            if (b2 >= 0 && b2 < g_ordN)
            {
                wchar_t tn[64], tl[64];
                lstrcpynW(tn, g_ord[a].name, 64);
                lstrcpynW(tl, g_ord[a].label, 64);
                lstrcpynW(g_ord[a].name, g_ord[b2].name, 64);
                lstrcpynW(g_ord[a].label, g_ord[b2].label, 64);
                lstrcpynW(g_ord[b2].name, tn, 64);
                lstrcpynW(g_ord[b2].label, tl, 64);
                if (g_ord[a].text)
                    setTextOn(reinterpret_cast<UObject*>(g_ord[a].text), g_ord[a].label, "pad-ord");
                if (g_ord[b2].text)
                    setTextOn(reinterpret_cast<UObject*>(g_ord[b2].text), g_ord[b2].label, "pad-ord");
                if (g_ord[a].band)
                    setBrushColor(reinterpret_cast<UObject*>(g_ord[a].band), {1, 1, 1, 0.07f}, "pad-ord");
                if (g_ord[b2].band)
                    setBrushColor(reinterpret_cast<UObject*>(g_ord[b2].band), {1, 1, 1, 0.20f}, "pad-ord");
                g_padOrdLift = b2;
                // 선택 테두리도 그 행으로
                int oldSel = g_navSel;
                for (int i = 0; i < g_navN; ++i)
                    if (g_nav[i].kind == NAVK_ORD && g_nav[i].row == b2) { g_navSel = i; break; }
                navPaintSel(oldSel, g_navSel);
                navEnsureVisible(g_navSel);
            }
            return false;
        }
        int oldSel = g_navSel;
        int ns = (g_navSel < 0) ? (dir > 0 ? 0 : g_navN - 1) : g_navSel + dir;
        if (ns < 0) ns = 0;
        if (ns >= g_navN) ns = g_navN - 1;
        g_navSel = ns;
        navPaintSel(oldSel, ns);
        navEnsureVisible(ns, dir);
        // 방금 민 행이 이번 프레임에 그려지면 다음 틱에 정확히 맞춘다 (마지막 행 대비)
        g_navSettleUntil = GetTickCount64() + 600;
        return false;
    }
    if (g_navSel < 0 || g_navSel >= g_navN) return false;
    NavItem& nv = g_nav[g_navSel];
    int dir = (pe & PAD_RIGHT) ? 1 : ((pe & PAD_LEFT) ? -1 : 0);
    bool act = (pe & PAD_A) != 0;
    if (!dir && !act) return false;

    if (nv.kind == NAVK_FOLDER)
    {
        if (act) openPluginsFolder();
        return false;
    }
    if (nv.kind == NAVK_LANG)
    {
        if (act)   // A = 드롭다운 열기 (게임 설정창과 동일)
        {
            if (openCombo(reinterpret_cast<UObject*>(g_langHs), g_langChoices, 2,
                          (g_lang == 1) ? 1 : 0, -1, -1, g_langTx))
            {
                g_comboPadSel = (g_lang == 1) ? 1 : 0;
                paintComboItem(g_comboPadSel, true);
            }
            return false;
        }
        int want = (dir > 0 ? 1 : 0);
        if (want != g_lang)
        {
            applyLang(want);
            saveLang();
            logf("pad: 언어 -> %s", g_lang ? "en" : "ko");
            setLabel(clone, "lang");
            rebuildPanel(clone, "언어 변경(패드)");
            navAfterRebuild();
            return true;
        }
        return false;
    }
    if (nv.kind == NAVK_FOLD && nv.row >= 0 && nv.row < g_plgN)
    {
        if (act)
        {
            PlgRow& r = g_plg[nv.row];
            setExpanded(r.name, !isExpanded(r.name));
            rebuildPanel(clone, "펼치기(패드)");
            navAfterRebuild();
            return true;
        }
        return false;
    }
    if (nv.kind == NAVK_MOD && nv.row >= 0 && nv.row < g_plgN)
    {
        PlgRow& r = g_plg[nv.row];
        bool want = act ? !r.on : (dir > 0);
        if (want != r.on)
        {
            r.on = want;
            paintToggle(r);
            applyPluginState(r);
            bool pop = r.on && !sessionLoaded(r.name);
            // v0.50: 미로드 모드는 옵션이 없어도 재구축 -- '재시작 필요' 배지 갱신
            if (r.optN || !sessionLoaded(r.name))
            {
                rebuildPanel(clone, "모드 토글(패드)");
                navAfterRebuild();
                if (pop) showRestartPopup(nullptr);
                return true;
            }
            if (pop) showRestartPopup(nullptr);
        }
        return false;
    }
    if (nv.kind == NAVK_ORD && nv.row >= 0 && nv.row < g_ordN)
    {
        if (act)   // A = 집기 / 놓기(저장)
        {
            if (g_padOrdLift == nv.row)
            {
                g_padOrdLift = -1;
                if (g_ord[nv.row].band)
                    setBrushColor(reinterpret_cast<UObject*>(g_ord[nv.row].band), {1, 1, 1, 0.07f}, "pad-ord");
                saveOrderFile();
                logf("pad: 순서 저장");
            }
            else
            {
                g_padOrdLift = nv.row;
                if (g_ord[nv.row].band)
                    setBrushColor(reinterpret_cast<UObject*>(g_ord[nv.row].band), {1, 1, 1, 0.20f}, "pad-ord");
                logf("pad: '%s' 집음", u8(g_ord[nv.row].name).c_str());
            }
        }
        return false;
    }
    if (nv.kind != NAVK_OPT || nv.row < 0 || nv.row >= g_plgN) return false;
    PlgRow& r = g_plg[nv.row];
    if (nv.opt < 0 || nv.opt >= r.optN) return false;
    PlgOpt& o = r.opt[nv.opt];
    if (o.type == 2)          // 키 지정: A = 캡처 시작
    {
        if (act)
        {
            g_keyCapture.store(true, std::memory_order_relaxed);
            g_capturedVk.store(0, std::memory_order_relaxed);
            g_keyCapRow = nv.row;
            g_keyCapOpt = nv.opt;
            if (o.comboTx)
                setTextOn(reinterpret_cast<UObject*>(o.comboTx),
                          TR(L"키를 누르세요  (Delete 해제 · ESC 취소)",
                             L"Press a key  (Delete = clear · ESC = cancel)"), "pad.keycap");
        }
        return false;
    }
    if (o.type == 3)          // 색상: A = 선택창 열기
    {
        if (act) openColorPicker(reinterpret_cast<UObject*>(o.comboHs), o.val & 0xFFFFFF, nv.row, nv.opt);
        return false;
    }
    if (o.type == 5)          // 실행 버튼
    {
        if (act)
        {
            o.val = (o.val < 0x7FFFFFF) ? o.val + 1 : 1;
            saveOptionValues(r);
            logf("pad: button '%s' %s -> %d", u8(r.name).c_str(), o.key, o.val);
        }
        return false;
    }
    if (o.choiceN >= 2)       // 콤보: A = 드롭다운 열기, 좌/우 = 열지 않고 순환
    {
        if (act)
        {
            int cur = (o.val >= 0 && o.val < o.choiceN) ? o.val : 0;
            if (openCombo(reinterpret_cast<UObject*>(o.comboHs), o.choices, o.choiceN,
                          cur, nv.row, nv.opt, o.comboTx))
            {
                g_comboPadSel = cur;
                paintComboItem(cur, true);
            }
            return false;
        }
        int n = o.choiceN;
        int nv2 = (o.val + dir + n) % n;
        if (nv2 != o.val)
        {
            o.val = nv2;
            if (o.comboTx)
                setTextOn(reinterpret_cast<UObject*>(o.comboTx), o.choices[nv2], "pad-combo");
            saveOptionValues(r);
            if (optHasChildren(r, nv.opt))
            {
                rebuildPanel(clone, "옵션 갱신(패드)");
                navAfterRebuild();
                return true;
            }
        }
        return false;
    }
    if (o.type == 0 || o.type == 4)   // 토글 / 체크박스
    {
        int want = act ? (o.val ? 0 : 1) : (dir > 0 ? 1 : 0);
        if (want != o.val)
        {
            o.val = want;
            saveOptionValues(r);
            if (o.type == 4 || optHasChildren(r, nv.opt))
            {   // 체크 그림은 텍스처 교체 = 재구축 (v0.29 규약)
                rebuildPanel(clone, "옵션 갱신(패드)");
                navAfterRebuild();
                return true;
            }
            paintOpt(o);
        }
        return false;
    }
    if (o.type == 6)          // 슬라이더: 좌/우 = 범위의 1/20 씩 (최소 1)
    {
        if (!dir) return false;
        int stepv = (o.maxV - o.minV) / 20;
        if (stepv < 1) stepv = 1;
        int nv2 = o.val + dir * stepv;
        if (nv2 < o.minV) nv2 = o.minV;
        if (nv2 > o.maxV) nv2 = o.maxV;
        if (nv2 != o.val)
        {
            o.val = nv2;
            paintSlider(o.sliderFill, o.sliderRest, o.valText, nv2, o.minV, o.maxV);
            saveOptionValues(r);
            if (optHasChildren(r, nv.opt))
            {
                rebuildPanel(clone, "옵션 갱신(패드)");
                navAfterRebuild();
                return true;
            }
        }
        return false;
    }
    if (o.type == 1 && dir)   // 스테퍼
    {
        int nv2 = o.val + dir * o.step;
        if (nv2 < o.minV) nv2 = o.minV;
        if (nv2 > o.maxV) nv2 = o.maxV;
        if (nv2 != o.val)
        {
            o.val = nv2;
            if (o.valText)
            {
                wchar_t vbuf[16];
                swprintf(vbuf, 16, L"%d", o.val);
                setTextOn(reinterpret_cast<UObject*>(o.valText), vbuf, "pad-step");
            }
            saveOptionValues(r);
            if (optHasChildren(r, nv.opt))
            {
                rebuildPanel(clone, "옵션 갱신(패드)");
                navAfterRebuild();
                return true;
            }
        }
    }
    return false;
}

// ======================= v0.2: 33ms 게임스레드 펌프 ========================

// PE 콜백에 편승해 33ms 간격으로 실행 (게임 스레드 판정 = 삽입 순간 캡처한
// 스레드 ID 비교, 네이티브라 예외 불가능). 역할: 메뉴 항목 호버 토글, 클릭
// -> 패널 열기, 패널 열림 중 ESC/X 닫기.
static void pump(ULONGLONG now)
{
    void* mc = g_myClone.load(std::memory_order_relaxed);
    if (!mc && !g_panel && !g_panelOpen) return;
    g_reflFault = false;

    // 생존 게이트 (리뷰 확정: 1.5초 유예는 월드 전환 GC 를 통과시킨다 -> 250ms).
    // cls==1 트래픽이 250ms 안에 없으면, 250ms 간격의 멤버십 스캔(FindAllOf 는
    // 죽은 객체를 만지지 않아 항상 안전)으로 생존을 직접 확인하고 유예를 갱신.
    // 스캔 성공이 유예를 되살리므로 펌프 자체는 33ms 전속력을 유지한다.
    if (mc && now - g_lastTitleMs.load(std::memory_order_relaxed) > 250)
    {
        if (now - g_lastLiveScanMs < 250) return;  // 스캔 대기 중엔 클론을 만지지 않는다
        g_lastLiveScanMs = now;
        if (!cloneStillListed(mc))
        {
            cloneLost("멤버십 소실 (월드 전환/메뉴 파괴 추정)");
            return;
        }
        g_lastTitleMs.store(now, std::memory_order_relaxed);
    }
    if (!mc)
    {
        // 클론은 없는데 패널 참조가 남은 비정상 상태 -- 가드된 정리
        if (g_panel || g_panelOpen) closePanel("클론 부재 정리");
        return;
    }
    UObject* clone = reinterpret_cast<UObject*>(mc);

    // v0.23: 안전모드 안내 -- 타이틀 UI 가 살아난 뒤 1회 (게임 스레드)
    if (g_safeModePending.load(std::memory_order_relaxed) && !g_popupOpen)
    {
        g_safeModePending.store(false, std::memory_order_relaxed);
        logf("안전모드 안내 팝업 표시");
        showRestartPopup(g_safeModeText);
    }
    // v0.14: 모드발 재시작 안내 신호 폴링 (1초 스로틀 내장)
    checkModNotifications(now);

    // v0.8: 펌프 박동 계측 (하트비트에서 pumps/gap 출력 -- 굶주림 진단용)
    g_pumpTicks.fetch_add(1, std::memory_order_relaxed);
    if (g_lastPumpTickMs)
    {
        unsigned long long gap = now - g_lastPumpTickMs;
        unsigned long long prev = g_pumpMaxGapMs.load(std::memory_order_relaxed);
        if (gap > prev) g_pumpMaxGapMs.store(gap, std::memory_order_relaxed);
    }
    g_lastPumpTickMs = now;

    // v0.7: 입력은 on_update(5ms) 가 래치한 것을 소비 -- PE 기근에도 클릭을 안 놓친다.
    // 부호 있는 차이 = 래치가 now 캡처 직후에 찍혀도(미래 시각) 신선으로 취급.
    // v0.8: 신선도 600->1200ms -- 펌프 간격이 벌어져도 클릭이 만료되지 않게.
    ULONGLONG pcm = g_pendClickMs.exchange(0, std::memory_order_relaxed);
    bool lmbEdge = pcm != 0 && (long long)(now - pcm) < 1200;
    ULONGLONG pem = g_pendEscMs.exchange(0, std::memory_order_relaxed);
    bool escEdge = pem != 0 && (long long)(now - pem) < 1200;
    // v0.8.2: 클릭 지연 직접 계측 -- "눌렀는데 늦게 반응" 을 클릭마다 수치로
    // v0.17: 뗌 엣지 -- 펼치기/접기처럼 "눌렀다 뗐을 때"만 실행하는 컨트롤용
    ULONGLONG pum = g_pendUpMs.exchange(0, std::memory_order_relaxed);
    bool lmbUpEdge = pum != 0 && (long long)(now - pum) < 1200;
    if (lmbEdge) logf("입력: 클릭 소비 (래치 후 %llums, mouse=%.0f,%.0f)",
                      (unsigned long long)(now - pcm), g_mouseX, g_mouseY);
    if (pcm != 0 && !lmbEdge) logf("입력: 클릭 만료 폐기 (래치 후 %llums)", (unsigned long long)(now - pcm));

    if (g_popupOpen)  // 팝업이 최상위 모달
    {
        if (g_popupPadIcon)
        {   // v0.50: 팝업 (A) 아이콘도 패드 사용 중에만
            bool po = g_inputMode.load(std::memory_order_relaxed) == 1 &&
                      g_padPresent.load(std::memory_order_relaxed);
            static int s_popIco = -1;
            if ((int)po != s_popIco)
            {
                s_popIco = (int)po;
                setVisibility(reinterpret_cast<UObject*>(g_popupPadIcon), po ? 4 : 1, "popup.padtog");
            }
        }
        {   // v0.40(pad): A = 확인. 그 외 패드 엣지는 버린다 -- 쌓아두면 팝업이
            // 닫힌 뒤 묵은 엣지가 재생된다 (리뷰 확정: 켠 모드가 도로 꺼졌다)
            unsigned pe = g_padEdges.exchange(0, std::memory_order_relaxed);
            if (pe & PAD_A)
            {
                closePopup("확인(패드)");
                return;
            }
        }
        // 복제 팝업의 BP 가 스스로 닫혔을 수 있다(자체 버튼 핸들러) -- 상태 동기화
        if (g_popup)
        {
            UObject* pw = reinterpret_cast<UObject*>(g_popup);
            UFunction* fv = fnOf(pw, L"IsVisible", "pop-vis");
            if (fv && (int)fv->GetParmsSize() == 1 && (int)fv->GetReturnValueOffset() == 0)
            {
                PB pb;
                if (peGuard(pw, fv, pb.b) && pb.b[0] == 0)
                {
                    closePopup("자연 소멸 (BP 자체 닫힘)");
                    return;
                }
            }
        }
        if (escEdge)
        {
            closePopup("ESC");
            return;
        }
        if (lmbEdge)
        {
            sampleMouse();
            for (int i = 0; i < 2; ++i)
            {
                if (g_hsPop[i] && isHovered(reinterpret_cast<UObject*>(g_hsPop[i]), "pop-hs") == 1)
                {
                    closePopup("확인");
                    return;
                }
            }
        }
        if (g_reflFault) cloneLost("팝업 리플렉션 SEH");
        return;
    }

    if (g_panelOpen)
    {
        // v0.17: 재구축 직후 스크롤 위치 복원 재시도 (레이아웃이 잡히면 값이 문다)
        if (g_pendScroll >= 0.0f)
        {
            if (now > g_pendScrollUntilMs) g_pendScroll = -1.0f;
            else
            {
                float cur = readScrollOffset(g_scrollBox);
                if (cur >= g_pendScroll - 1.0f) g_pendScroll = -1.0f;  // 닿음(또는 최대치)
                else writeScrollOffset(g_scrollBox, g_pendScroll);
            }
        }
        // v0.50: 패드 이동 직후 잠깐 정밀 재확인 -- 방금 밀어서 그려진 행의 진짜
        // 좌표가 이제야 잡히므로 여기서 정확히 맞춘다 (마지막 행도 제자리에 온다).
        if (g_navSettleUntil && g_inputMode.load(std::memory_order_relaxed) == 1)
        {
            if (now > g_navSettleUntil) g_navSettleUntil = 0;
            else navEnsureVisible(g_navSel, 0);
        }
        // v0.40(pad): 장치 전환 표시 동기화 -- 패드 UI(칩·힌트·테두리)는 패드를
        // 쓸 때만 보이고, 키/마 입력이 오면 즉시 사라진다 (게임 설정창 실측).
        {
            static int s_prevIm = -1;
            int im = g_inputMode.load(std::memory_order_relaxed);
            if (im != s_prevIm)
            {
                s_prevIm = im;
                for (int c = 0; c < 2; ++c)
                    if (g_chipBox[c])
                        setVisibility(reinterpret_cast<UObject*>(g_chipBox[c]), im == 1 ? 4 : 2, "pad-chip");
                if (g_padHintBox)
                    setVisibility(reinterpret_cast<UObject*>(g_padHintBox), im == 1 ? 4 : 1, "pad-hint");
                if (im == 1) navPaintSel(-1, g_navSel);
                else
                {
                    navPaintSel(g_navSel, -1);
                    if (g_comboPadSel >= 0) { paintComboItem(g_comboPadSel, false); g_comboPadSel = -1; }
                }
            }
        }
        // v0.40(pad): 패드 입력 소비. 항상 배출하고, 캡처/색상창/마우스 드래그 중이거나
        // 신선도(400ms)를 넘긴 엣지는 **버린다**(묵은 엣지 재생 방지 -- 리뷰 확정).
        // 드롭다운이 열려 있으면 **드롭다운 안**을 내비게이션한다 (메인과 동시 이동 금지).
        {
            unsigned pe = g_padEdges.exchange(0, std::memory_order_relaxed);
            bool fresh = pe && now - g_padEdgeMs.load(std::memory_order_relaxed) < 400;
            if (fresh && g_comboOpen)
            {
                if (pe & (PAD_UP | PAD_DOWN))
                {
                    int dir2 = (pe & PAD_DOWN) ? 1 : -1;
                    int ns = (g_comboPadSel < 0) ? (dir2 > 0 ? 0 : g_comboN - 1) : g_comboPadSel + dir2;
                    if (ns < 0) ns = 0;
                    if (ns >= g_comboN) ns = g_comboN - 1;
                    if (ns != g_comboPadSel)
                    {
                        paintComboItem(g_comboPadSel, false);
                        g_comboPadSel = ns;
                        paintComboItem(ns, true);
                    }
                }
                else if ((pe & PAD_A) && g_comboPadSel >= 0 && g_comboPadSel < g_comboN)
                {
                    if (comboApplyIdx(g_comboPadSel, clone))
                    {
                        if (g_reflFault) cloneLost("패드 콤보 SEH");
                        return;
                    }
                }
                if (g_reflFault) { cloneLost("패드 콤보 SEH"); return; }
            }
            else if (fresh)
            {
                bool blocked = g_keyCapture.load(std::memory_order_relaxed) || g_colorOpen ||
                               g_sldHs != nullptr || (g_activeTab == 1 && g_dragIdx >= 0);
                if (!blocked)
                {
                    if (padPanelInput(pe, clone))
                    {
                        if (g_reflFault) cloneLost("패드 처리 SEH");
                        return;
                    }
                    if (g_reflFault) { cloneLost("패드 처리 SEH"); return; }
                }
            }
        }
        // v0.29: 슬라이더 드래그 -- 커서 X 를 값으로 바꿔 칸 폭/숫자를 곧바로 고친다.
        // 슬라이더는 '뗄 때 실행' 규약의 예외다(누른 순간부터 손잡이가 따라와야 한다).
        bool sldReopen = false;
        // 추적: 커서 X 를 값으로 바꿔 칸 폭/숫자를 고친다 (저장하지 않는다).
        // 부르기 전에 sampleMouse() 를 해 둘 것 -- 누르고 있는 동안에만 부른다.
        auto sliderTrack = [&]() {
            if (!g_sldHs || g_sldRow < 0 || g_sldRow >= g_plgN) return;
            PlgRow& pr = g_plg[g_sldRow];
            if (g_sldOpt < 0 || g_sldOpt >= pr.optN) return;
            PlgOpt& o = pr.opt[g_sldOpt];
            int nv = sliderValueAt(g_sldHs, o.minV, o.maxV);
            if (nv >= 0 && nv != o.val)
            {
                o.val = nv;
                paintSlider(o.sliderFill, o.sliderRest, o.valText, nv, o.minV, o.maxV);
            }
        };
        // 놓기: 마지막으로 따라간 값을 그대로 확정 저장하고 잡기를 푼다.
        // ⚠ 여기서 커서를 다시 읽지 않는다. 팝업이 끼거나(모달이 펌프를 가로챈다)
        // 펌프가 굶어 뗌을 늦게 알아채면 커서는 이미 딴 데 있고, 그 자리 값을
        // 저장하면 사용자가 놓은 값이 통째로 뒤바뀐다 (리뷰 확정, v0.29).
        auto sliderRelease = [&]() {
            if (g_sldRow >= 0 && g_sldRow < g_plgN)
            {
                PlgRow& pr = g_plg[g_sldRow];
                if (g_sldOpt >= 0 && g_sldOpt < pr.optN)
                {
                    saveOptionValues(pr);
                    logf("slider: '%s' %s = %d", u8(pr.name).c_str(),
                         pr.opt[g_sldOpt].key, pr.opt[g_sldOpt].val);
                    // 이 슬라이더를 부모로 둔 자식 옵션이 있으면 표시를 갱신해야 한다
                    if (optHasChildren(pr, g_sldOpt)) sldReopen = true;
                }
            }
            g_sldHs = nullptr;
            g_sldRow = g_sldOpt = -1;
        };
        if (g_sldHs)
        {
            // 펌프가 굶어 뗌을 통째로 놓쳐도 g_lmbUpMs 변화로 알아챈다(드래그와 같은 규약)
            bool released = !g_lmbHeld.load(std::memory_order_relaxed) ||
                            g_lmbUpMs.load(std::memory_order_relaxed) != g_sldUpSnap;
            if (!released)
            {
                sampleMouse();
                sliderTrack();
            }
            else
            {
                sliderRelease();
                if (sldReopen)
                {
                    rebuildPanel(clone, "슬라이더 자식 갱신");
                    navAfterRebuild();
                    if (g_reflFault) cloneLost("슬라이더 재구축 SEH");
                    return;
                }
            }
        }
        // v0.18: 드래그 스크롤 진행 -- 누른 채 세로로 8px 넘게 움직이면 스크롤로
        // 확정하고(= 대기 중이던 클릭은 취소) 내용이 손가락을 따라오게 한다.
        // 이 빌드의 Slate 는 좌클릭 드래그 스크롤을 주지 않아 직접 구현한다.
        if (g_dsBox)
        {
            if (!g_lmbHeld.load(std::memory_order_relaxed))
            {
                // v0.40: 뗌 엣지가 있으면 아래 뗌 처리가 정리한다. 그런데 배경에서
                // 뗀 경우(전경 게이트가 엣지를 만들지 않음)나 엣지가 신선도(1200ms)를
                // 넘겨 증발한 경우에는 그 처리가 영영 안 온다 -- 여기서 걷는다.
                // (리뷰 확정: 스크롤바 고정, 콤보 바깥클릭 1회 먹힘, 복귀 클릭 때
                //  묵은 g_dsStartY 로 목록이 순간이동)
                if (!lmbUpEdge)
                {
                    clearArm();
                    clearDragScroll();
                }
            }
            else
            {
                sampleMouse();
                double dy = g_dsStartY - g_mouseY;
                if (!g_dsActive && (dy > 8.0 || dy < -8.0))
                {
                    g_dsActive = true;
                    clearArm();  // 끌기로 확정 = 눌렀던 버튼은 실행하지 않는다
                }
                if (g_dsActive)
                {
                    float nv = g_dsStartOff + (float)dy;
                    if (nv < 0.0f) nv = 0.0f;
                    writeScrollOffset(g_dsBox, nv);
                    tickScrollbar(g_dsWhich, g_dsBox, now, true);
                }
            }
        }
        // v0.18: 스크롤바 자동 숨김 -- 오프셋이 스스로 바뀌면(휠) 띄우고 1초 뒤 감춘다.
        // (우리가 복원 중일 때는 건드리지 않는다 -- 그 움직임은 사용자 것이 아니다)
        if (g_pendScroll < 0.0f && !(g_dsActive && g_dsWhich == 0))
            tickScrollbar(0, g_scrollBox, now, false);
        if (g_comboOpen && !(g_dsActive && g_dsWhich == 1))
            tickScrollbar(1, g_comboScrollBox, now, false);
        // v0.28: 키 캡처 중 -- 눌린 키를 잡아 저장한다. ESC 는 취소.
        if (g_keyCapture.load(std::memory_order_relaxed))
        {
            int vk = g_capturedVk.exchange(0, std::memory_order_relaxed);
            if (escEdge) { vk = -1; }
            if (vk != 0)
            {
                g_keyCapture.store(false, std::memory_order_relaxed);
                int row = g_keyCapRow, oi = g_keyCapOpt;
                g_keyCapRow = g_keyCapOpt = -1;
                if (vk > 0 && row >= 0 && row < g_plgN)
                {
                    PlgRow& r = g_plg[row];
                    if (oi >= 0 && oi < r.optN)
                    {
                        // Delete = 해제(0 = 없음). 매니페스트 default 를 생략했을 때와 같은 상태.
                        int nv = (vk == VK_DELETE) ? 0 : vk;
                        r.opt[oi].val = nv;
                        saveOptionValues(r);
                        logf("key: '%s' %s = %d%s", u8(r.name).c_str(), r.opt[oi].key, nv,
                             nv == 0 ? " (해제)" : "");
                    }
                }
                rebuildPanel(clone, "키 지정 갱신");
                navAfterRebuild();
                if (g_reflFault) cloneLost("키 캡처 재구축 SEH");
                return;
            }
            if (g_reflFault) cloneLost("키 캡처 SEH");
            return;   // 캡처 중에는 다른 입력을 받지 않는다
        }
        // v0.28: 색상 선택창 (콤보와 같은 모달 층)
        if (g_colorOpen)
        {
            if (escEdge) { closeColor("ESC"); return; }
            sampleMouse();
            bool held = g_lmbHeld.load(std::memory_order_relaxed);
            bool changed = false;
            if (held || lmbEdge)
            {
                double x0, y0, x1, y1;
                if (g_colorSV && widgetRectAbs(reinterpret_cast<UObject*>(g_colorSV), &x0, &y0, &x1, &y1) &&
                    g_mouseX >= x0 && g_mouseX <= x1 && g_mouseY >= y0 && g_mouseY <= y1)
                {
                    g_colorS = (float)((g_mouseX - x0) / (x1 - x0));
                    g_colorV = 1.0f - (float)((g_mouseY - y0) / (y1 - y0));
                    g_hexLen = 0;
                    changed = true;
                }
                else if (g_colorHue && widgetRectAbs(reinterpret_cast<UObject*>(g_colorHue), &x0, &y0, &x1, &y1) &&
                         g_mouseX >= x0 && g_mouseX <= x1 && g_mouseY >= y0 && g_mouseY <= y1)
                {
                    g_colorH = (float)((g_mouseX - x0) / (x1 - x0));
                    if (g_colorH >= 1.0f) g_colorH = 0.999f;
                    g_hexLen = 0;
                    changed = true;
                }
            }
            if (lmbEdge && !changed)
            {
                bool hitPreset = false;
                for (int i = 0; i < g_colorSwatchN; ++i)
                {
                    if (!g_colorSwatchHs[i]) continue;
                    if (hitPtQuiet(reinterpret_cast<UObject*>(g_colorSwatchHs[i])) != 1) continue;
                    int rgb = g_colorSwatchRgb[i];
                    float rr = ((rgb >> 16) & 0xFF) / 255.0f, gg = ((rgb >> 8) & 0xFF) / 255.0f,
                          bb = (rgb & 0xFF) / 255.0f;
                    float mx = rr > gg ? (rr > bb ? rr : bb) : (gg > bb ? gg : bb);
                    float mn = rr < gg ? (rr < bb ? rr : bb) : (gg < bb ? gg : bb);
                    float d = mx - mn;
                    g_colorV = mx;
                    g_colorS = mx > 0 ? d / mx : 0;
                    if (d <= 0.0001f) g_colorH = 0;
                    else if (mx == rr) g_colorH = fmodf((gg - bb) / d / 6.0f + 1.0f, 1.0f);
                    else if (mx == gg) g_colorH = ((bb - rr) / d + 2.0f) / 6.0f;
                    else g_colorH = ((rr - gg) / d + 4.0f) / 6.0f;
                    g_hexLen = 0;
                    changed = hitPreset = true;
                    break;
                }
                if (!hitPreset)
                {
                    // 창 밖을 눌렀다 = 닫기 (콤보와 같은 규약)
                    double bx0, by0, bx1, by1;
                    bool inside = g_colorHost &&
                                  widgetRectAbs(reinterpret_cast<UObject*>(g_colorSV), &bx0, &by0, &bx1, &by1);
                    (void)inside;
                    bool onPanel = false;
                    for (void* w : {g_colorSV, g_colorHue, g_colorPreview})
                        if (w && hitPtQuiet(reinterpret_cast<UObject*>(w)) == 1) onPanel = true;
                    if (!onPanel) { closeColor("바깥 클릭"); return; }
                }
            }
            // HEX 타이핑 (0-9 A-F, 6자리 채우면 즉시 적용)
            int vk = g_capturedVk.exchange(0, std::memory_order_relaxed);
            if (vk > 0)
            {
                wchar_t ch = 0;
                if (vk >= '0' && vk <= '9') ch = (wchar_t)vk;
                else if (vk >= 'A' && vk <= 'F') ch = (wchar_t)vk;
                else if (vk == VK_BACK && g_hexLen > 0) { g_hexBuf[--g_hexLen] = 0; changed = true; }
                if (ch && g_hexLen < 6)
                {
                    g_hexBuf[g_hexLen++] = ch;
                    g_hexBuf[g_hexLen] = 0;
                    changed = true;
                    if (g_hexLen == 6)
                    {
                        int rgb = (int)wcstol(g_hexBuf, nullptr, 16);
                        float rr = ((rgb >> 16) & 0xFF) / 255.0f, gg = ((rgb >> 8) & 0xFF) / 255.0f,
                              bb = (rgb & 0xFF) / 255.0f;
                        float mx = rr > gg ? (rr > bb ? rr : bb) : (gg > bb ? gg : bb);
                        float mn = rr < gg ? (rr < bb ? rr : bb) : (gg < bb ? gg : bb);
                        float d = mx - mn;
                        g_colorV = mx;
                        g_colorS = mx > 0 ? d / mx : 0;
                        if (d <= 0.0001f) g_colorH = 0;
                        else if (mx == rr) g_colorH = fmodf((gg - bb) / d / 6.0f + 1.0f, 1.0f);
                        else if (mx == gg) g_colorH = ((bb - rr) / d + 2.0f) / 6.0f;
                        else g_colorH = ((rr - gg) / d + 4.0f) / 6.0f;
                        g_hexLen = 0;
                        g_hexBuf[0] = 0;
                    }
                }
            }
            if (changed)
            {
                paintColorPicker();
                if (g_colorRow >= 0 && g_colorRow < g_plgN)
                {
                    PlgRow& r = g_plg[g_colorRow];
                    if (g_colorOpt >= 0 && g_colorOpt < r.optN)
                    {
                        PlgOpt& o = r.opt[g_colorOpt];
                        o.val = currentColorRgb();
                        if (o.swatch)
                            setBrushColor(reinterpret_cast<UObject*>(o.swatch), rgbToLin(o.val), "opt.sw");
                        if (o.comboTx)
                        {
                            wchar_t hx[16];
                            swprintf(hx, 16, L"#%06X", o.val & 0xFFFFFF);
                            setTextOn(reinterpret_cast<UObject*>(o.comboTx), hx, "opt.hex");
                        }
                        saveOptionValues(r);
                    }
                }
            }
            if (g_reflFault) cloneLost("색상창 SEH");
            return;
        }
        // v0.15: 드롭다운이 팝업 다음 최상위 모달 -- 호버 페인팅은 매 틱(설정창처럼
        // 마우스 따라 하이라이트), 클릭 = 항목 선택 / 바깥 닫기, ESC = 드롭다운만 닫기
        if (g_comboOpen)
        {
            if (escEdge)
            {
                closeCombo("ESC");
                return;
            }
            sampleMouse();  // 호버도 클릭과 같은 좌표 산술 -- 판정 불일치 원천 차단
            int hov = -1;
            for (int i = 0; i < g_comboN; ++i)
                if (g_comboItemHs[i] && hitPtQuiet(reinterpret_cast<UObject*>(g_comboItemHs[i])) == 1)
                {
                    hov = i;
                    break;
                }
            if (hov != g_comboHover)
            {
                paintComboItem(g_comboHover, false);
                paintComboItem(hov, true);
                g_comboHover = hov;
            }
            // v0.18: 누를 때는 '대기'만 -- 끌면 스크롤, 같은 항목에서 떼면 선택.
            if (lmbEdge && g_lmbHeld.load(std::memory_order_relaxed))
            {
                clearArm();
                g_armKind = ARM_COMBO_ITEM;
                g_armItem = hov;  // -1 = 목록 바깥(떼면 닫기)
                if (g_comboScrollBox && hov >= 0)
                {
                    g_dsBox = g_comboScrollBox;
                    g_dsWhich = 1;
                    g_dsActive = false;
                    g_dsStartY = g_mouseY;
                    float cur = readScrollOffset(g_comboScrollBox);
                    g_dsStartOff = cur > 0.0f ? cur : 0.0f;
                }
            }
            if (lmbUpEdge)
            {
                int armed = (g_armKind == ARM_COMBO_ITEM) ? g_armItem : -2;
                bool wasDrag = g_dsActive;
                clearArm();
                clearDragScroll();
                if (!wasDrag && armed != -2)
                {
                    bool reopenAfter = false;
                    // 누른 항목 위에서 떼야 선택 (다른 항목으로 옮겨 떼면 취소)
                    if (armed >= 0 && armed == hov)
                    {
                        if (g_comboRow < 0)   // v0.40: 언어 콤보 (매니저 자체)
                        {
                            int want = (hov == 1) ? 1 : 0;
                            if (want != g_lang)
                            {
                                applyLang(want);
                                saveLang();
                                logf("lang: 사용자 선택 -> %s", g_lang ? "en" : "ko");
                                setLabel(clone, "lang");   // 타이틀 메뉴 항목 이름 갱신
                                reopenAfter = true;        // 모든 라벨 갱신 = 패널 재구축
                            }
                        }
                        else if (g_comboRow >= 0 && g_comboRow < g_plgN)
                        {
                            PlgRow& r = g_plg[g_comboRow];
                            if (g_comboOpt >= 0 && g_comboOpt < r.optN)
                            {
                                PlgOpt& o = r.opt[g_comboOpt];
                                if (hov < o.choiceN && o.val != hov)
                                {
                                    o.val = hov;
                                    if (g_comboValTx)
                                        setTextOn(reinterpret_cast<UObject*>(g_comboValTx),
                                                  o.choices[hov], "combo-val");
                                    saveOptionValues(r);
                                    if (optHasChildren(r, g_comboOpt)) reopenAfter = true;
                                    logf("combo: '%s' %s = %d (%s)", u8(r.name).c_str(), o.key,
                                         hov, u8(o.choices[hov]).c_str());
                                }
                            }
                        }
                        closeCombo("선택");
                    }
                    else if (armed < 0 && hov < 0) closeCombo("바깥 클릭");
                    if (reopenAfter)
                    {
                        rebuildPanel(clone, "옵션 갱신(콤보)");
                        navAfterRebuild();
                    }
                }
            }
            if (g_reflFault) cloneLost("콤보 리플렉션 SEH");
            return;
        }
        // v0.16: 순서 탭 드래그 진행 -- 매 틱: 마우스 아래 행으로 실시간 재배열,
        // 버튼을 놓으면(on_update 5ms 래치) 저장. ESC 는 그 자리에서 드롭 확정.
        if (g_activeTab == 1 && g_dragIdx >= 0 && g_dragIdx < g_ordN)
        {
            bool held = g_lmbHeld.load(std::memory_order_relaxed);
            // 잡은 뒤 뗌 엣지가 한 번이라도 있었으면 지금 눌려 있어도 그건 '새 누름'
            // 이다 -- 그 사이 마우스가 옮겨갔을 수 있어 계속 끌면 안 된다.
            if (g_lmbUpMs.load(std::memory_order_relaxed) != g_dragUpSnap) held = false;
            if (escEdge) held = false;  // ESC = 현재 위치에서 드래그 종료 (패널 유지)
            sampleMouse();
            int over = -1;
            for (int i = 0; i < g_ordN; ++i)
                if (g_ord[i].band && hitPtQuiet(reinterpret_cast<UObject*>(g_ord[i].band)) == 1)
                {
                    over = i;
                    break;
                }
            if (over >= 0 && over != g_dragIdx)
            {
                // 내용(name/label)만 이동 -- 위젯(밴드/라벨)은 고정 슬롯
                wchar_t tn[64], tl[64];
                lstrcpynW(tn, g_ord[g_dragIdx].name, 64);
                lstrcpynW(tl, g_ord[g_dragIdx].label, 64);
                int step = over > g_dragIdx ? 1 : -1;
                for (int i = g_dragIdx; i != over; i += step)
                {
                    lstrcpynW(g_ord[i].name, g_ord[i + step].name, 64);
                    lstrcpynW(g_ord[i].label, g_ord[i + step].label, 64);
                }
                lstrcpynW(g_ord[over].name, tn, 64);
                lstrcpynW(g_ord[over].label, tl, 64);
                for (int i = 0; i < g_ordN; ++i)
                    if (g_ord[i].text)
                        setTextOn(reinterpret_cast<UObject*>(g_ord[i].text), g_ord[i].label, "ord-lbl");
                if (g_ord[g_dragIdx].band)  // 하이라이트 이동 (이전 행 원복)
                    setBrushColor(reinterpret_cast<UObject*>(g_ord[g_dragIdx].band),
                                  {1, 1, 1, 0.07f}, "ord-band");
                g_dragIdx = over;
                g_dragMoved = true;
            }
            if (g_ord[g_dragIdx].band)
                setBrushColor(reinterpret_cast<UObject*>(g_ord[g_dragIdx].band),
                              {1, 1, 1, 0.20f}, "ord-band");
            if (!held)
            {
                if (g_ord[g_dragIdx].band)
                    setBrushColor(reinterpret_cast<UObject*>(g_ord[g_dragIdx].band),
                                  {1, 1, 1, 0.07f}, "ord-band");
                if (g_dragMoved) saveOrderFile();
                logf("order: 드래그 종료 (%s)", g_dragMoved ? "변경 저장" : "변경 없음");
                g_dragIdx = -1;
                g_dragMoved = false;
            }
            if (g_reflFault) cloneLost("순서 드래그 SEH");
            return;  // 드래그 중 다른 패널 입력 정지
        }
        if (escEdge)
        {
            if (g_padOrdLift >= 0)   // v0.40(pad): 집어든 이동을 버리지 않는다 (마우스 ESC 와 동일)
            {
                g_padOrdLift = -1;
                saveOrderFile();
                logf("pad: 순서 저장(닫기)");
            }
            closePanel("ESC");
            return;
        }
        if (lmbEdge)
        {
            sampleMouse();  // 이번 클릭의 좌표 1회 확보 (이하 산술 판정에 공용)
            for (int i = 0; i < 3; ++i)
            {
                if (g_hsX[i] && isHovered(reinterpret_cast<UObject*>(g_hsX[i]), "x-hs") == 1)
                {
                    if (g_padOrdLift >= 0)
                    {
                        g_padOrdLift = -1;
                        saveOrderFile();
                        logf("pad: 순서 저장(닫기)");
                    }
                    closePanel("X 버튼");
                    return;
                }
            }
            // v0.16: 탭 전환 -- 클릭 = 해당 탭으로 패널 재구축 (자식 옵션 reopen 경로)
            for (int t = 0; t < 2; ++t)
            {
                if (g_hsTab[t] && isHovered(reinterpret_cast<UObject*>(g_hsTab[t]), "tab-hs") == 1)
                {
                    if (g_activeTab != t)
                    {
                        g_activeTab = t;
                        g_padOrdLift = -1;
                        logf("panel: 탭 전환 -> %s", t == 0 ? "모드" : "순서");
                        rebuildPanel(clone, "탭 전환", false);
                        navAfterRebuild();
                    }
                    return;  // 탭 클릭 소비
                }
            }
            // v0.16: 순서 탭 -- 행 잡기 (드래그 시작). 이후 틱은 위 드래그 블록이 처리
            if (g_activeTab == 1)
            {
                // 아직 누르고 있을 때만 잡는다 -- 클릭 엣지는 1200ms 까지 신선으로
                // 취급되므로, 이미 뗀 묵은 엣지로 유령 드래그가 시작되는 걸 막는다.
                if (g_lmbHeld.load(std::memory_order_relaxed))
                {
                    for (int i = 0; i < g_ordN; ++i)
                    {
                        if (g_ord[i].band && hitPtQuiet(reinterpret_cast<UObject*>(g_ord[i].band)) == 1)
                        {
                            g_dragIdx = i;
                            g_dragMoved = false;
                            g_padOrdLift = -1;   // v0.40(pad): 패드 집기와 동시 진행 금지
                            g_dragUpSnap = g_lmbUpMs.load(std::memory_order_relaxed);
                            setBrushColor(reinterpret_cast<UObject*>(g_ord[i].band),
                                          {1, 1, 1, 0.20f}, "ord-band");
                            logf("order: '%s' 잡음 (행 %d)", u8(g_ord[i].name).c_str(), i);
                            break;
                        }
                    }
                }
                if (g_reflFault) cloneLost("패널 리플렉션 SEH");
                return;  // 순서 탭에는 폴더 버튼/토글류가 없다
            }
            // v0.18: 여기부터는 스크롤 영역 안의 컨트롤이다. 누를 때는 대기만 걸고
            // (끌면 스크롤이 되어야 하므로) 실행은 뗄 때 한다. 동시에 드래그 스크롤
            // 후보를 세운다 -- 어디를 눌러도 끌면 목록이 따라온다.
            clearArm();
            clearDragScroll();
            // v0.29: 슬라이더를 잡았으면 여기서 끝낸다 -- 무장(뗄 때 실행)도,
            // 패널 드래그 스크롤도 걸지 않는다. 가로 드래그가 목록을 흔들면 안 된다.
            g_sldHs = nullptr;
            g_sldRow = g_sldOpt = -1;
            for (int i = 0; i < g_plgN && !g_sldHs; ++i)
            {
                PlgRow& r = g_plg[i];
                for (int oi = 0; oi < r.optN; ++oi)
                {
                    PlgOpt& o = r.opt[oi];
                    if (o.type != 6 || !o.comboHs) continue;
                    if (hitPtQuiet(reinterpret_cast<UObject*>(o.comboHs)) != 1) continue;
                    g_sldHs = o.comboHs;
                    g_sldRow = i;
                    g_sldOpt = oi;
                    break;
                }
            }
            if (g_sldHs)
            {
                sliderTrack();   // 누른 그 지점으로 손잡이가 즉시 온다
                if (g_lmbHeld.load(std::memory_order_relaxed))
                {
                    g_sldUpSnap = g_lmbUpMs.load(std::memory_order_relaxed);  // 끌기 시작
                }
                else
                {
                    // 펌프 두 틱 사이에 끝난 짧은 클릭. 다른 컨트롤은 무장->뗌이 한 틱
                    // 안에 끝나 살아남지만 슬라이더는 무장 경로가 없다(아래 루프가
                    // type 6 을 건너뛴다) -- 여기서 확정하지 않으면 클릭이 통째로
                    // 사라진다 (리뷰 확정, v0.29).
                    sliderRelease();
                }
                if (sldReopen)
                {
                    rebuildPanel(clone, "슬라이더 자식 갱신");
                    navAfterRebuild();
                    if (g_reflFault) cloneLost("슬라이더 재구축 SEH");
                    return;
                }
                if (g_reflFault) cloneLost("슬라이더 잡기 SEH");
                return;
            }
            for (int i = 0; i < 3 && g_armKind == ARM_NONE; ++i)
            {
                if (g_hsBtn[i] && isHovered(reinterpret_cast<UObject*>(g_hsBtn[i]), "btn-hs") == 1)
                {
                    g_armKind = ARM_FOLDER;
                    g_armHs = g_hsBtn[i];
                }
            }
            if (g_armKind == ARM_NONE && g_langHs &&
                isHovered(reinterpret_cast<UObject*>(g_langHs), "lang-cbo") == 1)
            {
                g_armKind = ARM_LANG;
                g_armHs = g_langHs;
                g_armRow = -1;
            }
            for (int i = 0; i < g_plgN && g_armKind == ARM_NONE; ++i)
            {
                PlgRow& r = g_plg[i];
                if (r.offPill && isHovered(reinterpret_cast<UObject*>(r.offPill), "tgl-off") == 1)
                {
                    g_armKind = ARM_MOD_OFF;
                    g_armHs = r.offPill;
                    g_armRow = i;
                    break;
                }
                if (r.onPill && isHovered(reinterpret_cast<UObject*>(r.onPill), "tgl-on") == 1)
                {
                    g_armKind = ARM_MOD_ON;
                    g_armHs = r.onPill;
                    g_armRow = i;
                    break;
                }
                if (r.expandHs && isHovered(reinterpret_cast<UObject*>(r.expandHs), "opt-fold") == 1)
                {
                    g_armKind = ARM_FOLD;
                    g_armHs = r.expandHs;
                    g_armRow = i;
                    break;
                }
                for (int oi = 0; oi < r.optN && g_armKind == ARM_NONE; ++oi)
                {
                    PlgOpt& o = r.opt[oi];
                    if (o.type == 6) continue;        // v0.29: 슬라이더는 누를 때 처리했다
                    if (o.type == 4 || o.type == 5)   // v0.29: 체크박스 / 실행 버튼
                    {
                        if (o.comboHs && isHovered(reinterpret_cast<UObject*>(o.comboHs), "opt-cb") == 1)
                        {
                            g_armKind = (o.type == 4) ? ARM_CHECK : ARM_BUTTON;
                            g_armHs = o.comboHs;
                        }
                    }
                    else if (o.type == 2 || o.type == 3)   // v0.28: 키 바인딩 / 색상
                    {
                        if (o.comboHs && isHovered(reinterpret_cast<UObject*>(o.comboHs), "opt-kc") == 1)
                        {
                            g_armKind = (o.type == 2) ? ARM_KEYBIND : ARM_COLOR_OPEN;
                            g_armHs = o.comboHs;
                        }
                    }
                    else if (o.choiceN >= 2)
                    {
                        if (o.comboHs && isHovered(reinterpret_cast<UObject*>(o.comboHs), "opt-cbo") == 1)
                        {
                            g_armKind = ARM_COMBO_OPEN;
                            g_armHs = o.comboHs;
                        }
                    }
                    else if (o.type == 0)
                    {
                        if (o.boolOff && isHovered(reinterpret_cast<UObject*>(o.boolOff), "opt-off") == 1)
                        {
                            g_armKind = ARM_OPT_OFF;
                            g_armHs = o.boolOff;
                        }
                        else if (o.boolOn && isHovered(reinterpret_cast<UObject*>(o.boolOn), "opt-on") == 1)
                        {
                            g_armKind = ARM_OPT_ON;
                            g_armHs = o.boolOn;
                        }
                    }
                    else
                    {
                        if (o.hsDec && isHovered(reinterpret_cast<UObject*>(o.hsDec), "opt-dec") == 1)
                        {
                            g_armKind = ARM_DEC;
                            g_armHs = o.hsDec;
                        }
                        else if (o.hsInc && isHovered(reinterpret_cast<UObject*>(o.hsInc), "opt-inc") == 1)
                        {
                            g_armKind = ARM_INC;
                            g_armHs = o.hsInc;
                        }
                    }
                    if (g_armKind != ARM_NONE) { g_armRow = i; g_armOpt = oi; }
                }
            }
            if (g_scrollBox && g_lmbHeld.load(std::memory_order_relaxed))
            {   // 컨트롤 위든 빈 곳이든 드래그 스크롤은 항상 가능
                // v0.40: 재구축 복원(g_pendScroll)이 진행 중이면 여기서 접는다 --
                // 복원 루프가 매 틱 목표 오프셋을 다시 쓰면서 사용자의 드래그와
                // 싸웠고, 손을 떼면 목표(때로 끝)까지 관성처럼 끌려갔다 (실측 보고).
                g_pendScroll = -1.0f;
                g_dsBox = g_scrollBox;
                g_dsWhich = 0;
                g_dsActive = false;
                g_dsStartY = g_mouseY;
                float cur = readScrollOffset(g_scrollBox);
                g_dsStartOff = cur > 0.0f ? cur : 0.0f;
            }
        }
        // v0.18: 뗄 때 실행 -- 누른 그 컨트롤 위에서 떼야 하고, 끌었으면 취소된다
        if (lmbUpEdge && g_armKind != ARM_NONE)
        {
            int kind = g_armKind;
            void* hs = g_armHs;
            int row = g_armRow, oi = g_armOpt;
            bool wasDrag = g_dsActive;
            clearArm();
            clearDragScroll();
            sampleMouse();
            bool onIt = !wasDrag && hs &&
                        isHovered(reinterpret_cast<UObject*>(hs), "arm-up") == 1;
            bool needReopen = false, needPopup = false;
            if (onIt && kind == ARM_FOLDER)
            {
                logf("panel: '폴더 바로가기' 클릭");
                openPluginsFolder();  // 패널은 열린 채 유지
            }
            else if (onIt && kind == ARM_LANG)
            {
                // v0.40: 언어 콤보 -- g_comboRow = -1 이 매니저 자체 옵션 표식
                openCombo(reinterpret_cast<UObject*>(g_langHs), g_langChoices, 2,
                          (g_lang == 1) ? 1 : 0, -1, -1, g_langTx);
            }
            else if (onIt && row >= 0 && row < g_plgN)
            {
                PlgRow& r = g_plg[row];
                if (kind == ARM_MOD_OFF)
                {
                    if (r.on)
                    {
                        r.on = false;
                        paintToggle(r);
                        applyPluginState(r);
                        // v0.50: 미로드 모드는 옵션 없어도 재구축 ('재시작 필요' 배지 갱신)
                        if (r.optN || !sessionLoaded(r.name)) needReopen = true;  // 옵션 서브행 접기
                    }
                }
                else if (kind == ARM_MOD_ON)
                {
                    if (!r.on)
                    {
                        r.on = true;
                        paintToggle(r);
                        applyPluginState(r);
                        // v0.50: 미로드 모드는 옵션 없어도 재구축 ('재시작 필요' 배지 갱신)
                        if (r.optN || !sessionLoaded(r.name)) needReopen = true;  // 옵션 서브행 펼치기
                        // 이번 세션에 로드 안 된 모드를 켬 = 재시작해야 적용 -> 게임식 팝업
                        if (!sessionLoaded(r.name)) needPopup = true;
                    }
                }
                else if (kind == ARM_FOLD)
                {
                    bool on = !isExpanded(r.name);
                    setExpanded(r.name, on);
                    logf("panel: '%s' 옵션 %s", u8(r.name).c_str(), on ? "펼침" : "접힘");
                    needReopen = true;
                }
                else if (oi >= 0 && oi < r.optN)
                {
                    PlgOpt& o = r.opt[oi];
                    if (kind == ARM_KEYBIND)
                    {
                        g_keyCapture.store(true, std::memory_order_relaxed);
                        g_capturedVk.store(0, std::memory_order_relaxed);
                        g_keyCapRow = row;
                        g_keyCapOpt = oi;
                        if (o.comboTx)
                            setTextOn(reinterpret_cast<UObject*>(o.comboTx),
                                      TR(L"키를 누르세요  (Delete 해제 · ESC 취소)",
                                         L"Press a key  (Delete = clear · ESC = cancel)"), "opt.keycap");
                        logf("key: '%s' %s 캡처 시작", u8(r.name).c_str(), o.key);
                    }
                    else if (kind == ARM_COLOR_OPEN)
                    {
                        openColorPicker(reinterpret_cast<UObject*>(o.comboHs), o.val & 0xFFFFFF, row, oi);
                    }
                    else if (kind == ARM_COMBO_OPEN)
                    {
                        openCombo(reinterpret_cast<UObject*>(o.comboHs), o.choices, o.choiceN,
                                  (o.val >= 0 && o.val < o.choiceN) ? o.val : 0, row, oi, o.comboTx);
                    }
                    else if (kind == ARM_CHECK)
                    {
                        o.val = o.val ? 0 : 1;
                        saveOptionValues(r);
                        logf("check: '%s' %s = %d", u8(r.name).c_str(), o.key, o.val);
                        needReopen = true;   // 체크 그림 = 텍스처 교체
                    }
                    else if (kind == ARM_BUTTON)
                    {
                        // 값은 '누른 횟수'. 모드는 값이 늘어난 것을 보고 한 번 일한다.
                        o.val = (o.val < 0x7FFFFFF) ? o.val + 1 : 1;
                        saveOptionValues(r);
                        logf("button: '%s' %s -> %d", u8(r.name).c_str(), o.key, o.val);
                        if (optHasChildren(r, oi)) needReopen = true;
                    }
                    else if (kind == ARM_OPT_OFF || kind == ARM_OPT_ON)
                    {
                        int want = (kind == ARM_OPT_ON) ? 1 : 0;
                        if (o.val != want)
                        {
                            o.val = want;
                            paintOpt(o);
                            saveOptionValues(r);
                            // 매니페스트에 이 옵션을 부모로 둔 자식이 있을 때만 화면 갱신
                            if (optHasChildren(r, oi)) needReopen = true;
                        }
                    }
                    else if (kind == ARM_DEC || kind == ARM_INC)
                    {
                        int nv = o.val + (kind == ARM_INC ? o.step : -o.step);
                        if (nv < o.minV) nv = o.minV;
                        if (nv > o.maxV) nv = o.maxV;
                        if (nv != o.val)
                        {
                            o.val = nv;
                            if (o.valText)
                            {
                                wchar_t vbuf[16];
                                swprintf(vbuf, 16, L"%d", o.val);
                                setTextOn(reinterpret_cast<UObject*>(o.valText), vbuf, "opt-val");
                            }
                            saveOptionValues(r);
                            if (optHasChildren(r, oi)) needReopen = true;
                        }
                    }
                }
            }
            if (needReopen)
            {
                rebuildPanel(clone, "옵션 갱신");
                navAfterRebuild();
            }
            if (needPopup) showRestartPopup();
        }
        else if (lmbUpEdge) clearDragScroll();  // 컨트롤 밖에서 끌다 놓음
        if (g_reflFault) cloneLost("패널 리플렉션 SEH");
        return;  // 패널 열림 중에는 메뉴 호버/클릭 처리 안 함 (스크림이 어차피 흡수)
    }

    // v0.40: 라벨 키퍼 -- 게임 언어를 바꾸면 게임이 텍스트 바인딩을 다시 풀며
    // 우리가 SetText 한 라벨을 지운다 (실측 2026-08-10: TitleMenuType=100 은 게임
    // 문자열표에 없어 빈 값이 됐다. '나가기'는 렌더되므로 폰트 문제가 아니다).
    // v0.40: 매 틱 SetText 는 FText 누수를 무한히 쌓는다(전사 규율 = FText 파괴
    // 금지, 리뷰 확정 시간당 ~1800개). 라벨이 지워지는 사건은 게임 언어(문화)
    // 전환뿐이므로, LanguageText 값이 실제로 바뀐 틱과 우리 언어가 바뀐 틱에만
    // 다시 쓴다. strict=false = 실패해도 조용히(다음 전환 틱에 재시도).
    static ULONGLONG s_lastLangKeepMs = 0;
    static int s_lastLangText = -2;   // -2 = 아직 못 읽음
    if (now - s_lastLangKeepMs > 150 && !g_panelOpen && !g_popupOpen && !g_comboOpen)
    {   // v0.40 6차: 150ms -- 게임 언어 전환이 라벨을 지우는 공백을 한 프레임 수준으로.
        // 파일 부담은 mtime 캐시가 막는다(변경 없으면 stat 한 번).
        // (9차: ≈2초 내비 규칙 재주장 삭제 -- 메뉴는 포커스 구동이 아님이 확정)
        s_lastLangKeepMs = now;
        bool padActive = g_inputMode.load(std::memory_order_relaxed) == 1 &&
                         g_padPresent.load(std::memory_order_relaxed);
        static bool s_lastPadActive = false;
        if (padActive != s_lastPadActive)
        {   // 11b: 패드 사용 중에만 Y 아이콘 표시(가시성 토글 -- 위젯 재생성 없음).
            s_lastPadActive = padActive;
            if (g_padIcon)
                setVisibility(reinterpret_cast<UObject*>(g_padIcon), padActive ? 4 : 1, "padico.tog");
        }
        // v0.50: 위치 원복 -- 게임의 Exit 로컬라이즈가 안정(라벨 무변화 0.8초)된
        // 뒤 1회 재정렬해 클론을 설정<->나가기 사이로. Exit 라벨은 재정렬 순간 캡처해
        // 복원(remove+readd 가 라벨을 기본값으로 되돌리므로). 안정 대기로 로컬라이즈
        // 완료 후 캡처가 보장된다(영어판 "나가기" 오류의 뿌리 = 너무 이른 캡처였다).
        // 정렬이 끝날 때까지 클론은 Collapsed(finishClone) -- 완료/실패/시간초과
        // 어느 경로든 반드시 Visible(0) 복귀시킨다 (숨은 채 남으면 메뉴에서 실종).
        if (g_reorderPending)
        {
            // 이 블록은 게이트(팝업·패널 닫힘)가 열렸을 때만 도니, 틱 수 = 실제로
            // 재정렬을 시도할 수 있었던 시간이다 (150ms 간격 x 33 = 약 5초).
            if (++g_reorderOpenTicks > 33)
            {   // 안전망: Exit 라벨을 끝내 못 읽거나 계속 흔들리면 포기 -- 맨 아래로라도 표시
                g_reorderPending = false;
                setVisibility(clone, 0, "reorderShow");
                logf("WARN reorder: 시간초과(열린 틱 %d) -- 재정렬 포기, 맨 아래 표시", g_reorderOpenTicks);
            }
            else
            {
            UObject* rbox = reinterpret_cast<UObject*>(g_doneBox.load());
            UObject* rexit = reinterpret_cast<UObject*>(g_menuExit.load(std::memory_order_relaxed));
            UObject* rtt = rexit ? readObjProp(rexit, L"TitleText", "reorder") : nullptr;
            PB cur;
            bool haveCur = false;
            if (rtt)
            {
                UFunction* fg = fnOf(rtt, L"GetText", "reorder");
                if (fg && (int)fg->GetParmsSize() == 24 && (int)fg->GetReturnValueOffset() == 0)
                    haveCur = peGuard(rtt, fg, cur.b);
            }
            if (rbox && rexit && haveCur)
            {
                bool changed = !g_reorderHaveLast || memcmp(cur.b, g_reorderLastExit, 24) != 0;
                if (changed)
                {
                    memcpy(g_reorderLastExit, cur.b, 24);
                    g_reorderHaveLast = true;
                    g_reorderStableSince = now;
                }
                else if (now - g_reorderStableSince >= 800)
                {
                    g_reorderPending = false;
                    UFunction* fAdd = fnOf(rbox, L"AddChild", "reorder");
                    UFunction* fRm = fnOf(rbox, L"RemoveChild", "reorder");
                    if (fAdd && fRm && (int)fAdd->GetParmsSize() == 16)
                    {
                        int ps = (int)fRm->GetParmsSize(), ro = (int)fRm->GetReturnValueOffset();
                        if (ps > 8 && ps <= 64 && ro >= 8 && ro < 64)
                        {
                            // C-2: 떼기 직전 라벨을 전역에 보관 -- 재부착이 다음
                            // 틱(수리 모드)으로 밀려도 복원할 수 있게.
                            memcpy(g_reorderSavedLabel, cur.b, 24);
                            g_reorderHaveSaved = true;
                            PB pb;
                            memcpy(pb.b, &rexit, 8);
                            bool removed = peGuard(rbox, fRm, pb.b) && pb.b[ro] != 0;
                            if (removed)
                            {
                                void* slot = nullptr;
                                int aro = (int)fAdd->GetReturnValueOffset();
                                for (int at = 0; at < 3 && !slot; ++at)
                                {
                                    PB pb2;
                                    memcpy(pb2.b, &rexit, 8);
                                    if (peGuard(rbox, fAdd, pb2.b)) memcpy(&slot, pb2.b + aro, 8);
                                }
                                if (slot)
                                {
                                    UFunction* fSet = fnOf(rtt, L"SetText", "reorder");
                                    if (fSet && (int)fSet->GetParmsSize() == 24) peGuard(rtt, fSet, cur.b);
                                    logf("reorder: 완료 -- 클론이 설정<->나가기 사이로 (Exit 라벨 복원)");
                                }
                                else
                                {   // C-2: Exit 가 떨어진 채다 -- 수리 모드로 틱마다 재부착 시도
                                    g_reorderRepair = true;
                                    logf("FAIL reorder: Exit 재부착 실패(슬롯 null) -- 수리 모드 진입");
                                }
                            }
                        }
                    }
                    // 재정렬 시도가 끝났다(성공/실패 불문) -- 클론을 반드시 보이게
                    setVisibility(clone, 0, "reorderShow");
                }
            }
            }
            if (g_reflFault) { cloneLost("위치 원복 SEH"); return; }
        }
        // v0.50(리뷰 C-2): 재부착 수리 -- RemoveChild(Exit) 성공 후 AddChild 실패로
        // Exit(나가기)가 메뉴에서 떨어진 채면, 붙을 때까지 틱마다 재시도한다.
        // 메뉴가 죽으면 cloneLost 가 리셋하고, 재부착 성공 시 보관해 둔 라벨 복원.
        if (g_reorderRepair)
        {
            UObject* rbox2 = reinterpret_cast<UObject*>(g_doneBox.load());
            UObject* rexit2 = reinterpret_cast<UObject*>(g_menuExit.load(std::memory_order_relaxed));
            if (rbox2 && rexit2)
            {
                UFunction* fAdd2 = fnOf(rbox2, L"AddChild", "reorder.fix");
                if (fAdd2 && (int)fAdd2->GetParmsSize() == 16)
                {
                    PB pb3;
                    memcpy(pb3.b, &rexit2, 8);
                    void* slot2 = nullptr;
                    if (peGuard(rbox2, fAdd2, pb3.b))
                        memcpy(&slot2, pb3.b + (int)fAdd2->GetReturnValueOffset(), 8);
                    if (slot2)
                    {
                        g_reorderRepair = false;
                        if (g_reorderHaveSaved)
                            if (UObject* rtt2 = readObjProp(rexit2, L"TitleText", "reorder.fix"))
                            {
                                UFunction* fSet2 = fnOf(rtt2, L"SetText", "reorder.fix");
                                if (fSet2 && (int)fSet2->GetParmsSize() == 24)
                                    peGuard(rtt2, fSet2, g_reorderSavedLabel);
                            }
                        logf("reorder: 수리 성공 -- 떨어졌던 Exit 재부착 (라벨 복원)");
                    }
                }
            }
            if (g_reflFault) { cloneLost("Exit 수리 SEH"); return; }
        }
        int lt = readGameLangText();
        bool cultureFlip = (lt >= 0 && s_lastLangText != -2 && lt != s_lastLangText);
        bool langChanged = recheckLang(lt);
        if (cultureFlip || langChanged || s_lastLangText == -2)
        {
            setLabel(clone, "keep", false);
            if (g_padIconSpacer)
            {   // v0.50b: 라벨 폭이 언어마다 달라 아이콘 간격도 언어별로 갱신
                float spw = (g_lang == 1) ? 300.0f : 240.0f;
                callBytes(reinterpret_cast<UObject*>(g_padIconSpacer), L"SetWidthOverride", &spw, 4, "padico.sp");
            }
        }
        if (lt >= 0) s_lastLangText = lt;
        else if (s_lastLangText == -2) s_lastLangText = -1;   // 첫 틱 재주장은 1회만
        if (g_reflFault) { cloneLost("라벨 키퍼 SEH"); return; }
    }
    int h = isHoveredSlate(clone, "hover");  // 하이라이트는 Slate hover 와 동기
    if (h < 0 || g_reflFault)
    {
        cloneLost("IsHovered 실패/SEH");
        return;
    }
    if ((h == 1) != g_lastHover)
    {
        g_lastHover = (h == 1);
        setSelectedRow(clone, g_lastHover);
        if (g_lastHover) clearOthers(clone);
        pokeSounds(clone, g_lastHover);
        if (g_reflFault)
        {
            cloneLost("호버 처리 중 SEH");
            return;
        }
    }
    // v0.40 8차: B/ESC 를 놓았으면 미뤄둔 입력모드 복구를 실행한다
    if (g_restoreInputPending && !g_vstop &&   // 10차d: 재진입(예측) 중이면 동결 유지
        !g_padBHeld.load(std::memory_order_relaxed) &&
        !(GetAsyncKeyState(VK_ESCAPE) & 0x8000) &&
        (!g_restoreWaitDir || !g_padDirHeld.load(std::memory_order_relaxed)))
    {
        g_restoreInputPending = false;
        g_restoreWaitDir = false;
        panelInputMode(false);
        logf("inputmode: 지연 복구 실행 (키 놓음)");
    }
    // v0.40 9차: 진입의 정공은 편입(게임이 스스로 클론을 순회). 10차: 실측
    // rootarr num=0(배열 미사용/늦은 초기화)에 대응해 ① 편입을 2초 주기 재시도
    // ② 폴백 = 가상 정지(vstop): 설정<->나가기 경계 전이 순간 클론을 선택 표시
    // + 게임 입력 UIOnly 동결(패널 실증 기성품), 방향/A/B 는 XInput 으로 처리.
    // 포커스 계열(bIsFocusable/내비규칙/SetKeyboardFocus)은 실측 무효 -- 금지.
    {
        static ULONGLONG s_selPollMs = 0;
        static int s_prevIdx = -1;
        static unsigned char s_snap[280];
        static bool s_snapValid = false;
        static int s_snapLogBudget = 40;
        static void* s_snapRoot = nullptr;
        static int s_gen = -1;
        static int s_parentChk = 0;
        static ULONGLONG s_adoptRetryMs = 0;
        static int s_idxBefore = -1;            // 10차e: 직전 선택 (전이 직전 값)
        static ULONGLONG s_idxChangedMs = 0;    // 10차e: 그 전이를 관측한 시각
        if (s_gen != g_menuGen)
        {   // 메뉴 세대 교체(cloneLost) -- 지역 정적 잔존이 오발/허위 diff 를 만든다
            s_gen = g_menuGen;
            s_prevIdx = -1;
            s_snapValid = false;
            s_idxBefore = -1;
            s_idxChangedMs = 0;
        }
        // 선택 스캔: 80ms 폴링과 패드 A 순간 재판정이 공유 (스테일 판정 방지)
        auto scanSel = [&](bool& selOut, int& idxOut)
        {
            selOut = false;
            idxOut = -1;
            for (int si = 0; si < (int)g_siblings.size(); ++si)
            {
                UObject* it = reinterpret_cast<UObject*>(g_siblings[si]);
                if (!it) continue;
                UObject* ovl = readObjProp(it, L"OverlaySelected", "selpoll");
                if (!ovl) continue;
                UFunction* fv = fnOf(ovl, L"GetVisibility", "selpoll");
                if (!fv || (int)fv->GetParmsSize() != 1) continue;
                PB pb;
                if (!peGuard(ovl, fv, pb.b)) continue;
                if (pb.b[0] != 1)   // Collapsed(1) 아님 = 선택됨
                {
                    if (it == (UObject*)clone) selOut = true;
                    else if (idxOut < 0) idxOut = si;
                }
            }
        };
        ULONGLONG navMs = g_lastPadNavMs.load(std::memory_order_relaxed);
        bool padRecent = navMs && (now - navMs) < 400;
        if (now - s_selPollMs >= 16)
        {   // 10차c: 80→16ms -- 게임이 이웃(나가기/설정)을 먼저 칠하고 우리가
            // 나중에 모드매니저로 바꿔 칠하는 "갔다가 돌아오는" 두 단계 이동이
            // 사용자에게 그대로 보였다(라이브 보고). 사실상 매 펌프 틱 폴링으로
            // 중간 단계를 1프레임 수준으로 줄인다. 스캔 비용은 PE 십수 회 = 무해.
            s_selPollMs = now;
            bool sel = false;
            int curIdx = -1;
            scanSel(sel, curIdx);
            // 이중 하이라이트 소등 -- 비편입·비동결 세계 전용(vstop 표시는 우리가 켠 것)
            if (!g_rootAdopted && !g_vstop && sel && curIdx >= 0 && !g_lastHover && !padRecent)
            {
                setSelectedRow(clone, false);
                sel = false;
            }
            if (sel != g_menuCloneSel)
            {
                g_menuCloneSel = sel;
                logf("menu: 클론 선택 %s%s (adopt=%d vstop=%d)", sel ? "ON" : "off",
                     !sel ? "" : padRecent ? " (pad)" : g_lastHover ? " (hover)" : " (?)",
                     (int)g_rootAdopted, (int)g_vstop);
            }
            // 10차d 흡수: 동결이 게임의 입력 처리보다 늦은 레이스 패배면 이웃이
            // 늦게 켜진다 -- 그것이 게임 인덱스의 진실이므로 숨기고 복원 목표 갱신
            if (g_vstop && curIdx >= 0 &&
                g_inputMode.load(std::memory_order_relaxed) == 1)
            {
                void* late = g_siblings[curIdx];
                UObject* lw = reinterpret_cast<UObject*>(late);
                if (UObject* ovl = readObjProp(lw, L"OverlaySelected", "vstop"))
                    setVisibility(ovl, 1, "vstop.late");
                g_vstopGameSel = late;
            }
            // 10차g: 내비 기계가 아직 미특정이면 3초 주기 재탐색 (늦은 생성 대비)
            static ULONGLONG s_navRetryMs = 0;
            if (!g_navObj && g_root && now - s_navRetryMs >= 3000)
            {
                s_navRetryMs = now;
                probeNavGraph(reinterpret_cast<UObject*>(g_root));
            }
            // v0.50e 편입 상시 감시(2초): 게임이 배열을 자기 손으로 재구축하면
            // (설정 왕복 경로, 00:23 실측) 우리 클론이 빠진다 -- 들어있는지 확인하고
            // 없으면 재편입. v0.50c 의 "성공 후 감시 중단"이 설정 왕복 실패의 원인.
            if (g_adoptEnabled && !g_vstop && now - s_adoptRetryMs >= 2000)
            {
                s_adoptRetryMs = now;
                if (cloneInAnyRootArray(clone)) g_rootAdopted = true;
                else
                {
                    UObject* bx = reinterpret_cast<UObject*>(g_doneBox.load());
                    UObject* ex = reinterpret_cast<UObject*>(g_menuExit.load(std::memory_order_relaxed));
                    if (bx && ex)
                    {
                        if (g_rootAdopted)
                            logf("adopt: 배열에서 클론 소실(게임 재구축 추정) -- 재편입");
                        int ad = adoptAndAlign(clone, bx, ex);
                        if (ad < 0) return;
                        g_rootAdopted = (ad == 1);
                    }
                }
            }
            // 고아 감시(≈2.4초): 편입된 클론은 배열 GC 강참조로 "살아있으나 화면 밖"
            // 유령이 가능 -- 부모 상실이면 전체 리셋으로 복구
            if (g_rootAdopted && ++s_parentChk >= 150)   // 10차c: 16ms 틱 기준 ≈2.4초
            {
                s_parentChk = 0;
                UFunction* fp = fnOf(clone, L"GetParent", "orphanChk");
                if (fp && (int)fp->GetParmsSize() == 8)
                {
                    PB pb;
                    void* par = nullptr;
                    if (peGuard(clone, fp, pb.b))
                    {
                        memcpy(&par, pb.b + (int)fp->GetReturnValueOffset(), 8);
                        if (!par)
                        {
                            cloneLost("클론이 박스에서 분리됨 (고아 감시)");
                            return;
                        }
                    }
                }
            }
            // 10차f navdiff: 선택 전이 순간 내비 기계(wnav + 그래프)의 변화 바이트.
            // 선택 상태는 뿌리에 없음이 확정(rootidx 0줄) -- 실제 거처를 여기서 찾는다.
            {
                static unsigned char s_wnavSnap[0x100];
                static unsigned char s_gSnap[2][0x180];
                static bool s_navValid = false;
                static void* s_navTag = nullptr;
                static int s_navDiffBudget = 80;
                if (g_navObj)
                {
                    if (s_navTag != g_navObj)
                    {
                        s_navTag = g_navObj;
                        s_navValid = false;
                    }
                    unsigned char curW2[0x100];
                    unsigned char curG[2][0x180];
                    bool okW = readBytesGuard(g_navObj, 0, curW2, 0x100);
                    bool okG0 = g_navGraphN > 0 && readBytesGuard(g_navGraph[0], 0, curG[0], 0x180);
                    bool okG1 = g_navGraphN > 1 && readBytesGuard(g_navGraph[1], 0, curG[1], 0x180);
                    if (okW)
                    {
                        if (curIdx != s_prevIdx && s_navValid && s_navDiffBudget > 0)
                        {
                            for (int k = 0; k < 0x100 && s_navDiffBudget > 0; ++k)
                                if (curW2[k] != s_wnavSnap[k])
                                {
                                    --s_navDiffBudget;
                                    logf("navdiff: wnav +0x%X %02X->%02X (sel %d->%d)",
                                         k, s_wnavSnap[k], curW2[k], s_prevIdx, curIdx);
                                }
                            if (okG0)
                                for (int k = 0; k < 0x180 && s_navDiffBudget > 0; ++k)
                                    if (curG[0][k] != s_gSnap[0][k])
                                    {
                                        --s_navDiffBudget;
                                        logf("navdiff: g0(k%d) +0x%X %02X->%02X (sel %d->%d)",
                                             g_navGraphKey[0], k, s_gSnap[0][k], curG[0][k], s_prevIdx, curIdx);
                                    }
                            if (okG1)
                                for (int k = 0; k < 0x180 && s_navDiffBudget > 0; ++k)
                                    if (curG[1][k] != s_gSnap[1][k])
                                    {
                                        --s_navDiffBudget;
                                        logf("navdiff: g1(k%d) +0x%X %02X->%02X (sel %d->%d)",
                                             g_navGraphKey[1], k, s_gSnap[1][k], curG[1][k], s_prevIdx, curIdx);
                                    }
                        }
                        memcpy(s_wnavSnap, curW2, 0x100);
                        if (okG0) memcpy(s_gSnap[0], curG[0], 0x180);
                        if (okG1) memcpy(s_gSnap[1], curG[1], 0x180);
                        s_navValid = true;
                    }
                }
            }
            // rootidx 계측: 선택 전이 순간의 뿌리 스냅샷 diff (전이 동기 변화만 후보)
            if (g_root && g_rootSnapOff > 0)
            {
                if (s_snapRoot != g_root)
                {   // 뿌리 교체 -- 옛 뿌리 스냅샷과의 diff 는 전량 허위
                    s_snapRoot = g_root;
                    s_snapValid = false;
                }
                unsigned char cur[280];
                bool rok = readBytesGuard(g_root, g_rootSnapOff, cur, 280);
                static int s_snapHealth = -1;
                if (s_snapHealth != (int)rok)
                {   // 읽기 성패 1회 로그 -- "diff 0줄"이 침묵인지 무변화인지 판별용
                    s_snapHealth = (int)rok;
                    logf("rootsnap: read=%d win=0x%X+280", (int)rok, g_rootSnapOff);
                }
                if (rok)
                {
                    if (curIdx != s_prevIdx && s_snapValid && s_snapLogBudget > 0)
                    {
                        for (int k = 0; k < 280 && s_snapLogBudget > 0; ++k)
                        {
                            if (cur[k] != s_snap[k])
                            {
                                --s_snapLogBudget;
                                logf("rootidx: [+0x%X] %02X->%02X (sel %d->%d)",
                                     g_rootSnapOff + k, s_snap[k], cur[k], s_prevIdx, curIdx);
                            }
                        }
                    }
                    memcpy(s_snap, cur, 280);
                    s_snapValid = true;
                }
            }
            // 전이 처리: menusel 로그 + 가상 정지 진입
            if (curIdx != s_prevIdx)
            {
                s_idxBefore = s_prevIdx;    // 10차e: "누른 시점의 선택" 복원용 이력
                s_idxChangedMs = now;
                static int s_selLogBudget = 30;
                if (s_selLogBudget > 0)
                {
                    --s_selLogBudget;
                    logf("menusel: %d -> %d (pad=%d)", s_prevIdx, curIdx, (int)padRecent);
                }
                void* optW = g_menuOption.load(std::memory_order_relaxed);
                void* extW = g_menuExit.load(std::memory_order_relaxed);
                void* curW = (curIdx >= 0 && curIdx < (int)g_siblings.size()) ? g_siblings[curIdx] : nullptr;
                void* prvW = (s_prevIdx >= 0 && s_prevIdx < (int)g_siblings.size()) ? g_siblings[s_prevIdx] : nullptr;
                bool crossOptExit = curW && prvW && optW && extW &&
                    ((curW == extW && prvW == optW) || (curW == optW && prvW == extW));
                if (g_vstopEnabled && !g_rootAdopted && !g_vstop && crossOptExit && padRecent && !g_panelOpen &&
                    g_inputMode.load(std::memory_order_relaxed) == 1)
                {   // 가상 정지 진입: 게임이 방금 선택한 항목(curW)을 숨기고 클론을
                    // 표시, 게임 입력을 동결한다. 게임 내부 인덱스는 curW 에 머문다 --
                    // 같은 방향으로 나가면 그 자리가 정답이라 정합이 유지된다.
                    // 동결이 실제로 성공했을 때만 진입 -- 실패한 채 진행하면 게임과
                    // 모드가 A/방향을 동시에 받아 이중 실행 사고(리뷰 확정).
                    if (panelInputMode(true, clone))
                    {
                        g_vstop = true;
                        g_vstopGameSel = curW;
                        setSelectedRow(clone, true);
                        clearOthers(clone);
                        pokeSounds(clone, true);
                        // 진입을 유발한 방향 엣지가 같은 틱의 엣지 소비부에서 즉시
                        // 해제를 부르는 레이스(~25%, 리뷰) -- 트리거 엣지를 소거
                        g_padEdges.exchange(0, std::memory_order_relaxed);
                        logf("vstop: 가상 정지 진입 (게임선택 숨김, 입력 동결)");
                    }
                    else logf("WARN vstop: 입력 동결 실패 -- 진입 포기");
                }
            }
            s_prevIdx = curIdx;
        }
        unsigned pe = g_padEdges.exchange(0, std::memory_order_relaxed);
        bool fresh = pe && (now - g_padEdgeMs.load(std::memory_order_relaxed) < 400);
        if (g_vstop)
        {
            if (g_inputMode.load(std::memory_order_relaxed) == 0)
            {   // 마우스/키보드 개입(장치 전환) -- 취소하고 이번 틱은 계속 진행.
                // (패널 열림 중에는 펌프가 훨씬 위에서 반환하므로 여기 못 온다)
                exitVstop(clone, false);
            }
            else
            {
                if (fresh && (pe & PAD_A))
                {   // 게임 선택 오버레이를 먼저 복원해 두고 패널을 연다(스크림 아래라
                    // 시각 무해) -- 안 하면 패널을 닫은 뒤 "하이라이트 없는 메뉴에서
                    // A = 보이지 않는 나가기 실행" 사고(리뷰 critical). 복구 보류를
                    // 미리 무장해 openPanel 실패/SEH 에도 동결이 안 남게 한다 --
                    // 성공 경로는 openPanel 이 보류를 취소하고 UIOnly 를 이어받는다.
                    if (g_vstopGameSel)
                    {
                        UObject* gs = reinterpret_cast<UObject*>(g_vstopGameSel);
                        if (UObject* ovl = readObjProp(gs, L"OverlaySelected", "vstop"))
                            setVisibility(ovl, 4, "vstop.ovl");
                        g_vstopGameSel = nullptr;
                    }
                    g_vstop = false;
                    g_restoreInputPending = true;
                    g_restoreWaitDir = false;
                    logf("menu: 패드 A -- 패널 연다 (가상정지)");
                    g_navSel = -1;
                    openPanel(clone);
                    if (g_reflFault) cloneLost("패널 생성 중 SEH");
                    return;
                }
                if (fresh && (pe & (PAD_UP | PAD_DOWN)))
                {   // 방향 이동 -- 게임 선택(동결 자리)으로 복귀. 같은 방향 진행이면
                    // 그 자리가 다음 정답이라 자연 정합. 역방향은 화면이 한 칸
                    // 되돌아가 보이지만(동결 자리 표시) 화면=게임 인덱스 일치가
                    // 유지돼 A 오발이 원리적으로 없다 -- 안전 우선. 다음 전이에서
                    // 재진입으로 수렴. 홀드 스크롤은 경계에서 한 번 멈춘다(의도).
                    exitVstop(clone, true);
                    return;
                }
                if (escEdge)
                {   // 패드 B/키보드 ESC -- 취소 복귀. 래치는 펌프 선두가 이번 틱에
                    // 소비한 값(escEdge)을 쓴다 -- 여기서 재소비하면 항상 0(리뷰).
                    exitVstop(clone, false);
                    return;
                }
                return;   // 동결 중에는 이하 일반 처리 건너뜀
            }
        }
        // 10차d 예측 진입(이벤트 구동): 경계에서의 방향 엣지는 XInput(5ms 래치)이
        // 게임보다 먼저 본다. 관측(폴링)을 기다리지 않고 즉시 동결+클론 표시 --
        // "이웃을 갔다가 돌아오는" 두 단계와 왕복 시 복구 대기 지연을 함께 제거.
        // 직전 해제의 동결(복구 보류)이 살아 있으면 그대로 재진입 = 게임 무관여.
        // 게임이 이미 입력을 처리한 레이스 패배는 위 흡수부가 정합시킨다.
        if (g_vstopEnabled && !g_vstop && !g_rootAdopted && !g_panelOpen && fresh &&
            (pe & (PAD_UP | PAD_DOWN)) &&
            g_inputMode.load(std::memory_order_relaxed) == 1)
        {
            void* optW = g_menuOption.load(std::memory_order_relaxed);
            void* extW = g_menuExit.load(std::memory_order_relaxed);
            // 10차e: s_prevIdx 는 이미 "이 입력의 결과"를 반영했을 수 있다(게임
            // 프레임 16ms < 엣지 소비 33ms). 선택 변경이 엣지 래치 시각 이후에
            // 관측됐다면 누른 시점의 값(s_idxBefore)으로 판정한다 -- 안 그러면
            // 불러오기+↓ 가 "설정+↓" 로 오인돼 한 입력에 두 칸 이동(라이브 실측).
            ULONGLONG pressMs = g_padEdgeMs.load(std::memory_order_relaxed);
            int selIdx = s_prevIdx;
            if (s_idxChangedMs && pressMs && s_idxChangedMs >= pressMs)
                selIdx = s_idxBefore;
            void* selW = (selIdx >= 0 && selIdx < (int)g_siblings.size())
                             ? g_siblings[selIdx] : nullptr;
            bool down = (pe & PAD_DOWN) != 0;
            if (selW && optW && extW &&
                ((selW == optW && down) || (selW == extW && !down)))
            {
                bool frozen = g_restoreInputPending;   // 직전 해제의 동결이 아직 유효
                if (frozen || panelInputMode(true, clone))
                {
                    g_restoreInputPending = false;   // 재진입 -- 보류 복구 무효
                    g_restoreWaitDir = false;
                    g_vstop = true;
                    g_vstopGameSel = selW;   // 동결이 선점했으면 게임 인덱스는 여기 머문다
                    setSelectedRow(clone, true);
                    clearOthers(clone);
                    pokeSounds(clone, true);
                    g_padEdges.exchange(0, std::memory_order_relaxed);
                    logf("vstop: 예측 진입 (%s%s)", down ? "설정+아래" : "나가기+위",
                         frozen ? ", 동결 연장" : "");
                    return;
                }
            }
        }
        // v0.50b: 게임 네이티브 확정(클론 선택 + A, 또는 클릭) -- OnClickedButton 편승
        {
            unsigned long long ck = g_cloneClickedMs.exchange(0, std::memory_order_relaxed);
            if (ck && now - ck < 600)
            {
                logf("menu: 클론 확정(OnClickedButton) -- 패널 연다");
                g_navSel = -1;
                openPanel(clone);
                if (g_reflFault) cloneLost("패널 생성 중 SEH");
                return;
            }
        }
        if (fresh && (pe & PAD_Y))
        {   // 10차h: 패드 Y = 모드매니저 바로 열기. 메뉴 순회 편입은 다섯 구조 전부
            // 실측 배제(STATUS) -- 게임과 경합하지 않는 전용 버튼이 정식 패드 경로다.
            logf("menu: 패드 Y -- 패널 연다 (전용 버튼)");
            g_navSel = -1;
            openPanel(clone);
            if (g_reflFault) cloneLost("패널 생성 중 SEH");
            return;
        }
        if (fresh && (pe & PAD_A))
        {   // A 는 즉석 재판정 -- 최대 80ms 묵은 폴링값은 이중 발동/무반응을 만든다
            bool selNow = false;
            int idxNow = -1;
            scanSel(selNow, idxNow);
            g_menuCloneSel = selNow;
            if (selNow || g_lastHover)
            {
                logf("menu: 패드 A -- 패널 연다 (%s)", selNow ? "클론선택" : "호버");
                g_navSel = -1;
                openPanel(clone);
                if (g_reflFault) cloneLost("패널 생성 중 SEH");
                return;
            }
        }
    }
    if (lmbEdge)
    {
        // 클릭 순간에는 stale 한 Slate hover 대신 좌표 산술로 다시 판정한다
        sampleMouse();
        int hitNow = isHovered(clone, "menu-hit");
        if (hitNow == 1 || (hitNow < 0 && h == 1))
        {
            logf("menu: 클릭 감지 -- 패널 연다 (판정=%s)", hitNow == 1 ? "좌표" : "hover");
            g_navSel = -1;   // 새로 열기 = 선택 없음
            openPanel(clone);
            if (g_reflFault) cloneLost("패널 생성 중 SEH");
        }
    }
}

// ======================= 삽입 본체 =========================================

// ======================= v0.40 9차: 게임 항목 배열 편입 ====================
// 조사 확정(STATUS 9차 절): 타이틀 메뉴의 패드 선택은 Slate 포커스가 아니라
// 뿌리 UDLayerTitle(BP: DLayerTitleGame_C)이 내부 인덱스로 항목 목록
// ListTitleMenuBtn(TArray @리플렉션 해석)을 순회하는 구조다. 클론이 그 목록에
// 없으면 영원히 건너뛴다. 포커스 계열(bIsFocusable/내비규칙/SetKeyboardFocus)은
// 전부 실측 무효 -- 재시도 금지.

static void memSelfTest()
{
    if (g_memSelfTest != 0) return;
    void* p = nullptr;
    if (sehEngineMalloc(64, &p) == 0 && p)
    {
        void* pat = (void*)(unsigned long long)0x11223344AABBCCDDull;
        void* r = nullptr;
        if (writePtrGuard(p, 0, pat) && readPtrGuard(p, 0, &r) && r == pat &&
            sehEngineFree(p) == 0)
        {
            g_memSelfTest = 1;
            logf("mem: FMemory 왕복 자가시험 OK");
            return;
        }
    }
    g_memSelfTest = -1;
    logf("WARN mem: FMemory 자가시험 실패 -- 재할당 영구 봉인 (편입은 Num<Max 일 때만)");
}

// BP 그래프가 선택을 몰면 인덱스는 리플렉션 가시 BP 변수다 -- 이름 탐침(1회 로그).
// fnOf/propOffset 대신 seh* 직접 사용: 없는 이름이 정상이라 WARN/폴트를 내면 안 된다.
static void probeRootVars(UObject* root)
{
    static const wchar_t* varNames[] = {
        L"CurrentIndex", L"SelectedIndex", L"MenuIndex", L"SelectIndex", L"CurIndex",
        L"NowIndex", L"CurrentMenuIndex", L"SelectedMenuIndex", L"FocusIndex",
        L"SelectedIdx", L"CursorIndex", L"SelectCursor", L"CurrentSelect", L"MenuCursor",
    };
    for (const wchar_t* n : varNames)
    {
        FProperty* pr = nullptr;
        if (sehGetProp(root, n, &pr) != 0 || !pr) continue;
        int off = 0, sz = 0;
        if (sehPropOffSize(pr, &off, &sz) != 0) continue;
        logf("rootvar: %s off=0x%X size=%d", u8(n).c_str(), off, sz);
    }
    static const wchar_t* fnNames[] = {
        L"SelectMenu", L"SetMenuIndex", L"MoveSelection", L"SetSelectMenu", L"SelectTitleMenu",
        L"SetCurrentIndex", L"ChangeMenu", L"MoveMenu", L"NextMenu", L"PrevMenu",
        L"SetSelect", L"OnSelectMenu", L"RefreshMenu", L"UpdateMenuSelect",
    };
    for (const wchar_t* n : fnNames)
    {
        UFunction* fn = nullptr;
        if (sehGetFn(root, n, &fn) != 0 || !fn) continue;
        logf("rootfn: %s parms=%d", u8(n).c_str(), (int)fn->GetParmsSize());
    }
}

// v0.40 10차g: 게임의 방향 이동 기계 계측 v2. 뿌리의 ParentPanel 은 null 실측 --
// DWidgetNavigation/DWidgetGraph 는 네이티브 클래스라 FindAllOf 로 직접 열거한다.
// DWidgetGraph 는 리플렉션 0개 = 노드가 비-UObject 내부 구조 -> 메모리 스캔으로
// 타이틀 항목 포인터(g_siblings)를 찾아 "타이틀 선택기 그래프"를 특정한다.

static void dumpGraphHex(void* g, int gi, int key)
{
    unsigned char buf[0x180];
    if (!readBytesGuard(g, 0, buf, 0x180))
    {
        logf("navhex: g%d 읽기 실패", gi);
        return;
    }
    for (int off = 0; off < 0x180; off += 48)
    {
        char hex[48 * 2 + 4];
        int p = 0;
        for (int b = 0; b < 48; ++b) p += snprintf(hex + p, sizeof(hex) - p, "%02X", buf[off + b]);
        logf("navhex: g%d(k%d) +0x%03X %s", gi, key, off, hex);
    }
}

// 그래프 메모리에서 타이틀 항목 포인터 검색. 반환 = 히트 수.
static int scanGraphForItems(void* g, int gi, int key)
{
    unsigned char buf[0x400];
    if (!readBytesGuard(g, 0, buf, 0x400)) return 0;
    int hits = 0;
    for (int off = 0; off + 8 <= 0x400; off += 8)
    {
        void* v = nullptr;
        memcpy(&v, buf + off, 8);
        if (!v) continue;
        for (int si = 0; si < (int)g_siblings.size(); ++si)
            if (v == g_siblings[si])
            {
                ++hits;
                logf("navhit: g%d(k%d) +0x%X = item[%d]", gi, key, off, si);
            }
    }
    if (!hits)
    {   // 1단계 간접: 앞 0xC0 의 포인터를 따라가 0x100 씩 훑는다 (노드 컨테이너 추정)
        for (int off = 0; off + 8 <= 0xC0 && hits <= 12; off += 8)
        {
            void* p = nullptr;
            memcpy(&p, buf + off, 8);
            if (!p || ((unsigned long long)p & 0x7) != 0 || (unsigned long long)p < 0x10000) continue;
            unsigned char sub[0x100];
            if (!readBytesGuard(p, 0, sub, 0x100)) continue;
            for (int so = 0; so + 8 <= 0x100; so += 8)
            {
                void* v = nullptr;
                memcpy(&v, sub + so, 8);
                if (!v) continue;
                for (int si = 0; si < (int)g_siblings.size(); ++si)
                    if (v == g_siblings[si])
                    {
                        ++hits;
                        logf("navhit2: g%d(k%d) +0x%X -> +0x%X = item[%d]", gi, key, off, so, si);
                    }
            }
        }
    }
    return hits;
}

static void probeNavGraph(UObject* root)
{
    (void)root;
    g_navObj = nullptr;
    g_navGraphN = 0;
    struct Cand { void* g; int key; void* owner; };
    Cand cands[16];
    int candN = 0;
    // 1) DWidgetNavigation 인스턴스들의 RoutingTable 에서 그래프 수집
    std::vector<UObject*> wnavs;
    UOG::FindAllOf(L"DWidgetNavigation", wnavs);
    int wi = 0;
    int rtOff = -1;
    for (UObject* w : wnavs)
    {
        if (!w) continue;
        std::wstring full = w->GetFullName(nullptr);
        if (wcontains(full, L"Default__")) continue;
        if (rtOff < 0)
        {
            rtOff = propOffset(w, L"RoutingTable", 80, "navg");
            if (rtOff < 0) break;
        }
        struct { void* data; int num; int max; } arr{};
        if (!readBytesGuard(w, rtOff, &arr, 16)) continue;
        logf("navg: wnav[%d]=%p rt num=%d max=%d", wi, (void*)w, arr.num, arr.max);
        if (arr.data && arr.num > 0 && arr.num <= 8)
        {
            for (int k = 0; k < arr.num && candN < 16; ++k)
            {   // TMap 원소 = TSetElement<TPair<uint8,ptr>> 추정: stride 24, key@0, val@8
                unsigned char key = 255;
                void* gp = nullptr;
                if (!readByteGuard(arr.data, k * 24, &key)) continue;
                if (!readPtrGuard(arr.data, k * 24 + 8, &gp)) continue;
                if (!gp) continue;
                logf("navmap: wnav[%d][%d] key=%d graph=%p", wi, k, (int)key, gp);
                cands[candN].g = gp;
                cands[candN].key = (int)key;
                cands[candN].owner = (void*)w;
                ++candN;
            }
        }
        if (++wi >= 8) break;
    }
    // 2) DWidgetGraph 직접 열거 (RoutingTable 에 안 걸린 것 포함)
    std::vector<UObject*> graphs;
    UOG::FindAllOf(L"DWidgetGraph", graphs);
    for (UObject* g : graphs)
    {
        if (!g || candN >= 16) continue;
        std::wstring full = g->GetFullName(nullptr);
        if (wcontains(full, L"Default__")) continue;
        bool known = false;
        for (int k = 0; k < candN; ++k)
            if (cands[k].g == (void*)g) known = true;
        if (known) continue;
        cands[candN].g = (void*)g;
        cands[candN].key = -1;
        cands[candN].owner = nullptr;
        ++candN;
    }
    logf("navg: wnav %d개, 그래프 후보 %d개 (전체 열거 %d)", wi, candN, (int)graphs.size());
    // 3) 스캔 -- 히트 그래프 우선 등록
    int hitCount[16] = {};
    for (int k = 0; k < candN; ++k)
        hitCount[k] = scanGraphForItems(cands[k].g, k, cands[k].key);
    for (int pass = 0; pass < 2; ++pass)
        for (int k = 0; k < candN && g_navGraphN < 4; ++k)
        {
            bool want = (pass == 0) ? hitCount[k] > 0 : hitCount[k] == 0;
            if (!want) continue;
            g_navGraph[g_navGraphN] = cands[k].g;
            g_navGraphKey[g_navGraphN] = cands[k].key;
            ++g_navGraphN;
            if (!g_navObj && cands[k].owner) g_navObj = cands[k].owner;
        }
    if (!g_navObj && g_navGraphN > 0) g_navObj = g_navGraph[0];
    // 4) 히트 그래프 헥사 (없으면 앞 2개라도)
    int dumped = 0;
    for (int k = 0; k < candN && dumped < 3; ++k)
        if (hitCount[k] > 0)
        {
            dumpGraphHex(cands[k].g, k, cands[k].key);
            ++dumped;
        }
    if (!dumped)
        for (int k = 0; k < candN && dumped < 2; ++k)
        {
            dumpGraphHex(cands[k].g, k, cands[k].key);
            ++dumped;
        }
}

static bool ensureRoot()
{
    if (g_root && g_rootArrOff >= 0) return true;
    std::vector<UObject*> found;
    UOG::FindAllOf(L"DLayerTitleGame_C", found);
    // v0.50d: 다중 인스턴스 대비 -- 뿌리의 VerticalBox_BotButtons 가 우리가 쥔
    // 메뉴 박스(g_doneBox = Exit.GetParent())와 일치하는 인스턴스가 진짜 라이브다.
    // (항목 포인터는 부팅 경로에선 끝내 null 실측 -- 검증 기준으로 못 쓴다)
    void* liveBox = g_doneBox.load();
    UObject* cand = nullptr;
    UObject* first = nullptr;
    for (UObject* o : found)
    {
        if (!o) continue;
        std::wstring full = o->GetFullName(nullptr);
        if (wcontains(full, L"Default__")) continue;
        if (!first) first = o;
        if (liveBox && readObjProp(o, L"VerticalBox_BotButtons", "root") == (UObject*)liveBox)
        {
            cand = o;
            break;
        }
    }
    bool boxMatched = cand != nullptr;
    if (!cand) cand = first;
    if (!cand)
    {
        logf("WARN root: DLayerTitleGame_C 라이브 인스턴스 없음");
        return false;
    }
    logf("root: 후보 %d개, 박스일치=%d", (int)found.size(), (int)boxMatched);
    int off = propOffset(cand, L"ListTitleMenuBtn", 16, "root");
    if (off < 0)
    {
        logf("WARN root: ListTitleMenuBtn 해석 실패 -- 편입 불가");
        return false;
    }
    int la = propOffset(cand, L"Load_Anim", 8, "root.snap");
    g_root = (void*)cand;
    g_rootArrOff = off;
    g_rootSnapOff = (la > 0) ? la + 8 : off - 0x20;
    logf("root: %s arrOff=0x%X snapOff=0x%X", u8(cand->GetName()).c_str(), off, g_rootSnapOff);
    probeRootVars(cand);
    probeNavGraph(cand);   // 10차f: 내비 기계(그래프) 계측
    return true;
}

// v0.50e: 한 뿌리에 대한 편입 시도. 반환 1=클론이 배열에 있음(이번에 넣었든
// 원래 있었든) / 0=실패·불가. 배열이 비었으면 부트스트랩(항목 4개는 FindAllOf
// 라이브 목록에서 이름 식별 -- 뿌리 바인딩 프로퍼티는 부팅 경로에서 끝내 null 실측),
// 채워져 있으면 라이브 exitW 검증 후 append. 로그에 뿌리 전체 경로를 남긴다
// (다중 인스턴스 중 어느 쪽이 라이브인지 식별할 유일한 단서).
static int adoptIntoRoot(UObject* root, int arrOff, UObject* clone, UObject* liveExit, int ri)
{
    struct ArrHdr { void* data; int num; int max; };
    ArrHdr arr{};
    if (!readBytesGuard(root, arrOff, &arr, 16)) return 0;
    // 1) 이미 들어 있나 (조용)
    if (arr.data && arr.num > 0 && arr.num <= 16)
        for (int jj = 0; jj < arr.num; ++jj)
        {
            void* e = nullptr;
            if (readPtrGuard(arr.data, jj * 8, &e) && e == (void*)clone) return 1;
        }
    // 2) 빈 배열 -> 부트스트랩
    if (!arr.data && arr.num == 0 && arr.max == 0)
    {
        memSelfTest();
        if (g_memSelfTest != 1) return 0;
        void* items[4] = {};
        for (void* sPtr : g_siblings)
        {
            UObject* it = reinterpret_cast<UObject*>(sPtr);
            if (!it || it == clone) continue;
            std::wstring nm = it->GetName();
            if (wcontains(nm, L"PlayGame")) items[0] = (void*)it;
            else if (wcontains(nm, L"LoadGame")) items[1] = (void*)it;
            else if (wcontains(nm, L"Option")) items[2] = (void*)it;
            else if (wcontains(nm, L"Exit")) items[3] = (void*)it;
        }
        if (!(items[0] && items[1] && items[2] && items[3]) || items[3] != (void*)liveExit)
        {
            logf("WARN bootstrap[r%d]: 항목 확보 실패 -- 포기", ri);
            return 0;
        }
        void* nb = nullptr;
        if (sehEngineMalloc(5 * 8, &nb) != 0 || !nb) return 0;
        bool ok = true;
        for (int k = 0; k < 4 && ok; ++k) ok = writePtrGuard(nb, k * 8, items[k]);
        if (ok) ok = writePtrGuard(nb, 4 * 8, (void*)clone);
        if (!ok) { sehEngineFree(nb); return 0; }
        if (!writePtrGuard(root, arrOff, nb)) { sehEngineFree(nb); return 0; }
        if (!writeIntGuard(root, arrOff + 8, 5) || !writeIntGuard(root, arrOff + 12, 5))
        {
            writeIntGuard(root, arrOff + 8, 0);
            return 0;
        }
        logf("bootstrap[r%d]: 배열 생성 0 -> 5  root=%s", ri, u8(root->GetFullName(nullptr)).c_str());
        return 1;
    }
    // 3) 게임이 채운 배열 -> 검증 후 append
    if (arr.data && arr.num >= 1 && arr.num <= 16 && arr.max >= arr.num && arr.max <= 64)
    {
        bool liveIn = false;
        for (int jj = 0; jj < arr.num; ++jj)
        {
            void* e = nullptr;
            if (readPtrGuard(arr.data, jj * 8, &e) && e == (void*)liveExit) liveIn = true;
        }
        if (!liveIn)
        {
            static int s_stale = 0;
            if (++s_stale <= 3) logf("WARN adopt[r%d]: 배열에 라이브 Exit 없음 -- 스테일 뿌리", ri);
            return 0;
        }
        if (arr.num < arr.max)
        {
            if (!writePtrGuard(arr.data, arr.num * 8, (void*)clone) ||
                !writeIntGuard(root, arrOff + 8, arr.num + 1))
                return 0;
            logf("append[r%d]: num %d->%d  root=%s", ri, arr.num, arr.num + 1,
                 u8(root->GetFullName(nullptr)).c_str());
            return 1;
        }
        memSelfTest();
        if (g_memSelfTest != 1) return 0;
        int newMax = arr.max + 4;
        void* nb = nullptr;
        if (sehEngineMalloc((unsigned long long)newMax * 8, &nb) != 0 || !nb) return 0;
        bool ok = true;
        for (int jj = 0; jj < arr.num && ok; ++jj)
        {
            void* e = nullptr;
            ok = readPtrGuard(arr.data, jj * 8, &e) && writePtrGuard(nb, jj * 8, e);
        }
        if (ok) ok = writePtrGuard(nb, arr.num * 8, (void*)clone);
        if (!ok) { sehEngineFree(nb); return 0; }
        if (!writePtrGuard(root, arrOff, nb)) { sehEngineFree(nb); return 0; }
        if (!writeIntGuard(root, arrOff + 8, arr.num + 1) ||
            !writeIntGuard(root, arrOff + 12, newMax))
        {
            writeIntGuard(root, arrOff + 8, arr.num);
            writeIntGuard(root, arrOff + 12, newMax);
            return 0;
        }
        sehEngineFree(arr.data);
        logf("append[r%d]: num %d->%d realloc  root=%s", ri, arr.num, arr.num + 1,
             u8(root->GetFullName(nullptr)).c_str());
        return 1;
    }
    return 0;
}

// v0.50e: 라이브 DLayerTitleGame_C **전부**에 편입한다. 어느 인스턴스가 진짜
// 순회 주체인지 특정 불가(후보 2개, 바인딩 전부 null 실측) -- 전부 커버가 답.
static bool tryAdoptClone(UObject* clone, UObject* liveExit)
{
    if (!ensureRoot()) return false;      // g_rootArrOff(클래스 오프셋)·계측용 g_root 확보
    if (g_rootArrOff < 0) return false;
    std::vector<UObject*> roots;
    UOG::FindAllOf(L"DLayerTitleGame_C", roots);
    bool any = false;
    int ri = 0;
    for (UObject* r : roots)
    {
        if (!r) continue;
        std::wstring full = r->GetFullName(nullptr);
        if (wcontains(full, L"Default__")) continue;
        if (adoptIntoRoot(r, g_rootArrOff, clone, liveExit, ri) == 1) any = true;
        ++ri;
    }
    return any;
}

// v0.50e: 상시 감시용 -- 클론이 어느 라이브 뿌리의 배열에든 들어 있는가 (무로그).
static bool cloneInAnyRootArray(UObject* clone)
{
    if (g_rootArrOff < 0) return false;
    std::vector<UObject*> roots;
    UOG::FindAllOf(L"DLayerTitleGame_C", roots);
    for (UObject* r : roots)
    {
        if (!r) continue;
        std::wstring full = r->GetFullName(nullptr);
        if (wcontains(full, L"Default__")) continue;
        struct { void* data; int num; int max; } a{};
        if (!readBytesGuard(r, g_rootArrOff, &a, 16)) continue;
        if (!a.data || a.num <= 0 || a.num > 16) continue;
        for (int jj = 0; jj < a.num; ++jj)
        {
            void* e = nullptr;
            if (readPtrGuard(a.data, jj * 8, &e) && e == (void*)clone) return true;
        }
    }
    return false;
}

// 편입 성공 시 클론을 수직박스 맨 아래로 -- 배열 순서 [..., Exit, 클론] 과 화면
// 순서를 일치시킨다. 어긋나면 하이라이트가 건너뛰고 역행해 '나가기' 오발 사고
// (반박 검증에서 시뮬레이션으로 확정된 시나리오).
// 반환 3상: 1=성공 / 0=떼기 실패(클론은 제자리, 편입만 철회하면 됨) /
//          -1=붙이기 실패(클론이 박스에서 떨어진 유령 -- 호출부가 전체 리셋)
static int moveCloneBottom(UObject* box, UObject* clone)
{
    UFunction* fnAdd = fnOf(box, L"AddChild", "reorder2");
    UFunction* fnRm = fnOf(box, L"RemoveChild", "reorder2");
    if (!fnAdd || !fnRm || (int)fnAdd->GetParmsSize() != 16) return 0;
    int ps = (int)fnRm->GetParmsSize();
    int ro = (int)fnRm->GetReturnValueOffset();
    if (!(ps > 8 && ps <= 64 && ro >= 8 && ro < 64)) return 0;
    PB pb;
    memcpy(pb.b, &clone, 8);
    if (!peGuard(box, fnRm, pb.b) || pb.b[ro] == 0) return 0;
    void* slot = nullptr;
    for (int attempt = 0; attempt < 2 && !slot; ++attempt)
    {   // AddChild 실패 = 클론 분리 유령 -- 1회 재시도(리뷰 확정 구멍)
        PB pb2;
        memcpy(pb2.b, &clone, 8);
        if (peGuard(box, fnAdd, pb2.b))
            memcpy(&slot, pb2.b + (int)fnAdd->GetReturnValueOffset(), 8);
    }
    return slot ? 1 : -1;
}

// v0.40 10차: 편입 + 순서 정합 일괄 (tryInject 와 폴링 재시도가 공유).
// 반환 1=편입·정렬 완료 / 0=미편입(무손상 롤백) / -1=cloneLost 로 전체 리셋됨.
static int adoptAndAlign(UObject* clone, UObject* box, UObject* exitW)
{
    if (!tryAdoptClone(clone, exitW)) return 0;
    int mv = moveCloneBottom(box, clone);
    if (mv == 1)
    {
        logf("adopt: 편입 완료 -- 클론 위치 = 맨 아래 (배열 순서 정합)");
        return 1;
    }
    if (mv == 0)
    {   // 클론은 제자리(나가기 위) -- 편입만 철회하면 비편입 세계로 무손상 복귀
        struct { void* data; int num; int max; } a2{};
        if (readBytesGuard(g_root, g_rootArrOff, &a2, 16) && a2.num > 0 && a2.data)
        {
            void* last = nullptr;
            if (readPtrGuard(a2.data, (a2.num - 1) * 8, &last) && last == (void*)clone)
                writeIntGuard(g_root, g_rootArrOff + 8, a2.num - 1);
        }
        logf("WARN adopt: 재정렬 불가(클론 무사) -- 편입 철회 (Num 원복)");
        return 0;
    }
    logf("WARN adopt: 재정렬 중 클론 분리 -- 전체 리셋 후 재삽입");
    cloneLost("편입 재정렬 분리");
    return -1;
}

static void tryInject()
{
    ++g_attempts;
    if (!g_sbl) g_sbl = findObj(L"/Script/UMG.Default__SlateBlueprintLibrary", "hit.sbl");
    if (!g_wll) g_wll = findObj(L"/Script/UMG.Default__WidgetLayoutLibrary", "hit.wll");

    // 1) 런타임 메뉴 항목 수집
    std::vector<UObject*> all;
    UOG::FindAllOf(L"DUWG_TitleMenu_C", all);
    if (all.empty())
    {
        softRetry("DUWG_TitleMenu_C 인스턴스 0개");
        return;
    }

    UObject* exitW = nullptr;
    UObject* sample = nullptr;  // Option 항목 (클론 원형)
    int runtimeCount = 0;
    bool clonePresent = false;
    void* myClone = g_myClone.load();
    std::vector<void*> runtimeEntries;
    for (UObject* o : all)
    {
        if (!o) continue;
        if (myClone && o == reinterpret_cast<UObject*>(myClone)) clonePresent = true;
        std::wstring full = o->GetFullName(nullptr);
        if (wcontains(full, L"/Game/") || wcontains(full, L"Default__")) continue;  // 템플릿/CDO 제외
        ++runtimeCount;
        runtimeEntries.push_back((void*)o);
        std::wstring name = o->GetName();
        if (!exitW && wcontains(name, L"Exit")) exitW = o;
        if (!sample && wcontains(name, L"Option")) sample = o;
    }
    g_siblings = runtimeEntries;  // clearOthers 캐시 (게임 스레드 전용)
    {   // v0.40 진단(1회): menusel 인덱스가 어느 항목인지 해석할 이름표
        static bool s_menuDumped = false;
        if (!s_menuDumped)
        {
            s_menuDumped = true;
            for (int si = 0; si < (int)g_siblings.size(); ++si)
                logf("menuitem[%d]=%s", si,
                     u8(reinterpret_cast<UObject*>(g_siblings[si])->GetName()).c_str());
        }
    }

    // 재삽입 판정: 박스 주소 비교는 할당자 주소 재사용(ABA)에 속는다(리뷰 확정).
    // 대신 "내 클론이 방금 가져온 살아있는 목록에 있는가"로 판정한다 -- 역참조 없음.
    if (clonePresent) return;  // 삽입돼 있고 살아 있음
    if (myClone) cloneLost("메뉴 재생성/재구축 -- 재삽입 예정");

    if (!exitW || runtimeCount < 4)
    {
        char why[128];
        snprintf(why, sizeof(why), "런타임 항목 %d개, Exit=%d -- 메뉴 미완성", runtimeCount, exitW ? 1 : 0);
        softRetry(why);
        return;
    }
    if (!sample) sample = exitW;  // Lua: (sample or exitW)

    // 2) 컨테이너 = Exit 의 GetParent()
    UFunction* fnParent = fnOf(exitW, L"GetParent", "getParent");
    if (!fnParent)
    {
        anchorFail("GetParent UFunction 없음");
        return;
    }
    if (!parmsExact(fnParent, 8, "GetParent")) return;
    PB pbP;
    if (!peGuard(exitW, fnParent, pbP.b))
    {
        logf("FAIL GetParent: SEH 예외");
        g_hardFail = true;
        return;
    }
    void* boxRaw = nullptr;
    memcpy(&boxRaw, pbP.b + (int)fnParent->GetReturnValueOffset(), 8);
    UObject* box = reinterpret_cast<UObject*>(boxRaw);
    if (!box)
    {
        softRetry("GetParent()=null");
        return;
    }

    // v0.40(pad): 워프 판정용 이웃(설정/나가기)과 선택 이벤트 함수 캐시
    g_menuOption.store((void*)sample, std::memory_order_relaxed);
    g_menuExit.store((void*)exitW, std::memory_order_relaxed);
    if (!g_fnSelChanged.load(std::memory_order_relaxed))
        g_fnSelChanged.store((void*)fnOf(exitW, L"BP_OnItemSelectionChanged", "selhook"),
                             std::memory_order_relaxed);

    ensureLang();   // v0.40: 메뉴 항목 라벨 언어를 먼저 확정한다
    logf("inject: 시작 -- 런타임 항목 %d개, exit=%s, box=%p",
         runtimeCount, u8(exitW->GetName()).c_str(), (void*)box);

    // 4) 클론 원형 클래스 = 라이브 인스턴스의 클래스 (에셋 경로 하드코딩 금지)
    UClass* wcls = sample->GetClassPrivate();
    if (!wcls)
    {
        anchorFail("GetClassPrivate()=null");
        return;
    }

    // 5) PlayerController (타이틀에도 DsTPCTR_C 존재 실측 -- world_census.txt)
    UObject* pc = UOG::FindFirstOf(L"PlayerController");
    logf("inject: pc=%p%s", (void*)pc, pc ? "" : " (null -- WorldContext 는 형제 위젯으로 대체)");
    g_pcPanel = (void*)pc;   // v0.40 4차: 메뉴 가상 포커스의 입력모드 전환용

    // 6) WidgetBlueprintLibrary CDO -> Create(ctx, wcls, pc)
    UObject* lib = UOG::StaticFindObject_InternalSlow(
        nullptr, nullptr, L"/Script/UMG.Default__WidgetBlueprintLibrary", false);
    if (!lib)
    {
        logf("FAIL StaticFindObject: WidgetBlueprintLibrary CDO 없음");
        g_hardFail = true;
        return;
    }
    UFunction* fnCreate = fnOf(lib, L"Create", "create");
    if (!fnCreate)
    {
        anchorFail("Create UFunction 없음");
        return;
    }
    if (!parmsExact(fnCreate, 32, "Create")) return;

    UObject* ctxObj = pc ? pc : sample;  // umg.lua 본선 = pc, 구판 폴백 = 형제 위젯
    PB pbC;
    memcpy(pbC.b + 0, &ctxObj, 8);
    memcpy(pbC.b + 8, &wcls, 8);
    memcpy(pbC.b + 16, &pc, 8);
    if (!peGuard(lib, fnCreate, pbC.b))
    {
        logf("FAIL Create: SEH 예외");
        g_hardFail = true;
        return;
    }
    void* cloneRaw = nullptr;
    memcpy(&cloneRaw, pbC.b + (int)fnCreate->GetReturnValueOffset(), 8);
    UObject* clone = reinterpret_cast<UObject*>(cloneRaw);
    if (!clone)
    {
        anchorFail("Create 반환 null");
        return;
    }
    logf("OK Create: clone=%p (%s)", (void*)clone, u8(clone->GetName()).c_str());
    g_anchorFails = 0;  // 준비 단계 통과

    // 7-11) 무해화/부착/마감 -- 실패 시 고아 클론이 남으므로 횟수를 센다.
    //       (700ms 스로틀로 무한 재시도되면 위젯이 누적되는 것을 막는 안전판)
    if (finishClone(box, exitW, sample, clone))
    {
        g_cloneOrphans = 0;
        g_doneBox = (void*)box;
        g_myClone = (void*)clone;  // 이후 생존 확인은 FindAllOf 멤버십으로만
        // 지금 이 순간 = cls==1 UI 함수 실행 중 = 게임 스레드. 펌프의 스레드
        // 판정 기준으로 캡처한다 (IsInGameThread 는 이 설치본에서 못 믿는다).
        g_gameThreadId.store(GetCurrentThreadId(), std::memory_order_relaxed);
        g_lastHover = false;
        g_siblings.push_back((void*)clone);
        // v0.40 9차: 게임 항목 배열 편입. 성공하면 시각 순서도 배열과 일치시키고,
        // 시각 재정렬이 실패하면 편입을 철회한다(순서 불일치 = 나가기 오발 위험).
        if (g_adoptEnabled)
        {   // 10차: 배열이 비어 있으면(실측 num=0) 여기선 미편입으로 남고,
            // 폴링이 2초 주기로 재시도한다. 폴백 = 가상 정지(vstop).
            int ad = adoptAndAlign(clone, box, exitW);
            if (ad < 0) return;
            g_rootAdopted = (ad == 1);
        }
        wireNav(clone, sample, exitW);   // v0.40 5차: 게임 내비 링에 끼우기 (무해 -- 유지)
        startPulseTimer(clone);  // v0.7: 20ms 게임스레드 펄스 (입력 반응성)
        // v0.9: 좌표 히트테스트 준비 상태 1회 진단
        sampleMouse();
        logf("hit: sbl=%p wll=%p mouse=%s(%.0f,%.0f)", (void*)g_sbl, (void*)g_wll,
             g_mouseOk ? "OK" : "실패", g_mouseX, g_mouseY);
        logf("MM_RESULT: INJECT_OK clone=%p box=%p gameThread=%lu label='%s'",
             (void*)clone, (void*)box, GetCurrentThreadId(), u8(trLabel()).c_str());
    }
    else
    {
        int n = ++g_cloneOrphans;
        logf("WARN 고아 클론 발생 (%d/5)", n);
        if (n >= 5)
        {
            g_hardFail = true;
            logf("MM_RESULT: FAIL 고아 클론 5개 누적 -- 영구 중단 (위젯 누수 방지)");
        }
    }
}

// v0.40 11b: 클론 항목에 패드 Y 아이콘을 붙인다. 순회 편입(패드로 항목 이동해
// 클론에 멈추기)은 다섯 구조 + 네이티브 RE 로 전부 배제됐다(순회=엔진 Slate 내비,
// 게임 함수 아님, 바이트 시그니처 디투어는 200+ 충돌로 폐기). 그래서 타이틀
// 패드 진입은 전용 Y 버튼이 정식 경로이고, 그 힌트로 라벨 옆에 Y 아이콘을 둔다.
// 안전: 전부 SEH 가드, 실패하면 조용히 생략(라벨은 그대로, Y 버튼도 그대로 동작).
static void addClonePadIcon(UObject* clone)
{
    g_padIcon = nullptr;
    UObject* tree = readObjProp(clone, L"WidgetTree", "padico");
    UObject* tt = readObjProp(clone, L"TitleText", "padico");
    if (!tree || !tt) { logf("padico: WidgetTree/TitleText 없음 -- 아이콘 생략"); return; }
    // 글자 바로 옆에 붙이려면 TitleText 와 아이콘을 HorizontalBox 로 묶어 원래
    // 자리(TitleText 의 부모)에 도로 넣는다. TitleText 부모는 Border(단일자식)
    // 실측 -- HBox 를 그 content 로 넣으면 [글자][아이콘]이 한 덩이로 붙는다.
    UFunction* fnPar = fnOf(tt, L"GetParent", "padico");
    if (!fnPar || (int)fnPar->GetParmsSize() != 8) { logf("padico: GetParent 없음"); return; }
    PB pbp;
    if (!peGuard(tt, fnPar, pbp.b)) { logf("padico: GetParent SEH"); return; }
    void* parRaw = nullptr;
    memcpy(&parRaw, pbp.b + (int)fnPar->GetReturnValueOffset(), 8);
    UObject* parent = reinterpret_cast<UObject*>(parRaw);
    if (!parent) { logf("padico: TitleText 부모 null -- 생략"); return; }
    std::wstring pname;
    if (UClass* pc = parent->GetClassPrivate()) pname = pc->GetName();
    logf("padico: TitleText 부모 = %s", u8(pname).c_str());
    // Border/SizeBox/ScaleBox 등 단일자식 컨테이너여야 안전하게 감쌀 수 있다.
    bool single = wcontains(pname, L"Border") || wcontains(pname, L"SizeBox") ||
                  wcontains(pname, L"ScaleBox") || wcontains(pname, L"Button");
    if (!single) { logf("padico: 부모가 단일자식 아님(%s) -- 감싸기 생략", u8(pname).c_str()); return; }
    UObject* gs = findObj(L"/Script/Engine.Default__GameplayStatics", "padico");
    UObject* bCls = findObj(L"/Script/UMG.Border", "padico");
    UObject* sCls = findObj(L"/Script/UMG.SizeBox", "padico");
    UObject* hCls = findObj(L"/Script/UMG.HorizontalBox", "padico");
    UObject* oCls = findObj(L"/Script/UMG.Overlay", "padico");
    UObject* krl = findObj(L"/Script/Engine.Default__KismetRenderingLibrary", "padico");
    if (!gs || !bCls || !sCls || !hCls || !oCls) { logf("padico: 클래스 해석 실패 -- 생략"); return; }
    UFunction* fnSpawn = fnOf(gs, L"SpawnObject", "padico");
    if (!fnSpawn || (int)fnSpawn->GetParmsSize() != 24) { logf("padico: SpawnObject 없음"); return; }
    auto spawn = [&](UObject* cls) -> UObject* {
        PB pb;
        memcpy(pb.b + 0, &cls, 8);
        memcpy(pb.b + 8, &tree, 8);
        if (!peGuard(gs, fnSpawn, pb.b)) return nullptr;
        void* w = nullptr;
        memcpy(&w, pb.b + (int)fnSpawn->GetReturnValueOffset(), 8);
        return reinterpret_cast<UObject*>(w);
    };
    UObject* box = spawn(sCls);
    if (!box) { logf("padico: SizeBox spawn 실패 -- 생략"); return; }
    float iw = 37.0f, ih = 37.0f;   // v0.50c: 46 -> 41 -> 37 (사용자 피드백 -10% x2)
    callBytes(box, L"SetWidthOverride", &iw, 4, "padico");
    callBytes(box, L"SetHeightOverride", &ih, 4, "padico");
    UObject* tex = nullptr;
    UFunction* fnImp = krl ? fnOf(krl, L"ImportFileAsTexture2D", "padico") : nullptr;
    if (fnImp && (int)fnImp->GetParmsSize() == 32)
    {
        static wchar_t tp[MAX_PATH * 2];
        assetPath(tp, L"pad_y.png");
        PB pb;
        UObject* c3 = clone;
        memcpy(pb.b + 0, &c3, 8);
        FStringRaw fs{tp, (int)wcslen(tp) + 1, (int)wcslen(tp) + 1};
        memcpy(pb.b + 8, &fs, 16);
        if (peGuard(krl, fnImp, pb.b))
            memcpy(&tex, pb.b + (int)fnImp->GetReturnValueOffset(), 8);
    }
    if (UObject* im = spawn(bCls))
    {
        if (tex) callBytes(im, L"SetBrushFromTexture", &tex, 8, "padico");
        setBrushColor(im, {1, 1, 1, 1}, "padico");
        setVisibility(im, 4, "padico");
        slotAlign(addChildTo(box, im, "padico"), 0, 0, "padico");
    }
    // v0.50: Border content = Overlay. 글자는 중앙 그대로(움직이지 않음), 아이콘은
    // "간격자(spacer)+아이콘" HBox 를 중앙에 얹어 글자 오른쪽으로 고정 오프셋만큼
    // 민다(아이콘 중심 = 행중심 + 간격자폭/2). => 글자 위치 불변 + 아이콘 오른쪽 +
    // 세로 중앙. HBox 를 그대로 중앙정렬하면 글자가 밀리므로(구현 v11b) 분리했다.
    UObject* ov = spawn(oCls);
    if (!ov) { logf("padico: Overlay spawn 실패 -- 생략"); return; }
    slotAlign(addChildTo(ov, tt, "padico"), 2, 2, "padico");   // 글자: 중앙(불변)
    UObject* ihb = spawn(hCls);
    if (ihb)
    {
        if (UObject* sp = spawn(sCls))
        {   // 간격자 -- 폭의 절반만큼 아이콘이 중앙에서 오른쪽으로. 라벨 폭이 언어마다
            // 달라 언어별 상수(실측: EN "ModManager" 300 / KO "모드매니저" 240 = 여백 동일).
            float spw = (g_lang == 1) ? 300.0f : 240.0f, sph = 1.0f;
            callBytes(sp, L"SetWidthOverride", &spw, 4, "padico");
            callBytes(sp, L"SetHeightOverride", &sph, 4, "padico");
            setVisibility(sp, 4, "padico");
            slotAlign(addChildTo(ihb, sp, "padico"), -1, 2, "padico");
            g_padIconSpacer = (void*)sp;
        }
        slotAlign(addChildTo(ihb, box, "padico"), -1, 2, "padico");   // 아이콘: 세로중앙
        slotAlign(addChildTo(ov, ihb, "padico"), 2, 2, "padico");      // 덩이: Overlay 중앙
    }
    UObject* osl = addChildTo(parent, ov, "padico");
    if (!osl) { logf("padico: Border 에 Overlay 부착 실패 -- 생략"); return; }
    slotAlign(osl, 2, 2, "padico");
    bool padOn = g_inputMode.load(std::memory_order_relaxed) == 1 &&
                 g_padPresent.load(std::memory_order_relaxed);
    setVisibility(box, padOn ? 4 : 1, "padico");
    g_padIcon = (void*)box;
    logf("padico: Y 아이콘 부착 완료 (부모=%s tex=%d)", u8(pname).c_str(), tex ? 1 : 0);
}

// 7-11) Create 된 클론의 무해화 -> 부착 -> 마감. 핵심 성공 조건은
//       TitleMenuType=100 검증 + AddChild slot != null. 나머지는 최선 노력.
static bool finishClone(UObject* box, UObject* exitW, UObject* sample, UObject* clone)
{
    // 7) TitleMenuType = 100 -- AddChild 전에! (신품=0=이어하기 함정)
    {
        int off = propOffset(clone, L"TitleMenuType", 1, "menuType");
        if (off < 0)
        {
            logf("FAIL TitleMenuType 해석 불가 -- 클릭 무해화가 불가능하므로 삽입 중단");
            return false;  // 무해화 없이 붙이면 클릭=게임시작. 붙이지 않는 것이 안전.
        }
        unsigned char before = 255;
        readByteGuard(clone, off, &before);
        if (!writeByteGuard(clone, off, 100))
        {
            logf("FAIL TitleMenuType 쓰기 AV (off=0x%X)", off);
            g_hardFail = true;
            return false;
        }
        unsigned char after = 255;
        readByteGuard(clone, off, &after);
        logf("OK TitleMenuType: off=0x%X, %d -> %d (기대 0->100)", off, (int)before, (int)after);
        if (after != 100)
        {
            logf("FAIL TitleMenuType 검증 실패 -- 삽입 중단");
            return false;
        }
    }

    // 8) 라벨 1차 (AddChild 전). 라벨 실패가 "구조적"(FText 전사 무효)이면
    //    라벨 없는 정체불명 행을 메뉴에 남기지 말고 부착 전에 중단한다(리뷰 확정).
    setLabel(clone, "pre");
    if (g_hardFail.load())
    {
        logf("FAIL 라벨 단계에서 구조적 실패 -- 부착 전 중단");
        return false;
    }

    // 9) AddChild(클론) -> RemoveChild(Exit) -> AddChild(Exit)
    UFunction* fnAdd = fnOf(box, L"AddChild", "addChild");
    if (!fnAdd || !parmsExact(fnAdd, 16, "AddChild")) return false;
    {
        PB pb;
        memcpy(pb.b, &clone, 8);
        if (!peGuard(box, fnAdd, pb.b))
        {
            logf("FAIL AddChild(clone): SEH 예외");
            g_hardFail = true;
            return false;
        }
        void* slot = nullptr;
        memcpy(&slot, pb.b + (int)fnAdd->GetReturnValueOffset(), 8);
        logf("%s AddChild(clone): slot=%p", slot ? "OK" : "WARN", slot);
        if (!slot) return false;  // 붙지 않았으면 이후 단계 무의미
    }
    // v0.50: 위치 원복 예약 -- 지금은 클론이 맨 아래(AddChild 결과). 키퍼가 게임의
    // Exit 로컬라이즈가 안정된 뒤 재정렬(Exit 를 클론 아래로 내려 클론을 설정<->나가기
    // 사이에 놓음)하고, 그 순간의 Exit 라벨을 캡처해 복원한다(언어 라벨 보호).
    g_reorderPending = true;
    g_reorderHaveLast = false;
    g_reorderStableSince = 0;

    // 10) 라벨 2차 (Construct 가 라벨을 되돌렸을 수 있음)
    setLabel(clone, "post");

    // 11) 마감 -- Lua 실측 규약 그대로
    //     ImgSelected: 항상 Visible(0) ("접으면 안 된다" -- 과거 실수)
    //     OverlaySelected: Collapsed(1) = 비호버 상태
    if (UObject* img = readObjProp(clone, L"ImgSelected", "deco"))
        setVisibility(img, 0, "ImgSelected");
    if (UObject* ovl = readObjProp(clone, L"OverlaySelected", "deco"))
        setVisibility(ovl, 1, "OverlaySelected");

    //     forceShow: 행 + 라벨 불투명
    setRenderOpacity(clone, 1.0f, "forceShow.row");
    if (UObject* tt = readObjProp(clone, L"TitleText", "forceShow"))
        setRenderOpacity(tt, 1.0f, "forceShow.text");

    //     matchSibling: 형제(Option)의 내부 위젯 가시성/불투명도 복사
    {
        static const wchar_t* const kids[2] = {L"TitleMenuBtn", L"TitleText"};
        for (const wchar_t* kid : kids)
        {
            UObject* src = readObjProp(sample, kid, "match.src");
            UObject* dst = readObjProp(clone, kid, "match.dst");
            if (!src || !dst) continue;
            int v = getVisibility(src, "match.vis");
            if (v >= 0) setVisibility(dst, (unsigned char)v, "match.vis");
            bool ok = false;
            float op = getRenderOpacity(src, "match.op", &ok);
            if (ok) setRenderOpacity(dst, op, "match.op");
        }
    }

    //     setHitTest: 행 자체는 Visible(0) = 히트테스트 대상
    setVisibility(clone, 0, "hitTest");

    addClonePadIcon(clone);   // 11b: 패드 Y 아이콘(전용 진입 버튼 힌트)

    // v0.50: 정렬 전 은신 -- 지금 클론은 맨 아래(AddChild 결과)다. 재정렬이 끝날
    // 때까지 Collapsed 로 숨겨 "맨 아래에 나타났다가 1초 뒤 위로 점프"를 없앤다.
    // 복귀(Visible 0)는 키퍼의 재정렬 블록이 모든 경로(성공/실패/시간초과)에서 보장.
    setVisibility(clone, 1, "reorderHide");
    g_reorderOpenTicks = 0;
    blackboxPhase("title");   // v0.50: 블랙박스 -- 타이틀 도달 = 모드 시작 전부 완료
    return true;
}

// ---------------------------------------------------------------- PE 콜백

static void onProcessEventPre(UObject* context, UFunction* function, void* parms)
{
    if (!function || t_busy || g_hardFail.load(std::memory_order_relaxed)) return;

    // v0.7: 펄스 타이머가 클론의 게터를 부를 때 context == 클론 -- 그 자체가
    // "클론이 살아있다"는 증명이라 생존 신호를 공짜로 갱신한다 (FindAllOf 절약)
    if (context && context == reinterpret_cast<UObject*>(g_myClone.load(std::memory_order_relaxed)))
    {
        ULONGLONG pnow = GetTickCount64();
        g_lastTitleMs.store(pnow, std::memory_order_relaxed);
        // v0.8.2: 펄스 박동 계측
        g_pulseTicks.fetch_add(1, std::memory_order_relaxed);
        ULONGLONG last = g_lastPulseMs.exchange(pnow, std::memory_order_relaxed);
        if (last)
        {
            ULONGLONG gap = pnow - last;
            ULONGLONG prev = g_pulseMaxGapMs.load(std::memory_order_relaxed);
            if (gap > prev) g_pulseMaxGapMs.store(gap, std::memory_order_relaxed);
        }
    }

    // v0.40(pad): 메뉴 선택 이동 관측 -- BP_OnItemSelectionChanged(bool).
    // 포인터 비교 한 번이라 모든 PE 트래픽에 얹어도 공짜다.
    // 진단(예산 8): 클론이 실제로 포커스를 받는 순간을 직접 증명
    if (context && function == reinterpret_cast<UFunction*>(g_fnFocusRecv.load(std::memory_order_relaxed)) &&
        context == reinterpret_cast<UObject*>(g_myClone.load(std::memory_order_relaxed)))
    {
        static std::atomic<int> s_focusLogBudget{8};
        if (s_focusLogBudget.fetch_sub(1, std::memory_order_relaxed) > 0)
            logf("menufocus: 클론 OnFocusReceived!");
    }
    if (parms && context &&
        function == reinterpret_cast<UFunction*>(g_fnSelChanged.load(std::memory_order_relaxed)))
    {
        g_selEvtOn.store(*(unsigned char*)parms, std::memory_order_relaxed);
        g_selEvtItem.store((void*)context, std::memory_order_relaxed);
    }
    // v0.50b: 클론 확정(패드 A/마우스 클릭 -- 게임이 부르는 OnClickedButton) 편승.
    // 선택 시각이 OverlaySelected 가 아니어도(포커스 스타일) A 진입이 성립한다.
    if (context && function == reinterpret_cast<UFunction*>(g_fnItemClicked.load(std::memory_order_relaxed)) &&
        context == reinterpret_cast<UObject*>(g_myClone.load(std::memory_order_relaxed)))
        g_cloneClickedMs.store(GetTickCount64(), std::memory_order_relaxed);

    // 콜백 밖으로는 어떤 C++ 예외도 내보내지 않는다 (14:19 크래시 교훈:
    // 엔진/UE4SS 경계를 넘는 예외 = terminate).
    try
    {
        // v0.40 9차 진단(예산 6): OnKeyDown 수신자 = 포커스를 쥔 위젯(뿌리 검증).
        // 여기(이벤트 수신 순간)는 context 생존이 보장된다 -- 펌프로 미루면 TRAPS X.
        if (context && function == reinterpret_cast<UFunction*>(g_fnKeyDown.load(std::memory_order_relaxed)))
        {
            static std::atomic<void*> s_lastKeyCtx{nullptr};
            static std::atomic<int> s_keyCtxBudget{6};
            if ((void*)context != s_lastKeyCtx.load(std::memory_order_relaxed) &&
                s_keyCtxBudget.fetch_sub(1, std::memory_order_relaxed) > 0)
            {
                s_lastKeyCtx.store((void*)context, std::memory_order_relaxed);
                logf("keyrecv: %s", u8(context->GetFullName(nullptr)).c_str());
            }
        }
        // v0.40 9차 진단(예산 60): 게임의 선택 이동 스트림. 편입 후 패드 하강에서
        // 클론에 on=1 이 찍히면 결정적 성공 신호 (침묵은 실패 증거가 아니다 --
        // 패드 경로는 이 이벤트를 안 거칠 수 있음이 실측됨. 정본 판정은 폴링)
        if (parms && context &&
            function == reinterpret_cast<UFunction*>(g_fnSelChanged.load(std::memory_order_relaxed)))
        {
            static std::atomic<int> s_selEvtBudget{60};
            if (s_selEvtBudget.fetch_sub(1, std::memory_order_relaxed) > 0)
            {
                logf("selEvt: %s on=%d%s", u8(context->GetName()).c_str(),
                     (int)*(unsigned char*)parms,
                     (void*)context == g_myClone.load(std::memory_order_relaxed) ? " *클론" : "");
            }
        }
        // 함수 분류(포인터당 1회): 타이틀 메뉴 관련 트래픽만 트리거로 삼는다
        unsigned char cls = 0;
        {
            std::lock_guard<std::mutex> lk(g_mx);
            auto it = g_fnClass.find(function);
            if (it != g_fnClass.end()) cls = it->second;
        }
        if (cls == 0)
        {
            std::wstring full = function->GetFullName(nullptr);
            cls = (wcontains(full, L"DUWG_TitleMenu") || wcontains(full, L"DTitleMenuUserWidget") ||
                   wcontains(full, L"DLayerTitle") || wcontains(full, L"DPanelTitle"))
                      ? 1
                      : 2;
            std::lock_guard<std::mutex> lk(g_mx);
            g_fnClass[function] = cls;
            // v0.40 진단: 타이틀 메뉴/내비 함수 어휘를 처음 볼 때 잡는다(패드로 메뉴를
            // 움직이면 게임이 부르는 내비 함수가 여기 찍힌다). 예산 90, 함수당 1회.
            if (wcontains(full, L"UserWidget:OnFocusReceived"))
                g_fnFocusRecv.store((void*)function, std::memory_order_relaxed);
            if (wcontains(full, L"UserWidget:OnKeyDown"))
                g_fnKeyDown.store((void*)function, std::memory_order_relaxed);
            if (wcontains(full, L"DTitleMenuUserWidget:OnClickedButton"))
                g_fnItemClicked.store((void*)function, std::memory_order_relaxed);
            static int s_menuFnBudget = 90;
            if (s_menuFnBudget > 0 &&
                (wcontains(full, L"TitleMenu") || wcontains(full, L"Navigat") ||
                 wcontains(full, L"MenuMove") || wcontains(full, L"MoveMenu") ||
                 wcontains(full, L"SelectMenu") || wcontains(full, L"MenuSelect") ||
                 wcontains(full, L"OnKey") || wcontains(full, L"Focus") ||
                 wcontains(full, L"MenuBtn") || wcontains(full, L"MenuButton")))
            {
                --s_menuFnBudget;
                logf("menufn: %s", u8(full).c_str());
            }
        }
        // v0.2 펌프: 삽입 완료 후, 게임 스레드(삽입 순간 캡처한 ID)의 아무
        // PE 호출에나 편승해 33ms 간격으로 호버/클릭/패널 입력을 처리한다.
        unsigned long gtid = g_gameThreadId.load(std::memory_order_relaxed);
        if (gtid && GetCurrentThreadId() == gtid &&
            (g_myClone.load(std::memory_order_relaxed) || g_panel || g_panelOpen))
        {
            ULONGLONG pnow = GetTickCount64();
            if (pnow - g_lastPumpMs >= 33)
            {
                g_lastPumpMs = pnow;
                t_busy = true;
                try
                {
                    pump(pnow);
                }
                catch (...)
                {
                    logf("WARN pump: C++ 예외 -- 상태 리셋");
                    cloneLost("pump 예외");
                }
                t_busy = false;
            }
        }

        if (cls != 1) return;

        g_relevantHits.fetch_add(1, std::memory_order_relaxed);
        g_lastTitleMs.store(GetTickCount64(), std::memory_order_relaxed);  // 타이틀 UI 생존 신호

        // UMG 는 게임 스레드에서만. gtGate 는 예외 안전 + 고장 시 cls 게이트 위임.
        if (!gtGate()) return;

        ULONGLONG now = GetTickCount64();
        if (now - g_lastTryMs < 700) return;  // 스로틀
        g_lastTryMs = now;

        t_busy = true;  // 우리 자신의 ProcessEvent 재진입 차단
        try
        {
            tryInject();
        }
        catch (...)
        {
            logf("FAIL tryInject: C++ 예외 -- 영구 중단");
            g_hardFail = true;
        }
        t_busy = false;
    }
    catch (...)
    {
        t_busy = false;
        static std::atomic<int> n{0};
        int c = n.fetch_add(1) + 1;
        if (c <= 3) logf("WARN PE 콜백 C++ 예외 (#%d) -- 무시하고 계속", c);
    }
}

// ---------------------------------------------------------------- 모드

class DsCppModManager final : public RC::CppUserModBase
{
    ULONGLONG m_lastBeat = 0;
    ULONGLONG m_lastZipScan = 0;  // v0.22: plugins zip 자동 해제 스캔 주기
    bool m_first = true;
    bool m_lmbPrev = false;
    bool m_escPrev = false;

  public:
    DsCppModManager()
    {
        ModName = L"DsCppModManager";
        ModVersion = L"0.50";
        ModDescription = L"Mod manager: key-bind and color-picker option controls";
        ModAuthors = L"SummerSpring";
        logf("start_mod: ctor OK (%s)", u8(MOD_VER_W).c_str());
        // v0.50: 블랙박스 회전 + 초입 안전장치 -- 엔진이 pak 을 열기 전에 파일
        // 계층만 손본다. (순수 Win32 I/O 전용. UE API 는 이 시점에 절대 금지.)
        try
        {
            blackboxRotate();
            earlyBootGuard();
        }
        catch (...)
        {
            logf("WARN earlyGuard: C++ 예외 -- 건너뜀 (본조치는 첫 틱 bootGuard)");
        }
    }

    ~DsCppModManager() override
    {
        logf("dtor: hits=%llu attempts=%llu done=%p hardFail=%d",
             (unsigned long long)g_relevantHits.load(),
             (unsigned long long)g_attempts.load(),
             g_doneBox.load(), (int)g_hardFail.load());
    }

    auto on_unreal_init() -> void override
    {
        logf("on_unreal_init");
        blackboxPhase("engine-init");   // v0.50: 블랙박스
        RC::Unreal::Hook::RegisterProcessEventPreCallback(&onProcessEventPre);
        logf("ProcessEvent pre-callback 등록 완료 (구형 오버로드)");
    }

    auto on_update() -> void override
    {
        // 이 스레드는 게임 스레드가 아니다 -- UMG 는 금지, 키 상태 읽기는 안전
        // (프로브 F9 실증). v0.7: 입력 엣지를 여기(5ms 주기, 항상 돎)서 래치해
        // 게임 스레드 펌프가 소비 -- PE 기근으로 클릭을 놓치던 지연의 해결책 ①.
        // v0.40: 게임 창이 앞에 있을 때만 입력 엣지를 만든다. GetAsyncKeyState 는
        // 전역이고 게임의 커서 좌표는 포커스가 없어도 살아 있어서, 게임 위에 겹친
        // 탐색기를 클릭하면 그 자리의 컨트롤이 눌렸다 (실측 2026-08-10 21:14 --
        // 화면 밖 좌표 (5562,1937) 가 히트 판정에 들어온 로그).
        // 물리 상태(m_lmbPrev/m_escPrev)는 배경에서도 계속 추적한다 --
        // 포커스 복귀 순간 배경에서 생긴 눌림/뗌이 유령 엣지가 되는 것을 막는다.
        HWND fgWnd = GetForegroundWindow();
        DWORD fgPid = 0;
        if (fgWnd) GetWindowThreadProcessId(fgWnd, &fgPid);
        const bool fgOurs = (fgPid == GetCurrentProcessId());
        // v0.50: 입력 진단 기록 (스팀 컨트롤러 조사). 타이틀 = 클론 생존 신호.
        // ⚠ 리뷰 D2: 진단 호출은 **클릭/ESC 래치 뒤**에 둔다 -- 진단이 파일·장치
        // 열거로 잠깐 멎으면 그 사이의 짧은 클릭이 통째로 유실된다(래치는 틱당 1샘플).
        // ⚠ 리뷰 확정 #2: 포인터만 보면 **고착**한다 -- 펌프가 SEH 로 죽으면
        // (g_hardFail) cloneLost 에 영영 못 가서 g_myClone 이 non-null 로 남고,
        // 그때부터 '타이틀'로 오인해 **실제 게임플레이 중에도** 기록하게 된다.
        // 생존 신호(g_lastTitleMs, 자체가 hardFail 게이트 안이라 실패 시 멎는다)로
        // 신선도를 함께 본다 -- 닫히는 쪽으로 실패한다.
        const bool atTitle = g_myClone.load(std::memory_order_relaxed) != nullptr &&
                             GetTickCount64() - g_lastTitleMs.load(std::memory_order_relaxed) < 2000;
        bool lmb = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
        if (fgOurs)
        {
            if (lmb && !m_lmbPrev)
            {
                g_pendClickMs.store(GetTickCount64(), std::memory_order_relaxed);
                g_inputMode.store(0, std::memory_order_relaxed);   // v0.40: 마우스 사용
            }
            if (!lmb && m_lmbPrev)
            {
                ULONGLONG t = GetTickCount64();
                g_lmbUpMs.store(t, std::memory_order_relaxed);
                g_pendUpMs.store(t, std::memory_order_relaxed);  // v0.17: 펌프가 소비할 뗌 엣지
            }
            g_lmbHeld.store(lmb, std::memory_order_relaxed);  // v0.16: 드래그 유지 판정용
        }
        else
            g_lmbHeld.store(false, std::memory_order_relaxed);
        m_lmbPrev = lmb;
        // v0.28: 키 캡처 중에만 전체 가상키를 훑는다 (평상시엔 낭비라 안 한다)
        // v0.40: 배경에서는 훑지 않는다 -- 다른 창에 치는 타자가 바인딩되면 안 된다
        if (fgOurs && g_keyCapture.load(std::memory_order_relaxed) &&
            g_capturedVk.load(std::memory_order_relaxed) == 0)
        {
            for (int vk = 0x01; vk <= 0xFE; ++vk)
            {
                if (vk == VK_LBUTTON || vk == VK_ESCAPE) continue;  // 클릭/취소는 UI 몫
                if (GetAsyncKeyState(vk) & 0x8000)
                {
                    g_capturedVk.store(vk, std::memory_order_relaxed);
                    break;
                }
            }
        }
        bool esc = (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
        if (fgOurs && esc && !m_escPrev)
        {
            g_pendEscMs.store(GetTickCount64(), std::memory_order_relaxed);
            g_inputMode.store(0, std::memory_order_relaxed);   // v0.40: 키보드 사용
        }
        m_escPrev = esc;
        inputDiagTick(fgOurs, atTitle);   // 리뷰 D2: 래치를 다 세운 뒤에 진단(순수 관찰)
        // v0.40: 마우스가 3px 만 움직여도 키/마 모드 -- 게임 설정창 실측(패드 UI 즉시 숨김)
        {
            static POINT s_lastCur = {-100000, -100000};
            POINT cp;
            if (GetCursorPos(&cp))
            {
                if (fgOurs && s_lastCur.x != -100000 &&
                    (cp.x - s_lastCur.x > 3 || s_lastCur.x - cp.x > 3 ||
                     cp.y - s_lastCur.y > 3 || s_lastCur.y - cp.y > 3))
                    g_inputMode.store(0, std::memory_order_relaxed);
                s_lastCur = cp;
            }
        }
        // v0.40(pad): XInput 폴링 -- 엣지 래치 + 방향 자동 반복(350ms 후 130ms)
        // 리뷰 반영 셋: ① 물리 상태(s_prevBtn)는 배경에서도 추적한다 -- 포커스 복귀
        // 순간의 유령 엣지 방지(마우스와 같은 규율). ② 패드가 없으면 3초에 한 번만
        // 재탐색한다 -- 빈 슬롯 XInputGetState 는 장치 열거라 비싸다. ③ 반복 방향은
        // 언제나 하나(대각선 플릭은 세로 우선)고, 놓으면 눌려 있는 다른 방향이 이어받는다.
        {
            static int s_padSlot = -1;
            static WORD s_prevBtn = 0;
            static ULONGLONG s_repDirMs = 0;
            static unsigned s_repDir = 0;
            static ULONGLONG s_nextScanMs = 0;
            ULONGLONG pnow = GetTickCount64();
            XINPUT_STATE xs;
            memset(&xs, 0, sizeof(xs));
            bool got = false;
            // v0.50: 보조 XInput -- 기본(XINPUT1_4)이 못 보면 **게임이 쓰는 DLL**
            // (XINPUT1_3 등, 이미 로드된 것)로도 물어본다. s_padAlt 가 그 슬롯을 기억한다.
            static bool s_padAlt = false;
            if (s_padSlot >= 0)
            {
                DWORD r = s_padAlt ? xiAltGetState((DWORD)s_padSlot, &xs)
                                   : XInputGetState((DWORD)s_padSlot, &xs);
                if (r == ERROR_SUCCESS) got = true;
                else { s_padSlot = -1; s_padAlt = false; s_nextScanMs = pnow + 3000; }
            }
            else if (pnow >= s_nextScanMs)
            {
                /* ★ 조사 확정(2026-08-17, Valve 문서): Windows 에서 스팀 컨트롤러는
                   **진짜 XInput 장치가 아니다**. 스팀 오버레이가 **게임 프로세스 안에서**
                   XInput 함수 본체에 훅을 걸어 가짜 Xbox 패드를 만들어 낸다. 즉
                   시스템 드라이버가 아니라 "어느 DLL 로 물어보느냐"가 보이냐 마느냐를
                   가른다. 게임이 쓰는 DLL(XINPUT1_3)은 오버레이 초기화 시점에 이미
                   올라와 있어 훅이 확실하고, 우리가 정적 링크로 늦게 들여오는
                   XINPUT1_4 는 훅을 놓쳤을 수 있다. → **게임이 쓰는 쪽부터** 물어본다. */
                xiAltResolve();
                for (int pass = 0; pass < 2 && !got; ++pass)
                {
                    bool useAlt = (pass == 0) ? (g_xiAlt != nullptr) : false;
                    if (pass == 1 && g_xiAlt == nullptr && s_padSlot >= 0) break;
                    for (int gi = 0; gi < 4 && !got; ++gi)
                    {
                        DWORD r = useAlt ? xiAltGetState((DWORD)gi, &xs)
                                         : XInputGetState((DWORD)gi, &xs);
                        if (r != ERROR_SUCCESS) continue;
                        s_padSlot = gi;
                        s_padAlt = useAlt;
                        got = true;
                        static bool s_padWhereLogged = false;
                        if (!s_padWhereLogged)
                        {
                            s_padWhereLogged = true;
                            const char* who = useAlt
                                ? (g_xiAltName ? u8(g_xiAltName).c_str() : "보조 XInput")
                                : "XINPUT1_4(기본)";
                            logf("pad: 슬롯 %d 인식 -- %s 경유", gi, who);
                            inputLog("XInput: 슬롯 %d 인식 -- %s 경유%s", gi, who,
                                     useAlt ? " (게임이 쓰는 DLL. 스팀 입력 에뮬 패드는 "
                                              "이쪽에만 붙는 경우가 있다)" : "");
                        }
                    }
                }
                if (!got) s_nextScanMs = pnow + 3000;
            }
            g_padPresent.store(got, std::memory_order_relaxed);
            // v0.50: 입력 진단 -- 타이틀에서만. 연결 상태·장치 능력(가상패드 식별)·
            // 버튼/스틱 변화·패킷 번호(장치가 살아 있는지)를 남긴다.
            if (atTitle && !g_inLogOff)
            {
                static int s_diagSlot = -2;      // -2 = 아직 안 봄 (첫 상태도 남긴다)
                static ULONGLONG s_diagNextMs = 0;
                static DWORD s_diagPacket = 0;
                static WORD s_diagBtn = 0;
                int nowSlot = got ? s_padSlot : -1;
                if (nowSlot != s_diagSlot)
                {
                    s_diagSlot = nowSlot;
                    if (got)
                    {
                        inputLog("XInput: 패드 인식됨 (슬롯 %d, %s)", s_padSlot,
                                 s_padAlt ? (g_xiAltName ? u8(g_xiAltName).c_str() : "보조") : "기본 1_4");
                        // ⚠ 리뷰: 보조(1_3) 슬롯인데 기본(1_4)의 GetCapabilities 를
                        // 부르면 **항상 실패**한다 -- 같은 DLL 로 물어야 한다.
                        // 보조 경로에선 능력 조회를 건너뛴다(연결 판정은 GetState 가 한다).
                        if (!s_padAlt)
                        {
                            XINPUT_CAPABILITIES caps;
                            memset(&caps, 0, sizeof(caps));
                            if (XInputGetCapabilities((DWORD)s_padSlot, XINPUT_FLAG_GAMEPAD, &caps) == ERROR_SUCCESS)
                                inputLog("XInput: 장치 능력 type=%u subtype=%u flags=0x%04X",
                                         (unsigned)caps.Type, (unsigned)caps.SubType, (unsigned)caps.Flags);
                            else
                                inputLog("XInput: 장치 능력 조회 실패");
                        }
                    }
                    else
                    {
                        inputLog("XInput: 인식된 패드 없음 (슬롯 0~3 전부 미연결)");
                        inputLogJoysticks();   // XInput 밖에 장치가 있는지 (이름까지)
                    }
                    s_diagNextMs = pnow + 10000;
                }
                if (got)
                {
                    WORD nb = xs.Gamepad.wButtons;
                    if (nb != s_diagBtn)
                    {
                        s_diagBtn = nb;
                        inputLog("패드 버튼=0x%04X LX=%d LY=%d RX=%d RY=%d LT=%u RT=%u",
                                 (unsigned)nb, (int)xs.Gamepad.sThumbLX, (int)xs.Gamepad.sThumbLY,
                                 (int)xs.Gamepad.sThumbRX, (int)xs.Gamepad.sThumbRY,
                                 (unsigned)xs.Gamepad.bLeftTrigger, (unsigned)xs.Gamepad.bRightTrigger);
                    }
                    if (pnow >= s_diagNextMs)
                    {
                        s_diagNextMs = pnow + 10000;
                        inputLog("패드 요약: 슬롯=%d 패킷=%lu(직전 %lu) 버튼=0x%04X 전경=%d",
                                 s_padSlot, (unsigned long)xs.dwPacketNumber,
                                 (unsigned long)s_diagPacket, (unsigned)xs.Gamepad.wButtons,
                                 (int)fgOurs);
                        s_diagPacket = xs.dwPacketNumber;
                    }
                }
                else if (pnow >= s_diagNextMs)
                {
                    s_diagNextMs = pnow + 30000;
                    inputLog("패드 없음 상태 유지 -- 스팀 컨트롤러라면 스팀의 컨트롤러 설정이 "
                             "Xbox(XInput) 형식으로 넘겨주지 않는 구성일 수 있다");
                    // 리뷰 D2: 장치 열거는 블로킹이다 -- 장치 목록은 30초마다 바뀌는
                    // 값이 아니니 세션당 3회로 막는다 (같은 줄이 반복될 뿐이다).
                    static int s_joyScans = 0;
                    if (s_joyScans < 3) { ++s_joyScans; inputLogJoysticks(); }
                }
            }
            if (got)
            {
                WORD b = xs.Gamepad.wButtons;
                if (xs.Gamepad.sThumbLY > 20000) b |= XINPUT_GAMEPAD_DPAD_UP;
                if (xs.Gamepad.sThumbLY < -20000) b |= XINPUT_GAMEPAD_DPAD_DOWN;
                if (xs.Gamepad.sThumbLX < -20000) b |= XINPUT_GAMEPAD_DPAD_LEFT;
                if (xs.Gamepad.sThumbLX > 20000) b |= XINPUT_GAMEPAD_DPAD_RIGHT;
                WORD rise = (WORD)(b & (WORD)~s_prevBtn);
                s_prevBtn = b;   // ★ 전경 여부와 무관하게 추적
                g_padBHeld.store((b & XINPUT_GAMEPAD_B) != 0, std::memory_order_relaxed);
                g_padDirHeld.store((b & (XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_DOWN |
                                         XINPUT_GAMEPAD_DPAD_LEFT | XINPUT_GAMEPAD_DPAD_RIGHT)) != 0,
                                   std::memory_order_relaxed);
                if (fgOurs)
                {
                    unsigned e = 0;
                    if (rise & XINPUT_GAMEPAD_DPAD_UP) e |= PAD_UP;
                    if (rise & XINPUT_GAMEPAD_DPAD_DOWN) e |= PAD_DOWN;
                    if (rise & XINPUT_GAMEPAD_DPAD_LEFT) e |= PAD_LEFT;
                    if (rise & XINPUT_GAMEPAD_DPAD_RIGHT) e |= PAD_RIGHT;
                    if (rise & XINPUT_GAMEPAD_A) e |= PAD_A;
                    if (rise & XINPUT_GAMEPAD_LEFT_SHOULDER) e |= PAD_LB;
                    if (rise & XINPUT_GAMEPAD_RIGHT_SHOULDER) e |= PAD_RB;
                    if (rise & XINPUT_GAMEPAD_Y) e |= PAD_Y;   // 10차h: 타이틀 = 매니저 열기
                    if (rise & XINPUT_GAMEPAD_B)
                    {
                        g_pendEscMs.store(pnow, std::memory_order_relaxed);
                        g_inputMode.store(1, std::memory_order_relaxed);
                    }
                    unsigned held = 0;
                    if (b & XINPUT_GAMEPAD_DPAD_UP) held |= PAD_UP;
                    if (b & XINPUT_GAMEPAD_DPAD_DOWN) held |= PAD_DOWN;
                    if (b & XINPUT_GAMEPAD_DPAD_LEFT) held |= PAD_LEFT;
                    if (b & XINPUT_GAMEPAD_DPAD_RIGHT) held |= PAD_RIGHT;
                    unsigned eDir = e & (PAD_UP | PAD_DOWN | PAD_LEFT | PAD_RIGHT);
                    if (eDir)
                    {
                        s_repDir = (eDir & PAD_UP) ? PAD_UP : (eDir & PAD_DOWN) ? PAD_DOWN
                                 : (eDir & PAD_LEFT) ? PAD_LEFT : PAD_RIGHT;
                        s_repDirMs = pnow + 350;
                    }
                    else if (s_repDir && !(held & s_repDir))
                    {
                        s_repDir = (held & PAD_UP) ? PAD_UP : (held & PAD_DOWN) ? PAD_DOWN
                                 : (held & PAD_LEFT) ? PAD_LEFT : (held & PAD_RIGHT) ? PAD_RIGHT : 0;
                        s_repDirMs = pnow + 350;
                    }
                    else if (s_repDir && pnow >= s_repDirMs)
                    {
                        e |= s_repDir;
                        s_repDirMs = pnow + 130;
                    }
                    if (e)
                    {
                        g_padEdges.fetch_or(e, std::memory_order_relaxed);
                        g_padEdgeMs.store(pnow, std::memory_order_relaxed);
                        g_inputMode.store(1, std::memory_order_relaxed);   // v0.40: 패드 사용
                        if (e & (PAD_UP | PAD_DOWN | PAD_LEFT | PAD_RIGHT))
                            g_lastPadNavMs.store(pnow, std::memory_order_relaxed);
                    }
                }
                else s_repDir = 0;
            }
            else
            {
                s_prevBtn = 0;
                g_padBHeld.store(false, std::memory_order_relaxed);
                g_padDirHeld.store(false, std::memory_order_relaxed);
            }
        }

        // ⚠ 14:19 크래시 원인 지점: 여기서 IsInGameThread() 를 무방비로 불렀었다.
        if (m_first)
        {
            m_first = false;
            bool gt = gtGate();  // 예외 안전 래퍼 -- 첫 호출로 정상/고장 판별
            logf("on_update 첫 틱: gtGate=%d state=%d (state=1 정상이면 0 기대 -- UpdateThread)",
                 (int)gt, g_gtState.load());
            // v0.8: 게임 시작 시 1회 상태 정합 -- 매니저의 켜기/끄기(로드 계층)를
            // 런타임 협조 계약(probe/mods_enabled.txt)에 반영한다. 파일 I/O 뿐이라
            // 이 스레드에서 안전.
            // v0.23: 부팅 안전장치 -- 게임 패치/직전 크래시를 감지해 모드를 내린다.
            // reconcile 보다 **먼저** 한다: 안전모드가 끈 상태를 그대로 계약에 반영해야
            // 한다(순서가 뒤바뀌면 방금 끈 것을 다시 켠 값으로 덮어쓴다).
            // 리뷰 D6: 로그 파일 이름을 **세션 시작 날짜**로 확정한다. 지연 초기화에
            // 맡기면 첫 기록(타이틀 진입)이 10~18초 뒤라 자정 직전 실행에서 날짜가
            // 하루 밀린다. (g_inLogInit 가드라 두 번 열려도 무해)
            inputLogOpen();
            bootGuard();
            reconcileRuntimeContract("게임 시작");
            // v0.50: 켜짐인데 진입점(정션)이 없는 모드를 복구 -- 안전모드가 끈 것을
            // 되살리지 않도록 반드시 bootGuard 뒤에 둔다.
            reconcileEntryPoints("게임 시작");
        }
        ULONGLONG now = GetTickCount64();
        // v0.22: plugins 에 넣어 둔 zip 을 풀어 준다. 이 스레드(UpdateThread)에서만 --
        // 게임 스레드에서 tar 를 기다리면 그동안 화면이 멎는다. 이미 풀린 zip 은
        // 폴더 존재 확인만 하고 지나가므로 3초 주기 스캔이 부담되지 않는다.
        if (m_lastZipScan == 0 || now - m_lastZipScan >= 3000)
        {
            m_lastZipScan = now;
            extractPluginZips();
            wrapLoosePaks();   // v0.40: 낱개 pak 파일 감싸기 (같은 3초 스윕)
        }
        blackboxTick(now);   // v0.50: 블랙박스 -- 모드 시작 브래킷 + 로그 꼬리 스냅샷(5초)
        if (now - m_lastBeat >= 60000)
        {
            m_lastBeat = now;
            unsigned long long pumps = g_pumpTicks.exchange(0, std::memory_order_relaxed);
            unsigned long long maxGap = g_pumpMaxGapMs.exchange(0, std::memory_order_relaxed);
            unsigned long long pulses = g_pulseTicks.exchange(0, std::memory_order_relaxed);
            unsigned long long pulseGap = g_pulseMaxGapMs.exchange(0, std::memory_order_relaxed);
            logf("heartbeat: hits=%llu attempts=%llu softRetries=%llu done=%p hardFail=%d "
                 "pumps/min=%llu maxGap=%llums pulses/min=%llu pulseGap=%llums",
                 (unsigned long long)g_relevantHits.load(),
                 (unsigned long long)g_attempts.load(),
                 (unsigned long long)g_softRetries.load(),
                 g_doneBox.load(), (int)g_hardFail.load(), pumps, maxGap, pulses, pulseGap);
        }
    }
};

extern "C" __declspec(dllexport) auto start_mod() -> RC::CppUserModBase*
{
    return new DsCppModManager();
}

extern "C" __declspec(dllexport) auto uninstall_mod(RC::CppUserModBase* mod) -> void
{
    delete mod;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_self = inst;
        DisableThreadLibraryCalls(inst);
        logf("ATTACH");
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        logf("DETACH");
    }
    return TRUE;
}
