//// dllmain.cpp : Defines the entry point for the DLL application.
//#include "pch.h"
//
//BOOL APIENTRY DllMain( HMODULE hModule,
//                       DWORD  ul_reason_for_call,
//                       LPVOID lpReserved
//                     )
//{
//    switch (ul_reason_for_call)
//    {
//    case DLL_PROCESS_ATTACH:
//    case DLL_THREAD_ATTACH:
//    case DLL_THREAD_DETACH:
//    case DLL_PROCESS_DETACH:
//        break;
//    }
//    return TRUE;
//}

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdarg.h>
#include <cstring>
#include <unordered_map>
#include <functional>
#include <vector>
#include <string>
#include <algorithm>
#include <limits>
#include <utility>
#include <cstdlib>
#include <cerrno>
#include <cmath>

#include "MinHook.h"

static_assert(sizeof(void*) == 8, "ElementalSystemExpanded requires a 64-bit process.");
static_assert(sizeof(uintptr_t) == 8, "ElementalSystemExpanded requires 64-bit uintptr_t.");
static_assert(sizeof(float) == 4, "Unexpected float ABI.");
static_assert(sizeof(double) == 8, "Unexpected double ABI.");
static_assert(sizeof(bool) == 1, "Unexpected bool ABI.");

// Development-only bootstrap probe. Default builds keep this disabled.
// A dedicated probe source flips it to 1 so the exact 1.0.4 executable is
// deliberately treated as unknown and keyed-profile assistance is bypassed.
#ifndef ESE_DEV_FORCE_UNKNOWN_BOOTSTRAP
#define ESE_DEV_FORCE_UNKNOWN_BOOTSTRAP 0
#endif

#if defined(__has_include)
#  if __has_include("ElementalSystemExpanded_GeneratedPalLayout_v8.hpp")
#    include "ElementalSystemExpanded_GeneratedPalLayout_v8.hpp"
#    define ESE_HAS_GENERATED_PAL_LAYOUT 1
#    define ESE_HAS_GENERATED_ENGINE_OPTIONALS 1
#    define ESE_HAS_GENERATED_SCENE_RELATIVE_LOCATION 1
#    define ESE_HAS_GENERATED_BUILD_FINGERPRINT 1
#    define ESE_HAS_GENERATED_LIGHT_TARGET_LAYOUT 1
#  elif __has_include("ElementalSystemExpanded_GeneratedPalLayout_v7.hpp")
#    include "ElementalSystemExpanded_GeneratedPalLayout_v7.hpp"
#    define ESE_HAS_GENERATED_PAL_LAYOUT 1
#    define ESE_HAS_GENERATED_ENGINE_OPTIONALS 1
#    define ESE_HAS_GENERATED_SCENE_RELATIVE_LOCATION 1
#    define ESE_HAS_GENERATED_BUILD_FINGERPRINT 1
#    define ESE_HAS_GENERATED_LIGHT_TARGET_LAYOUT 0
#  elif __has_include("ElementalSystemExpanded_GeneratedPalLayout_v6.hpp")
#    include "ElementalSystemExpanded_GeneratedPalLayout_v6.hpp"
#    define ESE_HAS_GENERATED_PAL_LAYOUT 1
#    define ESE_HAS_GENERATED_ENGINE_OPTIONALS 1
#    define ESE_HAS_GENERATED_SCENE_RELATIVE_LOCATION 1
#    define ESE_HAS_GENERATED_BUILD_FINGERPRINT 0
#    define ESE_HAS_GENERATED_LIGHT_TARGET_LAYOUT 0
#  elif __has_include("ElementalSystemExpanded_GeneratedPalLayout_v5.hpp")
#    include "ElementalSystemExpanded_GeneratedPalLayout_v5.hpp"
#    define ESE_HAS_GENERATED_PAL_LAYOUT 1
#    define ESE_HAS_GENERATED_ENGINE_OPTIONALS 1
#    define ESE_HAS_GENERATED_SCENE_RELATIVE_LOCATION 0
#    define ESE_HAS_GENERATED_BUILD_FINGERPRINT 0
#    define ESE_HAS_GENERATED_LIGHT_TARGET_LAYOUT 0
#  elif __has_include("ElementalSystemExpanded_GeneratedPalLayout_v2.hpp")
#    include "ElementalSystemExpanded_GeneratedPalLayout_v2.hpp"
#    define ESE_HAS_GENERATED_PAL_LAYOUT 1
#    define ESE_HAS_GENERATED_ENGINE_OPTIONALS 1
#    define ESE_HAS_GENERATED_SCENE_RELATIVE_LOCATION 0
#    define ESE_HAS_GENERATED_BUILD_FINGERPRINT 0
#    define ESE_HAS_GENERATED_LIGHT_TARGET_LAYOUT 0
#  elif __has_include("ElementalSystemExpanded_GeneratedPalLayout.hpp")
#    include "ElementalSystemExpanded_GeneratedPalLayout.hpp"
#    define ESE_HAS_GENERATED_PAL_LAYOUT 1
#    define ESE_HAS_GENERATED_ENGINE_OPTIONALS 0
#    define ESE_HAS_GENERATED_SCENE_RELATIVE_LOCATION 0
#    define ESE_HAS_GENERATED_BUILD_FINGERPRINT 0
#    define ESE_HAS_GENERATED_LIGHT_TARGET_LAYOUT 0
#  else
#    define ESE_HAS_GENERATED_PAL_LAYOUT 0
#    define ESE_HAS_GENERATED_ENGINE_OPTIONALS 0
#    define ESE_HAS_GENERATED_SCENE_RELATIVE_LOCATION 0
#    define ESE_HAS_GENERATED_BUILD_FINGERPRINT 0
#    define ESE_HAS_GENERATED_LIGHT_TARGET_LAYOUT 0
#  endif
#else
#  define ESE_HAS_GENERATED_PAL_LAYOUT 0
#  define ESE_HAS_GENERATED_ENGINE_OPTIONALS 0
#  define ESE_HAS_GENERATED_SCENE_RELATIVE_LOCATION 0
#  define ESE_HAS_GENERATED_BUILD_FINGERPRINT 0
#  define ESE_HAS_GENERATED_LIGHT_TARGET_LAYOUT 0
#endif

#pragma comment(lib, "psapi.lib")

// ---------------------------------------------------------
// Deterministic artifact paths
//
// Never use the process current-working-directory for persistent resolver
// state. Palworld/UE/plugin loaders may change CWD after startup, which made
// Stage 5.1 read the profile from one directory and write it into another.
// All artifacts are rooted beside this DLL instead.
// ---------------------------------------------------------
static HMODULE g_SelfModule_104 = nullptr;
static std::string g_ArtifactDirectory_104;
static std::string g_ProfilePath_104;
static std::string g_LegacyProfilePath_104;
static std::string g_MigrationReportPath_104;
static std::string g_ResolverReportPath_104;
static std::string g_UpdateDiagnosticsPath_104;
static std::string g_LogPath_104;
static std::string g_RuntimeSnapshotPath_104;
static std::string g_DebugMarkerPath_104;
static std::string g_DiagnosticsInitStatus_104 = "not-initialized";
static std::string g_ArtifactPathStatus_104 = "not-initialized";
static bool g_DebugDiagnosticsEnabled_104 = false;
static bool g_ForceFailureArtifacts_104 = false;
static volatile LONG g_StartRequested_104 = 0;
// Retained intentionally until uninstall so hot-unload can wait for the
// asynchronous initialization thread to exit before this DLL is torn down.
static HANDLE g_InitThread_104 = nullptr;

static std::string JoinArtifactPath_104(const std::string& directory, const char* leaf)
{
    if (!leaf || !*leaf)
        return directory;
    if (directory.empty())
        return std::string(leaf);
    const char last = directory.back();
    if (last == '\\' || last == '/')
        return directory + leaf;
    return directory + "\\" + leaf;
}

static bool InitializeArtifactPaths_104()
{
    // MAX_PATH-sized stack buffers are too small for extended Windows paths,
    // while a 32 KiB local array causes MSVC C6262-style large-stack warnings.
    // Keep the extended-path capacity, but allocate the scratch buffer on heap.
    static constexpr DWORD kModulePathCapacity_104 = 32768;
    std::vector<char> modulePath(kModulePathCapacity_104, '\0');

    const DWORD n = GetModuleFileNameA(
        g_SelfModule_104,
        modulePath.data(),
        kModulePathCapacity_104);

    if (!n || n >= kModulePathCapacity_104) {
        g_ArtifactPathStatus_104 = "dll-path-resolution-failed";
        return false;
    }

    std::string full(modulePath.data(), modulePath.data() + n);
    const size_t slash = full.find_last_of("\\/");
    g_ArtifactDirectory_104 =
        slash == std::string::npos ? std::string(".") : full.substr(0, slash);

    g_LegacyProfilePath_104 = JoinArtifactPath_104(
        g_ArtifactDirectory_104,
        "ElementalSystemExpanded_build_profile.txt");
    g_ProfilePath_104.clear();
    g_MigrationReportPath_104 = JoinArtifactPath_104(
        g_ArtifactDirectory_104,
        "ElementalSystemExpanded_migration_report.txt");
    g_ResolverReportPath_104 = JoinArtifactPath_104(
        g_ArtifactDirectory_104,
        "ElementalSystemExpanded_resolver_report.txt");
    g_UpdateDiagnosticsPath_104 = JoinArtifactPath_104(
        g_ArtifactDirectory_104,
        "ElementalSystemExpanded_update_diagnostics.txt");
    g_LogPath_104 = JoinArtifactPath_104(
        g_ArtifactDirectory_104,
        "ElementalSystemExpanded_log.txt");
    g_RuntimeSnapshotPath_104 = JoinArtifactPath_104(
        g_ArtifactDirectory_104,
        "ElementalSystemExpanded_runtime_snapshot.txt");
    g_DebugMarkerPath_104 = JoinArtifactPath_104(
        g_ArtifactDirectory_104,
        "debug_enabled.txt");

    g_ArtifactPathStatus_104 = "dll-directory-absolute";
    return true;
}

static const char* ArtifactPathOrFallback_104(
    const std::string& absolutePath,
    const char* fallback)
{
    return absolutePath.empty() ? fallback : absolutePath.c_str();
}

// ---------------------------------------------------------
// Shipping logger / opt-in diagnostics
//
// Normal shipping mode keeps gameplay and concise warnings/errors, but avoids
// the verbose trace stream and periodic diagnostic I/O. Creating an empty file
// named "debug_enabled.txt" beside this DLL enables the full developer framework:
// resolver/migration/update reports, periodic runtime snapshots, and verbose
// trace logging. The marker is sampled once during startup.
// ---------------------------------------------------------
bool g_LogToTxt = false;

static bool FileExistsRegular_104(const std::string& path)
{
    if (path.empty())
        return false;

    const DWORD attributes = GetFileAttributesA(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

static void InitializeDebugDiagnosticsMode_104()
{
    g_DebugDiagnosticsEnabled_104 =
        FileExistsRegular_104(g_DebugMarkerPath_104);

    // Keep gameplay hooks free of synchronous log-file I/O even in debug mode.
    // Developer traces go to debugger/console; structured artifacts are written
    // only from startup code or the low-priority snapshot worker.
    g_LogToTxt = false;
}

static void EmitLogV_104(
    bool alwaysEmit,
    const char* format,
    va_list args)
{
    if (!alwaysEmit && !g_DebugDiagnosticsEnabled_104)
        return;

    char buffer[1024]{};
    vsnprintf(buffer, sizeof(buffer), format, args);

    OutputDebugStringA(buffer);

    HANDLE hStdOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hStdOut && hStdOut != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(
            hStdOut,
            buffer,
            static_cast<DWORD>(strlen(buffer)),
            &written,
            nullptr);
    }

    if (!g_LogToTxt)
        return;

    SYSTEMTIME st{};
    GetLocalTime(&st);
    char timeBuffer[32]{};
    snprintf(
        timeBuffer,
        sizeof(timeBuffer),
        "[%02d:%02d:%02d] ",
        st.wHour,
        st.wMinute,
        st.wSecond);

    HANDLE hFile = CreateFileA(
        ArtifactPathOrFallback_104(
            g_LogPath_104,
            "ElementalSystemExpanded_log.txt"),
        FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);

    if (hFile != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(
            hFile,
            timeBuffer,
            static_cast<DWORD>(strlen(timeBuffer)),
            &written,
            nullptr);
        WriteFile(
            hFile,
            buffer,
            static_cast<DWORD>(strlen(buffer)),
            &written,
            nullptr);
        CloseHandle(hFile);
    }
}

// Verbose developer trace. Compiled into the release DLL but silent unless the
// marker file is present, so the same binary can be used for field diagnosis.
void ModLog(const char* format, ...)
{
    if (!g_DebugDiagnosticsEnabled_104)
        return;

    va_list args;
    va_start(args, format);
    EmitLogV_104(false, format, args);
    va_end(args);
}

// Concise shipping-visible log for failures/degraded capabilities.
static void ShipLog_104(const char* format, ...)
{
    va_list args;
    va_start(args, format);
    EmitLogV_104(true, format, args);
    va_end(args);
}

// ---------------------------------------------------------
// Durable build profile / resolver state
//
// Gameplay code should not own version-specific addresses. 1.0.4 values are
// retained only as known-good hints/validation answers. The resolver may
// replace native RVAs and virtual slots at startup before any gameplay hook is
// enabled. Layout offsets are centralized here and can be regenerated from a
// fresh SDK with the companion script shipped beside this source.
// ---------------------------------------------------------
namespace Known104 {
    namespace Build {
        static constexpr DWORD TimeDateStamp = 0x6A97E443;
        static constexpr DWORD SizeOfImage = 0x0A011000;
        static constexpr uint64_t TextHash = 0x1888AD3E278F8B12ull;
    }
    namespace Rva {
        static constexpr uintptr_t MatchupCallsite = 0x32C62A5;
        static constexpr uintptr_t MatchupHelper = 0x32FB570;
        static constexpr uintptr_t ElementBuildup_Call = 0x28E0E74;
        static constexpr uintptr_t ElementBuildup = 0x2C0C680;
        static constexpr uintptr_t RawEffectCaller = 0x2C0C220;
        static constexpr uintptr_t AddStatus_FromBuildup_Call = 0x2C0C877;
        static constexpr uintptr_t AddStatus = 0x2CCA0A0;

        static constexpr uintptr_t SetActionClassParameter_Call = 0x281FAF2;
        static constexpr uintptr_t SetActionClassParameter_Native = 0x2BD4830;
        static constexpr uintptr_t PlayAction_Call = 0x2810304;
        static constexpr uintptr_t PlayAction_Native = 0x2BCF7A0;
        static constexpr uintptr_t SetJumpDisableFlag_Call = 0x28B0917;
        static constexpr uintptr_t SetJumpDisableFlag_Native = 0x2BFA430;
        static constexpr uintptr_t SetStepDisableFlag_Call = 0x28B1557;
        static constexpr uintptr_t SetStepDisableFlag_Native = 0x2BFB390;
        static constexpr uintptr_t SetMoveDisableFlag_Call = 0x28B0C57;
        static constexpr uintptr_t SetMoveDisableFlag_Native = 0x2BFA5D0;
        static constexpr uintptr_t SetWalkSpeedMultiplier_Call = 0x28B1A34;
        static constexpr uintptr_t SetWalkSpeedMultiplier_Native = 0x2BFB5E0;
        static constexpr uintptr_t SetYawRotatorMultiplier_Call = 0x28B1BD4;
        static constexpr uintptr_t SetYawRotatorMultiplier_Native = 0x2BFB740;
        static constexpr uintptr_t SetDisableAimFlag_Call = 0x2AEC5DC;
        static constexpr uintptr_t SetDisableAimFlag_Native = 0x2CB9E50;
        static constexpr uintptr_t SetDisableShootFlag_Call = 0x2AECC2C;
        static constexpr uintptr_t SetDisableShootFlag_Native = 0x2CBA450;
        static constexpr uintptr_t SetDisableChangeWeaponFlag_Call = 0x2AEC7CC;
        static constexpr uintptr_t SetDisableChangeWeaponFlag_Native = 0x2CB9F80;
        static constexpr uintptr_t AddVisualEffect_Call = 0x2B9C8B0;
        static constexpr uintptr_t AddVisualEffect_Native = 0x2CCAD10;
        static constexpr uintptr_t AddVisualEffectLocal_Call = 0x2B9CC70;
        static constexpr uintptr_t AddVisualEffectLocal_Native = 0x2CCB110;
        static constexpr uintptr_t RemoveVisualEffectLocal_Call = 0x2B9DBCB;
        static constexpr uintptr_t RemoveVisualEffectLocal_Native = 0x2CE1650;

        static constexpr uintptr_t SetComponentTickEnabled_Dispatch = 0x510E655;
        static constexpr uintptr_t StopAnimMontage_Dispatch = 0x50A4183;
        static constexpr uintptr_t StatusTick_Dispatch = 0x2845912;
    }

    namespace VSlot {
        static constexpr uintptr_t SetComponentTickEnabled = 0x3B8;
        static constexpr uintptr_t StopAnimMontage = 0x888;
        static constexpr uintptr_t StatusTick = 0x2C0;
    }

    namespace Offset {
        static constexpr uintptr_t UObject_OuterPrivate = 0x20;

        static constexpr uintptr_t DamageInfo_AttackElement = 0x30;
        static constexpr uintptr_t DamageInfo_EffectType1 = 0x90;
        static constexpr uintptr_t DamageInfo_EffectValue1 = 0x94;
        static constexpr uintptr_t DamageInfo_EffectType2 = 0x9C;
        static constexpr uintptr_t DamageInfo_EffectValue2 = 0xA0;

        static constexpr uintptr_t Character_RootComponent = 0x198;
        static constexpr uintptr_t Character_CharacterParameterComponent = 0x630;
        static constexpr uintptr_t Character_StaticCharacterParameterComponent = 0x638;
        static constexpr uintptr_t Character_DamageReactionComponent = 0x640;
        static constexpr uintptr_t Character_StatusComponent = 0x648;
        static constexpr uintptr_t Character_VisualEffectComponent = 0x678;

        static constexpr uintptr_t CharacterParameter_ElementType1 = 0xD8;
        static constexpr uintptr_t CharacterParameter_ElementType2 = 0xD9;
        static constexpr uintptr_t StaticCharacterParameter_IsPal = 0x4A0;

        static constexpr uintptr_t StatusComponent_ExecutionStatusList = 0x110;
        static constexpr uintptr_t StatusBase_IsEndStatus = 0x48;
        static constexpr uintptr_t StatusBase_StatusID = 0x90;
        static constexpr uintptr_t StatusBase_Duration = 0xA4;
        static constexpr uintptr_t StatusBase_DurationTimer = 0xAC;
        static constexpr uintptr_t StatusBase_NativeSize = 0xB0;

        static constexpr uintptr_t RootComponent_WorldLocation = 0x260;
        // Reflected USceneComponent field emitted by the user's 1.0.4 Engine.hpp.
        static constexpr uintptr_t SceneComponent_RelativeLocation = 0x128;

        static constexpr uintptr_t VisualEffectComponent_ExecutionVisualEffects = 0x120;
        static constexpr uintptr_t VisualEffectBase_IsEnd = 0x28;
        static constexpr uintptr_t VisualEffectBase_ID = 0x50;
    }
}

namespace ActiveLayout {
#if ESE_HAS_GENERATED_LIGHT_TARGET_LAYOUT
    static constexpr uintptr_t Character_CharacterParameterComponent =
        EseGeneratedPalLayout::Character_CharacterParameterComponent;
    static constexpr uintptr_t Character_StaticCharacterParameterComponent =
        EseGeneratedPalLayout::Character_StaticCharacterParameterComponent;
    static constexpr uintptr_t CharacterParameter_ElementType1 =
        EseGeneratedPalLayout::CharacterParameter_ElementType1;
    static constexpr uintptr_t CharacterParameter_ElementType2 =
        EseGeneratedPalLayout::CharacterParameter_ElementType2;
    static constexpr uintptr_t StaticCharacterParameter_IsPal =
        EseGeneratedPalLayout::StaticCharacterParameter_IsPal;
#else
    // Exact 1.0.4 oracle fallback only. Unknown builds are rejected below
    // unless these fields come from a fingerprint-bound v8 layout.
    static constexpr uintptr_t Character_CharacterParameterComponent =
        Known104::Offset::Character_CharacterParameterComponent;
    static constexpr uintptr_t Character_StaticCharacterParameterComponent =
        Known104::Offset::Character_StaticCharacterParameterComponent;
    static constexpr uintptr_t CharacterParameter_ElementType1 =
        Known104::Offset::CharacterParameter_ElementType1;
    static constexpr uintptr_t CharacterParameter_ElementType2 =
        Known104::Offset::CharacterParameter_ElementType2;
    static constexpr uintptr_t StaticCharacterParameter_IsPal =
        Known104::Offset::StaticCharacterParameter_IsPal;
#endif

#if ESE_HAS_GENERATED_ENGINE_OPTIONALS
    static constexpr uintptr_t UObject_OuterPrivate =
        EseGeneratedPalLayout::Has_UObject_OuterPrivate
        ? EseGeneratedPalLayout::UObject_OuterPrivate
        : Known104::Offset::UObject_OuterPrivate;
    static constexpr uintptr_t Character_RootComponent =
        EseGeneratedPalLayout::Has_Actor_RootComponent
        ? EseGeneratedPalLayout::Actor_RootComponent
        : Known104::Offset::Character_RootComponent;
    static constexpr uintptr_t RootComponent_WorldLocation =
        EseGeneratedPalLayout::Has_RootComponent_WorldLocation
        ? EseGeneratedPalLayout::RootComponent_WorldLocation
        : Known104::Offset::RootComponent_WorldLocation;
#else
    static constexpr uintptr_t UObject_OuterPrivate = Known104::Offset::UObject_OuterPrivate;
    static constexpr uintptr_t Character_RootComponent = Known104::Offset::Character_RootComponent;
    static constexpr uintptr_t RootComponent_WorldLocation = Known104::Offset::RootComponent_WorldLocation;
#endif

#if ESE_HAS_GENERATED_SCENE_RELATIVE_LOCATION
    static constexpr uintptr_t SceneComponent_RelativeLocation =
        EseGeneratedPalLayout::Has_SceneComponent_RelativeLocation
        ? EseGeneratedPalLayout::SceneComponent_RelativeLocation
        : Known104::Offset::SceneComponent_RelativeLocation;
    static constexpr bool HasGeneratedSceneRelativeLocation =
        EseGeneratedPalLayout::Has_SceneComponent_RelativeLocation;
#else
    static constexpr uintptr_t SceneComponent_RelativeLocation =
        Known104::Offset::SceneComponent_RelativeLocation;
    static constexpr bool HasGeneratedSceneRelativeLocation = false;
#endif

#if ESE_HAS_GENERATED_SCENE_RELATIVE_LOCATION
    static constexpr uintptr_t SceneComponent_AttachParent =
        EseGeneratedPalLayout::Has_SceneComponent_AttachParent
        ? EseGeneratedPalLayout::SceneComponent_AttachParent
        : 0;
    static constexpr bool HasGeneratedSceneAttachParent =
        EseGeneratedPalLayout::Has_SceneComponent_AttachParent;
#else
    static constexpr uintptr_t SceneComponent_AttachParent = 0;
    static constexpr bool HasGeneratedSceneAttachParent = false;
#endif

#if ESE_HAS_GENERATED_PAL_LAYOUT
    static constexpr uintptr_t DamageInfo_AttackElement = EseGeneratedPalLayout::DamageInfo_AttackElement;
    static constexpr uintptr_t DamageInfo_EffectType1 = EseGeneratedPalLayout::DamageInfo_EffectType1;
    static constexpr uintptr_t DamageInfo_EffectValue1 = EseGeneratedPalLayout::DamageInfo_EffectValue1;
    static constexpr uintptr_t DamageInfo_EffectType2 = EseGeneratedPalLayout::DamageInfo_EffectType2;
    static constexpr uintptr_t DamageInfo_EffectValue2 = EseGeneratedPalLayout::DamageInfo_EffectValue2;
    static constexpr uintptr_t Character_DamageReactionComponent = EseGeneratedPalLayout::Character_DamageReactionComponent;
    static constexpr uintptr_t Character_StatusComponent = EseGeneratedPalLayout::Character_StatusComponent;
    static constexpr uintptr_t Character_VisualEffectComponent = EseGeneratedPalLayout::Character_VisualEffectComponent;
    static constexpr uintptr_t StatusComponent_ExecutionStatusList = EseGeneratedPalLayout::StatusComponent_ExecutionStatusList;
    static constexpr uintptr_t StatusBase_IsEndStatus = EseGeneratedPalLayout::StatusBase_IsEndStatus;
    static constexpr uintptr_t StatusBase_StatusID = EseGeneratedPalLayout::StatusBase_StatusID;
    static constexpr uintptr_t StatusBase_Duration = EseGeneratedPalLayout::StatusBase_Duration;
    static constexpr uintptr_t StatusBase_DurationTimer = EseGeneratedPalLayout::StatusBase_DurationTimer;
    static constexpr uintptr_t StatusBase_NativeSize = EseGeneratedPalLayout::StatusBase_NativeSize;
    static constexpr uintptr_t VisualEffectComponent_ExecutionVisualEffects = EseGeneratedPalLayout::VisualEffectComponent_ExecutionVisualEffects;
    static constexpr uintptr_t VisualEffectBase_IsEnd = EseGeneratedPalLayout::VisualEffectBase_IsEnd;
    static constexpr uintptr_t VisualEffectBase_ID = EseGeneratedPalLayout::VisualEffectBase_ID;
#else
    static constexpr uintptr_t DamageInfo_AttackElement = Known104::Offset::DamageInfo_AttackElement;
    static constexpr uintptr_t DamageInfo_EffectType1 = Known104::Offset::DamageInfo_EffectType1;
    static constexpr uintptr_t DamageInfo_EffectValue1 = Known104::Offset::DamageInfo_EffectValue1;
    static constexpr uintptr_t DamageInfo_EffectType2 = Known104::Offset::DamageInfo_EffectType2;
    static constexpr uintptr_t DamageInfo_EffectValue2 = Known104::Offset::DamageInfo_EffectValue2;
    static constexpr uintptr_t Character_DamageReactionComponent = Known104::Offset::Character_DamageReactionComponent;
    static constexpr uintptr_t Character_StatusComponent = Known104::Offset::Character_StatusComponent;
    static constexpr uintptr_t Character_VisualEffectComponent = Known104::Offset::Character_VisualEffectComponent;
    static constexpr uintptr_t StatusComponent_ExecutionStatusList = Known104::Offset::StatusComponent_ExecutionStatusList;
    static constexpr uintptr_t StatusBase_IsEndStatus = Known104::Offset::StatusBase_IsEndStatus;
    static constexpr uintptr_t StatusBase_StatusID = Known104::Offset::StatusBase_StatusID;
    static constexpr uintptr_t StatusBase_Duration = Known104::Offset::StatusBase_Duration;
    static constexpr uintptr_t StatusBase_DurationTimer = Known104::Offset::StatusBase_DurationTimer;
    static constexpr uintptr_t StatusBase_NativeSize = Known104::Offset::StatusBase_NativeSize;
    static constexpr uintptr_t VisualEffectComponent_ExecutionVisualEffects = Known104::Offset::VisualEffectComponent_ExecutionVisualEffects;
    static constexpr uintptr_t VisualEffectBase_IsEnd = Known104::Offset::VisualEffectBase_IsEnd;
    static constexpr uintptr_t VisualEffectBase_ID = Known104::Offset::VisualEffectBase_ID;
#endif
}

namespace ActiveIds {
#if ESE_HAS_GENERATED_PAL_LAYOUT
    static constexpr uint8_t Effect_Burn = EseGeneratedPalLayout::Effect_Burn;
    static constexpr uint8_t Effect_Wetness = EseGeneratedPalLayout::Effect_Wetness;
    static constexpr uint8_t Effect_Freeze = EseGeneratedPalLayout::Effect_Freeze;
    static constexpr uint8_t Effect_Electrical = EseGeneratedPalLayout::Effect_Electrical;
    static constexpr uint8_t Effect_Muddy = EseGeneratedPalLayout::Effect_Muddy;
    static constexpr uint8_t Effect_IvyCling = EseGeneratedPalLayout::Effect_IvyCling;
    static constexpr uint8_t Effect_Darkness = EseGeneratedPalLayout::Effect_Darkness;

    static constexpr uint8_t Element_None = EseGeneratedPalLayout::Element_None;
    static constexpr uint8_t Element_Normal = EseGeneratedPalLayout::Element_Normal;
    static constexpr uint8_t Element_Fire = EseGeneratedPalLayout::Element_Fire;
    static constexpr uint8_t Element_Water = EseGeneratedPalLayout::Element_Water;
    static constexpr uint8_t Element_Leaf = EseGeneratedPalLayout::Element_Leaf;
    static constexpr uint8_t Element_Electricity = EseGeneratedPalLayout::Element_Electricity;
    static constexpr uint8_t Element_Ice = EseGeneratedPalLayout::Element_Ice;
    static constexpr uint8_t Element_Earth = EseGeneratedPalLayout::Element_Earth;
    static constexpr uint8_t Element_Dark = EseGeneratedPalLayout::Element_Dark;
    static constexpr uint8_t Element_Dragon = EseGeneratedPalLayout::Element_Dragon;

    static constexpr uint8_t Status_Burn = EseGeneratedPalLayout::Status_Burn;
    static constexpr uint8_t Status_Wetness = EseGeneratedPalLayout::Status_Wetness;
    static constexpr uint8_t Status_Freeze = EseGeneratedPalLayout::Status_Freeze;
    static constexpr uint8_t Status_Electrical = EseGeneratedPalLayout::Status_Electrical;
    static constexpr uint8_t Status_Muddy = EseGeneratedPalLayout::Status_Muddy;
    static constexpr uint8_t Status_IvyCling = EseGeneratedPalLayout::Status_IvyCling;
    static constexpr uint8_t Status_Darkness = EseGeneratedPalLayout::Status_Darkness;
    static constexpr uint8_t Status_VanillaWetFreeze = EseGeneratedPalLayout::Status_VanillaWetFreeze;
    static constexpr uint8_t Status_VanillaWetFreezeResist = EseGeneratedPalLayout::Status_VanillaWetFreezeResist;
    static constexpr uint8_t VisualEffect_IceCondition = EseGeneratedPalLayout::VisualEffect_IceCondition;
#else
    static constexpr uint8_t Effect_Burn = 4;
    static constexpr uint8_t Effect_Wetness = 5;
    static constexpr uint8_t Effect_Freeze = 6;
    static constexpr uint8_t Effect_Electrical = 7;
    static constexpr uint8_t Effect_Muddy = 8;
    static constexpr uint8_t Effect_IvyCling = 9;
    static constexpr uint8_t Effect_Darkness = 10;

    static constexpr uint8_t Element_None = 0;
    static constexpr uint8_t Element_Normal = 1;
    static constexpr uint8_t Element_Fire = 2;
    static constexpr uint8_t Element_Water = 3;
    static constexpr uint8_t Element_Leaf = 4;
    static constexpr uint8_t Element_Electricity = 5;
    static constexpr uint8_t Element_Ice = 6;
    static constexpr uint8_t Element_Earth = 7;
    static constexpr uint8_t Element_Dark = 8;
    static constexpr uint8_t Element_Dragon = 9;

    static constexpr uint8_t Status_Burn = 19;
    static constexpr uint8_t Status_Wetness = 20;
    static constexpr uint8_t Status_Freeze = 21;
    static constexpr uint8_t Status_Electrical = 22;
    static constexpr uint8_t Status_Muddy = 23;
    static constexpr uint8_t Status_IvyCling = 24;
    static constexpr uint8_t Status_Darkness = 25;
    static constexpr uint8_t Status_VanillaWetFreeze = 59;
    static constexpr uint8_t Status_VanillaWetFreezeResist = 60;
    static constexpr uint8_t VisualEffect_IceCondition = 17;
#endif
}

enum class EResolveConfidence_104 : uint8_t {
    Exact,
    Strong,
    RuntimeValidated,
    ProfileStatic,
    Deferred,
    Failed
};

static const char* ResolveConfidenceName_104(EResolveConfidence_104 value)
{
    switch (value) {
    case EResolveConfidence_104::Exact: return "EXACT";
    case EResolveConfidence_104::Strong: return "STRONG";
    case EResolveConfidence_104::RuntimeValidated: return "RUNTIME_VALIDATED";
    case EResolveConfidence_104::ProfileStatic: return "PROFILE_STATIC";
    case EResolveConfidence_104::Deferred: return "DEFERRED";
    default: return "FAILED";
    }
}

struct FResolverRecord_104 {
    std::string Name;
    std::string Method;
    EResolveConfidence_104 Confidence = EResolveConfidence_104::Failed;
    uintptr_t Address = 0;
    uintptr_t Rva = 0;
    std::string Detail;
};

static std::vector<FResolverRecord_104> g_ResolverRecords_104;
static SRWLOCK g_ResolverRecordsLock_104 = SRWLOCK_INIT;
static uintptr_t g_ModuleBase_104 = 0;
static intptr_t g_RvaShiftHint_104 = 0;
static std::vector<intptr_t> g_AnchorRvaShifts_104;

static uintptr_t g_Resolved_ElementBuildup_104 = 0;
static uintptr_t g_Resolved_AddStatus_104 = 0;
static uintptr_t g_Resolved_MatchupHelper_104 = 0;
static uintptr_t g_Resolved_RawEffectCaller_104 = 0;
static uintptr_t g_Resolved_AddVisualEffect_104 = 0;
static uintptr_t g_Resolved_AddVisualEffectLocal_104 = 0;
static uintptr_t g_Resolved_RemoveVisualEffectLocal_104 = 0;

static uintptr_t g_VSlot_SetComponentTickEnabled_104 = Known104::VSlot::SetComponentTickEnabled;
static uintptr_t g_VSlot_StopAnimMontage_104 = Known104::VSlot::StopAnimMontage;
static uintptr_t g_VSlot_StatusTick_104 = Known104::VSlot::StatusTick;

struct FBuildFingerprint_104 {
    DWORD TimeDateStamp = 0;
    DWORD SizeOfImage = 0;
    DWORD TextRva = 0;
    DWORD TextSize = 0;
    uint64_t TextHash = 0;
};


struct FBuildProfileCache_104 {
    bool Loaded = false;
    bool FingerprintMatched = false;
    FBuildFingerprint_104 Fingerprint{};
    std::unordered_map<std::string, uintptr_t> Rvas;
    std::unordered_map<std::string, uintptr_t> Callsites;
    std::unordered_map<std::string, uintptr_t> VSlots;
    std::unordered_map<std::string, uintptr_t> VDispatches;
};

static FBuildProfileCache_104 g_BuildProfileCache_104;
static std::unordered_map<std::string, uintptr_t> g_ProfileOutputRvas_104;
static std::unordered_map<std::string, uintptr_t> g_ProfileOutputCallsites_104;
static std::unordered_map<std::string, uintptr_t> g_ProfileOutputVSlots_104;
static std::unordered_map<std::string, uintptr_t> g_ProfileOutputVDispatches_104;
static std::string g_ProfileCacheStatus_104 = "not-loaded";
static std::string g_ProfileWriteStatus_104 = "not-written";
static FBuildFingerprint_104 g_CurrentFingerprint_104{};

// Runtime-discovered UObject ownership link. The resolver report may be
// written before the first elemental hit performs discovery.
static SRWLOCK g_UObjectOuterOffsetLock_104 = SRWLOCK_INIT;
static uintptr_t g_RuntimeUObjectOuterOffset_104 = 0;
static bool g_RuntimeUObjectOuterLogged_104 = false;

// Container ABI guard counters. These are cheap atomics updated only when
// status/VFX arrays are inspected, not per frame.
static volatile LONG g_StatusArrayValidationPasses_104 = 0;
static volatile LONG g_StatusArrayValidationFailures_104 = 0;
static volatile LONG g_VfxArrayValidationPasses_104 = 0;
static volatile LONG g_VfxArrayValidationFailures_104 = 0;

// Persistent diagnostics are maintained by a dedicated low-priority worker.
// Gameplay paths only mutate in-memory state and optionally signal the worker;
// they never perform disk I/O.
static HANDLE g_RuntimeSnapshotThread_104 = nullptr;
static HANDLE g_RuntimeSnapshotStopEvent_104 = nullptr;
static HANDLE g_RuntimeSnapshotWakeEvent_104 = nullptr;
static volatile LONG g_RuntimeSnapshotDirty_104 = 1;
static volatile LONG g_RuntimeSnapshotWrites_104 = 0;
static volatile LONG g_RuntimeSnapshotWriteFailures_104 = 0;

static void MarkRuntimeSnapshotDirty_104();
static bool WriteWholeFileDurable_104(const char* finalPath, const std::string& data);

// Runtime-discovered UObject ownership link. Declared here because the resolver
// report may be written before the first elemental hit performs discovery.
static bool FingerprintsEqual_104(
    const FBuildFingerprint_104& a,
    const FBuildFingerprint_104& b)
{
    return a.TimeDateStamp == b.TimeDateStamp &&
        a.SizeOfImage == b.SizeOfImage &&
        a.TextHash == b.TextHash;
}

static bool IsActualKnown104Fingerprint_104(const FBuildFingerprint_104& fp)
{
    return fp.TimeDateStamp == Known104::Build::TimeDateStamp &&
        fp.SizeOfImage == Known104::Build::SizeOfImage &&
        fp.TextHash == Known104::Build::TextHash;
}

static bool IsKnown104Fingerprint_104(const FBuildFingerprint_104& fp)
{
#if ESE_DEV_FORCE_UNKNOWN_BOOTSTRAP
    (void)fp;
    return false;
#else
    return IsActualKnown104Fingerprint_104(fp);
#endif
}

static uintptr_t ParseHexU64_104(const char* text)
{
    if (!text)
        return 0;
    return static_cast<uintptr_t>(strtoull(text, nullptr, 0));
}

static std::string BuildProfileLeafForFingerprint_104(
    const FBuildFingerprint_104& fp)
{
    char leaf[192]{};
    snprintf(
        leaf,
        sizeof(leaf),
        "ElementalSystemExpanded_build_profile_%08lX_%08lX_%016llX.txt",
        static_cast<unsigned long>(fp.TimeDateStamp),
        static_cast<unsigned long>(fp.SizeOfImage),
        static_cast<unsigned long long>(fp.TextHash));
    return std::string(leaf);
}

static void SelectCurrentBuildProfilePath_104(
    const FBuildFingerprint_104& fp)
{
    const std::string leaf = BuildProfileLeafForFingerprint_104(fp);
    g_ProfilePath_104 = JoinArtifactPath_104(
        g_ArtifactDirectory_104,
        leaf.c_str());
}

static bool ParseBuildProfileFile_104(
    const char* path,
    FBuildProfileCache_104* out)
{
    if (!path || !*path || !out)
        return false;

    FILE* file = nullptr;
#if defined(_MSC_VER)
    fopen_s(&file, path, "r");
#else
    file = fopen(path, "r");
#endif
    if (!file)
        return false;

    FBuildProfileCache_104 temp{};
    temp.Loaded = true;

    char line[768]{};
    while (fgets(line, sizeof(line), file)) {
        size_t len = strlen(line);
        while (len && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';

        char* eq = strchr(line, '=');
        if (!eq)
            continue;
        *eq = '\0';
        const char* key = line;
        const char* value = eq + 1;

        if (strcmp(key, "Fingerprint.TimeDateStamp") == 0)
            temp.Fingerprint.TimeDateStamp = static_cast<DWORD>(ParseHexU64_104(value));
        else if (strcmp(key, "Fingerprint.SizeOfImage") == 0)
            temp.Fingerprint.SizeOfImage = static_cast<DWORD>(ParseHexU64_104(value));
        else if (strcmp(key, "Fingerprint.TextHash") == 0)
            temp.Fingerprint.TextHash = static_cast<uint64_t>(strtoull(value, nullptr, 0));
        else if (strncmp(key, "RVA.", 4) == 0)
            temp.Rvas[key + 4] = ParseHexU64_104(value);
        else if (strncmp(key, "CALL.", 5) == 0)
            temp.Callsites[key + 5] = ParseHexU64_104(value);
        else if (strncmp(key, "VSLOT.", 6) == 0)
            temp.VSlots[key + 6] = ParseHexU64_104(value);
        else if (strncmp(key, "VDISPATCH.", 10) == 0)
            temp.VDispatches[key + 10] = ParseHexU64_104(value);
    }
    fclose(file);

    *out = std::move(temp);
    return true;
}

static bool LoadBuildProfileCache_104(const FBuildFingerprint_104& current)
{
    g_BuildProfileCache_104 = {};
    SelectCurrentBuildProfilePath_104(current);

#if ESE_DEV_FORCE_UNKNOWN_BOOTSTRAP
    g_ProfileCacheStatus_104 = "dev-forced-bootstrap-no-profile";
    return false;
#endif

    FBuildProfileCache_104 keyed{};
    if (ParseBuildProfileFile_104(
        ArtifactPathOrFallback_104(
            g_ProfilePath_104,
            "ElementalSystemExpanded_build_profile_current.txt"),
        &keyed)) {
        keyed.FingerprintMatched = FingerprintsEqual_104(keyed.Fingerprint, current);
        if (keyed.FingerprintMatched) {
            g_BuildProfileCache_104 = std::move(keyed);
            g_ProfileCacheStatus_104 = "matched-keyed-profile";
            return true;
        }

        // A fingerprint-keyed filename containing another fingerprint is
        // internally inconsistent/corrupt. Do not trust it, but preserve it
        // for diagnosis and still allow a matching legacy profile to rescue
        // this exact build.
        g_ProfileCacheStatus_104 = "keyed-profile-fingerprint-mismatch";
    }
    else {
        g_ProfileCacheStatus_104 = "keyed-profile-missing";
    }

    FBuildProfileCache_104 legacy{};
    if (ParseBuildProfileFile_104(
        ArtifactPathOrFallback_104(
            g_LegacyProfilePath_104,
            "ElementalSystemExpanded_build_profile.txt"),
        &legacy)) {
        legacy.FingerprintMatched = FingerprintsEqual_104(legacy.Fingerprint, current);
        if (legacy.FingerprintMatched) {
            g_BuildProfileCache_104 = std::move(legacy);
            g_ProfileCacheStatus_104 = "matched-legacy-profile-import-pending";
            return true;
        }

        if (g_ProfileCacheStatus_104 == "keyed-profile-missing")
            g_ProfileCacheStatus_104 = "keyed-missing-legacy-other-build-preserved";
    }

    return false;
}

static uintptr_t CachedProfileValue_104(
    const std::unordered_map<std::string, uintptr_t>& map,
    const char* name)
{
    if (!g_BuildProfileCache_104.FingerprintMatched || !name)
        return 0;
    const auto it = map.find(name);
    return it == map.end() ? 0 : it->second;
}

static void CacheResolvedRva_104(const char* name, uintptr_t address)
{
    if (!name || !address || !g_ModuleBase_104 || address < g_ModuleBase_104)
        return;
    g_ProfileOutputRvas_104[name] = address - g_ModuleBase_104;
}

static void CacheResolvedCallsite_104(const char* name, uintptr_t callsite)
{
    if (!name || !callsite || !g_ModuleBase_104 || callsite < g_ModuleBase_104)
        return;
    g_ProfileOutputCallsites_104[name] = callsite - g_ModuleBase_104;
}

static void CacheResolvedVSlot_104(
    const char* name,
    uintptr_t slot,
    uintptr_t dispatchAddress)
{
    if (!name || !slot)
        return;
    g_ProfileOutputVSlots_104[name] = slot;
    if (dispatchAddress && g_ModuleBase_104 && dispatchAddress >= g_ModuleBase_104)
        g_ProfileOutputVDispatches_104[name] = dispatchAddress - g_ModuleBase_104;
}

template <typename TMap>
static void AppendProfileMapSorted_104(
    std::string& output,
    const char* prefix,
    const TMap& map)
{
    std::vector<std::pair<std::string, uintptr_t>> items;
    items.reserve(map.size());
    for (const auto& kv : map)
        items.emplace_back(kv.first, kv.second);
    std::sort(
        items.begin(),
        items.end(),
        [](const auto& a, const auto& b) { return a.first < b.first; });

    char line[768]{};
    for (const auto& kv : items) {
        snprintf(
            line,
            sizeof(line),
            "%s%s=0x%llX\n",
            prefix,
            kv.first.c_str(),
            static_cast<unsigned long long>(kv.second));
        output += line;
    }
}

static bool ProfileOutputsMatchLoadedCache_104()
{
    return g_BuildProfileCache_104.FingerprintMatched &&
        g_BuildProfileCache_104.Rvas == g_ProfileOutputRvas_104 &&
        g_BuildProfileCache_104.Callsites == g_ProfileOutputCallsites_104 &&
        g_BuildProfileCache_104.VSlots == g_ProfileOutputVSlots_104 &&
        g_BuildProfileCache_104.VDispatches == g_ProfileOutputVDispatches_104;
}

static void WriteBuildProfileCache_104(const FBuildFingerprint_104& fp)
{
    if (g_ProfilePath_104.empty())
        SelectCurrentBuildProfilePath_104(fp);

#if ESE_DEV_FORCE_UNKNOWN_BOOTSTRAP
    g_ProfileWriteStatus_104 = "dev-forced-bootstrap-no-write";
    return;
#endif

    // A matching fingerprint-keyed profile that revalidated to the same
    // resolved map does not need to be rewritten every boot.
    if (g_ProfileCacheStatus_104 == "matched-keyed-profile" &&
        ProfileOutputsMatchLoadedCache_104()) {
        g_ProfileWriteStatus_104 = "unchanged-keyed-profile-not-rewritten";
        return;
    }

    std::string output;
    output.reserve(8192);
    char header[768]{};
    snprintf(
        header,
        sizeof(header),
        "# ElementalSystemExpanded auto-generated resolved build profile\n"
        "ProfileVersion=2\n"
        "InfrastructureStage=6.6.3\n"
        "Fingerprint.TimeDateStamp=0x%08lX\n"
        "Fingerprint.SizeOfImage=0x%08lX\n"
        "Fingerprint.TextHash=0x%016llX\n",
        static_cast<unsigned long>(fp.TimeDateStamp),
        static_cast<unsigned long>(fp.SizeOfImage),
        static_cast<unsigned long long>(fp.TextHash));
    output += header;

    AppendProfileMapSorted_104(output, "RVA.", g_ProfileOutputRvas_104);
    AppendProfileMapSorted_104(output, "CALL.", g_ProfileOutputCallsites_104);
    AppendProfileMapSorted_104(output, "VSLOT.", g_ProfileOutputVSlots_104);
    AppendProfileMapSorted_104(output, "VDISPATCH.", g_ProfileOutputVDispatches_104);

    if (!WriteWholeFileDurable_104(
        ArtifactPathOrFallback_104(
            g_ProfilePath_104,
            "ElementalSystemExpanded_build_profile_current.txt"),
        output)) {
        g_ProfileWriteStatus_104 = "keyed-profile-write-failed";
        return;
    }

    if (g_ProfileCacheStatus_104 == "matched-legacy-profile-import-pending")
        g_ProfileWriteStatus_104 = "legacy-imported-to-keyed-profile";
    else
        g_ProfileWriteStatus_104 = "written-keyed-current-build";
}

static void RecordResolver_104(
    const char* name,
    const char* method,
    EResolveConfidence_104 confidence,
    uintptr_t address,
    const char* detail)
{
    FResolverRecord_104 rec{};
    rec.Name = name ? name : "";
    rec.Method = method ? method : "";
    rec.Confidence = confidence;
    rec.Address = address;
    rec.Rva = (address && g_ModuleBase_104 && address >= g_ModuleBase_104)
        ? (address - g_ModuleBase_104)
        : 0;
    rec.Detail = detail ? detail : "";

    AcquireSRWLockExclusive(&g_ResolverRecordsLock_104);
    g_ResolverRecords_104.push_back(rec);
    ReleaseSRWLockExclusive(&g_ResolverRecordsLock_104);

    ModLog(
        "[ElementalSystemExpanded] RESOLVE %-44s %-17s RVA=+0x%llX Method=%s %s\n",
        rec.Name.c_str(),
        ResolveConfidenceName_104(rec.Confidence),
        static_cast<unsigned long long>(rec.Rva),
        rec.Method.c_str(),
        rec.Detail.c_str());
}

static uint64_t Fnv1a64_104(const uint8_t* data, size_t size)
{
    uint64_t hash = 1469598103934665603ull;
    for (size_t i = 0; i < size; ++i) {
        hash ^= static_cast<uint64_t>(data[i]);
        hash *= 1099511628211ull;
    }
    return hash;
}

static bool QueryBuildFingerprint_104(uintptr_t base, FBuildFingerprint_104* out)
{
    if (!base || !out)
        return false;

    __try {
        auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE)
            return false;

        auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE)
            return false;

        out->TimeDateStamp = nt->FileHeader.TimeDateStamp;
        out->SizeOfImage = nt->OptionalHeader.SizeOfImage;

        IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
        for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
            char name[9]{};
            memcpy(name, section[i].Name, 8);
            if (strcmp(name, ".text") != 0)
                continue;

            out->TextRva = section[i].VirtualAddress;
            out->TextSize = section[i].Misc.VirtualSize;
            if (!out->TextSize)
                out->TextSize = section[i].SizeOfRawData;

            const uint8_t* text = reinterpret_cast<const uint8_t*>(base + out->TextRva);
            out->TextHash = Fnv1a64_104(text, out->TextSize);
            return true;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }

    return false;
}

static bool GeneratedLayoutFingerprintMatches_104(
    const FBuildFingerprint_104& fp,
    std::string* detail)
{
#if ESE_HAS_GENERATED_BUILD_FINGERPRINT
    if (!EseGeneratedPalLayout::Has_BuildFingerprint) {
        if (detail)
            *detail = "generated layout header is explicitly unbound to an executable";
        return false;
    }

    const bool match =
        EseGeneratedPalLayout::Build_TimeDateStamp == fp.TimeDateStamp &&
        EseGeneratedPalLayout::Build_SizeOfImage == fp.SizeOfImage &&
        EseGeneratedPalLayout::Build_TextHash == fp.TextHash;
    if (detail) {
        char buffer[384]{};
        snprintf(
            buffer,
            sizeof(buffer),
            "header=%08lX/%08lX/%016llX runtime=%08lX/%08lX/%016llX",
            static_cast<unsigned long>(EseGeneratedPalLayout::Build_TimeDateStamp),
            static_cast<unsigned long>(EseGeneratedPalLayout::Build_SizeOfImage),
            static_cast<unsigned long long>(EseGeneratedPalLayout::Build_TextHash),
            static_cast<unsigned long>(fp.TimeDateStamp),
            static_cast<unsigned long>(fp.SizeOfImage),
            static_cast<unsigned long long>(fp.TextHash));
        *detail = buffer;
    }
    return match;
#else
    if (detail)
        *detail = "legacy generated header has no executable fingerprint";
    return false;
#endif
}

static bool GeneratedLayoutFingerprintAvailable_104()
{
#if ESE_HAS_GENERATED_BUILD_FINGERPRINT
    return EseGeneratedPalLayout::Has_BuildFingerprint;
#else
    return false;
#endif
}

static void AppendMigrationValue_104(
    std::string& output,
    const char* kind,
    const char* name,
    uintptr_t oldValue,
    const std::unordered_map<std::string, uintptr_t>& currentMap)
{
    const auto it = currentMap.find(name ? name : "");
    char line[768]{};
    if (it == currentMap.end()) {
        snprintf(
            line,
            sizeof(line),
            "%-10s %-52s old=0x%llX new=<missing>\n",
            kind ? kind : "?",
            name ? name : "?",
            static_cast<unsigned long long>(oldValue));
    }
    else {
        const intptr_t delta =
            static_cast<intptr_t>(it->second) - static_cast<intptr_t>(oldValue);
        snprintf(
            line,
            sizeof(line),
            "%-10s %-52s old=0x%llX new=0x%llX delta=%+lld\n",
            kind ? kind : "?",
            name ? name : "?",
            static_cast<unsigned long long>(oldValue),
            static_cast<unsigned long long>(it->second),
            static_cast<long long>(delta));
    }
    output += line;
}

static void WriteMigrationReport_104(const FBuildFingerprint_104& fp)
{
    if (!g_DebugDiagnosticsEnabled_104 && !g_ForceFailureArtifacts_104)
        return;

    std::string output;
    output.reserve(12288);
    char header[1024]{};
    snprintf(
        header,
        sizeof(header),
        "ElementalSystemExpanded build migration report\n"
        "Baseline=Palworld 1.0.4 / Stage 4.2 core + Stage 6.6.3 status exchange + Darkness/Light 3s cap\n"
        "InfrastructureStage=6.6.3\n"
        "Current.TimeDateStamp=0x%08lX\n"
        "Current.SizeOfImage=0x%08lX\n"
        "Current.TextHash=0x%016llX\n"
        "Current.IsKnown104=%s\n"
        "Current.ActualKnown104Fingerprint=%s\n"
        "DevForceUnknownBootstrap=%s\n"
        "RvaShiftHint=%+lld\n"
        "GeneratedLayoutFingerprintBound=%s\n",
        static_cast<unsigned long>(fp.TimeDateStamp),
        static_cast<unsigned long>(fp.SizeOfImage),
        static_cast<unsigned long long>(fp.TextHash),
        IsKnown104Fingerprint_104(fp) ? "YES" : "NO",
        IsActualKnown104Fingerprint_104(fp) ? "YES" : "NO",
        ESE_DEV_FORCE_UNKNOWN_BOOTSTRAP ? "YES" : "NO",
        static_cast<long long>(g_RvaShiftHint_104),
        GeneratedLayoutFingerprintAvailable_104() ? "YES" : "NO");
    output += header;

#if ESE_HAS_GENERATED_BUILD_FINGERPRINT
    if (EseGeneratedPalLayout::Has_BuildFingerprint) {
        char bound[512]{};
        snprintf(
            bound,
            sizeof(bound),
            "GeneratedLayout.TimeDateStamp=0x%08lX\n"
            "GeneratedLayout.SizeOfImage=0x%08lX\n"
            "GeneratedLayout.TextHash=0x%016llX\n",
            static_cast<unsigned long>(EseGeneratedPalLayout::Build_TimeDateStamp),
            static_cast<unsigned long>(EseGeneratedPalLayout::Build_SizeOfImage),
            static_cast<unsigned long long>(EseGeneratedPalLayout::Build_TextHash));
        output += bound;
    }
#endif

    output += "\n[Resolved native RVAs vs 1.0.4]\n";
    AppendMigrationValue_104(output, "RVA", "RawFPalDamageInfoCaller", Known104::Rva::RawEffectCaller, g_ProfileOutputRvas_104);
    AppendMigrationValue_104(output, "RVA", "DamageReaction::AddElementStatusAdditionalValue_OneType.wrapper", Known104::Rva::ElementBuildup, g_ProfileOutputRvas_104);
    AppendMigrationValue_104(output, "RVA", "PalStatusComponent::AddStatus.from-buildup", Known104::Rva::AddStatus, g_ProfileOutputRvas_104);
    AppendMigrationValue_104(output, "RVA", "CalcElementMatchupTier", Known104::Rva::MatchupHelper, g_ProfileOutputRvas_104);
    AppendMigrationValue_104(output, "RVA", "PalVisualEffectComponent::AddVisualEffect", Known104::Rva::AddVisualEffect_Native, g_ProfileOutputRvas_104);
    AppendMigrationValue_104(output, "RVA", "PalVisualEffectComponent::AddVisualEffect_Local", Known104::Rva::AddVisualEffectLocal_Native, g_ProfileOutputRvas_104);
    AppendMigrationValue_104(output, "RVA", "PalVisualEffectComponent::RemoveVisualEffect_Local", Known104::Rva::RemoveVisualEffectLocal_Native, g_ProfileOutputRvas_104);
    AppendMigrationValue_104(output, "RVA", "PalAIActionComponent::SetActionClassParameter", Known104::Rva::SetActionClassParameter_Native, g_ProfileOutputRvas_104);
    AppendMigrationValue_104(output, "RVA", "PalActionComponent::PlayAction", Known104::Rva::PlayAction_Native, g_ProfileOutputRvas_104);
    AppendMigrationValue_104(output, "RVA", "PalCharacterMovementComponent::SetJumpDisableFlag", Known104::Rva::SetJumpDisableFlag_Native, g_ProfileOutputRvas_104);
    AppendMigrationValue_104(output, "RVA", "PalCharacterMovementComponent::SetStepDisableFlag", Known104::Rva::SetStepDisableFlag_Native, g_ProfileOutputRvas_104);
    AppendMigrationValue_104(output, "RVA", "PalCharacterMovementComponent::SetMoveDisableFlag", Known104::Rva::SetMoveDisableFlag_Native, g_ProfileOutputRvas_104);
    AppendMigrationValue_104(output, "RVA", "PalCharacterMovementComponent::SetWalkSpeedMultiplier", Known104::Rva::SetWalkSpeedMultiplier_Native, g_ProfileOutputRvas_104);
    AppendMigrationValue_104(output, "RVA", "PalCharacterMovementComponent::SetYawRotatorMultiplier", Known104::Rva::SetYawRotatorMultiplier_Native, g_ProfileOutputRvas_104);
    AppendMigrationValue_104(output, "RVA", "PalShooterComponent::SetDisableAimFlag_Layered", Known104::Rva::SetDisableAimFlag_Native, g_ProfileOutputRvas_104);
    AppendMigrationValue_104(output, "RVA", "PalShooterComponent::SetDisableShootFlag_Layered", Known104::Rva::SetDisableShootFlag_Native, g_ProfileOutputRvas_104);
    AppendMigrationValue_104(output, "RVA", "PalShooterComponent::SetDisableChangeWeaponFlag_Layered", Known104::Rva::SetDisableChangeWeaponFlag_Native, g_ProfileOutputRvas_104);

    output += "\n[Wrapper / semantic callsites vs 1.0.4]\n";
    AppendMigrationValue_104(output, "CALL", "CalcElementMatchupTier.SemanticCallsite", Known104::Rva::MatchupCallsite, g_ProfileOutputCallsites_104);
    AppendMigrationValue_104(output, "CALL", "DamageReaction::AddElementStatusAdditionalValue_OneType.wrapper", Known104::Rva::ElementBuildup_Call, g_ProfileOutputCallsites_104);
    AppendMigrationValue_104(output, "CALL", "PalStatusComponent::AddStatus.from-buildup", Known104::Rva::AddStatus_FromBuildup_Call, g_ProfileOutputCallsites_104);
    AppendMigrationValue_104(output, "CALL", "PalVisualEffectComponent::AddVisualEffect", Known104::Rva::AddVisualEffect_Call, g_ProfileOutputCallsites_104);
    AppendMigrationValue_104(output, "CALL", "PalVisualEffectComponent::AddVisualEffect_Local", Known104::Rva::AddVisualEffectLocal_Call, g_ProfileOutputCallsites_104);
    AppendMigrationValue_104(output, "CALL", "PalVisualEffectComponent::RemoveVisualEffect_Local", Known104::Rva::RemoveVisualEffectLocal_Call, g_ProfileOutputCallsites_104);
    AppendMigrationValue_104(output, "CALL", "PalAIActionComponent::SetActionClassParameter", Known104::Rva::SetActionClassParameter_Call, g_ProfileOutputCallsites_104);
    AppendMigrationValue_104(output, "CALL", "PalActionComponent::PlayAction", Known104::Rva::PlayAction_Call, g_ProfileOutputCallsites_104);
    AppendMigrationValue_104(output, "CALL", "PalCharacterMovementComponent::SetJumpDisableFlag", Known104::Rva::SetJumpDisableFlag_Call, g_ProfileOutputCallsites_104);
    AppendMigrationValue_104(output, "CALL", "PalCharacterMovementComponent::SetStepDisableFlag", Known104::Rva::SetStepDisableFlag_Call, g_ProfileOutputCallsites_104);
    AppendMigrationValue_104(output, "CALL", "PalCharacterMovementComponent::SetMoveDisableFlag", Known104::Rva::SetMoveDisableFlag_Call, g_ProfileOutputCallsites_104);
    AppendMigrationValue_104(output, "CALL", "PalCharacterMovementComponent::SetWalkSpeedMultiplier", Known104::Rva::SetWalkSpeedMultiplier_Call, g_ProfileOutputCallsites_104);
    AppendMigrationValue_104(output, "CALL", "PalCharacterMovementComponent::SetYawRotatorMultiplier", Known104::Rva::SetYawRotatorMultiplier_Call, g_ProfileOutputCallsites_104);
    AppendMigrationValue_104(output, "CALL", "PalShooterComponent::SetDisableAimFlag_Layered", Known104::Rva::SetDisableAimFlag_Call, g_ProfileOutputCallsites_104);
    AppendMigrationValue_104(output, "CALL", "PalShooterComponent::SetDisableShootFlag_Layered", Known104::Rva::SetDisableShootFlag_Call, g_ProfileOutputCallsites_104);
    AppendMigrationValue_104(output, "CALL", "PalShooterComponent::SetDisableChangeWeaponFlag_Layered", Known104::Rva::SetDisableChangeWeaponFlag_Call, g_ProfileOutputCallsites_104);

    output += "\n[VSlots vs 1.0.4]\n";
    AppendMigrationValue_104(output, "VSLOT", "VSlot.ActorComponent::SetComponentTickEnabled", Known104::VSlot::SetComponentTickEnabled, g_ProfileOutputVSlots_104);
    AppendMigrationValue_104(output, "VSLOT", "VSlot.Character::StopAnimMontage", Known104::VSlot::StopAnimMontage, g_ProfileOutputVSlots_104);
    AppendMigrationValue_104(output, "VSLOT", "VSlot.PalStatusBase::TickStatus", Known104::VSlot::StatusTick, g_ProfileOutputVSlots_104);

    if (!WriteWholeFileDurable_104(
        ArtifactPathOrFallback_104(
            g_MigrationReportPath_104,
            "ElementalSystemExpanded_migration_report.txt"),
        output)) {
        ShipLog_104("[ElementalSystemExpanded] WARNING: migration report write failed.\n");
    }
}

static void WriteResolverReport_104(const FBuildFingerprint_104& fp)
{
    if (!g_DebugDiagnosticsEnabled_104 && !g_ForceFailureArtifacts_104)
        return;

    FILE* file = nullptr;
#if defined(_MSC_VER)
    fopen_s(&file, ArtifactPathOrFallback_104(g_ResolverReportPath_104, "ElementalSystemExpanded_resolver_report.txt"), "w");
#else
    file = fopen(ArtifactPathOrFallback_104(g_ResolverReportPath_104, "ElementalSystemExpanded_resolver_report.txt"), "w");
#endif
    if (!file)
        return;

    fprintf(file, "ElementalSystemExpanded resolver report\n");
    fprintf(file, "Baseline semantics: Stage 4.2 core + Stage 6.6.3 status exchange + Darkness/Light 3s cap\n");
    fprintf(file, "ModuleBase=0x%llX\n", static_cast<unsigned long long>(g_ModuleBase_104));
    fprintf(file, "PE.TimeDateStamp=0x%08lX\n", static_cast<unsigned long>(fp.TimeDateStamp));
    fprintf(file, "PE.SizeOfImage=0x%08lX\n", static_cast<unsigned long>(fp.SizeOfImage));
    fprintf(file, ".text.RVA=0x%08lX\n", static_cast<unsigned long>(fp.TextRva));
    fprintf(file, ".text.Size=0x%08lX\n", static_cast<unsigned long>(fp.TextSize));
    fprintf(file, ".text.FNV1a64=0x%016llX\n", static_cast<unsigned long long>(fp.TextHash));
    fprintf(file, "RvaShiftHint=%+lld\n", static_cast<long long>(g_RvaShiftHint_104));
    fprintf(file, "Known104Fingerprint=%s\n", IsKnown104Fingerprint_104(fp) ? "YES" : "NO");
    fprintf(file, "ActualKnown104Fingerprint=%s\n", IsActualKnown104Fingerprint_104(fp) ? "YES" : "NO");
    fprintf(file, "DevForceUnknownBootstrap=%s\n", ESE_DEV_FORCE_UNKNOWN_BOOTSTRAP ? "YES" : "NO");
    fprintf(file, "BuildProfileCache=%s\n", g_ProfileCacheStatus_104.c_str());
    fprintf(file, "BuildProfileWrite=%s\n", g_ProfileWriteStatus_104.c_str());
    fprintf(file, "ArtifactPathMode=%s\n", g_ArtifactPathStatus_104.c_str());
    fprintf(file, "ArtifactDirectory=%s\n", g_ArtifactDirectory_104.c_str());
    fprintf(file, "BuildProfilePath=%s\n", ArtifactPathOrFallback_104(g_ProfilePath_104, "ElementalSystemExpanded_build_profile_current.txt"));
    fprintf(file, "LegacyBuildProfilePath=%s\n", ArtifactPathOrFallback_104(g_LegacyProfilePath_104, "ElementalSystemExpanded_build_profile.txt"));
    fprintf(file, "MigrationReportPath=%s\n", ArtifactPathOrFallback_104(g_MigrationReportPath_104, "ElementalSystemExpanded_migration_report.txt"));
    fprintf(file, "UpdateDiagnosticsPath=%s\n", ArtifactPathOrFallback_104(g_UpdateDiagnosticsPath_104, "ElementalSystemExpanded_update_diagnostics.txt"));
    fprintf(file, "RuntimeSnapshotPath=%s\n", ArtifactPathOrFallback_104(g_RuntimeSnapshotPath_104, "ElementalSystemExpanded_runtime_snapshot.txt"));
    std::string layoutBindingDetail;
    const bool layoutBindingMatch = GeneratedLayoutFingerprintMatches_104(fp, &layoutBindingDetail);
    fprintf(file, "GeneratedLayoutFingerprintBound=%s\n", GeneratedLayoutFingerprintAvailable_104() ? "YES" : "NO");
    fprintf(file, "GeneratedLayoutFingerprintMatch=%s\n", layoutBindingMatch ? "YES" : "NO");
    fprintf(file, "GeneratedLayoutFingerprintDetail=%s\n", layoutBindingDetail.c_str());
    fprintf(file, "GeneratedLightImmunityTargetLayout=%s\n",
        ESE_HAS_GENERATED_LIGHT_TARGET_LAYOUT ? "YES" : "NO");
    fprintf(file, "UpdateDiagnosticsInit=%s\n\n", g_DiagnosticsInitStatus_104.c_str());

    size_t exactCount = 0;
    size_t strongCount = 0;
    size_t runtimeCount = 0;
    size_t profileCount = 0;
    size_t deferredCount = 0;
    size_t failedCount = 0;

    AcquireSRWLockShared(&g_ResolverRecordsLock_104);
    for (const auto& rec : g_ResolverRecords_104) {
        switch (rec.Confidence) {
        case EResolveConfidence_104::Exact: ++exactCount; break;
        case EResolveConfidence_104::Strong: ++strongCount; break;
        case EResolveConfidence_104::RuntimeValidated: ++runtimeCount; break;
        case EResolveConfidence_104::ProfileStatic: ++profileCount; break;
        case EResolveConfidence_104::Deferred: ++deferredCount; break;
        default: ++failedCount; break;
        }

        fprintf(file,
            "%-46s | %-17s | RVA +0x%llX | %-24s | %s\n",
            rec.Name.c_str(),
            ResolveConfidenceName_104(rec.Confidence),
            static_cast<unsigned long long>(rec.Rva),
            rec.Method.c_str(),
            rec.Detail.c_str());
    }

    fprintf(file,
        "\nResolverSummary: EXACT=%llu STRONG=%llu RUNTIME_VALIDATED=%llu "
        "PROFILE_STATIC=%llu DEFERRED=%llu FAILED=%llu\n",
        static_cast<unsigned long long>(exactCount),
        static_cast<unsigned long long>(strongCount),
        static_cast<unsigned long long>(runtimeCount),
        static_cast<unsigned long long>(profileCount),
        static_cast<unsigned long long>(deferredCount),
        static_cast<unsigned long long>(failedCount));

    fprintf(file, "\n[Capabilities]\n");
    for (const auto& rec : g_ResolverRecords_104) {
        if (rec.Name.rfind("CAPABILITY.", 0) != 0)
            continue;
        fprintf(file, "%-46s | %-17s | %s\n",
            rec.Name.c_str(),
            ResolveConfidenceName_104(rec.Confidence),
            rec.Detail.c_str());
    }
    ReleaseSRWLockShared(&g_ResolverRecordsLock_104);

    fprintf(file, "\n[Centralized layout profile]\n");
    fprintf(file, "Pal SDK layout source=%s\n", ESE_HAS_GENERATED_PAL_LAYOUT ? "generated-header" : "embedded-1.0.4-fallback");
    fprintf(file, "UObject.OuterPrivate=<runtime-discovered; static value not authoritative>\n");
    fprintf(file, "FPalDamageInfo.AttackElement=0x%llX\n", (unsigned long long)ActiveLayout::DamageInfo_AttackElement);
    fprintf(file, "FPalDamageInfo.EffectType1=0x%llX EffectValue1=0x%llX EffectType2=0x%llX EffectValue2=0x%llX\n",
        (unsigned long long)ActiveLayout::DamageInfo_EffectType1,
        (unsigned long long)ActiveLayout::DamageInfo_EffectValue1,
        (unsigned long long)ActiveLayout::DamageInfo_EffectType2,
        (unsigned long long)ActiveLayout::DamageInfo_EffectValue2);
    fprintf(file, "APalCharacter.Root=0x%llX DamageReaction=0x%llX Status=0x%llX VisualEffect=0x%llX\n",
        (unsigned long long)ActiveLayout::Character_RootComponent,
        (unsigned long long)ActiveLayout::Character_DamageReactionComponent,
        (unsigned long long)ActiveLayout::Character_StatusComponent,
        (unsigned long long)ActiveLayout::Character_VisualEffectComponent);
    fprintf(file, "LightImmunity.TargetLayout: CharacterParameter=0x%llX StaticParameter=0x%llX Element1=0x%llX Element2=0x%llX IsPal=0x%llX Source=%s\n",
        (unsigned long long)ActiveLayout::Character_CharacterParameterComponent,
        (unsigned long long)ActiveLayout::Character_StaticCharacterParameterComponent,
        (unsigned long long)ActiveLayout::CharacterParameter_ElementType1,
        (unsigned long long)ActiveLayout::CharacterParameter_ElementType2,
        (unsigned long long)ActiveLayout::StaticCharacterParameter_IsPal,
        ESE_HAS_GENERATED_LIGHT_TARGET_LAYOUT ? "generated-v8" : "known-1.0.4-fallback");
    fprintf(file, "UPalStatusComponent.ExecutionStatusList=0x%llX\n",
        (unsigned long long)ActiveLayout::StatusComponent_ExecutionStatusList);
    fprintf(file, "UPalStatusBase.End=0x%llX ID=0x%llX Duration=0x%llX Timer=0x%llX NativeSize=0x%llX\n",
        (unsigned long long)ActiveLayout::StatusBase_IsEndStatus,
        (unsigned long long)ActiveLayout::StatusBase_StatusID,
        (unsigned long long)ActiveLayout::StatusBase_Duration,
        (unsigned long long)ActiveLayout::StatusBase_DurationTimer,
        (unsigned long long)ActiveLayout::StatusBase_NativeSize);
    fprintf(file, "RootComponent.WorldLocation=<hidden backing field not trusted on unknown builds>\n");
    fprintf(file, "UPalVisualEffectComponent.ExecutionVisualEffects=0x%llX\n",
        (unsigned long long)ActiveLayout::VisualEffectComponent_ExecutionVisualEffects);
    fprintf(file, "UPalVisualEffectBase.End=0x%llX ID=0x%llX\n",
        (unsigned long long)ActiveLayout::VisualEffectBase_IsEnd,
        (unsigned long long)ActiveLayout::VisualEffectBase_ID);
    fprintf(file, "IDs: Effects Burn/Wet/Freeze/Elec/Muddy/Ivy/Dark=%u/%u/%u/%u/%u/%u/%u\n",
        ActiveIds::Effect_Burn, ActiveIds::Effect_Wetness, ActiveIds::Effect_Freeze,
        ActiveIds::Effect_Electrical, ActiveIds::Effect_Muddy, ActiveIds::Effect_IvyCling,
        ActiveIds::Effect_Darkness);
    fprintf(file, "IDs: Status Burn/Wet/Freeze/Elec/Muddy/Ivy/Dark=%u/%u/%u/%u/%u/%u/%u WetFreeze=%u/%u IceCondition=%u\n",
        ActiveIds::Status_Burn, ActiveIds::Status_Wetness, ActiveIds::Status_Freeze,
        ActiveIds::Status_Electrical, ActiveIds::Status_Muddy, ActiveIds::Status_IvyCling,
        ActiveIds::Status_Darkness, ActiveIds::Status_VanillaWetFreeze,
        ActiveIds::Status_VanillaWetFreezeResist, ActiveIds::VisualEffect_IceCondition);
    fprintf(file, "\n[Runtime / engine-layout durability]\n");
    fprintf(file, "UObject.OuterPrivate=runtime semantic discovery; known/generated values are hints only. CurrentDiscovered=0x%llX\n",
        (unsigned long long)g_RuntimeUObjectOuterOffset_104);
    fprintf(file, "AActor.RootComponent=0x%llX (%s)\n",
        (unsigned long long)ActiveLayout::Character_RootComponent,
#if ESE_HAS_GENERATED_ENGINE_OPTIONALS
        EseGeneratedPalLayout::Has_Actor_RootComponent ? "generated-from-SDK" : "known-build fallback"
#else
        "known-build fallback"
#endif
    );
#if ESE_HAS_GENERATED_SCENE_RELATIVE_LOCATION
    fprintf(file, "Root position reference=USceneComponent::RelativeLocation 0x%llX (%s); BP StartLocation proximity validation required before write.\n",
        (unsigned long long)ActiveLayout::SceneComponent_RelativeLocation,
        EseGeneratedPalLayout::Has_SceneComponent_RelativeLocation ? "generated-from-SDK" : "known-build fallback");
    fprintf(file, "USceneComponent.AttachParent=%s",
        ActiveLayout::HasGeneratedSceneAttachParent ? "" : "<not generated>");
    if (ActiveLayout::HasGeneratedSceneAttachParent)
        fprintf(file, "0x%llX", (unsigned long long)ActiveLayout::SceneComponent_AttachParent);
    fprintf(file, "; unknown-build attached roots fail closed rather than treating RelativeLocation as world-space.\n");
#else
    fprintf(file, "Root position reference=known-build hidden world field +0x260 only on exact 1.0.4; unknown builds fail closed until a generated RelativeLocation is available.\n");
#endif
    fprintf(file, "Hidden ComponentToWorld backing offset is NOT trusted on unknown builds.\n");
    fprintf(file, "RawTArray ABI mirror: Data@0x0 Num@0x8 Max@0xC Size=0x10; runtime requires 0<=Num<=Max, bounded capacity, aligned/readable storage, and UObject-like element validation.\n");

    fprintf(file, "ResolvedVSlots: TickEnabled=0x%llX StopMontage=0x%llX StatusTick=0x%llX\n",
        (unsigned long long)g_VSlot_SetComponentTickEnabled_104,
        (unsigned long long)g_VSlot_StopAnimMontage_104,
        (unsigned long long)g_VSlot_StatusTick_104);

    fclose(file);
}

static void RecomputeRvaShiftHint_104()
{
    if (g_AnchorRvaShifts_104.empty()) {
        g_RvaShiftHint_104 = 0;
        return;
    }

    auto values = g_AnchorRvaShifts_104;
    std::sort(values.begin(), values.end());
    g_RvaShiftHint_104 = values[values.size() / 2];
}

// ---------------------------------------------------------
// Element matchup matrix
// ---------------------------------------------------------
enum class EPalElementType : unsigned char {
    None = ActiveIds::Element_None,
    Normal = ActiveIds::Element_Normal,
    Fire = ActiveIds::Element_Fire,
    Water = ActiveIds::Element_Water,
    Leaf = ActiveIds::Element_Leaf,
    Electricity = ActiveIds::Element_Electricity,
    Ice = ActiveIds::Element_Ice,
    Earth = ActiveIds::Element_Earth,
    Dark = ActiveIds::Element_Dark,
    Dragon = ActiveIds::Element_Dragon,
    MAX = 10
};

int GetSingleMatchupTier(EPalElementType attacker, EPalElementType defender) {
    if (attacker == EPalElementType::None || defender == EPalElementType::None ||
        attacker >= EPalElementType::MAX || defender >= EPalElementType::MAX) {
        return 0;
    }

    switch (attacker) {
    case EPalElementType::Normal:
        switch (defender) {
        case EPalElementType::Dark: return 1;
        case EPalElementType::Leaf: return -1;
        default: break;
        }
        break;

    case EPalElementType::Fire:
        switch (defender) {
        case EPalElementType::Dark:
        case EPalElementType::Leaf:
        case EPalElementType::Ice:
            return 1;
        case EPalElementType::Earth:
        case EPalElementType::Electricity:
        case EPalElementType::Water:
            return -1;
        default: break;
        }
        break;

    case EPalElementType::Water:
        switch (defender) {
        case EPalElementType::Fire:
        case EPalElementType::Earth:
            return 1;
        case EPalElementType::Electricity:
        case EPalElementType::Leaf:
        case EPalElementType::Ice:
            return -1;
        default: break;
        }
        break;

    case EPalElementType::Leaf:
        switch (defender) {
        case EPalElementType::Water:
        case EPalElementType::Normal:
        case EPalElementType::Earth:
            return 1;
        case EPalElementType::Fire:
        case EPalElementType::Ice:
        case EPalElementType::Dark:
            return -1;
        default: break;
        }
        break;

    case EPalElementType::Electricity:
        switch (defender) {
        case EPalElementType::Water:
        case EPalElementType::Fire:
            return 1;
        case EPalElementType::Earth:
            return -1;
        default: break;
        }
        break;

    case EPalElementType::Ice:
        switch (defender) {
        case EPalElementType::Water:
        case EPalElementType::Leaf:
            return 1;
        case EPalElementType::Fire:
            return -1;
        default: break;
        }
        break;

    case EPalElementType::Earth:
        switch (defender) {
        case EPalElementType::Electricity:
        case EPalElementType::Fire:
            return 1;
        case EPalElementType::Water:
        case EPalElementType::Leaf:
            return -1;
        default: break;
        }
        break;

    case EPalElementType::Dark:
        switch (defender) {
        case EPalElementType::Leaf:
            return 1;
        case EPalElementType::Fire:
        case EPalElementType::Normal:
            return -1;
        default: break;
        }
        break;

    default:
        break;
    }

    return 0;
}

using CalcElementMatchupTier_104_t =
int(__fastcall*)(
    EPalElementType AttackElementType,
    EPalElementType DefenceTypeA,
    EPalElementType DefenceTypeB);

static CalcElementMatchupTier_104_t
Original_CalcElementMatchupTier_104 = nullptr;

static int __fastcall Detour_CalcElementMatchupTier_104(
    EPalElementType attackElement,
    EPalElementType defenceTypeA,
    EPalElementType defenceTypeB)
{
    const int tierA =
        GetSingleMatchupTier(attackElement, defenceTypeA);
    const int tierB =
        GetSingleMatchupTier(attackElement, defenceTypeB);
    return tierA + tierB;
}

// ---------------------------------------------------------
// Common safety / hook helpers
// ---------------------------------------------------------
static bool IsReadableAddress_104(uintptr_t address, size_t size)
{
    if (!address || !size)
        return false;

    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(reinterpret_cast<const void*>(address), &mbi, sizeof(mbi)) != sizeof(mbi))
        return false;

    if (mbi.State != MEM_COMMIT)
        return false;

    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))
        return false;

    const uintptr_t regionStart = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
    if (mbi.RegionSize > (std::numeric_limits<uintptr_t>::max)() - regionStart)
        return false;
    const uintptr_t regionEnd = regionStart + mbi.RegionSize;
    if (address < regionStart || address >= regionEnd)
        return false;
    return size <= (regionEnd - address);
}


static void InitializeUpdateDiagnostics_104(const FBuildFingerprint_104& fp)
{
    if (!g_DebugDiagnosticsEnabled_104) {
        g_DiagnosticsInitStatus_104 = "disabled-no-debug-marker";
        return;
    }

    FILE* file = nullptr;
#if defined(_MSC_VER)
    fopen_s(&file, ArtifactPathOrFallback_104(g_UpdateDiagnosticsPath_104, "ElementalSystemExpanded_update_diagnostics.txt"), "w");
#else
    file = fopen(ArtifactPathOrFallback_104(g_UpdateDiagnosticsPath_104, "ElementalSystemExpanded_update_diagnostics.txt"), "w");
#endif
    if (!file) {
        char status[96]{};
        snprintf(status, sizeof(status), "open-failed errno=%d", errno);
        g_DiagnosticsInitStatus_104 = status;
        ShipLog_104("[ElementalSystemExpanded] WARNING: update diagnostics open failed: %s path=%s\n",
            g_DiagnosticsInitStatus_104.c_str(),
            ArtifactPathOrFallback_104(g_UpdateDiagnosticsPath_104, "ElementalSystemExpanded_update_diagnostics.txt"));
        return;
    }
    g_DiagnosticsInitStatus_104 = "created";
    fprintf(file, "ElementalSystemExpanded update diagnostics\n");
    fprintf(file, "TimeDateStamp=0x%08lX SizeOfImage=0x%08lX TextHash=0x%016llX\n",
        static_cast<unsigned long>(fp.TimeDateStamp),
        static_cast<unsigned long>(fp.SizeOfImage),
        static_cast<unsigned long long>(fp.TextHash));
    fprintf(file, "Known104=%s\n\n", IsKnown104Fingerprint_104(fp) ? "YES" : "NO");
    fclose(file);
}

static void AppendDiagnosticWindow_104(
    const char* name,
    const char* reason,
    uintptr_t centerRva)
{
    if (!g_DebugDiagnosticsEnabled_104 ||
        !g_ModuleBase_104 ||
        !centerRva) {
        return;
    }

    const uintptr_t radiusBefore = 0x30;
    const size_t dumpSize = 0x90;
    const uintptr_t startRva = centerRva > radiusBefore
        ? centerRva - radiusBefore
        : 0;
    const uintptr_t address = g_ModuleBase_104 + startRva;

    uint8_t bytes[dumpSize]{};
    bool copied = false;
    __try {
        memcpy(bytes, reinterpret_cast<const void*>(address), dumpSize);
        copied = true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        copied = false;
    }

    FILE* file = nullptr;
#if defined(_MSC_VER)
    fopen_s(&file, ArtifactPathOrFallback_104(g_UpdateDiagnosticsPath_104, "ElementalSystemExpanded_update_diagnostics.txt"), "a");
#else
    file = fopen(ArtifactPathOrFallback_104(g_UpdateDiagnosticsPath_104, "ElementalSystemExpanded_update_diagnostics.txt"), "a");
#endif
    if (!file)
        return;

    fprintf(file, "[%s]\nReason=%s\nCenterRVA=+0x%llX RvaShiftHint=%+lld\n",
        name ? name : "unnamed",
        reason ? reason : "unspecified",
        static_cast<unsigned long long>(centerRva),
        static_cast<long long>(g_RvaShiftHint_104));

    if (!copied) {
        fprintf(file, "HexDump=<unreadable>\n\n");
        fclose(file);
        return;
    }

    for (size_t row = 0; row < dumpSize; row += 16) {
        fprintf(file, "+0x%08llX : ",
            static_cast<unsigned long long>(startRva + row));
        for (size_t i = 0; i < 16 && row + i < dumpSize; ++i)
            fprintf(file, "%02X ", static_cast<unsigned>(bytes[row + i]));
        fprintf(file, "\n");
    }
    fprintf(file, "\n");
    fclose(file);
}

static bool IsExecutableAddress_104(uintptr_t address)
{
    MEMORY_BASIC_INFORMATION mbi{};
    if (!address ||
        VirtualQuery(
            reinterpret_cast<const void*>(address),
            &mbi,
            sizeof(mbi)) != sizeof(mbi)) {
        return false;
    }

    if (mbi.State != MEM_COMMIT ||
        (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
        return false;
    }

    const DWORD p = mbi.Protect & 0xFFu;
    return p == PAGE_EXECUTE ||
        p == PAGE_EXECUTE_READ ||
        p == PAGE_EXECUTE_READWRITE ||
        p == PAGE_EXECUTE_WRITECOPY;
}

static uintptr_t ResolveRel32Call_104(uintptr_t callSite)
{
    if (!IsReadableAddress_104(callSite, 5))
        return 0;

    __try {
        if (*reinterpret_cast<const uint8_t*>(callSite) != 0xE8)
            return 0;

        const int32_t rel =
            *reinterpret_cast<const int32_t*>(callSite + 1);
        return callSite + 5 + static_cast<intptr_t>(rel);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

static bool MatchMaskedBytes_104(
    uintptr_t address,
    const uint8_t* pattern,
    const char* mask)
{
    if (!address || !pattern || !mask)
        return false;

    const size_t length = strlen(mask);
    if (!length || !IsReadableAddress_104(address, length))
        return false;

    __try {
        for (size_t i = 0; i < length; ++i) {
            if (mask[i] == 'x' &&
                *reinterpret_cast<const uint8_t*>(address + i) != pattern[i]) {
                return false;
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }

    return true;
}

struct FExecutableSectionRange_104 {
    uintptr_t Rva = 0;
    uintptr_t Size = 0;
};

static bool SnapshotExecutableSections_104(
    uintptr_t moduleBase,
    FExecutableSectionRange_104* outSections,
    size_t capacity,
    size_t* outCount,
    uintptr_t* outImageSize)
{
    if (outCount)
        *outCount = 0;
    if (outImageSize)
        *outImageSize = 0;
    if (!moduleBase || !outSections || !capacity || !outCount)
        return false;

    __try {
        auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(moduleBase);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0)
            return false;

        auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(moduleBase + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE)
            return false;

        const uintptr_t imageSize = nt->OptionalHeader.SizeOfImage;
        if (!imageSize)
            return false;
        if (outImageSize)
            *outImageSize = imageSize;

        IMAGE_SECTION_HEADER* sections = IMAGE_FIRST_SECTION(nt);
        size_t count = 0;
        for (WORD i = 0;
            i < nt->FileHeader.NumberOfSections && count < capacity;
            ++i) {
            if (!(sections[i].Characteristics & IMAGE_SCN_MEM_EXECUTE))
                continue;

            uintptr_t rva = sections[i].VirtualAddress;
            uintptr_t size = sections[i].Misc.VirtualSize;
            if (!size)
                size = sections[i].SizeOfRawData;

            if (!size || rva >= imageSize || size > imageSize - rva)
                continue;

            outSections[count++] = { rva, size };
        }

        *outCount = count;
        return count != 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        *outCount = 0;
        return false;
    }
}

static std::vector<uintptr_t> FindPatternInExecutableSections_104(
    uintptr_t moduleBase,
    const uint8_t* pattern,
    const char* mask,
    size_t maxMatches)
{
    std::vector<uintptr_t> matches;
    if (!moduleBase || !pattern || !mask || !mask[0] || maxMatches == 0)
        return matches;

    const size_t patternLen = strlen(mask);
    FExecutableSectionRange_104 sections[96]{};
    size_t sectionCount = 0;
    uintptr_t imageSize = 0;
    if (!SnapshotExecutableSections_104(
        moduleBase, sections, 96, &sectionCount, &imageSize)) {
        return matches;
    }

    for (size_t s = 0; s < sectionCount; ++s) {
        const uintptr_t begin = moduleBase + sections[s].Rva;
        const uintptr_t size = sections[s].Size;
        if (size < patternLen)
            continue;

        // One region-level sanity probe; scanning itself contains no SEH/STL
        // interaction, keeping this function MSVC C2712-safe.
        if (!IsReadableAddress_104(begin, patternLen))
            continue;

        const uintptr_t last = begin + size - patternLen;
        for (uintptr_t at = begin; at <= last; ++at) {
            bool ok = true;
            for (size_t i = 0; i < patternLen; ++i) {
                if (mask[i] == 'x' &&
                    *reinterpret_cast<const uint8_t*>(at + i) != pattern[i]) {
                    ok = false;
                    break;
                }
            }

            if (!ok)
                continue;

            matches.push_back(at);
            if (matches.size() >= maxMatches)
                return matches;
        }
    }

    return matches;
}

static std::vector<uintptr_t> FindPatternNearRva_104(
    uintptr_t moduleBase,
    uintptr_t expectedRva,
    uintptr_t radius,
    const uint8_t* pattern,
    const char* mask,
    size_t maxMatches)
{
    std::vector<uintptr_t> matches;
    if (!moduleBase || !pattern || !mask || !mask[0] || maxMatches == 0)
        return matches;

    const size_t patternLen = strlen(mask);
    FExecutableSectionRange_104 sections[96]{};
    size_t sectionCount = 0;
    uintptr_t imageSize = 0;
    if (!SnapshotExecutableSections_104(
        moduleBase, sections, 96, &sectionCount, &imageSize)) {
        return matches;
    }

    uintptr_t wantedLow = expectedRva > radius ? expectedRva - radius : 0;
    uintptr_t wantedHigh = expectedRva + radius;
    if (wantedHigh < expectedRva || wantedHigh > imageSize)
        wantedHigh = imageSize;

    for (size_t s = 0; s < sectionCount; ++s) {
        const uintptr_t sectionLow = sections[s].Rva;
        const uintptr_t sectionHigh = sectionLow + sections[s].Size;
        const uintptr_t scanLow = (std::max)(wantedLow, sectionLow);
        const uintptr_t scanHigh = (std::min)(wantedHigh, sectionHigh);
        if (scanHigh <= scanLow || scanHigh - scanLow < patternLen)
            continue;

        const uintptr_t begin = moduleBase + scanLow;
        if (!IsReadableAddress_104(begin, patternLen))
            continue;
        const uintptr_t last = moduleBase + scanHigh - patternLen;

        for (uintptr_t at = begin; at <= last; ++at) {
            bool ok = true;
            for (size_t i = 0; i < patternLen; ++i) {
                if (mask[i] == 'x' &&
                    *reinterpret_cast<const uint8_t*>(at + i) != pattern[i]) {
                    ok = false;
                    break;
                }
            }
            if (!ok)
                continue;

            matches.push_back(at);
            if (matches.size() >= maxMatches)
                return matches;
        }
    }

    return matches;
}

static uintptr_t ChoosePatternNearestHint_104(
    const std::vector<uintptr_t>& matches,
    uintptr_t moduleBase,
    uintptr_t expectedRva,
    uintptr_t ambiguityMargin,
    uintptr_t* outDistance)
{
    if (outDistance)
        *outDistance = 0;
    if (matches.empty())
        return 0;

    uintptr_t best = 0;
    uintptr_t bestDistance = (std::numeric_limits<uintptr_t>::max)();
    uintptr_t secondDistance = (std::numeric_limits<uintptr_t>::max)();

    for (uintptr_t match : matches) {
        if (match < moduleBase)
            continue;
        const uintptr_t rva = match - moduleBase;
        const uintptr_t distance = rva > expectedRva
            ? rva - expectedRva
            : expectedRva - rva;

        if (distance < bestDistance) {
            secondDistance = bestDistance;
            bestDistance = distance;
            best = match;
        }
        else if (distance < secondDistance) {
            secondDistance = distance;
        }
    }

    if (!best)
        return 0;

    if (matches.size() > 1 &&
        secondDistance != (std::numeric_limits<uintptr_t>::max)() &&
        secondDistance >= bestDistance &&
        (secondDistance - bestDistance) <= ambiguityMargin) {
        return 0;
    }

    if (outDistance)
        *outDistance = bestDistance;
    return best;
}

static uintptr_t ResolvePatternTarget_104(
    const char* name,
    uintptr_t moduleBase,
    uintptr_t knownRva,
    const uint8_t* pattern,
    const char* mask,
    bool useAsShiftAnchor)
{
    const uintptr_t cachedRva = CachedProfileValue_104(g_BuildProfileCache_104.Rvas, name);
    if (cachedRva) {
        const uintptr_t cachedAddress = moduleBase + cachedRva;
        if (MatchMaskedBytes_104(cachedAddress, pattern, mask)) {
            RecordResolver_104(
                name, "fingerprint-profile+pattern", EResolveConfidence_104::ProfileStatic,
                cachedAddress, "cached RVA revalidated against exact build fingerprint");
            CacheResolvedRva_104(name, cachedAddress);
            return cachedAddress;
        }
    }

    const uintptr_t knownAddress = moduleBase + knownRva;
    if (IsKnown104Fingerprint_104(g_CurrentFingerprint_104) &&
        MatchMaskedBytes_104(knownAddress, pattern, mask)) {
        if (useAsShiftAnchor)
            g_AnchorRvaShifts_104.push_back(0);
        RecordResolver_104(
            name, "known-rva+pattern", EResolveConfidence_104::Exact,
            knownAddress, "exact 1.0.4 fingerprint + byte pattern verified");
        CacheResolvedRva_104(name, knownAddress);
        return knownAddress;
    }

    const auto matches = FindPatternInExecutableSections_104(
        moduleBase, pattern, mask, 3);

    if (matches.size() != 1) {
        char detail[96]{};
        snprintf(detail, sizeof(detail), "module-wide matches=%llu",
            static_cast<unsigned long long>(matches.size()));
        RecordResolver_104(
            name, "unique-aob", EResolveConfidence_104::Failed,
            0, detail);
        const intptr_t shifted = static_cast<intptr_t>(knownRva) + g_RvaShiftHint_104;
        AppendDiagnosticWindow_104(name, detail, shifted > 0 ? static_cast<uintptr_t>(shifted) : knownRva);
        return 0;
    }

    const uintptr_t found = matches[0];
    if (useAsShiftAnchor) {
        const intptr_t shift = static_cast<intptr_t>(found - moduleBase) -
            static_cast<intptr_t>(knownRva);
        g_AnchorRvaShifts_104.push_back(shift);
        RecomputeRvaShiftHint_104();
    }

    char detail[128]{};
    snprintf(detail, sizeof(detail), "old=+0x%llX shift=%+lld",
        static_cast<unsigned long long>(knownRva),
        static_cast<long long>(
            static_cast<intptr_t>(found - moduleBase) - static_cast<intptr_t>(knownRva)));

    RecordResolver_104(
        name, "unique-aob", EResolveConfidence_104::Strong,
        found, detail);
    CacheResolvedRva_104(name, found);
    return found;
}

static uintptr_t ResolveNativeFromWrapperCall_104(
    const char* name,
    uintptr_t moduleBase,
    uintptr_t knownCallRva,
    uintptr_t knownNativeRva,
    const uint8_t* callsitePattern,
    const char* callsiteMask,
    size_t callOffset)
{
    const uintptr_t cachedCallRva = CachedProfileValue_104(
        g_BuildProfileCache_104.Callsites, name);
    if (cachedCallRva && cachedCallRva >= callOffset) {
        const uintptr_t cachedPatternStart =
            moduleBase + cachedCallRva - callOffset;
        if (MatchMaskedBytes_104(cachedPatternStart, callsitePattern, callsiteMask)) {
            const uintptr_t cachedCallsite = cachedPatternStart + callOffset;
            const uintptr_t cachedTarget = ResolveRel32Call_104(cachedCallsite);
            if (cachedTarget && IsExecutableAddress_104(cachedTarget)) {
                RecordResolver_104(
                    name, "fingerprint-profile-wrapper-rel32",
                    EResolveConfidence_104::ProfileStatic,
                    cachedTarget,
                    "cached wrapper callsite revalidated against exact build fingerprint");
                CacheResolvedCallsite_104(name, cachedCallsite);
                CacheResolvedRva_104(name, cachedTarget);
                return cachedTarget;
            }
        }
    }

    const uintptr_t knownPatternStart =
        moduleBase + knownCallRva - callOffset;

    // Fast path is authoritative only on the exact known build. On an unknown
    // executable the historical location remains a search hint only.
    if (IsKnown104Fingerprint_104(g_CurrentFingerprint_104) &&
        MatchMaskedBytes_104(knownPatternStart, callsitePattern, callsiteMask)) {
        const uintptr_t callSite = knownPatternStart + callOffset;
        const uintptr_t target = ResolveRel32Call_104(callSite);
        if (target && IsExecutableAddress_104(target)) {
            const bool exactTarget =
                (target == moduleBase + knownNativeRva);
            char detail[128]{};
            snprintf(detail, sizeof(detail),
                "wrapper=+0x%llX native-old=+0x%llX%s",
                static_cast<unsigned long long>(knownCallRva),
                static_cast<unsigned long long>(knownNativeRva),
                exactTarget ? "" : " target-rva-drifted");
            RecordResolver_104(
                name,
                "wrapper-rel32-fast",
                exactTarget ? EResolveConfidence_104::Exact : EResolveConfidence_104::Strong,
                target,
                detail);
            CacheResolvedCallsite_104(name, callSite);
            CacheResolvedRva_104(name, target);
            return target;
        }
    }

    // Unknown/minor build: use the anchor-derived global shift only as a hint,
    // search a bounded neighborhood, then decode the wrapper's own rel32 CALL.
    const intptr_t shifted =
        static_cast<intptr_t>(knownCallRva) + g_RvaShiftHint_104;
    const uintptr_t expectedCallRva = shifted > 0
        ? static_cast<uintptr_t>(shifted)
        : knownCallRva;
    const uintptr_t expectedPatternRva =
        expectedCallRva > callOffset ? expectedCallRva - callOffset : 0;

    auto matches = FindPatternNearRva_104(
        moduleBase,
        expectedPatternRva,
        0x40000,
        callsitePattern,
        callsiteMask,
        32);

    uintptr_t distance = 0;
    const uintptr_t chosen = ChoosePatternNearestHint_104(
        matches,
        moduleBase,
        expectedPatternRva,
        0x80,
        &distance);

    uintptr_t selected = chosen;
    const char* selectionMethod = "wrapper-near-aob+rel32";
    if (!selected) {
        const auto globalMatches = FindPatternInExecutableSections_104(
            moduleBase, callsitePattern, callsiteMask, 3);
        if (globalMatches.size() == 1) {
            selected = globalMatches[0];
            selectionMethod = "wrapper-global-unique-aob+rel32";
            distance = selected >= moduleBase + expectedPatternRva
                ? selected - (moduleBase + expectedPatternRva)
                : (moduleBase + expectedPatternRva) - selected;
        }
        else {
            char detail[160]{};
            snprintf(detail, sizeof(detail),
                "near-hint matches=%llu global-matches=%llu shiftHint=%+lld",
                static_cast<unsigned long long>(matches.size()),
                static_cast<unsigned long long>(globalMatches.size()),
                static_cast<long long>(g_RvaShiftHint_104));
            RecordResolver_104(name, "wrapper-aob", EResolveConfidence_104::Failed, 0, detail);
            AppendDiagnosticWindow_104(name, detail, expectedCallRva);
            return 0;
        }
    }

    const uintptr_t callSite = selected + callOffset;
    const uintptr_t target = ResolveRel32Call_104(callSite);
    if (!target || !IsExecutableAddress_104(target)) {
        RecordResolver_104(
            name, "wrapper-near-aob", EResolveConfidence_104::Failed,
            0, "decoded target is not executable");
        AppendDiagnosticWindow_104(name, "decoded target is not executable", callSite - moduleBase);
        return 0;
    }

    char detail[160]{};
    snprintf(detail, sizeof(detail),
        "callsite=+0x%llX old-call=+0x%llX hint-distance=0x%llX",
        static_cast<unsigned long long>(callSite - moduleBase),
        static_cast<unsigned long long>(knownCallRva),
        static_cast<unsigned long long>(distance));

    RecordResolver_104(
        name, selectionMethod, EResolveConfidence_104::Strong,
        target, detail);
    CacheResolvedCallsite_104(name, callSite);
    CacheResolvedRva_104(name, target);
    return target;
}

static uintptr_t ResolveCallInsideResolvedFunction_104(
    const char* name,
    uintptr_t functionStart,
    size_t scanSize,
    uintptr_t knownTargetRva,
    const uint8_t* localPattern,
    const char* localMask,
    size_t callOffset)
{
    if (!functionStart || !scanSize || !localPattern || !localMask)
        return 0;

    const size_t patternLen = strlen(localMask);
    if (!patternLen || callOffset > patternLen || patternLen - callOffset < 5)
        return 0;
    if (scanSize < patternLen ||
        scanSize >(std::numeric_limits<uintptr_t>::max)() - functionStart)
        return 0;

    std::vector<uintptr_t> matches;
    const uintptr_t end = functionStart + scanSize;
    for (uintptr_t at = functionStart;
        at + patternLen <= end;
        ++at) {
        if (!MatchMaskedBytes_104(at, localPattern, localMask))
            continue;
        matches.push_back(at);
        if (matches.size() > 4)
            break;
    }

    if (matches.size() != 1) {
        char detail[96]{};
        snprintf(detail, sizeof(detail),
            "inside-function matches=%llu",
            static_cast<unsigned long long>(matches.size()));
        RecordResolver_104(
            name, "semantic-local-call", EResolveConfidence_104::Failed,
            0, detail);
        if (g_ModuleBase_104 && functionStart >= g_ModuleBase_104)
            AppendDiagnosticWindow_104(name, detail, functionStart - g_ModuleBase_104);
        return 0;
    }

    const uintptr_t callSite = matches[0] + callOffset;
    const uintptr_t target = ResolveRel32Call_104(callSite);
    if (!target || !IsExecutableAddress_104(target)) {
        RecordResolver_104(
            name, "semantic-local-call", EResolveConfidence_104::Failed,
            0, "decoded rel32 target is not executable");
        if (g_ModuleBase_104 && callSite >= g_ModuleBase_104)
            AppendDiagnosticWindow_104(name, "decoded rel32 target is not executable", callSite - g_ModuleBase_104);
        return 0;
    }

    const uintptr_t rva = target - g_ModuleBase_104;
    char detail[128]{};
    snprintf(detail, sizeof(detail),
        "callsite=+0x%llX old-target=+0x%llX",
        static_cast<unsigned long long>(callSite - g_ModuleBase_104),
        static_cast<unsigned long long>(knownTargetRva));

    RecordResolver_104(
        name,
        "semantic-local-call",
        (IsKnown104Fingerprint_104(g_CurrentFingerprint_104) && rva == knownTargetRva)
        ? EResolveConfidence_104::Exact
        : EResolveConfidence_104::Strong,
        target,
        detail);
    CacheResolvedCallsite_104(name, callSite);
    CacheResolvedRva_104(name, target);
    return target;
}

static bool ResolveVirtualSlot_104(
    const char* name,
    uintptr_t moduleBase,
    uintptr_t knownDispatchRva,
    uintptr_t knownSlot,
    const uint8_t* tailPattern,
    const char* tailMask,
    size_t slotImmediateOffset,
    uintptr_t* outSlot)
{
    if (!outSlot)
        return false;

    auto readSlot = [&](uintptr_t patternStart, uintptr_t* slot) -> bool {
        if (!MatchMaskedBytes_104(patternStart, tailPattern, tailMask))
            return false;
        __try {
            const uint32_t value = *reinterpret_cast<const uint32_t*>(
                patternStart + slotImmediateOffset);
            if (value < 0x100 || value > 0x2000 || (value % sizeof(void*)) != 0)
                return false;
            *slot = static_cast<uintptr_t>(value);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
        };

    const uintptr_t cachedDispatchRva = CachedProfileValue_104(
        g_BuildProfileCache_104.VDispatches, name);
    const uintptr_t cachedSlot = CachedProfileValue_104(
        g_BuildProfileCache_104.VSlots, name);
    if (cachedDispatchRva && cachedSlot) {
        const size_t cachedDispatchOffset = slotImmediateOffset - 2;
        if (cachedDispatchRva >= cachedDispatchOffset) {
            const uintptr_t cachedPatternStart =
                moduleBase + cachedDispatchRva - cachedDispatchOffset;
            uintptr_t verifiedSlot = 0;
            if (readSlot(cachedPatternStart, &verifiedSlot) &&
                verifiedSlot == cachedSlot) {
                *outSlot = verifiedSlot;
                RecordResolver_104(
                    name,
                    "fingerprint-profile-vslot",
                    EResolveConfidence_104::ProfileStatic,
                    moduleBase + cachedDispatchRva,
                    "cached dispatch+slot revalidated against exact build fingerprint");
                CacheResolvedVSlot_104(name, verifiedSlot, moduleBase + cachedDispatchRva);
                return true;
            }
        }
    }

    // knownDispatchRva points to FF 90; derive pattern start from immediate offset:
    // slotImmediateOffset = FF90 offset + 2.
    const size_t dispatchOffset = slotImmediateOffset - 2;
    const uintptr_t knownPatternStart =
        moduleBase + knownDispatchRva - dispatchOffset;
    uintptr_t slot = 0;

    if (IsKnown104Fingerprint_104(g_CurrentFingerprint_104) &&
        readSlot(knownPatternStart, &slot)) {
        *outSlot = slot;
        char detail[96]{};
        snprintf(detail, sizeof(detail), "slot=+0x%llX%s",
            static_cast<unsigned long long>(slot),
            slot == knownSlot ? "" : " changed-from-profile");
        RecordResolver_104(
            name, "wrapper-vslot-fast",
            slot == knownSlot ? EResolveConfidence_104::Exact : EResolveConfidence_104::Strong,
            knownPatternStart + dispatchOffset,
            detail);
        CacheResolvedVSlot_104(name, slot, knownPatternStart + dispatchOffset);
        return true;
    }

    const intptr_t shifted =
        static_cast<intptr_t>(knownDispatchRva) + g_RvaShiftHint_104;
    const uintptr_t expectedDispatchRva = shifted > 0
        ? static_cast<uintptr_t>(shifted)
        : knownDispatchRva;
    const uintptr_t expectedPatternRva =
        expectedDispatchRva > dispatchOffset ? expectedDispatchRva - dispatchOffset : 0;

    auto matches = FindPatternNearRva_104(
        moduleBase,
        expectedPatternRva,
        0x80000,
        tailPattern,
        tailMask,
        16);

    uintptr_t distance = 0;
    const uintptr_t chosen = ChoosePatternNearestHint_104(
        matches, moduleBase, expectedPatternRva, 0x100, &distance);

    uintptr_t selected = chosen;
    const char* selectionMethod = "wrapper-vslot-near-aob";
    if (!selected || !readSlot(selected, &slot)) {
        const auto globalMatches = FindPatternInExecutableSections_104(
            moduleBase, tailPattern, tailMask, 3);
        uintptr_t uniqueValid = 0;
        size_t validCount = 0;
        uintptr_t candidateSlot = 0;
        for (uintptr_t candidate : globalMatches) {
            if (!readSlot(candidate, &candidateSlot))
                continue;
            uniqueValid = candidate;
            slot = candidateSlot;
            ++validCount;
            if (validCount > 1)
                break;
        }
        if (validCount == 1) {
            selected = uniqueValid;
            selectionMethod = "wrapper-vslot-global-unique-aob";
            distance = selected >= moduleBase + expectedPatternRva
                ? selected - (moduleBase + expectedPatternRva)
                : (moduleBase + expectedPatternRva) - selected;
        }
        else {
            char detail[160]{};
            snprintf(detail, sizeof(detail),
                "near-matches=%llu global-valid=%llu shiftHint=%+lld",
                static_cast<unsigned long long>(matches.size()),
                static_cast<unsigned long long>(validCount),
                static_cast<long long>(g_RvaShiftHint_104));
            RecordResolver_104(name, "wrapper-vslot-aob", EResolveConfidence_104::Failed, 0, detail);
            AppendDiagnosticWindow_104(name, detail, expectedDispatchRva);
            return false;
        }
    }

    *outSlot = slot;
    char detail[128]{};
    snprintf(detail, sizeof(detail),
        "slot=+0x%llX old=+0x%llX hint-distance=0x%llX",
        static_cast<unsigned long long>(slot),
        static_cast<unsigned long long>(knownSlot),
        static_cast<unsigned long long>(distance));
    RecordResolver_104(
        name, selectionMethod, EResolveConfidence_104::Strong,
        selected + dispatchOffset,
        detail);
    CacheResolvedVSlot_104(name, slot, selected + dispatchOffset);
    return true;
}

// ---------------------------------------------------------
// Durable elemental matchup helper resolver / installer
// ---------------------------------------------------------
static bool InstallElementMatchupTier_104(uintptr_t moduleBase)
{
    static const uint8_t kPattern[] = {
        0x48,0x89,0x5C,0x24,0x00,
        0x48,0x89,0x6C,0x24,0x00,
        0x48,0x89,0x74,0x24,0x00,
        0x57,0x48,0x83,0xEC,0x00,
        0x41,0x0F,0xB6,0xF0,
        0x0F,0xB6,0xEA,
        0x0F,0xB6,0xF9,
        0x33,0xDB
    };
    static const char kMask[] =
        "xxxx?xxxx?xxxx?xxxx?xxxxxxxxxxxx";

    const uintptr_t target = ResolvePatternTarget_104(
        "CalcElementMatchupTier",
        moduleBase,
        Known104::Rva::MatchupHelper,
        kPattern,
        kMask,
        true);

    if (!target || !IsExecutableAddress_104(target))
        return false;

    // On the known build, additionally verify the semantic callsite relation.
    // On a migrated build the unique helper pattern is accepted as STRONG and
    // the resolver report makes that downgrade explicit.
    if (target == moduleBase + Known104::Rva::MatchupHelper) {
        const uintptr_t callerTarget = ResolveRel32Call_104(
            moduleBase + Known104::Rva::MatchupCallsite);
        if (callerTarget != target) {
            RecordResolver_104(
                "CalcElementMatchupTier.SemanticCallsite",
                "known-caller-rel32",
                EResolveConfidence_104::Failed,
                0,
                "CalcDamageCharacter callsite no longer targets helper");
            return false;
        }
        RecordResolver_104(
            "CalcElementMatchupTier.SemanticCallsite",
            "known-caller-rel32",
            IsKnown104Fingerprint_104(g_CurrentFingerprint_104)
            ? EResolveConfidence_104::Exact
            : EResolveConfidence_104::Strong,
            moduleBase + Known104::Rva::MatchupCallsite,
            IsKnown104Fingerprint_104(g_CurrentFingerprint_104)
            ? "CalcDamageCharacter -> tier helper verified on exact 1.0.4"
            : "historical callsite still semantically targets uniquely resolved helper; treated as strong only");
        CacheResolvedCallsite_104(
            "CalcElementMatchupTier.SemanticCallsite",
            moduleBase + Known104::Rva::MatchupCallsite);
    }

    const MH_STATUS create = MH_CreateHook(
        reinterpret_cast<LPVOID>(target),
        reinterpret_cast<LPVOID>(&Detour_CalcElementMatchupTier_104),
        reinterpret_cast<LPVOID*>(&Original_CalcElementMatchupTier_104));
    if (create != MH_OK) {
        ShipLog_104("[ElementalSystemExpanded] ERROR: matchup hook create failed (%d).\n",
            static_cast<int>(create));
        return false;
    }

    const MH_STATUS enable = MH_EnableHook(reinterpret_cast<LPVOID>(target));
    if (enable != MH_OK) {
        MH_RemoveHook(reinterpret_cast<LPVOID>(target));
        Original_CalcElementMatchupTier_104 = nullptr;
        ShipLog_104("[ElementalSystemExpanded] ERROR: matchup hook enable failed (%d).\n",
            static_cast<int>(enable));
        return false;
    }

    g_Resolved_MatchupHelper_104 = target;
    return true;
}

// ---------------------------------------------------------
// Raw FPalDamageInfo capture/context
// ---------------------------------------------------------
using DamageEffectCaller_104_t =
uintptr_t(__fastcall*)(void* DamageReaction, const void* DamageInfo);

static DamageEffectCaller_104_t Original_DamageEffectCaller_104 = nullptr;
static uintptr_t g_DamageEffectCaller_104 = 0;

static bool IsTrackedElementalEffect_104(uint8_t effect) {
    return effect == ActiveIds::Effect_Burn ||
        effect == ActiveIds::Effect_Wetness ||
        effect == ActiveIds::Effect_Freeze ||
        effect == ActiveIds::Effect_Electrical ||
        effect == ActiveIds::Effect_Muddy ||
        effect == ActiveIds::Effect_IvyCling ||
        effect == ActiveIds::Effect_Darkness;
}

static const char* ElementalEffectName_104(uint8_t effect) {
    if (effect == ActiveIds::Effect_Burn) return "Burn";
    if (effect == ActiveIds::Effect_Wetness) return "Wetness";
    if (effect == ActiveIds::Effect_Freeze) return "Freeze";
    if (effect == ActiveIds::Effect_Electrical) return "Electrical";
    if (effect == ActiveIds::Effect_Muddy) return "Muddy";
    if (effect == ActiveIds::Effect_IvyCling) return "IvyCling";
    if (effect == ActiveIds::Effect_Darkness) return "Darkness";
    return "Other";
}

static bool SnapshotRawDamageInfo_104(
    const void* damageInfo,
    uint8_t* effect1,
    int32_t* value1,
    uint8_t* effect2,
    int32_t* value2)
{
    if (!damageInfo || !effect1 || !value1 || !effect2 || !value2)
        return false;

    const uintptr_t p = reinterpret_cast<uintptr_t>(damageInfo);

    const auto readableField = [p](uintptr_t offset, size_t size) -> bool {
        if (offset > (std::numeric_limits<uintptr_t>::max)() - p)
            return false;
        return IsReadableAddress_104(p + offset, size);
        };

    if (!readableField(ActiveLayout::DamageInfo_EffectType1, sizeof(uint8_t)) ||
        !readableField(ActiveLayout::DamageInfo_EffectValue1, sizeof(int32_t)) ||
        !readableField(ActiveLayout::DamageInfo_EffectType2, sizeof(uint8_t)) ||
        !readableField(ActiveLayout::DamageInfo_EffectValue2, sizeof(int32_t))) {
        return false;
    }

    __try {
        *effect1 = *reinterpret_cast<const uint8_t*>(p + ActiveLayout::DamageInfo_EffectType1);
        *value1 = *reinterpret_cast<const int32_t*>(p + ActiveLayout::DamageInfo_EffectValue1);
        *effect2 = *reinterpret_cast<const uint8_t*>(p + ActiveLayout::DamageInfo_EffectType2);
        *value2 = *reinterpret_cast<const int32_t*>(p + ActiveLayout::DamageInfo_EffectValue2);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// One FPalDamageInfo caller invocation wraps the subsequent calls to
// +0x2C0C680, so a thread-local context is sufficient to carry the original
// integer EffectValue into the accumulation hook without global races.
struct FRawEffectContext_104 {
    bool Active = false;
    uint8_t AttackElement = 0;
    uint8_t Effect1 = 0;
    int32_t Value1 = 0;
    uint8_t Effect2 = 0;
    int32_t Value2 = 0;
    bool Consumed1 = false;
    bool Consumed2 = false;
};

static thread_local FRawEffectContext_104 g_RawEffectContext_104[4];
static thread_local int g_RawEffectDepth_104 = 0;

struct FRawEffectContextGuard_104 {
    bool Entered = false;

    FRawEffectContextGuard_104(
        uint8_t attackElement,
        uint8_t effect1, int32_t value1,
        uint8_t effect2, int32_t value2)
    {
        if (g_RawEffectDepth_104 >= 4)
            return;

        auto& ctx = g_RawEffectContext_104[g_RawEffectDepth_104++];
        ctx.Active = true;
        ctx.AttackElement = attackElement;
        ctx.Effect1 = effect1;
        ctx.Value1 = value1;
        ctx.Effect2 = effect2;
        ctx.Value2 = value2;
        ctx.Consumed1 = false;
        ctx.Consumed2 = false;
        Entered = true;
    }

    ~FRawEffectContextGuard_104()
    {
        if (!Entered)
            return;

        --g_RawEffectDepth_104;
        g_RawEffectContext_104[g_RawEffectDepth_104] = {};
    }
};

static bool GetRawUnitsForEffect_104(uint8_t effect, int32_t* outRawValue)
{
    if (!outRawValue || g_RawEffectDepth_104 <= 0)
        return false;

    for (int depth = g_RawEffectDepth_104 - 1; depth >= 0; --depth) {
        auto& ctx = g_RawEffectContext_104[depth];
        if (!ctx.Active)
            continue;

        if (!ctx.Consumed1 && ctx.Effect1 == effect) {
            ctx.Consumed1 = true;
            *outRawValue = ctx.Value1;
            return true;
        }

        if (!ctx.Consumed2 && ctx.Effect2 == effect) {
            ctx.Consumed2 = true;
            *outRawValue = ctx.Value2;
            return true;
        }
    }

    return false;
}

static bool GetRawAttackElement_104(uint8_t* outAttackElement)
{
    if (!outAttackElement || g_RawEffectDepth_104 <= 0)
        return false;

    for (int depth = g_RawEffectDepth_104 - 1; depth >= 0; --depth) {
        const auto& ctx = g_RawEffectContext_104[depth];
        if (!ctx.Active)
            continue;

        *outAttackElement = ctx.AttackElement;
        return true;
    }

    return false;
}

static uint8_t UnitsFromRawValue_104(int32_t rawValue)
{
    // User rule:
    //   1  -> 1U
    //   2  -> 2U
    //   >2 -> forced/capped at 2U
    // Non-positive values are not expected for a supported elemental effect;
    // treating them as 1U keeps the replacement conservative.
    return (rawValue >= 2) ? 2 : 1;
}

static int CountRel32CallsToTarget_104(
    uintptr_t functionStart,
    size_t scanSize,
    uintptr_t target)
{
    if (!functionStart || !target || !scanSize)
        return 0;

    int count = 0;
    for (size_t i = 0; i + 5 <= scanSize; ++i) {
        const uintptr_t at = functionStart + i;
        if (!IsReadableAddress_104(at, 5))
            continue;
        __try {
            if (*reinterpret_cast<const uint8_t*>(at) != 0xE8)
                continue;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            continue;
        }
        if (ResolveRel32Call_104(at) == target)
            ++count;
    }
    return count;
}

static uintptr_t FindEffectCallerStart_104(uintptr_t moduleBase)
{
    if (!moduleBase || !g_Resolved_ElementBuildup_104)
        return 0;

    static const uint8_t kCallerPrologue[] = {
        0x48,0x8B,0xC4,
        0x48,0x89,0x58,0x18,
        0x55,0x56,0x57,
        0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,
        0x48,0x8D,0x68,0x00,
        0x48,0x81,0xEC,0x00,0x01,0x00,0x00,
        0x0F,0x29,0x70,0xB8,
        0x0F,0x29,0x78,0xA8
    };
    static const char kCallerMask[] =
        "xxxxxxxxxxxxxxxxxxxxx?xxxxxxxxxxxxxxx";

    auto qualifies = [&](uintptr_t candidate) -> bool {
        if (!candidate || !MatchMaskedBytes_104(candidate, kCallerPrologue, kCallerMask))
            return false;
        return CountRel32CallsToTarget_104(
            candidate, 0x300, g_Resolved_ElementBuildup_104) >= 2;
        };

    const uintptr_t cachedRva = CachedProfileValue_104(
        g_BuildProfileCache_104.Rvas,
        "RawFPalDamageInfoCaller");
    if (cachedRva) {
        const uintptr_t cached = moduleBase + cachedRva;
        if (qualifies(cached)) {
            RecordResolver_104(
                "RawFPalDamageInfoCaller",
                "fingerprint-profile+prologue+dual-call",
                EResolveConfidence_104::ProfileStatic,
                cached,
                "cached raw caller revalidated against exact build fingerprint");
            CacheResolvedRva_104("RawFPalDamageInfoCaller", cached);
            return cached;
        }
    }

    const uintptr_t known = moduleBase + Known104::Rva::RawEffectCaller;
    if (qualifies(known)) {
        const bool knownOracle = IsKnown104Fingerprint_104(g_CurrentFingerprint_104);
        RecordResolver_104(
            "RawFPalDamageInfoCaller",
            "known-rva+prologue+dual-call",
            knownOracle ? EResolveConfidence_104::Exact : EResolveConfidence_104::Strong,
            known,
            knownOracle
            ? "exact 1.0.4 prologue + two calls to resolved buildup target"
            : "historical RVA used only as hint; prologue + dual-call semantics independently verified");
        CacheResolvedRva_104("RawFPalDamageInfoCaller", known);
        return known;
    }

    const auto matches = FindPatternInExecutableSections_104(
        moduleBase, kCallerPrologue, kCallerMask, 16);
    uintptr_t unique = 0;
    int qualified = 0;
    for (uintptr_t candidate : matches) {
        if (!qualifies(candidate))
            continue;
        unique = candidate;
        ++qualified;
        if (qualified > 1)
            break;
    }

    if (qualified != 1) {
        char detail[96]{};
        snprintf(detail, sizeof(detail),
            "prologue-matches=%llu qualified=%d",
            static_cast<unsigned long long>(matches.size()), qualified);
        RecordResolver_104(
            "RawFPalDamageInfoCaller",
            "unique-prologue+dual-call",
            EResolveConfidence_104::Failed,
            0,
            detail);
        const intptr_t shifted = static_cast<intptr_t>(Known104::Rva::RawEffectCaller) + g_RvaShiftHint_104;
        AppendDiagnosticWindow_104(
            "RawFPalDamageInfoCaller",
            detail,
            shifted > 0 ? static_cast<uintptr_t>(shifted) : Known104::Rva::RawEffectCaller);
        return 0;
    }

    const intptr_t shift = static_cast<intptr_t>(unique - moduleBase) -
        static_cast<intptr_t>(Known104::Rva::RawEffectCaller);
    g_AnchorRvaShifts_104.push_back(shift);
    RecomputeRvaShiftHint_104();

    char detail[96]{};
    snprintf(detail, sizeof(detail), "shift=%+lld dual-call verified",
        static_cast<long long>(shift));
    RecordResolver_104(
        "RawFPalDamageInfoCaller",
        "unique-prologue+dual-call",
        EResolveConfidence_104::Strong,
        unique,
        detail);
    CacheResolvedRva_104("RawFPalDamageInfoCaller", unique);
    return unique;
}

// Keep SEH isolated from Detour_DamageEffectCaller_104. That detour owns
// FRawEffectContextGuard_104 (a C++ object with a destructor), and MSVC rejects
// __try in any function that requires C++ object unwinding (C2712).
// Use the active generated/validated layout rather than hardcoding the historical
// 1.0.4 +0x30 offset.
static uint8_t SafeReadAttackElement_104(const void* damageInfo)
{
    if (!damageInfo)
        return 0;

    __try {
        return *reinterpret_cast<const uint8_t*>(
            reinterpret_cast<uintptr_t>(damageInfo) +
            ActiveLayout::DamageInfo_AttackElement);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

static uintptr_t __fastcall Detour_DamageEffectCaller_104(
    void* damageReaction,
    const void* damageInfo)
{
    uint8_t effect1 = 0;
    int32_t value1 = 0;
    uint8_t effect2 = 0;
    int32_t value2 = 0;

    const bool haveRaw = SnapshotRawDamageInfo_104(
        damageInfo, &effect1, &value1, &effect2, &value2);

    const uint8_t attackElement =
        haveRaw ? SafeReadAttackElement_104(damageInfo) : 0;

    FRawEffectContextGuard_104 guard(
        haveRaw ? attackElement : 0,
        haveRaw ? effect1 : 0,
        haveRaw ? value1 : 0,
        haveRaw ? effect2 : 0,
        haveRaw ? value2 : 0);

    if (haveRaw &&
        (IsTrackedElementalEffect_104(effect1) ||
            IsTrackedElementalEffect_104(effect2))) {
        ModLog(
            "[ElementalSystemExpanded] RAW UNITS: DamageReaction=%p DamageInfo=%p "
            "AttackElement=%u | E1=%u (%s) V1=%d | E2=%u (%s) V2=%d\n",
            damageReaction,
            damageInfo,
            static_cast<unsigned>(attackElement),
            static_cast<unsigned>(effect1), ElementalEffectName_104(effect1), value1,
            static_cast<unsigned>(effect2), ElementalEffectName_104(effect2), value2);
    }

    if (Original_DamageEffectCaller_104)
        return Original_DamageEffectCaller_104(damageReaction, damageInfo);

    return 0;
}

// ---------------------------------------------------------
// Native elemental status replacement
// ---------------------------------------------------------
using AddElementStatusAdditionalValue_OneType_104_t =
void(__fastcall*)(void* DamageReactionComponent, uint8_t Effect, float Value);

using NativeAddStatus_104_t =
void(__fastcall*)(void* StatusComponent, uint8_t StatusID);

static AddElementStatusAdditionalValue_OneType_104_t
Original_AddElementStatusAdditionalValue_OneType_104 = nullptr;

static NativeAddStatus_104_t Original_NativeAddStatus_104 = nullptr;
static NativeAddStatus_104_t Native_AddStatus_104 = nullptr;

static constexpr double kElementICDSeconds_104 = 2.5;
static constexpr float kOneUnitDuration_104 = 7.5f;
static constexpr float kTwoUnitDuration_104 = 12.0f;
static constexpr float kDarknessDuration_104 = 3.0f;
static constexpr float kWetFreezeDuration_104 = 2.0f;
static constexpr float kMinimumExchangeCarryover_104 = 2.5f;
// Never write an eroded status timer to exact zero. Palworld's native status
// lifecycle expects a positive timer to cross through zero inside TickStatus;
// writing 0 directly can strand the status object in ExecutionStatusList.
static constexpr float kErosionExpiryTick_104 = 0.001f;
static constexpr uint8_t kMaxConsecutiveHits_104 = 3;

static float GaugeDurationForUnits_104(uint8_t units)
{
    return (units >= 2)
        ? kTwoUnitDuration_104
        : kOneUnitDuration_104;
}

static float MaxDurationForStatus_104(uint8_t statusID)
{
    // Genuine Dark and Light blindness share native Darkness status ID 25.
    // Both retain the established 3-second maximum despite the generic
    // 7.5s / 12s gauge budget used by the exchange system.
    return (statusID == ActiveIds::Status_Darkness)
        ? kDarknessDuration_104
        : kTwoUnitDuration_104;
}

static uint8_t ElementalEffectToStatusID_104(uint8_t effect) {
    if (effect == ActiveIds::Effect_Burn) return ActiveIds::Status_Burn;
    if (effect == ActiveIds::Effect_Wetness) return ActiveIds::Status_Wetness;
    if (effect == ActiveIds::Effect_Freeze) return ActiveIds::Status_Freeze;
    if (effect == ActiveIds::Effect_Electrical) return ActiveIds::Status_Electrical;
    if (effect == ActiveIds::Effect_Muddy) return ActiveIds::Status_Muddy;
    if (effect == ActiveIds::Effect_IvyCling) return ActiveIds::Status_IvyCling;
    if (effect == ActiveIds::Effect_Darkness) return ActiveIds::Status_Darkness;
    return 0;
}

static bool IsTrackedElementalStatusID_104(uint8_t statusID)
{
    return statusID == ActiveIds::Status_Burn ||
        statusID == ActiveIds::Status_Wetness ||
        statusID == ActiveIds::Status_Freeze ||
        statusID == ActiveIds::Status_Electrical ||
        statusID == ActiveIds::Status_Muddy ||
        statusID == ActiveIds::Status_IvyCling ||
        statusID == ActiveIds::Status_Darkness;
}

static uint8_t NominalElementForEffect_104(uint8_t effect)
{
    if (effect == ActiveIds::Effect_Burn) return ActiveIds::Element_Fire;
    if (effect == ActiveIds::Effect_Wetness) return ActiveIds::Element_Water;
    if (effect == ActiveIds::Effect_Freeze) return ActiveIds::Element_Ice;
    if (effect == ActiveIds::Effect_Electrical) return ActiveIds::Element_Electricity;
    if (effect == ActiveIds::Effect_Muddy) return ActiveIds::Element_Earth;
    if (effect == ActiveIds::Effect_IvyCling) return ActiveIds::Element_Leaf;
    if (effect == ActiveIds::Effect_Darkness) return ActiveIds::Element_Dark;
    return ActiveIds::Element_None;
}

static uint8_t NominalElementForStatus_104(uint8_t statusID)
{
    if (statusID == ActiveIds::Status_Burn) return ActiveIds::Element_Fire;
    if (statusID == ActiveIds::Status_Wetness) return ActiveIds::Element_Water;
    if (statusID == ActiveIds::Status_Freeze) return ActiveIds::Element_Ice;
    if (statusID == ActiveIds::Status_Electrical) return ActiveIds::Element_Electricity;
    if (statusID == ActiveIds::Status_Muddy) return ActiveIds::Element_Earth;
    if (statusID == ActiveIds::Status_IvyCling) return ActiveIds::Element_Leaf;
    if (statusID == ActiveIds::Status_Darkness) return ActiveIds::Element_Dark;
    return ActiveIds::Element_None;
}

static uint8_t ResolveIncomingElement_104(uint8_t effect)
{
    uint8_t attackElement = ActiveIds::Element_None;
    if (GetRawAttackElement_104(&attackElement) &&
        attackElement >= ActiveIds::Element_Normal &&
        attackElement <= ActiveIds::Element_Dragon) {
        return attackElement;
    }

    return NominalElementForEffect_104(effect);
}

static const char* StatusIDName_104(uint8_t statusID) {
    if (statusID == ActiveIds::Status_Burn) return "Burn";
    if (statusID == ActiveIds::Status_Wetness) return "Wetness";
    if (statusID == ActiveIds::Status_Freeze) return "Freeze";
    if (statusID == ActiveIds::Status_Electrical) return "Electrical";
    if (statusID == ActiveIds::Status_Muddy) return "Muddy";
    if (statusID == ActiveIds::Status_IvyCling) return "IvyCling";
    if (statusID == ActiveIds::Status_Darkness) return "Darkness";
    return "Other";
}

// ---------------------------------------------------------
// DamageReactionComponent -> APalCharacter -> StatusComponent
//
// UObject::OuterPrivate is intentionally NOT trusted as a static layout
// constant anymore. The SDK does not emit it. We treat +0x20 only as a known
// 1.0.4 hint, validate it through the APalCharacter reverse member, and scan a
// tiny pointer-aligned UObject header window if the hint no longer works.
// ---------------------------------------------------------
static bool ReadPointerAtOffset_104(
    uintptr_t object,
    uintptr_t offset,
    uintptr_t* outValue)
{
    if (!object || !outValue ||
        offset > (std::numeric_limits<uintptr_t>::max)() - object) {
        return false;
    }

    const uintptr_t fieldAddress = object + offset;
    if (!IsReadableAddress_104(fieldAddress, sizeof(uintptr_t)))
        return false;

    __try {
        *outValue = *reinterpret_cast<const uintptr_t*>(fieldAddress);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        *outValue = 0;
        return false;
    }
}

static bool TryResolveOwningPalCharacterWithOuterOffset_104(
    void* damageReaction,
    uintptr_t outerOffset,
    void** outCharacter)
{
    if (outCharacter)
        *outCharacter = nullptr;
    if (!damageReaction || !outerOffset)
        return false;

    const uintptr_t original = reinterpret_cast<uintptr_t>(damageReaction);
    uintptr_t current = original;

    for (int depth = 0; depth < 4 && current; ++depth) {
        uintptr_t outer = 0;
        if (!ReadPointerAtOffset_104(current, outerOffset, &outer) || !outer)
            return false;

        uintptr_t reverse = 0;
        if (ReadPointerAtOffset_104(
            outer,
            ActiveLayout::Character_DamageReactionComponent,
            &reverse) &&
            reverse == original) {
            if (outCharacter)
                *outCharacter = reinterpret_cast<void*>(outer);
            return true;
        }

        current = outer;
    }

    return false;
}

static void* ResolveOwningPalCharacter_104(void* damageReaction) {
    if (!damageReaction)
        return nullptr;

    uintptr_t cached = 0;
    AcquireSRWLockShared(&g_UObjectOuterOffsetLock_104);
    cached = g_RuntimeUObjectOuterOffset_104;
    ReleaseSRWLockShared(&g_UObjectOuterOffsetLock_104);

    void* character = nullptr;
    if (cached &&
        TryResolveOwningPalCharacterWithOuterOffset_104(
            damageReaction, cached, &character)) {
        return character;
    }

    AcquireSRWLockExclusive(&g_UObjectOuterOffsetLock_104);

    // Another thread may have discovered it while we waited.
    if (g_RuntimeUObjectOuterOffset_104 &&
        TryResolveOwningPalCharacterWithOuterOffset_104(
            damageReaction,
            g_RuntimeUObjectOuterOffset_104,
            &character)) {
        ReleaseSRWLockExclusive(&g_UObjectOuterOffsetLock_104);
        return character;
    }

    // First try the SDK value when emitted; otherwise the old value is only a
    // fast hint. It is accepted solely if the generated APalCharacter reverse
    // member proves the relationship.
    uintptr_t hint = Known104::Offset::UObject_OuterPrivate;
#if ESE_HAS_GENERATED_ENGINE_OPTIONALS
    if (EseGeneratedPalLayout::Has_UObject_OuterPrivate)
        hint = EseGeneratedPalLayout::UObject_OuterPrivate;
#endif

    if (TryResolveOwningPalCharacterWithOuterOffset_104(
        damageReaction, hint, &character)) {
        g_RuntimeUObjectOuterOffset_104 = hint;
    }
    else {
        uintptr_t candidates[32]{};
        void* candidateCharacters[32]{};
        int count = 0;

        // UObject header members are pointer aligned and low in the object.
        // Keep this deliberately narrow; if the layout is radically different
        // we fail closed instead of scanning arbitrary object memory.
        for (uintptr_t off = 0x10; off <= 0x100; off += sizeof(uintptr_t)) {
            if (off == hint)
                continue;
            void* candidateCharacter = nullptr;
            if (TryResolveOwningPalCharacterWithOuterOffset_104(
                damageReaction, off, &candidateCharacter)) {
                if (count < 32) {
                    candidates[count] = off;
                    candidateCharacters[count] = candidateCharacter;
                    ++count;
                }
            }
        }

        if (count == 1) {
            g_RuntimeUObjectOuterOffset_104 = candidates[0];
            character = candidateCharacters[0];
        }
        else {
            ShipLog_104(
                "[ElementalSystemExpanded] WARNING: OWNER LINK DISCOVERY %s: DamageReaction=%p "
                "Candidates=%d; no owner returned.\n",
                count == 0 ? "FAILED" : "AMBIGUOUS",
                damageReaction,
                count);
            ReleaseSRWLockExclusive(&g_UObjectOuterOffsetLock_104);
            return nullptr;
        }
    }

    const uintptr_t discovered = g_RuntimeUObjectOuterOffset_104;
    const bool firstLog = !g_RuntimeUObjectOuterLogged_104;
    g_RuntimeUObjectOuterLogged_104 = true;
    ReleaseSRWLockExclusive(&g_UObjectOuterOffsetLock_104);

    if (firstLog) {
        char detail[192]{};
        snprintf(
            detail,
            sizeof(detail),
            "discovered Outer link +0x%llX via APalCharacter::DamageReactionComponent reverse pointer",
            static_cast<unsigned long long>(discovered));
        RecordResolver_104(
            "Runtime.UObjectOuterPrivate",
            "runtime-owner-backreference",
            EResolveConfidence_104::RuntimeValidated,
            0,
            detail);
        ModLog(
            "[ElementalSystemExpanded] OWNER LINK DISCOVERY PASSED: "
            "OuterOffset=+0x%llX DamageReaction=%p Character=%p.\n",
            static_cast<unsigned long long>(discovered),
            damageReaction,
            character);
        MarkRuntimeSnapshotDirty_104();
        // No file I/O here: runtime discovery happens on a gameplay path.
        // The one-time console record is enough; persistent artifacts remain
        // startup-only to avoid introducing action-time disk latency.
    }

    return character;
}

static void* ResolveStatusComponent_104(void* damageReaction) {
    void* character = ResolveOwningPalCharacter_104(damageReaction);
    if (!character)
        return nullptr;

    uintptr_t statusComponent = 0;
    if (!ReadPointerAtOffset_104(
        reinterpret_cast<uintptr_t>(character),
        ActiveLayout::Character_StatusComponent,
        &statusComponent)) {
        return nullptr;
    }

    return reinterpret_cast<void*>(statusComponent);
}

// ---------------------------------------------------------
// ICD state: keyed by defender DamageReactionComponent + element effect
// ---------------------------------------------------------
struct FElementICDState_104 {
    double LastSuccessfulApplicationTime = 0.0;
    uint8_t ConsecutiveBlockedHits = 0;
    bool HasSuccessfulApplication = false;
};

struct FElementICDKey_104 {
    void* DamageReaction = nullptr;
    uint8_t Effect = 0;

    bool operator==(const FElementICDKey_104& other) const {
        return DamageReaction == other.DamageReaction && Effect == other.Effect;
    }
};

struct FElementICDKeyHash_104 {
    size_t operator()(const FElementICDKey_104& key) const noexcept {
        const size_t p = std::hash<uintptr_t>{}(
            reinterpret_cast<uintptr_t>(key.DamageReaction));
        const size_t e = std::hash<unsigned>{}(key.Effect);
        return p ^ (e + static_cast<size_t>(0x9E3779B9u) + (p << 6) + (p >> 2));
    }
};

static std::unordered_map<
    FElementICDKey_104,
    FElementICDState_104,
    FElementICDKeyHash_104> g_ElementICD_104;

static SRWLOCK g_ElementICDLock_104 = SRWLOCK_INIT;

static double GetMonotonicSeconds_104()
{
    return static_cast<double>(GetTickCount64()) / 1000.0;
}

static bool ElementICDAllows_104(
    void* damageReaction,
    uint8_t effect,
    uint8_t* outPreviousBlockedHits,
    double* outElapsed)
{
    const double now = GetMonotonicSeconds_104();
    const FElementICDKey_104 key{ damageReaction, effect };

    AcquireSRWLockExclusive(&g_ElementICDLock_104);
    auto& state = g_ElementICD_104[key];

    *outPreviousBlockedHits = state.ConsecutiveBlockedHits;
    *outElapsed = state.HasSuccessfulApplication
        ? (now - state.LastSuccessfulApplicationTime)
        : 1.0e30;

    if (!state.HasSuccessfulApplication) {
        ReleaseSRWLockExclusive(&g_ElementICDLock_104);
        return true;
    }

    if (*outElapsed >= kElementICDSeconds_104) {
        // A timer-expired application is allowed immediately. The successful
        // application record will clear the hit chain below.
        ReleaseSRWLockExclusive(&g_ElementICDLock_104);
        return true;
    }

    // Current registration becomes hit #1/#2/#3 after the last successful
    // application. Third registration is allowed.
    if (state.ConsecutiveBlockedHits + 1 >= kMaxConsecutiveHits_104) {
        ReleaseSRWLockExclusive(&g_ElementICDLock_104);
        return true;
    }

    ++state.ConsecutiveBlockedHits;
    *outPreviousBlockedHits = state.ConsecutiveBlockedHits;

    ReleaseSRWLockExclusive(&g_ElementICDLock_104);
    return false;
}

static void ElementICDRecordSuccess_104(void* damageReaction, uint8_t effect)
{
    const FElementICDKey_104 key{ damageReaction, effect };

    AcquireSRWLockExclusive(&g_ElementICDLock_104);
    auto& state = g_ElementICD_104[key];
    state.LastSuccessfulApplicationTime = GetMonotonicSeconds_104();
    state.ConsecutiveBlockedHits = 0;
    state.HasSuccessfulApplication = true;
    ReleaseSRWLockExclusive(&g_ElementICDLock_104);
}

// ---------------------------------------------------------
// Status list / duration helpers
// ---------------------------------------------------------
struct FRawTArray_104 {
    uintptr_t Data;
    int32_t Num;
    int32_t Max;
};

static_assert(sizeof(FRawTArray_104) == 0x10, "Unexpected local TArray ABI mirror size");
static_assert(offsetof(FRawTArray_104, Data) == 0x0, "Unexpected local TArray Data offset");
static_assert(offsetof(FRawTArray_104, Num) == 0x8, "Unexpected local TArray Num offset");
static_assert(offsetof(FRawTArray_104, Max) == 0xC, "Unexpected local TArray Max offset");

static bool ValidateRawPointerArray_104(
    const FRawTArray_104& list,
    int32_t maxNum,
    int32_t maxCapacity)
{
    if (list.Num < 0 || list.Max < 0 ||
        list.Num > list.Max ||
        list.Num > maxNum ||
        list.Max > maxCapacity) {
        return false;
    }

    if (list.Max == 0)
        return list.Num == 0;

    if (!list.Data ||
        (list.Data % alignof(void*)) != 0) {
        return false;
    }

    if (list.Num == 0)
        return true;

    const size_t count = static_cast<size_t>(list.Num);
    if (count > (std::numeric_limits<size_t>::max)() / sizeof(void*))
        return false;

    return IsReadableAddress_104(
        list.Data,
        count * sizeof(void*));
}

static bool ValidateUObjectLikePointer_104(
    void* object,
    size_t readableSpan)
{
    if (!object)
        return false;

    const uintptr_t address = reinterpret_cast<uintptr_t>(object);
    if ((address % alignof(void*)) != 0 ||
        !IsReadableAddress_104(address, readableSpan)) {
        return false;
    }

    uintptr_t vtable = 0;
    uintptr_t firstVirtual = 0;
    __try {
        vtable = *reinterpret_cast<const uintptr_t*>(address);
        if (!vtable || !IsReadableAddress_104(vtable, sizeof(uintptr_t)))
            return false;
        firstVirtual = *reinterpret_cast<const uintptr_t*>(vtable);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }

    return IsExecutableAddress_104(firstVirtual);
}

// ---------------------------------------------------------
// Light self-element immunity
//
// Light is still carried by native Darkness (effect 10/status 25). Vanilla
// Darkness resistance therefore cannot express the mod rule that Neutral/Light
// Pals are immune to Light blindness. The target classification uses the Pal's
// live element slots and IsPal flag from the fingerprint-bound generated layout.
//
// Source-specific behavior (validated in the earlier branch):
//   Light source + Normal target Pal  -> reject blindness before ICD/AddStatus.
//   Genuine Dark source               -> unchanged.
//   Human/player/non-Pal target       -> unchanged by this extra rule.
//   Dual-element Pal containing Normal -> immune.
// ---------------------------------------------------------
static LONG g_LightNeutralLayoutWarningLogged_104 = 0;

static bool IsNeutralPalTarget_104(
    void* character,
    bool* outLayoutValidated)
{
    if (outLayoutValidated)
        *outLayoutValidated = false;

    if (!character)
        return false;

    uintptr_t characterParameter = 0;
    uintptr_t staticParameter = 0;

    if (!ReadPointerAtOffset_104(
        reinterpret_cast<uintptr_t>(character),
        ActiveLayout::Character_CharacterParameterComponent,
        &characterParameter) ||
        !ReadPointerAtOffset_104(
            reinterpret_cast<uintptr_t>(character),
            ActiveLayout::Character_StaticCharacterParameterComponent,
            &staticParameter) ||
        !characterParameter ||
        !staticParameter) {
        return false;
    }

    if (ActiveLayout::CharacterParameter_ElementType2 >
        (std::numeric_limits<size_t>::max)() - sizeof(uint8_t) ||
        ActiveLayout::StaticCharacterParameter_IsPal >
        (std::numeric_limits<size_t>::max)() - sizeof(uint8_t)) {
        return false;
    }

    const size_t characterParameterSpan =
        static_cast<size_t>(
            ActiveLayout::CharacterParameter_ElementType2 +
            sizeof(uint8_t));
    const size_t staticParameterSpan =
        static_cast<size_t>(
            ActiveLayout::StaticCharacterParameter_IsPal +
            sizeof(uint8_t));

    if (!ValidateUObjectLikePointer_104(
        reinterpret_cast<void*>(characterParameter),
        characterParameterSpan) ||
        !ValidateUObjectLikePointer_104(
            reinterpret_cast<void*>(staticParameter),
            staticParameterSpan)) {
        return false;
    }

    uint8_t element1 = 0xFF;
    uint8_t element2 = 0xFF;
    uint8_t isPal = 0xFF;

    __try {
        element1 = *reinterpret_cast<const uint8_t*>(
            characterParameter +
            ActiveLayout::CharacterParameter_ElementType1);
        element2 = *reinterpret_cast<const uint8_t*>(
            characterParameter +
            ActiveLayout::CharacterParameter_ElementType2);
        isPal = *reinterpret_cast<const uint8_t*>(
            staticParameter +
            ActiveLayout::StaticCharacterParameter_IsPal);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }

    // Current generated EPalElementType gameplay range is None..Dragon.
    if (element1 > ActiveIds::Element_Dragon ||
        element2 > ActiveIds::Element_Dragon ||
        isPal > 1) {
        return false;
    }

    if (outLayoutValidated)
        *outLayoutValidated = true;

    if (!isPal)
        return false;

    return element1 == ActiveIds::Element_Normal ||
        element2 == ActiveIds::Element_Normal;
}

static bool ShouldBlockLightBlindOnNeutralPal_104(
    void* character,
    uint8_t effect)
{
    if (effect != ActiveIds::Effect_Darkness)
        return false;

    // Preserve the already-validated branch discriminator: the blindness carrier
    // must be Darkness and the captured attack element must be Normal/Light.
    uint8_t attackElement = ActiveIds::Element_None;
    if (!GetRawAttackElement_104(&attackElement) ||
        attackElement != ActiveIds::Element_Normal) {
        return false;
    }

    bool layoutValidated = false;
    const bool neutralPal =
        IsNeutralPalTarget_104(
            character,
            &layoutValidated);

    if (!layoutValidated) {
        if (InterlockedCompareExchange(
            &g_LightNeutralLayoutWarningLogged_104,
            1,
            0) == 0) {
            ShipLog_104(
                "[ElementalSystemExpanded] WARNING: Light Neutral-immunity "
                "target layout could not be validated; failing open and "
                "preserving existing status behavior. Character=%p\n",
                character);
        }
        return false;
    }

    return neutralPal;
}

static bool FindStatusInstance_104(
    void* statusComponent,
    uint8_t wantedStatusID,
    void** outStatus,
    float* outDuration,
    float* outTimer)
{
    if (!statusComponent || !outStatus)
        return false;

    *outStatus = nullptr;
    if (outDuration) *outDuration = 0.0f;
    if (outTimer) *outTimer = 0.0f;

    FRawTArray_104 list{};
    __try {
        list = *reinterpret_cast<FRawTArray_104*>(
            reinterpret_cast<uintptr_t>(statusComponent) + ActiveLayout::StatusComponent_ExecutionStatusList);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }

    if (!ValidateRawPointerArray_104(list, 1024, 4096)) {
        if (g_DebugDiagnosticsEnabled_104) {
            InterlockedIncrement(&g_StatusArrayValidationFailures_104);
            MarkRuntimeSnapshotDirty_104();
        }
        ShipLog_104(
            "[ElementalSystemExpanded] WARNING: Status array ABI/layout invalid. "
            "Component=%p Data=%p Num=%d Max=%d\n",
            statusComponent,
            reinterpret_cast<void*>(list.Data),
            list.Num,
            list.Max);
        return false;
    }

    if (g_DebugDiagnosticsEnabled_104)
        InterlockedIncrement(&g_StatusArrayValidationPasses_104);

    const int32_t limit = (list.Num < 64) ? list.Num : 64;
    for (int32_t i = 0; i < limit; ++i) {
        void* status = nullptr;
        __try {
            status = *reinterpret_cast<void**>(
                list.Data + static_cast<uintptr_t>(i) * sizeof(void*));
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }

        if (!status)
            continue;

        const size_t statusReadableSpan =
            static_cast<size_t>((std::max)(
                ActiveLayout::StatusBase_IsEndStatus + sizeof(bool),
                (std::max)(
                    ActiveLayout::StatusBase_StatusID + sizeof(uint8_t),
                    (std::max)(
                        ActiveLayout::StatusBase_Duration + sizeof(float),
                        ActiveLayout::StatusBase_DurationTimer + sizeof(float)))));

        if (!ValidateUObjectLikePointer_104(
            status,
            statusReadableSpan)) {
            continue;
        }

        bool isEndStatus = false;
        uint8_t statusID = 0;
        float duration = 0.0f;
        float timer = 0.0f;

        __try {
            const uintptr_t addr = reinterpret_cast<uintptr_t>(status);
            isEndStatus = *reinterpret_cast<bool*>(addr + ActiveLayout::StatusBase_IsEndStatus);
            statusID = *reinterpret_cast<uint8_t*>(addr + ActiveLayout::StatusBase_StatusID);
            duration = *reinterpret_cast<float*>(addr + ActiveLayout::StatusBase_Duration);
            timer = *reinterpret_cast<float*>(addr + ActiveLayout::StatusBase_DurationTimer);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            continue;
        }

        if (isEndStatus ||
            !std::isfinite(duration) ||
            !std::isfinite(timer) ||
            duration <= kErosionExpiryTick_104 ||
            timer <= kErosionExpiryTick_104) {
            continue;
        }

        if (statusID == wantedStatusID) {
            *outStatus = status;
            if (outDuration) *outDuration = duration;
            if (outTimer) *outTimer = timer;
            return true;
        }
    }

    return false;
}

struct FCurrentElementalStatus_104 {
    void* Instance = nullptr;
    uint8_t StatusID = 0;
    float Duration = 0.0f;
    float Timer = 0.0f;
};

struct FElementalStatusProvenance_104 {
    void* Instance = nullptr;
    uint8_t StatusID = 0;
    uint8_t SourceElement = 0;
};

static std::unordered_map<void*, FElementalStatusProvenance_104>
g_ElementalStatusProvenance_104;
static SRWLOCK g_ElementalStatusProvenanceLock_104 = SRWLOCK_INIT;

static void RememberElementalStatusProvenance_104(
    void* statusComponent,
    void* status,
    uint8_t statusID,
    uint8_t sourceElement)
{
    if (!statusComponent || !status || !IsTrackedElementalStatusID_104(statusID))
        return;

    if (sourceElement < ActiveIds::Element_Normal ||
        sourceElement > ActiveIds::Element_Dragon) {
        sourceElement = NominalElementForStatus_104(statusID);
    }

    AcquireSRWLockExclusive(&g_ElementalStatusProvenanceLock_104);
    g_ElementalStatusProvenance_104[statusComponent] = {
        status,
        statusID,
        sourceElement
    };
    ReleaseSRWLockExclusive(&g_ElementalStatusProvenanceLock_104);
}

static void ClearElementalStatusProvenance_104(void* statusComponent)
{
    if (!statusComponent)
        return;

    AcquireSRWLockExclusive(&g_ElementalStatusProvenanceLock_104);
    g_ElementalStatusProvenance_104.erase(statusComponent);
    ReleaseSRWLockExclusive(&g_ElementalStatusProvenanceLock_104);
}

static uint8_t ResolveCurrentElementSource_104(
    void* statusComponent,
    const FCurrentElementalStatus_104& current)
{
    FElementalStatusProvenance_104 provenance{};
    bool matched = false;

    AcquireSRWLockShared(&g_ElementalStatusProvenanceLock_104);
    const auto it = g_ElementalStatusProvenance_104.find(statusComponent);
    if (it != g_ElementalStatusProvenance_104.end()) {
        provenance = it->second;
        matched = provenance.Instance == current.Instance &&
            provenance.StatusID == current.StatusID;
    }
    ReleaseSRWLockShared(&g_ElementalStatusProvenanceLock_104);

    if (matched &&
        provenance.SourceElement >= ActiveIds::Element_Normal &&
        provenance.SourceElement <= ActiveIds::Element_Dragon) {
        return provenance.SourceElement;
    }

    return NominalElementForStatus_104(current.StatusID);
}

enum class ECurrentElementalStatusQuery_104 : uint8_t {
    Failed = 0,
    None = 1,
    Found = 2
};

static ECurrentElementalStatusQuery_104 QueryCurrentElementalStatus_104(
    void* statusComponent,
    FCurrentElementalStatus_104* outCurrent)
{
    if (!statusComponent || !outCurrent)
        return ECurrentElementalStatusQuery_104::Failed;

    *outCurrent = {};

    FRawTArray_104 list{};
    __try {
        list = *reinterpret_cast<FRawTArray_104*>(
            reinterpret_cast<uintptr_t>(statusComponent) +
            ActiveLayout::StatusComponent_ExecutionStatusList);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return ECurrentElementalStatusQuery_104::Failed;
    }

    if (!ValidateRawPointerArray_104(list, 1024, 4096)) {
        if (g_DebugDiagnosticsEnabled_104) {
            InterlockedIncrement(&g_StatusArrayValidationFailures_104);
            MarkRuntimeSnapshotDirty_104();
        }
        ShipLog_104(
            "[ElementalSystemExpanded] WARNING: status erosion could not validate "
            "the status array. Component=%p Data=%p Num=%d Max=%d\n",
            statusComponent,
            reinterpret_cast<void*>(list.Data),
            list.Num,
            list.Max);
        return ECurrentElementalStatusQuery_104::Failed;
    }

    if (g_DebugDiagnosticsEnabled_104)
        InterlockedIncrement(&g_StatusArrayValidationPasses_104);

    FCurrentElementalStatus_104 candidates[7]{};
    size_t candidateCount = 0;

    const int32_t limit = (list.Num < 64) ? list.Num : 64;
    for (int32_t i = 0; i < limit; ++i) {
        void* status = nullptr;
        __try {
            status = *reinterpret_cast<void**>(
                list.Data + static_cast<uintptr_t>(i) * sizeof(void*));
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            return ECurrentElementalStatusQuery_104::Failed;
        }

        if (!status)
            continue;

        const size_t statusReadableSpan =
            static_cast<size_t>((std::max)(
                ActiveLayout::StatusBase_IsEndStatus + sizeof(bool),
                (std::max)(
                    ActiveLayout::StatusBase_StatusID + sizeof(uint8_t),
                    (std::max)(
                        ActiveLayout::StatusBase_Duration + sizeof(float),
                        ActiveLayout::StatusBase_DurationTimer + sizeof(float)))));

        if (!ValidateUObjectLikePointer_104(status, statusReadableSpan))
            continue;

        bool isEndStatus = false;
        uint8_t statusID = 0;
        float duration = 0.0f;
        float timer = 0.0f;

        __try {
            const uintptr_t addr = reinterpret_cast<uintptr_t>(status);
            isEndStatus = *reinterpret_cast<const bool*>(
                addr + ActiveLayout::StatusBase_IsEndStatus);
            statusID = *reinterpret_cast<const uint8_t*>(
                addr + ActiveLayout::StatusBase_StatusID);
            duration = *reinterpret_cast<const float*>(
                addr + ActiveLayout::StatusBase_Duration);
            timer = *reinterpret_cast<const float*>(
                addr + ActiveLayout::StatusBase_DurationTimer);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            continue;
        }

        if (isEndStatus ||
            !IsTrackedElementalStatusID_104(statusID) ||
            !std::isfinite(duration) ||
            !std::isfinite(timer) ||
            duration <= kErosionExpiryTick_104 ||
            timer <= kErosionExpiryTick_104) {
            continue;
        }

        if (candidateCount < 7) {
            candidates[candidateCount++] = {
                status,
                statusID,
                duration,
                timer
            };
        }
    }

    if (candidateCount == 0) {
        ClearElementalStatusProvenance_104(statusComponent);
        return ECurrentElementalStatusQuery_104::None;
    }

    // Native gameplay is expected to expose one active tracked elemental
    // status. If another mod/engine edge case leaves several, prefer the exact
    // instance whose source provenance we own; otherwise choose the greatest
    // remaining timer deterministically and emit only a developer trace.
    FElementalStatusProvenance_104 provenance{};
    bool haveProvenance = false;
    AcquireSRWLockShared(&g_ElementalStatusProvenanceLock_104);
    const auto it = g_ElementalStatusProvenance_104.find(statusComponent);
    if (it != g_ElementalStatusProvenance_104.end()) {
        provenance = it->second;
        haveProvenance = true;
    }
    ReleaseSRWLockShared(&g_ElementalStatusProvenanceLock_104);

    size_t selected = 0;
    bool selectedByProvenance = false;
    if (haveProvenance) {
        for (size_t i = 0; i < candidateCount; ++i) {
            if (candidates[i].Instance == provenance.Instance &&
                candidates[i].StatusID == provenance.StatusID) {
                selected = i;
                selectedByProvenance = true;
                break;
            }
        }
    }

    if (!selectedByProvenance && candidateCount > 1) {
        for (size_t i = 1; i < candidateCount; ++i) {
            if (candidates[i].Timer > candidates[selected].Timer)
                selected = i;
        }
    }

    if (candidateCount > 1) {
        ModLog(
            "[ElementalSystemExpanded] STATUS EROSION NOTE: Component=%p has "
            "%llu active tracked statuses; selected StatusID=%u Instance=%p.\n",
            statusComponent,
            static_cast<unsigned long long>(candidateCount),
            static_cast<unsigned>(candidates[selected].StatusID),
            candidates[selected].Instance);
    }

    *outCurrent = candidates[selected];
    return ECurrentElementalStatusQuery_104::Found;
}

static bool SetStatusDuration_104(void* status, float duration)
{
    if (!status)
        return false;

    __try {
        const uintptr_t addr = reinterpret_cast<uintptr_t>(status);
        *reinterpret_cast<float*>(addr + ActiveLayout::StatusBase_Duration) = duration;
        *reinterpret_cast<float*>(addr + ActiveLayout::StatusBase_DurationTimer) = duration;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}


static bool SetStatusRemainingFromErosion_104(
    void* status,
    float semanticRemaining,
    float* outStoredRemaining)
{
    if (outStoredRemaining)
        *outStoredRemaining = 0.0f;
    if (!status || !std::isfinite(semanticRemaining))
        return false;

    const float clampedSemantic = (std::max)(0.0f, semanticRemaining);
    const float storedRemaining =
        clampedSemantic > 0.0f
        ? clampedSemantic
        : kErosionExpiryTick_104;

    if (!SetStatusDuration_104(status, storedRemaining))
        return false;

    if (outStoredRemaining)
        *outStoredRemaining = storedRemaining;
    return true;
}

static bool IsWetStrongReactionPair_104(
    const FCurrentElementalStatus_104& current,
    uint8_t incomingStatusID)
{
    if (current.StatusID != ActiveIds::Status_Wetness)
        return false;

    return incomingStatusID == ActiveIds::Status_Freeze ||
        incomingStatusID == ActiveIds::Status_Electrical;
}


// ---------------------------------------------------------
// Wet-gated strong reaction control
//
// Verified 1.0.4 native implementation RVAs from reflected wrappers:
//   UPalAIActionComponent::SetActionClassParameter -> +0x2BD4830
//   UPalActionComponent::PlayAction                -> +0x2BCF7A0
//   JumpDisableFlag                                -> +0x2BFA430
//   StepDisableFlag                                -> +0x2BFB390
//   MoveDisableFlag                                -> +0x2BFA5D0
//   WalkSpeedMultiplier                            -> +0x2BFB5E0
//   YawRotatorMultiplier                           -> +0x2BFB740
//
// The gate is intentionally synchronous and narrow: it is armed only around
// our own native AddStatus(Freeze/Electrical) call. This avoids ProcessEvent,
// Blueprint VM hooks, UE4SS reflection, global FName lookup, and broad action
// suppression.
// ---------------------------------------------------------

static constexpr double kReactionICDSeconds_104 = 14.0;

enum class EStrongReaction_104 : uint8_t {
    None = 0,
    Freeze = 1,
    Shock = 2
};

static const char* StrongReactionName_104(EStrongReaction_104 reaction)
{
    switch (reaction) {
    case EStrongReaction_104::Freeze: return "Freeze";
    case EStrongReaction_104::Shock:  return "Shock";
    default:                           return "None";
    }
}

static EStrongReaction_104 StatusToStrongReaction_104(uint8_t statusID)
{
    if (statusID == ActiveIds::Status_Freeze)
        return EStrongReaction_104::Freeze;
    if (statusID == 22)
        return EStrongReaction_104::Shock;
    return EStrongReaction_104::None;
}

static bool HasActiveStatus_104(void* statusComponent, uint8_t statusID)
{
    void* status = nullptr;
    return FindStatusInstance_104(
        statusComponent, statusID, &status, nullptr, nullptr);
}

struct FReactionICDKey_104 {
    void* DamageReaction = nullptr;
    uint8_t Reaction = 0;

    bool operator==(const FReactionICDKey_104& other) const
    {
        return DamageReaction == other.DamageReaction &&
            Reaction == other.Reaction;
    }
};

struct FReactionICDKeyHash_104 {
    size_t operator()(const FReactionICDKey_104& key) const noexcept
    {
        const size_t p = std::hash<uintptr_t>{}(
            reinterpret_cast<uintptr_t>(key.DamageReaction));
        const size_t r = std::hash<unsigned>{}(key.Reaction);
        return p ^ (r + static_cast<size_t>(0x9E3779B9u) +
            (p << 6) + (p >> 2));
    }
};

static std::unordered_map<
    FReactionICDKey_104,
    double,
    FReactionICDKeyHash_104> g_ReactionLastSuccess_104;

static SRWLOCK g_ReactionICDLock_104 = SRWLOCK_INIT;

static bool ReactionICDReady_104(
    void* damageReaction,
    EStrongReaction_104 reaction,
    double* outElapsed)
{
    if (outElapsed)
        *outElapsed = -1.0;

    if (!damageReaction || reaction == EStrongReaction_104::None)
        return false;

    const FReactionICDKey_104 key{
        damageReaction,
        static_cast<uint8_t>(reaction)
    };

    const double now = GetMonotonicSeconds_104();

    AcquireSRWLockShared(&g_ReactionICDLock_104);
    const auto it = g_ReactionLastSuccess_104.find(key);
    const bool found = (it != g_ReactionLastSuccess_104.end());
    const double elapsed = found ? (now - it->second) : -1.0;
    ReleaseSRWLockShared(&g_ReactionICDLock_104);

    if (outElapsed)
        *outElapsed = elapsed;

    return !found || elapsed >= kReactionICDSeconds_104;
}

static void ReactionICDRecordSuccess_104(
    void* damageReaction,
    EStrongReaction_104 reaction)
{
    if (!damageReaction || reaction == EStrongReaction_104::None)
        return;

    const FReactionICDKey_104 key{
        damageReaction,
        static_cast<uint8_t>(reaction)
    };

    AcquireSRWLockExclusive(&g_ReactionICDLock_104);
    g_ReactionLastSuccess_104[key] = GetMonotonicSeconds_104();
    ReleaseSRWLockExclusive(&g_ReactionICDLock_104);
}

struct FReactionTLSContext_104 {
    bool Active = false;
    uint8_t StatusID = 0;
    EStrongReaction_104 Reaction = EStrongReaction_104::None;
    void* DamageReaction = nullptr;
    void* StatusComponent = nullptr;
    bool HadWetness = false;
    bool ICDReady = false;
    bool AllowStrongControl = false;
    bool ActionGateSeen = false;
    bool ReactionRecorded = false;
};

static thread_local FReactionTLSContext_104 g_ReactionTLS_104{};

struct FReactionTLSGuard_104 {
    FReactionTLSContext_104 Previous{};

    FReactionTLSGuard_104(
        void* damageReaction,
        void* statusComponent,
        uint8_t statusID)
    {
        Previous = g_ReactionTLS_104;
        g_ReactionTLS_104 = {};

        const EStrongReaction_104 reaction =
            StatusToStrongReaction_104(statusID);

        if (reaction == EStrongReaction_104::None)
            return;

        const bool wet = HasActiveStatus_104(statusComponent, ActiveIds::Status_Wetness);
        double elapsed = -1.0;
        const bool icdReady =
            wet && ReactionICDReady_104(
                damageReaction, reaction, &elapsed);

        g_ReactionTLS_104.Active = true;
        g_ReactionTLS_104.StatusID = statusID;
        g_ReactionTLS_104.Reaction = reaction;
        g_ReactionTLS_104.DamageReaction = damageReaction;
        g_ReactionTLS_104.StatusComponent = statusComponent;
        g_ReactionTLS_104.HadWetness = wet;
        g_ReactionTLS_104.ICDReady = icdReady;
        g_ReactionTLS_104.AllowStrongControl = wet && icdReady;

        ModLog(
            "[ElementalSystemExpanded] REACTION PREP: DamageReaction=%p "
            "StatusComponent=%p StatusID=%u Reaction=%s Wet=%s "
            "ICD=%s Elapsed=%.3f Control=%s\n",
            damageReaction,
            statusComponent,
            static_cast<unsigned>(statusID),
            StrongReactionName_104(reaction),
            wet ? "YES" : "NO",
            wet ? (icdReady ? "READY" : "ACTIVE") : "N/A",
            elapsed,
            g_ReactionTLS_104.AllowStrongControl ? "ALLOW" : "BLOCK");
    }

    ~FReactionTLSGuard_104()
    {
        g_ReactionTLS_104 = Previous;
    }
};

static void RecordAllowedReactionIfNeeded_104(const char* gateName)
{
    if (!g_ReactionTLS_104.Active ||
        !g_ReactionTLS_104.AllowStrongControl ||
        g_ReactionTLS_104.ReactionRecorded) {
        return;
    }

    ReactionICDRecordSuccess_104(
        g_ReactionTLS_104.DamageReaction,
        g_ReactionTLS_104.Reaction);

    g_ReactionTLS_104.ReactionRecorded = true;

    ModLog(
        "[ElementalSystemExpanded] REACTION TRIGGER: DamageReaction=%p "
        "Reaction=%s Gate=%s ICD=%.1fs\n",
        g_ReactionTLS_104.DamageReaction,
        StrongReactionName_104(g_ReactionTLS_104.Reaction),
        gateName ? gateName : "Unknown",
        kReactionICDSeconds_104);
}

// Native reaction/action signatures inferred directly from the verified
// wrapper register setup in memoryViewDump7/8.
using NativeSetActionClassParameter_104_t =
void* (__fastcall*)(
    void* AIActionComponent,
    void* NewActionClass,
    const void* DynamicParameter);

using NativePlayAction_104_t =
void* (__fastcall*)(
    void* ActionComponent,
    void* ActionTarget,
    void* ActionClass);

using NativeSetNamedBool_104_t =
void(__fastcall*)(
    void* MovementComponent,
    uint64_t RawFName,
    bool Enabled);

using NativeSetNamedFloat_104_t =
void(__fastcall*)(
    void* MovementComponent,
    uint64_t RawFName,
    float Value);

// PalShooterComponent layered disable functions, verified from
// memoryViewDump10. Windows x64 register layout:
//   RCX = ShooterComponent
//   DL  = Layer/Priority byte
//   R8  = FName (8-byte value)
//   R9B = Disabled
using NativeSetShooterLayeredBool_104_t =
void(__fastcall*)(
    void* ShooterComponent,
    uint8_t Layer,
    uint64_t RawFName,
    bool Disabled);

// Character::StopAnimMontage wrapper dispatches:
//   RCX = Character
//   RDX = UAnimMontage*
//   call [vtable + 0x888]
using NativeSetComponentTickEnabledVirtual_104_t =
void(__fastcall*)(
    void* ActorComponent,
    bool Enabled);

using NativeStopAnimMontageVirtual_104_t =
void(__fastcall*)(
    void* Character,
    void* Montage);

// PalStatusBase::TickStatus virtual ABI from memoryViewDump9:
//   RCX = Status
//   XMM1 = DeltaTime
using NativeStatusTickVirtual_104_t =
void(__fastcall*)(
    void* Status,
    float DeltaTime);

// Native AddVisualEffect / AddVisualEffect_Local ABI derived from their
// reflected wrappers:
//   RCX = UPalVisualEffectComponent*
//   EDX = EPalVisualEffectID
//   R8  = const FPalVisualEffectDynamicParameter*
//   RAX = UPalVisualEffectBase*
using NativeAddVisualEffect_104_t =
void* (__fastcall*)(
    void* VisualEffectComponent,
    uint8_t VisualEffectID,
    const void* DynamicParameter);

using NativeRemoveVisualEffectLocal_104_t =
void(__fastcall*)(
    void* VisualEffectComponent,
    uint8_t VisualEffectID);

static NativeSetActionClassParameter_104_t
Original_SetActionClassParameter_104 = nullptr;
static NativePlayAction_104_t
Original_PlayAction_104 = nullptr;
static NativeSetNamedBool_104_t
Original_SetJumpDisableFlag_104 = nullptr;
static NativeSetNamedBool_104_t
Original_SetStepDisableFlag_104 = nullptr;
static NativeSetNamedBool_104_t
Original_SetMoveDisableFlag_104 = nullptr;
static NativeSetNamedFloat_104_t
Original_SetWalkSpeedMultiplier_104 = nullptr;
static NativeSetNamedFloat_104_t
Original_SetYawRotatorMultiplier_104 = nullptr;

static NativeSetShooterLayeredBool_104_t
Original_SetDisableAimFlag_Layered_104 = nullptr;
static NativeSetShooterLayeredBool_104_t
Original_SetDisableShootFlag_Layered_104 = nullptr;
static NativeSetShooterLayeredBool_104_t
Original_SetDisableChangeWeaponFlag_Layered_104 = nullptr;

// Dynamically resolved exact virtuals for the current validated runtime.
static NativeSetComponentTickEnabledVirtual_104_t
Original_SetComponentTickEnabledVirtual_104 = nullptr;
static NativeStopAnimMontageVirtual_104_t
Original_StopAnimMontageVirtual_104 = nullptr;
static NativeStatusTickVirtual_104_t
Original_FreezeTickVirtual_104 = nullptr;

static NativeAddVisualEffect_104_t
Original_AddVisualEffect_104 = nullptr;
static NativeAddVisualEffect_104_t
Original_AddVisualEffect_Local_104 = nullptr;
static NativeRemoveVisualEffectLocal_104_t
Native_RemoveVisualEffect_Local_104 = nullptr;

static void* g_SetComponentTickEnabledVirtualTarget_104 = nullptr;
static void* g_StopAnimMontageVirtualTarget_104 = nullptr;
static void* g_FreezeTickVirtualTarget_104 = nullptr;

enum class EBlindnessSource_104 : uint8_t {
    Unknown = 0,
    Dark = 1,
    Light = 2
};

static thread_local EBlindnessSource_104 g_BlindnessSourceTLS_104 =
EBlindnessSource_104::Unknown;

static const char* BlindnessSourceName_104(EBlindnessSource_104 source)
{
    switch (source) {
    case EBlindnessSource_104::Dark: return "DARK";
    case EBlindnessSource_104::Light: return "LIGHT";
    default: return "UNKNOWN";
    }
}

struct FBlindnessSourceScope_104 {
    EBlindnessSource_104 Previous = EBlindnessSource_104::Unknown;

    explicit FBlindnessSourceScope_104(EBlindnessSource_104 source)
        : Previous(g_BlindnessSourceTLS_104)
    {
        g_BlindnessSourceTLS_104 = source;
    }

    ~FBlindnessSourceScope_104()
    {
        g_BlindnessSourceTLS_104 = Previous;
    }
};

static SRWLOCK g_FreezeDynamicHookLock_104 = SRWLOCK_INIT;

static bool IsBlockedReactionScope_104()
{
    return g_ReactionTLS_104.Active &&
        !g_ReactionTLS_104.AllowStrongControl &&
        (g_ReactionTLS_104.StatusID == ActiveIds::Status_Freeze ||
            g_ReactionTLS_104.StatusID == ActiveIds::Status_Electrical);
}

static void* __fastcall Detour_SetActionClassParameter_104(
    void* aiActionComponent,
    void* newActionClass,
    const void* dynamicParameter)
{
    if (g_ReactionTLS_104.Active) {
        g_ReactionTLS_104.ActionGateSeen = true;

        if (!g_ReactionTLS_104.AllowStrongControl) {
            ModLog(
                "[ElementalSystemExpanded] REACTION BLOCK: %s "
                "SetActionClassParameter Component=%p Class=%p\n",
                StrongReactionName_104(g_ReactionTLS_104.Reaction),
                aiActionComponent,
                newActionClass);
            return nullptr;
        }

        RecordAllowedReactionIfNeeded_104("SetActionClassParameter");
    }

    return Original_SetActionClassParameter_104
        ? Original_SetActionClassParameter_104(
            aiActionComponent, newActionClass, dynamicParameter)
        : nullptr;
}

static void* __fastcall Detour_PlayAction_104(
    void* actionComponent,
    void* actionTarget,
    void* actionClass)
{
    if (g_ReactionTLS_104.Active) {
        if (!g_ReactionTLS_104.AllowStrongControl) {
            ModLog(
                "[ElementalSystemExpanded] REACTION BLOCK: %s "
                "PlayAction Component=%p Target=%p Class=%p\n",
                StrongReactionName_104(g_ReactionTLS_104.Reaction),
                actionComponent,
                actionTarget,
                actionClass);
            return nullptr;
        }

        // Fallback in case a reaction path reaches PlayAction without first
        // traversing SetActionClassParameter.
        RecordAllowedReactionIfNeeded_104("PlayAction");
    }

    return Original_PlayAction_104
        ? Original_PlayAction_104(
            actionComponent, actionTarget, actionClass)
        : nullptr;
}

static void __fastcall Detour_SetJumpDisableFlag_104(
    void* movementComponent,
    uint64_t rawFName,
    bool disabled)
{
    if (IsBlockedReactionScope_104() && disabled) {
        ModLog(
            "[ElementalSystemExpanded] REACTION BLOCK: %s "
            "SetJumpDisableFlag(true) Component=%p\n",
            StrongReactionName_104(g_ReactionTLS_104.Reaction),
            movementComponent);
        return;
    }

    if (Original_SetJumpDisableFlag_104)
        Original_SetJumpDisableFlag_104(
            movementComponent, rawFName, disabled);
}

static void __fastcall Detour_SetStepDisableFlag_104(
    void* movementComponent,
    uint64_t rawFName,
    bool disabled)
{
    if (IsBlockedReactionScope_104() && disabled) {
        ModLog(
            "[ElementalSystemExpanded] REACTION BLOCK: %s "
            "SetStepDisableFlag(true) Component=%p\n",
            StrongReactionName_104(g_ReactionTLS_104.Reaction),
            movementComponent);
        return;
    }

    if (Original_SetStepDisableFlag_104)
        Original_SetStepDisableFlag_104(
            movementComponent, rawFName, disabled);
}

static void __fastcall Detour_SetMoveDisableFlag_104(
    void* movementComponent,
    uint64_t rawFName,
    bool disabled)
{
    if (IsBlockedReactionScope_104() && disabled) {
        ModLog(
            "[ElementalSystemExpanded] REACTION BLOCK: %s "
            "SetMoveDisableFlag(true) Component=%p\n",
            StrongReactionName_104(g_ReactionTLS_104.Reaction),
            movementComponent);
        return;
    }

    if (Original_SetMoveDisableFlag_104)
        Original_SetMoveDisableFlag_104(
            movementComponent, rawFName, disabled);
}

static bool ShouldBlockWalkMultiplier_104(float value)
{
    if (!IsBlockedReactionScope_104())
        return false;

    if (g_ReactionTLS_104.StatusID == ActiveIds::Status_Freeze) {
        // Freeze SetFlag true branch: 0.6.
        return value >= 0.55f && value <= 0.65f;
    }

    if (g_ReactionTLS_104.StatusID == ActiveIds::Status_Electrical) {
        // Electrical reaction runtime observation: 0.0.
        return value >= -0.01f && value <= 0.01f;
    }

    return false;
}

static bool ShouldBlockYawMultiplier_104(float value)
{
    if (!IsBlockedReactionScope_104())
        return false;

    // Both strong control reactions use a zero yaw multiplier.
    return value >= -0.01f && value <= 0.01f;
}

static void __fastcall Detour_SetWalkSpeedMultiplier_104(
    void* movementComponent,
    uint64_t rawFName,
    float value)
{
    if (ShouldBlockWalkMultiplier_104(value)) {
        ModLog(
            "[ElementalSystemExpanded] REACTION BLOCK: %s "
            "SetWalkSpeedMultiplier(%.3f) Component=%p\n",
            StrongReactionName_104(g_ReactionTLS_104.Reaction),
            value,
            movementComponent);
        return;
    }

    if (Original_SetWalkSpeedMultiplier_104)
        Original_SetWalkSpeedMultiplier_104(
            movementComponent, rawFName, value);
}

static void __fastcall Detour_SetYawRotatorMultiplier_104(
    void* movementComponent,
    uint64_t rawFName,
    float value)
{
    if (ShouldBlockYawMultiplier_104(value)) {
        ModLog(
            "[ElementalSystemExpanded] REACTION BLOCK: %s "
            "SetYawRotatorMultiplier(%.3f) Component=%p\n",
            StrongReactionName_104(g_ReactionTLS_104.Reaction),
            value,
            movementComponent);
        return;
    }

    if (Original_SetYawRotatorMultiplier_104)
        Original_SetYawRotatorMultiplier_104(
            movementComponent, rawFName, value);
}


static bool ShouldBlockFreezeShooterDisable_104(bool disabled)
{
    return disabled &&
        IsBlockedReactionScope_104() &&
        g_ReactionTLS_104.StatusID == ActiveIds::Status_Freeze;
}

static void __fastcall Detour_SetDisableAimFlag_Layered_104(
    void* shooterComponent,
    uint8_t layer,
    uint64_t rawFName,
    bool disabled)
{
    if (ShouldBlockFreezeShooterDisable_104(disabled)) {
        ModLog(
            "[ElementalSystemExpanded] REACTION BLOCK: Freeze "
            "SetDisableAimFlag_Layered(true) Component=%p Layer=%u\n",
            shooterComponent,
            static_cast<unsigned>(layer));
        return;
    }

    if (Original_SetDisableAimFlag_Layered_104)
        Original_SetDisableAimFlag_Layered_104(
            shooterComponent, layer, rawFName, disabled);
}

static void __fastcall Detour_SetDisableShootFlag_Layered_104(
    void* shooterComponent,
    uint8_t layer,
    uint64_t rawFName,
    bool disabled)
{
    if (ShouldBlockFreezeShooterDisable_104(disabled)) {
        ModLog(
            "[ElementalSystemExpanded] REACTION BLOCK: Freeze "
            "SetDisableShootFlag_Layered(true) Component=%p Layer=%u\n",
            shooterComponent,
            static_cast<unsigned>(layer));
        return;
    }

    if (Original_SetDisableShootFlag_Layered_104)
        Original_SetDisableShootFlag_Layered_104(
            shooterComponent, layer, rawFName, disabled);
}

static void __fastcall Detour_SetDisableChangeWeaponFlag_Layered_104(
    void* shooterComponent,
    uint8_t layer,
    uint64_t rawFName,
    bool disabled)
{
    if (ShouldBlockFreezeShooterDisable_104(disabled)) {
        ModLog(
            "[ElementalSystemExpanded] REACTION BLOCK: Freeze "
            "SetDisableChangeWeaponFlag_Layered(true) Component=%p Layer=%u\n",
            shooterComponent,
            static_cast<unsigned>(layer));
        return;
    }

    if (Original_SetDisableChangeWeaponFlag_Layered_104)
        Original_SetDisableChangeWeaponFlag_Layered_104(
            shooterComponent, layer, rawFName, disabled);
}


struct FHookTransactionSpec_104 {
    const char* Name = nullptr;
    uintptr_t Target = 0;
    void* Detour = nullptr;
    void** Original = nullptr;
};

static void RollbackCreatedHooks_104(
    const FHookTransactionSpec_104* specs,
    size_t createdCount)
{
    if (!specs)
        return;
    for (size_t i = 0; i < createdCount; ++i) {
        if (specs[i].Target)
            MH_RemoveHook(reinterpret_cast<LPVOID>(specs[i].Target));
        if (specs[i].Original)
            *specs[i].Original = nullptr;
    }
}

static bool InstallHookTransaction_104(
    const char* capabilityName,
    const FHookTransactionSpec_104* specs,
    size_t count)
{
    if (!specs || !count)
        return false;

    for (size_t i = 0; i < count; ++i) {
        if (!specs[i].Target || !IsExecutableAddress_104(specs[i].Target)) {
            ShipLog_104("[ElementalSystemExpanded] ERROR: transaction %s has invalid target for %s.\n",
                capabilityName, specs[i].Name ? specs[i].Name : "unnamed");
            return false;
        }
    }

    size_t created = 0;
    for (; created < count; ++created) {
        const MH_STATUS status = MH_CreateHook(
            reinterpret_cast<LPVOID>(specs[created].Target),
            specs[created].Detour,
            specs[created].Original);
        if (status != MH_OK) {
            ShipLog_104("[ElementalSystemExpanded] ERROR: transaction %s create failed for %s (%d). Rolling back.\n",
                capabilityName,
                specs[created].Name ? specs[created].Name : "unnamed",
                static_cast<int>(status));
            RollbackCreatedHooks_104(specs, created);
            return false;
        }
    }

    size_t queued = 0;
    for (; queued < count; ++queued) {
        const MH_STATUS status = MH_QueueEnableHook(
            reinterpret_cast<LPVOID>(specs[queued].Target));
        if (status != MH_OK) {
            ShipLog_104("[ElementalSystemExpanded] ERROR: transaction %s queue-enable failed for %s (%d). Rolling back.\n",
                capabilityName,
                specs[queued].Name ? specs[queued].Name : "unnamed",
                static_cast<int>(status));
            for (size_t j = 0; j < queued; ++j)
                MH_QueueDisableHook(reinterpret_cast<LPVOID>(specs[j].Target));
            MH_ApplyQueued();
            RollbackCreatedHooks_104(specs, count);
            return false;
        }
    }

    const MH_STATUS apply = MH_ApplyQueued();
    if (apply != MH_OK) {
        ShipLog_104("[ElementalSystemExpanded] ERROR: transaction %s apply failed (%d). Rolling back.\n",
            capabilityName, static_cast<int>(apply));
        for (size_t i = 0; i < count; ++i)
            MH_QueueDisableHook(reinterpret_cast<LPVOID>(specs[i].Target));
        MH_ApplyQueued();
        RollbackCreatedHooks_104(specs, count);
        return false;
    }

    for (size_t i = 0; i < count; ++i) {
        ModLog("[ElementalSystemExpanded] SUCCESS: Transaction %s hooked %s at RVA +0x%llX.\n",
            capabilityName,
            specs[i].Name ? specs[i].Name : "unnamed",
            static_cast<unsigned long long>(specs[i].Target - g_ModuleBase_104));
    }
    return true;
}


struct FDynamicHookTransactionSpec_104 {
    const char* Name = nullptr;
    void* Target = nullptr;
    void* Detour = nullptr;
    void** Original = nullptr;
    void** StoredTarget = nullptr;
    uintptr_t Slot = 0;
};

static bool EnsureDynamicHookTransaction_104(
    const char* capabilityName,
    FDynamicHookTransactionSpec_104* specs,
    size_t count)
{
    if (!specs || !count)
        return false;

    std::vector<size_t> newIndexes;
    newIndexes.reserve(count);

    for (size_t i = 0; i < count; ++i) {
        if (!specs[i].StoredTarget || !specs[i].Original || !specs[i].Target)
            return false;

        if (*specs[i].StoredTarget) {
            if (*specs[i].StoredTarget != specs[i].Target) {
                ShipLog_104(
                    "[ElementalSystemExpanded] ERROR: dynamic transaction %s target changed for %s Existing=%p New=%p\n",
                    capabilityName,
                    specs[i].Name ? specs[i].Name : "unnamed",
                    *specs[i].StoredTarget,
                    specs[i].Target);
                return false;
            }
            continue;
        }

        if (!IsExecutableAddress_104(reinterpret_cast<uintptr_t>(specs[i].Target))) {
            ShipLog_104(
                "[ElementalSystemExpanded] ERROR: dynamic transaction %s invalid target for %s: %p\n",
                capabilityName,
                specs[i].Name ? specs[i].Name : "unnamed",
                specs[i].Target);
            return false;
        }
        newIndexes.push_back(i);
    }

    if (newIndexes.empty())
        return true;

    size_t createdCount = 0;
    for (; createdCount < newIndexes.size(); ++createdCount) {
        const size_t i = newIndexes[createdCount];
        const MH_STATUS status = MH_CreateHook(
            specs[i].Target,
            specs[i].Detour,
            specs[i].Original);
        if (status != MH_OK) {
            ShipLog_104(
                "[ElementalSystemExpanded] ERROR: dynamic transaction %s create failed for %s (%d). Rolling back.\n",
                capabilityName,
                specs[i].Name ? specs[i].Name : "unnamed",
                static_cast<int>(status));
            for (size_t j = 0; j < createdCount; ++j) {
                const size_t k = newIndexes[j];
                MH_RemoveHook(specs[k].Target);
                *specs[k].Original = nullptr;
            }
            return false;
        }
    }

    size_t queuedCount = 0;
    for (; queuedCount < newIndexes.size(); ++queuedCount) {
        const size_t i = newIndexes[queuedCount];
        const MH_STATUS status = MH_QueueEnableHook(specs[i].Target);
        if (status != MH_OK) {
            for (size_t j = 0; j < queuedCount; ++j)
                MH_QueueDisableHook(specs[newIndexes[j]].Target);
            MH_ApplyQueued();
            for (size_t j = 0; j < newIndexes.size(); ++j) {
                const size_t k = newIndexes[j];
                MH_RemoveHook(specs[k].Target);
                *specs[k].Original = nullptr;
            }
            ShipLog_104(
                "[ElementalSystemExpanded] ERROR: dynamic transaction %s queue-enable failed for %s (%d). Rolled back.\n",
                capabilityName,
                specs[i].Name ? specs[i].Name : "unnamed",
                static_cast<int>(status));
            return false;
        }
    }

    const MH_STATUS apply = MH_ApplyQueued();
    if (apply != MH_OK) {
        for (size_t j = 0; j < newIndexes.size(); ++j)
            MH_QueueDisableHook(specs[newIndexes[j]].Target);
        MH_ApplyQueued();
        for (size_t j = 0; j < newIndexes.size(); ++j) {
            const size_t k = newIndexes[j];
            MH_RemoveHook(specs[k].Target);
            *specs[k].Original = nullptr;
        }
        ShipLog_104(
            "[ElementalSystemExpanded] ERROR: dynamic transaction %s apply failed (%d). Rolled back.\n",
            capabilityName,
            static_cast<int>(apply));
        return false;
    }

    for (size_t j = 0; j < newIndexes.size(); ++j) {
        const size_t i = newIndexes[j];
        *specs[i].StoredTarget = specs[i].Target;
        ModLog(
            "[ElementalSystemExpanded] SUCCESS: Dynamic transaction %s hooked %s at %p (slot +0x%llX).\n",
            capabilityName,
            specs[i].Name ? specs[i].Name : "unnamed",
            specs[i].Target,
            static_cast<unsigned long long>(specs[i].Slot));
    }

    return true;
}

static bool InstallNativeReactionHooks_104(uintptr_t base)
{
    // Wrapper-tail signatures. The rel32 itself is wildcarded; after locating
    // the wrapper tail we decode the CALL to recover the native target.
    static const uint8_t kSetActionTail[] = {
        0x0F,0x28,0x45,0xC7, 0x0F,0x29,0x4D,0x07,
        0x0F,0x28,0x4D,0xD7, 0x0F,0x29,0x45,0x17,
        0x0F,0x28,0x45,0xE7, 0x48,0x89,0x7B,0x20,
        0x0F,0x29,0x4D,0x27, 0x0F,0x29,0x45,0x37,
        0xE8,0x00,0x00,0x00,0x00
    };
    static const char kSetActionMask[] =
        "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx????";

    static const uint8_t kPlayActionTail[] = {
        0x48,0x8B,0x43,0x20, 0x48,0x8B,0xCD,
        0x4C,0x8B,0x44,0x24,0x48,
        0x48,0x85,0xC0,
        0x48,0x8B,0x54,0x24,0x50,
        0x40,0x0F,0x95,0xC7,
        0x48,0x03,0xF8,
        0x48,0x89,0x7B,0x20,
        0xE8,0x00,0x00,0x00,0x00
    };
    static const char kPlayActionMask[] =
        "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx????";

    static const uint8_t kNamedBoolTail[] = {
        0x48,0x8B,0xCE,
        0x48,0x8B,0x54,0x24,0x48,
        0x48,0x85,0xC0,
        0x40,0x0F,0x95,0xC7,
        0x48,0x03,0xF8,
        0x83,0x7C,0x24,0x38,0x00,
        0x48,0x89,0x7B,0x20,
        0x41,0x0F,0x95,0xC0,
        0xE8,0x00,0x00,0x00,0x00
    };
    static const char kNamedBoolMask[] =
        "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx????";

    static const uint8_t kNamedFloatTail[] = {
        0x48,0x8B,0x43,0x20,
        0x48,0x8B,0xCE,
        0xF3,0x0F,0x10,0x54,0x24,0x38,
        0x48,0x85,0xC0,
        0x48,0x8B,0x54,0x24,0x48,
        0x40,0x0F,0x95,0xC7,
        0x48,0x03,0xF8,
        0x48,0x89,0x7B,0x20,
        0xE8,0x00,0x00,0x00,0x00
    };
    static const char kNamedFloatMask[] =
        "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx????";

    static const uint8_t kShooterTail[] = {
        0x4C,0x8B,0x44,0x24,0x20,
        0x48,0x85,0xC0,
        0x0F,0xB6,0x54,0x24,0x48,
        0x40,0x0F,0x95,0xC7,
        0x48,0x03,0xF8,
        0x83,0x7C,0x24,0x58,0x00,
        0x48,0x89,0x7B,0x20,
        0x41,0x0F,0x95,0xC1,
        0xE8,0x00,0x00,0x00,0x00
    };
    static const char kShooterMask[] =
        "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx????";

    struct FHookSpec {
        const char* Name;
        uintptr_t KnownCallRva;
        uintptr_t KnownNativeRva;
        const uint8_t* Pattern;
        const char* Mask;
        size_t CallOffset;
        void* Detour;
        void** Original;
    };

    const FHookSpec hooks[] = {
        {"PalAIActionComponent::SetActionClassParameter",
            Known104::Rva::SetActionClassParameter_Call,
            Known104::Rva::SetActionClassParameter_Native,
            kSetActionTail,kSetActionMask,32,
            reinterpret_cast<void*>(&Detour_SetActionClassParameter_104),
            reinterpret_cast<void**>(&Original_SetActionClassParameter_104)},
        {"PalActionComponent::PlayAction",
            Known104::Rva::PlayAction_Call,
            Known104::Rva::PlayAction_Native,
            kPlayActionTail,kPlayActionMask,31,
            reinterpret_cast<void*>(&Detour_PlayAction_104),
            reinterpret_cast<void**>(&Original_PlayAction_104)},
        {"PalCharacterMovementComponent::SetJumpDisableFlag",
            Known104::Rva::SetJumpDisableFlag_Call,
            Known104::Rva::SetJumpDisableFlag_Native,
            kNamedBoolTail,kNamedBoolMask,31,
            reinterpret_cast<void*>(&Detour_SetJumpDisableFlag_104),
            reinterpret_cast<void**>(&Original_SetJumpDisableFlag_104)},
        {"PalCharacterMovementComponent::SetStepDisableFlag",
            Known104::Rva::SetStepDisableFlag_Call,
            Known104::Rva::SetStepDisableFlag_Native,
            kNamedBoolTail,kNamedBoolMask,31,
            reinterpret_cast<void*>(&Detour_SetStepDisableFlag_104),
            reinterpret_cast<void**>(&Original_SetStepDisableFlag_104)},
        {"PalCharacterMovementComponent::SetMoveDisableFlag",
            Known104::Rva::SetMoveDisableFlag_Call,
            Known104::Rva::SetMoveDisableFlag_Native,
            kNamedBoolTail,kNamedBoolMask,31,
            reinterpret_cast<void*>(&Detour_SetMoveDisableFlag_104),
            reinterpret_cast<void**>(&Original_SetMoveDisableFlag_104)},
        {"PalCharacterMovementComponent::SetWalkSpeedMultiplier",
            Known104::Rva::SetWalkSpeedMultiplier_Call,
            Known104::Rva::SetWalkSpeedMultiplier_Native,
            kNamedFloatTail,kNamedFloatMask,32,
            reinterpret_cast<void*>(&Detour_SetWalkSpeedMultiplier_104),
            reinterpret_cast<void**>(&Original_SetWalkSpeedMultiplier_104)},
        {"PalCharacterMovementComponent::SetYawRotatorMultiplier",
            Known104::Rva::SetYawRotatorMultiplier_Call,
            Known104::Rva::SetYawRotatorMultiplier_Native,
            kNamedFloatTail,kNamedFloatMask,32,
            reinterpret_cast<void*>(&Detour_SetYawRotatorMultiplier_104),
            reinterpret_cast<void**>(&Original_SetYawRotatorMultiplier_104)},
        {"PalShooterComponent::SetDisableAimFlag_Layered",
            Known104::Rva::SetDisableAimFlag_Call,
            Known104::Rva::SetDisableAimFlag_Native,
            kShooterTail,kShooterMask,33,
            reinterpret_cast<void*>(&Detour_SetDisableAimFlag_Layered_104),
            reinterpret_cast<void**>(&Original_SetDisableAimFlag_Layered_104)},
        {"PalShooterComponent::SetDisableShootFlag_Layered",
            Known104::Rva::SetDisableShootFlag_Call,
            Known104::Rva::SetDisableShootFlag_Native,
            kShooterTail,kShooterMask,33,
            reinterpret_cast<void*>(&Detour_SetDisableShootFlag_Layered_104),
            reinterpret_cast<void**>(&Original_SetDisableShootFlag_Layered_104)},
        {"PalShooterComponent::SetDisableChangeWeaponFlag_Layered",
            Known104::Rva::SetDisableChangeWeaponFlag_Call,
            Known104::Rva::SetDisableChangeWeaponFlag_Native,
            kShooterTail,kShooterMask,33,
            reinterpret_cast<void*>(&Detour_SetDisableChangeWeaponFlag_Layered_104),
            reinterpret_cast<void**>(&Original_SetDisableChangeWeaponFlag_Layered_104)}
    };

    // Resolve every dependency first. Do not install a partial reaction layer.
    uintptr_t targets[sizeof(hooks) / sizeof(hooks[0])]{};
    for (size_t i = 0; i < sizeof(hooks) / sizeof(hooks[0]); ++i) {
        targets[i] = ResolveNativeFromWrapperCall_104(
            hooks[i].Name,
            base,
            hooks[i].KnownCallRva,
            hooks[i].KnownNativeRva,
            hooks[i].Pattern,
            hooks[i].Mask,
            hooks[i].CallOffset);
        if (!targets[i])
            return false;
    }

    FHookTransactionSpec_104 transaction[sizeof(hooks) / sizeof(hooks[0])]{};
    for (size_t i = 0; i < sizeof(hooks) / sizeof(hooks[0]); ++i) {
        transaction[i] = FHookTransactionSpec_104{
            hooks[i].Name,
            targets[i],
            hooks[i].Detour,
            hooks[i].Original
        };
    }

    return InstallHookTransaction_104(
        "WetGatedStrongReactions",
        transaction,
        sizeof(transaction) / sizeof(transaction[0]));
}

// ---------------------------------------------------------
// Durable reflected-wrapper virtual-slot extraction
// ---------------------------------------------------------
static bool ResolveDurableVirtualSlots_104(uintptr_t base)
{
    static const uint8_t kComponentTickTail[] = {
        0x48,0x8B,0x43,0x20,
        0x48,0x8B,0xCE,
        0x48,0x85,0xC0,
        0x40,0x0F,0x95,0xC7,
        0x48,0x03,0xF8,
        0x83,0x7C,0x24,0x30,0x00,
        0x48,0x89,0x7B,0x20,
        0x48,0x8B,0x06,
        0x0F,0x95,0xC2,
        0xFF,0x90,0x00,0x00,0x00,0x00
    };
    static const char kComponentTickMask[] =
        "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx????";

    static const uint8_t kStopMontageTail[] = {
        0x48,0x8B,0x43,0x20,
        0x48,0x8B,0xCE,
        0x48,0x8B,0x54,0x24,0x30,
        0x48,0x85,0xC0,
        0x40,0x0F,0x95,0xC7,
        0x48,0x03,0xF8,
        0x48,0x89,0x7B,0x20,
        0x48,0x8B,0x06,
        0xFF,0x90,0x00,0x00,0x00,0x00
    };
    static const char kStopMontageMask[] =
        "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx????";

    static const uint8_t kStatusTickTail[] = {
        0x48,0x8B,0x43,0x20,
        0x33,0xC9,
        0xF3,0x0F,0x10,0x4C,0x24,0x30,
        0x48,0x85,0xC0,
        0x0F,0x95,0xC1,
        0x48,0x03,0xC8,
        0x48,0x89,0x4B,0x20,
        0x48,0x8B,0xCF,
        0x48,0x8B,0x07,
        0xFF,0x90,0x00,0x00,0x00,0x00
    };
    static const char kStatusTickMask[] =
        "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx????";

    bool ok = true;
    ok = ok && ResolveVirtualSlot_104(
        "VSlot.ActorComponent::SetComponentTickEnabled",
        base,
        Known104::Rva::SetComponentTickEnabled_Dispatch,
        Known104::VSlot::SetComponentTickEnabled,
        kComponentTickTail,
        kComponentTickMask,
        34,
        &g_VSlot_SetComponentTickEnabled_104);

    ok = ok && ResolveVirtualSlot_104(
        "VSlot.Character::StopAnimMontage",
        base,
        Known104::Rva::StopAnimMontage_Dispatch,
        Known104::VSlot::StopAnimMontage,
        kStopMontageTail,
        kStopMontageMask,
        31,
        &g_VSlot_StopAnimMontage_104);

    ok = ok && ResolveVirtualSlot_104(
        "VSlot.PalStatusBase::TickStatus",
        base,
        Known104::Rva::StatusTick_Dispatch,
        Known104::VSlot::StatusTick,
        kStatusTickTail,
        kStatusTickMask,
        33,
        &g_VSlot_StatusTick_104);

    return ok;
}

// ---------------------------------------------------------
// Blocked Freeze persistent runtime state
//
// The synchronous TLS gate ends after AddStatus. Freeze's StartLocation pin
// occurs later from BP_Status_Freeze::TickStatus, so remember only those exact
// Freeze status instances for which strong control was denied.
// ---------------------------------------------------------

struct FBlockedFreezeRuntime_104 {
    void* Character = nullptr;
    void* RootComponent = nullptr;
    bool TickObservedLogged = false;
    // 0 = not validated yet, 1 = the dynamically located StartLocation field
    // has been validated for this instance, -1 = validation failed.
    int8_t StartLocationState = 0;
    bool StartLocationRefreshLogged = false;
};

static std::unordered_map<void*, FBlockedFreezeRuntime_104>
g_BlockedFreezeRuntime_104;
static SRWLOCK g_BlockedFreezeRuntimeLock_104 = SRWLOCK_INIT;

struct FFreezeTickTLS_104 {
    bool Active = false;
    void* Status = nullptr;
};

static thread_local FFreezeTickTLS_104 g_FreezeTickTLS_104{};

static void RemoveBlockedFreezeRuntime_104(void* status)
{
    if (!status)
        return;

    AcquireSRWLockExclusive(&g_BlockedFreezeRuntimeLock_104);
    g_BlockedFreezeRuntime_104.erase(status);
    ReleaseSRWLockExclusive(&g_BlockedFreezeRuntimeLock_104);
}

static bool GetBlockedFreezeRuntime_104(
    void* status,
    FBlockedFreezeRuntime_104* outState)
{
    if (!status || !outState)
        return false;

    bool found = false;

    AcquireSRWLockShared(&g_BlockedFreezeRuntimeLock_104);
    const auto it = g_BlockedFreezeRuntime_104.find(status);
    if (it != g_BlockedFreezeRuntime_104.end()) {
        *outState = it->second;
        found = true;
    }
    ReleaseSRWLockShared(&g_BlockedFreezeRuntimeLock_104);

    if (!found)
        return false;

    uint8_t statusID = 0;
    bool isEndStatus = false;

    __try {
        const uintptr_t p = reinterpret_cast<uintptr_t>(status);
        statusID = *reinterpret_cast<const uint8_t*>(p + ActiveLayout::StatusBase_StatusID);
        isEndStatus = *reinterpret_cast<const bool*>(p + ActiveLayout::StatusBase_IsEndStatus);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        RemoveBlockedFreezeRuntime_104(status);
        return false;
    }

    if (statusID != ActiveIds::Status_Freeze || isEndStatus) {
        RemoveBlockedFreezeRuntime_104(status);
        return false;
    }

    return true;
}

static void MarkBlockedFreezeTickObserved_104(void* status)
{
    AcquireSRWLockExclusive(&g_BlockedFreezeRuntimeLock_104);
    const auto it = g_BlockedFreezeRuntime_104.find(status);
    if (it != g_BlockedFreezeRuntime_104.end())
        it->second.TickObservedLogged = true;
    ReleaseSRWLockExclusive(&g_BlockedFreezeRuntimeLock_104);
}

static void PruneEndedBlockedFreezeRuntime_104(void* status)
{
    if (!status)
        return;

    bool ended = false;
    bool readable = false;
    __try {
        const uintptr_t p = reinterpret_cast<uintptr_t>(status);
        ended = *reinterpret_cast<const bool*>(
            p + ActiveLayout::StatusBase_IsEndStatus);
        readable = true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        readable = false;
    }

    // After the original TickStatus returns, an ended Freeze may never tick
    // again. Remove its raw-pointer key immediately so long sessions do not
    // retain stale status instances. An unreadable instance is equally stale.
    if (!readable || ended)
        RemoveBlockedFreezeRuntime_104(status);
}


struct FVectorDouble_104 {
    double X;
    double Y;
    double Z;
};

struct FVectorFloat_104 {
    float X;
    float Y;
    float Z;
};

static double AbsDouble_104(double v)
{
    return (v < 0.0) ? -v : v;
}

static bool IsPlausibleWorldVector_104(
    const FVectorDouble_104& v)
{
    if (v.X != v.X || v.Y != v.Y || v.Z != v.Z)
        return false;

    constexpr double kLimit = 1000000000.0;
    return AbsDouble_104(v.X) < kLimit &&
        AbsDouble_104(v.Y) < kLimit &&
        AbsDouble_104(v.Z) < kLimit;
}

static double VectorDistanceSq_104(
    const FVectorDouble_104& a,
    const FVectorDouble_104& b)
{
    const double dx = a.X - b.X;
    const double dy = a.Y - b.Y;
    const double dz = a.Z - b.Z;
    return dx * dx + dy * dy + dz * dz;
}

static void SetBlockedFreezeStartLocationState_104(
    void* status,
    int8_t state,
    bool refreshLogged)
{
    AcquireSRWLockExclusive(&g_BlockedFreezeRuntimeLock_104);

    const auto it = g_BlockedFreezeRuntime_104.find(status);
    if (it != g_BlockedFreezeRuntime_104.end()) {
        it->second.StartLocationState = state;
        if (refreshLogged)
            it->second.StartLocationRefreshLogged = true;
    }

    ReleaseSRWLockExclusive(&g_BlockedFreezeRuntimeLock_104);
}

// Per-class/runtime discovery. 0 means unresolved.
// Positive values are byte offsets from BP_Status_Freeze_C instance base.
static ptrdiff_t g_FreezeStartLocationOffset_104 = 0;
static bool g_FreezeStartLocationUsesFloat_104 = false;
static SRWLOCK g_FreezeStartLocationLocatorLock_104 = SRWLOCK_INIT;

static bool g_RootReferenceValidationLogged_104 = false;
static bool g_RootReferenceCrossCheckPassed_104 = false;
static double g_RootReferenceCrossCheckDistSq_104 = -1.0;
static bool g_RootReferenceUsedHiddenFallback_104 = false;
static SRWLOCK g_RootReferenceDiagnosticsLock_104 = SRWLOCK_INIT;

static bool ShouldRunRootReferenceCrossCheck_104()
{
    bool shouldRun = false;
    AcquireSRWLockShared(&g_RootReferenceDiagnosticsLock_104);
    shouldRun = !g_RootReferenceValidationLogged_104;
    ReleaseSRWLockShared(&g_RootReferenceDiagnosticsLock_104);
    return shouldRun;
}

static void StoreRootReferenceCrossCheck_104(bool passed, double distSq)
{
    bool changed = false;
    AcquireSRWLockExclusive(&g_RootReferenceDiagnosticsLock_104);
    if (!g_RootReferenceValidationLogged_104) {
        g_RootReferenceCrossCheckPassed_104 = passed;
        g_RootReferenceCrossCheckDistSq_104 = distSq;
        g_RootReferenceValidationLogged_104 = true;
        changed = true;
    }
    ReleaseSRWLockExclusive(&g_RootReferenceDiagnosticsLock_104);
    if (changed)
        MarkRuntimeSnapshotDirty_104();
}

static void MarkRootReferenceHiddenFallbackUsed_104()
{
    bool changed = false;
    AcquireSRWLockExclusive(&g_RootReferenceDiagnosticsLock_104);
    if (!g_RootReferenceUsedHiddenFallback_104) {
        g_RootReferenceUsedHiddenFallback_104 = true;
        changed = true;
    }
    ReleaseSRWLockExclusive(&g_RootReferenceDiagnosticsLock_104);
    if (changed)
        MarkRuntimeSnapshotDirty_104();
}


static bool ReadRootWorldLocation_104(
    void* rootComponent,
    FVectorDouble_104* outLocation)
{
    if (!rootComponent || !outLocation)
        return false;

    // Preferred durable source: USceneComponent::RelativeLocation is a normal
    // reflected/generated field. For an unattached actor root it is the actor's
    // world position. Even when attachment semantics differ, the Freeze
    // StartLocation locator below requires this reference to agree with the BP
    // stored pin coordinate before any write, so a bad reference fails closed.
#if ESE_HAS_GENERATED_SCENE_RELATIVE_LOCATION
    if (EseGeneratedPalLayout::Has_SceneComponent_RelativeLocation) {
        bool rootIsAttached = false;
        bool attachStateKnown = false;

        if (ActiveLayout::HasGeneratedSceneAttachParent) {
            uintptr_t attachParent = 0;
            __try {
                attachParent = *reinterpret_cast<const uintptr_t*>(
                    reinterpret_cast<uintptr_t>(rootComponent) +
                    ActiveLayout::SceneComponent_AttachParent);
                attachStateKnown = true;
                rootIsAttached = attachParent != 0;
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {
                attachStateKnown = false;
            }

            // RelativeLocation is world-space only for an unattached root.
            // On an unknown build, inability to read AttachParent is itself
            // insufficient evidence, so fail closed instead of assuming the
            // root is unattached.
            if (!attachStateKnown &&
                !IsKnown104Fingerprint_104(g_CurrentFingerprint_104)) {
                return false;
            }

            // On unknown builds attached roots also fail closed rather than
            // misusing local-space coordinates. The exact 1.0.4 oracle may
            // still use its separately validated hidden world translation.
            if (attachStateKnown && rootIsAttached &&
                !IsKnown104Fingerprint_104(g_CurrentFingerprint_104)) {
                return false;
            }
        }

        FVectorDouble_104 relative{};
        bool readOk = false;
        __try {
            relative = *reinterpret_cast<const FVectorDouble_104*>(
                reinterpret_cast<uintptr_t>(rootComponent) +
                EseGeneratedPalLayout::SceneComponent_RelativeLocation);
            readOk = true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            readOk = false;
        }

        if (readOk && IsPlausibleWorldVector_104(relative)) {
            // If the generated SDK proves that this root is attached,
            // RelativeLocation is local-space. Preserve known-1.0.4 behavior
            // via the separately validated hidden world field, but never
            // generalize that hidden offset to an unknown executable.
            if (attachStateKnown && rootIsAttached &&
                IsKnown104Fingerprint_104(g_CurrentFingerprint_104)) {
                FVectorDouble_104 attachedWorld{};
                bool attachedWorldOk = false;
                __try {
                    attachedWorld = *reinterpret_cast<const FVectorDouble_104*>(
                        reinterpret_cast<uintptr_t>(rootComponent) +
                        Known104::Offset::RootComponent_WorldLocation);
                    attachedWorldOk = true;
                }
                __except (EXCEPTION_EXECUTE_HANDLER) {
                    attachedWorldOk = false;
                }

                if (!attachedWorldOk ||
                    !IsPlausibleWorldVector_104(attachedWorld)) {
                    return false;
                }

                *outLocation = attachedWorld;
                MarkRootReferenceHiddenFallbackUsed_104();
                return true;
            }

            *outLocation = relative;

            // On the preserved 1.0.4 oracle only, independently compare the
            // generated reflected location against the old hidden
            // ComponentToWorld translation. The hidden +0x260 value is never
            // trusted on unknown builds.
            if (ShouldRunRootReferenceCrossCheck_104() &&
                IsKnown104Fingerprint_104(g_CurrentFingerprint_104)) {
                FVectorDouble_104 oldWorld{};
                bool oldOk = false;
                __try {
                    oldWorld = *reinterpret_cast<const FVectorDouble_104*>(
                        reinterpret_cast<uintptr_t>(rootComponent) +
                        Known104::Offset::RootComponent_WorldLocation);
                    oldOk = true;
                }
                __except (EXCEPTION_EXECUTE_HANDLER) {
                    oldOk = false;
                }

                const double distSq = oldOk
                    ? VectorDistanceSq_104(relative, oldWorld)
                    : 1.0e30;
                const bool crossCheckPass =
                    oldOk && IsPlausibleWorldVector_104(oldWorld) && distSq <= 1.0;
                ModLog(
                    "[ElementalSystemExpanded] ROOT POSITION SOURCE: generated "
                    "USceneComponent::RelativeLocation +0x%llX; 1.0.4 hidden-world "
                    "cross-check %s DistSq=%.3f.\n",
                    static_cast<unsigned long long>(
                        EseGeneratedPalLayout::SceneComponent_RelativeLocation),
                    crossCheckPass ? "PASS" : "DIFF",
                    distSq);
                StoreRootReferenceCrossCheck_104(crossCheckPass, distSq);
            }
            return true;
        }
    }
#endif

    // Compatibility fallback for the preserved known build / older generated
    // headers. Never use the hidden +0x260 field as an authority on an unknown
    // executable.
    if (!IsKnown104Fingerprint_104(g_CurrentFingerprint_104))
        return false;

    MarkRootReferenceHiddenFallbackUsed_104();
    __try {
        *outLocation =
            *reinterpret_cast<const FVectorDouble_104*>(
                reinterpret_cast<uintptr_t>(rootComponent) +
                Known104::Offset::RootComponent_WorldLocation);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }

    return IsPlausibleWorldVector_104(*outLocation);
}

static bool IsReadableRange_104(
    uintptr_t address,
    size_t size)
{
    if (!address || !size)
        return false;

    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(
        reinterpret_cast<const void*>(address),
        &mbi,
        sizeof(mbi)) != sizeof(mbi)) {
        return false;
    }

    if (mbi.State != MEM_COMMIT ||
        (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
        return false;
    }

    const uintptr_t regionBegin =
        reinterpret_cast<uintptr_t>(mbi.BaseAddress);
    if (mbi.RegionSize > (std::numeric_limits<uintptr_t>::max)() - regionBegin)
        return false;
    const uintptr_t regionEnd = regionBegin + mbi.RegionSize;

    if (address < regionBegin || address >= regionEnd)
        return false;
    return size <= (regionEnd - address);
}

static bool LocateFreezeStartLocation_104(
    void* status,
    const FVectorDouble_104& current,
    ptrdiff_t* outOffset,
    bool* outUsesFloat)
{
    if (!status || !outOffset || !outUsesFloat)
        return false;

    struct FCandidate {
        ptrdiff_t Offset;
        bool UsesFloat;
        double DistanceSq;
        FVectorDouble_104 Value;
    };

    FCandidate candidates[32]{};
    int candidateCount = 0;

    // Search begins at the generated native UPalStatusBase size, so a future
    // SDK layout shift does not require a hardcoded Blueprint-tail offset.
    // This intentionally avoids scanning arbitrary UObject memory.
    const ptrdiff_t kStart =
        static_cast<ptrdiff_t>(ActiveLayout::StatusBase_NativeSize);
    constexpr ptrdiff_t kEnd = 0x400;

    const uintptr_t base =
        reinterpret_cast<uintptr_t>(status);

    // UE5 FVector = 3 doubles. Search 8-byte-aligned locations.
    for (ptrdiff_t off = kStart; off <= kEnd - 24; off += 8) {
        if (!IsReadableRange_104(base + off, 24))
            break;

        FVectorDouble_104 value{};

        __try {
            value =
                *reinterpret_cast<const FVectorDouble_104*>(
                    base + off);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            continue;
        }

        if (!IsPlausibleWorldVector_104(value))
            continue;

        const double distSq =
            VectorDistanceSq_104(value, current);

        // 2 metres is deliberately generous for initial discovery.
        if (distSq <= 40000.0 && candidateCount < 32) {
            candidates[candidateCount++] =
                FCandidate{ off, false, distSq, value };
        }
    }

    // Also inspect 3-float layouts in case this generated BP property was
    // emitted with float precision despite the engine-side root transform.
    for (ptrdiff_t off = kStart; off <= kEnd - 12; off += 4) {
        if (!IsReadableRange_104(base + off, 12))
            break;

        FVectorFloat_104 f{};

        __try {
            f =
                *reinterpret_cast<const FVectorFloat_104*>(
                    base + off);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            continue;
        }

        const FVectorDouble_104 value{
            static_cast<double>(f.X),
            static_cast<double>(f.Y),
            static_cast<double>(f.Z)
        };

        if (!IsPlausibleWorldVector_104(value))
            continue;

        const double distSq =
            VectorDistanceSq_104(value, current);

        if (distSq <= 40000.0 && candidateCount < 32) {
            // Avoid reporting a float candidate that is just the low halves of
            // an already-matching double FVector at the same offset.
            bool duplicate = false;
            for (int i = 0; i < candidateCount; ++i) {
                if (candidates[i].Offset == off &&
                    !candidates[i].UsesFloat) {
                    duplicate = true;
                    break;
                }
            }

            if (!duplicate) {
                candidates[candidateCount++] =
                    FCandidate{ off, true, distSq, value };
            }
        }
    }

    ModLog(
        "[ElementalSystemExpanded] STARTLOCATION LOCATOR: "
        "Instance=%p RootCurrent=(%.3f, %.3f, %.3f) Candidates=%d\n",
        status,
        current.X,
        current.Y,
        current.Z,
        candidateCount);

    for (int i = 0; i < candidateCount; ++i) {
        const FCandidate& c = candidates[i];

        ModLog(
            "[ElementalSystemExpanded] STARTLOCATION CANDIDATE: "
            "Offset=+0x%llX Format=%s Value=(%.3f, %.3f, %.3f) "
            "DistSq=%.3f\n",
            static_cast<unsigned long long>(c.Offset),
            c.UsesFloat ? "float3" : "double3",
            c.Value.X,
            c.Value.Y,
            c.Value.Z,
            c.DistanceSq);
    }

    if (candidateCount != 1) {
        ShipLog_104(
            "[ElementalSystemExpanded] WARNING: STARTLOCATION LOCATOR %s: "
            "need exactly one candidate; NO WRITE performed.\n",
            candidateCount == 0 ? "FAILED" : "AMBIGUOUS");
        return false;
    }

    *outOffset = candidates[0].Offset;
    *outUsesFloat = candidates[0].UsesFloat;

    ModLog(
        "[ElementalSystemExpanded] STARTLOCATION LOCATOR PASSED: "
        "unique candidate Offset=+0x%llX Format=%s.\n",
        static_cast<unsigned long long>(*outOffset),
        *outUsesFloat ? "float3" : "double3");

    return true;
}

static bool RefreshBlockedFreezeStartLocation_104(
    void* status,
    FBlockedFreezeRuntime_104* ioState)
{
    if (!status || !ioState || !ioState->RootComponent)
        return false;

    FVectorDouble_104 current{};
    if (!ReadRootWorldLocation_104(
        ioState->RootComponent,
        &current)) {
        return false;
    }

    if (ioState->StartLocationState == 0) {
        ptrdiff_t locatedOffset = 0;
        bool usesFloat = false;
        bool discoveredNow = false;

        AcquireSRWLockExclusive(
            &g_FreezeStartLocationLocatorLock_104);

        if (g_FreezeStartLocationOffset_104 == 0) {
            if (!LocateFreezeStartLocation_104(
                status,
                current,
                &locatedOffset,
                &usesFloat)) {
                ReleaseSRWLockExclusive(
                    &g_FreezeStartLocationLocatorLock_104);

                ioState->StartLocationState = -1;
                SetBlockedFreezeStartLocationState_104(
                    status, -1, false);
                return false;
            }

            g_FreezeStartLocationOffset_104 =
                locatedOffset;
            g_FreezeStartLocationUsesFloat_104 =
                usesFloat;
            discoveredNow = true;
        }

        locatedOffset =
            g_FreezeStartLocationOffset_104;
        usesFloat =
            g_FreezeStartLocationUsesFloat_104;

        ReleaseSRWLockExclusive(
            &g_FreezeStartLocationLocatorLock_104);

        if (discoveredNow)
            MarkRuntimeSnapshotDirty_104();

        // Per-instance re-validation of the cached class offset.
        FVectorDouble_104 candidate{};
        bool readOk = false;

        __try {
            if (usesFloat) {
                const FVectorFloat_104 f =
                    *reinterpret_cast<const FVectorFloat_104*>(
                        reinterpret_cast<uintptr_t>(status) +
                        locatedOffset);

                candidate = FVectorDouble_104{
                    static_cast<double>(f.X),
                    static_cast<double>(f.Y),
                    static_cast<double>(f.Z)
                };
            }
            else {
                candidate =
                    *reinterpret_cast<const FVectorDouble_104*>(
                        reinterpret_cast<uintptr_t>(status) +
                        locatedOffset);
            }

            readOk = true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            readOk = false;
        }

        const double distSq =
            readOk
            ? VectorDistanceSq_104(candidate, current)
            : 1.0e30;

        if (!readOk ||
            !IsPlausibleWorldVector_104(candidate) ||
            distSq > 40000.0) {
            ShipLog_104(
                "[ElementalSystemExpanded] WARNING: STARTLOCATION REVALIDATE FAILED: "
                "Instance=%p Offset=+0x%llX Format=%s DistSq=%.3f; "
                "NO WRITE.\n",
                status,
                static_cast<unsigned long long>(locatedOffset),
                usesFloat ? "float3" : "double3",
                distSq);

            ioState->StartLocationState = -1;
            SetBlockedFreezeStartLocationState_104(
                status, -1, false);
            return false;
        }

        ioState->StartLocationState = 1;
        SetBlockedFreezeStartLocationState_104(
            status, 1, false);
    }

    if (ioState->StartLocationState != 1)
        return false;

    ptrdiff_t offset = 0;
    bool usesFloat = false;
    AcquireSRWLockShared(&g_FreezeStartLocationLocatorLock_104);
    offset = g_FreezeStartLocationOffset_104;
    usesFloat = g_FreezeStartLocationUsesFloat_104;
    ReleaseSRWLockShared(&g_FreezeStartLocationLocatorLock_104);

    // A validated instance must never write through an absent class locator.
    if (offset <= 0) {
        ioState->StartLocationState = -1;
        SetBlockedFreezeStartLocationState_104(status, -1, false);
        ShipLog_104(
            "[ElementalSystemExpanded] WARNING: STARTLOCATION REFRESH ERROR: "
            "validated instance has no active locator; refresh disabled for Instance=%p.\n",
            status);
        return false;
    }

    __try {
        if (usesFloat) {
            *reinterpret_cast<FVectorFloat_104*>(
                reinterpret_cast<uintptr_t>(status) + offset) =
                FVectorFloat_104{
                    static_cast<float>(current.X),
                    static_cast<float>(current.Y),
                    static_cast<float>(current.Z)
            };
        }
        else {
            *reinterpret_cast<FVectorDouble_104*>(
                reinterpret_cast<uintptr_t>(status) + offset) =
                current;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        ioState->StartLocationState = -1;
        SetBlockedFreezeStartLocationState_104(
            status, -1, false);

        ShipLog_104(
            "[ElementalSystemExpanded] WARNING: STARTLOCATION REFRESH ERROR: "
            "write failed; refresh disabled for Instance=%p.\n",
            status);
        return false;
    }

    if (!ioState->StartLocationRefreshLogged) {
        ModLog(
            "[ElementalSystemExpanded] STARTLOCATION REFRESH: "
            "Instance=%p Offset=+0x%llX Format=%s <- "
            "current root (%.3f, %.3f, %.3f) before Freeze TickStatus.\n",
            status,
            static_cast<unsigned long long>(offset),
            usesFloat ? "float3" : "double3",
            current.X,
            current.Y,
            current.Z);

        ioState->StartLocationRefreshLogged = true;
        SetBlockedFreezeStartLocationState_104(
            status, 1, true);
    }

    return true;
}

static bool IsExecutableAddressPtr_104(void* p)
{
    return p && IsExecutableAddress_104(
        reinterpret_cast<uintptr_t>(p));
}

static void __fastcall Detour_SetComponentTickEnabledVirtual_104(
    void* actorComponent,
    bool enabled)
{
    if (IsBlockedReactionScope_104() &&
        g_ReactionTLS_104.StatusID == ActiveIds::Status_Freeze &&
        !enabled) {
        ModLog(
            "[ElementalSystemExpanded] REACTION BLOCK: Freeze "
            "SetComponentTickEnabled(false) Component=%p\n",
            actorComponent);
        return;
    }

    if (Original_SetComponentTickEnabledVirtual_104)
        Original_SetComponentTickEnabledVirtual_104(
            actorComponent, enabled);
}

static void __fastcall Detour_StopAnimMontageVirtual_104(
    void* character,
    void* montage)
{
    if (IsBlockedReactionScope_104() &&
        g_ReactionTLS_104.StatusID == ActiveIds::Status_Freeze) {

        void* targetCharacter =
            ResolveOwningPalCharacter_104(
                g_ReactionTLS_104.DamageReaction);

        if (targetCharacter == character) {
            ModLog(
                "[ElementalSystemExpanded] REACTION BLOCK: Freeze "
                "StopAnimMontage Character=%p Montage=%p\n",
                character,
                montage);
            return;
        }
    }

    if (Original_StopAnimMontageVirtual_104)
        Original_StopAnimMontageVirtual_104(
            character, montage);
}

static void __fastcall Detour_FreezeTickVirtual_104(
    void* status,
    float deltaTime)
{
    FBlockedFreezeRuntime_104 state{};

    if (!GetBlockedFreezeRuntime_104(status, &state)) {
        if (Original_FreezeTickVirtual_104)
            Original_FreezeTickVirtual_104(
                status, deltaTime);
        return;
    }

    if (!state.TickObservedLogged) {
        ModLog(
            "[ElementalSystemExpanded] FREEZE TICK HIT: "
            "Blocked Instance=%p Character=%p Root=%p DeltaTime=%.6f\n",
            status,
            state.Character,
            state.RootComponent,
            deltaTime);

        MarkBlockedFreezeTickObserved_104(status);
    }

    // Keep the complete original TickStatus/lifecycle; if the
    // inferred BP StartLocation field validates against the real root position,
    // refresh it to the Pal's current position immediately before each tick.
    RefreshBlockedFreezeStartLocation_104(
        status,
        &state);

    const FFreezeTickTLS_104 previous =
        g_FreezeTickTLS_104;

    g_FreezeTickTLS_104.Active = true;
    g_FreezeTickTLS_104.Status = status;

    // The original TickStatus call MUST execute for proper duration and
    // cleanup. We no longer skip the Freeze tick.
    if (Original_FreezeTickVirtual_104)
        Original_FreezeTickVirtual_104(
            status, deltaTime);

    PruneEndedBlockedFreezeRuntime_104(status);
    g_FreezeTickTLS_104 = previous;
}

static bool InstallPreAddFreezeVirtualHooks_104(
    void* statusComponent,
    void* character)
{
    if (!statusComponent || !character)
        return false;

    void* tickEnabledTarget = nullptr;
    void* stopMontageTarget = nullptr;

    __try {
        void** componentVtable =
            *reinterpret_cast<void***>(statusComponent);
        void** characterVtable =
            *reinterpret_cast<void***>(character);

        if (!componentVtable || !characterVtable)
            return false;

        // ActorComponent::SetComponentTickEnabled wrapper:
        //   call [component vtable + 0x3B8]
        // This method is expected to be inherited by PalSkeletalMeshComponent.
        tickEnabledTarget =
            componentVtable[g_VSlot_SetComponentTickEnabled_104 / sizeof(void*)];

        // Character::StopAnimMontage wrapper:
        //   call [character vtable + 0x888]
        stopMontageTarget =
            characterVtable[g_VSlot_StopAnimMontage_104 / sizeof(void*)];
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }

    if (!IsExecutableAddressPtr_104(tickEnabledTarget) ||
        !IsExecutableAddressPtr_104(stopMontageTarget)) {
        ShipLog_104(
            "[ElementalSystemExpanded] ERROR: pre-AddStatus Freeze virtual "
            "target invalid. ComponentTick=%p StopMontage=%p\n",
            tickEnabledTarget,
            stopMontageTarget);
        return false;
    }

    AcquireSRWLockExclusive(
        &g_FreezeDynamicHookLock_104);

    FDynamicHookTransactionSpec_104 hooks[] = {
        {
            "ActorComponent::SetComponentTickEnabled",
            tickEnabledTarget,
            reinterpret_cast<void*>(&Detour_SetComponentTickEnabledVirtual_104),
            reinterpret_cast<void**>(&Original_SetComponentTickEnabledVirtual_104),
            &g_SetComponentTickEnabledVirtualTarget_104,
            g_VSlot_SetComponentTickEnabled_104
        },
        {
            "Character::StopAnimMontage",
            stopMontageTarget,
            reinterpret_cast<void*>(&Detour_StopAnimMontageVirtual_104),
            reinterpret_cast<void**>(&Original_StopAnimMontageVirtual_104),
            &g_StopAnimMontageVirtualTarget_104,
            g_VSlot_StopAnimMontage_104
        }
    };

    const bool ok = EnsureDynamicHookTransaction_104(
        "FreezePreAddVirtuals",
        hooks,
        sizeof(hooks) / sizeof(hooks[0]));

    ReleaseSRWLockExclusive(
        &g_FreezeDynamicHookLock_104);

    return ok;
}

static bool InstallDynamicFreezeVirtualHooks_104(
    void* character,
    void* freezeStatus)
{
    if (!character || !freezeStatus)
        return false;

    void* stopMontageTarget = nullptr;
    void* freezeTickTarget = nullptr;
    void* rootComponent = nullptr;

    __try {
        void** characterVtable =
            *reinterpret_cast<void***>(character);
        void** statusVtable =
            *reinterpret_cast<void***>(freezeStatus);

        if (!characterVtable || !statusVtable)
            return false;

        stopMontageTarget =
            characterVtable[g_VSlot_StopAnimMontage_104 / sizeof(void*)];

        freezeTickTarget =
            statusVtable[g_VSlot_StatusTick_104 / sizeof(void*)];

        // AActor::RootComponent from the active generated/validated layout.
        rootComponent =
            *reinterpret_cast<void**>(
                reinterpret_cast<uintptr_t>(character) + ActiveLayout::Character_RootComponent);

    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }

    size_t rootReadableSpan =
        static_cast<size_t>(ActiveLayout::SceneComponent_RelativeLocation + sizeof(FVectorDouble_104));
#if ESE_HAS_GENERATED_SCENE_RELATIVE_LOCATION
    if (ActiveLayout::HasGeneratedSceneAttachParent) {
        rootReadableSpan = (std::max)(
            rootReadableSpan,
            static_cast<size_t>(
                ActiveLayout::SceneComponent_AttachParent + sizeof(uintptr_t)));
    }
#endif

    if (!IsExecutableAddressPtr_104(stopMontageTarget) ||
        !IsExecutableAddressPtr_104(freezeTickTarget) ||
        !ValidateUObjectLikePointer_104(rootComponent, rootReadableSpan)) {
        ShipLog_104(
            "[ElementalSystemExpanded] ERROR: dynamic Freeze target "
            "invalid. StopMontage=%p Tick=%p Root=%p\n",
            stopMontageTarget,
            freezeTickTarget,
            rootComponent);
        return false;
    }

    AcquireSRWLockExclusive(
        &g_FreezeDynamicHookLock_104);

    FDynamicHookTransactionSpec_104 hooks[] = {
        {
            "Character::StopAnimMontage",
            stopMontageTarget,
            reinterpret_cast<void*>(&Detour_StopAnimMontageVirtual_104),
            reinterpret_cast<void**>(&Original_StopAnimMontageVirtual_104),
            &g_StopAnimMontageVirtualTarget_104,
            g_VSlot_StopAnimMontage_104
        },
        {
            "Freeze TickStatus",
            freezeTickTarget,
            reinterpret_cast<void*>(&Detour_FreezeTickVirtual_104),
            reinterpret_cast<void**>(&Original_FreezeTickVirtual_104),
            &g_FreezeTickVirtualTarget_104,
            g_VSlot_StatusTick_104
        }
    };

    const bool ok = EnsureDynamicHookTransaction_104(
        "FreezePersistentVirtuals",
        hooks,
        sizeof(hooks) / sizeof(hooks[0]));

    ReleaseSRWLockExclusive(
        &g_FreezeDynamicHookLock_104);

    if (!ok)
        return false;

    AcquireSRWLockExclusive(
        &g_BlockedFreezeRuntimeLock_104);

    g_BlockedFreezeRuntime_104[freezeStatus] =
        FBlockedFreezeRuntime_104{
            character,
            rootComponent,
            false,
            0,
            false
    };

    ReleaseSRWLockExclusive(
        &g_BlockedFreezeRuntimeLock_104);

    ModLog(
        "[ElementalSystemExpanded] Freeze persistent native gate: "
        "Instance=%p Character=%p Root=%p State=BLOCKED; "
        "StartLocation validation pending.\n",
        freezeStatus,
        character,
        rootComponent);

    return true;
}


// ---------------------------------------------------------
// Freeze IceCondition visual-effect cleanup
//
// SDK / memoryViewDump12:
//   APalCharacter::VisualEffectComponent     +0x678
//   UPalVisualEffectComponent::ExecutionVisualEffects +0x120
//   UPalVisualEffectBase::VisualEffectID     +0x50
//   EPalVisualEffectID::IceCondition         17
//
// We do NOT prevent creation. For blocked dry/ICD Freeze we let vanilla create
// the effect first, then locate the live ID 17 instance and invoke the verified
// native RemoveVisualEffect_Local(Component, 17). This preserves the visual-
// effect system's normal OnEnd/cleanup path without suppressing creation.
// ---------------------------------------------------------

static constexpr uint8_t kIceConditionVisualEffectID_104 = ActiveIds::VisualEffect_IceCondition;

static void* ResolveVisualEffectComponent_104(void* character)
{
    if (!character)
        return nullptr;

    __try {
        return *reinterpret_cast<void**>(
            reinterpret_cast<uintptr_t>(character) + ActiveLayout::Character_VisualEffectComponent);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

static int CountVisualEffectID_104(
    void* visualEffectComponent,
    uint8_t wantedID,
    bool logEntries)
{
    if (!visualEffectComponent)
        return 0;

    FRawTArray_104 list{};

    __try {
        list = *reinterpret_cast<FRawTArray_104*>(
            reinterpret_cast<uintptr_t>(visualEffectComponent) + ActiveLayout::VisualEffectComponent_ExecutionVisualEffects);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }

    if (!ValidateRawPointerArray_104(list, 1024, 4096)) {
        if (g_DebugDiagnosticsEnabled_104) {
            InterlockedIncrement(&g_VfxArrayValidationFailures_104);
            MarkRuntimeSnapshotDirty_104();
        }
        ShipLog_104(
            "[ElementalSystemExpanded] WARNING: VisualEffect array ABI/layout invalid. "
            "Component=%p Data=%p Num=%d Max=%d\n",
            visualEffectComponent,
            reinterpret_cast<void*>(list.Data),
            list.Num,
            list.Max);
        return 0;
    }

    if (g_DebugDiagnosticsEnabled_104)
        InterlockedIncrement(&g_VfxArrayValidationPasses_104);

    int matches = 0;
    const int32_t limit = (list.Num < 64) ? list.Num : 64;

    for (int32_t i = 0; i < limit; ++i) {
        void* effect = nullptr;
        uint8_t effectID = 0xFF;
        bool isEnd = false;

        __try {
            effect = *reinterpret_cast<void**>(
                list.Data + static_cast<uintptr_t>(i) * sizeof(void*));

            if (!effect)
                continue;

            const size_t effectReadableSpan =
                static_cast<size_t>((std::max)(
                    ActiveLayout::VisualEffectBase_ID + sizeof(uint8_t),
                    ActiveLayout::VisualEffectBase_IsEnd + sizeof(bool)));

            if (!ValidateUObjectLikePointer_104(
                effect,
                effectReadableSpan)) {
                continue;
            }

            const uintptr_t e =
                reinterpret_cast<uintptr_t>(effect);

            effectID =
                *reinterpret_cast<uint8_t*>(e + ActiveLayout::VisualEffectBase_ID);
            isEnd =
                *reinterpret_cast<bool*>(e + ActiveLayout::VisualEffectBase_IsEnd);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            continue;
        }

        if (logEntries) {
            ModLog(
                "[ElementalSystemExpanded] VFX ACTIVE: "
                "Component=%p Index=%d Effect=%p ID=%u End=%s\n",
                visualEffectComponent,
                i,
                effect,
                static_cast<unsigned>(effectID),
                isEnd ? "YES" : "NO");
        }

        if (effectID == wantedID && !isEnd)
            ++matches;
    }

    return matches;
}

static bool RemoveBlockedFreezeIceCondition_104(
    void* character)
{
    if (!character) {
        ModLog(
            "[ElementalSystemExpanded] ICE VFX: no owning character; "
            "cannot inspect IceCondition.\n");
        return false;
    }

    void* visualEffectComponent =
        ResolveVisualEffectComponent_104(character);

    if (!visualEffectComponent) {
        ModLog(
            "[ElementalSystemExpanded] ICE VFX: Character=%p has no "
            "VisualEffectComponent at active offset +0x%llX.\n",
            character,
            static_cast<unsigned long long>(
                ActiveLayout::Character_VisualEffectComponent));
        return false;
    }

    const int before = CountVisualEffectID_104(
        visualEffectComponent,
        kIceConditionVisualEffectID_104,
        true);

    ModLog(
        "[ElementalSystemExpanded] ICE VFX BEFORE: Character=%p "
        "Component=%p IceCondition(ID=%u) ActiveCount=%d\n",
        character,
        visualEffectComponent,
        static_cast<unsigned>(kIceConditionVisualEffectID_104),
        before);

    if (before <= 0) {
        ModLog(
            "[ElementalSystemExpanded] ICE VFX NOTE: no active ID 17 was "
            "present immediately after blocked Freeze AddStatus.\n");
        return false;
    }

    if (!Native_RemoveVisualEffect_Local_104) {
        ShipLog_104(
            "[ElementalSystemExpanded] ERROR: native "
            "RemoveVisualEffect_Local is unavailable.\n");
        return false;
    }

    Native_RemoveVisualEffect_Local_104(
        visualEffectComponent,
        kIceConditionVisualEffectID_104);

    const int after = CountVisualEffectID_104(
        visualEffectComponent,
        kIceConditionVisualEffectID_104,
        false);

    ModLog(
        "[ElementalSystemExpanded] ICE VFX REMOVE: "
        "RemoveVisualEffect_Local(ID=%u) called. "
        "Component=%p ActiveCount %d->%d\n",
        static_cast<unsigned>(kIceConditionVisualEffectID_104),
        visualEffectComponent,
        before,
        after);

    return after < before;
}

static constexpr uint8_t kDarkConditionVisualEffectID_104 = 21;
static constexpr uint8_t kCameraVignetteVisualEffectID_104 = 24;
static constexpr uint8_t kLightVisualEffectLookupID_104 = 57;
static constexpr uint8_t kLightCameraVisualEffectLookupID_104 = 58;

static bool SafeWriteVisualEffectID_104(
    void* visualEffect,
    uint8_t visualEffectID)
{
    if (!visualEffect)
        return false;

    const size_t requiredSpan =
        static_cast<size_t>(
            ActiveLayout::VisualEffectBase_ID + sizeof(uint8_t));

    if (!ValidateUObjectLikePointer_104(
        visualEffect,
        requiredSpan)) {
        return false;
    }

    __try {
        *reinterpret_cast<uint8_t*>(
            reinterpret_cast<uintptr_t>(visualEffect) +
            ActiveLayout::VisualEffectBase_ID) = visualEffectID;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static void* AddVisualEffectWithLightSubstitution_104(
    const char* pathName,
    NativeAddVisualEffect_104_t original,
    void* visualEffectComponent,
    uint8_t visualEffectID,
    const void* dynamicParameter)
{
    if (!original)
        return nullptr;

    const EBlindnessSource_104 source =
        g_BlindnessSourceTLS_104;

    // CameraVignette is created through AddVisualEffect_Local by
    // BP_Status_Darkness. For Light provenance, redirect class lookup to the
    // custom white camera VFX registered by Lua under lookup ID 58, then
    // restore runtime identity to CameraVignette (24) for vanilla teardown.
    if (visualEffectID == kCameraVignetteVisualEffectID_104) {
        ModLog(
            "[ElementalSystemExpanded] CAMERA VIGNETTE PATH: "
            "Path=%s Component=%p ID=%u Source=%s Parameter=%p\n",
            pathName ? pathName : "<unknown>",
            visualEffectComponent,
            static_cast<unsigned>(visualEffectID),
            BlindnessSourceName_104(source),
            dynamicParameter);

        if (source != EBlindnessSource_104::Light) {
            return original(
                visualEffectComponent,
                visualEffectID,
                dynamicParameter);
        }

        // End any previous CameraVignette through Palworld's own cleanup
        // before constructing the Light replacement.
        if (Native_RemoveVisualEffect_Local_104) {
            Native_RemoveVisualEffect_Local_104(
                visualEffectComponent,
                kCameraVignetteVisualEffectID_104);
        }

        void* lightCameraEffect = original(
            visualEffectComponent,
            kLightCameraVisualEffectLookupID_104,
            dynamicParameter);

        if (lightCameraEffect &&
            SafeWriteVisualEffectID_104(
                lightCameraEffect,
                kCameraVignetteVisualEffectID_104)) {
            ModLog(
                "[ElementalSystemExpanded] LIGHT CAMERA VFX SUBSTITUTE: "
                "Path=%s Component=%p LookupID=%u->RuntimeID=%u "
                "Effect=%p Source=LIGHT Result=OK\n",
                pathName ? pathName : "<unknown>",
                visualEffectComponent,
                static_cast<unsigned>(
                    kLightCameraVisualEffectLookupID_104),
                static_cast<unsigned>(
                    kCameraVignetteVisualEffectID_104),
                lightCameraEffect);
            return lightCameraEffect;
        }

        // Fail closed. Remove a partially-created lookup-ID effect if possible,
        // then fall back to vanilla CameraVignette ID 24.
        if (lightCameraEffect && Native_RemoveVisualEffect_Local_104) {
            Native_RemoveVisualEffect_Local_104(
                visualEffectComponent,
                kLightCameraVisualEffectLookupID_104);
        }

        ShipLog_104(
            "[ElementalSystemExpanded] WARNING: LIGHT CAMERA VFX SUBSTITUTE failed. "
            "Path=%s Component=%p LookupID=%u Effect=%p; "
            "falling back to vanilla CameraVignette ID=%u.\n",
            pathName ? pathName : "<unknown>",
            visualEffectComponent,
            static_cast<unsigned>(
                kLightCameraVisualEffectLookupID_104),
            lightCameraEffect,
            static_cast<unsigned>(
                kCameraVignetteVisualEffectID_104));

        return original(
            visualEffectComponent,
            kCameraVignetteVisualEffectID_104,
            dynamicParameter);
    }

    // Dark and all unrelated visual effects remain completely vanilla.
    if (visualEffectID != kDarkConditionVisualEffectID_104 ||
        source != EBlindnessSource_104::Light) {
        return original(
            visualEffectComponent,
            visualEffectID,
            dynamicParameter);
    }

    // End any previous DarkCondition through Palworld's own cleanup before
    // constructing the Light replacement.
    if (Native_RemoveVisualEffect_Local_104) {
        Native_RemoveVisualEffect_Local_104(
            visualEffectComponent,
            kDarkConditionVisualEffectID_104);
    }

    // Lua registers BP_VisualEffect_Status_Light_C in
    // UPalVisualEffectDataBase::VisualEffectClassDataAsset under lookup ID 57.
    // Use that ID only for class selection/creation.
    void* lightEffect = original(
        visualEffectComponent,
        kLightVisualEffectLookupID_104,
        dynamicParameter);

    // Restore the runtime identity to DarkCondition so vanilla status teardown
    // and visual-effect cleanup continue to operate on ID 21.
    if (lightEffect &&
        SafeWriteVisualEffectID_104(
            lightEffect,
            kDarkConditionVisualEffectID_104)) {
        ModLog(
            "[ElementalSystemExpanded] LIGHT VFX SUBSTITUTE: "
            "Path=%s Component=%p LookupID=%u->RuntimeID=%u "
            "Effect=%p Source=LIGHT Result=OK\n",
            pathName ? pathName : "<unknown>",
            visualEffectComponent,
            static_cast<unsigned>(
                kLightVisualEffectLookupID_104),
            static_cast<unsigned>(
                kDarkConditionVisualEffectID_104),
            lightEffect);
        return lightEffect;
    }

    // Fail closed. Remove a partially-created lookup-ID effect if possible,
    // then fall back to the unmodified vanilla DarkCondition path.
    if (lightEffect && Native_RemoveVisualEffect_Local_104) {
        Native_RemoveVisualEffect_Local_104(
            visualEffectComponent,
            kLightVisualEffectLookupID_104);
    }

    ShipLog_104(
        "[ElementalSystemExpanded] WARNING: LIGHT VFX SUBSTITUTE failed. "
        "Path=%s Component=%p LookupID=%u Effect=%p; "
        "falling back to vanilla DarkCondition ID=%u.\n",
        pathName ? pathName : "<unknown>",
        visualEffectComponent,
        static_cast<unsigned>(
            kLightVisualEffectLookupID_104),
        lightEffect,
        static_cast<unsigned>(
            kDarkConditionVisualEffectID_104));

    return original(
        visualEffectComponent,
        kDarkConditionVisualEffectID_104,
        dynamicParameter);
}

static void* __fastcall Detour_AddVisualEffect_104(
    void* visualEffectComponent,
    uint8_t visualEffectID,
    const void* dynamicParameter)
{
    return AddVisualEffectWithLightSubstitution_104(
        "AddVisualEffect",
        Original_AddVisualEffect_104,
        visualEffectComponent,
        visualEffectID,
        dynamicParameter);
}

static void* __fastcall Detour_AddVisualEffect_Local_104(
    void* visualEffectComponent,
    uint8_t visualEffectID,
    const void* dynamicParameter)
{
    return AddVisualEffectWithLightSubstitution_104(
        "AddVisualEffect_Local",
        Original_AddVisualEffect_Local_104,
        visualEffectComponent,
        visualEffectID,
        dynamicParameter);
}

static EBlindnessSource_104 ResolveBlindnessSourceFromRawContext_104()
{
    uint8_t attackElement = 0;
    if (!GetRawAttackElement_104(&attackElement))
        return EBlindnessSource_104::Unknown;

    if (attackElement == ActiveIds::Element_Normal)
        return EBlindnessSource_104::Light;
    if (attackElement == ActiveIds::Element_Dark)
        return EBlindnessSource_104::Dark;

    return EBlindnessSource_104::Unknown;
}

static bool ApplyStatusWithDuration_104(
    void* damageReaction,
    void* statusComponent,
    uint8_t statusID,
    uint8_t units,
    uint8_t sourceElement,
    float durationOverride)
{
    if (!Native_AddStatus_104 || !Original_NativeAddStatus_104) {
        ShipLog_104(
            "[ElementalSystemExpanded] ERROR: Native AddStatus unavailable; "
            "StatusID=%u not applied.\n",
            static_cast<unsigned>(statusID));
        return false;
    }

    bool blockedFreezePersistent = false;
    bool freezeHadWetness = false;
    void* reactionCharacter = nullptr;

    {
        FReactionTLSGuard_104 reactionGuard(
            damageReaction,
            statusComponent,
            statusID);

        freezeHadWetness =
            g_ReactionTLS_104.Active &&
            g_ReactionTLS_104.StatusID == ActiveIds::Status_Freeze &&
            g_ReactionTLS_104.HadWetness;

        blockedFreezePersistent =
            g_ReactionTLS_104.Active &&
            g_ReactionTLS_104.StatusID == ActiveIds::Status_Freeze &&
            !g_ReactionTLS_104.AllowStrongControl;

        if (blockedFreezePersistent) {
            reactionCharacter =
                ResolveOwningPalCharacter_104(
                    damageReaction);

            if (!InstallPreAddFreezeVirtualHooks_104(
                statusComponent,
                reactionCharacter)) {
                ShipLog_104(
                    "[ElementalSystemExpanded] WARNING: pre-AddStatus Freeze "
                    "virtual hooks were not fully installed. "
                    "StatusComponent=%p Character=%p\n",
                    statusComponent,
                    reactionCharacter);
            }
        }

        const EBlindnessSource_104 blindnessSource =
            (statusID == ActiveIds::Status_Darkness)
            ? ResolveBlindnessSourceFromRawContext_104()
            : EBlindnessSource_104::Unknown;

        FBlindnessSourceScope_104 blindnessScope(blindnessSource);
        Native_AddStatus_104(statusComponent, statusID);

        if (reactionGuard.Previous.Active == false &&
            g_ReactionTLS_104.Active &&
            g_ReactionTLS_104.AllowStrongControl &&
            !g_ReactionTLS_104.ReactionRecorded) {
            ModLog(
                "[ElementalSystemExpanded] REACTION NOTE: %s status completed "
                "without reaching a native action gate; reaction ICD was not consumed.\n",
                StrongReactionName_104(g_ReactionTLS_104.Reaction));
        }
    }

    void* status = nullptr;
    float oldDuration = 0.0f;
    float oldTimer = 0.0f;

    if (!FindStatusInstance_104(
        statusComponent, statusID, &status, &oldDuration, &oldTimer)) {
        ShipLog_104(
            "[ElementalSystemExpanded] ERROR: AddStatus returned but no status "
            "instance was found. StatusID=%u.\n",
            static_cast<unsigned>(statusID));
        return false;
    }

    if (statusID == ActiveIds::Status_Freeze) {
        if (blockedFreezePersistent) {
            if (!InstallDynamicFreezeVirtualHooks_104(
                reactionCharacter,
                status)) {
                ShipLog_104(
                    "[ElementalSystemExpanded] ERROR: Freeze persistent "
                    "native gate installation failed. Instance=%p Character=%p\n",
                    status,
                    reactionCharacter);
            }
        }
        else {
            RemoveBlockedFreezeRuntime_104(status);
        }
    }

    float desiredDuration = 0.0f;
    if (statusID == ActiveIds::Status_Freeze && freezeHadWetness) {
        // Wet -> Freeze is a reaction, not an exchange carryover.
        desiredDuration = kWetFreezeDuration_104;
    }
    else {
        const float statusCap = MaxDurationForStatus_104(statusID);

        if (std::isfinite(durationOverride) && durationOverride > 0.0f) {
            // Cross-element carryover may contain more exchange budget than
            // the incoming status is allowed to retain. Darkness/Light are
            // therefore clamped to 3 seconds while ordinary statuses cap at 12.
            desiredDuration = (std::min)(
                statusCap,
                durationOverride);
        }
        else {
            // Initial application uses the gauge budget, then applies the
            // per-status cap. Darkness/Light blindness therefore remain
            // 3 seconds maximum for both 1U and 2U hits.
            desiredDuration = (std::min)(
                statusCap,
                GaugeDurationForUnits_104(units));
        }
    }

    const bool changed = SetStatusDuration_104(status, desiredDuration);
    if (changed) {
        RememberElementalStatusProvenance_104(
            statusComponent,
            status,
            statusID,
            sourceElement);
    }

    if (statusID == ActiveIds::Status_Freeze &&
        blockedFreezePersistent) {
        const bool removedIce =
            RemoveBlockedFreezeIceCondition_104(
                reactionCharacter);

        ModLog(
            "[ElementalSystemExpanded] ICE VFX CLEANUP: "
            "BlockedFreeze Instance=%p Character=%p "
            "Removal=%s\n",
            status,
            reactionCharacter,
            removedIce ? "REMOVED_OR_ENDED" : "NOT_REMOVED");
    }

    ModLog(
        "[ElementalSystemExpanded] CUSTOM STATUS: StatusID=%u (%s) Units=%u "
        "Instance=%p Duration %.3f->%.3f Timer %.3f->%.3f Result=%s%s\n",
        static_cast<unsigned>(statusID),
        StatusIDName_104(statusID),
        static_cast<unsigned>(units),
        status,
        oldDuration,
        desiredDuration,
        oldTimer,
        desiredDuration,
        changed ? "OK" : "FAILED",
        (statusID == ActiveIds::Status_Freeze && freezeHadWetness)
        ? " Mode=WET_FREEZE_FIXED"
        : ((std::isfinite(durationOverride) && durationOverride > 0.0f)
            ? " Mode=EXCHANGE_CARRYOVER"
            : ((statusID == ActiveIds::Status_Freeze)
                ? " Mode=DRY_FREEZE_GAUGE"
                : " Mode=GAUGE_DURATION")));

    return changed;
}

// ---------------------------------------------------------
// Native hooks
// ---------------------------------------------------------
static void __fastcall Detour_AddElementStatusAdditionalValue_OneType_104(
    void* damageReaction,
    uint8_t effect,
    float vanillaValue)
{
    // All non-elemental effects remain completely vanilla.
    if (!IsTrackedElementalEffect_104(effect)) {
        if (Original_AddElementStatusAdditionalValue_OneType_104) {
            Original_AddElementStatusAdditionalValue_OneType_104(
                damageReaction, effect, vanillaValue);
        }
        return;
    }

    const uint8_t statusID = ElementalEffectToStatusID_104(effect);
    if (!statusID) {
        if (Original_AddElementStatusAdditionalValue_OneType_104) {
            Original_AddElementStatusAdditionalValue_OneType_104(
                damageReaction, effect, vanillaValue);
        }
        return;
    }

    void* character = ResolveOwningPalCharacter_104(damageReaction);
    void* statusComponent = ResolveStatusComponent_104(damageReaction);

    if (character &&
        ShouldBlockLightBlindOnNeutralPal_104(
            character,
            effect)) {
        ModLog(
            "[ElementalSystemExpanded] LIGHT IMMUNITY: "
            "Character=%p has Normal/Light element; "
            "Light-sourced Darkness carrier rejected before ICD/AddStatus.\n",
            character);
        return;
    }

    if (!character || !statusComponent || !Native_AddStatus_104) {
        ShipLog_104(
            "[ElementalSystemExpanded] WARNING: CUSTOM FALLBACK: DamageReaction=%p "
            "Effect=%u (%s) VanillaValue=%.6f could not resolve status component; "
            "calling vanilla.\n",
            damageReaction,
            static_cast<unsigned>(effect),
            ElementalEffectName_104(effect),
            vanillaValue);

        if (Original_AddElementStatusAdditionalValue_OneType_104) {
            Original_AddElementStatusAdditionalValue_OneType_104(
                damageReaction, effect, vanillaValue);
        }
        return;
    }

    // Primary source: original integer EffectValue1/2 from FPalDamageInfo.
    // Direct callers outside the verified raw-damage wrapper use a conservative
    // 1U fallback. In particular, sentinel/derived values such as 9999999 must
    // never be interpreted as a real 2U request.
    int32_t rawValue = 1;
    const bool haveRawValue = GetRawUnitsForEffect_104(effect, &rawValue);

    // BP_Status_WetFreeze worker signature observed with Elgrove Cryst:
    // a non-elemental hit directly injects Freeze buildup=9999999 outside the
    // verified FPalDamageInfo elemental-damage caller. Kill that legacy worker
    // path in addition to blacklisting status IDs 59/60.
    //
    // Ordinary Freeze attacks arrive with raw damage context and are untouched.
    if (effect == ActiveIds::Effect_Freeze &&
        !haveRawValue &&
        vanillaValue >= 9999990.0f) {
        ModLog(
            "[ElementalSystemExpanded] VANILLA WETFREEZE WORKER BLOCK: "
            "DamageReaction=%p Effect=6 Freeze VanillaValue=%.3f "
            "(no raw damage context) rejected before ICD/status apply.\n",
            damageReaction,
            vanillaValue);
        return;
    }

    uint8_t units = 1;
    if (haveRawValue) {
        units = UnitsFromRawValue_104(rawValue);
    }
    else {
        units = 1;
        ShipLog_104(
            "[ElementalSystemExpanded] WARNING: no raw EffectValue context for "
            "Effect=%u (%s); conservative fallback units=1 from VanillaValue=%.6f.\n",
            static_cast<unsigned>(effect),
            ElementalEffectName_104(effect),
            vanillaValue);
    }

    const uint8_t incomingElement = ResolveIncomingElement_104(effect);

    uint8_t previousBlockedHits = 0;
    double elapsed = 0.0;

    const bool allowed = ElementICDAllows_104(
        damageReaction,
        effect,
        &previousBlockedHits,
        &elapsed);

    if (!allowed) {
        ModLog(
            "[ElementalSystemExpanded] ICD BLOCK: DamageReaction=%p "
            "Effect=%u (%s) Units=%u Elapsed=%.3f BlockedHits=%u/2\n",
            damageReaction,
            static_cast<unsigned>(effect),
            ElementalEffectName_104(effect),
            static_cast<unsigned>(units),
            elapsed,
            static_cast<unsigned>(previousBlockedHits));

        // Do not call vanilla. The elemental buildup registration is discarded.
        return;
    }

    ModLog(
        "[ElementalSystemExpanded] ICD ALLOW: DamageReaction=%p "
        "Effect=%u (%s) RawValue=%d Units=%u Element=%u Elapsed=%.3f\n",
        damageReaction,
        static_cast<unsigned>(effect),
        ElementalEffectName_104(effect),
        rawValue,
        static_cast<unsigned>(units),
        static_cast<unsigned>(incomingElement),
        elapsed);

    FCurrentElementalStatus_104 current{};
    const ECurrentElementalStatusQuery_104 currentQuery =
        QueryCurrentElementalStatus_104(
            statusComponent,
            &current);

    if (currentQuery == ECurrentElementalStatusQuery_104::Failed) {
        ShipLog_104(
            "[ElementalSystemExpanded] WARNING: could not safely inspect the current "
            "elemental status; incoming StatusID=%u was discarded rather than risking "
            "an unintended overwrite.\n",
            static_cast<unsigned>(statusID));
        return;
    }

    if (currentQuery == ECurrentElementalStatusQuery_104::Found) {
        const uint8_t currentElement =
            ResolveCurrentElementSource_104(
                statusComponent,
                current);

        // Preserve the established elemental reactions. Wet -> Freeze and
        // Wet -> Electrical bypass exchange/erosion completely and flow into
        // the original reaction path below. Wet Freeze therefore remains the
        // fixed 2 s reaction and both strong reactions keep their 14 s ICD.
        const bool wetStrongReaction =
            IsWetStrongReactionPair_104(current, statusID);

        if (wetStrongReaction) {
            ModLog(
                "[ElementalSystemExpanded] STATUS EXCHANGE BYPASS: Wetness + %s "
                "uses the established strong-reaction path.\n",
                StatusIDName_104(statusID));
        }
        else if (currentElement == incomingElement) {
            // Same-element hits extend the remaining status time by the
            // incoming gauge budget, subject to the status-specific cap:
            // 3 s for Darkness/Light blindness, 12 s for ordinary statuses.
            const float addition = GaugeDurationForUnits_104(units);
            const float statusCap =
                MaxDurationForStatus_104(current.StatusID);
            const float refreshed =
                (std::min)(
                    statusCap,
                    current.Timer + addition);

            if (!SetStatusDuration_104(current.Instance, refreshed)) {
                ShipLog_104(
                    "[ElementalSystemExpanded] WARNING: same-element status "
                    "extension failed. CurrentStatusID=%u IncomingStatusID=%u.\n",
                    static_cast<unsigned>(current.StatusID),
                    static_cast<unsigned>(statusID));
                return;
            }

            RememberElementalStatusProvenance_104(
                statusComponent,
                current.Instance,
                current.StatusID,
                currentElement);

            ModLog(
                "[ElementalSystemExpanded] STATUS EXTEND: StatusID=%u (%s) "
                "Element=%u Units=%u Addition=%.3f Duration %.3f->%.3f "
                "Timer %.3f->%.3f Cap=%.3f.\n",
                static_cast<unsigned>(current.StatusID),
                StatusIDName_104(current.StatusID),
                static_cast<unsigned>(currentElement),
                static_cast<unsigned>(units),
                addition,
                current.Duration,
                refreshed,
                current.Timer,
                refreshed,
                statusCap);

            ElementICDRecordSuccess_104(damageReaction, effect);
            return;
        }
        else {
            const float incomingBudget =
                GaugeDurationForUnits_104(units);
            const float semanticRemaining =
                (std::max)(0.0f, current.Timer - incomingBudget);
            const float carryover =
                (std::max)(0.0f, incomingBudget - current.Timer);
            float storedRemaining = 0.0f;

            if (!SetStatusRemainingFromErosion_104(
                current.Instance,
                semanticRemaining,
                &storedRemaining)) {
                ShipLog_104(
                    "[ElementalSystemExpanded] WARNING: cross-element status erosion "
                    "failed. CurrentStatusID=%u IncomingStatusID=%u; incoming status "
                    "was discarded to preserve the existing status.\n",
                    static_cast<unsigned>(current.StatusID),
                    static_cast<unsigned>(statusID));
                return;
            }

            if (semanticRemaining <= 0.0f) {
                // Logical lifetime is zero now. The stored 0.001 s sentinel is
                // intentionally ignored by all mod status queries and exists
                // only so native TickStatus can observe positive->zero and run
                // its normal teardown on the next frame.
                ClearElementalStatusProvenance_104(statusComponent);
            }

            const bool carryoverApplies =
                carryover >= kMinimumExchangeCarryover_104;

            ModLog(
                "[ElementalSystemExpanded] STATUS EXCHANGE: CurrentStatusID=%u (%s) "
                "CurrentElement=%u IncomingStatusID=%u (%s) IncomingElement=%u "
                "Units=%u Budget=%.3f Duration %.3f->%.3f Timer %.3f->%.3f "
                "StoredTimer=%.3f Carryover=%.3f Threshold=%.3f ApplyIncoming=%s.\n",
                static_cast<unsigned>(current.StatusID),
                StatusIDName_104(current.StatusID),
                static_cast<unsigned>(currentElement),
                static_cast<unsigned>(statusID),
                StatusIDName_104(statusID),
                static_cast<unsigned>(incomingElement),
                static_cast<unsigned>(units),
                incomingBudget,
                current.Duration,
                semanticRemaining,
                current.Timer,
                semanticRemaining,
                storedRemaining,
                carryover,
                kMinimumExchangeCarryover_104,
                carryoverApplies ? "YES" : "NO");

            if (carryoverApplies) {
                const bool applied =
                    ApplyStatusWithDuration_104(
                        damageReaction,
                        statusComponent,
                        statusID,
                        units,
                        incomingElement,
                        carryover);

                // The hit already successfully eroded the previous status, so
                // it consumes its ICD even if carryover application itself
                // unexpectedly fails.
                ElementICDRecordSuccess_104(damageReaction, effect);

                if (!applied) {
                    ShipLog_104(
                        "[ElementalSystemExpanded] WARNING: status exchange "
                        "eroded StatusID=%u but failed to apply incoming "
                        "StatusID=%u with Carryover=%.3f.\n",
                        static_cast<unsigned>(current.StatusID),
                        static_cast<unsigned>(statusID),
                        carryover);
                }
                return;
            }

            // Erosion without enough carryover is itself the successful result
            // of this elemental hit.
            ElementICDRecordSuccess_104(damageReaction, effect);
            return;
        }
    }

    if (ApplyStatusWithDuration_104(
        damageReaction,
        statusComponent,
        statusID,
        units,
        incomingElement,
        -1.0f)) {
        ElementICDRecordSuccess_104(damageReaction, effect);
    }
    else {
        ShipLog_104(
            "[ElementalSystemExpanded] WARNING: custom StatusID=%u application "
            "failed; ICD state was not marked successful.\n",
            static_cast<unsigned>(statusID));
    }

    // Intentionally do not call Original_AddElementStatusAdditionalValue_OneType_104
    // for supported elemental effects. Vanilla buildup is completely replaced.
}

// Native AddStatus policy hook. Most statuses pass through unchanged; the two
// obsolete vanilla WetFreeze status IDs are intentionally rejected.
static void __fastcall Detour_NativeAddStatus_104(
    void* statusComponent,
    uint8_t statusID)
{
    // Kill the vanilla WetFreeze subsystem at apply level.
    // 59 = PlayerInflictEffect_AttackWet_ApplyFreeze
    // 60 = PlayerInflictEffect_AttackWet_ApplyFreeze_Resist
    if (statusID == ActiveIds::Status_VanillaWetFreeze ||
        statusID == ActiveIds::Status_VanillaWetFreezeResist) {
        ModLog(
            "[ElementalSystemExpanded] VANILLA WETFREEZE BLOCK: "
            "Component=%p StatusID=%u (%s) rejected at native AddStatus.\n",
            statusComponent,
            static_cast<unsigned>(statusID),
            statusID == ActiveIds::Status_VanillaWetFreeze
            ? "PlayerInflictEffect_AttackWet_ApplyFreeze"
            : "PlayerInflictEffect_AttackWet_ApplyFreeze_Resist");
        return;
    }

    const bool tracked =
        statusID == ActiveIds::Status_Burn ||
        statusID == ActiveIds::Status_Wetness ||
        statusID == ActiveIds::Status_Freeze ||
        statusID == ActiveIds::Status_Electrical ||
        statusID == ActiveIds::Status_Muddy ||
        statusID == ActiveIds::Status_IvyCling ||
        statusID == ActiveIds::Status_Darkness;

    if (tracked) {
        ModLog(
            "[ElementalSystemExpanded] AddStatus CALL: Component=%p StatusID=%u (%s)\n",
            statusComponent,
            static_cast<unsigned>(statusID),
            StatusIDName_104(statusID));
    }

    if (Original_NativeAddStatus_104) {
        Original_NativeAddStatus_104(statusComponent, statusID);
    }
}

// ---------------------------------------------------------
// Persistent runtime durability snapshot
//
// Runtime discoveries happen on gameplay paths, but gameplay threads NEVER
// perform disk I/O. A dedicated low-priority worker persists the current
// diagnostic state beside the DLL. It writes a temporary file, flushes it,
// then atomically replaces the public snapshot. Therefore abrupt process exit
// does not depend on uninstall_mod(), and readers should see either the prior
// complete snapshot or the new complete snapshot rather than a partial file.
// ---------------------------------------------------------
static void AppendSnapshotFormat_104(
    std::string& out,
    const char* format,
    ...)
{
    char buffer[1024]{};
    va_list args;
    va_start(args, format);
    const int n = vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    if (n <= 0)
        return;

    if (static_cast<size_t>(n) < sizeof(buffer)) {
        out.append(buffer, static_cast<size_t>(n));
        return;
    }

    // Snapshot lines are intentionally short. If a future line exceeds the
    // fixed formatting buffer, keep the diagnostic file valid and explicit.
    out += "<snapshot-line-truncated>\n";
}

static bool WriteWholeFileDurable_104(
    const char* finalPath,
    const std::string& data)
{
    if (!finalPath || !*finalPath)
        return false;

    std::string tempPath(finalPath);
    tempPath += ".tmp";

    HANDLE file = CreateFileA(
        tempPath.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
        nullptr);

    if (file == INVALID_HANDLE_VALUE)
        return false;

    const char* cursor = data.data();
    size_t remaining = data.size();
    bool ok = true;

    while (remaining != 0) {
        const DWORD chunk = static_cast<DWORD>(
            (std::min)(remaining, static_cast<size_t>(1u << 20)));
        DWORD written = 0;
        if (!WriteFile(file, cursor, chunk, &written, nullptr) ||
            written != chunk) {
            ok = false;
            break;
        }
        cursor += written;
        remaining -= written;
    }

    if (ok && !FlushFileBuffers(file))
        ok = false;

    CloseHandle(file);

    if (ok) {
        if (!MoveFileExA(
            tempPath.c_str(),
            finalPath,
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            ok = false;
        }
    }

    if (!ok)
        DeleteFileA(tempPath.c_str());

    return ok;
}

static bool WriteRuntimeSnapshot_104(const char* reason)
{
    if (!g_DebugDiagnosticsEnabled_104)
        return false;

    uintptr_t outerOffset = 0;
    bool outerLogged = false;
    AcquireSRWLockShared(&g_UObjectOuterOffsetLock_104);
    outerOffset = g_RuntimeUObjectOuterOffset_104;
    outerLogged = g_RuntimeUObjectOuterLogged_104;
    ReleaseSRWLockShared(&g_UObjectOuterOffsetLock_104);

    ptrdiff_t startLocationOffset = 0;
    bool startLocationUsesFloat = false;
    AcquireSRWLockShared(&g_FreezeStartLocationLocatorLock_104);
    startLocationOffset = g_FreezeStartLocationOffset_104;
    startLocationUsesFloat = g_FreezeStartLocationUsesFloat_104;
    ReleaseSRWLockShared(&g_FreezeStartLocationLocatorLock_104);

    bool rootCrossCheckSeen = false;
    bool rootCrossCheckPassed = false;
    double rootCrossCheckDistSq = -1.0;
    bool rootHiddenFallbackUsed = false;
    AcquireSRWLockShared(&g_RootReferenceDiagnosticsLock_104);
    rootCrossCheckSeen = g_RootReferenceValidationLogged_104;
    rootCrossCheckPassed = g_RootReferenceCrossCheckPassed_104;
    rootCrossCheckDistSq = g_RootReferenceCrossCheckDistSq_104;
    rootHiddenFallbackUsed = g_RootReferenceUsedHiddenFallback_104;
    ReleaseSRWLockShared(&g_RootReferenceDiagnosticsLock_104);

    size_t blockedFreezeCount = 0;
    AcquireSRWLockShared(&g_BlockedFreezeRuntimeLock_104);
    blockedFreezeCount = g_BlockedFreezeRuntime_104.size();
    ReleaseSRWLockShared(&g_BlockedFreezeRuntimeLock_104);

    const LONG statusPasses =
        InterlockedCompareExchange(&g_StatusArrayValidationPasses_104, 0, 0);
    const LONG statusFailures =
        InterlockedCompareExchange(&g_StatusArrayValidationFailures_104, 0, 0);
    const LONG vfxPasses =
        InterlockedCompareExchange(&g_VfxArrayValidationPasses_104, 0, 0);
    const LONG vfxFailures =
        InterlockedCompareExchange(&g_VfxArrayValidationFailures_104, 0, 0);

    const LONG sequence =
        InterlockedIncrement(&g_RuntimeSnapshotWrites_104);

    SYSTEMTIME utc{};
    GetSystemTime(&utc);

    std::string output;
    output.reserve(4096);

    AppendSnapshotFormat_104(
        output,
        "ElementalSystemExpanded runtime durability snapshot\n"
        "BaselineSemantics=Stage4.2-core+Stage6.6.2-status-exchange\n"
        "InfrastructureStage=6.6.3\n"
        "PersistenceMode=low-priority-worker+atomic-replace\n"
        "Reason=%s\n"
        "Sequence=%ld\n"
        "WrittenUTC=%04u-%02u-%02uT%02u:%02u:%02u.%03uZ\n"
        "ProcessUptimeMs=%llu\n",
        reason ? reason : "unspecified",
        static_cast<long>(sequence),
        static_cast<unsigned>(utc.wYear),
        static_cast<unsigned>(utc.wMonth),
        static_cast<unsigned>(utc.wDay),
        static_cast<unsigned>(utc.wHour),
        static_cast<unsigned>(utc.wMinute),
        static_cast<unsigned>(utc.wSecond),
        static_cast<unsigned>(utc.wMilliseconds),
        static_cast<unsigned long long>(GetTickCount64()));

    AppendSnapshotFormat_104(
        output,
        "Fingerprint.TimeDateStamp=0x%08lX\n"
        "Fingerprint.SizeOfImage=0x%08lX\n"
        "Fingerprint.TextHash=0x%016llX\n"
        "BuildProfileCache=%s\n",
        static_cast<unsigned long>(g_CurrentFingerprint_104.TimeDateStamp),
        static_cast<unsigned long>(g_CurrentFingerprint_104.SizeOfImage),
        static_cast<unsigned long long>(g_CurrentFingerprint_104.TextHash),
        g_ProfileCacheStatus_104.c_str());

    AppendSnapshotFormat_104(
        output,
        "\n[Deferred runtime discoveries]\n"
        "UObjectOuterLink.Discovered=%s\n"
        "UObjectOuterLink.Offset=0x%llX\n"
        "UObjectOuterLink.Logged=%s\n"
        "FreezeStartLocation.Discovered=%s\n"
        "FreezeStartLocation.Offset=0x%llX\n"
        "FreezeStartLocation.Format=%s\n",
        outerOffset ? "YES" : "NO",
        static_cast<unsigned long long>(outerOffset),
        outerLogged ? "YES" : "NO",
        startLocationOffset ? "YES" : "NO",
        static_cast<unsigned long long>(
            startLocationOffset > 0 ? startLocationOffset : 0),
        startLocationOffset
        ? (startLocationUsesFloat ? "float3" : "double3")
        : "unknown");

    AppendSnapshotFormat_104(
        output,
        "RootReference.CrossCheckSeen=%s\n"
        "RootReference.CrossCheckPassed=%s\n"
        "RootReference.CrossCheckDistSq=%.9f\n"
        "RootReference.Hidden104FallbackUsed=%s\n",
        rootCrossCheckSeen ? "YES" : "NO",
        rootCrossCheckPassed ? "YES" : "NO",
        rootCrossCheckDistSq,
        rootHiddenFallbackUsed ? "YES" : "NO");

#if ESE_HAS_GENERATED_SCENE_RELATIVE_LOCATION
    AppendSnapshotFormat_104(
        output,
        "RootReference.RelativeLocationOffset=0x%llX\n"
        "RootReference.AttachParentAvailable=%s\n",
        static_cast<unsigned long long>(
            ActiveLayout::SceneComponent_RelativeLocation),
        ActiveLayout::HasGeneratedSceneAttachParent ? "YES" : "NO");
    if (ActiveLayout::HasGeneratedSceneAttachParent) {
        AppendSnapshotFormat_104(
            output,
            "RootReference.AttachParentOffset=0x%llX\n",
            static_cast<unsigned long long>(
                ActiveLayout::SceneComponent_AttachParent));
    }
#endif

    AppendSnapshotFormat_104(
        output,
        "\n[Container ABI guards]\n"
        "StatusArray.ValidationPasses=%ld\n"
        "StatusArray.ValidationFailures=%ld\n"
        "VisualEffectArray.ValidationPasses=%ld\n"
        "VisualEffectArray.ValidationFailures=%ld\n"
        "RawTArray.Invariant=0<=Num<=Max; Max<=4096; Data aligned/readable; active elements UObject-like\n",
        static_cast<long>(statusPasses),
        static_cast<long>(statusFailures),
        static_cast<long>(vfxPasses),
        static_cast<long>(vfxFailures));

    AppendSnapshotFormat_104(
        output,
        "\n[Runtime state]\n"
        "BlockedFreezeEntriesAtSnapshot=%llu\n"
        "ResolvedVSlot.SetComponentTickEnabled=0x%llX\n"
        "ResolvedVSlot.StopAnimMontage=0x%llX\n"
        "ResolvedVSlot.StatusTick=0x%llX\n"
        "SnapshotWriteFailuresBeforeThisAttempt=%ld\n",
        static_cast<unsigned long long>(blockedFreezeCount),
        static_cast<unsigned long long>(
            g_VSlot_SetComponentTickEnabled_104),
        static_cast<unsigned long long>(
            g_VSlot_StopAnimMontage_104),
        static_cast<unsigned long long>(
            g_VSlot_StatusTick_104),
        static_cast<long>(
            InterlockedCompareExchange(
                &g_RuntimeSnapshotWriteFailures_104, 0, 0)));

    const bool ok = WriteWholeFileDurable_104(
        ArtifactPathOrFallback_104(
            g_RuntimeSnapshotPath_104,
            "ElementalSystemExpanded_runtime_snapshot.txt"),
        output);

    if (!ok) {
        InterlockedIncrement(&g_RuntimeSnapshotWriteFailures_104);
        return false;
    }

    return true;
}

static void MarkRuntimeSnapshotDirty_104()
{
    if (!g_DebugDiagnosticsEnabled_104)
        return;

    InterlockedExchange(&g_RuntimeSnapshotDirty_104, 1);

    HANDLE wake = g_RuntimeSnapshotWakeEvent_104;
    if (wake)
        SetEvent(wake);
}

static DWORD WINAPI RuntimeSnapshotWorker_104(LPVOID)
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

    // Create a baseline snapshot immediately. It will be atomically replaced
    // as soon as deferred runtime evidence appears.
    WriteRuntimeSnapshot_104("worker-start");
    InterlockedExchange(&g_RuntimeSnapshotDirty_104, 0);

    HANDLE waits[2] = {
        g_RuntimeSnapshotStopEvent_104,
        g_RuntimeSnapshotWakeEvent_104
    };

    for (;;) {
        const DWORD wait = WaitForMultipleObjects(
            2,
            waits,
            FALSE,
            5000);

        if (wait == WAIT_OBJECT_0) {
            // Cleanup-time flush is opportunistic only. Persistence does not
            // depend on this branch ever running.
            WriteRuntimeSnapshot_104("worker-stop");
            return 0;
        }

        const bool explicitlyDirty =
            InterlockedExchange(&g_RuntimeSnapshotDirty_104, 0) != 0;

        if (wait == WAIT_OBJECT_0 + 1) {
            WriteRuntimeSnapshot_104(
                explicitlyDirty ? "runtime-milestone" : "worker-wake");
            continue;
        }

        if (wait == WAIT_TIMEOUT) {
            // Periodic persistence captures monotonically changing guard
            // counters even when there has been no discrete milestone.
            WriteRuntimeSnapshot_104(
                explicitlyDirty ? "periodic-dirty" : "periodic");
            continue;
        }

        // Unexpected wait failure: retry after a short pause instead of
        // terminating the diagnostics thread permanently.
        Sleep(250);
    }
}

static bool StartRuntimeSnapshotWorker_104()
{
    if (!g_DebugDiagnosticsEnabled_104)
        return true;

    if (g_RuntimeSnapshotThread_104)
        return true;

    g_RuntimeSnapshotStopEvent_104 =
        CreateEventA(nullptr, TRUE, FALSE, nullptr);
    g_RuntimeSnapshotWakeEvent_104 =
        CreateEventA(nullptr, FALSE, FALSE, nullptr);

    if (!g_RuntimeSnapshotStopEvent_104 ||
        !g_RuntimeSnapshotWakeEvent_104) {
        if (g_RuntimeSnapshotStopEvent_104) {
            CloseHandle(g_RuntimeSnapshotStopEvent_104);
            g_RuntimeSnapshotStopEvent_104 = nullptr;
        }
        if (g_RuntimeSnapshotWakeEvent_104) {
            CloseHandle(g_RuntimeSnapshotWakeEvent_104);
            g_RuntimeSnapshotWakeEvent_104 = nullptr;
        }
        return false;
    }

    DWORD threadId = 0;
    g_RuntimeSnapshotThread_104 =
        CreateThread(
            nullptr,
            0,
            RuntimeSnapshotWorker_104,
            nullptr,
            0,
            &threadId);

    if (!g_RuntimeSnapshotThread_104) {
        CloseHandle(g_RuntimeSnapshotStopEvent_104);
        CloseHandle(g_RuntimeSnapshotWakeEvent_104);
        g_RuntimeSnapshotStopEvent_104 = nullptr;
        g_RuntimeSnapshotWakeEvent_104 = nullptr;
        return false;
    }

    ModLog(
        "[ElementalSystemExpanded] Persistent diagnostics worker started "
        "(periodic=5000ms, atomic-replace, threadId=%lu).\n",
        static_cast<unsigned long>(threadId));
    return true;
}

static void StopRuntimeSnapshotWorker_104()
{
    HANDLE thread = g_RuntimeSnapshotThread_104;
    if (!thread)
        return;

    if (g_RuntimeSnapshotStopEvent_104)
        SetEvent(g_RuntimeSnapshotStopEvent_104);

    const DWORD waited = WaitForSingleObject(thread, 5000);
    if (waited != WAIT_OBJECT_0) {
        ShipLog_104(
            "[ElementalSystemExpanded] WARNING: diagnostics worker did not "
            "stop within 5s; handles left intact.\n");
        return;
    }

    CloseHandle(thread);
    g_RuntimeSnapshotThread_104 = nullptr;

    if (g_RuntimeSnapshotStopEvent_104) {
        CloseHandle(g_RuntimeSnapshotStopEvent_104);
        g_RuntimeSnapshotStopEvent_104 = nullptr;
    }
    if (g_RuntimeSnapshotWakeEvent_104) {
        CloseHandle(g_RuntimeSnapshotWakeEvent_104);
        g_RuntimeSnapshotWakeEvent_104 = nullptr;
    }
}

// ---------------------------------------------------------
// Entry point
// ---------------------------------------------------------
DWORD WINAPI MainThread(LPVOID lpParam) {
    (void)lpParam;

    const bool artifactPathsReady = InitializeArtifactPaths_104();
    InitializeDebugDiagnosticsMode_104();

    ShipLog_104(
        "[ElementalSystemExpanded] Stage 6.6.3 status-exchange shipping core starting. "
        "Diagnostics=%s%s\n",
        g_DebugDiagnosticsEnabled_104 ? "ON" : "OFF",
        ESE_DEV_FORCE_UNKNOWN_BOOTSTRAP ? " DevForceUnknown=YES" : "");

    if (!artifactPathsReady) {
        ShipLog_104(
            "[ElementalSystemExpanded] ERROR: DLL artifact path resolution failed; "
            "refusing to use current-working-directory fallbacks. Mod not started.\n");
        return 1;
    }

    ModLog("[ElementalSystemExpanded] Artifact directory: %s (%s)\n",
        g_ArtifactDirectory_104.c_str(),
        g_ArtifactPathStatus_104.c_str());

    if (MH_Initialize() != MH_OK) {
        ShipLog_104("[ElementalSystemExpanded] ERROR: MinHook failed to initialize.\n");
        return 1;
    }

    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA(NULL));
    if (!base) {
        ShipLog_104("[ElementalSystemExpanded] ERROR: Failed to resolve Palworld module base.\n");
        MH_Uninitialize();
        return 1;
    }

    g_ModuleBase_104 = base;
    FBuildFingerprint_104 fingerprint{};
    if (!QueryBuildFingerprint_104(base, &fingerprint)) {
        ShipLog_104(
            "[ElementalSystemExpanded] ERROR: Failed to fingerprint Palworld executable; "
            "refusing to install hooks.\n");
        MH_Uninitialize();
        return 1;
    }

    ModLog(
        "[ElementalSystemExpanded] Build fingerprint: TimeDateStamp=0x%08lX "
        "SizeOfImage=0x%08lX .textHash=0x%016llX\n",
        static_cast<unsigned long>(fingerprint.TimeDateStamp),
        static_cast<unsigned long>(fingerprint.SizeOfImage),
        static_cast<unsigned long long>(fingerprint.TextHash));

    g_CurrentFingerprint_104 = fingerprint;
    InitializeUpdateDiagnostics_104(fingerprint);
    LoadBuildProfileCache_104(fingerprint);

    ModLog(
        "[ElementalSystemExpanded] Build identity: Known104=%s ProfileCache=%s\n",
        IsKnown104Fingerprint_104(fingerprint) ? "YES" : "NO",
        g_ProfileCacheStatus_104.c_str());

    auto Fatal = [&](const char* message) -> DWORD {
        ShipLog_104("[ElementalSystemExpanded] ERROR: %s\n", message);
        // Normal shipping mode keeps diagnostics silent, but a resolver/hook
        // initialization failure is actionable and warrants one-shot reports.
        g_ForceFailureArtifacts_104 = true;
        WriteMigrationReport_104(fingerprint);
        WriteResolverReport_104(fingerprint);
        g_ForceFailureArtifacts_104 = false;
        MH_DisableHook(MH_ALL_HOOKS);
        MH_Uninitialize();
        return 1;
        };

    if (!IsKnown104Fingerprint_104(fingerprint) && !ESE_HAS_GENERATED_PAL_LAYOUT) {
        return Fatal(
            "unknown game build without a generated Pal layout header; "
            "refusing to use embedded 1.0.4 Pal offsets");
    }

    std::string layoutBindingDetail;
    const bool layoutFingerprintBound = GeneratedLayoutFingerprintAvailable_104();
    const bool layoutFingerprintMatch =
        GeneratedLayoutFingerprintMatches_104(fingerprint, &layoutBindingDetail);

    bool generatedLayoutBindingOK = true;
    if (ESE_HAS_GENERATED_PAL_LAYOUT) {
        // A bound generated header must always match the running executable. Legacy
        // unbound headers are accepted only for the exact preserved 1.0.4
        // oracle where every critical layout value is checked below.
        generatedLayoutBindingOK = layoutFingerprintBound
            ? layoutFingerprintMatch
            : IsKnown104Fingerprint_104(fingerprint);
    }
    else {
        generatedLayoutBindingOK = IsKnown104Fingerprint_104(fingerprint);
    }

    if (generatedLayoutBindingOK && !IsKnown104Fingerprint_104(fingerprint)) {
#if ESE_HAS_GENERATED_ENGINE_OPTIONALS
        if (!EseGeneratedPalLayout::Has_Actor_RootComponent) {
            generatedLayoutBindingOK = false;
            layoutBindingDetail += "; missing generated AActor::RootComponent";
        }
#else
        generatedLayoutBindingOK = false;
        layoutBindingDetail += "; no generated engine optionals";
#endif
#if ESE_HAS_GENERATED_SCENE_RELATIVE_LOCATION
        if (!EseGeneratedPalLayout::Has_SceneComponent_RelativeLocation) {
            generatedLayoutBindingOK = false;
            layoutBindingDetail += "; missing generated USceneComponent::RelativeLocation";
        }
        if (!EseGeneratedPalLayout::Has_SceneComponent_AttachParent) {
            generatedLayoutBindingOK = false;
            layoutBindingDetail += "; missing generated USceneComponent::AttachParent";
        }
#else
        generatedLayoutBindingOK = false;
        layoutBindingDetail += "; no generated scene-component layout";
#endif

#if !ESE_HAS_GENERATED_LIGHT_TARGET_LAYOUT
        generatedLayoutBindingOK = false;
        layoutBindingDetail +=
            "; missing v8 generated Light-immunity target layout "
            "(CharacterParameterComponent/StaticCharacterParameterComponent/"
            "ElementType1/ElementType2/IsPal)";
#endif
    }

    RecordResolver_104(
        "CAPABILITY.GeneratedLayoutBinding",
        layoutFingerprintBound ? "generated-header-fingerprint" : "known-build-oracle-only",
        generatedLayoutBindingOK
        ? (layoutFingerprintBound
            ? EResolveConfidence_104::Exact
            : EResolveConfidence_104::Exact)
        : EResolveConfidence_104::Failed,
        0,
        generatedLayoutBindingOK
        ? (layoutFingerprintBound
            ? layoutBindingDetail.c_str()
            : "legacy/unbound header accepted only because runtime is exact known 1.0.4 oracle")
        : layoutBindingDetail.c_str());

    if (!generatedLayoutBindingOK)
        return Fatal(
            "generated SDK layout header is stale/unbound for this unknown executable; "
            "regenerate v8 against the current CXXHeaderDump and Palworld-Win64-Shipping.exe; required engine/Light-immunity fields must be emitted");

    bool layoutProfileOK = true;
    if (IsKnown104Fingerprint_104(fingerprint)) {
        layoutProfileOK =
            ActiveLayout::Character_RootComponent == Known104::Offset::Character_RootComponent &&
            ActiveLayout::Character_CharacterParameterComponent == Known104::Offset::Character_CharacterParameterComponent &&
            ActiveLayout::Character_StaticCharacterParameterComponent == Known104::Offset::Character_StaticCharacterParameterComponent &&
            ActiveLayout::CharacterParameter_ElementType1 == Known104::Offset::CharacterParameter_ElementType1 &&
            ActiveLayout::CharacterParameter_ElementType2 == Known104::Offset::CharacterParameter_ElementType2 &&
            ActiveLayout::StaticCharacterParameter_IsPal == Known104::Offset::StaticCharacterParameter_IsPal &&
            ActiveLayout::DamageInfo_AttackElement == Known104::Offset::DamageInfo_AttackElement &&
            ActiveLayout::DamageInfo_EffectType1 == Known104::Offset::DamageInfo_EffectType1 &&
            ActiveLayout::DamageInfo_EffectValue1 == Known104::Offset::DamageInfo_EffectValue1 &&
            ActiveLayout::DamageInfo_EffectType2 == Known104::Offset::DamageInfo_EffectType2 &&
            ActiveLayout::DamageInfo_EffectValue2 == Known104::Offset::DamageInfo_EffectValue2 &&
            ActiveLayout::Character_DamageReactionComponent == Known104::Offset::Character_DamageReactionComponent &&
            ActiveLayout::Character_StatusComponent == Known104::Offset::Character_StatusComponent &&
            ActiveLayout::Character_VisualEffectComponent == Known104::Offset::Character_VisualEffectComponent &&
            ActiveLayout::StatusComponent_ExecutionStatusList == Known104::Offset::StatusComponent_ExecutionStatusList &&
            ActiveLayout::StatusBase_StatusID == Known104::Offset::StatusBase_StatusID &&
            ActiveLayout::StatusBase_Duration == Known104::Offset::StatusBase_Duration &&
            ActiveLayout::StatusBase_DurationTimer == Known104::Offset::StatusBase_DurationTimer &&
            ActiveLayout::StatusBase_NativeSize == Known104::Offset::StatusBase_NativeSize;
    }
    else {
        layoutProfileOK =
            ActiveLayout::Character_RootComponent >= 0x100 &&
            ActiveLayout::Character_RootComponent < 0x1000 &&
            (ActiveLayout::Character_RootComponent % sizeof(void*)) == 0 &&
            ActiveLayout::Character_CharacterParameterComponent >= 0x100 &&
            ActiveLayout::Character_CharacterParameterComponent < 0x2000 &&
            (ActiveLayout::Character_CharacterParameterComponent % sizeof(void*)) == 0 &&
            ActiveLayout::Character_StaticCharacterParameterComponent >= 0x100 &&
            ActiveLayout::Character_StaticCharacterParameterComponent < 0x2000 &&
            (ActiveLayout::Character_StaticCharacterParameterComponent % sizeof(void*)) == 0 &&
            ActiveLayout::Character_CharacterParameterComponent !=
            ActiveLayout::Character_StaticCharacterParameterComponent &&
            ActiveLayout::CharacterParameter_ElementType1 < 0x1000 &&
            ActiveLayout::CharacterParameter_ElementType2 < 0x1000 &&
            ActiveLayout::CharacterParameter_ElementType1 <
            ActiveLayout::CharacterParameter_ElementType2 &&
            ActiveLayout::StaticCharacterParameter_IsPal < 0x1000 &&
            ActiveLayout::Character_DamageReactionComponent >= 0x100 &&
            ActiveLayout::Character_DamageReactionComponent < 0x2000 &&
            (ActiveLayout::Character_DamageReactionComponent % sizeof(void*)) == 0 &&
            ActiveLayout::Character_StatusComponent >= 0x100 &&
            ActiveLayout::Character_StatusComponent < 0x2000 &&
            (ActiveLayout::Character_StatusComponent % sizeof(void*)) == 0 &&
            ActiveLayout::Character_VisualEffectComponent >= 0x100 &&
            ActiveLayout::Character_VisualEffectComponent < 0x2000 &&
            (ActiveLayout::Character_VisualEffectComponent % sizeof(void*)) == 0 &&
            ActiveLayout::Character_DamageReactionComponent != ActiveLayout::Character_StatusComponent &&
            ActiveLayout::Character_StatusComponent != ActiveLayout::Character_VisualEffectComponent &&
            ActiveLayout::DamageInfo_AttackElement < ActiveLayout::DamageInfo_EffectType1 &&
            ActiveLayout::DamageInfo_EffectType1 < ActiveLayout::DamageInfo_EffectValue1 &&
            ActiveLayout::DamageInfo_EffectValue1 < ActiveLayout::DamageInfo_EffectType2 &&
            ActiveLayout::DamageInfo_EffectType2 < ActiveLayout::DamageInfo_EffectValue2 &&
            ActiveLayout::DamageInfo_EffectValue2 < 0x400 &&
            (ActiveLayout::DamageInfo_EffectValue1 % alignof(int32_t)) == 0 &&
            (ActiveLayout::DamageInfo_EffectValue2 % alignof(int32_t)) == 0 &&
            ActiveLayout::StatusComponent_ExecutionStatusList >= 0x80 &&
            ActiveLayout::StatusComponent_ExecutionStatusList < 0x1000 &&
            (ActiveLayout::StatusComponent_ExecutionStatusList % sizeof(void*)) == 0 &&
            ActiveLayout::StatusBase_NativeSize >= 0x80 &&
            ActiveLayout::StatusBase_NativeSize < 0x400 &&
            (ActiveLayout::StatusBase_NativeSize % sizeof(void*)) == 0 &&
            ActiveLayout::StatusBase_IsEndStatus < ActiveLayout::StatusBase_StatusID &&
            ActiveLayout::StatusBase_StatusID < ActiveLayout::StatusBase_Duration &&
            ActiveLayout::StatusBase_Duration < ActiveLayout::StatusBase_DurationTimer &&
            ActiveLayout::StatusBase_DurationTimer + sizeof(float) <= ActiveLayout::StatusBase_NativeSize &&
            ActiveLayout::VisualEffectComponent_ExecutionVisualEffects >= 0x80 &&
            ActiveLayout::VisualEffectComponent_ExecutionVisualEffects < 0x1000 &&
            (ActiveLayout::VisualEffectComponent_ExecutionVisualEffects % sizeof(void*)) == 0 &&
            ActiveLayout::VisualEffectBase_IsEnd < ActiveLayout::VisualEffectBase_ID &&
            ActiveLayout::VisualEffectBase_ID < 0x400 &&
            ActiveLayout::SceneComponent_RelativeLocation >= 0x80 &&
            ActiveLayout::SceneComponent_RelativeLocation < 0x1000 &&
            ActiveLayout::SceneComponent_AttachParent >= 0x80 &&
            ActiveLayout::SceneComponent_AttachParent < 0x1000 &&
            (ActiveLayout::SceneComponent_AttachParent % sizeof(void*)) == 0;
    }

    RecordResolver_104(
        "CAPABILITY.LayoutProfile",
        ESE_HAS_GENERATED_PAL_LAYOUT ? "generated-sdk-layout" : "embedded-known-build-layout",
        layoutProfileOK
        ? (IsKnown104Fingerprint_104(fingerprint)
            ? EResolveConfidence_104::Exact
            : EResolveConfidence_104::ProfileStatic)
        : EResolveConfidence_104::Failed,
        0,
        layoutProfileOK
        ? (IsKnown104Fingerprint_104(fingerprint)
            ? "generated/active layout matches the 1.0.4 oracle"
            : "fingerprint-bound unknown-build generated layout, including Light-immunity target fields, passed structural sanity checks")
        : "layout profile failed sanity/oracle validation");

    if (!layoutProfileOK)
        return Fatal("active layout profile failed validation");

    RecordResolver_104(
        "CAPABILITY.RuntimeOwnerLink",
        "runtime-owner-backreference",
        EResolveConfidence_104::Deferred,
        0,
        "deferred: first elemental owner resolution must discover/verify UObject outer link before the capability is proven");

#if ESE_HAS_GENERATED_SCENE_RELATIVE_LOCATION
    RecordResolver_104(
        "CAPABILITY.RootPositionReference",
        "generated-scene-relative-location",
        EseGeneratedPalLayout::Has_SceneComponent_RelativeLocation
        ? EResolveConfidence_104::ProfileStatic
        : (IsKnown104Fingerprint_104(fingerprint)
            ? EResolveConfidence_104::RuntimeValidated
            : EResolveConfidence_104::Failed),
        0,
        EseGeneratedPalLayout::Has_SceneComponent_RelativeLocation
        ? (EseGeneratedPalLayout::Has_SceneComponent_AttachParent
            ? "USceneComponent::RelativeLocation + AttachParent generated; unattached-root semantics enforced and Freeze StartLocation proximity validates on use"
            : "USceneComponent::RelativeLocation generated; attachment state unavailable, so StartLocation proximity remains the fail-closed validator")
        : "generated relative location unavailable");
#else
    RecordResolver_104(
        "CAPABILITY.RootPositionReference",
        "known-build-hidden-world-fallback",
        IsKnown104Fingerprint_104(fingerprint)
        ? EResolveConfidence_104::RuntimeValidated
        : EResolveConfidence_104::Failed,
        0,
        IsKnown104Fingerprint_104(fingerprint)
        ? "exact 1.0.4 fallback +0x260; regenerate the v8 SDK header before unknown-build use"
        : "unknown build lacks generated USceneComponent::RelativeLocation");
#endif

    // -----------------------------------------------------
    // Phase A: resolve unique target anchors before hooks.
    // These anchors also establish an RVA-shift hint used by wrapper-local
    // recovery for otherwise similar generated wrappers.
    // -----------------------------------------------------
    // Primary buildup identity: reflected UFunction wrapper -> rel32 native CALL.
    // Native prologue AOB remains an independent fallback for compiler/linker drift.
    static const uint8_t kBuildupWrapperTail[] = {
        0x48,0x8B,0x43,0x20,
        0x33,0xC9,
        0xF3,0x0F,0x10,0x54,0x24,0x48,
        0x48,0x85,0xC0,
        0x0F,0xB6,0x54,0x24,0x38,
        0x0F,0x95,0xC1,
        0x48,0x03,0xC8,
        0x48,0x89,0x4B,0x20,
        0x48,0x8B,0xCF,
        0xE8,0x00,0x00,0x00,0x00
    };
    static const char kBuildupWrapperMask[] =
        "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx????";

    static const uint8_t kBuildupPattern[] = {
        0x40,0x55,0x56,0x48,0x83,0xEC,0x58,
        0x0F,0x29,0x7C,0x24,0x30,
        0x0F,0x57,0xC0,
        0x0F,0x28,0xFA,
        0x0F,0xB6,0xF2,
        0x0F,0x2F,0xF8,
        0x48,0x8B,0xE9
    };
    static const char kBuildupMask[] =
        "xxxxxxxxxxxxxxxxxxxxxxxxxxx";

    g_Resolved_ElementBuildup_104 = ResolveNativeFromWrapperCall_104(
        "DamageReaction::AddElementStatusAdditionalValue_OneType.wrapper",
        base,
        Known104::Rva::ElementBuildup_Call,
        Known104::Rva::ElementBuildup,
        kBuildupWrapperTail,
        kBuildupWrapperMask,
        33);

    if (!g_Resolved_ElementBuildup_104) {
        g_Resolved_ElementBuildup_104 = ResolvePatternTarget_104(
            "DamageReaction::AddElementStatusAdditionalValue_OneType.fallback",
            base,
            Known104::Rva::ElementBuildup,
            kBuildupPattern,
            kBuildupMask,
            true);
    }
    else {
        const intptr_t shift =
            static_cast<intptr_t>(g_Resolved_ElementBuildup_104 - base) -
            static_cast<intptr_t>(Known104::Rva::ElementBuildup);
        g_AnchorRvaShifts_104.push_back(shift);
        RecomputeRvaShiftHint_104();
    }

    if (!g_Resolved_ElementBuildup_104)
        return Fatal("elemental buildup implementation could not be resolved safely");

    // AddStatus is semantically recoverable from the resolved buildup routine:
    // map additional-effect -> StatusID, then call PalStatusComponent::AddStatus.
    static const uint8_t kAddStatusLocalCall[] = {
        0x40,0x0F,0xB6,0xD6,
        0x48,0x8B,0xC8,
        0xE8,0x00,0x00,0x00,0x00,
        0x0F,0xB6,0xD0,
        0x48,0x8B,0xCB,
        0xE8,0x00,0x00,0x00,0x00,
        0xEB,0x04,
        0xF3,0x0F,0x11,0x03
    };
    static const char kAddStatusLocalMask[] =
        "xxxxxxxx????xxxxxxx????xxxxxx";

    g_Resolved_AddStatus_104 = ResolveCallInsideResolvedFunction_104(
        "PalStatusComponent::AddStatus.from-buildup",
        g_Resolved_ElementBuildup_104,
        0x300,
        Known104::Rva::AddStatus,
        kAddStatusLocalCall,
        kAddStatusLocalMask,
        18);

    // Independent fallback: exact native prologue, useful if the local buildup
    // sequence is rearranged while AddStatus itself remains unchanged.
    if (!g_Resolved_AddStatus_104) {
        static const uint8_t kAddStatusPattern[] = {
            0x40,0x55,0x48,0x8D,0x6C,0x24,0xA9,
            0x48,0x81,0xEC,0xB0,0x00,0x00,0x00,
            0x45,0x33,0xC0,0x4C,0x89,0x45,0xC7,0x44,0x89,0x45,0xCF,
            0x0F,0x57,0xC9,0xF3,0x0F,0x11,0x4D,0xD3
        };
        static const char kAddStatusMask[] =
            "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx";

        g_Resolved_AddStatus_104 = ResolvePatternTarget_104(
            "PalStatusComponent::AddStatus.fallback",
            base,
            Known104::Rva::AddStatus,
            kAddStatusPattern,
            kAddStatusMask,
            true);
    }
    else {
        const intptr_t shift =
            static_cast<intptr_t>(g_Resolved_AddStatus_104 - base) -
            static_cast<intptr_t>(Known104::Rva::AddStatus);
        g_AnchorRvaShifts_104.push_back(shift);
        RecomputeRvaShiftHint_104();
    }

    if (!g_Resolved_AddStatus_104)
        return Fatal("native AddStatus implementation could not be resolved safely");

    RecomputeRvaShiftHint_104();

    // Raw FPalDamageInfo capture is a core semantic dependency, not an optional
    // enhancement. Without it normal 2U attacks would silently degrade to 1U.
    g_DamageEffectCaller_104 = FindEffectCallerStart_104(base);
    g_Resolved_RawEffectCaller_104 = g_DamageEffectCaller_104;
    if (g_DamageEffectCaller_104)
        CacheResolvedRva_104("RawFPalDamageInfoCaller", g_DamageEffectCaller_104);
    if (!g_DamageEffectCaller_104)
        return Fatal("raw FPalDamageInfo caller could not be resolved safely");

    RecordResolver_104(
        "CAPABILITY.RawUnits.Resolve",
        "capability",
        EResolveConfidence_104::RuntimeValidated,
        g_DamageEffectCaller_104,
        "raw EffectValue1/2 source resolved; hook pending");

    RecomputeRvaShiftHint_104();

    // Matchup is an independent capability. If its unique helper cannot be
    // proven after an update, elemental statuses/reactions may still run.
    const bool matchupReady = InstallElementMatchupTier_104(base);
    RecordResolver_104(
        "CAPABILITY.MatchupMatrix",
        "capability",
        matchupReady ? EResolveConfidence_104::RuntimeValidated : EResolveConfidence_104::Failed,
        matchupReady ? g_Resolved_MatchupHelper_104 : 0,
        matchupReady ? "enabled" : "disabled; core continues");

    RecomputeRvaShiftHint_104();

    // -----------------------------------------------------
    // Phase B: resolve reflected-wrapper-backed native calls.
    // -----------------------------------------------------
    // AddVisualEffect and AddVisualEffect_Local carry synchronous Light/Dark
    // provenance into visual-effect construction. Failure disables only the
    // affected presentation substitution path; core gameplay remains
    // active. Both reflected wrappers share the same tail shape but dispatch
    // to distinct native implementations.
    static const uint8_t kAddVfxTail[] = {
        0x48,0x8B,0x43,0x38,
        0x4C,0x8D,0x44,0x24,0x20,
        0x48,0x85,0xC0,
        0x4C,0x0F,0x45,0xC0,
        0x48,0x8B,0x43,0x20,
        0x48,0x85,0xC0,
        0x40,0x0F,0x95,0xC7,
        0x48,0x03,0xF8,
        0x48,0x89,0x7B,0x20,
        0x0F,0xB6,0x54,0x24,0x48,
        0x48,0x8B,0xCD,
        0xE8,0x00,0x00,0x00,0x00
    };
    static const char kAddVfxMask[] =
        "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx????";

    g_Resolved_AddVisualEffect_104 = ResolveNativeFromWrapperCall_104(
        "PalVisualEffectComponent::AddVisualEffect",
        base,
        Known104::Rva::AddVisualEffect_Call,
        Known104::Rva::AddVisualEffect_Native,
        kAddVfxTail,
        kAddVfxMask,
        42);

    g_Resolved_AddVisualEffectLocal_104 = ResolveNativeFromWrapperCall_104(
        "PalVisualEffectComponent::AddVisualEffect_Local",
        base,
        Known104::Rva::AddVisualEffectLocal_Call,
        Known104::Rva::AddVisualEffectLocal_Native,
        kAddVfxTail,
        kAddVfxMask,
        42);

    std::vector<FHookTransactionSpec_104> lightVfxHooks;
    lightVfxHooks.reserve(2);

    if (g_Resolved_AddVisualEffect_104) {
        lightVfxHooks.push_back(FHookTransactionSpec_104{
            "PalVisualEffectComponent::AddVisualEffect.light-substitution",
            g_Resolved_AddVisualEffect_104,
            reinterpret_cast<void*>(&Detour_AddVisualEffect_104),
            reinterpret_cast<void**>(&Original_AddVisualEffect_104)
            });
    }
    else {
        ShipLog_104(
            "[ElementalSystemExpanded] WARNING: AddVisualEffect could not "
            "be resolved; non-local Light VFX substitution disabled.\n");
    }

    if (g_Resolved_AddVisualEffectLocal_104) {
        lightVfxHooks.push_back(FHookTransactionSpec_104{
            "PalVisualEffectComponent::AddVisualEffect_Local.light-substitution",
            g_Resolved_AddVisualEffectLocal_104,
            reinterpret_cast<void*>(&Detour_AddVisualEffect_Local_104),
            reinterpret_cast<void**>(&Original_AddVisualEffect_Local_104)
            });
    }
    else {
        ShipLog_104(
            "[ElementalSystemExpanded] WARNING: AddVisualEffect_Local could not "
            "be resolved; local Light VFX substitution disabled.\n");
    }

    if (!lightVfxHooks.empty()) {
        if (InstallHookTransaction_104(
            "LightVfxSubstitution",
            lightVfxHooks.data(),
            lightVfxHooks.size())) {
            const uintptr_t capabilityAddress =
                g_Resolved_AddVisualEffect_104
                ? g_Resolved_AddVisualEffect_104
                : g_Resolved_AddVisualEffectLocal_104;
            RecordResolver_104(
                "CAPABILITY.LightVfxSubstitution",
                "capability",
                EResolveConfidence_104::RuntimeValidated,
                capabilityAddress,
                "Light-sourced DarkCondition and CameraVignette construction redirect through registered Light VFX classes and restore runtime IDs 21/24");
        }
        else {
            Original_AddVisualEffect_104 = nullptr;
            Original_AddVisualEffect_Local_104 = nullptr;
            ShipLog_104(
                "[ElementalSystemExpanded] WARNING: Light VFX substitution "
                "hook transaction failed; gameplay continues unchanged.\n");
        }
    }
    else {
        RecordResolver_104(
            "CAPABILITY.LightVfxSubstitution",
            "capability",
            EResolveConfidence_104::Failed,
            0,
            "AddVisualEffect creation paths unresolved; Light world/camera VFX substitution disabled; gameplay continues unchanged");
    }

    static const uint8_t kRemoveVfxTail[] = {
        0x48,0x8B,0x43,0x20,
        0x33,0xC9,
        0x0F,0xB6,0x54,0x24,0x38,
        0x48,0x85,0xC0,
        0x0F,0x95,0xC1,
        0x48,0x03,0xC8,
        0x48,0x89,0x4B,0x20,
        0x48,0x8B,0xCF,
        0xE8,0x00,0x00,0x00,0x00
    };
    static const char kRemoveVfxMask[] =
        "xxxxxxxxxxxxxxxxxxxxxxxxxxxx????";

    g_Resolved_RemoveVisualEffectLocal_104 = ResolveNativeFromWrapperCall_104(
        "PalVisualEffectComponent::RemoveVisualEffect_Local",
        base,
        Known104::Rva::RemoveVisualEffectLocal_Call,
        Known104::Rva::RemoveVisualEffectLocal_Native,
        kRemoveVfxTail,
        kRemoveVfxMask,
        27);

    if (!g_Resolved_RemoveVisualEffectLocal_104)
        return Fatal("RemoveVisualEffect_Local could not be resolved safely");

    Native_RemoveVisualEffect_Local_104 =
        reinterpret_cast<NativeRemoveVisualEffectLocal_104_t>(
            g_Resolved_RemoveVisualEffectLocal_104);

    if (!ResolveDurableVirtualSlots_104(base))
        return Fatal("required Freeze virtual slots could not be resolved safely");

    // -----------------------------------------------------
    // Phase C: install core status hooks at resolved addresses.
    // -----------------------------------------------------
    Native_AddStatus_104 =
        reinterpret_cast<NativeAddStatus_104_t>(g_Resolved_AddStatus_104);

    const FHookTransactionSpec_104 coreHooks[] = {
        {
            "AddElementStatusAdditionalValue_OneType",
            g_Resolved_ElementBuildup_104,
            reinterpret_cast<void*>(&Detour_AddElementStatusAdditionalValue_OneType_104),
            reinterpret_cast<void**>(&Original_AddElementStatusAdditionalValue_OneType_104)
        },
        {
            "PalStatusComponent::AddStatus",
            g_Resolved_AddStatus_104,
            reinterpret_cast<void*>(&Detour_NativeAddStatus_104),
            reinterpret_cast<void**>(&Original_NativeAddStatus_104)
        },
        {
            "RawFPalDamageInfoCaller",
            g_DamageEffectCaller_104,
            reinterpret_cast<void*>(&Detour_DamageEffectCaller_104),
            reinterpret_cast<void**>(&Original_DamageEffectCaller_104)
        }
    };

    if (!InstallHookTransaction_104(
        "ElementStatusCore",
        coreHooks,
        sizeof(coreHooks) / sizeof(coreHooks[0]))) {
        Native_AddStatus_104 = nullptr;
        g_DamageEffectCaller_104 = 0;
        g_Resolved_RawEffectCaller_104 = 0;
        return Fatal("elemental core hook transaction failed");
    }

    RecordResolver_104(
        "CAPABILITY.RawUnits",
        "capability",
        EResolveConfidence_104::RuntimeValidated,
        g_Resolved_RawEffectCaller_104,
        "raw EffectValue1/2 capture enabled");

    RecordResolver_104(
        "CAPABILITY.ElementStatusCore",
        "capability",
        EResolveConfidence_104::RuntimeValidated,
        g_Resolved_ElementBuildup_104,
        "buildup replacement + native AddStatus + raw units ready");

    // -----------------------------------------------------
    // Phase D: resolve/install complete reaction dependency set.
    // -----------------------------------------------------
    if (!InstallNativeReactionHooks_104(base))
        return Fatal("native reaction dependency set could not be resolved/installed transactionally");

    RecordResolver_104(
        "CAPABILITY.WetGatedStrongReactions",
        "capability",
        EResolveConfidence_104::RuntimeValidated,
        0,
        "all wrapper-backed reaction hooks installed");

    // Raw FPalDamageInfo capture was resolved and installed as part of the
    // elemental core transaction above.

    WriteBuildProfileCache_104(fingerprint);
    if (g_DebugDiagnosticsEnabled_104) {
        WriteMigrationReport_104(fingerprint);
        WriteResolverReport_104(fingerprint);

        if (!StartRuntimeSnapshotWorker_104()) {
            ShipLog_104(
                "[ElementalSystemExpanded] WARNING: persistent diagnostics worker "
                "could not be started; gameplay remains active, runtime snapshot "
                "persistence unavailable.\n");
        }
    }

    ShipLog_104(
        "[ElementalSystemExpanded] ACTIVE: Stage 6.3 validated gameplay/presentation baseline; "
        "Stage 6.6.3 final shipping + Neutral/Light immunity + elemental status exchange + Darkness/Light 3s cap. Debug diagnostics=%s.\n",
        g_DebugDiagnosticsEnabled_104 ? "ON" : "OFF");

    ModLog(
        "[ElementalSystemExpanded] Semantics: elemental ICD=%.1fs or 3rd hit; "
        "1U=%.1fs 2U=%.1fs; exchange carryover threshold=%.1fs; "
        "Darkness/Light cap=%.1fs; reaction ICD=%.1fs; WetFreeze=%.1fs.\n",
        kElementICDSeconds_104,
        kOneUnitDuration_104,
        kTwoUnitDuration_104,
        kMinimumExchangeCarryover_104,
        kDarknessDuration_104,
        kReactionICDSeconds_104,
        kWetFreezeDuration_104);

    ModLog(
        "[ElementalSystemExpanded] Light presentation: DarkCondition runtime ID=%u "
        "uses lookup %u; CameraVignette runtime ID=%u uses lookup %u; "
        "genuine Dark remains vanilla.\n",
        static_cast<unsigned>(kDarkConditionVisualEffectID_104),
        static_cast<unsigned>(kLightVisualEffectLookupID_104),
        static_cast<unsigned>(kCameraVignetteVisualEffectID_104),
        static_cast<unsigned>(kLightCameraVisualEffectLookupID_104));

    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    (void)lpReserved;

    if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
        g_SelfModule_104 = hModule;
        DisableThreadLibraryCalls(hModule);
    }

    return TRUE;
}

extern "C" {
    __declspec(dllexport) void* start_mod() {
        if (InterlockedCompareExchange(&g_StartRequested_104, 1, 0) != 0)
            return nullptr;

        // Create suspended so the lifecycle handle is published before any
        // initialization code can run. The handle is retained intentionally
        // and closed by uninstall_mod after the thread has exited.
        HANDLE thread = CreateThread(
            nullptr,
            0,
            MainThread,
            nullptr,
            CREATE_SUSPENDED,
            nullptr);

        if (!thread) {
            InterlockedExchange(&g_StartRequested_104, 0);
            OutputDebugStringA(
                "[ElementalSystemExpanded] ERROR: failed to create initialization thread.\n");
            return nullptr;
        }

        g_InitThread_104 = thread;
        if (ResumeThread(thread) == static_cast<DWORD>(-1)) {
            g_InitThread_104 = nullptr;
            CloseHandle(thread);
            InterlockedExchange(&g_StartRequested_104, 0);
            OutputDebugStringA(
                "[ElementalSystemExpanded] ERROR: failed to resume initialization thread.\n");
            return nullptr;
        }

        return nullptr;
    }

    __declspec(dllexport) void uninstall_mod(void* mod) {
        (void)mod;

        // UE4SS may request unload immediately after start_mod. Wait for the
        // asynchronous initializer to exit before disabling MinHook or clearing
        // globals, otherwise the worker could execute code from an unloaded DLL.
        HANDLE initThread = g_InitThread_104;
        if (initThread) {
            WaitForSingleObject(initThread, INFINITE);
            CloseHandle(initThread);
            g_InitThread_104 = nullptr;
        }

        MH_DisableHook(MH_ALL_HOOKS);

        // In debug mode, stop the diagnostics worker before clearing runtime
        // state so no worker thread can outlive this module.
        StopRuntimeSnapshotWorker_104();

        AcquireSRWLockExclusive(&g_ElementICDLock_104);
        g_ElementICD_104.clear();
        ReleaseSRWLockExclusive(&g_ElementICDLock_104);

        AcquireSRWLockExclusive(&g_ElementalStatusProvenanceLock_104);
        g_ElementalStatusProvenance_104.clear();
        ReleaseSRWLockExclusive(&g_ElementalStatusProvenanceLock_104);

        AcquireSRWLockExclusive(&g_ReactionICDLock_104);
        g_ReactionLastSuccess_104.clear();
        ReleaseSRWLockExclusive(&g_ReactionICDLock_104);

        g_ReactionTLS_104 = {};

        Original_SetActionClassParameter_104 = nullptr;
        Original_PlayAction_104 = nullptr;
        Original_SetJumpDisableFlag_104 = nullptr;
        Original_SetStepDisableFlag_104 = nullptr;
        Original_SetMoveDisableFlag_104 = nullptr;
        Original_SetWalkSpeedMultiplier_104 = nullptr;
        Original_SetYawRotatorMultiplier_104 = nullptr;
        Original_SetDisableAimFlag_Layered_104 = nullptr;
        Original_SetDisableShootFlag_Layered_104 = nullptr;
        Original_SetDisableChangeWeaponFlag_Layered_104 = nullptr;

        AcquireSRWLockExclusive(&g_BlockedFreezeRuntimeLock_104);
        g_BlockedFreezeRuntime_104.clear();
        ReleaseSRWLockExclusive(&g_BlockedFreezeRuntimeLock_104);

        g_FreezeTickTLS_104 = {};
        Original_SetComponentTickEnabledVirtual_104 = nullptr;
        Original_StopAnimMontageVirtual_104 = nullptr;
        Original_FreezeTickVirtual_104 = nullptr;
        g_SetComponentTickEnabledVirtualTarget_104 = nullptr;
        g_StopAnimMontageVirtualTarget_104 = nullptr;
        g_FreezeTickVirtualTarget_104 = nullptr;

        AcquireSRWLockExclusive(&g_UObjectOuterOffsetLock_104);
        g_RuntimeUObjectOuterOffset_104 = 0;
        g_RuntimeUObjectOuterLogged_104 = false;
        ReleaseSRWLockExclusive(&g_UObjectOuterOffsetLock_104);

        AcquireSRWLockExclusive(
            &g_FreezeStartLocationLocatorLock_104);
        g_FreezeStartLocationOffset_104 = 0;
        g_FreezeStartLocationUsesFloat_104 = false;
        ReleaseSRWLockExclusive(
            &g_FreezeStartLocationLocatorLock_104);

        AcquireSRWLockExclusive(&g_RootReferenceDiagnosticsLock_104);
        g_RootReferenceValidationLogged_104 = false;
        g_RootReferenceCrossCheckPassed_104 = false;
        g_RootReferenceCrossCheckDistSq_104 = -1.0;
        g_RootReferenceUsedHiddenFallback_104 = false;
        ReleaseSRWLockExclusive(&g_RootReferenceDiagnosticsLock_104);
        InterlockedExchange(&g_StatusArrayValidationPasses_104, 0);
        InterlockedExchange(&g_StatusArrayValidationFailures_104, 0);
        InterlockedExchange(&g_VfxArrayValidationPasses_104, 0);
        InterlockedExchange(&g_VfxArrayValidationFailures_104, 0);
        InterlockedExchange(&g_RuntimeSnapshotDirty_104, 1);
        InterlockedExchange(&g_RuntimeSnapshotWrites_104, 0);
        InterlockedExchange(&g_RuntimeSnapshotWriteFailures_104, 0);
        InterlockedExchange(&g_LightNeutralLayoutWarningLogged_104, 0);

        Original_CalcElementMatchupTier_104 = nullptr;
        Original_AddVisualEffect_104 = nullptr;
        Original_AddVisualEffect_Local_104 = nullptr;
        Native_RemoveVisualEffect_Local_104 = nullptr;
        Native_AddStatus_104 = nullptr;
        Original_AddElementStatusAdditionalValue_OneType_104 = nullptr;
        Original_NativeAddStatus_104 = nullptr;
        Original_DamageEffectCaller_104 = nullptr;
        g_DamageEffectCaller_104 = 0;

        MH_Uninitialize();

        InterlockedExchange(&g_StartRequested_104, 0);
    }
}