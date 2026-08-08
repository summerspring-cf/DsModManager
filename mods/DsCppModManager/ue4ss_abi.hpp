/*
  UE4SS CppUserModBase + Unreal 리플렉션 ABI 전사(轉寫) 헤더 -- 의존성 0.
  (DsCppProbe 의 ue4ss_abi.hpp 를 기반으로 DsCppModManager 용으로 확장)

  원본: research/ue4ss_src/RE-UE4SS/UE4SS/include/Mod/CppUserModBase.hpp
        (커밋 c838a8acaade1a0f860bdf249f039e58f4e10088 -- 게임에 설치된
         UE4SS.dll 과 정확히 같은 커밋. STATUS.md 'DsCppProbe' 절 참고)

  ⚠ 이 파일을 고칠 때의 규칙 (원본 헤더 주석에서 승계):
  - CppUserModBase 가상함수의 "이름/순서/시그니처"를 원본과 다르게 만들면
    안 된다. MSVC 는 같은 이름의 가상 오버로드를 묶어 재배열하는데, 선언
    순서가 같으면 재배열 결과도 같다.
  - 데이터 멤버(벡터 1 + 문자열 5)의 타입/순서도 원본 그대로.
  - /MD Release 로만 빌드한다(UE4SS.dll 이 MSVCP140/VCRUNTIME140 = /MD).
  - Unreal 오브젝트의 레이아웃을 아는 척하지 않는다: 포인터로만 다루고,
    멤버 접근은 전부 export 된 멤버함수 임포트로만 한다.
    (예외 1개: FText 는 아래 주석 참고 -- 실측 24바이트 + 런타임
     StaticSize() 검증을 전제로 불투명 24바이트 값 타입으로 전사)
  - sizeof 를 모르는 구조체의 값 전달 API(FCallbackOptions 확장판,
    TFieldRange, FFieldClassVariant, FName 요구 오버로드)는 쓰지 않는다.

  전사 근거: 모든 시그니처는 mods/DsCppModManager/UE4SS.def 의 실제
  익스포트 맹글링에서 역산했다(줄번호 주석). 전사가 틀리면 크래시가
  아니라 링크 에러(LNK2019)로 잡히는 것이 이 방식의 안전망이다.
*/
#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#ifndef RC_UE4SS_API
#define RC_UE4SS_API __declspec(dllimport)
#endif

namespace RC
{
    using CharType = wchar_t;
    using StringType = std::basic_string<CharType>;
    using StringViewType = std::basic_string_view<CharType>;

    namespace LuaMadeSimple
    {
        class Lua;
    }
    namespace GUI
    {
        class GUITab;
    }

    class CppUserModBase
    {
      protected:
        std::vector<std::shared_ptr<GUI::GUITab>> GUITabs{};

      public:
        StringType ModName{};
        StringType ModVersion{};
        StringType ModDescription{};
        StringType ModAuthors{};
        StringType ModIntendedSDKVersion{};

      public:
        RC_UE4SS_API CppUserModBase();
        RC_UE4SS_API virtual ~CppUserModBase();

      public:
        virtual auto on_update() -> void {}
        virtual auto on_unreal_init() -> void {}
        virtual auto on_ui_init() -> void {}
        virtual auto on_program_start() -> void {}

        // (원본에서 deprecated 표시된 구형 오버로드 4개 -- vector<Lua*>& 판)
        virtual auto on_lua_start(StringViewType mod_name,
                                  LuaMadeSimple::Lua& lua,
                                  LuaMadeSimple::Lua& main_lua,
                                  LuaMadeSimple::Lua& async_lua,
                                  std::vector<LuaMadeSimple::Lua*>& hook_luas) -> void {}
        virtual auto on_lua_start(LuaMadeSimple::Lua& lua,
                                  LuaMadeSimple::Lua& main_lua,
                                  LuaMadeSimple::Lua& async_lua,
                                  std::vector<LuaMadeSimple::Lua*>& hook_luas) -> void {}
        virtual auto on_lua_stop(StringViewType mod_name,
                                 LuaMadeSimple::Lua& lua,
                                 LuaMadeSimple::Lua& main_lua,
                                 LuaMadeSimple::Lua& async_lua,
                                 std::vector<LuaMadeSimple::Lua*>& hook_luas) -> void {}
        virtual auto on_lua_stop(LuaMadeSimple::Lua& lua,
                                 LuaMadeSimple::Lua& main_lua,
                                 LuaMadeSimple::Lua& async_lua,
                                 std::vector<LuaMadeSimple::Lua*>& hook_luas) -> void {}

        virtual auto on_dll_load(StringViewType dll_name) -> void {}
        virtual auto render_tab() -> void {}

        // (신형 오버로드 4개 -- Lua* hook_lua 판)
        virtual auto on_lua_start(StringViewType mod_name,
                                  LuaMadeSimple::Lua& lua,
                                  LuaMadeSimple::Lua& main_lua,
                                  LuaMadeSimple::Lua& async_lua,
                                  LuaMadeSimple::Lua* hook_lua) -> void {}
        virtual auto on_lua_start(LuaMadeSimple::Lua& lua,
                                  LuaMadeSimple::Lua& main_lua,
                                  LuaMadeSimple::Lua& async_lua,
                                  LuaMadeSimple::Lua* hook_lua) -> void {}
        virtual auto on_lua_stop(StringViewType mod_name,
                                 LuaMadeSimple::Lua& lua,
                                 LuaMadeSimple::Lua& main_lua,
                                 LuaMadeSimple::Lua& async_lua,
                                 LuaMadeSimple::Lua* hook_lua) -> void {}
        virtual auto on_lua_stop(LuaMadeSimple::Lua& lua,
                                 LuaMadeSimple::Lua& main_lua,
                                 LuaMadeSimple::Lua& async_lua,
                                 LuaMadeSimple::Lua* hook_lua) -> void {}

        virtual auto on_cpp_mods_loaded() -> void {}
    };
} // namespace RC

/*
  ---- RC::Unreal 전사 (DsCppModManager: 리플렉션 호출 세트) --------------

  DsCppProbe 3단계에서 실증된 것: UObject::GetName / GetFullName /
  Hook::RegisterProcessEventPreCallback.
  이번에 새로 쓰는 것(전부 UE4SS.def 에 익스포트 존재, 줄번호 주석):
  - UObjectGlobals::FindAllOf / FindFirstOf / StaticFindObject_InternalSlow
  - UObject::ProcessEvent / GetFunctionByNameInChain / GetPropertyByNameInChain
  - UObjectBase::GetClassPrivate
  - UFunction::GetParmsSize / GetReturnValueOffset / GetNumParms
  - FField::GetName, FProperty::GetOffset_Internal / GetSize
  - FText(const wchar_t*) 생성자 + FText::StaticSize()
  - RC::Unreal::IsInGameThread()

  상속 사슬 단순화 근거: 실제 사슬(UObjectBase→UObjectBaseUtility→UObject
  →UField→UStruct→UFunction/UClass, FField→FProperty)은 전부 오프셋 0
  단일 상속이라 this 조정이 없다. 맹글링은 "선언된 클래스 이름"만 쓰므로
  중간 클래스를 생략해도 링크가 맞으면 호출이 맞다(프로브에서 실증).
*/

#include <functional>

namespace RC::Unreal
{
    class UClass;
    class UFunction;
    class FProperty;

    class UObjectBase
    {
      public:
        UObjectBase() = delete;
        // UE4SS.def:1292  ?GetClassPrivate@UObjectBase@...QEAAAEAPEAVUClass@23@XZ
        __declspec(dllimport) auto GetClassPrivate() -> UClass*&;
    };

    class UObject : public UObjectBase
    {
      public:
        // UE4SS.def:1615 / 1478 (DsCppProbe 실증 완료)
        __declspec(dllimport) auto GetName() const -> std::wstring;
        __declspec(dllimport) auto GetFullName(UObject* stop_outer) const -> std::wstring;
        // UE4SS.def:2750  ?ProcessEvent@UObject@...QEAAXPEAVUFunction@23@PEAX@Z
        __declspec(dllimport) auto ProcessEvent(UFunction* function, void* parms) -> void;
        // UE4SS.def:1493  ?GetFunctionByNameInChain@...PEB_W@Z  (FName 불필요)
        __declspec(dllimport) auto GetFunctionByNameInChain(const wchar_t* name) -> UFunction*;
        // UE4SS.def:1851  ?GetPropertyByNameInChain@...PEB_W@Z  (BP 추가 프로퍼티도 잡힘)
        __declspec(dllimport) auto GetPropertyByNameInChain(const wchar_t* name) -> FProperty*;
    };

    class UFunction : public UObject
    {
      public:
        UFunction() = delete;
        // UE4SS.def:1805  uint16& GetParmsSize()   -- 멤버 참조 반환 스타일
        __declspec(dllimport) auto GetParmsSize() -> unsigned short&;
        // UE4SS.def:2056  uint16& GetReturnValueOffset()
        __declspec(dllimport) auto GetReturnValueOffset() -> unsigned short&;
        // UE4SS.def:1666  uint8& GetNumParms()
        __declspec(dllimport) auto GetNumParms() -> unsigned char&;
    };

    class UClass : public UObject
    {
      public:
        UClass() = delete;
    };

    class FField
    {
      public:
        FField() = delete;
        // UE4SS.def:1611  std::wstring FField::GetName()
        __declspec(dllimport) auto GetName() -> std::wstring;
    };

    class FProperty : public FField
    {
      public:
        FProperty() = delete;
        // UE4SS.def:1713  int32& GetOffset_Internal()
        __declspec(dllimport) auto GetOffset_Internal() -> int&;
        // UE4SS.def:2093  int32 GetSize()
        __declspec(dllimport) auto GetSize() -> int;
    };

    /*
      FText -- 유일하게 "값 타입"으로 전사하는 예외.
      근거: (1) 이 게임(UE 5.3.2)의 UTextBlock:SetText parmSize = 24 실측
            (2) UE4SS.def:3473 에 StaticSize() 가 있어 런타임 재검증 가능.
      사용 규칙: 생성만 하고 파괴하지 않는다(소멸자 미선언 = trivial).
      공유 레퍼런스 1개가 의도적으로 누수되는데, 삽입 1회당 수십 바이트라
      무해하고, 잘못된 해제(레이아웃 가정 위반)보다 압도적으로 안전하다.
      반드시 FText::StaticSize() == 24 확인 후 사용할 것.
    */
    class FText
    {
      public:
        unsigned char opaque[24];
        // UE4SS.def:135  ??0FText@Unreal@RC@@QEAA@PEB_W@Z
        __declspec(dllimport) FText(const wchar_t* str);
        // UE4SS.def:3473  ?StaticSize@FText@...SAHXZ
        __declspec(dllimport) static auto StaticSize() -> int;
    };
    static_assert(sizeof(FText) == 24, "FText 전사 크기가 실측(24)과 다름");

    namespace UObjectGlobals
    {
        // UE4SS.def:1124  UObject* FindFirstOf(const wchar_t*)
        __declspec(dllimport) auto FindFirstOf(const wchar_t* class_name) -> UObject*;
        // UE4SS.def:1115  void FindAllOf(const wchar_t*, std::vector<UObject*>&)
        __declspec(dllimport) auto FindAllOf(const wchar_t* class_name,
                                             std::vector<UObject*>& out_storage) -> void;
        // UE4SS.def:3440  UObject* StaticFindObject_InternalSlow(UClass*, UObject*, const wchar_t*, bool)
        //  = Lua 의 StaticFindObject(경로) 와 같은 고전 시그니처
        __declspec(dllimport) auto StaticFindObject_InternalSlow(UClass* object_class,
                                                                 UObject* in_outer,
                                                                 const wchar_t* name,
                                                                 bool exact_class) -> UObject*;
    } // namespace UObjectGlobals

    // UE4SS.def:2543  bool IsInGameThread()
    __declspec(dllimport) auto IsInGameThread() -> bool;

    namespace Hook
    {
        // 구형(스톡 호환) 오버로드 -- DsCppProbe 실증 완료. 등록 해제 없음.
        __declspec(dllimport) auto RegisterProcessEventPreCallback(
            std::function<void(UObject*, UFunction*, void*)> callback) -> void;
    } // namespace Hook
} // namespace RC::Unreal
