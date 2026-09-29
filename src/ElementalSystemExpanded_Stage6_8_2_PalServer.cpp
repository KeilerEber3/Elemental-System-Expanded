#include <intrin.h>
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
#  if __has_include("ElementalSystemExpanded_GeneratedPalServerLayout_v8.hpp")
#    include "ElementalSystemExpanded_GeneratedPalServerLayout_v8.hpp"
#    define ESE_HAS_GENERATED_PAL_LAYOUT 1
#    define ESE_HAS_GENERATED_ENGINE_OPTIONALS 1
#    define ESE_HAS_GENERATED_SCENE_RELATIVE_LOCATION 1
#    define ESE_HAS_GENERATED_BUILD_FINGERPRINT 1
#    define ESE_HAS_GENERATED_LIGHT_TARGET_LAYOUT 1
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
static HMODULE g_SelfModule = nullptr;
static std::string g_ArtifactDirectory;
static std::string g_ProfilePath;
static std::string g_LegacyProfilePath;
static std::string g_MigrationReportPath;
static std::string g_ResolverReportPath;
static std::string g_UpdateDiagnosticsPath;
static std::string g_LogPath;
static std::string g_RuntimeSnapshotPath;
static std::string g_DebugMarkerPath;
static std::string g_DiagnosticsInitStatus = "not-initialized";
static std::string g_ArtifactPathStatus = "not-initialized";
static bool g_DebugDiagnosticsEnabled = false;
static bool g_ForceFailureArtifacts = false;
static volatile LONG g_StartRequested = 0;
// Retained intentionally until uninstall so hot-unload can wait for the
// asynchronous initialization thread to exit before this DLL is torn down.
static HANDLE g_InitThread = nullptr;

static std::string JoinArtifactPath(const std::string& directory, const char* leaf)
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

static bool InitializeArtifactPaths()
{
    // MAX_PATH-sized stack buffers are too small for extended Windows paths.
    // Allocate the full Windows extended-path scratch area explicitly on heap
    // so MSVC cannot fold a ~64 KiB local frame into this or an inlined caller.
    static constexpr DWORD kModulePathCapacity = 32768;
    char* modulePath = static_cast<char*>(
        HeapAlloc(
            GetProcessHeap(),
            HEAP_ZERO_MEMORY,
            static_cast<SIZE_T>(kModulePathCapacity)));

    if (!modulePath) {
        g_ArtifactPathStatus = "dll-path-allocation-failed";
        return false;
    }

    const DWORD n = GetModuleFileNameA(
        g_SelfModule,
        modulePath,
        kModulePathCapacity);

    if (!n || n >= kModulePathCapacity) {
        HeapFree(GetProcessHeap(), 0, modulePath);
        g_ArtifactPathStatus = "dll-path-resolution-failed";
        return false;
    }

    std::string full(modulePath, modulePath + n);
    HeapFree(GetProcessHeap(), 0, modulePath);
    const size_t slash = full.find_last_of("\\/");
    g_ArtifactDirectory =
        slash == std::string::npos ? std::string(".") : full.substr(0, slash);

    g_LegacyProfilePath = JoinArtifactPath(
        g_ArtifactDirectory,
        "ElementalSystemExpanded_build_profile.txt");
    g_ProfilePath.clear();
    g_MigrationReportPath = JoinArtifactPath(
        g_ArtifactDirectory,
        "ElementalSystemExpanded_migration_report.txt");
    g_ResolverReportPath = JoinArtifactPath(
        g_ArtifactDirectory,
        "ElementalSystemExpanded_resolver_report.txt");
    g_UpdateDiagnosticsPath = JoinArtifactPath(
        g_ArtifactDirectory,
        "ElementalSystemExpanded_update_diagnostics.txt");
    g_LogPath = JoinArtifactPath(
        g_ArtifactDirectory,
        "ElementalSystemExpanded_log.txt");
    g_RuntimeSnapshotPath = JoinArtifactPath(
        g_ArtifactDirectory,
        "ElementalSystemExpanded_runtime_snapshot.txt");
    g_DebugMarkerPath = JoinArtifactPath(
        g_ArtifactDirectory,
        "debug_enabled.txt");

    g_ArtifactPathStatus = "dll-directory-absolute";
    return true;
}

static const char* ArtifactPathOrFallback(
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

static bool FileExistsRegular(const std::string& path)
{
    if (path.empty())
        return false;

    const DWORD attributes = GetFileAttributesA(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

static void InitializeDebugDiagnosticsMode()
{
    g_DebugDiagnosticsEnabled =
        FileExistsRegular(g_DebugMarkerPath);

    // Keep gameplay hooks free of synchronous log-file I/O even in debug mode.
    // Developer traces go to debugger/console; structured artifacts are written
    // only from startup code or the low-priority snapshot worker.
    g_LogToTxt = false;
}

static void EmitLogV(
    bool alwaysEmit,
    const char* format,
    va_list args)
{
    if (!alwaysEmit && !g_DebugDiagnosticsEnabled)
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
        ArtifactPathOrFallback(
            g_LogPath,
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
    if (!g_DebugDiagnosticsEnabled)
        return;

    va_list args;
    va_start(args, format);
    EmitLogV(false, format, args);
    va_end(args);
}

// Concise shipping-visible log for failures/degraded capabilities.
static void ShipLog(const char* format, ...)
{
    va_list args;
    va_start(args, format);
    EmitLogV(true, format, args);
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

        static constexpr uintptr_t DamageInfo_BasePower = 0x04;
        static constexpr uintptr_t DamageInfo_AttackElement = 0x30;
        static constexpr uintptr_t DamageInfo_Attacker = 0x48;
        static constexpr uintptr_t DamageInfo_AttackType = 0x58;
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

        // Reflected BlueprintGeneratedClass fields recovered from the shipped
        // Burn/Wetness status classes. Burn and Wetness maintain a second
        // lifetime clock in Blueprint; native Duration/DurationTimer alone are
        // not the semantic lifetime for these two statuses. These derived-class
        // offsets are never trusted from the executable fingerprint alone:
        // SetStatusDuration runtime-validates each one against a live
        // status instance before the first write and caches only a proven match.
        static constexpr uintptr_t StatusBurn_DurationTimerBP = 0xD8;
        static constexpr uintptr_t StatusWetness_DurationTimerBP = 0xD0;

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
    static constexpr uintptr_t DamageInfo_BasePower = EseGeneratedPalLayout::DamageInfo_BasePower;
    static constexpr uintptr_t DamageInfo_AttackElement = EseGeneratedPalLayout::DamageInfo_AttackElement;
    static constexpr uintptr_t DamageInfo_Attacker = EseGeneratedPalLayout::DamageInfo_Attacker;
    static constexpr uintptr_t DamageInfo_AttackType = EseGeneratedPalLayout::DamageInfo_AttackType;
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
    static constexpr uintptr_t DamageInfo_BasePower = Known104::Offset::DamageInfo_BasePower;
    static constexpr uintptr_t DamageInfo_AttackElement = Known104::Offset::DamageInfo_AttackElement;
    static constexpr uintptr_t DamageInfo_Attacker = Known104::Offset::DamageInfo_Attacker;
    static constexpr uintptr_t DamageInfo_AttackType = Known104::Offset::DamageInfo_AttackType;
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

enum class EResolveConfidence : uint8_t {
    Exact,
    Strong,
    RuntimeValidated,
    ProfileStatic,
    Deferred,
    Failed
};

static const char* ResolveConfidenceName(EResolveConfidence value)
{
    switch (value) {
    case EResolveConfidence::Exact: return "EXACT";
    case EResolveConfidence::Strong: return "STRONG";
    case EResolveConfidence::RuntimeValidated: return "RUNTIME_VALIDATED";
    case EResolveConfidence::ProfileStatic: return "PROFILE_STATIC";
    case EResolveConfidence::Deferred: return "DEFERRED";
    default: return "FAILED";
    }
}

struct FResolverRecord {
    std::string Name;
    std::string Method;
    EResolveConfidence Confidence = EResolveConfidence::Failed;
    uintptr_t Address = 0;
    uintptr_t Rva = 0;
    std::string Detail;
};

static std::vector<FResolverRecord> g_ResolverRecords;
static SRWLOCK g_ResolverRecordsLock = SRWLOCK_INIT;
static uintptr_t g_ModuleBase = 0;
static intptr_t g_RvaShiftHint = 0;
static std::vector<intptr_t> g_AnchorRvaShifts;

static uintptr_t g_Resolved_ElementBuildup = 0;
static uintptr_t g_Resolved_AddStatus = 0;
static uintptr_t g_Resolved_AddStatusParameter = 0;
static uintptr_t g_Resolved_MatchupHelper = 0;
static uintptr_t g_Resolved_RawEffectCaller = 0;
static uintptr_t g_Resolved_AddVisualEffect = 0;
static uintptr_t g_Resolved_AddVisualEffectLocal = 0;
static uintptr_t g_Resolved_RemoveVisualEffectLocal = 0;

static uintptr_t g_VSlot_SetComponentTickEnabled = Known104::VSlot::SetComponentTickEnabled;
static uintptr_t g_VSlot_StopAnimMontage = Known104::VSlot::StopAnimMontage;
static uintptr_t g_VSlot_StatusTick = Known104::VSlot::StatusTick;

struct FBuildFingerprint {
    DWORD TimeDateStamp = 0;
    DWORD SizeOfImage = 0;
    DWORD TextRva = 0;
    DWORD TextSize = 0;
    uint64_t TextHash = 0;
};


struct FBuildProfileCache {
    bool Loaded = false;
    bool FingerprintMatched = false;
    FBuildFingerprint Fingerprint{};
    std::unordered_map<std::string, uintptr_t> Rvas;
    std::unordered_map<std::string, uintptr_t> Callsites;
    std::unordered_map<std::string, uintptr_t> VSlots;
    std::unordered_map<std::string, uintptr_t> VDispatches;
};

static FBuildProfileCache g_BuildProfileCache;
static std::unordered_map<std::string, uintptr_t> g_ProfileOutputRvas;
static std::unordered_map<std::string, uintptr_t> g_ProfileOutputCallsites;
static std::unordered_map<std::string, uintptr_t> g_ProfileOutputVSlots;
static std::unordered_map<std::string, uintptr_t> g_ProfileOutputVDispatches;
static std::string g_ProfileCacheStatus = "not-loaded";
static std::string g_ProfileWriteStatus = "not-written";
static constexpr const char* kResolverRevision =
    "Stage6.7.4.11.1-bounded-native-registration-vslot-resolver";
static std::string g_ShooterCacheRepairStatus = "not-checked";
static std::string g_DryFreezeVSlotProbeStatus = "not-started";
static FBuildFingerprint g_CurrentFingerprint{};

// Runtime-discovered UObject ownership link. The resolver report may be
// written before the first elemental hit performs discovery.
static SRWLOCK g_UObjectOuterOffsetLock = SRWLOCK_INIT;
static uintptr_t g_RuntimeUObjectOuterOffset = 0;
static bool g_RuntimeUObjectOuterLogged = false;

// Container ABI guard counters. These are cheap atomics updated only when
// status/VFX arrays are inspected, not per frame.
static volatile LONG g_StatusArrayValidationPasses = 0;
static volatile LONG g_StatusArrayValidationFailures = 0;
static volatile LONG g_VfxArrayValidationPasses = 0;
static volatile LONG g_VfxArrayValidationFailures = 0;

// Persistent diagnostics are maintained by a dedicated low-priority worker.
// Gameplay paths only mutate in-memory state and optionally signal the worker;
// they never perform disk I/O.
static HANDLE g_RuntimeSnapshotThread = nullptr;
static HANDLE g_RuntimeSnapshotStopEvent = nullptr;
static HANDLE g_RuntimeSnapshotWakeEvent = nullptr;
static volatile LONG g_RuntimeSnapshotDirty = 1;
static volatile LONG g_RuntimeSnapshotWrites = 0;
static volatile LONG g_RuntimeSnapshotWriteFailures = 0;

static void MarkRuntimeSnapshotDirty();
static bool WriteWholeFileDurable(const char* finalPath, const std::string& data);

// Runtime-discovered UObject ownership link. Declared here because the resolver
// report may be written before the first elemental hit performs discovery.
static bool FingerprintsEqual(
    const FBuildFingerprint& a,
    const FBuildFingerprint& b)
{
    return a.TimeDateStamp == b.TimeDateStamp &&
        a.SizeOfImage == b.SizeOfImage &&
        a.TextHash == b.TextHash;
}

static bool IsActualKnown104Fingerprint(const FBuildFingerprint& fp)
{
    return fp.TimeDateStamp == Known104::Build::TimeDateStamp &&
        fp.SizeOfImage == Known104::Build::SizeOfImage &&
        fp.TextHash == Known104::Build::TextHash;
}

static bool IsKnown104Fingerprint(const FBuildFingerprint& fp)
{
#if ESE_DEV_FORCE_UNKNOWN_BOOTSTRAP
    (void)fp;
    return false;
#else
    return IsActualKnown104Fingerprint(fp);
#endif
}

static uintptr_t ParseHexU64(const char* text)
{
    if (!text)
        return 0;
    return static_cast<uintptr_t>(strtoull(text, nullptr, 0));
}

static std::string BuildProfileLeafForFingerprint(
    const FBuildFingerprint& fp)
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

static void SelectCurrentBuildProfilePath(
    const FBuildFingerprint& fp)
{
    const std::string leaf = BuildProfileLeafForFingerprint(fp);
    g_ProfilePath = JoinArtifactPath(
        g_ArtifactDirectory,
        leaf.c_str());
}

static bool ParseBuildProfileFile(
    const char* path,
    FBuildProfileCache* out)
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

    FBuildProfileCache temp{};
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
            temp.Fingerprint.TimeDateStamp = static_cast<DWORD>(ParseHexU64(value));
        else if (strcmp(key, "Fingerprint.SizeOfImage") == 0)
            temp.Fingerprint.SizeOfImage = static_cast<DWORD>(ParseHexU64(value));
        else if (strcmp(key, "Fingerprint.TextHash") == 0)
            temp.Fingerprint.TextHash = static_cast<uint64_t>(strtoull(value, nullptr, 0));
        else if (strncmp(key, "RVA.", 4) == 0)
            temp.Rvas[key + 4] = ParseHexU64(value);
        else if (strncmp(key, "CALL.", 5) == 0)
            temp.Callsites[key + 5] = ParseHexU64(value);
        else if (strncmp(key, "VSLOT.", 6) == 0)
            temp.VSlots[key + 6] = ParseHexU64(value);
        else if (strncmp(key, "VDISPATCH.", 10) == 0)
            temp.VDispatches[key + 10] = ParseHexU64(value);
    }
    fclose(file);

    *out = std::move(temp);
    return true;
}

static bool LoadBuildProfileCache(const FBuildFingerprint& current)
{
    g_BuildProfileCache = {};
    SelectCurrentBuildProfilePath(current);

#if ESE_DEV_FORCE_UNKNOWN_BOOTSTRAP
    g_ProfileCacheStatus = "dev-forced-bootstrap-no-profile";
    return false;
#endif

    FBuildProfileCache keyed{};
    if (ParseBuildProfileFile(
        ArtifactPathOrFallback(
            g_ProfilePath,
            "ElementalSystemExpanded_build_profile_current.txt"),
        &keyed)) {
        keyed.FingerprintMatched = FingerprintsEqual(keyed.Fingerprint, current);
        if (keyed.FingerprintMatched) {
            g_BuildProfileCache = std::move(keyed);
            g_ProfileCacheStatus = "matched-keyed-profile";
            return true;
        }

        // A fingerprint-keyed filename containing another fingerprint is
        // internally inconsistent/corrupt. Do not trust it, but preserve it
        // for diagnosis and still allow a matching legacy profile to rescue
        // this exact build.
        g_ProfileCacheStatus = "keyed-profile-fingerprint-mismatch";
    }
    else {
        g_ProfileCacheStatus = "keyed-profile-missing";
    }

    FBuildProfileCache legacy{};
    if (ParseBuildProfileFile(
        ArtifactPathOrFallback(
            g_LegacyProfilePath,
            "ElementalSystemExpanded_build_profile.txt"),
        &legacy)) {
        legacy.FingerprintMatched = FingerprintsEqual(legacy.Fingerprint, current);
        if (legacy.FingerprintMatched) {
            g_BuildProfileCache = std::move(legacy);
            g_ProfileCacheStatus = "matched-legacy-profile-import-pending";
            return true;
        }

        if (g_ProfileCacheStatus == "keyed-profile-missing")
            g_ProfileCacheStatus = "keyed-missing-legacy-other-build-preserved";
    }

    return false;
}

static uintptr_t CachedProfileValue(
    const std::unordered_map<std::string, uintptr_t>& map,
    const char* name)
{
    if (!g_BuildProfileCache.FingerprintMatched || !name)
        return 0;
    const auto it = map.find(name);
    return it == map.end() ? 0 : it->second;
}

static void CacheResolvedRva(const char* name, uintptr_t address)
{
    if (!name || !address || !g_ModuleBase || address < g_ModuleBase)
        return;
    g_ProfileOutputRvas[name] = address - g_ModuleBase;
}

static void CacheResolvedCallsite(const char* name, uintptr_t callsite)
{
    if (!name || !callsite || !g_ModuleBase || callsite < g_ModuleBase)
        return;
    g_ProfileOutputCallsites[name] = callsite - g_ModuleBase;
}

static void CacheResolvedVSlot(
    const char* name,
    uintptr_t slot,
    uintptr_t dispatchAddress)
{
    if (!name || !slot)
        return;
    g_ProfileOutputVSlots[name] = slot;
    if (dispatchAddress && g_ModuleBase && dispatchAddress >= g_ModuleBase)
        g_ProfileOutputVDispatches[name] = dispatchAddress - g_ModuleBase;
}

template <typename TMap>
static void AppendProfileMapSorted(
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

static bool ProfileOutputsMatchLoadedCache()
{
    return g_BuildProfileCache.FingerprintMatched &&
        g_BuildProfileCache.Rvas == g_ProfileOutputRvas &&
        g_BuildProfileCache.Callsites == g_ProfileOutputCallsites &&
        g_BuildProfileCache.VSlots == g_ProfileOutputVSlots &&
        g_BuildProfileCache.VDispatches == g_ProfileOutputVDispatches;
}

static void WriteBuildProfileCache(const FBuildFingerprint& fp)
{
    if (g_ProfilePath.empty())
        SelectCurrentBuildProfilePath(fp);

#if ESE_DEV_FORCE_UNKNOWN_BOOTSTRAP
    g_ProfileWriteStatus = "dev-forced-bootstrap-no-write";
    return;
#endif

    // A matching fingerprint-keyed profile that revalidated to the same
    // resolved map does not need to be rewritten every boot.
    if (g_ProfileCacheStatus == "matched-keyed-profile" &&
        ProfileOutputsMatchLoadedCache()) {
        g_ProfileWriteStatus = "unchanged-keyed-profile-not-rewritten";
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

    AppendProfileMapSorted(output, "RVA.", g_ProfileOutputRvas);
    AppendProfileMapSorted(output, "CALL.", g_ProfileOutputCallsites);
    AppendProfileMapSorted(output, "VSLOT.", g_ProfileOutputVSlots);
    AppendProfileMapSorted(output, "VDISPATCH.", g_ProfileOutputVDispatches);

    if (!WriteWholeFileDurable(
        ArtifactPathOrFallback(
            g_ProfilePath,
            "ElementalSystemExpanded_build_profile_current.txt"),
        output)) {
        g_ProfileWriteStatus = "keyed-profile-write-failed";
        return;
    }

    if (g_ProfileCacheStatus == "matched-legacy-profile-import-pending")
        g_ProfileWriteStatus = "legacy-imported-to-keyed-profile";
    else
        g_ProfileWriteStatus = "written-keyed-current-build";
}

static void RecordResolver(
    const char* name,
    const char* method,
    EResolveConfidence confidence,
    uintptr_t address,
    const char* detail)
{
    FResolverRecord rec{};
    rec.Name = name ? name : "";
    rec.Method = method ? method : "";
    rec.Confidence = confidence;
    rec.Address = address;
    rec.Rva = (address && g_ModuleBase && address >= g_ModuleBase)
        ? (address - g_ModuleBase)
        : 0;
    rec.Detail = detail ? detail : "";

    AcquireSRWLockExclusive(&g_ResolverRecordsLock);
    g_ResolverRecords.push_back(rec);
    ReleaseSRWLockExclusive(&g_ResolverRecordsLock);

    ModLog(
        "[ElementalSystemExpanded] RESOLVE %-44s %-17s RVA=+0x%llX Method=%s %s\n",
        rec.Name.c_str(),
        ResolveConfidenceName(rec.Confidence),
        static_cast<unsigned long long>(rec.Rva),
        rec.Method.c_str(),
        rec.Detail.c_str());
}

static uint64_t Fnv1a64(const uint8_t* data, size_t size)
{
    uint64_t hash = 1469598103934665603ull;
    for (size_t i = 0; i < size; ++i) {
        hash ^= static_cast<uint64_t>(data[i]);
        hash *= 1099511628211ull;
    }
    return hash;
}

static bool IsReadableFingerprintRange(
    uintptr_t address,
    size_t size)
{
    if (!address || !size)
        return false;

    if (size > (std::numeric_limits<uintptr_t>::max)() - address)
        return false;

    const uintptr_t end = address + size;
    uintptr_t cursor = address;

    while (cursor < end) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(
                reinterpret_cast<const void*>(cursor),
                &mbi,
                sizeof(mbi)) != sizeof(mbi)) {
            return false;
        }

        if (mbi.State != MEM_COMMIT ||
            (mbi.Protect & PAGE_GUARD) != 0 ||
            (mbi.Protect & PAGE_NOACCESS) != 0) {
            return false;
        }

        const uintptr_t regionBase =
            reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        const uintptr_t regionEnd =
            regionBase + static_cast<uintptr_t>(mbi.RegionSize);

        if (regionEnd <= cursor)
            return false;

        cursor = regionEnd < end ? regionEnd : end;
    }

    return true;
}

__declspec(noinline)
static bool QueryBuildFingerprint(
    uintptr_t base,
    FBuildFingerprint* out)
{
    if (!base || !out)
        return false;

    if (!IsReadableFingerprintRange(
            base,
            sizeof(IMAGE_DOS_HEADER))) {
        return false;
    }

    IMAGE_DOS_HEADER dos{};
    memcpy(
        &dos,
        reinterpret_cast<const void*>(base),
        sizeof(dos));

    if (dos.e_magic != IMAGE_DOS_SIGNATURE ||
        dos.e_lfanew <= 0) {
        return false;
    }

    const uintptr_t ntAddress =
        base + static_cast<uintptr_t>(dos.e_lfanew);

    if (!IsReadableFingerprintRange(
            ntAddress,
            sizeof(IMAGE_NT_HEADERS64))) {
        return false;
    }

    IMAGE_NT_HEADERS64 nt{};
    memcpy(
        &nt,
        reinterpret_cast<const void*>(ntAddress),
        sizeof(nt));

    if (nt.Signature != IMAGE_NT_SIGNATURE ||
        nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        nt.FileHeader.NumberOfSections == 0 ||
        nt.FileHeader.NumberOfSections > 96) {
        return false;
    }

    out->TimeDateStamp = nt.FileHeader.TimeDateStamp;
    out->SizeOfImage = nt.OptionalHeader.SizeOfImage;

    const uintptr_t sectionTable =
        ntAddress +
        sizeof(DWORD) +
        sizeof(IMAGE_FILE_HEADER) +
        nt.FileHeader.SizeOfOptionalHeader;

    const size_t sectionTableSize =
        static_cast<size_t>(nt.FileHeader.NumberOfSections) *
        sizeof(IMAGE_SECTION_HEADER);

    if (!IsReadableFingerprintRange(
            sectionTable,
            sectionTableSize)) {
        return false;
    }

    for (WORD i = 0; i < nt.FileHeader.NumberOfSections; ++i) {
        IMAGE_SECTION_HEADER section{};

        memcpy(
            &section,
            reinterpret_cast<const void*>(
                sectionTable +
                static_cast<uintptr_t>(i) *
                    sizeof(IMAGE_SECTION_HEADER)),
            sizeof(section));

        if (memcmp(section.Name, ".text", 5) != 0)
            continue;

        const uint32_t textRva = section.VirtualAddress;
        uint32_t textSize = section.Misc.VirtualSize;
        if (!textSize)
            textSize = section.SizeOfRawData;

        if (!textSize ||
            textRva >= nt.OptionalHeader.SizeOfImage ||
            textSize > nt.OptionalHeader.SizeOfImage - textRva) {
            return false;
        }

        const uintptr_t textAddress =
            base + static_cast<uintptr_t>(textRva);

        if (!IsReadableFingerprintRange(
                textAddress,
                textSize)) {
            return false;
        }

        out->TextRva = textRva;
        out->TextSize = textSize;
        out->TextHash = Fnv1a64(
            reinterpret_cast<const uint8_t*>(textAddress),
            textSize);
        return true;
    }

    return false;
}

__declspec(noinline)
static bool GeneratedLayoutFingerprintMatches(
    const FBuildFingerprint& fp,
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
        static constexpr SIZE_T kDetailCapacity = 384;

        char* buffer = static_cast<char*>(
            HeapAlloc(
                GetProcessHeap(),
                HEAP_ZERO_MEMORY,
                kDetailCapacity));

        if (buffer) {
            snprintf(
                buffer,
                kDetailCapacity,
                "header=%08lX/%08lX/%016llX runtime=%08lX/%08lX/%016llX",
                static_cast<unsigned long>(EseGeneratedPalLayout::Build_TimeDateStamp),
                static_cast<unsigned long>(EseGeneratedPalLayout::Build_SizeOfImage),
                static_cast<unsigned long long>(EseGeneratedPalLayout::Build_TextHash),
                static_cast<unsigned long>(fp.TimeDateStamp),
                static_cast<unsigned long>(fp.SizeOfImage),
                static_cast<unsigned long long>(fp.TextHash));

            detail->assign(buffer);
            HeapFree(GetProcessHeap(), 0, buffer);
        }
        else {
            *detail = match
                ? "generated layout fingerprint matches runtime"
                : "generated layout fingerprint mismatch; detail allocation failed";
        }
    }

    return match;
#else
    if (detail)
        *detail = "legacy generated header has no executable fingerprint";
    return false;
#endif
}

static bool GeneratedLayoutFingerprintAvailable()
{
#if ESE_HAS_GENERATED_BUILD_FINGERPRINT
    return EseGeneratedPalLayout::Has_BuildFingerprint;
#else
    return false;
#endif
}

static void AppendMigrationValue(
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

static void WriteMigrationReport(const FBuildFingerprint& fp)
{
    if (!g_DebugDiagnosticsEnabled && !g_ForceFailureArtifacts)
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
        IsKnown104Fingerprint(fp) ? "YES" : "NO",
        IsActualKnown104Fingerprint(fp) ? "YES" : "NO",
        ESE_DEV_FORCE_UNKNOWN_BOOTSTRAP ? "YES" : "NO",
        static_cast<long long>(g_RvaShiftHint),
        GeneratedLayoutFingerprintAvailable() ? "YES" : "NO");
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
    AppendMigrationValue(output, "RVA", "RawFPalDamageInfoCaller", Known104::Rva::RawEffectCaller, g_ProfileOutputRvas);
    AppendMigrationValue(output, "RVA", "DamageReaction::AddElementStatusAdditionalValue_OneType.wrapper", Known104::Rva::ElementBuildup, g_ProfileOutputRvas);
    AppendMigrationValue(output, "RVA", "PalStatusComponent::AddStatus.from-buildup", Known104::Rva::AddStatus, g_ProfileOutputRvas);
    AppendMigrationValue(output, "RVA", "CalcElementMatchupTier", Known104::Rva::MatchupHelper, g_ProfileOutputRvas);
    AppendMigrationValue(output, "RVA", "PalVisualEffectComponent::AddVisualEffect", Known104::Rva::AddVisualEffect_Native, g_ProfileOutputRvas);
    AppendMigrationValue(output, "RVA", "PalVisualEffectComponent::AddVisualEffect_Local", Known104::Rva::AddVisualEffectLocal_Native, g_ProfileOutputRvas);
    AppendMigrationValue(output, "RVA", "PalVisualEffectComponent::RemoveVisualEffect_Local", Known104::Rva::RemoveVisualEffectLocal_Native, g_ProfileOutputRvas);
    AppendMigrationValue(output, "RVA", "PalAIActionComponent::SetActionClassParameter", Known104::Rva::SetActionClassParameter_Native, g_ProfileOutputRvas);
    AppendMigrationValue(output, "RVA", "PalActionComponent::PlayAction", Known104::Rva::PlayAction_Native, g_ProfileOutputRvas);
    AppendMigrationValue(output, "RVA", "PalCharacterMovementComponent::SetJumpDisableFlag", Known104::Rva::SetJumpDisableFlag_Native, g_ProfileOutputRvas);
    AppendMigrationValue(output, "RVA", "PalCharacterMovementComponent::SetStepDisableFlag", Known104::Rva::SetStepDisableFlag_Native, g_ProfileOutputRvas);
    AppendMigrationValue(output, "RVA", "PalCharacterMovementComponent::SetMoveDisableFlag", Known104::Rva::SetMoveDisableFlag_Native, g_ProfileOutputRvas);
    AppendMigrationValue(output, "RVA", "PalCharacterMovementComponent::SetWalkSpeedMultiplier", Known104::Rva::SetWalkSpeedMultiplier_Native, g_ProfileOutputRvas);
    AppendMigrationValue(output, "RVA", "PalCharacterMovementComponent::SetYawRotatorMultiplier", Known104::Rva::SetYawRotatorMultiplier_Native, g_ProfileOutputRvas);
    AppendMigrationValue(output, "RVA", "PalShooterComponent::SetDisableAimFlag_Layered", Known104::Rva::SetDisableAimFlag_Native, g_ProfileOutputRvas);
    AppendMigrationValue(output, "RVA", "PalShooterComponent::SetDisableShootFlag_Layered", Known104::Rva::SetDisableShootFlag_Native, g_ProfileOutputRvas);
    AppendMigrationValue(output, "RVA", "PalShooterComponent::SetDisableChangeWeaponFlag_Layered", Known104::Rva::SetDisableChangeWeaponFlag_Native, g_ProfileOutputRvas);

    output += "\n[Wrapper / semantic callsites vs 1.0.4]\n";
    AppendMigrationValue(output, "CALL", "CalcElementMatchupTier.SemanticCallsite", Known104::Rva::MatchupCallsite, g_ProfileOutputCallsites);
    AppendMigrationValue(output, "CALL", "DamageReaction::AddElementStatusAdditionalValue_OneType.wrapper", Known104::Rva::ElementBuildup_Call, g_ProfileOutputCallsites);
    AppendMigrationValue(output, "CALL", "PalStatusComponent::AddStatus.from-buildup", Known104::Rva::AddStatus_FromBuildup_Call, g_ProfileOutputCallsites);
    AppendMigrationValue(output, "CALL", "PalVisualEffectComponent::AddVisualEffect", Known104::Rva::AddVisualEffect_Call, g_ProfileOutputCallsites);
    AppendMigrationValue(output, "CALL", "PalVisualEffectComponent::AddVisualEffect_Local", Known104::Rva::AddVisualEffectLocal_Call, g_ProfileOutputCallsites);
    AppendMigrationValue(output, "CALL", "PalVisualEffectComponent::RemoveVisualEffect_Local", Known104::Rva::RemoveVisualEffectLocal_Call, g_ProfileOutputCallsites);
    AppendMigrationValue(output, "CALL", "PalAIActionComponent::SetActionClassParameter", Known104::Rva::SetActionClassParameter_Call, g_ProfileOutputCallsites);
    AppendMigrationValue(output, "CALL", "PalActionComponent::PlayAction", Known104::Rva::PlayAction_Call, g_ProfileOutputCallsites);
    AppendMigrationValue(output, "CALL", "PalCharacterMovementComponent::SetJumpDisableFlag", Known104::Rva::SetJumpDisableFlag_Call, g_ProfileOutputCallsites);
    AppendMigrationValue(output, "CALL", "PalCharacterMovementComponent::SetStepDisableFlag", Known104::Rva::SetStepDisableFlag_Call, g_ProfileOutputCallsites);
    AppendMigrationValue(output, "CALL", "PalCharacterMovementComponent::SetMoveDisableFlag", Known104::Rva::SetMoveDisableFlag_Call, g_ProfileOutputCallsites);
    AppendMigrationValue(output, "CALL", "PalCharacterMovementComponent::SetWalkSpeedMultiplier", Known104::Rva::SetWalkSpeedMultiplier_Call, g_ProfileOutputCallsites);
    AppendMigrationValue(output, "CALL", "PalCharacterMovementComponent::SetYawRotatorMultiplier", Known104::Rva::SetYawRotatorMultiplier_Call, g_ProfileOutputCallsites);
    AppendMigrationValue(output, "CALL", "PalShooterComponent::SetDisableAimFlag_Layered", Known104::Rva::SetDisableAimFlag_Call, g_ProfileOutputCallsites);
    AppendMigrationValue(output, "CALL", "PalShooterComponent::SetDisableShootFlag_Layered", Known104::Rva::SetDisableShootFlag_Call, g_ProfileOutputCallsites);
    AppendMigrationValue(output, "CALL", "PalShooterComponent::SetDisableChangeWeaponFlag_Layered", Known104::Rva::SetDisableChangeWeaponFlag_Call, g_ProfileOutputCallsites);

    output += "\n[VSlots vs 1.0.4]\n";
    AppendMigrationValue(output, "VSLOT", "VSlot.ActorComponent::SetComponentTickEnabled", Known104::VSlot::SetComponentTickEnabled, g_ProfileOutputVSlots);
    AppendMigrationValue(output, "VSLOT", "VSlot.Character::StopAnimMontage", Known104::VSlot::StopAnimMontage, g_ProfileOutputVSlots);
    AppendMigrationValue(output, "VSLOT", "VSlot.PalStatusBase::TickStatus", Known104::VSlot::StatusTick, g_ProfileOutputVSlots);

    if (!WriteWholeFileDurable(
        ArtifactPathOrFallback(
            g_MigrationReportPath,
            "ElementalSystemExpanded_migration_report.txt"),
        output)) {
        ShipLog("[ElementalSystemExpanded] WARNING: migration report write failed.\n");
    }
}

static void WriteResolverReport(const FBuildFingerprint& fp)
{
    if (!g_DebugDiagnosticsEnabled && !g_ForceFailureArtifacts)
        return;

    FILE* file = nullptr;
#if defined(_MSC_VER)
    fopen_s(&file, ArtifactPathOrFallback(g_ResolverReportPath, "ElementalSystemExpanded_resolver_report.txt"), "w");
#else
    file = fopen(ArtifactPathOrFallback(g_ResolverReportPath, "ElementalSystemExpanded_resolver_report.txt"), "w");
#endif
    if (!file)
        return;

    fprintf(file, "ElementalSystemExpanded resolver report\n");
    fprintf(file, "Baseline semantics: Stage 4.2 core + Stage 6.6.3 status exchange + Darkness/Light 3s cap\n");
    fprintf(file, "ModuleBase=0x%llX\n", static_cast<unsigned long long>(g_ModuleBase));
    fprintf(file, "PE.TimeDateStamp=0x%08lX\n", static_cast<unsigned long>(fp.TimeDateStamp));
    fprintf(file, "PE.SizeOfImage=0x%08lX\n", static_cast<unsigned long>(fp.SizeOfImage));
    fprintf(file, ".text.RVA=0x%08lX\n", static_cast<unsigned long>(fp.TextRva));
    fprintf(file, ".text.Size=0x%08lX\n", static_cast<unsigned long>(fp.TextSize));
    fprintf(file, ".text.FNV1a64=0x%016llX\n", static_cast<unsigned long long>(fp.TextHash));
    fprintf(file, "RvaShiftHint=%+lld\n", static_cast<long long>(g_RvaShiftHint));
    fprintf(file, "Known104Fingerprint=%s\n", IsKnown104Fingerprint(fp) ? "YES" : "NO");
    fprintf(file, "ActualKnown104Fingerprint=%s\n", IsActualKnown104Fingerprint(fp) ? "YES" : "NO");
    fprintf(file, "DevForceUnknownBootstrap=%s\n", ESE_DEV_FORCE_UNKNOWN_BOOTSTRAP ? "YES" : "NO");
    fprintf(file, "ResolverRevision=%s\n", kResolverRevision);
    fprintf(file, "ShooterCacheRepair=%s\n", g_ShooterCacheRepairStatus.c_str());
    fprintf(file, "DryFreezeVSlotProbe=%s\n", g_DryFreezeVSlotProbeStatus.c_str());
    fprintf(file, "BuildProfileCache=%s\n", g_ProfileCacheStatus.c_str());
    fprintf(file, "BuildProfileWrite=%s\n", g_ProfileWriteStatus.c_str());
    fprintf(file, "ArtifactPathMode=%s\n", g_ArtifactPathStatus.c_str());
    fprintf(file, "ArtifactDirectory=%s\n", g_ArtifactDirectory.c_str());
    fprintf(file, "BuildProfilePath=%s\n", ArtifactPathOrFallback(g_ProfilePath, "ElementalSystemExpanded_build_profile_current.txt"));
    fprintf(file, "LegacyBuildProfilePath=%s\n", ArtifactPathOrFallback(g_LegacyProfilePath, "ElementalSystemExpanded_build_profile.txt"));
    fprintf(file, "MigrationReportPath=%s\n", ArtifactPathOrFallback(g_MigrationReportPath, "ElementalSystemExpanded_migration_report.txt"));
    fprintf(file, "UpdateDiagnosticsPath=%s\n", ArtifactPathOrFallback(g_UpdateDiagnosticsPath, "ElementalSystemExpanded_update_diagnostics.txt"));
    fprintf(file, "RuntimeSnapshotPath=%s\n", ArtifactPathOrFallback(g_RuntimeSnapshotPath, "ElementalSystemExpanded_runtime_snapshot.txt"));
    std::string layoutBindingDetail;
    const bool layoutBindingMatch = GeneratedLayoutFingerprintMatches(fp, &layoutBindingDetail);
    fprintf(file, "GeneratedLayoutFingerprintBound=%s\n", GeneratedLayoutFingerprintAvailable() ? "YES" : "NO");
    fprintf(file, "GeneratedLayoutFingerprintMatch=%s\n", layoutBindingMatch ? "YES" : "NO");
    fprintf(file, "GeneratedLayoutFingerprintDetail=%s\n", layoutBindingDetail.c_str());
    fprintf(file, "GeneratedLightImmunityTargetLayout=%s\n",
        ESE_HAS_GENERATED_LIGHT_TARGET_LAYOUT ? "YES" : "NO");
    fprintf(file, "UpdateDiagnosticsInit=%s\n\n", g_DiagnosticsInitStatus.c_str());

    size_t exactCount = 0;
    size_t strongCount = 0;
    size_t runtimeCount = 0;
    size_t profileCount = 0;
    size_t deferredCount = 0;
    size_t failedCount = 0;

    AcquireSRWLockShared(&g_ResolverRecordsLock);
    for (const auto& rec : g_ResolverRecords) {
        switch (rec.Confidence) {
        case EResolveConfidence::Exact: ++exactCount; break;
        case EResolveConfidence::Strong: ++strongCount; break;
        case EResolveConfidence::RuntimeValidated: ++runtimeCount; break;
        case EResolveConfidence::ProfileStatic: ++profileCount; break;
        case EResolveConfidence::Deferred: ++deferredCount; break;
        default: ++failedCount; break;
        }

        fprintf(file,
            "%-46s | %-17s | RVA +0x%llX | %-24s | %s\n",
            rec.Name.c_str(),
            ResolveConfidenceName(rec.Confidence),
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
    for (const auto& rec : g_ResolverRecords) {
        if (rec.Name.rfind("CAPABILITY.", 0) != 0)
            continue;
        fprintf(file, "%-46s | %-17s | %s\n",
            rec.Name.c_str(),
            ResolveConfidenceName(rec.Confidence),
            rec.Detail.c_str());
    }
    ReleaseSRWLockShared(&g_ResolverRecordsLock);

    fprintf(file, "\n[Centralized layout profile]\n");
    fprintf(file, "Pal SDK layout source=%s\n", ESE_HAS_GENERATED_PAL_LAYOUT ? "generated-header" : "embedded-1.0.4-fallback");
    fprintf(file, "UObject.OuterPrivate=<runtime-discovered; static value not authoritative>\n");
    fprintf(file, "FPalDamageInfo.BasePower=0x%llX Attacker=0x%llX AttackType=0x%llX AttackElement=0x%llX\n",
        (unsigned long long)ActiveLayout::DamageInfo_BasePower,
        (unsigned long long)ActiveLayout::DamageInfo_Attacker,
        (unsigned long long)ActiveLayout::DamageInfo_AttackType,
        (unsigned long long)ActiveLayout::DamageInfo_AttackElement);
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
    fprintf(file, "Reflected BP status timers: Burn.DurationTimer_BP=0x%llX Wetness.DurationTimer_BP=0x%llX; runtime semantic validation required before first write\n",
        (unsigned long long)Known104::Offset::StatusBurn_DurationTimerBP,
        (unsigned long long)Known104::Offset::StatusWetness_DurationTimerBP);
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
        (unsigned long long)g_RuntimeUObjectOuterOffset);
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
        (unsigned long long)g_VSlot_SetComponentTickEnabled,
        (unsigned long long)g_VSlot_StopAnimMontage,
        (unsigned long long)g_VSlot_StatusTick);

    fclose(file);
}

static void RecomputeRvaShiftHint()
{
    if (g_AnchorRvaShifts.empty()) {
        g_RvaShiftHint = 0;
        return;
    }

    auto values = g_AnchorRvaShifts;
    std::sort(values.begin(), values.end());
    g_RvaShiftHint = values[values.size() / 2];
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

using CalcElementMatchupTier_t =
int(__fastcall*)(
    EPalElementType AttackElementType,
    EPalElementType DefenceTypeA,
    EPalElementType DefenceTypeB);

static CalcElementMatchupTier_t
Original_CalcElementMatchupTier = nullptr;

static int __fastcall Detour_CalcElementMatchupTier(
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
static bool IsReadableAddress(uintptr_t address, size_t size)
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


static void InitializeUpdateDiagnostics(const FBuildFingerprint& fp)
{
    if (!g_DebugDiagnosticsEnabled) {
        g_DiagnosticsInitStatus = "disabled-no-debug-marker";
        return;
    }

    FILE* file = nullptr;
#if defined(_MSC_VER)
    fopen_s(&file, ArtifactPathOrFallback(g_UpdateDiagnosticsPath, "ElementalSystemExpanded_update_diagnostics.txt"), "w");
#else
    file = fopen(ArtifactPathOrFallback(g_UpdateDiagnosticsPath, "ElementalSystemExpanded_update_diagnostics.txt"), "w");
#endif
    if (!file) {
        char status[96]{};
        snprintf(status, sizeof(status), "open-failed errno=%d", errno);
        g_DiagnosticsInitStatus = status;
        ShipLog("[ElementalSystemExpanded] WARNING: update diagnostics open failed: %s path=%s\n",
            g_DiagnosticsInitStatus.c_str(),
            ArtifactPathOrFallback(g_UpdateDiagnosticsPath, "ElementalSystemExpanded_update_diagnostics.txt"));
        return;
    }
    g_DiagnosticsInitStatus = "created";
    fprintf(file, "ElementalSystemExpanded update diagnostics\n");
    fprintf(file, "TimeDateStamp=0x%08lX SizeOfImage=0x%08lX TextHash=0x%016llX\n",
        static_cast<unsigned long>(fp.TimeDateStamp),
        static_cast<unsigned long>(fp.SizeOfImage),
        static_cast<unsigned long long>(fp.TextHash));
    fprintf(file, "Known104=%s\n\n", IsKnown104Fingerprint(fp) ? "YES" : "NO");
    fclose(file);
}

static void AppendDiagnosticWindow(
    const char* name,
    const char* reason,
    uintptr_t centerRva)
{
    if (!g_DebugDiagnosticsEnabled ||
        !g_ModuleBase ||
        !centerRva) {
        return;
    }

    const uintptr_t radiusBefore = 0x30;
    const size_t dumpSize = 0x90;
    const uintptr_t startRva = centerRva > radiusBefore
        ? centerRva - radiusBefore
        : 0;
    const uintptr_t address = g_ModuleBase + startRva;

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
    fopen_s(&file, ArtifactPathOrFallback(g_UpdateDiagnosticsPath, "ElementalSystemExpanded_update_diagnostics.txt"), "a");
#else
    file = fopen(ArtifactPathOrFallback(g_UpdateDiagnosticsPath, "ElementalSystemExpanded_update_diagnostics.txt"), "a");
#endif
    if (!file)
        return;

    fprintf(file, "[%s]\nReason=%s\nCenterRVA=+0x%llX RvaShiftHint=%+lld\n",
        name ? name : "unnamed",
        reason ? reason : "unspecified",
        static_cast<unsigned long long>(centerRva),
        static_cast<long long>(g_RvaShiftHint));

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

static bool IsExecutableAddress(uintptr_t address)
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

static uintptr_t ResolveRel32Call(uintptr_t callSite)
{
    if (!IsReadableAddress(callSite, 5))
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

static bool MatchMaskedBytes(
    uintptr_t address,
    const uint8_t* pattern,
    const char* mask)
{
    if (!address || !pattern || !mask)
        return false;

    const size_t length = strlen(mask);
    if (!length || !IsReadableAddress(address, length))
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

struct FExecutableSectionRange {
    uintptr_t Rva = 0;
    uintptr_t Size = 0;
};

static bool SnapshotExecutableSections(
    uintptr_t moduleBase,
    FExecutableSectionRange* outSections,
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

static std::vector<uintptr_t> FindPatternInExecutableSections(
    uintptr_t moduleBase,
    const uint8_t* pattern,
    const char* mask,
    size_t maxMatches)
{
    std::vector<uintptr_t> matches;
    if (!moduleBase || !pattern || !mask || !mask[0] || maxMatches == 0)
        return matches;

    const size_t patternLen = strlen(mask);
    FExecutableSectionRange sections[96]{};
    size_t sectionCount = 0;
    uintptr_t imageSize = 0;
    if (!SnapshotExecutableSections(
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
        if (!IsReadableAddress(begin, patternLen))
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

static std::vector<uintptr_t> FindPatternNearRva(
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
    FExecutableSectionRange sections[96]{};
    size_t sectionCount = 0;
    uintptr_t imageSize = 0;
    if (!SnapshotExecutableSections(
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
        if (!IsReadableAddress(begin, patternLen))
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

static uintptr_t ChoosePatternNearestHint(
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

static uintptr_t ResolvePatternTarget(
    const char* name,
    uintptr_t moduleBase,
    uintptr_t knownRva,
    const uint8_t* pattern,
    const char* mask,
    bool useAsShiftAnchor)
{
    const uintptr_t cachedRva = CachedProfileValue(g_BuildProfileCache.Rvas, name);
    if (cachedRva) {
        const uintptr_t cachedAddress = moduleBase + cachedRva;
        if (MatchMaskedBytes(cachedAddress, pattern, mask)) {
            RecordResolver(
                name, "fingerprint-profile+pattern", EResolveConfidence::ProfileStatic,
                cachedAddress, "cached RVA revalidated against exact build fingerprint");
            CacheResolvedRva(name, cachedAddress);
            return cachedAddress;
        }
    }

    const uintptr_t knownAddress = moduleBase + knownRva;
    if (IsKnown104Fingerprint(g_CurrentFingerprint) &&
        MatchMaskedBytes(knownAddress, pattern, mask)) {
        if (useAsShiftAnchor)
            g_AnchorRvaShifts.push_back(0);
        RecordResolver(
            name, "known-rva+pattern", EResolveConfidence::Exact,
            knownAddress, "exact 1.0.4 fingerprint + byte pattern verified");
        CacheResolvedRva(name, knownAddress);
        return knownAddress;
    }

    const auto matches = FindPatternInExecutableSections(
        moduleBase, pattern, mask, 3);

    if (matches.size() != 1) {
        char detail[96]{};
        snprintf(detail, sizeof(detail), "module-wide matches=%llu",
            static_cast<unsigned long long>(matches.size()));
        RecordResolver(
            name, "unique-aob", EResolveConfidence::Failed,
            0, detail);
        const intptr_t shifted = static_cast<intptr_t>(knownRva) + g_RvaShiftHint;
        AppendDiagnosticWindow(name, detail, shifted > 0 ? static_cast<uintptr_t>(shifted) : knownRva);
        return 0;
    }

    const uintptr_t found = matches[0];
    if (useAsShiftAnchor) {
        const intptr_t shift = static_cast<intptr_t>(found - moduleBase) -
            static_cast<intptr_t>(knownRva);
        g_AnchorRvaShifts.push_back(shift);
        RecomputeRvaShiftHint();
    }

    char detail[128]{};
    snprintf(detail, sizeof(detail), "old=+0x%llX shift=%+lld",
        static_cast<unsigned long long>(knownRva),
        static_cast<long long>(
            static_cast<intptr_t>(found - moduleBase) - static_cast<intptr_t>(knownRva)));

    RecordResolver(
        name, "unique-aob", EResolveConfidence::Strong,
        found, detail);
    CacheResolvedRva(name, found);
    return found;
}

static uintptr_t ResolveNativeFromWrapperCall(
    const char* name,
    uintptr_t moduleBase,
    uintptr_t knownCallRva,
    uintptr_t knownNativeRva,
    const uint8_t* callsitePattern,
    const char* callsiteMask,
    size_t callOffset)
{
    const uintptr_t cachedCallRva = CachedProfileValue(
        g_BuildProfileCache.Callsites, name);
    if (cachedCallRva && cachedCallRva >= callOffset) {
        const uintptr_t cachedPatternStart =
            moduleBase + cachedCallRva - callOffset;
        if (MatchMaskedBytes(cachedPatternStart, callsitePattern, callsiteMask)) {
            const uintptr_t cachedCallsite = cachedPatternStart + callOffset;
            const uintptr_t cachedTarget = ResolveRel32Call(cachedCallsite);
            if (cachedTarget && IsExecutableAddress(cachedTarget)) {
                RecordResolver(
                    name, "fingerprint-profile-wrapper-rel32",
                    EResolveConfidence::ProfileStatic,
                    cachedTarget,
                    "cached wrapper callsite revalidated against exact build fingerprint");
                CacheResolvedCallsite(name, cachedCallsite);
                CacheResolvedRva(name, cachedTarget);
                return cachedTarget;
            }
        }
    }

    const uintptr_t knownPatternStart =
        moduleBase + knownCallRva - callOffset;

    // Fast path is authoritative only on the exact known build. On an unknown
    // executable the historical location remains a search hint only.
    if (IsKnown104Fingerprint(g_CurrentFingerprint) &&
        MatchMaskedBytes(knownPatternStart, callsitePattern, callsiteMask)) {
        const uintptr_t callSite = knownPatternStart + callOffset;
        const uintptr_t target = ResolveRel32Call(callSite);
        if (target && IsExecutableAddress(target)) {
            const bool exactTarget =
                (target == moduleBase + knownNativeRva);
            char detail[128]{};
            snprintf(detail, sizeof(detail),
                "wrapper=+0x%llX native-old=+0x%llX%s",
                static_cast<unsigned long long>(knownCallRva),
                static_cast<unsigned long long>(knownNativeRva),
                exactTarget ? "" : " target-rva-drifted");
            RecordResolver(
                name,
                "wrapper-rel32-fast",
                exactTarget ? EResolveConfidence::Exact : EResolveConfidence::Strong,
                target,
                detail);
            CacheResolvedCallsite(name, callSite);
            CacheResolvedRva(name, target);
            return target;
        }
    }

    // Unknown/minor build: use the anchor-derived global shift only as a hint,
    // search a bounded neighborhood, then decode the wrapper's own rel32 CALL.
    const intptr_t shifted =
        static_cast<intptr_t>(knownCallRva) + g_RvaShiftHint;
    const uintptr_t expectedCallRva = shifted > 0
        ? static_cast<uintptr_t>(shifted)
        : knownCallRva;
    const uintptr_t expectedPatternRva =
        expectedCallRva > callOffset ? expectedCallRva - callOffset : 0;

    auto matches = FindPatternNearRva(
        moduleBase,
        expectedPatternRva,
        0x40000,
        callsitePattern,
        callsiteMask,
        32);

    uintptr_t distance = 0;
    const uintptr_t chosen = ChoosePatternNearestHint(
        matches,
        moduleBase,
        expectedPatternRva,
        0x80,
        &distance);

    uintptr_t selected = chosen;
    const char* selectionMethod = "wrapper-near-aob+rel32";
    if (!selected) {
        const auto globalMatches = FindPatternInExecutableSections(
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
                static_cast<long long>(g_RvaShiftHint));
            RecordResolver(name, "wrapper-aob", EResolveConfidence::Failed, 0, detail);
            AppendDiagnosticWindow(name, detail, expectedCallRva);
            return 0;
        }
    }

    const uintptr_t callSite = selected + callOffset;
    const uintptr_t target = ResolveRel32Call(callSite);
    if (!target || !IsExecutableAddress(target)) {
        RecordResolver(
            name, "wrapper-near-aob", EResolveConfidence::Failed,
            0, "decoded target is not executable");
        AppendDiagnosticWindow(name, "decoded target is not executable", callSite - moduleBase);
        return 0;
    }

    char detail[160]{};
    snprintf(detail, sizeof(detail),
        "callsite=+0x%llX old-call=+0x%llX hint-distance=0x%llX",
        static_cast<unsigned long long>(callSite - moduleBase),
        static_cast<unsigned long long>(knownCallRva),
        static_cast<unsigned long long>(distance));

    RecordResolver(
        name, selectionMethod, EResolveConfidence::Strong,
        target, detail);
    CacheResolvedCallsite(name, callSite);
    CacheResolvedRva(name, target);
    return target;
}



static uintptr_t ResolveNativeFromForwardSiblingCluster(
    const char* name,
    uintptr_t moduleBase,
    const char* anchorName,
    uintptr_t knownAnchorCallRva,
    uintptr_t knownTargetCallRva,
    uintptr_t knownAnchorNativeRva,
    uintptr_t knownTargetNativeRva)
{
    if (!name || !anchorName || !moduleBase)
        return 0;

    const auto anchorCallIt =
        g_ProfileOutputCallsites.find(anchorName);
    const auto anchorTargetIt =
        g_ProfileOutputRvas.find(anchorName);

    if (anchorCallIt == g_ProfileOutputCallsites.end() ||
        anchorTargetIt == g_ProfileOutputRvas.end()) {
        RecordResolver(
            name,
            "forward-sibling-native-cluster+rel32",
            EResolveConfidence::Failed,
            0,
            "resolved sibling anchor is unavailable");
        return 0;
    }

    if (knownTargetCallRva <= knownAnchorCallRva ||
        knownTargetNativeRva == knownAnchorNativeRva) {
        RecordResolver(
            name,
            "forward-sibling-native-cluster+rel32",
            EResolveConfidence::Failed,
            0,
            "invalid historical sibling relationship");
        return 0;
    }

    const uintptr_t anchorCallRva = anchorCallIt->second;

    // g_ProfileOutputRvas stores RVAs, not absolute process addresses.
    // Rebase the already-resolved sibling target before executable-address
    // validation and native-cluster distance comparisons.
    const uintptr_t anchorTargetRva = anchorTargetIt->second;
    const uintptr_t anchorTarget = moduleBase + anchorTargetRva;

    if (!anchorCallRva ||
        !anchorTarget ||
        !IsExecutableAddress(anchorTarget)) {
        RecordResolver(
            name,
            "forward-sibling-native-cluster+rel32",
            EResolveConfidence::Failed,
            0,
            "resolved sibling anchor is not executable");
        return 0;
    }

    const uintptr_t knownCallDelta =
        knownTargetCallRva - knownAnchorCallRva;

    const uintptr_t knownNativeDelta =
        (knownTargetNativeRva > knownAnchorNativeRva)
        ? (knownTargetNativeRva - knownAnchorNativeRva)
        : (knownAnchorNativeRva - knownTargetNativeRva);

    // Derive both search bounds from the known sibling relationship rather than
    // from a PalServer RVA. Allow modest compiler-layout drift while keeping the
    // search local to the same reflected-wrapper/native-setter families.
    const uintptr_t callWindow =
        (knownCallDelta < 0x80)
        ? 0x100
        : knownCallDelta * 2;

    const uintptr_t nativeWindow =
        (knownNativeDelta < 0x80)
        ? 0x200
        : knownNativeDelta * 4;

    const bool historicalNativeForward =
        knownTargetNativeRva > knownAnchorNativeRva;

    const uintptr_t searchBegin = anchorCallRva + 5;
    const uintptr_t searchEnd = anchorCallRva + callWindow;

    uintptr_t clusterMatchedCallRva = 0;
    uintptr_t clusterMatchedTarget = 0;
    size_t clusterCandidateCount = 0;

    // Stage 6.7.4.8: the broad local cluster can contain several adjacent
    // reflected movement-setter wrappers. Preserve the broad scan, but use the
    // historical WalkSpeed->Yaw native-implementation delta as an independent
    // semantic discriminator. This is relative to the already-resolved current
    // WalkSpeed native target; no current-build PalServer RVA is hardcoded.
    uintptr_t deltaMatchedCallRva = 0;
    uintptr_t deltaMatchedTarget = 0;
    size_t deltaCandidateCount = 0;

    for (uintptr_t candidateRva = searchBegin;
         candidateRva <= searchEnd;
         ++candidateRva) {

        const uintptr_t candidateSite =
            moduleBase + candidateRva;

        if (!IsReadableAddress(candidateSite, 5))
            continue;

        if (*reinterpret_cast<const uint8_t*>(candidateSite) != 0xE8)
            continue;

        const uintptr_t candidateTarget =
            ResolveRel32Call(candidateSite);

        if (!candidateTarget ||
            candidateTarget == anchorTarget ||
            !IsExecutableAddress(candidateTarget))
            continue;

        const uintptr_t nativeDistance =
            (candidateTarget > anchorTarget)
            ? (candidateTarget - anchorTarget)
            : (anchorTarget - candidateTarget);

        if (!nativeDistance ||
            nativeDistance > nativeWindow)
            continue;

        const bool candidateNativeForward =
            candidateTarget > anchorTarget;

        if (candidateNativeForward != historicalNativeForward)
            continue;

        // Final reflected-wrapper native dispatches are followed immediately by
        // a short stack/register epilogue and RET. Require a RET very near the
        // CALL so helper calls inside the same wrapper cannot qualify.
        bool nearbyRet = false;
        for (uintptr_t k = 5; k <= 0x20; ++k) {
            const uintptr_t p = candidateSite + k;
            if (!IsReadableAddress(p, 1))
                break;
            if (*reinterpret_cast<const uint8_t*>(p) == 0xC3) {
                nearbyRet = true;
                break;
            }
        }

        if (!nearbyRet)
            continue;

        ++clusterCandidateCount;
        clusterMatchedCallRva = candidateRva;
        clusterMatchedTarget = candidateTarget;

        if (nativeDistance == knownNativeDelta) {
            ++deltaCandidateCount;
            deltaMatchedCallRva = candidateRva;
            deltaMatchedTarget = candidateTarget;
        }
    }

    uintptr_t matchedCallRva = 0;
    uintptr_t matchedTarget = 0;
    const char* selectionMethod = nullptr;

    // Prefer the unique candidate that preserves the independently-known native
    // sibling delta. If that relationship changes on a future build, retain the
    // old unique-cluster behavior; otherwise fail closed rather than guessing.
    if (deltaCandidateCount == 1 &&
        deltaMatchedCallRva &&
        deltaMatchedTarget) {
        matchedCallRva = deltaMatchedCallRva;
        matchedTarget = deltaMatchedTarget;
        selectionMethod = "forward-sibling-native-delta+rel32";
    }
    else if (clusterCandidateCount == 1 &&
        clusterMatchedCallRva &&
        clusterMatchedTarget) {
        matchedCallRva = clusterMatchedCallRva;
        matchedTarget = clusterMatchedTarget;
        selectionMethod = "forward-sibling-native-cluster+rel32";
    }

    if (!selectionMethod ||
        !matchedCallRva ||
        !matchedTarget) {
        char detail[256]{};
        snprintf(
            detail,
            sizeof(detail),
            "anchor=%s@+0x%llX callWindow=0x%llX nativeWindow=0x%llX clusterCandidates=%llu exactNativeDeltaCandidates=%llu expectedNativeDelta=0x%llX",
            anchorName,
            static_cast<unsigned long long>(anchorCallRva),
            static_cast<unsigned long long>(callWindow),
            static_cast<unsigned long long>(nativeWindow),
            static_cast<unsigned long long>(clusterCandidateCount),
            static_cast<unsigned long long>(deltaCandidateCount),
            static_cast<unsigned long long>(knownNativeDelta));

        RecordResolver(
            name,
            "forward-sibling-native-delta+rel32",
            EResolveConfidence::Failed,
            0,
            detail);

        AppendDiagnosticWindow(
            name,
            detail,
            anchorCallRva + knownCallDelta);
        return 0;
    }

    char detail[352]{};
    snprintf(
        detail,
        sizeof(detail),
        "anchor=%s call=+0x%llX target=+0x%llX; matched-call=+0x%llX matched-target=+0x%llX callDelta=0x%llX nativeDelta=0x%llX expectedCallDelta=0x%llX expectedNativeDelta=0x%llX clusterCandidates=%llu exactNativeDeltaCandidates=%llu",
        anchorName,
        static_cast<unsigned long long>(anchorCallRva),
        static_cast<unsigned long long>(anchorTarget - moduleBase),
        static_cast<unsigned long long>(matchedCallRva),
        static_cast<unsigned long long>(matchedTarget - moduleBase),
        static_cast<unsigned long long>(matchedCallRva - anchorCallRva),
        static_cast<unsigned long long>(
            matchedTarget > anchorTarget
            ? matchedTarget - anchorTarget
            : anchorTarget - matchedTarget),
        static_cast<unsigned long long>(knownCallDelta),
        static_cast<unsigned long long>(knownNativeDelta),
        static_cast<unsigned long long>(clusterCandidateCount),
        static_cast<unsigned long long>(deltaCandidateCount));

    RecordResolver(
        name,
        selectionMethod,
        EResolveConfidence::Strong,
        matchedTarget,
        detail);

    CacheResolvedCallsite(
        name,
        moduleBase + matchedCallRva);
    CacheResolvedRva(
        name,
        matchedTarget);

    return matchedTarget;
}

static uintptr_t ResolveNativeFromShooterSiblingTopology(
    const char* name,
    uintptr_t moduleBase,
    const char* beforeName,
    const char* afterName,
    uintptr_t knownBeforeCallRva,
    uintptr_t knownTargetCallRva,
    uintptr_t knownAfterCallRva,
    uintptr_t knownBeforeNativeRva,
    uintptr_t knownTargetNativeRva,
    uintptr_t knownAfterNativeRva)
{
    static constexpr const char* kMethod =
        "shooter-sibling-call+native-topology+bounded-rel32";

    if (!name || !beforeName || !afterName || !moduleBase)
        return 0;

    const auto beforeCallIt = g_ProfileOutputCallsites.find(beforeName);
    const auto afterCallIt = g_ProfileOutputCallsites.find(afterName);
    const auto beforeNativeIt = g_ProfileOutputRvas.find(beforeName);
    const auto afterNativeIt = g_ProfileOutputRvas.find(afterName);

    if (beforeCallIt == g_ProfileOutputCallsites.end() ||
        afterCallIt == g_ProfileOutputCallsites.end() ||
        beforeNativeIt == g_ProfileOutputRvas.end() ||
        afterNativeIt == g_ProfileOutputRvas.end()) {
        RecordResolver(
            name,
            kMethod,
            EResolveConfidence::Failed,
            0,
            "required shooter sibling anchors are unavailable");
        return 0;
    }

    if (knownTargetCallRva <= knownBeforeCallRva ||
        knownAfterCallRva <= knownTargetCallRva ||
        knownTargetNativeRva <= knownBeforeNativeRva ||
        knownAfterNativeRva <= knownTargetNativeRva) {
        RecordResolver(
            name,
            kMethod,
            EResolveConfidence::Failed,
            0,
            "invalid historical shooter sibling ordering");
        return 0;
    }

    const uintptr_t currentBeforeCallRva = beforeCallIt->second;
    const uintptr_t currentAfterCallRva = afterCallIt->second;
    const uintptr_t currentBeforeNativeRva = beforeNativeIt->second;
    const uintptr_t currentAfterNativeRva = afterNativeIt->second;

    if (currentAfterCallRva <= currentBeforeCallRva ||
        currentAfterNativeRva <= currentBeforeNativeRva) {
        RecordResolver(
            name,
            kMethod,
            EResolveConfidence::Failed,
            0,
            "current shooter sibling ordering is invalid");
        return 0;
    }

    const uintptr_t knownCallSpan =
        knownAfterCallRva - knownBeforeCallRva;
    const uintptr_t currentCallSpan =
        currentAfterCallRva - currentBeforeCallRva;
    const uintptr_t knownNativeSpan =
        knownAfterNativeRva - knownBeforeNativeRva;
    const uintptr_t currentNativeSpan =
        currentAfterNativeRva - currentBeforeNativeRva;

    // Aim and Shoot are independently resolved before ChangeWeapon. On the
    // validated family their endpoint call/native spans are exact semantic
    // topology invariants. Refuse to infer the middle sibling if either family
    // span changed.
    if (currentCallSpan != knownCallSpan ||
        currentNativeSpan != knownNativeSpan) {
        char detail[256]{};
        snprintf(
            detail,
            sizeof(detail),
            "endpoint span mismatch call current=0x%llX historical=0x%llX; native current=0x%llX historical=0x%llX",
            static_cast<unsigned long long>(currentCallSpan),
            static_cast<unsigned long long>(knownCallSpan),
            static_cast<unsigned long long>(currentNativeSpan),
            static_cast<unsigned long long>(knownNativeSpan));
        RecordResolver(
            name,
            kMethod,
            EResolveConfidence::Failed,
            0,
            detail);
        return 0;
    }

    const uintptr_t knownBeforeToTargetCall =
        knownTargetCallRva - knownBeforeCallRva;
    const uintptr_t knownTargetToAfterCall =
        knownAfterCallRva - knownTargetCallRva;
    const uintptr_t knownBeforeToTargetNative =
        knownTargetNativeRva - knownBeforeNativeRva;
    const uintptr_t knownTargetToAfterNative =
        knownAfterNativeRva - knownTargetNativeRva;

    const uintptr_t predictedCallFromBefore =
        currentBeforeCallRva + knownBeforeToTargetCall;
    const uintptr_t predictedCallFromAfter =
        currentAfterCallRva - knownTargetToAfterCall;
    const uintptr_t predictedNativeFromBefore =
        currentBeforeNativeRva + knownBeforeToTargetNative;
    const uintptr_t predictedNativeFromAfter =
        currentAfterNativeRva - knownTargetToAfterNative;

    if (predictedCallFromBefore != predictedCallFromAfter ||
        predictedNativeFromBefore != predictedNativeFromAfter) {
        RecordResolver(
            name,
            kMethod,
            EResolveConfidence::Failed,
            0,
            "independent shooter sibling predictions disagree");
        return 0;
    }

    const uintptr_t predictedCallRva = predictedCallFromBefore;
    const uintptr_t predictedNativeRva = predictedNativeFromBefore;
    const uintptr_t predictedNative = moduleBase + predictedNativeRva;

    if (!IsExecutableAddress(predictedNative)) {
        RecordResolver(
            name,
            kMethod,
            EResolveConfidence::Failed,
            0,
            "predicted shooter native target is not executable");
        return 0;
    }

    // Preserve the successful Move-resolver lesson: compiler instruction
    // selection may move a final CALL by a few bytes even when sibling topology
    // remains stable. Search only a tiny bounded neighborhood and require that
    // the decoded target equals the independently predicted native target.
    static constexpr uintptr_t kCallDrift = 8;
    uintptr_t matchedCallRva = 0;
    size_t candidateCount = 0;

    const uintptr_t searchBegin =
        predictedCallRva > kCallDrift
        ? predictedCallRva - kCallDrift
        : 0;
    const uintptr_t searchEnd = predictedCallRva + kCallDrift;

    for (uintptr_t candidateRva = searchBegin;
         candidateRva <= searchEnd;
         ++candidateRva) {
        const uintptr_t candidateSite = moduleBase + candidateRva;
        if (!IsReadableAddress(candidateSite, 5))
            continue;
        if (*reinterpret_cast<const uint8_t*>(candidateSite) != 0xE8)
            continue;

        const uintptr_t candidateTarget =
            ResolveRel32Call(candidateSite);
        if (candidateTarget != predictedNative)
            continue;

        ++candidateCount;
        matchedCallRva = candidateRva;
    }

    if (candidateCount != 1 || !matchedCallRva) {
        char detail[320]{};
        snprintf(
            detail,
            sizeof(detail),
            "before=%s call=+0x%llX native=+0x%llX; after=%s call=+0x%llX native=+0x%llX; predicted-call=+0x%llX predicted-native=+0x%llX candidates=%llu",
            beforeName,
            static_cast<unsigned long long>(currentBeforeCallRva),
            static_cast<unsigned long long>(currentBeforeNativeRva),
            afterName,
            static_cast<unsigned long long>(currentAfterCallRva),
            static_cast<unsigned long long>(currentAfterNativeRva),
            static_cast<unsigned long long>(predictedCallRva),
            static_cast<unsigned long long>(predictedNativeRva),
            static_cast<unsigned long long>(candidateCount));
        RecordResolver(
            name,
            kMethod,
            EResolveConfidence::Failed,
            0,
            detail);
        AppendDiagnosticWindow(
            name,
            detail,
            predictedCallRva);
        return 0;
    }

    char detail[384]{};
    const long long drift =
        static_cast<long long>(matchedCallRva) -
        static_cast<long long>(predictedCallRva);
    snprintf(
        detail,
        sizeof(detail),
        "before=%s call=+0x%llX native=+0x%llX; after=%s call=+0x%llX native=+0x%llX; matched-call=+0x%llX predicted-call=+0x%llX drift=%+lld matched-target=+0x%llX callSpan=0x%llX nativeSpan=0x%llX unique=YES",
        beforeName,
        static_cast<unsigned long long>(currentBeforeCallRva),
        static_cast<unsigned long long>(currentBeforeNativeRva),
        afterName,
        static_cast<unsigned long long>(currentAfterCallRva),
        static_cast<unsigned long long>(currentAfterNativeRva),
        static_cast<unsigned long long>(matchedCallRva),
        static_cast<unsigned long long>(predictedCallRva),
        drift,
        static_cast<unsigned long long>(predictedNativeRva),
        static_cast<unsigned long long>(currentCallSpan),
        static_cast<unsigned long long>(currentNativeSpan));

    RecordResolver(
        name,
        kMethod,
        EResolveConfidence::Strong,
        predictedNative,
        detail);
    CacheResolvedCallsite(name, moduleBase + matchedCallRva);
    CacheResolvedRva(name, predictedNative);
    return predictedNative;
}

static uintptr_t ResolveNativeFromSiblingWrapperTopology(
    const char* name,
    uintptr_t moduleBase,
    const char* beforeName,
    const char* afterName,
    uintptr_t knownBeforeCallRva,
    uintptr_t knownTargetCallRva,
    uintptr_t knownAfterCallRva)
{
    if (!name || !beforeName || !afterName || !moduleBase)
        return 0;

    const auto beforeIt = g_ProfileOutputCallsites.find(beforeName);
    const auto afterIt = g_ProfileOutputCallsites.find(afterName);
    if (beforeIt == g_ProfileOutputCallsites.end() ||
        afterIt == g_ProfileOutputCallsites.end()) {
        RecordResolver(
            name,
            "sibling-wrapper-topology+rel32",
            EResolveConfidence::Failed,
            0,
            "required sibling callsite anchors are unavailable");
        return 0;
    }

    const uintptr_t currentBefore = beforeIt->second;
    const uintptr_t currentAfter = afterIt->second;

    if (knownTargetCallRva <= knownBeforeCallRva ||
        knownAfterCallRva <= knownTargetCallRva ||
        currentAfter <= currentBefore) {
        RecordResolver(
            name,
            "sibling-wrapper-topology+rel32",
            EResolveConfidence::Failed,
            0,
            "invalid sibling ordering");
        return 0;
    }

    const uintptr_t knownBeforeToTarget =
        knownTargetCallRva - knownBeforeCallRva;
    const uintptr_t knownTargetToAfter =
        knownAfterCallRva - knownTargetCallRva;
    const uintptr_t knownSpan =
        knownAfterCallRva - knownBeforeCallRva;
    const uintptr_t currentSpan =
        currentAfter - currentBefore;

    // The sibling wrapper topology itself is the semantic invariant. Historical
    // absolute RVAs are never used as runtime addresses.
    if (currentSpan != knownSpan) {
        char detail[192]{};
        snprintf(
            detail,
            sizeof(detail),
            "sibling span mismatch current=0x%llX historical=0x%llX before=+0x%llX after=+0x%llX",
            static_cast<unsigned long long>(currentSpan),
            static_cast<unsigned long long>(knownSpan),
            static_cast<unsigned long long>(currentBefore),
            static_cast<unsigned long long>(currentAfter));
        RecordResolver(
            name,
            "sibling-wrapper-topology+rel32",
            EResolveConfidence::Failed,
            0,
            detail);
        return 0;
    }

    const uintptr_t predictedFromBefore =
        currentBefore + knownBeforeToTarget;
    const uintptr_t predictedFromAfter =
        currentAfter - knownTargetToAfter;

    if (predictedFromBefore != predictedFromAfter) {
        RecordResolver(
            name,
            "sibling-wrapper-topology+rel32",
            EResolveConfidence::Failed,
            0,
            "independent sibling predictions disagree");
        return 0;
    }

    // Resolve the two independently proven sibling native implementations.
    const uintptr_t beforeTarget =
        ResolveRel32Call(moduleBase + currentBefore);
    const uintptr_t afterTarget =
        ResolveRel32Call(moduleBase + currentAfter);
    if (!beforeTarget || !afterTarget ||
        !IsExecutableAddress(beforeTarget) ||
        !IsExecutableAddress(afterTarget) ||
        beforeTarget == afterTarget) {
        RecordResolver(
            name,
            "sibling-wrapper-topology+bounded-rel32",
            EResolveConfidence::Failed,
            0,
            "resolved sibling callsites do not decode to distinct executable targets");
        return 0;
    }

    // The wrapper-family span is stable, but compiler instruction selection can
    // move the inner native CALL by a few bytes. Search only a tiny bounded
    // neighborhood around the independently predicted position. This is not a
    // general AOB scan: candidate CALLs must also land between the two resolved
    // sibling native implementations, preserving the sibling native topology.
    static constexpr uintptr_t kSiblingCallDrift = 8;

    const uintptr_t predictedCallRva = predictedFromBefore;
    const uintptr_t searchBegin =
        (predictedCallRva >= kSiblingCallDrift)
        ? predictedCallRva - kSiblingCallDrift
        : predictedCallRva;
    const uintptr_t searchEnd =
        predictedCallRva + kSiblingCallDrift;

    const uintptr_t nativeLow =
        (beforeTarget < afterTarget) ? beforeTarget : afterTarget;
    const uintptr_t nativeHigh =
        (beforeTarget < afterTarget) ? afterTarget : beforeTarget;

    uintptr_t matchedCallRva = 0;
    uintptr_t matchedTarget = 0;
    size_t candidateCount = 0;

    for (uintptr_t candidateRva = searchBegin;
         candidateRva <= searchEnd;
         ++candidateRva) {
        const uintptr_t candidateSite = moduleBase + candidateRva;
        if (!IsReadableAddress(candidateSite, 5))
            continue;

        if (*reinterpret_cast<const uint8_t*>(candidateSite) != 0xE8)
            continue;

        const uintptr_t candidateTarget =
            ResolveRel32Call(candidateSite);
        if (!candidateTarget ||
            !IsExecutableAddress(candidateTarget))
            continue;

        if (candidateTarget == beforeTarget ||
            candidateTarget == afterTarget)
            continue;

        if (candidateTarget <= nativeLow ||
            candidateTarget >= nativeHigh)
            continue;

        ++candidateCount;
        matchedCallRva = candidateRva;
        matchedTarget = candidateTarget;
    }

    if (candidateCount != 1 || !matchedCallRva || !matchedTarget) {
        char detail[192]{};
        snprintf(
            detail,
            sizeof(detail),
            "bounded sibling CALL search failed predicted=+0x%llX window=+/-0x%llX candidates=%llu",
            static_cast<unsigned long long>(predictedCallRva),
            static_cast<unsigned long long>(kSiblingCallDrift),
            static_cast<unsigned long long>(candidateCount));
        RecordResolver(
            name,
            "sibling-wrapper-topology+bounded-rel32",
            EResolveConfidence::Failed,
            0,
            detail);
        AppendDiagnosticWindow(
            name,
            detail,
            predictedCallRva);
        return 0;
    }

    const uintptr_t callRva = matchedCallRva;
    const uintptr_t callSite = moduleBase + callRva;
    const uintptr_t target = matchedTarget;

    const long long drift =
        static_cast<long long>(callRva) -
        static_cast<long long>(predictedCallRva);

    char detail[288]{};
    snprintf(
        detail,
        sizeof(detail),
        "before=%s@+0x%llX; after=%s@+0x%llX; predicted=+0x%llX matched=+0x%llX drift=%lld target-between-siblings=YES",
        beforeName,
        static_cast<unsigned long long>(currentBefore),
        afterName,
        static_cast<unsigned long long>(currentAfter),
        static_cast<unsigned long long>(predictedCallRva),
        static_cast<unsigned long long>(callRva),
        drift);

    RecordResolver(
        name,
        "sibling-wrapper-topology+bounded-rel32",
        EResolveConfidence::Strong,
        target,
        detail);
    CacheResolvedCallsite(name, callSite);
    CacheResolvedRva(name, target);
    return target;
}

static uintptr_t ResolveCallInsideResolvedFunction(
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
        scanSize > (std::numeric_limits<uintptr_t>::max)() - functionStart)
        return 0;

    std::vector<uintptr_t> matches;
    const uintptr_t end = functionStart + scanSize;
    for (uintptr_t at = functionStart;
        at + patternLen <= end;
        ++at) {
        if (!MatchMaskedBytes(at, localPattern, localMask))
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
        RecordResolver(
            name, "semantic-local-call", EResolveConfidence::Failed,
            0, detail);
        if (g_ModuleBase && functionStart >= g_ModuleBase)
            AppendDiagnosticWindow(name, detail, functionStart - g_ModuleBase);
        return 0;
    }

    const uintptr_t callSite = matches[0] + callOffset;
    const uintptr_t target = ResolveRel32Call(callSite);
    if (!target || !IsExecutableAddress(target)) {
        RecordResolver(
            name, "semantic-local-call", EResolveConfidence::Failed,
            0, "decoded rel32 target is not executable");
        if (g_ModuleBase && callSite >= g_ModuleBase)
            AppendDiagnosticWindow(name, "decoded rel32 target is not executable", callSite - g_ModuleBase);
        return 0;
    }

    const uintptr_t rva = target - g_ModuleBase;
    char detail[128]{};
    snprintf(detail, sizeof(detail),
        "callsite=+0x%llX old-target=+0x%llX",
        static_cast<unsigned long long>(callSite - g_ModuleBase),
        static_cast<unsigned long long>(knownTargetRva));

    RecordResolver(
        name,
        "semantic-local-call",
        (IsKnown104Fingerprint(g_CurrentFingerprint) && rva == knownTargetRva)
        ? EResolveConfidence::Exact
        : EResolveConfidence::Strong,
        target,
        detail);
    CacheResolvedCallsite(name, callSite);
    CacheResolvedRva(name, target);
    return target;
}

static bool ResolveVirtualSlot(
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
        if (!MatchMaskedBytes(patternStart, tailPattern, tailMask))
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

    const uintptr_t cachedDispatchRva = CachedProfileValue(
        g_BuildProfileCache.VDispatches, name);
    const uintptr_t cachedSlot = CachedProfileValue(
        g_BuildProfileCache.VSlots, name);
    if (cachedDispatchRva && cachedSlot) {
        const size_t cachedDispatchOffset = slotImmediateOffset - 2;
        if (cachedDispatchRva >= cachedDispatchOffset) {
            const uintptr_t cachedPatternStart =
                moduleBase + cachedDispatchRva - cachedDispatchOffset;
            uintptr_t verifiedSlot = 0;
            if (readSlot(cachedPatternStart, &verifiedSlot) &&
                verifiedSlot == cachedSlot) {
                *outSlot = verifiedSlot;
                RecordResolver(
                    name,
                    "fingerprint-profile-vslot",
                    EResolveConfidence::ProfileStatic,
                    moduleBase + cachedDispatchRva,
                    "cached dispatch+slot revalidated against exact build fingerprint");
                CacheResolvedVSlot(name, verifiedSlot, moduleBase + cachedDispatchRva);
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

    if (IsKnown104Fingerprint(g_CurrentFingerprint) &&
        readSlot(knownPatternStart, &slot)) {
        *outSlot = slot;
        char detail[96]{};
        snprintf(detail, sizeof(detail), "slot=+0x%llX%s",
            static_cast<unsigned long long>(slot),
            slot == knownSlot ? "" : " changed-from-profile");
        RecordResolver(
            name, "wrapper-vslot-fast",
            slot == knownSlot ? EResolveConfidence::Exact : EResolveConfidence::Strong,
            knownPatternStart + dispatchOffset,
            detail);
        CacheResolvedVSlot(name, slot, knownPatternStart + dispatchOffset);
        return true;
    }

    const intptr_t shifted =
        static_cast<intptr_t>(knownDispatchRva) + g_RvaShiftHint;
    const uintptr_t expectedDispatchRva = shifted > 0
        ? static_cast<uintptr_t>(shifted)
        : knownDispatchRva;
    const uintptr_t expectedPatternRva =
        expectedDispatchRva > dispatchOffset ? expectedDispatchRva - dispatchOffset : 0;

    auto matches = FindPatternNearRva(
        moduleBase,
        expectedPatternRva,
        0x80000,
        tailPattern,
        tailMask,
        16);

    uintptr_t distance = 0;
    const uintptr_t chosen = ChoosePatternNearestHint(
        matches, moduleBase, expectedPatternRva, 0x100, &distance);

    uintptr_t selected = chosen;
    const char* selectionMethod = "wrapper-vslot-near-aob";
    if (!selected || !readSlot(selected, &slot)) {
        const auto globalMatches = FindPatternInExecutableSections(
            moduleBase, tailPattern, tailMask, 64);

        // Several reflected wrappers can legitimately share the same generated
        // tail shape on PalServer. Wrapper-address uniqueness is therefore not
        // itself a semantic requirement. What matters to ESE is the virtual
        // dispatch slot encoded by FF 90 [slot].
        //
        // Accept multiple independently-valid wrappers only when *all* of them
        // decode to the same slot. This is stronger than choosing by historical
        // RVA/slot proximity and remains update-durable: disagreement fails
        // closed and is reported with candidate evidence.
        uintptr_t consensusSlot = 0;
        uintptr_t consensusCandidate = 0;
        uintptr_t consensusDistance = UINTPTR_MAX;
        size_t validCount = 0;
        size_t conflictingSlotCount = 0;

        char candidateDetail[320]{};
        size_t candidateDetailUsed = 0;

        for (uintptr_t candidate : globalMatches) {
            uintptr_t candidateSlot = 0;
            if (!readSlot(candidate, &candidateSlot))
                continue;

            ++validCount;

            const uintptr_t candidateDispatch =
                candidate + dispatchOffset;
            const uintptr_t candidateRva =
                candidateDispatch >= moduleBase
                ? candidateDispatch - moduleBase
                : 0;
            const uintptr_t candidateDistance =
                candidate >= moduleBase + expectedPatternRva
                ? candidate - (moduleBase + expectedPatternRva)
                : (moduleBase + expectedPatternRva) - candidate;

            if (candidateDetailUsed < sizeof(candidateDetail) - 1) {
                const int written = snprintf(
                    candidateDetail + candidateDetailUsed,
                    sizeof(candidateDetail) - candidateDetailUsed,
                    "%s+0x%llX/slot+0x%llX",
                    candidateDetailUsed ? ", " : "",
                    static_cast<unsigned long long>(candidateRva),
                    static_cast<unsigned long long>(candidateSlot));
                if (written > 0) {
                    candidateDetailUsed +=
                        (std::min)(
                            static_cast<size_t>(written),
                            sizeof(candidateDetail) - candidateDetailUsed - 1);
                }
            }

            if (!consensusSlot) {
                consensusSlot = candidateSlot;
                consensusCandidate = candidate;
                consensusDistance = candidateDistance;
            }
            else if (candidateSlot != consensusSlot) {
                ++conflictingSlotCount;
            }
            else if (candidateDistance < consensusDistance) {
                consensusCandidate = candidate;
                consensusDistance = candidateDistance;
            }
        }

        if (validCount >= 1 &&
            consensusSlot != 0 &&
            conflictingSlotCount == 0) {
            selected = consensusCandidate;
            slot = consensusSlot;
            distance = consensusDistance;
            selectionMethod =
                validCount == 1
                ? "wrapper-vslot-global-unique-aob"
                : "wrapper-vslot-global-slot-consensus";

            if (validCount > 1) {
                char detail[420]{};
                snprintf(
                    detail,
                    sizeof(detail),
                    "global-valid=%llu all decode slot=+0x%llX; candidates=[%s]",
                    static_cast<unsigned long long>(validCount),
                    static_cast<unsigned long long>(slot),
                    candidateDetail);
                RecordResolver(
                    name,
                    "wrapper-vslot-global-slot-consensus-evidence",
                    EResolveConfidence::Strong,
                    selected + dispatchOffset,
                    detail);
            }
        }
        else {
            char detail[480]{};
            snprintf(detail, sizeof(detail),
                "near-matches=%llu global-valid=%llu slot-conflicts=%llu "
                "shiftHint=%+lld candidates=[%s]",
                static_cast<unsigned long long>(matches.size()),
                static_cast<unsigned long long>(validCount),
                static_cast<unsigned long long>(conflictingSlotCount),
                static_cast<long long>(g_RvaShiftHint),
                candidateDetail);
            RecordResolver(
                name,
                "wrapper-vslot-aob",
                EResolveConfidence::Failed,
                0,
                detail);
            AppendDiagnosticWindow(
                name,
                detail,
                expectedDispatchRva);
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
    RecordResolver(
        name, selectionMethod, EResolveConfidence::Strong,
        selected + dispatchOffset,
        detail);
    CacheResolvedVSlot(name, slot, selected + dispatchOffset);
    return true;
}

// ---------------------------------------------------------
// Durable elemental matchup helper resolver / installer
// ---------------------------------------------------------
static bool InstallElementMatchupTier(uintptr_t moduleBase)
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

    const uintptr_t target = ResolvePatternTarget(
        "CalcElementMatchupTier",
        moduleBase,
        Known104::Rva::MatchupHelper,
        kPattern,
        kMask,
        true);

    if (!target || !IsExecutableAddress(target))
        return false;

    // On the known build, additionally verify the semantic callsite relation.
    // On a migrated build the unique helper pattern is accepted as STRONG and
    // the resolver report makes that downgrade explicit.
    if (target == moduleBase + Known104::Rva::MatchupHelper) {
        const uintptr_t callerTarget = ResolveRel32Call(
            moduleBase + Known104::Rva::MatchupCallsite);
        if (callerTarget != target) {
            RecordResolver(
                "CalcElementMatchupTier.SemanticCallsite",
                "known-caller-rel32",
                EResolveConfidence::Failed,
                0,
                "CalcDamageCharacter callsite no longer targets helper");
            return false;
        }
        RecordResolver(
            "CalcElementMatchupTier.SemanticCallsite",
            "known-caller-rel32",
            IsKnown104Fingerprint(g_CurrentFingerprint)
            ? EResolveConfidence::Exact
            : EResolveConfidence::Strong,
            moduleBase + Known104::Rva::MatchupCallsite,
            IsKnown104Fingerprint(g_CurrentFingerprint)
            ? "CalcDamageCharacter -> tier helper verified on exact 1.0.4"
            : "historical callsite still semantically targets uniquely resolved helper; treated as strong only");
        CacheResolvedCallsite(
            "CalcElementMatchupTier.SemanticCallsite",
            moduleBase + Known104::Rva::MatchupCallsite);
    }

    const MH_STATUS create = MH_CreateHook(
        reinterpret_cast<LPVOID>(target),
        reinterpret_cast<LPVOID>(&Detour_CalcElementMatchupTier),
        reinterpret_cast<LPVOID*>(&Original_CalcElementMatchupTier));
    if (create != MH_OK) {
        ShipLog("[ElementalSystemExpanded] ERROR: matchup hook create failed (%d).\n",
            static_cast<int>(create));
        return false;
    }

    const MH_STATUS enable = MH_EnableHook(reinterpret_cast<LPVOID>(target));
    if (enable != MH_OK) {
        MH_RemoveHook(reinterpret_cast<LPVOID>(target));
        Original_CalcElementMatchupTier = nullptr;
        ShipLog("[ElementalSystemExpanded] ERROR: matchup hook enable failed (%d).\n",
            static_cast<int>(enable));
        return false;
    }

    g_Resolved_MatchupHelper = target;
    return true;
}

// ---------------------------------------------------------
// PalServer native buildup-value observation probe
// ---------------------------------------------------------
static bool IsTrackedElementalEffect(uint8_t effect);
static const char* ElementalEffectName(uint8_t effect);

// Dedicated-server validation only. This hook does NOT replace gameplay:
// it records the authoritative native buildup arguments and calls vanilla
// immediately with the original RCX/DL/XMM2 values.
using ServerBuildupValueProbe_t =
    void(__fastcall*)(void* DamageReactionComponent, uint8_t Effect, float Value);

static ServerBuildupValueProbe_t
Original_ServerBuildupValueProbe = nullptr;

static volatile LONG64
g_ServerBuildupProbeSequence = 0;

static void __fastcall Detour_ServerBuildupValueProbe(
    void* damageReaction,
    uint8_t effect,
    float value)
{
    if (IsTrackedElementalEffect(effect)) {
        const unsigned long long seq =
            static_cast<unsigned long long>(
                InterlockedIncrement64(
                    &g_ServerBuildupProbeSequence));

        ShipLog(
            "[ElementalSystemExpanded] SERVER UNIT PROBE #%llu: "
            "DamageReaction=%p Effect=%u (%s) Value=%.9g\n",
            seq,
            damageReaction,
            static_cast<unsigned>(effect),
            ElementalEffectName(effect),
            static_cast<double>(value));
    }

    if (Original_ServerBuildupValueProbe) {
        Original_ServerBuildupValueProbe(
            damageReaction,
            effect,
            value);
    }
}

static bool InstallServerBuildupValueProbe(uintptr_t target)
{
    if (!target || !IsExecutableAddress(target))
        return false;

    const MH_STATUS create = MH_CreateHook(
        reinterpret_cast<LPVOID>(target),
        reinterpret_cast<LPVOID>(&Detour_ServerBuildupValueProbe),
        reinterpret_cast<LPVOID*>(&Original_ServerBuildupValueProbe));

    if (create != MH_OK || !Original_ServerBuildupValueProbe) {
        Original_ServerBuildupValueProbe = nullptr;
        return false;
    }

    const MH_STATUS enable =
        MH_EnableHook(reinterpret_cast<LPVOID>(target));

    if (enable != MH_OK) {
        MH_RemoveHook(reinterpret_cast<LPVOID>(target));
        Original_ServerBuildupValueProbe = nullptr;
        return false;
    }

    return true;
}

static bool LookupRuntimeFunctionBounds(
    uintptr_t moduleBase,
    uintptr_t address,
    uint32_t* outBeginRva,
    uint32_t* outEndRva);

// ---------------------------------------------------------
// PalServer native AddStatus caller observation probe
// ---------------------------------------------------------
// Observation-only: log the actual dedicated-server caller of native AddStatus,
// then call vanilla unchanged. This is used because PalServer combat bypasses the
// reflected AddElementStatusAdditionalValue_OneType path.
using ServerAddStatusCallerProbe_t =
    void(__fastcall*)(void* StatusComponent, uint8_t StatusID);

static ServerAddStatusCallerProbe_t
Original_ServerAddStatusCallerProbe = nullptr;

static volatile LONG64
g_ServerAddStatusProbeSequence = 0;

static volatile LONG
g_ServerAddStatusOwnerDiagnosticsDone = 0;

static void EmitServerAddStatusOwnerDiagnostics(
    uintptr_t moduleBase,
    uint32_t ownerBeginRva,
    uint32_t ownerEndRva,
    uintptr_t returnRva,
    uint8_t statusID);

static bool IsInterestingServerStatus(uint8_t statusID)
{
    return statusID == ActiveIds::Status_Burn ||
        statusID == ActiveIds::Status_Wetness ||
        statusID == ActiveIds::Status_Freeze ||
        statusID == ActiveIds::Status_Electrical ||
        statusID == ActiveIds::Status_Muddy ||
        statusID == ActiveIds::Status_IvyCling ||
        statusID == ActiveIds::Status_Darkness ||
        statusID == ActiveIds::Status_VanillaWetFreeze ||
        statusID == ActiveIds::Status_VanillaWetFreezeResist;
}

static void __fastcall Detour_ServerAddStatusCallerProbe(
    void* statusComponent,
    uint8_t statusID)
{
    if (IsInterestingServerStatus(statusID)) {
        const unsigned long long seq =
            static_cast<unsigned long long>(
                InterlockedIncrement64(
                    &g_ServerAddStatusProbeSequence));

        const uintptr_t returnAddress =
            reinterpret_cast<uintptr_t>(_ReturnAddress());

        uintptr_t moduleBase = 0;
        if (g_CurrentFingerprint.SizeOfImage != 0) {
            HMODULE mainModule = GetModuleHandleW(nullptr);
            moduleBase = reinterpret_cast<uintptr_t>(mainModule);
        }

        uint32_t ownerBegin = 0;
        uint32_t ownerEnd = 0;
        bool haveOwner = false;

        if (moduleBase &&
            returnAddress >= moduleBase &&
            returnAddress - moduleBase <= 0xFFFFFFFFull) {
            haveOwner = LookupRuntimeFunctionBounds(
                moduleBase,
                returnAddress,
                &ownerBegin,
                &ownerEnd);
        }

        if (moduleBase &&
            returnAddress >= moduleBase &&
            returnAddress - moduleBase <= 0xFFFFFFFFull) {
            const uintptr_t returnRva =
                returnAddress - moduleBase;

            if (haveOwner) {
                EmitServerAddStatusOwnerDiagnostics(
                    moduleBase,
                    ownerBegin,
                    ownerEnd,
                    returnRva,
                    statusID);

                ShipLog(
                    "[ElementalSystemExpanded] SERVER ADDSTATUS PROBE #%llu: "
                    "StatusComponent=%p StatusID=%u ReturnRVA=+0x%llX "
                    "Owner=+0x%X..+0x%X\n",
                    seq,
                    statusComponent,
                    static_cast<unsigned>(statusID),
                    static_cast<unsigned long long>(returnRva),
                    ownerBegin,
                    ownerEnd);
            }
            else {
                ShipLog(
                    "[ElementalSystemExpanded] SERVER ADDSTATUS PROBE #%llu: "
                    "StatusComponent=%p StatusID=%u ReturnRVA=+0x%llX "
                    "Owner=<unresolved>\n",
                    seq,
                    statusComponent,
                    static_cast<unsigned>(statusID),
                    static_cast<unsigned long long>(returnRva));
            }
        }
        else {
            ShipLog(
                "[ElementalSystemExpanded] SERVER ADDSTATUS PROBE #%llu: "
                "StatusComponent=%p StatusID=%u ReturnAddress=%p "
                "Owner=<outside-main-module>\n",
                seq,
                statusComponent,
                static_cast<unsigned>(statusID),
                reinterpret_cast<void*>(returnAddress));
        }
    }

    if (Original_ServerAddStatusCallerProbe) {
        Original_ServerAddStatusCallerProbe(
            statusComponent,
            statusID);
    }
}

static bool InstallServerAddStatusCallerProbe(uintptr_t target)
{
    if (!target || !IsExecutableAddress(target))
        return false;

    const MH_STATUS create = MH_CreateHook(
        reinterpret_cast<LPVOID>(target),
        reinterpret_cast<LPVOID>(&Detour_ServerAddStatusCallerProbe),
        reinterpret_cast<LPVOID*>(&Original_ServerAddStatusCallerProbe));

    if (create != MH_OK || !Original_ServerAddStatusCallerProbe) {
        Original_ServerAddStatusCallerProbe = nullptr;
        return false;
    }

    const MH_STATUS enable =
        MH_EnableHook(reinterpret_cast<LPVOID>(target));

    if (enable != MH_OK) {
        MH_RemoveHook(reinterpret_cast<LPVOID>(target));
        Original_ServerAddStatusCallerProbe = nullptr;
        return false;
    }

    return true;
}

// ---------------------------------------------------------
// Raw FPalDamageInfo capture/context
// ---------------------------------------------------------
using DamageEffectCaller_t =
uintptr_t(__fastcall*)(void* DamageReaction, const void* DamageInfo);

static DamageEffectCaller_t Original_DamageEffectCaller = nullptr;
static uintptr_t g_DamageEffectCaller = 0;

static bool IsTrackedElementalEffect(uint8_t effect) {
    return effect == ActiveIds::Effect_Burn ||
        effect == ActiveIds::Effect_Wetness ||
        effect == ActiveIds::Effect_Freeze ||
        effect == ActiveIds::Effect_Electrical ||
        effect == ActiveIds::Effect_Muddy ||
        effect == ActiveIds::Effect_IvyCling ||
        effect == ActiveIds::Effect_Darkness;
}


enum class EDragonReaction : uint8_t {
    None = 0,
    SteamBurst = 1,
    LavaBurst = 2,
    Wildfire = 3,
    Flashover = 4,
    ThermalShock = 5,
    ArcBurst = 6
};


// Dragon reactions use BP_DragonExplosion as the authoritative configurable AOE
// carrier. Reaction-spread provenance is kept out-of-band in this process, while
// native effect values remain valid positive Palworld values. Dedicated/listen
// synthetic AOE packets are authorized narrowly in Lua by setting only
// FPalDamageInfo::IgnoreCanProcessDamage for the authenticated reaction carrier.
//
// Reflected bridge dispatch is fail-closed: gameplay is enabled only after the
// live BP_ESEBridge UObject/UFunction pair, bounded execution-frame template and
// detached replay path have passed their per-process runtime validation.
static constexpr float kDragonReactionSpreadDuration = 3.0f;
static constexpr int32_t kDragonReactionNativeEffectValue = 1;
static constexpr uint64_t kDragonReactionSpreadContextLifetimeMs = 2000ull;

// Lua issues this harmless call after it has loaded/spawned BP_ESEBridge and
// registered the DispatchDragonReaction hook. The unusual scalar tuple is used
// only to capture the exact live UObject + UFunction pair at the proven
// Blueprint execution boundary. It is
// never written into FPalDamageInfo and never enters Palworld gameplay.
static constexpr int32_t kDragonBridgeBootstrapReaction = 0x45534231; // "ESB1"
static constexpr int32_t kDragonBridgeBootstrapUnits = 0x01234567;
static constexpr double  kDragonBridgeBootstrapPower = 12345.25;
static constexpr int32_t kDragonBridgeBootstrapAttackType = 0x00001357;
static constexpr int32_t kDragonBridgeBootstrapSequence = 0x02468ACE;

// Non-gameplay local replay validation. The deliberately invalid reaction ID
// exercises the reflected hook without queuing or spawning a Dragon explosion.
static constexpr int32_t kDragonBridgeReplayValidationReaction = 0x45534232; // "ESB2"
// Detached replay validation runs only after the temporary capture hook is
// retired and on a later raw-damage boundary, proving that gameplay dispatch no
// longer depends on the original bootstrap call stack being alive.
static constexpr int32_t kDragonBridgeDetachedValidationReaction = 0x45534233; // "ESB3"
static constexpr size_t kDragonBridgeReplayFrameCloneBytes = 0x200;

struct FDragonBridgeParams {
    void* Attacker = nullptr;
    void* Defender = nullptr;
    int32_t Reaction = 0;
    int32_t Units = 0;
    double AttackPower = 0.0;
    int32_t AttackType = 0;
    int32_t Sequence = 0;
};

static_assert(offsetof(FDragonBridgeParams, Attacker) == 0x00, "Dragon bridge ABI: Attacker");
static_assert(offsetof(FDragonBridgeParams, Defender) == 0x08, "Dragon bridge ABI: Defender");
static_assert(offsetof(FDragonBridgeParams, Reaction) == 0x10, "Dragon bridge ABI: Reaction");
static_assert(offsetof(FDragonBridgeParams, Units) == 0x14, "Dragon bridge ABI: Units");
static_assert(offsetof(FDragonBridgeParams, AttackPower) == 0x18, "Dragon bridge ABI: AttackPower");
static_assert(offsetof(FDragonBridgeParams, AttackType) == 0x20, "Dragon bridge ABI: AttackType");
static_assert(offsetof(FDragonBridgeParams, Sequence) == 0x24, "Dragon bridge ABI: Sequence");
static_assert(sizeof(FDragonBridgeParams) == 0x28, "Dragon bridge ABI: ParmsSize");

// Keep the validated Blueprint execution-stub capture,
// but fixes the reflected parameter ABI and removes every dependency on the
// obsolete +0x268 ProcessEvent interpretation. We hook the shared Blueprint execution
// stub established by the earlier VM/disassembly investigation. At that stub:
//   RCX = executing UObject
//   [RDX + 0x10] = active UFunction
//   [RDX + 0x28] = locals/parameter buffer passed to script execution
// The bootstrap tuple therefore identifies the bridge call without embedding
// any metadata in FPalDamageInfo and without guessing a UObject dispatch slot.
using DragonBridgeBlueprintExecutionStub_t =
void(__fastcall*)(void* Object, void* FrameOrContext, void* Param3);

static DragonBridgeBlueprintExecutionStub_t
    Original_DragonBridgeBlueprintExecutionStub = nullptr;
static uintptr_t g_DragonBridgeBlueprintExecutionStubTarget = 0;
static void* g_DragonBridgeObject = nullptr;
static void* g_DragonBridgeFunction = nullptr;
static volatile LONG g_DragonBridgeReady = 0;
// 0=not attempted, 1=enabled, 2=disabled-after-capture, 3=permanent fail-closed.
static volatile LONG g_DragonBridgeCaptureHookState = 0;
// 0=not attempted, 1=call returned normally, 2=failed/SEH, 3=in progress.
// This state is diagnostic only; it does NOT authorize gameplay dispatch.
static volatile LONG g_DragonBridgeLocalReplayValidationState = 0;
// 0=not captured, 1=persistent template captured, 2=template rejected.
static volatile LONG g_DragonBridgeDetachedTemplateState = 0;
// 0=not attempted, 1=returned normally, 2=failed/SEH, 3=in progress.
static volatile LONG g_DragonBridgeDetachedReplayValidationState = 0;
alignas(16) static uint8_t
    g_DragonBridgeDetachedFrameTemplate[kDragonBridgeReplayFrameCloneBytes]{};
static volatile LONG g_DragonBridgeSequence = 0;
static SRWLOCK g_DragonBridgeLock = SRWLOCK_INIT;

// Definitions are below the raw-damage wrapper; narrow forward declarations
// are retained for the rest of the authoritative reaction path.
static void* ResolveOwningPalCharacter(void* damageReaction);
static void* ResolveStatusComponent(void* damageReaction);
static bool SnapshotDragonBridgeCapture(void** outObject, void** outFunction);

struct FDragonSpreadContext {
    bool Active = false;
    void* Attacker = nullptr;
    uint8_t AttackElement = 0;
    uint8_t Effect1 = 0;
    int32_t EffectValue1 = 0;
    uint8_t Effect2 = 0;
    int32_t EffectValue2 = 0;
    float AttackPower = 0.0f;
    uint8_t AttackType = 0;
    uint8_t SourceUnits = 0;
    int32_t Sequence = 0;
    uint64_t ExpiresTick = 0;
};

static FDragonSpreadContext g_DragonSpreadContexts[32]{};
static uint32_t g_DragonSpreadContextCursor = 0;
static SRWLOCK g_DragonSpreadContextLock = SRWLOCK_INIT;

static bool IsLikelyBridgeUObject(void* object)
{
    if (!object)
        return false;

    const uintptr_t p = reinterpret_cast<uintptr_t>(object);
    if (!IsReadableAddress(p, 0x28))
        return false;

    uintptr_t vtable = 0;
    uintptr_t firstVirtual = 0;
    __try {
        vtable = *reinterpret_cast<const uintptr_t*>(p);
        if (!vtable ||
            !IsReadableAddress(vtable, sizeof(uintptr_t))) {
            return false;
        }
        firstVirtual = *reinterpret_cast<const uintptr_t*>(vtable);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }

    return firstVirtual && IsExecutableAddress(firstVirtual);
}

static bool IsDragonBridgeBootstrapParams(const void* parameters)
{
    if (!parameters ||
        !IsReadableAddress(
            reinterpret_cast<uintptr_t>(parameters),
            sizeof(FDragonBridgeParams))) {
        return false;
    }

    __try {
        const auto* p =
            reinterpret_cast<const FDragonBridgeParams*>(parameters);
        return p->Attacker != nullptr &&
            p->Attacker == p->Defender &&
            IsLikelyBridgeUObject(p->Attacker) &&
            p->Reaction == kDragonBridgeBootstrapReaction &&
            p->Units == kDragonBridgeBootstrapUnits &&
            p->AttackPower == kDragonBridgeBootstrapPower &&
            p->AttackType == kDragonBridgeBootstrapAttackType &&
            p->Sequence == kDragonBridgeBootstrapSequence;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool ReadDragonBridgeBlueprintFrame(
    void* frameOrContext,
    void** outFunction,
    void** outLocals)
{
    if (outFunction) *outFunction = nullptr;
    if (outLocals) *outLocals = nullptr;
    if (!frameOrContext || !outFunction || !outLocals ||
        !IsReadableAddress(
            reinterpret_cast<uintptr_t>(frameOrContext), 0x30)) {
        return false;
    }

    __try {
        const uintptr_t frame =
            reinterpret_cast<uintptr_t>(frameOrContext);
        void* function = *reinterpret_cast<void* const*>(frame + 0x10);
        void* locals = *reinterpret_cast<void* const*>(frame + 0x28);
        if (!function || !locals ||
            !IsLikelyBridgeUObject(function) ||
            !IsReadableAddress(
                reinterpret_cast<uintptr_t>(locals),
                sizeof(FDragonBridgeParams))) {
            return false;
        }

        *outFunction = function;
        *outLocals = locals;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        *outFunction = nullptr;
        *outLocals = nullptr;
        return false;
    }
}

static void TryDragonBridgeLocalReplayValidation(
    void* object,
    void* frameOrContext,
    void* param3,
    void* locals)
{
    if (!object || !frameOrContext || !locals ||
        !Original_DragonBridgeBlueprintExecutionStub) {
        return;
    }

    if (InterlockedCompareExchange(
            &g_DragonBridgeLocalReplayValidationState, 3, 0) != 0) {
        return;
    }

    if (!IsReadableAddress(
            reinterpret_cast<uintptr_t>(frameOrContext),
            kDragonBridgeReplayFrameCloneBytes) ||
        !IsReadableAddress(
            reinterpret_cast<uintptr_t>(locals),
            sizeof(FDragonBridgeParams))) {
        ShipLog(
            "[ElementalSystemExpanded] DRAGON BRIDGE LOCAL REPLAY VALIDATION FAIL-CLOSED: "
            "live bootstrap frame/locals are not readable for the bounded clone.\n");
        InterlockedExchange(&g_DragonBridgeLocalReplayValidationState, 2);
        return;
    }

    alignas(16) uint8_t frameClone[kDragonBridgeReplayFrameCloneBytes]{};
    FDragonBridgeParams validationParams{};

    __try {
        std::memcpy(
            frameClone,
            frameOrContext,
            kDragonBridgeReplayFrameCloneBytes);
        std::memcpy(
            &validationParams,
            locals,
            sizeof(validationParams));

        // Preserve the exact live UObject/UFunction/frame template. Replace only
        // the captured Locals pointer and one scalar with a deliberately
        // invalid reaction id. This is a non-gameplay reflection-path validation.
        validationParams.Reaction = kDragonBridgeReplayValidationReaction;
        *reinterpret_cast<void**>(frameClone + 0x28) = &validationParams;

        // Preserve one bounded copy of the pre-execution bootstrap frame for a
        // later detached replay. We deliberately do NOT retain Param3: for this
        // void Blueprint function, detached replay is allowed only when the live
        // bootstrap call proves Param3 is null. A non-null value may be transient
        // caller-owned storage and therefore fails closed instead of being saved.
        if (InterlockedCompareExchange(
                &g_DragonBridgeDetachedTemplateState, 0, 0) == 0) {
            if (param3 == nullptr) {
                AcquireSRWLockExclusive(&g_DragonBridgeLock);
                std::memcpy(
                    g_DragonBridgeDetachedFrameTemplate,
                    frameOrContext,
                    kDragonBridgeReplayFrameCloneBytes);
                ReleaseSRWLockExclusive(&g_DragonBridgeLock);
                InterlockedExchange(&g_DragonBridgeDetachedTemplateState, 1);
                ModLog(
                    "[ElementalSystemExpanded] DRAGON BRIDGE DETACHED TEMPLATE CAPTURED: "
                    "FrameBytes=0x%llX Param3=null. Detached replay may proceed only "
                    "after the capture hook is retired.\n",
                    static_cast<unsigned long long>(
                        kDragonBridgeReplayFrameCloneBytes));
            }
            else {
                InterlockedExchange(&g_DragonBridgeDetachedTemplateState, 2);
                ShipLog(
                    "[ElementalSystemExpanded] DRAGON BRIDGE DETACHED TEMPLATE FAIL-CLOSED: "
                    "bootstrap Param3=%p is non-null; refusing to retain a possibly "
                    "transient caller-owned pointer.\n",
                    param3);
            }
        }

        ModLog(
            "[ElementalSystemExpanded] DRAGON BRIDGE LOCAL REPLAY VALIDATION CALL: "
            "BridgeObject=%p FrameClone=%p UFunction=%p Locals=%p "
            "Reaction=0x%08X. Expect Lua to reject this invalid validation dispatch.\n",
            object,
            frameClone,
            *reinterpret_cast<void**>(frameClone + 0x10),
            &validationParams,
            static_cast<unsigned>(kDragonBridgeReplayValidationReaction));

        Original_DragonBridgeBlueprintExecutionStub(
            object,
            frameClone,
            param3);

        ModLog(
            "[ElementalSystemExpanded] DRAGON BRIDGE LOCAL REPLAY VALIDATION RETURNED: "
            "chained Blueprint executor returned normally; local replay validation complete.\n");
        InterlockedExchange(&g_DragonBridgeLocalReplayValidationState, 1);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        ShipLog(
            "[ElementalSystemExpanded] DRAGON BRIDGE LOCAL REPLAY VALIDATION FAIL-CLOSED: "
            "exception while replaying the captured live Blueprint frame clone.\n");
        InterlockedExchange(&g_DragonBridgeLocalReplayValidationState, 2);
    }
}

static void __fastcall Detour_DragonBridgeBlueprintExecutionStub(
    void* object,
    void* frameOrContext,
    void* param3)
{
    void* function = nullptr;
    void* locals = nullptr;

    if (object &&
        ReadDragonBridgeBlueprintFrame(
            frameOrContext, &function, &locals) &&
        IsDragonBridgeBootstrapParams(locals) &&
        IsLikelyBridgeUObject(object)) {

        bool first = false;
        AcquireSRWLockExclusive(&g_DragonBridgeLock);
        if (g_DragonBridgeObject != object ||
            g_DragonBridgeFunction != function) {
            g_DragonBridgeObject = object;
            g_DragonBridgeFunction = function;
            first = true;
        }
        ReleaseSRWLockExclusive(&g_DragonBridgeLock);

        InterlockedExchange(&g_DragonBridgeReady, 1);

        if (first) {
            ModLog(
                "[ElementalSystemExpanded] DRAGON BRIDGE CAPTURED: "
                "BridgeObject=%p UFunction=%p BlueprintStub=%p "
                "ParmsSize=0x28 AttackPower=double.\n",
                object,
                function,
                reinterpret_cast<void*>(
                    g_DragonBridgeBlueprintExecutionStubTarget));
        }

        TryDragonBridgeLocalReplayValidation(
            object,
            frameOrContext,
            param3,
            locals);
    }

    if (Original_DragonBridgeBlueprintExecutionStub) {
        Original_DragonBridgeBlueprintExecutionStub(
            object,
            frameOrContext,
            param3);
    }
}

static uintptr_t ResolveDragonBridgeBlueprintExecutionStub()
{
    if (!g_ModuleBase)
        return 0;

    // Resolved Blueprint execution-stub signature:
    //   mov [rsp+08],rbx
    //   mov [rsp+10],rbp
    //   mov [rsp+18],rsi
    //   mov [rsp+20],rdi
    //   push r14
    //   sub rsp,30
    //   mov rsi,[rdx+10]
    //   mov rbp,r8
    static const uint8_t kBlueprintStubPattern[] = {
        0x48,0x89,0x5C,0x24,0x08,
        0x48,0x89,0x6C,0x24,0x10,
        0x48,0x89,0x74,0x24,0x18,
        0x48,0x89,0x7C,0x24,0x20,
        0x41,0x56,
        0x48,0x83,0xEC,0x30,
        0x48,0x8B,0x72,0x10,
        0x49,0x8B,0xE8
    };
    static const char kBlueprintStubMask[] =
        "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx";
    static_assert(sizeof(kBlueprintStubPattern) ==
        sizeof(kBlueprintStubMask) - 1,
        "Blueprint stub signature/mask length drift");

    const auto matches = FindPatternInExecutableSections(
        g_ModuleBase,
        kBlueprintStubPattern,
        kBlueprintStubMask,
        2);

    if (matches.size() != 1) {
        ShipLog(
            "[ElementalSystemExpanded] DRAGON BRIDGE CAPTURE FAIL-CLOSED: "
            "Blueprint execution stub signature match count=%llu (expected 1).\n",
            static_cast<unsigned long long>(matches.size()));
        return 0;
    }

    return matches[0];
}

static bool EnsureDragonBridgeCaptureHook(void* damageReaction)
{
    (void)damageReaction;

    if (InterlockedCompareExchange(&g_DragonBridgeReady, 0, 0) != 0)
        return true;

    AcquireSRWLockExclusive(&g_DragonBridgeLock);

    bool ok = false;
    LONG state = InterlockedCompareExchange(
        &g_DragonBridgeCaptureHookState, 0, 0);

    if (state == 3) {
        ReleaseSRWLockExclusive(&g_DragonBridgeLock);
        return false;
    }

    uintptr_t target = g_DragonBridgeBlueprintExecutionStubTarget;
    if (!target && state == 0) {
        target = ResolveDragonBridgeBlueprintExecutionStub();
        if (!target) {
            InterlockedExchange(&g_DragonBridgeCaptureHookState, 3);
            ReleaseSRWLockExclusive(&g_DragonBridgeLock);
            return false;
        }
        g_DragonBridgeBlueprintExecutionStubTarget = target;
    }

    if (state == 0) {
        const MH_STATUS create = MH_CreateHook(
            reinterpret_cast<LPVOID>(target),
            reinterpret_cast<LPVOID>(
                &Detour_DragonBridgeBlueprintExecutionStub),
            reinterpret_cast<LPVOID*>(
                &Original_DragonBridgeBlueprintExecutionStub));

        if (create != MH_OK ||
            !Original_DragonBridgeBlueprintExecutionStub) {
            ShipLog(
                "[ElementalSystemExpanded] DRAGON BRIDGE CAPTURE FAIL-CLOSED: "
                "MH_CreateHook(BlueprintExecutionStub=%p)=%d.\n",
                reinterpret_cast<void*>(target),
                static_cast<int>(create));
            Original_DragonBridgeBlueprintExecutionStub = nullptr;
            InterlockedExchange(&g_DragonBridgeCaptureHookState, 3);
            ReleaseSRWLockExclusive(&g_DragonBridgeLock);
            return false;
        }

        const MH_STATUS enable =
            MH_EnableHook(reinterpret_cast<LPVOID>(target));
        if (enable != MH_OK) {
            ShipLog(
                "[ElementalSystemExpanded] DRAGON BRIDGE CAPTURE FAIL-CLOSED: "
                "MH_EnableHook(BlueprintExecutionStub=%p)=%d.\n",
                reinterpret_cast<void*>(target),
                static_cast<int>(enable));
            MH_RemoveHook(reinterpret_cast<LPVOID>(target));
            Original_DragonBridgeBlueprintExecutionStub = nullptr;
            InterlockedExchange(&g_DragonBridgeCaptureHookState, 3);
            ReleaseSRWLockExclusive(&g_DragonBridgeLock);
            return false;
        }

        InterlockedExchange(&g_DragonBridgeCaptureHookState, 1);
        state = 1;
        ModLog(
            "[ElementalSystemExpanded] DRAGON BRIDGE CAPTURE ARM: "
            "BlueprintExecutionStub=%p FunctionFrame=+0x10 Locals=+0x28 ParmsSize=0x28.\n",
            reinterpret_cast<void*>(target));
    }
    else if (state == 2 && target) {
        const MH_STATUS enable =
            MH_EnableHook(reinterpret_cast<LPVOID>(target));
        if (enable == MH_OK) {
            InterlockedExchange(&g_DragonBridgeCaptureHookState, 1);
            state = 1;
        }
        else {
            ShipLog(
                "[ElementalSystemExpanded] DRAGON BRIDGE CAPTURE FAIL-CLOSED: "
                "re-enable BlueprintExecutionStub=%p failed (%d).\n",
                reinterpret_cast<void*>(target),
                static_cast<int>(enable));
            InterlockedExchange(&g_DragonBridgeCaptureHookState, 3);
            state = 3;
        }
    }

    ok = (state == 1);
    ReleaseSRWLockExclusive(&g_DragonBridgeLock);
    return ok;
}

static void TryDragonBridgeDetachedReplayValidation()
{
    if (InterlockedCompareExchange(&g_DragonBridgeReady, 0, 0) == 0 ||
        InterlockedCompareExchange(&g_DragonBridgeDetachedTemplateState, 0, 0) != 1 ||
        InterlockedCompareExchange(&g_DragonBridgeCaptureHookState, 0, 0) != 2 ||
        !Original_DragonBridgeBlueprintExecutionStub) {
        return;
    }

    if (InterlockedCompareExchange(
            &g_DragonBridgeDetachedReplayValidationState, 3, 0) != 0) {
        return;
    }

    void* object = nullptr;
    void* function = nullptr;
    if (!SnapshotDragonBridgeCapture(&object, &function)) {
        ShipLog(
            "[ElementalSystemExpanded] DRAGON BRIDGE DETACHED REPLAY FAIL-CLOSED: "
            "captured UObject/UFunction is unavailable.\n");
        InterlockedExchange(&g_DragonBridgeDetachedReplayValidationState, 2);
        return;
    }

    alignas(16) uint8_t frameClone[kDragonBridgeReplayFrameCloneBytes]{};
    FDragonBridgeParams validationParams{};

    __try {
        AcquireSRWLockShared(&g_DragonBridgeLock);
        std::memcpy(
            frameClone,
            g_DragonBridgeDetachedFrameTemplate,
            kDragonBridgeReplayFrameCloneBytes);
        ReleaseSRWLockShared(&g_DragonBridgeLock);

        // Rebuild only the validated fields at the shared executor boundary.
        // The stored frame is a template; locals are fresh stack storage and the
        // captured UFunction is reasserted explicitly. Param3 is intentionally null
        // because the bootstrap call proved it null for this void function.
        validationParams.Attacker = object;
        validationParams.Defender = object;
        validationParams.Reaction = kDragonBridgeDetachedValidationReaction;
        validationParams.Units = kDragonBridgeBootstrapUnits;
        validationParams.AttackPower = kDragonBridgeBootstrapPower;
        validationParams.AttackType = kDragonBridgeBootstrapAttackType;
        validationParams.Sequence = kDragonBridgeBootstrapSequence;
        *reinterpret_cast<void**>(frameClone + 0x10) = function;
        *reinterpret_cast<void**>(frameClone + 0x28) = &validationParams;

        ModLog(
            "[ElementalSystemExpanded] DRAGON BRIDGE DETACHED REPLAY VALIDATION CALL: "
            "BridgeObject=%p FrameClone=%p UFunction=%p Locals=%p Reaction=0x%08X "
            "CaptureHookState=retired Param3=null. Expect Lua to reject the validation dispatch.\n",
            object,
            frameClone,
            function,
            &validationParams,
            static_cast<unsigned>(kDragonBridgeDetachedValidationReaction));

        Original_DragonBridgeBlueprintExecutionStub(
            object,
            frameClone,
            nullptr);

        ModLog(
            "[ElementalSystemExpanded] DRAGON BRIDGE DETACHED REPLAY VALIDATION RETURNED: "
            "chained Blueprint executor returned normally after the bootstrap frame "
            "lifetime ended; detached replay validation complete.\n");
        InterlockedExchange(&g_DragonBridgeDetachedReplayValidationState, 1);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        ShipLog(
            "[ElementalSystemExpanded] DRAGON BRIDGE DETACHED REPLAY FAIL-CLOSED: "
            "exception while replaying the retained frame template after capture retirement.\n");
        InterlockedExchange(&g_DragonBridgeDetachedReplayValidationState, 2);
    }
}

static void MaybeRetireDragonBridgeCaptureHook()
{
    if (InterlockedCompareExchange(&g_DragonBridgeReady, 0, 0) == 0)
        return;

    const LONG replayState = InterlockedCompareExchange(
        &g_DragonBridgeLocalReplayValidationState, 0, 0);
    if (replayState == 0 || replayState == 3)
        return;

    if (InterlockedCompareExchange(
            &g_DragonBridgeCaptureHookState, 0, 0) != 1 ||
        !g_DragonBridgeBlueprintExecutionStubTarget) {
        return;
    }

    const MH_STATUS disabled = MH_DisableHook(
        reinterpret_cast<LPVOID>(
            g_DragonBridgeBlueprintExecutionStubTarget));
    if (disabled == MH_OK) {
        InterlockedExchange(&g_DragonBridgeCaptureHookState, 2);
        ModLog(
            "[ElementalSystemExpanded] DRAGON BRIDGE CAPTURE RETIRED: "
            "captured UObject/UFunction and bounded frame template retained; live replay validation reached a terminal state; Blueprint execution-stub capture hook disabled.\n");
    }
}

static void InvalidateDragonBridgeEndpoint(const char* reason)
{
    AcquireSRWLockExclusive(&g_DragonBridgeLock);
    g_DragonBridgeObject = nullptr;
    g_DragonBridgeFunction = nullptr;
    ReleaseSRWLockExclusive(&g_DragonBridgeLock);
    InterlockedExchange(&g_DragonBridgeReady, 0);

    ShipLog(
        "[ElementalSystemExpanded] DRAGON BRIDGE INVALIDATED: %s\n",
        reason ? reason : "unknown");
}

static bool SnapshotDragonBridgeCapture(
    void** outObject,
    void** outFunction)
{
    if (outObject) *outObject = nullptr;
    if (outFunction) *outFunction = nullptr;
    if (!outObject || !outFunction ||
        InterlockedCompareExchange(&g_DragonBridgeReady, 0, 0) == 0) {
        return false;
    }

    void* object = nullptr;
    void* function = nullptr;

    AcquireSRWLockShared(&g_DragonBridgeLock);
    object = g_DragonBridgeObject;
    function = g_DragonBridgeFunction;
    ReleaseSRWLockShared(&g_DragonBridgeLock);

    if (!object || !function ||
        !IsLikelyBridgeUObject(object) ||
        !IsLikelyBridgeUObject(function)) {
        InvalidateDragonBridgeEndpoint(
            "captured UObject/UFunction is no longer valid");
        return false;
    }

    *outObject = object;
    *outFunction = function;
    return true;
}

static int32_t NextDragonBridgeSequence()
{
    LONG value = InterlockedIncrement(&g_DragonBridgeSequence);
    if (value <= 0) {
        InterlockedExchange(&g_DragonBridgeSequence, 1);
        value = 1;
    }
    return static_cast<int32_t>(value);
}

static bool DragonReactionSpreadSignature(
    EDragonReaction reaction,
    uint8_t* outElement,
    uint8_t* outEffect1,
    uint8_t* outEffect2,
    int32_t* outEffectValue2)
{
    if (!outElement || !outEffect1 || !outEffect2 || !outEffectValue2)
        return false;

    *outEffect2 = 0;
    *outEffectValue2 = 0;

    switch (reaction) {
    case EDragonReaction::SteamBurst:
        *outElement = ActiveIds::Element_Water;
        *outEffect1 = ActiveIds::Effect_Wetness;
        return true;
    case EDragonReaction::LavaBurst:
    case EDragonReaction::Wildfire:
        *outElement = ActiveIds::Element_Fire;
        *outEffect1 = ActiveIds::Effect_Burn;
        return true;
    case EDragonReaction::Flashover:
        *outElement = ActiveIds::Element_Fire;
        *outEffect1 = ActiveIds::Effect_Burn;
        *outEffect2 = 1; // native Stun
        *outEffectValue2 = 100;
        return true;
    case EDragonReaction::ThermalShock:
        *outElement = ActiveIds::Element_Ice;
        *outEffect1 = ActiveIds::Effect_Freeze;
        return true;
    case EDragonReaction::ArcBurst:
        *outElement = ActiveIds::Element_Electricity;
        *outEffect1 = ActiveIds::Effect_Electrical;
        return true;
    default:
        return false;
    }
}

static bool NearlyEqualDragonPower(float a, float b)
{
    if (!std::isfinite(a) || !std::isfinite(b))
        return false;
    const float delta = std::fabs(a - b);
    const float scale = (std::max)(1.0f, (std::max)(std::fabs(a), std::fabs(b)));
    return delta <= 0.01f || delta <= scale * 0.001f;
}

static void RegisterDragonSpreadContext(
    void* attacker,
    EDragonReaction reaction,
    uint8_t sourceUnits,
    float attackPower,
    uint8_t attackType,
    int32_t sequence)
{
    uint8_t element = 0;
    uint8_t effect1 = 0;
    uint8_t effect2 = 0;
    int32_t effectValue2 = 0;
    if (!attacker || !sequence ||
        !DragonReactionSpreadSignature(
            reaction,
            &element,
            &effect1,
            &effect2,
            &effectValue2)) {
        return;
    }

    const uint64_t now = GetTickCount64();
    FDragonSpreadContext ctx{};
    ctx.Active = true;
    ctx.Attacker = attacker;
    ctx.AttackElement = element;
    ctx.Effect1 = effect1;
    ctx.EffectValue1 = kDragonReactionNativeEffectValue;
    ctx.Effect2 = effect2;
    ctx.EffectValue2 = effectValue2;
    ctx.AttackPower = attackPower;
    ctx.AttackType = attackType;
    ctx.SourceUnits = sourceUnits;
    ctx.Sequence = sequence;
    ctx.ExpiresTick = now + kDragonReactionSpreadContextLifetimeMs;

    AcquireSRWLockExclusive(&g_DragonSpreadContextLock);

    size_t slot = sizeof(g_DragonSpreadContexts) /
        sizeof(g_DragonSpreadContexts[0]);
    for (size_t i = 0;
         i < sizeof(g_DragonSpreadContexts) /
             sizeof(g_DragonSpreadContexts[0]);
         ++i) {
        if (!g_DragonSpreadContexts[i].Active ||
            g_DragonSpreadContexts[i].ExpiresTick <= now) {
            slot = i;
            break;
        }
    }

    if (slot >= sizeof(g_DragonSpreadContexts) /
            sizeof(g_DragonSpreadContexts[0])) {
        slot = g_DragonSpreadContextCursor++ %
            (sizeof(g_DragonSpreadContexts) /
             sizeof(g_DragonSpreadContexts[0]));
    }

    g_DragonSpreadContexts[slot] = ctx;
    ReleaseSRWLockExclusive(&g_DragonSpreadContextLock);

    ModLog(
        "[ElementalSystemExpanded] DRAGON SPREAD CONTEXT ARM: "
        "Seq=%d Attacker=%p Element=%u Effect1=%u Value1=%d "
        "Effect2=%u Value2=%d Power=%.3f AttackType=%u Units=%u TTLms=%llu.\n",
        sequence,
        attacker,
        static_cast<unsigned>(element),
        static_cast<unsigned>(effect1),
        kDragonReactionNativeEffectValue,
        static_cast<unsigned>(effect2),
        effectValue2,
        attackPower,
        static_cast<unsigned>(attackType),
        static_cast<unsigned>(sourceUnits),
        static_cast<unsigned long long>(kDragonReactionSpreadContextLifetimeMs));
}

static void RemoveDragonSpreadContext(int32_t sequence)
{
    if (!sequence)
        return;

    AcquireSRWLockExclusive(&g_DragonSpreadContextLock);
    for (auto& ctx : g_DragonSpreadContexts) {
        if (ctx.Active && ctx.Sequence == sequence)
            ctx = {};
    }
    ReleaseSRWLockExclusive(&g_DragonSpreadContextLock);
}

static bool ReadDragonDamageBridgeFields(
    const void* damageInfo,
    void** outAttacker,
    float* outAttackPower,
    uint8_t* outAttackType)
{
    if (outAttacker) *outAttacker = nullptr;
    if (outAttackPower) *outAttackPower = 0.0f;
    if (outAttackType) *outAttackType = 0;
    if (!damageInfo || !outAttacker || !outAttackPower || !outAttackType)
        return false;

    // These three fields are generated/fingerprint-bound like the rest of the
    // FPalDamageInfo layout. Exact 1.0.4 constants remain only as the known-build
    // oracle fallback inside ActiveLayout. BasePower is int32; never reinterpret
    // those four bytes as float (e.g. int32 750 becomes a tiny denormal).
    const uintptr_t p = reinterpret_cast<uintptr_t>(damageInfo);
    if (!IsReadableAddress(
            p + ActiveLayout::DamageInfo_BasePower, sizeof(int32_t)) ||
        !IsReadableAddress(
            p + ActiveLayout::DamageInfo_Attacker, sizeof(void*)) ||
        !IsReadableAddress(
            p + ActiveLayout::DamageInfo_AttackType, sizeof(uint8_t))) {
        return false;
    }

    int32_t basePower = 0;
    __try {
        basePower = *reinterpret_cast<const int32_t*>(
            p + ActiveLayout::DamageInfo_BasePower);
        *outAttacker = *reinterpret_cast<void* const*>(
            p + ActiveLayout::DamageInfo_Attacker);
        *outAttackType = *reinterpret_cast<const uint8_t*>(
            p + ActiveLayout::DamageInfo_AttackType);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }

    if (!*outAttacker ||
        !IsLikelyBridgeUObject(*outAttacker) ||
        basePower <= 0 ||
        basePower > 10000000) {
        return false;
    }

    // Internal spread/context code historically carries power as float, while
    // BP_ESEBridge transports it as a reflected DoubleProperty. Every validated
    // integer in this range is exactly representable as float, so retain that
    // validated transport ABI and convert only after the correct int32 read.
    *outAttackPower = static_cast<float>(basePower);
    return true;
}

static bool MatchDragonSpreadContext(
    const void* damageInfo,
    uint8_t attackElement,
    uint8_t effect1,
    int32_t value1,
    uint8_t effect2,
    int32_t value2,
    int32_t* outSequence)
{
    if (outSequence)
        *outSequence = 0;
    if (!damageInfo || !outSequence ||
        value1 != kDragonReactionNativeEffectValue) {
        return false;
    }

    void* attacker = nullptr;
    float attackPower = 0.0f;
    uint8_t attackType = 0;
    if (!ReadDragonDamageBridgeFields(
            damageInfo,
            &attacker,
            &attackPower,
            &attackType)) {
        return false;
    }

    const uint64_t now = GetTickCount64();
    int32_t matchedSequence = 0;

    AcquireSRWLockExclusive(&g_DragonSpreadContextLock);
    for (auto& ctx : g_DragonSpreadContexts) {
        if (!ctx.Active)
            continue;
        if (ctx.ExpiresTick <= now) {
            ctx = {};
            continue;
        }

        if (ctx.Attacker != attacker ||
            ctx.AttackElement != attackElement ||
            ctx.Effect1 != effect1 ||
            ctx.EffectValue1 != value1 ||
            ctx.Effect2 != effect2 ||
            ctx.EffectValue2 != value2 ||
            ctx.AttackType != attackType ||
            !NearlyEqualDragonPower(ctx.AttackPower, attackPower)) {
            continue;
        }

        if (ctx.Sequence > matchedSequence)
            matchedSequence = ctx.Sequence;
    }
    ReleaseSRWLockExclusive(&g_DragonSpreadContextLock);

    if (!matchedSequence)
        return false;

    *outSequence = matchedSequence;
    return true;
}

static bool IsDragonBridgeCaptureReady()
{
    void* object = nullptr;
    void* function = nullptr;
    return SnapshotDragonBridgeCapture(
        &object,
        &function);
}

static bool CanDispatchDragonBridge()
{
    // Keep one process-local detached replay validation as an additional
    // fail-closed guard: gameplay is authorized only after capture is ready, the bounded
    // frame template is retained, the temporary capture hook is retired, and
    // the detached executor call has returned normally in this process.
    if (InterlockedCompareExchange(&g_DragonBridgeReady, 0, 0) == 0 ||
        InterlockedCompareExchange(&g_DragonBridgeDetachedTemplateState, 0, 0) != 1 ||
        InterlockedCompareExchange(&g_DragonBridgeCaptureHookState, 0, 0) != 2 ||
        InterlockedCompareExchange(&g_DragonBridgeDetachedReplayValidationState, 0, 0) != 1 ||
        !Original_DragonBridgeBlueprintExecutionStub) {
        return false;
    }

    void* object = nullptr;
    void* function = nullptr;
    return SnapshotDragonBridgeCapture(&object, &function);
}

static bool DispatchDragonBridge(
    void* attacker,
    void* defender,
    EDragonReaction reaction,
    uint8_t units,
    float attackPower,
    uint8_t attackType,
    int32_t sequence)
{
    if (!attacker || !defender ||
        !IsLikelyBridgeUObject(attacker) ||
        !IsLikelyBridgeUObject(defender) ||
        static_cast<uint8_t>(reaction) < static_cast<uint8_t>(EDragonReaction::SteamBurst) ||
        static_cast<uint8_t>(reaction) > static_cast<uint8_t>(EDragonReaction::ArcBurst) ||
        units < 1 || units > 2 ||
        sequence <= 0 ||
        !std::isfinite(attackPower) ||
        !CanDispatchDragonBridge()) {
        ShipLog(
            "[ElementalSystemExpanded] DRAGON BRIDGE DISPATCH FAIL-CLOSED: "
            "invalid dispatch state/arguments Reaction=%u Units=%u Power=%.3f "
            "AttackType=%u Seq=%d.\n",
            static_cast<unsigned>(reaction),
            static_cast<unsigned>(units),
            attackPower,
            static_cast<unsigned>(attackType),
            sequence);
        return false;
    }

    void* object = nullptr;
    void* function = nullptr;
    if (!SnapshotDragonBridgeCapture(&object, &function)) {
        ShipLog(
            "[ElementalSystemExpanded] DRAGON BRIDGE DISPATCH FAIL-CLOSED: "
            "captured UObject/UFunction is unavailable. Seq=%d.\n",
            sequence);
        return false;
    }

    alignas(16) uint8_t frameClone[kDragonBridgeReplayFrameCloneBytes]{};
    FDragonBridgeParams params{};

    // Template storage is immutable once DetachedTemplateState becomes 1.
    // Copy it under the bridge lock before entering SEH so no exception path can
    // strand an SRW lock held by this gameplay dispatch.
    AcquireSRWLockShared(&g_DragonBridgeLock);
    std::memcpy(
        frameClone,
        g_DragonBridgeDetachedFrameTemplate,
        kDragonBridgeReplayFrameCloneBytes);
    ReleaseSRWLockShared(&g_DragonBridgeLock);

    __try {
        params.Attacker = attacker;
        params.Defender = defender;
        params.Reaction = static_cast<int32_t>(reaction);
        params.Units = static_cast<int32_t>(units);
        params.AttackPower = static_cast<double>(attackPower);
        params.AttackType = static_cast<int32_t>(attackType);
        params.Sequence = sequence;

        // These are the only frame fields modified in the detached replay validation.
        // Reassert them for every gameplay call and pass Param3=null exactly as
        // validated by the bootstrap/template capture path.
        *reinterpret_cast<void**>(frameClone + 0x10) = function;
        *reinterpret_cast<void**>(frameClone + 0x28) = &params;

        ModLog(
            "[ElementalSystemExpanded] DRAGON BRIDGE DISPATCH: "
            "BridgeObject=%p UFunction=%p Reaction=%u Units=%u Power=%.3f "
            "AttackType=%u Seq=%d FrameClone=%p Locals=%p Param3=null.\n",
            object,
            function,
            static_cast<unsigned>(reaction),
            static_cast<unsigned>(units),
            attackPower,
            static_cast<unsigned>(attackType),
            sequence,
            frameClone,
            &params);

        Original_DragonBridgeBlueprintExecutionStub(
            object,
            frameClone,
            nullptr);

        ModLog(
            "[ElementalSystemExpanded] DRAGON BRIDGE DISPATCH RETURNED: "
            "Reaction=%u Seq=%d.\n",
            static_cast<unsigned>(reaction),
            sequence);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        ShipLog(
            "[ElementalSystemExpanded] DRAGON BRIDGE DISPATCH FAIL-CLOSED: "
            "exception during detached reflected invocation Reaction=%u Seq=%d.\n",
            static_cast<unsigned>(reaction),
            sequence);
        return false;
    }
}


// Defined after the current-status query/erosion helpers. Keeping only this
// narrow declaration here lets the trusted raw-damage wrapper decide Dragon
// semantics before elemental buildup without moving the existing status code.
static bool TryPrepareDragonReaction(
    void* damageReaction,
    const void* damageInfo,
    uint8_t attackElement,
    uint8_t* ioEffect1,
    int32_t* ioValue1,
    uint8_t* ioEffect2,
    int32_t* ioValue2);

static const char* ElementalEffectName(uint8_t effect) {
    if (effect == ActiveIds::Effect_Burn) return "Burn";
    if (effect == ActiveIds::Effect_Wetness) return "Wetness";
    if (effect == ActiveIds::Effect_Freeze) return "Freeze";
    if (effect == ActiveIds::Effect_Electrical) return "Electrical";
    if (effect == ActiveIds::Effect_Muddy) return "Muddy";
    if (effect == ActiveIds::Effect_IvyCling) return "IvyCling";
    if (effect == ActiveIds::Effect_Darkness) return "Darkness";
    return "Other";
}

static bool SnapshotRawDamageInfo(
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
        return IsReadableAddress(p + offset, size);
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
struct FRawEffectContext {
    bool Active = false;
    bool SuppressDragonBurn = false;
    uint8_t AttackElement = 0;
    uint8_t Effect1 = 0;
    int32_t Value1 = 0;
    uint8_t Effect2 = 0;
    int32_t Value2 = 0;
    bool Consumed1 = false;
    bool Consumed2 = false;
    bool DragonReactionSpread = false;
    int32_t DragonReactionSequence = 0;
};

static thread_local FRawEffectContext g_RawEffectContext[4];
static thread_local int g_RawEffectDepth = 0;

struct FRawEffectContextGuard {
    bool Entered = false;

    FRawEffectContextGuard(
        uint8_t attackElement,
        uint8_t effect1, int32_t value1,
        uint8_t effect2, int32_t value2,
        bool suppressDragonBurn,
        bool dragonReactionSpread,
        int32_t dragonReactionSequence)
    {
        if (g_RawEffectDepth >= 4)
            return;

        auto& ctx = g_RawEffectContext[g_RawEffectDepth++];
        ctx.Active = true;
        ctx.SuppressDragonBurn = suppressDragonBurn;
        ctx.AttackElement = attackElement;
        ctx.Effect1 = effect1;
        ctx.Value1 = value1;
        ctx.Effect2 = effect2;
        ctx.Value2 = value2;
        ctx.Consumed1 = false;
        ctx.Consumed2 = false;
        ctx.DragonReactionSpread = dragonReactionSpread;
        ctx.DragonReactionSequence = dragonReactionSequence;
        Entered = true;
    }

    ~FRawEffectContextGuard()
    {
        if (!Entered)
            return;

        --g_RawEffectDepth;
        g_RawEffectContext[g_RawEffectDepth] = {};
    }
};

static bool GetRawUnitsForEffect(
    uint8_t effect,
    int32_t* outRawValue,
    bool* outDragonReactionSpread,
    int32_t* outDragonReactionSequence)
{
    if (outDragonReactionSpread)
        *outDragonReactionSpread = false;
    if (outDragonReactionSequence)
        *outDragonReactionSequence = 0;
    if (!outRawValue || g_RawEffectDepth <= 0)
        return false;

    for (int depth = g_RawEffectDepth - 1; depth >= 0; --depth) {
        auto& ctx = g_RawEffectContext[depth];
        if (!ctx.Active)
            continue;

        if (!ctx.Consumed1 && ctx.Effect1 == effect) {
            ctx.Consumed1 = true;
            *outRawValue = ctx.Value1;
            if (outDragonReactionSpread)
                *outDragonReactionSpread = ctx.DragonReactionSpread;
            if (outDragonReactionSequence)
                *outDragonReactionSequence = ctx.DragonReactionSequence;
            return true;
        }

        if (!ctx.Consumed2 && ctx.Effect2 == effect) {
            ctx.Consumed2 = true;
            *outRawValue = ctx.Value2;
            if (outDragonReactionSpread)
                *outDragonReactionSpread = ctx.DragonReactionSpread;
            if (outDragonReactionSequence)
                *outDragonReactionSequence = ctx.DragonReactionSequence;
            return true;
        }
    }

    return false;
}

static bool GetRawAttackElement(uint8_t* outAttackElement)
{
    if (!outAttackElement || g_RawEffectDepth <= 0)
        return false;

    for (int depth = g_RawEffectDepth - 1; depth >= 0; --depth) {
        const auto& ctx = g_RawEffectContext[depth];
        if (!ctx.Active)
            continue;

        *outAttackElement = ctx.AttackElement;
        return true;
    }

    return false;
}

static bool ShouldSuppressDragonBurn(uint8_t effect)
{
    if (effect != ActiveIds::Effect_Burn || g_RawEffectDepth <= 0)
        return false;

    // Suppression belongs only to the innermost raw FPalDamageInfo call. A
    // nested damage transaction must never inherit its parent's spent-Burn flag.
    const auto& ctx =
        g_RawEffectContext[g_RawEffectDepth - 1];

    return ctx.Active && ctx.SuppressDragonBurn;
}

static uint8_t UnitsFromRawValue(int32_t rawValue)
{
    // User rule:
    //   1  -> 1U
    //   2  -> 2U
    //   >2 -> forced/capped at 2U
    // Non-positive values are not expected for a supported elemental effect;
    // treating them as 1U keeps the replacement conservative.
    return (rawValue >= 2) ? 2 : 1;
}

static int CountRel32CallsToTarget(
    uintptr_t functionStart,
    size_t scanSize,
    uintptr_t target)
{
    if (!functionStart || !target || !scanSize)
        return 0;

    int count = 0;
    for (size_t i = 0; i + 5 <= scanSize; ++i) {
        const uintptr_t at = functionStart + i;
        if (!IsReadableAddress(at, 5))
            continue;
        __try {
            if (*reinterpret_cast<const uint8_t*>(at) != 0xE8)
                continue;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            continue;
        }
        if (ResolveRel32Call(at) == target)
            ++count;
    }
    return count;
}

struct FRawCallerFunctionCandidate {
    uint32_t BeginRva = 0;
    uint32_t EndRva = 0;
    uint32_t FirstCallRva = 0;
    uint32_t SecondCallRva = 0;
    int CallsToBuildup = 0;
};

static bool LookupRuntimeFunctionBounds(
    uintptr_t moduleBase,
    uintptr_t address,
    uint32_t* outBeginRva,
    uint32_t* outEndRva)
{
    if (!moduleBase || !address || !outBeginRva || !outEndRva)
        return false;

    DWORD64 imageBase = 0;
    PRUNTIME_FUNCTION runtimeFunction = RtlLookupFunctionEntry(
        static_cast<DWORD64>(address),
        &imageBase,
        nullptr);

    if (!runtimeFunction ||
        static_cast<uintptr_t>(imageBase) != moduleBase) {
        return false;
    }

    const uint32_t beginRva = runtimeFunction->BeginAddress;
    const uint32_t endRva = runtimeFunction->EndAddress;

    if (beginRva >= endRva)
        return false;

    const uintptr_t begin = moduleBase + beginRva;
    const uintptr_t end = moduleBase + endRva;

    if (end <= begin ||
        !IsExecutableAddress(begin) ||
        !IsReadableAddress(begin, 1)) {
        return false;
    }

    *outBeginRva = beginRva;
    *outEndRva = endRva;
    return true;
}

struct FServerAddStatusOwnerXref {
    uint32_t RefRva = 0;
    uint32_t CallerBeginRva = 0;
    uint32_t CallerEndRva = 0;
    uint8_t Opcode = 0;
};

static void EmitServerAddStatusOwnerDiagnostics(
    uintptr_t moduleBase,
    uint32_t ownerBeginRva,
    uint32_t ownerEndRva,
    uintptr_t returnRva,
    uint8_t statusID)
{
    if (!moduleBase ||
        ownerBeginRva >= ownerEndRva ||
        returnRva > 0xFFFFFFFFull) {
        return;
    }

    // One runtime capture is enough: all later elemental AddStatus calls can be
    // compared against the same semantically discovered owner without flooding
    // disk/log output.
    if (InterlockedCompareExchange(
            &g_ServerAddStatusOwnerDiagnosticsDone,
            1,
            0) != 0) {
        return;
    }

    const uintptr_t ownerAddress =
        moduleBase + static_cast<uintptr_t>(ownerBeginRva);

    char ownerReason[384]{};
    snprintf(
        ownerReason,
        sizeof(ownerReason),
        "runtime AddStatus owner range=+0x%X..+0x%X "
        "StatusID=%u ReturnRVA=+0x%llX inferred-callsite=+0x%llX",
        ownerBeginRva,
        ownerEndRva,
        static_cast<unsigned>(statusID),
        static_cast<unsigned long long>(returnRva),
        static_cast<unsigned long long>(
            returnRva >= 5 ? returnRva - 5 : returnRva));

    AppendDiagnosticWindow(
        "PalServer.AddStatusOwner.Function",
        ownerReason,
        ownerBeginRva);

    if (returnRva >= 5) {
        AppendDiagnosticWindow(
            "PalServer.AddStatusOwner.Callsite",
            "return address observed inside native AddStatus hook; center is CALL-site candidate",
            returnRva - 5);
    }

    FExecutableSectionRange sections[96]{};
    size_t sectionCount = 0;
    uintptr_t imageSize = 0;

    if (!SnapshotExecutableSections(
            moduleBase,
            sections,
            96,
            &sectionCount,
            &imageSize)) {
        ShipLog(
            "[ElementalSystemExpanded] SERVER ADDSTATUS OWNER XREF PROBE: "
            "owner=+0x%X..+0x%X xref enumeration failed: executable-section snapshot unavailable.\n",
            ownerBeginRva,
            ownerEndRva);
        return;
    }

    std::vector<FServerAddStatusOwnerXref> refs;
    refs.reserve(16);

    size_t directCallCount = 0;
    size_t tailJumpCount = 0;

    for (size_t s = 0; s < sectionCount; ++s) {
        const uintptr_t sectionRva = sections[s].Rva;
        const uintptr_t sectionSize = sections[s].Size;

        if (!sectionSize ||
            sectionRva >= imageSize ||
            sectionSize > imageSize - sectionRva) {
            continue;
        }

        const uintptr_t begin = moduleBase + sectionRva;
        const uintptr_t end = begin + sectionSize;

        if (!IsReadableAddress(begin, 5))
            continue;

        for (uintptr_t at = begin; at + 5 <= end; ++at) {
            const uint8_t opcode =
                *reinterpret_cast<const uint8_t*>(at);

            if (opcode != 0xE8 && opcode != 0xE9)
                continue;

            int32_t rel = 0;
            memcpy(
                &rel,
                reinterpret_cast<const void*>(at + 1),
                sizeof(rel));

            const uintptr_t resolved =
                at + 5 + static_cast<intptr_t>(rel);

            if (resolved != ownerAddress)
                continue;

            if (opcode == 0xE8)
                ++directCallCount;
            else
                ++tailJumpCount;

            FServerAddStatusOwnerXref ref{};
            ref.RefRva =
                static_cast<uint32_t>(at - moduleBase);
            ref.Opcode = opcode;

            uint32_t callerBegin = 0;
            uint32_t callerEnd = 0;
            if (LookupRuntimeFunctionBounds(
                    moduleBase,
                    at,
                    &callerBegin,
                    &callerEnd)) {
                ref.CallerBeginRva = callerBegin;
                ref.CallerEndRva = callerEnd;
            }

            refs.push_back(ref);
        }
    }

    ShipLog(
        "[ElementalSystemExpanded] SERVER ADDSTATUS OWNER XREF PROBE: "
        "StatusID=%u Owner=+0x%X..+0x%X ReturnRVA=+0x%llX "
        "direct-CALL-xrefs=%llu tail-JMP-xrefs=%llu total=%llu\n",
        static_cast<unsigned>(statusID),
        ownerBeginRva,
        ownerEndRva,
        static_cast<unsigned long long>(returnRva),
        static_cast<unsigned long long>(directCallCount),
        static_cast<unsigned long long>(tailJumpCount),
        static_cast<unsigned long long>(refs.size()));

    // Emit each incoming direct rel32 edge and its runtime-function owner. This
    // remains diagnostic-only: no discovered caller is hooked or cached.
    for (size_t i = 0; i < refs.size() && i < 24; ++i) {
        const auto& ref = refs[i];

        char label[128]{};
        char reason[384]{};

        snprintf(
            label,
            sizeof(label),
            "PalServer.AddStatusOwner.Xref%llu",
            static_cast<unsigned long long>(i));

        if (ref.CallerBeginRva &&
            ref.CallerEndRva > ref.CallerBeginRva) {
            snprintf(
                reason,
                sizeof(reason),
                "%s to runtime AddStatus owner +0x%X; "
                "ref=+0x%X caller=+0x%X..+0x%X",
                ref.Opcode == 0xE8 ? "direct CALL" : "tail JMP",
                ownerBeginRva,
                ref.RefRva,
                ref.CallerBeginRva,
                ref.CallerEndRva);
        }
        else {
            snprintf(
                reason,
                sizeof(reason),
                "%s to runtime AddStatus owner +0x%X; "
                "ref=+0x%X caller=<unwind-unresolved>",
                ref.Opcode == 0xE8 ? "direct CALL" : "tail JMP",
                ownerBeginRva,
                ref.RefRva);
        }

        AppendDiagnosticWindow(
            label,
            reason,
            ref.RefRva);

        if (ref.CallerBeginRva &&
            ref.CallerEndRva > ref.CallerBeginRva) {
            char callerLabel[128]{};
            snprintf(
                callerLabel,
                sizeof(callerLabel),
                "PalServer.AddStatusOwner.Xref%llu.CallerFunction",
                static_cast<unsigned long long>(i));

            AppendDiagnosticWindow(
                callerLabel,
                reason,
                ref.CallerBeginRva);
        }
    }

    if (refs.empty()) {
        AppendDiagnosticWindow(
            "PalServer.AddStatusOwner.NoDirectXrefs",
            "no executable-section rel32 CALL/JMP targets the runtime AddStatus owner; likely indirect dispatch/reflection/RPC thunk",
            ownerBeginRva);
    }
}

static bool CollectRawCallerCandidates(
    uintptr_t moduleBase,
    uintptr_t target,
    std::vector<FRawCallerFunctionCandidate>* outCandidates,
    size_t* outTotalCalls)
{
    if (!moduleBase || !target || !outCandidates)
        return false;

    outCandidates->clear();
    if (outTotalCalls)
        *outTotalCalls = 0;

    // Reuse the existing Stage 6.x section snapshot helper. It isolates its
    // SEH internally, while this STL-owning function contains no __try/__except.
    FExecutableSectionRange sections[96]{};
    size_t sectionCount = 0;
    uintptr_t imageSize = 0;

    if (!SnapshotExecutableSections(
            moduleBase,
            sections,
            96,
            &sectionCount,
            &imageSize)) {
        return false;
    }

    std::unordered_map<uint32_t, size_t> byFunctionBegin;

    for (size_t s = 0; s < sectionCount; ++s) {
        const uintptr_t sectionRva = sections[s].Rva;
        const uintptr_t sectionSize = sections[s].Size;

        if (!sectionSize ||
            sectionRva >= imageSize ||
            sectionSize > imageSize - sectionRva) {
            continue;
        }

        const uintptr_t begin = moduleBase + sectionRva;
        const uintptr_t end = begin + sectionSize;

        // Same assumption already used by the existing executable-section
        // pattern scanner: once the mapped executable section is validated,
        // sequential byte reads are safe for this immutable module image.
        if (!IsReadableAddress(begin, 5))
            continue;

        for (uintptr_t at = begin; at + 5 <= end; ++at) {
            if (*reinterpret_cast<const uint8_t*>(at) != 0xE8)
                continue;

            int32_t rel = 0;
            memcpy(&rel, reinterpret_cast<const void*>(at + 1), sizeof(rel));

            const uintptr_t resolved =
                at + 5 + static_cast<intptr_t>(rel);

            if (resolved != target)
                continue;

            if (outTotalCalls)
                ++(*outTotalCalls);

            uint32_t functionBegin = 0;
            uint32_t functionEnd = 0;

            if (!LookupRuntimeFunctionBounds(
                    moduleBase,
                    at,
                    &functionBegin,
                    &functionEnd)) {
                continue;
            }

            auto it = byFunctionBegin.find(functionBegin);
            if (it == byFunctionBegin.end()) {
                FRawCallerFunctionCandidate candidate{};
                candidate.BeginRva = functionBegin;
                candidate.EndRva = functionEnd;
                candidate.FirstCallRva =
                    static_cast<uint32_t>(at - moduleBase);
                candidate.CallsToBuildup = 1;

                const size_t index = outCandidates->size();
                outCandidates->push_back(candidate);
                byFunctionBegin.emplace(functionBegin, index);
            }
            else {
                auto& candidate = (*outCandidates)[it->second];

                // A function start must always have one stable unwind end.
                if (candidate.EndRva != functionEnd)
                    return false;

                if (candidate.CallsToBuildup == 1) {
                    candidate.SecondCallRva =
                        static_cast<uint32_t>(at - moduleBase);
                }
                ++candidate.CallsToBuildup;
            }
        }
    }

    return true;
}


struct FRawParentCandidate {
    uint32_t BeginRva = 0;
    uint32_t EndRva = 0;
    uint32_t FirstCallToA = 0;
    uint32_t FirstCallToB = 0;
    int CallsToA = 0;
    int CallsToB = 0;
};

static bool CollectIncomingOwnersForTwoTargets(
    uintptr_t moduleBase,
    uintptr_t targetA,
    uintptr_t targetB,
    std::vector<FRawParentCandidate>* outParents)
{
    if (!moduleBase || !targetA || !targetB || !outParents)
        return false;

    outParents->clear();

    FExecutableSectionRange sections[96]{};
    size_t sectionCount = 0;
    uintptr_t imageSize = 0;

    if (!SnapshotExecutableSections(
            moduleBase,
            sections,
            96,
            &sectionCount,
            &imageSize)) {
        return false;
    }

    std::unordered_map<uint32_t, size_t> byFunctionBegin;

    for (size_t sidx = 0; sidx < sectionCount; ++sidx) {
        const uintptr_t sectionRva = sections[sidx].Rva;
        const uintptr_t sectionSize = sections[sidx].Size;

        if (!sectionSize ||
            sectionRva >= imageSize ||
            sectionSize > imageSize - sectionRva) {
            continue;
        }

        const uintptr_t begin = moduleBase + sectionRva;
        const uintptr_t end = begin + sectionSize;

        if (!IsReadableAddress(begin, 5))
            continue;

        for (uintptr_t at = begin; at + 5 <= end; ++at) {
            if (*reinterpret_cast<const uint8_t*>(at) != 0xE8)
                continue;

            int32_t rel = 0;
            memcpy(&rel, reinterpret_cast<const void*>(at + 1), sizeof(rel));

            const uintptr_t resolved =
                at + 5 + static_cast<intptr_t>(rel);

            const bool isA = resolved == targetA;
            const bool isB = resolved == targetB;

            if (!isA && !isB)
                continue;

            uint32_t ownerBegin = 0;
            uint32_t ownerEnd = 0;

            if (!LookupRuntimeFunctionBounds(
                    moduleBase,
                    at,
                    &ownerBegin,
                    &ownerEnd)) {
                continue;
            }

            auto found = byFunctionBegin.find(ownerBegin);
            if (found == byFunctionBegin.end()) {
                FRawParentCandidate parent{};
                parent.BeginRva = ownerBegin;
                parent.EndRva = ownerEnd;

                if (isA) {
                    parent.CallsToA = 1;
                    parent.FirstCallToA =
                        static_cast<uint32_t>(at - moduleBase);
                }
                if (isB) {
                    parent.CallsToB = 1;
                    parent.FirstCallToB =
                        static_cast<uint32_t>(at - moduleBase);
                }

                const size_t index = outParents->size();
                outParents->push_back(parent);
                byFunctionBegin.emplace(ownerBegin, index);
            }
            else {
                auto& parent = (*outParents)[found->second];

                if (parent.EndRva != ownerEnd)
                    return false;

                if (isA) {
                    ++parent.CallsToA;
                    if (!parent.FirstCallToA) {
                        parent.FirstCallToA =
                            static_cast<uint32_t>(at - moduleBase);
                    }
                }

                if (isB) {
                    ++parent.CallsToB;
                    if (!parent.FirstCallToB) {
                        parent.FirstCallToB =
                            static_cast<uint32_t>(at - moduleBase);
                    }
                }
            }
        }
    }

    return true;
}

static void EmitRawParentDiagnostics(
    uintptr_t moduleBase,
    const std::vector<FRawCallerFunctionCandidate>& splitCandidates)
{
    if (!moduleBase || splitCandidates.size() != 2)
        return;

    const uintptr_t targetA =
        moduleBase + splitCandidates[0].BeginRva;
    const uintptr_t targetB =
        moduleBase + splitCandidates[1].BeginRva;

    std::vector<FRawParentCandidate> parents;

    if (!CollectIncomingOwnersForTwoTargets(
            moduleBase,
            targetA,
            targetB,
            &parents)) {
        RecordResolver(
            "RawFPalDamageInfoParentProbe",
            "common-parent-diagnostic",
            EResolveConfidence::Failed,
            0,
            "failed to enumerate callers of the two split buildup helpers");
        return;
    }

    size_t commonCount = 0;
    uintptr_t commonAddress = 0;
    FRawParentCandidate common{};

    for (const auto& p : parents) {
        if (p.CallsToA > 0 && p.CallsToB > 0) {
            ++commonCount;
            commonAddress = moduleBase + p.BeginRva;
            common = p;
        }
    }

    char detail[512]{};
    int written = snprintf(
        detail,
        sizeof(detail),
        "splitA=+0x%X splitB=+0x%X incoming-owner-count=%llu common-owner-count=%llu",
        splitCandidates[0].BeginRva,
        splitCandidates[1].BeginRva,
        static_cast<unsigned long long>(parents.size()),
        static_cast<unsigned long long>(commonCount));

    for (size_t i = 0;
         i < parents.size() && i < 6 &&
         written > 0 && static_cast<size_t>(written) < sizeof(detail);
         ++i) {
        const auto& p = parents[i];
        const int appended = snprintf(
            detail + written,
            sizeof(detail) - static_cast<size_t>(written),
            " | P%llu=+0x%X..+0x%X A=%d@+0x%X B=%d@+0x%X",
            static_cast<unsigned long long>(i),
            p.BeginRva,
            p.EndRva,
            p.CallsToA,
            p.FirstCallToA,
            p.CallsToB,
            p.FirstCallToB);
        if (appended <= 0)
            break;
        written += appended;
    }

    RecordResolver(
        "RawFPalDamageInfoParentProbe",
        "common-parent-diagnostic",
        commonCount == 1
            ? EResolveConfidence::RuntimeValidated
            : EResolveConfidence::Failed,
        commonCount == 1 ? commonAddress : 0,
        detail);

    if (commonCount == 1) {
        AppendDiagnosticWindow(
            "RawCommonParent.Function",
            detail,
            common.BeginRva);

        if (common.FirstCallToA) {
            AppendDiagnosticWindow(
                "RawCommonParent.CallToSplitA",
                "direct call to split helper A",
                common.FirstCallToA);
        }

        if (common.FirstCallToB) {
            AppendDiagnosticWindow(
                "RawCommonParent.CallToSplitB",
                "direct call to split helper B",
                common.FirstCallToB);
        }
    }
    else {
        // Even without a unique common owner, dump up to the first four owner
        // functions so the next pass can identify whether the split occurs at
        // another level.
        for (size_t i = 0; i < parents.size() && i < 4; ++i) {
            char label[96]{};
            char reason[192]{};

            snprintf(
                label,
                sizeof(label),
                "RawParentCandidate%llu.Function",
                static_cast<unsigned long long>(i));

            snprintf(
                reason,
                sizeof(reason),
                "range=+0x%X..+0x%X A=%d@+0x%X B=%d@+0x%X",
                parents[i].BeginRva,
                parents[i].EndRva,
                parents[i].CallsToA,
                parents[i].FirstCallToA,
                parents[i].CallsToB,
                parents[i].FirstCallToB);

            AppendDiagnosticWindow(
                label,
                reason,
                parents[i].BeginRva);
        }
    }
}

static uintptr_t FindEffectCallerStart(uintptr_t moduleBase)
{
    if (!moduleBase || !g_Resolved_ElementBuildup)
        return 0;

    auto qualifies = [&](uintptr_t candidate) -> bool {
        if (!candidate || candidate < moduleBase)
            return false;

        uint32_t beginRva = 0;
        uint32_t endRva = 0;

        if (!LookupRuntimeFunctionBounds(
                moduleBase,
                candidate,
                &beginRva,
                &endRva)) {
            return false;
        }

        const uintptr_t candidateRva = candidate - moduleBase;
        if (candidateRva > 0xFFFFFFFFull ||
            beginRva != static_cast<uint32_t>(candidateRva) ||
            endRva <= beginRva) {
            return false;
        }

        const size_t functionSize =
            static_cast<size_t>(endRva - beginRva);

        // Exclude tiny thunks and implausibly huge dispatcher functions.
        if (functionSize < 0x20 || functionSize > 0x4000)
            return false;

        return CountRel32CallsToTarget(
            candidate,
            functionSize,
            g_Resolved_ElementBuildup) >= 2;
    };

    const uintptr_t cachedRva = CachedProfileValue(
        g_BuildProfileCache.Rvas,
        "RawFPalDamageInfoCaller");

    if (cachedRva) {
        const uintptr_t cached = moduleBase + cachedRva;
        if (qualifies(cached)) {
            RecordResolver(
                "RawFPalDamageInfoCaller",
                "fingerprint-profile+rtl-function+dual-call",
                EResolveConfidence::ProfileStatic,
                cached,
                "cached raw caller revalidated by RtlLookupFunctionEntry and >=2 direct calls to resolved buildup");
            CacheResolvedRva("RawFPalDamageInfoCaller", cached);
            return cached;
        }
    }

    std::vector<FRawCallerFunctionCandidate> candidates;
    size_t totalCalls = 0;

    if (!CollectRawCallerCandidates(
            moduleBase,
            g_Resolved_ElementBuildup,
            &candidates,
            &totalCalls)) {
        RecordResolver(
            "RawFPalDamageInfoCaller",
            "rtl-function+dual-call",
            EResolveConfidence::Failed,
            0,
            "failed to enumerate direct buildup calls / runtime-function ownership");
        AppendDiagnosticWindow(
            "RawFPalDamageInfoCaller",
            "runtime-function enumeration failed",
            Known104::Rva::RawEffectCaller);
        return 0;
    }

    uintptr_t unique = 0;
    uint32_t uniqueEndRva = 0;
    int uniqueCallCount = 0;
    size_t semanticCandidates = 0;

    for (const auto& candidate : candidates) {
        if (candidate.CallsToBuildup < 2 ||
            candidate.EndRva <= candidate.BeginRva) {
            continue;
        }

        const uint32_t functionSize =
            candidate.EndRva - candidate.BeginRva;

        if (functionSize < 0x20 || functionSize > 0x4000)
            continue;

        unique = moduleBase + candidate.BeginRva;
        uniqueEndRva = candidate.EndRva;
        uniqueCallCount = candidate.CallsToBuildup;
        ++semanticCandidates;
    }

    if (semanticCandidates != 1 || !unique) {
        char detail[512]{};
        int written = snprintf(
            detail,
            sizeof(detail),
            "direct-calls=%llu owning-functions=%llu dual-call-functions=%llu",
            static_cast<unsigned long long>(totalCalls),
            static_cast<unsigned long long>(candidates.size()),
            static_cast<unsigned long long>(semanticCandidates));

        // Preserve the exact split topology in the resolver report so the
        // next step can reason about the server ABI instead of guessing.
        for (size_t i = 0;
             i < candidates.size() && i < 4 &&
             written > 0 && static_cast<size_t>(written) < sizeof(detail);
             ++i) {
            const auto& c = candidates[i];
            const int appended = snprintf(
                detail + written,
                sizeof(detail) - static_cast<size_t>(written),
                " | C%llu=+0x%X..+0x%X calls=%d call1=+0x%X call2=+0x%X",
                static_cast<unsigned long long>(i),
                c.BeginRva,
                c.EndRva,
                c.CallsToBuildup,
                c.FirstCallRva,
                c.SecondCallRva);
            if (appended <= 0)
                break;
            written += appended;
        }

        RecordResolver(
            "RawFPalDamageInfoCaller",
            "rtl-function-split-diagnostic",
            EResolveConfidence::Failed,
            0,
            detail);

        // Dump both the function entry and the actual buildup CALL for every
        // candidate. These windows are diagnostic only; no candidate is hooked
        // until we can prove its ABI.
        for (size_t i = 0; i < candidates.size() && i < 4; ++i) {
            const auto& c = candidates[i];

            char functionLabel[96]{};
            char functionReason[192]{};
            snprintf(
                functionLabel,
                sizeof(functionLabel),
                "RawCallerCandidate%llu.Function",
                static_cast<unsigned long long>(i));
            snprintf(
                functionReason,
                sizeof(functionReason),
                "range=+0x%X..+0x%X calls=%d call1=+0x%X call2=+0x%X",
                c.BeginRva,
                c.EndRva,
                c.CallsToBuildup,
                c.FirstCallRva,
                c.SecondCallRva);
            AppendDiagnosticWindow(
                functionLabel,
                functionReason,
                c.BeginRva);

            if (c.FirstCallRva) {
                char callLabel[96]{};
                snprintf(
                    callLabel,
                    sizeof(callLabel),
                    "RawCallerCandidate%llu.Call1",
                    static_cast<unsigned long long>(i));
                AppendDiagnosticWindow(
                    callLabel,
                    "direct CALL to resolved elemental buildup",
                    c.FirstCallRva);
            }

            if (c.SecondCallRva) {
                char callLabel[96]{};
                snprintf(
                    callLabel,
                    sizeof(callLabel),
                    "RawCallerCandidate%llu.Call2",
                    static_cast<unsigned long long>(i));
                AppendDiagnosticWindow(
                    callLabel,
                    "second direct CALL to resolved elemental buildup",
                    c.SecondCallRva);
            }
        }

        // Dedicated PalServer currently splits the two elemental buildup calls
        // across two nearly-identical helpers. Probe one level upward for a
        // unique parent that directly calls both helpers. Diagnostic only:
        // no parent is hooked until its ABI is proven from emitted windows.
        if (candidates.size() == 2) {
            EmitRawParentDiagnostics(
                moduleBase,
                candidates);
        }

        return 0;
    }

    const uintptr_t uniqueRva = unique - moduleBase;
    const intptr_t historicalShift =
        static_cast<intptr_t>(uniqueRva) -
        static_cast<intptr_t>(Known104::Rva::RawEffectCaller);

    // Do not feed this independently linked server function into the global
    // RVA-shift hint. Its identity is semantic, not historical-locality based.
    char detail[192]{};
    snprintf(
        detail,
        sizeof(detail),
        "function=+0x%llX..+0x%X calls=%d historical-delta=%+lld",
        static_cast<unsigned long long>(uniqueRva),
        uniqueEndRva,
        uniqueCallCount,
        static_cast<long long>(historicalShift));

    RecordResolver(
        "RawFPalDamageInfoCaller",
        "rtl-function+dual-call",
        EResolveConfidence::Strong,
        unique,
        detail);

    CacheResolvedRva("RawFPalDamageInfoCaller", unique);
    return unique;
}

// Keep SEH isolated from Detour_DamageEffectCaller. That detour owns
// FRawEffectContextGuard (a C++ object with a destructor), and MSVC rejects
// __try in any function that requires C++ object unwinding (C2712).
// Use the active generated/validated layout rather than hardcoding the historical
// 1.0.4 +0x30 offset.
static uint8_t SafeReadAttackElement(const void* damageInfo)
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

static uintptr_t __fastcall Detour_DamageEffectCaller(
    void* damageReaction,
    const void* damageInfo)
{
    // Once the live bridge has been captured, remove the temporary global
    // Blueprint execution-stub hook at a safe raw-damage boundary. Retain the
    // One-shot detached replay validation; successful return authorizes the same
    // reflected execution path for gameplay dispatch.
    MaybeRetireDragonBridgeCaptureHook();
    TryDragonBridgeDetachedReplayValidation();

    uint8_t effect1 = 0;
    int32_t value1 = 0;
    uint8_t effect2 = 0;
    int32_t value2 = 0;

    const bool haveRaw = SnapshotRawDamageInfo(
        damageInfo, &effect1, &value1, &effect2, &value2);

    const uint8_t attackElement =
        haveRaw ? SafeReadAttackElement(damageInfo) : 0;

    int32_t dragonSpreadSequence = 0;
    const bool dragonReactionSpread =
        haveRaw &&
        MatchDragonSpreadContext(
            damageInfo,
            attackElement,
            effect1,
            value1,
            effect2,
            value2,
            &dragonSpreadSequence);

    if (dragonReactionSpread) {
        ModLog(
            "[ElementalSystemExpanded] DRAGON SPREAD CONTEXT MATCH: "
            "Seq=%d DamageReaction=%p DamageInfo=%p Element=%u "
            "E1=%u V1=%d E2=%u V2=%d.\n",
            dragonSpreadSequence,
            damageReaction,
            damageInfo,
            static_cast<unsigned>(attackElement),
            static_cast<unsigned>(effect1),
            value1,
            static_cast<unsigned>(effect2),
            value2);
    }

    bool dragonReactionPrepared = false;
    if (haveRaw && !dragonReactionSpread) {
        // A packet already authenticated by the out-of-band spread context is a
        // reaction result, never a new Dragon-reaction trigger. The current custom
        // BP carriers are non-Dragon elements, but keep this invariant explicit.
        dragonReactionPrepared =
            TryPrepareDragonReaction(
                damageReaction,
                damageInfo,
                attackElement,
                &effect1,
                &value1,
                &effect2,
                &value2);
    }

    FRawEffectContextGuard guard(
        haveRaw ? attackElement : 0,
        haveRaw ? effect1 : 0,
        haveRaw ? value1 : 0,
        haveRaw ? effect2 : 0,
        haveRaw ? value2 : 0,
        dragonReactionPrepared,
        dragonReactionSpread,
        dragonSpreadSequence);

    if (haveRaw &&
        (IsTrackedElementalEffect(effect1) ||
            IsTrackedElementalEffect(effect2))) {
        ModLog(
            "[ElementalSystemExpanded] RAW UNITS: DamageReaction=%p DamageInfo=%p "
            "AttackElement=%u | E1=%u (%s) V1=%d | E2=%u (%s) V2=%d "
            "SpreadContext=%s Seq=%d\n",
            damageReaction,
            damageInfo,
            static_cast<unsigned>(attackElement),
            static_cast<unsigned>(effect1), ElementalEffectName(effect1), value1,
            static_cast<unsigned>(effect2), ElementalEffectName(effect2), value2,
            dragonReactionSpread ? "YES" : "NO",
            dragonSpreadSequence);
    }

    uintptr_t result = 0;
    if (Original_DamageEffectCaller)
        result = Original_DamageEffectCaller(damageReaction, damageInfo);

    // Lazy arm occurs after the native raw-damage work but before the outer
    // PalUtility call returns to Lua. The same call's Lua post-hook can then
    // self-call BP_ESEBridge; the shared Blueprint execution stub exposes the
    // exact UFunction and locals buffer for bootstrap capture.
    if (InterlockedCompareExchange(&g_DragonBridgeReady, 0, 0) == 0)
        EnsureDragonBridgeCaptureHook(damageReaction);

    return result;
}

// ---------------------------------------------------------
// Native elemental status replacement
// ---------------------------------------------------------
using AddElementStatusAdditionalValue_OneType_t =
void(__fastcall*)(void* DamageReactionComponent, uint8_t Effect, float Value);

using NativeAddStatus_t =
void(__fastcall*)(void* StatusComponent, uint8_t StatusID);


// Stage 6.7.4.24 observation marker. The SDK layout is:
//   +0x00 GeneralIndex
//   +0x04 FName (8 bytes)
//   +0x0C GeneralFloatValue
//   +0x10 FPalInstanceID (0x30 bytes)
struct FStatusDynamicParameterProbe {
    int32_t GeneralIndex = 0;
    uint32_t GeneralNameComparisonIndex = 0;
    uint32_t GeneralNameNumber = 0;
    float GeneralFloatValue = 0.0f;
    uint8_t GeneralInstanceID[0x30]{};
};
static_assert(sizeof(FStatusDynamicParameterProbe) == 0x40,
    "FStatusDynamicParameter probe ABI drift");
static_assert(offsetof(FStatusDynamicParameterProbe, GeneralFloatValue) == 0x0C,
    "FStatusDynamicParameter::GeneralFloatValue probe ABI drift");

using NativeAddStatusParameterProbe_t =
void(__fastcall*)(
    void* StatusComponent,
    uint8_t StatusID,
    const FStatusDynamicParameterProbe* Param);

static uintptr_t ResolveAddStatusParameterSiblingProbe(uintptr_t addStatus)
{
    if (!addStatus || !IsExecutableAddress(addStatus))
        return 0;

    std::vector<uintptr_t> candidates;
    for (size_t off = 0; off < 0x180; ++off) {
        const uintptr_t at = addStatus + off;
        if (!IsReadableAddress(at, 10))
            break;

        const uint8_t* p = reinterpret_cast<const uint8_t*>(at);
        size_t callOffset = 0;
        if (p[0] == 0x4C && p[1] == 0x8D && p[2] == 0x45 && p[4] == 0xE8)
            callOffset = 4;
        else if (p[0] == 0x4C && p[1] == 0x8D && p[2] == 0x44 &&
                 p[3] == 0x24 && p[5] == 0xE8)
            callOffset = 5;
        else
            continue;

        const uintptr_t target = ResolveRel32Call(at + callOffset);
        if (!target || target == addStatus || !IsExecutableAddress(target))
            continue;
        if (std::find(candidates.begin(), candidates.end(), target) == candidates.end())
            candidates.push_back(target);
    }
    return candidates.size() == 1 ? candidates[0] : 0;
}

static AddElementStatusAdditionalValue_OneType_t
Original_AddElementStatusAdditionalValue_OneType = nullptr;

// PalServer reaction capabilities are deliberately split.
//
// Native reaction gates (named wrapper-backed functions) are sufficient for:
//   * Electrical dry suppression / Wet->Electrical strong control
//   * Wet->Freeze strong control and 14s reaction ICD observation
//
// Dry Freeze additionally requires three virtual gates so the ordinary Freeze
// status can exist without hard immobilization / montage cancellation / position
// pinning. Keeping these capabilities separate avoids disabling Electrical and
// Wet->Freeze just because the dry-Freeze virtual wrappers moved on PalServer.
static volatile LONG g_ReactionCapabilityReady = 0;
static volatile LONG g_DryFreezeVirtualCapabilityReady = 0;

static NativeAddStatus_t Original_NativeAddStatus = nullptr;
static NativeAddStatus_t Native_AddStatus = nullptr;
static NativeAddStatusParameterProbe_t Original_NativeAddStatusParameterProbe = nullptr;

static constexpr double kElementICDSeconds = 2.5;
static constexpr float kOneUnitDuration = 7.5f;
static constexpr float kTwoUnitDuration = 12.0f;
static constexpr float kDarknessDuration = 3.0f;
static constexpr float kWetFreezeDuration = 2.0f;
static constexpr float kMinimumExchangeCarryover = 2.5f;
// Never write an eroded status timer to exact zero. Palworld's native status
// lifecycle expects a positive timer to cross through zero inside TickStatus;
// writing 0 directly can strand the status object in ExecutionStatusList.
static constexpr float kErosionExpiryTick = 0.001f;
static constexpr uint8_t kMaxConsecutiveHits = 3;

static float GaugeDurationForUnits(uint8_t units)
{
    return (units >= 2)
        ? kTwoUnitDuration
        : kOneUnitDuration;
}

static float MaxDurationForStatus(uint8_t statusID)
{
    // Genuine Dark and Light blindness share native Darkness status ID 25.
    // Both retain the established 3-second maximum despite the generic
    // 7.5s / 12s gauge budget used by the exchange system.
    return (statusID == ActiveIds::Status_Darkness)
        ? kDarknessDuration
        : kTwoUnitDuration;
}

static uint8_t ElementalEffectToStatusID(uint8_t effect) {
    if (effect == ActiveIds::Effect_Burn) return ActiveIds::Status_Burn;
    if (effect == ActiveIds::Effect_Wetness) return ActiveIds::Status_Wetness;
    if (effect == ActiveIds::Effect_Freeze) return ActiveIds::Status_Freeze;
    if (effect == ActiveIds::Effect_Electrical) return ActiveIds::Status_Electrical;
    if (effect == ActiveIds::Effect_Muddy) return ActiveIds::Status_Muddy;
    if (effect == ActiveIds::Effect_IvyCling) return ActiveIds::Status_IvyCling;
    if (effect == ActiveIds::Effect_Darkness) return ActiveIds::Status_Darkness;
    return 0;
}

static uint8_t ElementalStatusIDToEffect(uint8_t statusID)
{
    if (statusID == ActiveIds::Status_Burn) return ActiveIds::Effect_Burn;
    if (statusID == ActiveIds::Status_Wetness) return ActiveIds::Effect_Wetness;
    if (statusID == ActiveIds::Status_Freeze) return ActiveIds::Effect_Freeze;
    if (statusID == ActiveIds::Status_Electrical) return ActiveIds::Effect_Electrical;
    if (statusID == ActiveIds::Status_Muddy) return ActiveIds::Effect_Muddy;
    if (statusID == ActiveIds::Status_IvyCling) return ActiveIds::Effect_IvyCling;
    if (statusID == ActiveIds::Status_Darkness) return ActiveIds::Effect_Darkness;
    return 0;
}

static bool IsTrackedElementalStatusID(uint8_t statusID)
{
    return statusID == ActiveIds::Status_Burn ||
        statusID == ActiveIds::Status_Wetness ||
        statusID == ActiveIds::Status_Freeze ||
        statusID == ActiveIds::Status_Electrical ||
        statusID == ActiveIds::Status_Muddy ||
        statusID == ActiveIds::Status_IvyCling ||
        statusID == ActiveIds::Status_Darkness;
}

static uint8_t NominalElementForEffect(uint8_t effect)
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

static uint8_t NominalElementForStatus(uint8_t statusID)
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

static uint8_t ResolveIncomingElement(uint8_t effect)
{
    uint8_t attackElement = ActiveIds::Element_None;
    if (GetRawAttackElement(&attackElement) &&
        attackElement >= ActiveIds::Element_Normal &&
        attackElement <= ActiveIds::Element_Dragon) {
        // Dragon Waza carry an ordinary Fire-sourced Burn aggregate whenever
        // no Dragon reaction consumes that aggregate. Dragon itself remains
        // outside the normal elemental exchange wheel.
        if (attackElement == ActiveIds::Element_Dragon &&
            effect == ActiveIds::Effect_Burn) {
            return ActiveIds::Element_Fire;
        }
        return attackElement;
    }

    return NominalElementForEffect(effect);
}

static const char* StatusIDName(uint8_t statusID) {
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
static bool ReadPointerAtOffset(
    uintptr_t object,
    uintptr_t offset,
    uintptr_t* outValue)
{
    if (!object || !outValue ||
        offset > (std::numeric_limits<uintptr_t>::max)() - object) {
        return false;
    }

    const uintptr_t fieldAddress = object + offset;
    if (!IsReadableAddress(fieldAddress, sizeof(uintptr_t)))
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

static bool TryResolveOwningPalCharacterWithOuterOffset(
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
        if (!ReadPointerAtOffset(current, outerOffset, &outer) || !outer)
            return false;

        uintptr_t reverse = 0;
        if (ReadPointerAtOffset(
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

static void* ResolveOwningPalCharacter(void* damageReaction) {
    if (!damageReaction)
        return nullptr;

    uintptr_t cached = 0;
    AcquireSRWLockShared(&g_UObjectOuterOffsetLock);
    cached = g_RuntimeUObjectOuterOffset;
    ReleaseSRWLockShared(&g_UObjectOuterOffsetLock);

    void* character = nullptr;
    if (cached &&
        TryResolveOwningPalCharacterWithOuterOffset(
            damageReaction, cached, &character)) {
        return character;
    }

    AcquireSRWLockExclusive(&g_UObjectOuterOffsetLock);

    // Another thread may have discovered it while we waited.
    if (g_RuntimeUObjectOuterOffset &&
        TryResolveOwningPalCharacterWithOuterOffset(
            damageReaction,
            g_RuntimeUObjectOuterOffset,
            &character)) {
        ReleaseSRWLockExclusive(&g_UObjectOuterOffsetLock);
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

    if (TryResolveOwningPalCharacterWithOuterOffset(
        damageReaction, hint, &character)) {
        g_RuntimeUObjectOuterOffset = hint;
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
            if (TryResolveOwningPalCharacterWithOuterOffset(
                damageReaction, off, &candidateCharacter)) {
                if (count < 32) {
                    candidates[count] = off;
                    candidateCharacters[count] = candidateCharacter;
                    ++count;
                }
            }
        }

        if (count == 1) {
            g_RuntimeUObjectOuterOffset = candidates[0];
            character = candidateCharacters[0];
        }
        else {
            ShipLog(
                "[ElementalSystemExpanded] WARNING: OWNER LINK DISCOVERY %s: DamageReaction=%p "
                "Candidates=%d; no owner returned.\n",
                count == 0 ? "FAILED" : "AMBIGUOUS",
                damageReaction,
                count);
            ReleaseSRWLockExclusive(&g_UObjectOuterOffsetLock);
            return nullptr;
        }
    }

    const uintptr_t discovered = g_RuntimeUObjectOuterOffset;
    const bool firstLog = !g_RuntimeUObjectOuterLogged;
    g_RuntimeUObjectOuterLogged = true;
    ReleaseSRWLockExclusive(&g_UObjectOuterOffsetLock);

    if (firstLog) {
        char detail[192]{};
        snprintf(
            detail,
            sizeof(detail),
            "discovered Outer link +0x%llX via APalCharacter::DamageReactionComponent reverse pointer",
            static_cast<unsigned long long>(discovered));
        RecordResolver(
            "Runtime.UObjectOuterPrivate",
            "runtime-owner-backreference",
            EResolveConfidence::RuntimeValidated,
            0,
            detail);
        ModLog(
            "[ElementalSystemExpanded] OWNER LINK DISCOVERY PASSED: "
            "OuterOffset=+0x%llX DamageReaction=%p Character=%p.\n",
            static_cast<unsigned long long>(discovered),
            damageReaction,
            character);
        MarkRuntimeSnapshotDirty();
        // No file I/O here: runtime discovery happens on a gameplay path.
        // The one-time console record is enough; persistent artifacts remain
        // startup-only to avoid introducing action-time disk latency.
    }

    return character;
}

static void* ResolveStatusComponent(void* damageReaction) {
    void* character = ResolveOwningPalCharacter(damageReaction);
    if (!character)
        return nullptr;

    uintptr_t statusComponent = 0;
    if (!ReadPointerAtOffset(
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
struct FElementICDState {
    double LastSuccessfulApplicationTime = 0.0;
    uint8_t ConsecutiveBlockedHits = 0;
    bool HasSuccessfulApplication = false;
};

struct FElementICDKey {
    void* DamageReaction = nullptr;
    uint8_t Effect = 0;

    bool operator==(const FElementICDKey& other) const {
        return DamageReaction == other.DamageReaction && Effect == other.Effect;
    }
};

struct FElementICDKeyHash {
    size_t operator()(const FElementICDKey& key) const noexcept {
        const size_t p = std::hash<uintptr_t>{}(
            reinterpret_cast<uintptr_t>(key.DamageReaction));
        const size_t e = std::hash<unsigned>{}(key.Effect);
        return p ^ (e + static_cast<size_t>(0x9E3779B9u) + (p << 6) + (p >> 2));
    }
};

static std::unordered_map<
    FElementICDKey,
    FElementICDState,
    FElementICDKeyHash> g_ElementICD;

static SRWLOCK g_ElementICDLock = SRWLOCK_INIT;

static double GetMonotonicSeconds()
{
    return static_cast<double>(GetTickCount64()) / 1000.0;
}

static bool ElementICDAllows(
    void* damageReaction,
    uint8_t effect,
    uint8_t* outPreviousBlockedHits,
    double* outElapsed)
{
    const double now = GetMonotonicSeconds();
    const FElementICDKey key{ damageReaction, effect };

    AcquireSRWLockExclusive(&g_ElementICDLock);
    auto& state = g_ElementICD[key];

    *outPreviousBlockedHits = state.ConsecutiveBlockedHits;
    *outElapsed = state.HasSuccessfulApplication
        ? (now - state.LastSuccessfulApplicationTime)
        : 1.0e30;

    if (!state.HasSuccessfulApplication) {
        ReleaseSRWLockExclusive(&g_ElementICDLock);
        return true;
    }

    if (*outElapsed >= kElementICDSeconds) {
        // A timer-expired application is allowed immediately. The successful
        // application record will clear the hit chain below.
        ReleaseSRWLockExclusive(&g_ElementICDLock);
        return true;
    }

    // Current registration becomes hit #1/#2/#3 after the last successful
    // application. Third registration is allowed.
    if (state.ConsecutiveBlockedHits + 1 >= kMaxConsecutiveHits) {
        ReleaseSRWLockExclusive(&g_ElementICDLock);
        return true;
    }

    ++state.ConsecutiveBlockedHits;
    *outPreviousBlockedHits = state.ConsecutiveBlockedHits;

    ReleaseSRWLockExclusive(&g_ElementICDLock);
    return false;
}

static void ElementICDRecordSuccess(void* damageReaction, uint8_t effect)
{
    const FElementICDKey key{ damageReaction, effect };

    AcquireSRWLockExclusive(&g_ElementICDLock);
    auto& state = g_ElementICD[key];
    state.LastSuccessfulApplicationTime = GetMonotonicSeconds();
    state.ConsecutiveBlockedHits = 0;
    state.HasSuccessfulApplication = true;
    ReleaseSRWLockExclusive(&g_ElementICDLock);
}

static void ElementICDClear(void* damageReaction, uint8_t effect)
{
    if (!damageReaction || !effect)
        return;

    const FElementICDKey key{ damageReaction, effect };

    AcquireSRWLockExclusive(&g_ElementICDLock);
    g_ElementICD.erase(key);
    ReleaseSRWLockExclusive(&g_ElementICDLock);
}

// ---------------------------------------------------------
// Status list / duration helpers
// ---------------------------------------------------------
struct FRawTArray {
    uintptr_t Data;
    int32_t Num;
    int32_t Max;
};

static_assert(sizeof(FRawTArray) == 0x10, "Unexpected local TArray ABI mirror size");
static_assert(offsetof(FRawTArray, Data) == 0x0, "Unexpected local TArray Data offset");
static_assert(offsetof(FRawTArray, Num) == 0x8, "Unexpected local TArray Num offset");
static_assert(offsetof(FRawTArray, Max) == 0xC, "Unexpected local TArray Max offset");

static bool ValidateRawPointerArray(
    const FRawTArray& list,
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

    return IsReadableAddress(
        list.Data,
        count * sizeof(void*));
}

static bool ValidateUObjectLikePointer(
    void* object,
    size_t readableSpan)
{
    if (!object)
        return false;

    const uintptr_t address = reinterpret_cast<uintptr_t>(object);
    if ((address % alignof(void*)) != 0 ||
        !IsReadableAddress(address, readableSpan)) {
        return false;
    }

    uintptr_t vtable = 0;
    uintptr_t firstVirtual = 0;
    __try {
        vtable = *reinterpret_cast<const uintptr_t*>(address);
        if (!vtable || !IsReadableAddress(vtable, sizeof(uintptr_t)))
            return false;
        firstVirtual = *reinterpret_cast<const uintptr_t*>(vtable);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }

    return IsExecutableAddress(firstVirtual);
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
static LONG g_LightNeutralLayoutWarningLogged = 0;

static bool IsNeutralPalTarget(
    void* character,
    bool* outLayoutValidated)
{
    if (outLayoutValidated)
        *outLayoutValidated = false;

    if (!character)
        return false;

    uintptr_t characterParameter = 0;
    uintptr_t staticParameter = 0;

    if (!ReadPointerAtOffset(
            reinterpret_cast<uintptr_t>(character),
            ActiveLayout::Character_CharacterParameterComponent,
            &characterParameter) ||
        !ReadPointerAtOffset(
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

    if (!ValidateUObjectLikePointer(
            reinterpret_cast<void*>(characterParameter),
            characterParameterSpan) ||
        !ValidateUObjectLikePointer(
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

static bool ShouldBlockLightBlindOnNeutralPal(
    void* character,
    uint8_t effect)
{
    if (effect != ActiveIds::Effect_Darkness)
        return false;

    // Preserve the already-validated branch discriminator: the blindness carrier
    // must be Darkness and the captured attack element must be Normal/Light.
    uint8_t attackElement = ActiveIds::Element_None;
    if (!GetRawAttackElement(&attackElement) ||
        attackElement != ActiveIds::Element_Normal) {
        return false;
    }

    bool layoutValidated = false;
    const bool neutralPal =
        IsNeutralPalTarget(
            character,
            &layoutValidated);

    if (!layoutValidated) {
        if (InterlockedCompareExchange(
                &g_LightNeutralLayoutWarningLogged,
                1,
                0) == 0) {
            ShipLog(
                "[ElementalSystemExpanded] WARNING: Light Neutral-immunity "
                "target layout could not be validated; failing open and "
                "preserving existing status behavior. Character=%p\n",
                character);
        }
        return false;
    }

    return neutralPal;
}

static bool FindStatusInstance(
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

    FRawTArray list{};
    __try {
        list = *reinterpret_cast<FRawTArray*>(
            reinterpret_cast<uintptr_t>(statusComponent) + ActiveLayout::StatusComponent_ExecutionStatusList);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }

    if (!ValidateRawPointerArray(list, 1024, 4096)) {
        if (g_DebugDiagnosticsEnabled) {
            InterlockedIncrement(&g_StatusArrayValidationFailures);
            MarkRuntimeSnapshotDirty();
        }
        ShipLog(
            "[ElementalSystemExpanded] WARNING: Status array ABI/layout invalid. "
            "Component=%p Data=%p Num=%d Max=%d\n",
            statusComponent,
            reinterpret_cast<void*>(list.Data),
            list.Num,
            list.Max);
        return false;
    }

    if (g_DebugDiagnosticsEnabled)
        InterlockedIncrement(&g_StatusArrayValidationPasses);

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

        if (!ValidateUObjectLikePointer(
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
            duration <= kErosionExpiryTick ||
            timer <= kErosionExpiryTick) {
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

struct FCurrentElementalStatus {
    void* Instance = nullptr;
    uint8_t StatusID = 0;
    float Duration = 0.0f;
    float Timer = 0.0f;
};

struct FElementalStatusProvenance {
    void* Instance = nullptr;
    uint8_t StatusID = 0;
    uint8_t SourceElement = 0;
};

static std::unordered_map<void*, FElementalStatusProvenance>
    g_ElementalStatusProvenance;
static SRWLOCK g_ElementalStatusProvenanceLock = SRWLOCK_INIT;

static void RememberElementalStatusProvenance(
    void* statusComponent,
    void* status,
    uint8_t statusID,
    uint8_t sourceElement)
{
    if (!statusComponent || !status || !IsTrackedElementalStatusID(statusID))
        return;

    if (sourceElement < ActiveIds::Element_Normal ||
        sourceElement > ActiveIds::Element_Dragon) {
        sourceElement = NominalElementForStatus(statusID);
    }

    AcquireSRWLockExclusive(&g_ElementalStatusProvenanceLock);
    g_ElementalStatusProvenance[statusComponent] = {
        status,
        statusID,
        sourceElement
    };
    ReleaseSRWLockExclusive(&g_ElementalStatusProvenanceLock);
}

static void ClearElementalStatusProvenance(void* statusComponent)
{
    if (!statusComponent)
        return;

    AcquireSRWLockExclusive(&g_ElementalStatusProvenanceLock);
    g_ElementalStatusProvenance.erase(statusComponent);
    ReleaseSRWLockExclusive(&g_ElementalStatusProvenanceLock);
}

static uint8_t ResolveCurrentElementSource(
    void* statusComponent,
    const FCurrentElementalStatus& current)
{
    FElementalStatusProvenance provenance{};
    bool matched = false;

    AcquireSRWLockShared(&g_ElementalStatusProvenanceLock);
    const auto it = g_ElementalStatusProvenance.find(statusComponent);
    if (it != g_ElementalStatusProvenance.end()) {
        provenance = it->second;
        matched = provenance.Instance == current.Instance &&
            provenance.StatusID == current.StatusID;
    }
    ReleaseSRWLockShared(&g_ElementalStatusProvenanceLock);

    if (matched &&
        provenance.SourceElement >= ActiveIds::Element_Normal &&
        provenance.SourceElement <= ActiveIds::Element_Dragon) {
        return provenance.SourceElement;
    }

    return NominalElementForStatus(current.StatusID);
}

enum class ECurrentElementalStatusQuery : uint8_t {
    Failed = 0,
    None = 1,
    Found = 2
};

static ECurrentElementalStatusQuery QueryCurrentElementalStatus(
    void* statusComponent,
    FCurrentElementalStatus* outCurrent)
{
    if (!statusComponent || !outCurrent)
        return ECurrentElementalStatusQuery::Failed;

    *outCurrent = {};

    FRawTArray list{};
    __try {
        list = *reinterpret_cast<FRawTArray*>(
            reinterpret_cast<uintptr_t>(statusComponent) +
            ActiveLayout::StatusComponent_ExecutionStatusList);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return ECurrentElementalStatusQuery::Failed;
    }

    if (!ValidateRawPointerArray(list, 1024, 4096)) {
        if (g_DebugDiagnosticsEnabled) {
            InterlockedIncrement(&g_StatusArrayValidationFailures);
            MarkRuntimeSnapshotDirty();
        }
        ShipLog(
            "[ElementalSystemExpanded] WARNING: status erosion could not validate "
            "the status array. Component=%p Data=%p Num=%d Max=%d\n",
            statusComponent,
            reinterpret_cast<void*>(list.Data),
            list.Num,
            list.Max);
        return ECurrentElementalStatusQuery::Failed;
    }

    if (g_DebugDiagnosticsEnabled)
        InterlockedIncrement(&g_StatusArrayValidationPasses);

    FCurrentElementalStatus candidates[7]{};
    size_t candidateCount = 0;

    const int32_t limit = (list.Num < 64) ? list.Num : 64;
    for (int32_t i = 0; i < limit; ++i) {
        void* status = nullptr;
        __try {
            status = *reinterpret_cast<void**>(
                list.Data + static_cast<uintptr_t>(i) * sizeof(void*));
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            return ECurrentElementalStatusQuery::Failed;
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

        if (!ValidateUObjectLikePointer(status, statusReadableSpan))
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
            !IsTrackedElementalStatusID(statusID) ||
            !std::isfinite(duration) ||
            !std::isfinite(timer) ||
            duration <= kErosionExpiryTick ||
            timer <= kErosionExpiryTick) {
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
        ClearElementalStatusProvenance(statusComponent);
        return ECurrentElementalStatusQuery::None;
    }

    // Native gameplay is expected to expose one active tracked elemental
    // status. If another mod/engine edge case leaves several, prefer the exact
    // instance whose source provenance we own; otherwise choose the greatest
    // remaining timer deterministically and emit only a developer trace.
    FElementalStatusProvenance provenance{};
    bool haveProvenance = false;
    AcquireSRWLockShared(&g_ElementalStatusProvenanceLock);
    const auto it = g_ElementalStatusProvenance.find(statusComponent);
    if (it != g_ElementalStatusProvenance.end()) {
        provenance = it->second;
        haveProvenance = true;
    }
    ReleaseSRWLockShared(&g_ElementalStatusProvenanceLock);

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
    return ECurrentElementalStatusQuery::Found;
}

static volatile LONG g_BurnDurationTimerBpValidated = 0;
static volatile LONG g_WetnessDurationTimerBpValidated = 0;

static volatile LONG* BlueprintDurationTimerValidationState(uint8_t statusID)
{
    if (statusID == ActiveIds::Status_Burn)
        return &g_BurnDurationTimerBpValidated;
    if (statusID == ActiveIds::Status_Wetness)
        return &g_WetnessDurationTimerBpValidated;
    return nullptr;
}

static bool NearlyEqualStatusLifetime(double lhs, double rhs)
{
    if (!std::isfinite(lhs) || !std::isfinite(rhs))
        return false;

    const double delta = std::fabs(lhs - rhs);
    const double scale = (std::max)(1.0, (std::max)(std::fabs(lhs), std::fabs(rhs)));

    // OnBegin copies the native float Duration into a Blueprint double, so on
    // a newly-created instance the values should normally match exactly.
    // The small mixed absolute/relative tolerance only covers float->double
    // representation and one-frame timing drift.
    return delta <= 0.050 || delta <= (scale * 0.005);
}

static bool RuntimeValidateBlueprintDurationTimer(
    void* status,
    uint8_t statusID,
    uintptr_t blueprintTimerOffset,
    double candidateTimer,
    float nativeDuration,
    float nativeTimer)
{
    volatile LONG* state =
        BlueprintDurationTimerValidationState(statusID);

    if (!state || blueprintTimerOffset == 0)
        return blueprintTimerOffset == 0;

    if (InterlockedCompareExchange(
            const_cast<LONG*>(state),
            0,
            0) == 1) {
        return true;
    }

    if (!std::isfinite(candidateTimer) ||
        candidateTimer <= 0.0 ||
        candidateTimer > 3600.0 ||
        !std::isfinite(nativeDuration) ||
        !std::isfinite(nativeTimer) ||
        nativeDuration <= 0.0f ||
        nativeTimer < 0.0f) {
        return false;
    }

    const bool matchesNative =
        NearlyEqualStatusLifetime(
            candidateTimer,
            static_cast<double>(nativeDuration)) ||
        NearlyEqualStatusLifetime(
            candidateTimer,
            static_cast<double>(nativeTimer));

    if (!matchesNative)
        return false;

    InterlockedExchange(
        const_cast<LONG*>(state),
        1);

    ShipLog(
        "[ElementalSystemExpanded] RUNTIME VALIDATED: StatusID=%u (%s) "
        "DurationTimer_BP Offset=0x%llX Candidate=%.6f "
        "NativeDuration=%.6f NativeTimer=%.6f Instance=%p.\n",
        static_cast<unsigned>(statusID),
        StatusIDName(statusID),
        static_cast<unsigned long long>(blueprintTimerOffset),
        candidateTimer,
        static_cast<double>(nativeDuration),
        static_cast<double>(nativeTimer),
        status);

    return true;
}

static bool SetStatusDuration(void* status, float duration)
{
    if (!status || !std::isfinite(duration) || duration <= 0.0f)
        return false;

    const uintptr_t addr = reinterpret_cast<uintptr_t>(status);

    uint8_t statusID = 0;
    uintptr_t blueprintTimerOffset = 0;

    __try {
        statusID = *reinterpret_cast<const uint8_t*>(
            addr + ActiveLayout::StatusBase_StatusID);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }

    if (statusID == ActiveIds::Status_Burn) {
        blueprintTimerOffset =
            Known104::Offset::StatusBurn_DurationTimerBP;
    }
    else if (statusID == ActiveIds::Status_Wetness) {
        blueprintTimerOffset =
            Known104::Offset::StatusWetness_DurationTimerBP;
    }

    const size_t requiredSpan =
        static_cast<size_t>((std::max)(
            ActiveLayout::StatusBase_DurationTimer + sizeof(float),
            blueprintTimerOffset != 0
                ? blueprintTimerOffset + sizeof(double)
                : ActiveLayout::StatusBase_DurationTimer + sizeof(float)));

    if (!ValidateUObjectLikePointer(status, requiredSpan))
        return false;

    __try {
        const float previousDuration =
            *reinterpret_cast<const float*>(
                addr + ActiveLayout::StatusBase_Duration);

        const float previousNativeTimer =
            *reinterpret_cast<const float*>(
                addr + ActiveLayout::StatusBase_DurationTimer);

        double previousBlueprintTimer = 0.0;

        if (blueprintTimerOffset != 0) {
            previousBlueprintTimer =
                *reinterpret_cast<const double*>(
                    addr + blueprintTimerOffset);

            if (!RuntimeValidateBlueprintDurationTimer(
                    status,
                    statusID,
                    blueprintTimerOffset,
                    previousBlueprintTimer,
                    previousDuration,
                    previousNativeTimer)) {

                ShipLog(
                    "[ElementalSystemExpanded] WARNING: semantic lifetime "
                    "rewrite refused for StatusID=%u (%s); reflected "
                    "DurationTimer_BP Offset=0x%llX failed runtime semantic "
                    "validation. Candidate=%.6f NativeDuration=%.6f "
                    "NativeTimer=%.6f.\n",
                    static_cast<unsigned>(statusID),
                    StatusIDName(statusID),
                    static_cast<unsigned long long>(blueprintTimerOffset),
                    previousBlueprintTimer,
                    static_cast<double>(previousDuration),
                    static_cast<double>(previousNativeTimer));
                return false;
            }
        }

        *reinterpret_cast<float*>(
            addr + ActiveLayout::StatusBase_Duration) = duration;
        *reinterpret_cast<float*>(
            addr + ActiveLayout::StatusBase_DurationTimer) = duration;

        if (blueprintTimerOffset != 0) {
            *reinterpret_cast<double*>(
                addr + blueprintTimerOffset) =
                static_cast<double>(duration);

            ModLog(
                "[ElementalSystemExpanded] STATUS BP TIMER SYNC: "
                "StatusID=%u (%s) Instance=%p Offset=0x%llX "
                "NativeDuration %.3f->%.3f NativeTimer %.3f->%.3f "
                "BPDurationTimer %.3f->%.3f.\n",
                static_cast<unsigned>(statusID),
                StatusIDName(statusID),
                status,
                static_cast<unsigned long long>(blueprintTimerOffset),
                static_cast<double>(previousDuration),
                static_cast<double>(duration),
                static_cast<double>(previousNativeTimer),
                static_cast<double>(duration),
                previousBlueprintTimer,
                static_cast<double>(duration));
        }

        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool SetStatusRemainingFromErosion(
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
        : kErosionExpiryTick;

    if (!SetStatusDuration(status, storedRemaining))
        return false;

    if (outStoredRemaining)
        *outStoredRemaining = storedRemaining;
    return true;
}


static EDragonReaction DragonReactionForStatus(uint8_t statusID)
{
    if (statusID == ActiveIds::Status_Wetness)
        return EDragonReaction::SteamBurst;
    if (statusID == ActiveIds::Status_Muddy)
        return EDragonReaction::LavaBurst;
    if (statusID == ActiveIds::Status_IvyCling)
        return EDragonReaction::Wildfire;
    if (statusID == ActiveIds::Status_Burn)
        return EDragonReaction::Flashover;
    if (statusID == ActiveIds::Status_Freeze)
        return EDragonReaction::ThermalShock;
    if (statusID == ActiveIds::Status_Electrical)
        return EDragonReaction::ArcBurst;
    return EDragonReaction::None;
}

static const char* DragonReactionName(EDragonReaction reaction)
{
    switch (reaction) {
    case EDragonReaction::SteamBurst:   return "SteamBurst";
    case EDragonReaction::LavaBurst:    return "LavaBurst";
    case EDragonReaction::Wildfire:     return "Wildfire";
    case EDragonReaction::Flashover:    return "Flashover";
    case EDragonReaction::ThermalShock: return "ThermalShock";
    case EDragonReaction::ArcBurst:     return "ArcBurst";
    default:                                return "None";
    }
}

static bool WriteDamageEffectSlot(
    const void* damageInfo,
    int slot,
    uint8_t effect,
    int32_t value)
{
    if (!damageInfo || (slot != 1 && slot != 2))
        return false;

    const uintptr_t base = reinterpret_cast<uintptr_t>(damageInfo);
    const uintptr_t effectOffset =
        (slot == 1)
        ? ActiveLayout::DamageInfo_EffectType1
        : ActiveLayout::DamageInfo_EffectType2;
    const uintptr_t valueOffset =
        (slot == 1)
        ? ActiveLayout::DamageInfo_EffectValue1
        : ActiveLayout::DamageInfo_EffectValue2;

    if (effectOffset > (std::numeric_limits<uintptr_t>::max)() - base ||
        valueOffset > (std::numeric_limits<uintptr_t>::max)() - base ||
        !IsReadableAddress(base + effectOffset, sizeof(uint8_t)) ||
        !IsReadableAddress(base + valueOffset, sizeof(int32_t))) {
        return false;
    }

    __try {
        *reinterpret_cast<uint8_t*>(base + effectOffset) = effect;
        *reinterpret_cast<int32_t*>(base + valueOffset) = value;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool TryPrepareDragonReaction(
    void* damageReaction,
    const void* damageInfo,
    uint8_t attackElement,
    uint8_t* ioEffect1,
    int32_t* ioValue1,
    uint8_t* ioEffect2,
    int32_t* ioValue2)
{
    if (!damageReaction || !damageInfo ||
        !ioEffect1 || !ioValue1 || !ioEffect2 || !ioValue2) {
        return false;
    }

    if (attackElement != ActiveIds::Element_Dragon)
        return false;

    const bool burn1 = *ioEffect1 == ActiveIds::Effect_Burn;
    const bool burn2 = *ioEffect2 == ActiveIds::Effect_Burn;
    if (!burn1 && !burn2)
        return false;

    void* statusComponent = ResolveStatusComponent(damageReaction);
    if (!statusComponent)
        return false;

    FCurrentElementalStatus current{};
    const ECurrentElementalStatusQuery query =
        QueryCurrentElementalStatus(statusComponent, &current);

    if (query == ECurrentElementalStatusQuery::Failed) {
        ShipLog(
            "[ElementalSystemExpanded] DRAGON REACTION FAIL-CLOSED: "
            "DamageReaction=%p could not inspect current elemental status; "
            "Dragon Burn remains ordinary.\n",
            damageReaction);
        return false;
    }

    if (query != ECurrentElementalStatusQuery::Found)
        return false;

    const EDragonReaction reaction =
        DragonReactionForStatus(current.StatusID);
    if (reaction == EDragonReaction::None) {
        // Darkness/Light and all non-reactable statuses intentionally leave the
        // Dragon Burn aggregate untouched.
        return false;
    }

    // Capture and dispatch remain separate fail-closed boundaries. Reaction
    // consumption is enabled only after detached replay validation has returned
    // normally in the current process.
    if (!IsDragonBridgeCaptureReady()) {
        ShipLog(
            "[ElementalSystemExpanded] DRAGON REACTION FAIL-CLOSED: "
            "reaction=%s is eligible but BP_ESEBridge UObject/UFunction is not captured; "
            "Dragon Burn remains ordinary.\n",
            DragonReactionName(reaction));
        return false;
    }

    if (!CanDispatchDragonBridge()) {
        ShipLog(
            "[ElementalSystemExpanded] DRAGON REACTION FAIL-CLOSED: "
            "reaction=%s is eligible and BP_ESEBridge capture is ready, but the "
            "detached-frame dispatch validation is not ready/passed in this process; "
            "Dragon Burn remains ordinary.\n",
            DragonReactionName(reaction));
        return false;
    }

    void* defender = ResolveOwningPalCharacter(damageReaction);
    void* attacker = nullptr;
    float attackPower = 0.0f;
    uint8_t attackType = 0;
    if (!defender || !IsLikelyBridgeUObject(defender) ||
        !ReadDragonDamageBridgeFields(
            damageInfo,
            &attacker,
            &attackPower,
            &attackType)) {
        ShipLog(
            "[ElementalSystemExpanded] DRAGON REACTION FAIL-CLOSED: "
            "reaction=%s could not validate Attacker/Defender/BasePower/AttackType.\n",
            DragonReactionName(reaction));
        return false;
    }

    const int32_t rawDragonValue = burn1 ? *ioValue1 : *ioValue2;
    const uint8_t units = UnitsFromRawValue(rawDragonValue);
    const int32_t sequence = NextDragonBridgeSequence();

    const uint8_t oldEffect1 = *ioEffect1;
    const int32_t oldValue1 = *ioValue1;
    const uint8_t oldEffect2 = *ioEffect2;
    const int32_t oldValue2 = *ioValue2;

    // Suppress every Burn slot from the triggering Dragon hit. No marker is
    // left in FPalDamageInfo: spent slots are ordinary None/0.
    if (burn1 && !WriteDamageEffectSlot(damageInfo, 1, 0, 0))
        return false;

    if (burn2 && !WriteDamageEffectSlot(damageInfo, 2, 0, 0)) {
        if (burn1)
            WriteDamageEffectSlot(damageInfo, 1, oldEffect1, oldValue1);
        return false;
    }

    float storedRemaining = 0.0f;
    if (!SetStatusRemainingFromErosion(
            current.Instance,
            0.0f,
            &storedRemaining)) {
        if (burn1)
            WriteDamageEffectSlot(damageInfo, 1, oldEffect1, oldValue1);
        if (burn2)
            WriteDamageEffectSlot(damageInfo, 2, oldEffect2, oldValue2);
        return false;
    }

    // Arm the out-of-band provenance before dispatch. The delayed explosion's
    // positive native effect packet is recognized by attacker + element/effects
    // + power + attack type inside this short-lived ESE-owned context.
    RegisterDragonSpreadContext(
        attacker,
        reaction,
        units,
        attackPower,
        attackType,
        sequence);

    if (!DispatchDragonBridge(
            attacker,
            defender,
            reaction,
            units,
            attackPower,
            attackType,
            sequence)) {
        RemoveDragonSpreadContext(sequence);

        // Dispatch failed before a reliable reaction handoff. Restore the
        // consumed semantic lifetime and the original Dragon Burn payload.
        SetStatusDuration(current.Instance, current.Timer);
        if (burn1)
            WriteDamageEffectSlot(damageInfo, 1, oldEffect1, oldValue1);
        if (burn2)
            WriteDamageEffectSlot(damageInfo, 2, oldEffect2, oldValue2);

        ShipLog(
            "[ElementalSystemExpanded] DRAGON REACTION ROLLBACK: "
            "reaction=%s Seq=%d bridge dispatch failed; original status/Burn restored.\n",
            DragonReactionName(reaction),
            sequence);
        return false;
    }

    ClearElementalStatusProvenance(statusComponent);

    const uint8_t consumedEffect =
        ElementalStatusIDToEffect(current.StatusID);
    if (consumedEffect)
        ElementICDClear(damageReaction, consumedEffect);

    if (burn1) {
        *ioEffect1 = 0;
        *ioValue1 = 0;
    }
    if (burn2) {
        *ioEffect2 = 0;
        *ioValue2 = 0;
    }

    ModLog(
        "[ElementalSystemExpanded] DRAGON REACTION: "
        "DamageReaction=%p CurrentStatus=%u (%s) Reaction=%s "
        "Units=%u Radius=%.1f Seq=%d StoredConsumedTimer=%.3f "
        "NativeSpreadValue=%d.\n",
        damageReaction,
        static_cast<unsigned>(current.StatusID),
        StatusIDName(current.StatusID),
        DragonReactionName(reaction),
        static_cast<unsigned>(units),
        units >= 2 ? 800.0 : 500.0,
        sequence,
        storedRemaining,
        kDragonReactionNativeEffectValue);

    return true;
}

static bool IsWetStrongReactionPair(
    const FCurrentElementalStatus& current,
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

static constexpr double kReactionICDSeconds = 14.0;

enum class EStrongReaction : uint8_t {
    None = 0,
    Freeze = 1,
    Shock = 2
};

static const char* StrongReactionName(EStrongReaction reaction)
{
    switch (reaction) {
    case EStrongReaction::Freeze: return "Freeze";
    case EStrongReaction::Shock:  return "Shock";
    default:                           return "None";
    }
}

static EStrongReaction StatusToStrongReaction(uint8_t statusID)
{
    if (statusID == ActiveIds::Status_Freeze)
        return EStrongReaction::Freeze;
    if (statusID == 22)
        return EStrongReaction::Shock;
    return EStrongReaction::None;
}

static bool HasActiveStatus(void* statusComponent, uint8_t statusID)
{
    void* status = nullptr;
    return FindStatusInstance(
        statusComponent, statusID, &status, nullptr, nullptr);
}

struct FReactionICDKey {
    void* DamageReaction = nullptr;
    uint8_t Reaction = 0;

    bool operator==(const FReactionICDKey& other) const
    {
        return DamageReaction == other.DamageReaction &&
            Reaction == other.Reaction;
    }
};

struct FReactionICDKeyHash {
    size_t operator()(const FReactionICDKey& key) const noexcept
    {
        const size_t p = std::hash<uintptr_t>{}(
            reinterpret_cast<uintptr_t>(key.DamageReaction));
        const size_t r = std::hash<unsigned>{}(key.Reaction);
        return p ^ (r + static_cast<size_t>(0x9E3779B9u) +
            (p << 6) + (p >> 2));
    }
};

static std::unordered_map<
    FReactionICDKey,
    double,
    FReactionICDKeyHash> g_ReactionLastSuccess;

static SRWLOCK g_ReactionICDLock = SRWLOCK_INIT;

static bool ReactionICDReady(
    void* damageReaction,
    EStrongReaction reaction,
    double* outElapsed)
{
    if (outElapsed)
        *outElapsed = -1.0;

    if (!damageReaction || reaction == EStrongReaction::None)
        return false;

    const FReactionICDKey key{
        damageReaction,
        static_cast<uint8_t>(reaction)
    };

    const double now = GetMonotonicSeconds();

    AcquireSRWLockShared(&g_ReactionICDLock);
    const auto it = g_ReactionLastSuccess.find(key);
    const bool found = (it != g_ReactionLastSuccess.end());
    const double elapsed = found ? (now - it->second) : -1.0;
    ReleaseSRWLockShared(&g_ReactionICDLock);

    if (outElapsed)
        *outElapsed = elapsed;

    return !found || elapsed >= kReactionICDSeconds;
}

static void ReactionICDRecordSuccess(
    void* damageReaction,
    EStrongReaction reaction)
{
    if (!damageReaction || reaction == EStrongReaction::None)
        return;

    const FReactionICDKey key{
        damageReaction,
        static_cast<uint8_t>(reaction)
    };

    AcquireSRWLockExclusive(&g_ReactionICDLock);
    g_ReactionLastSuccess[key] = GetMonotonicSeconds();
    ReleaseSRWLockExclusive(&g_ReactionICDLock);
}

struct FReactionTLSContext {
    bool Active = false;
    uint8_t StatusID = 0;
    EStrongReaction Reaction = EStrongReaction::None;
    void* DamageReaction = nullptr;
    void* StatusComponent = nullptr;
    bool HadWetness = false;
    bool ICDReady = false;
    bool AllowStrongControl = false;
    bool ActionGateSeen = false;
    bool ReactionRecorded = false;
};

static thread_local FReactionTLSContext g_ReactionTLS{};

struct FReactionTLSGuard {
    FReactionTLSContext Previous{};

    FReactionTLSGuard(
        void* damageReaction,
        void* statusComponent,
        uint8_t statusID)
    {
        Previous = g_ReactionTLS;
        g_ReactionTLS = {};

        const EStrongReaction reaction =
            StatusToStrongReaction(statusID);

        if (reaction == EStrongReaction::None)
            return;

        const bool wet = HasActiveStatus(statusComponent, ActiveIds::Status_Wetness);
        double elapsed = -1.0;
        const bool icdReady =
            wet && ReactionICDReady(
                damageReaction, reaction, &elapsed);

        g_ReactionTLS.Active = true;
        g_ReactionTLS.StatusID = statusID;
        g_ReactionTLS.Reaction = reaction;
        g_ReactionTLS.DamageReaction = damageReaction;
        g_ReactionTLS.StatusComponent = statusComponent;
        g_ReactionTLS.HadWetness = wet;
        g_ReactionTLS.ICDReady = icdReady;
        g_ReactionTLS.AllowStrongControl = wet && icdReady;

        ModLog(
            "[ElementalSystemExpanded] REACTION PREP: DamageReaction=%p "
            "StatusComponent=%p StatusID=%u Reaction=%s Wet=%s "
            "ICD=%s Elapsed=%.3f Control=%s\n",
            damageReaction,
            statusComponent,
            static_cast<unsigned>(statusID),
            StrongReactionName(reaction),
            wet ? "YES" : "NO",
            wet ? (icdReady ? "READY" : "ACTIVE") : "N/A",
            elapsed,
            g_ReactionTLS.AllowStrongControl ? "ALLOW" : "BLOCK");
    }

    ~FReactionTLSGuard()
    {
        g_ReactionTLS = Previous;
    }
};

static void RecordAllowedReactionIfNeeded(const char* gateName)
{
    if (!g_ReactionTLS.Active ||
        !g_ReactionTLS.AllowStrongControl ||
        g_ReactionTLS.ReactionRecorded) {
        return;
    }

    ReactionICDRecordSuccess(
        g_ReactionTLS.DamageReaction,
        g_ReactionTLS.Reaction);

    g_ReactionTLS.ReactionRecorded = true;

    ModLog(
        "[ElementalSystemExpanded] REACTION TRIGGER: DamageReaction=%p "
        "Reaction=%s Gate=%s ICD=%.1fs\n",
        g_ReactionTLS.DamageReaction,
        StrongReactionName(g_ReactionTLS.Reaction),
        gateName ? gateName : "Unknown",
        kReactionICDSeconds);
}

// Native reaction/action signatures inferred directly from the verified
// wrapper register setup in memoryViewDump7/8.
using NativeSetActionClassParameter_t =
void* (__fastcall*)(
    void* AIActionComponent,
    void* NewActionClass,
    const void* DynamicParameter);

using NativePlayAction_t =
void* (__fastcall*)(
    void* ActionComponent,
    void* ActionTarget,
    void* ActionClass);

using NativeSetNamedBool_t =
void(__fastcall*)(
    void* MovementComponent,
    uint64_t RawFName,
    bool Enabled);

using NativeSetNamedFloat_t =
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
using NativeSetShooterLayeredBool_t =
void(__fastcall*)(
    void* ShooterComponent,
    uint8_t Layer,
    uint64_t RawFName,
    bool Disabled);

// Character::StopAnimMontage wrapper dispatches:
//   RCX = Character
//   RDX = UAnimMontage*
//   call [vtable + 0x888]
using NativeSetComponentTickEnabledVirtual_t =
void(__fastcall*)(
    void* ActorComponent,
    bool Enabled);

using NativeStopAnimMontageVirtual_t =
void(__fastcall*)(
    void* Character,
    void* Montage);

// PalStatusBase::TickStatus virtual ABI from memoryViewDump9:
//   RCX = Status
//   XMM1 = DeltaTime
using NativeStatusTickVirtual_t =
void(__fastcall*)(
    void* Status,
    float DeltaTime);

// Native AddVisualEffect / AddVisualEffect_Local ABI derived from their
// reflected wrappers:
//   RCX = UPalVisualEffectComponent*
//   EDX = EPalVisualEffectID
//   R8  = const FPalVisualEffectDynamicParameter*
//   RAX = UPalVisualEffectBase*
using NativeAddVisualEffect_t =
void* (__fastcall*)(
    void* VisualEffectComponent,
    uint8_t VisualEffectID,
    const void* DynamicParameter);

using NativeRemoveVisualEffectLocal_t =
void(__fastcall*)(
    void* VisualEffectComponent,
    uint8_t VisualEffectID);

static NativeSetActionClassParameter_t
Original_SetActionClassParameter = nullptr;
static NativePlayAction_t
Original_PlayAction = nullptr;
static NativeSetNamedBool_t
Original_SetJumpDisableFlag = nullptr;
static NativeSetNamedBool_t
Original_SetStepDisableFlag = nullptr;
static NativeSetNamedBool_t
Original_SetMoveDisableFlag = nullptr;
static NativeSetNamedFloat_t
Original_SetWalkSpeedMultiplier = nullptr;
static NativeSetNamedFloat_t
Original_SetYawRotatorMultiplier = nullptr;

// Stage 6.7.4.29: exact reflected exec-wrapper observation. These wrappers are
// resolved by exact registration name and are never called by ESE; they are
// hooked observation-only so we can compare Kismet/ProcessEvent entry with the
// separately resolved native implementation detours.
using NativeReflectedExecWrapper_t =
void(__fastcall*)(void* Context, void* Stack, void* Result);

static NativeReflectedExecWrapper_t
Original_Exec_SetJumpDisableFlag = nullptr;
static NativeReflectedExecWrapper_t
Original_Exec_SetStepDisableFlag = nullptr;
static NativeReflectedExecWrapper_t
Original_Exec_SetMoveDisableFlag = nullptr;

static uintptr_t g_ExactWrapper_SetJumpDisableFlag = 0;
static uintptr_t g_ExactWrapper_SetStepDisableFlag = 0;
static uintptr_t g_ExactWrapper_SetMoveDisableFlag = 0;

static volatile LONG g_ExecHit_SetJumpDisableFlag = 0;
static volatile LONG g_ExecHit_SetStepDisableFlag = 0;
static volatile LONG g_ExecHit_SetMoveDisableFlag = 0;

static NativeSetShooterLayeredBool_t
Original_SetDisableAimFlag_Layered = nullptr;
static NativeSetShooterLayeredBool_t
Original_SetDisableShootFlag_Layered = nullptr;
static NativeSetShooterLayeredBool_t
Original_SetDisableChangeWeaponFlag_Layered = nullptr;

// Dynamically resolved exact virtuals for the current validated runtime.
static NativeSetComponentTickEnabledVirtual_t
Original_SetComponentTickEnabledVirtual = nullptr;
static NativeStopAnimMontageVirtual_t
Original_StopAnimMontageVirtual = nullptr;
static NativeStatusTickVirtual_t
Original_FreezeTickVirtual = nullptr;

static NativeAddVisualEffect_t
Original_AddVisualEffect = nullptr;
static NativeAddVisualEffect_t
Original_AddVisualEffect_Local = nullptr;
static NativeRemoveVisualEffectLocal_t
Native_RemoveVisualEffect_Local = nullptr;

static void* g_SetComponentTickEnabledVirtualTarget = nullptr;
static void* g_StopAnimMontageVirtualTarget = nullptr;
static void* g_FreezeTickVirtualTarget = nullptr;

enum class EBlindnessSource : uint8_t {
    Unknown = 0,
    Dark = 1,
    Light = 2
};

static thread_local EBlindnessSource g_BlindnessSourceTLS =
EBlindnessSource::Unknown;

// Dedicated multiplayer transport scope for the shared Darkness carrier.
// This is deliberately separate from presentation TLS: the parameter hook must
// stamp only the exact StatusID 25 AddStatus invocation whose authoritative
// source element was already resolved by ESE.
struct FBlindnessTransportTLS {
    bool Active = false;
    void* StatusComponent = nullptr;
    EBlindnessSource Source = EBlindnessSource::Unknown;
};

static thread_local FBlindnessTransportTLS g_BlindnessTransportTLS{};

struct FBlindnessTransportScope {
    FBlindnessTransportTLS Previous{};

    FBlindnessTransportScope(
        void* statusComponent,
        EBlindnessSource source)
        : Previous(g_BlindnessTransportTLS)
    {
        if (statusComponent &&
            (source == EBlindnessSource::Light ||
             source == EBlindnessSource::Dark)) {
            g_BlindnessTransportTLS.Active = true;
            g_BlindnessTransportTLS.StatusComponent = statusComponent;
            g_BlindnessTransportTLS.Source = source;
        }
        else {
            g_BlindnessTransportTLS = {};
        }
    }

    ~FBlindnessTransportScope()
    {
        g_BlindnessTransportTLS = Previous;
    }
};

static const char* BlindnessSourceName(EBlindnessSource source)
{
    switch (source) {
    case EBlindnessSource::Dark: return "DARK";
    case EBlindnessSource::Light: return "LIGHT";
    default: return "UNKNOWN";
    }
}

struct FBlindnessSourceScope {
    EBlindnessSource Previous = EBlindnessSource::Unknown;

    explicit FBlindnessSourceScope(EBlindnessSource source)
        : Previous(g_BlindnessSourceTLS)
    {
        g_BlindnessSourceTLS = source;
    }

    ~FBlindnessSourceScope()
    {
        g_BlindnessSourceTLS = Previous;
    }
};

static SRWLOCK g_FreezeDynamicHookLock = SRWLOCK_INIT;

static bool IsBlockedReactionScope()
{
    return g_ReactionTLS.Active &&
        !g_ReactionTLS.AllowStrongControl &&
        (g_ReactionTLS.StatusID == ActiveIds::Status_Freeze ||
            g_ReactionTLS.StatusID == ActiveIds::Status_Electrical);
}

static void* __fastcall Detour_SetActionClassParameter(
    void* aiActionComponent,
    void* newActionClass,
    const void* dynamicParameter)
{
    if (g_ReactionTLS.Active) {
        g_ReactionTLS.ActionGateSeen = true;

        if (!g_ReactionTLS.AllowStrongControl) {
            ModLog(
                "[ElementalSystemExpanded] REACTION BLOCK: %s "
                "SetActionClassParameter Component=%p Class=%p\n",
                StrongReactionName(g_ReactionTLS.Reaction),
                aiActionComponent,
                newActionClass);
            return nullptr;
        }

        RecordAllowedReactionIfNeeded("SetActionClassParameter");
    }

    return Original_SetActionClassParameter
        ? Original_SetActionClassParameter(
            aiActionComponent, newActionClass, dynamicParameter)
        : nullptr;
}

static void* __fastcall Detour_PlayAction(
    void* actionComponent,
    void* actionTarget,
    void* actionClass)
{
    if (g_ReactionTLS.Active) {
        if (!g_ReactionTLS.AllowStrongControl) {
            ModLog(
                "[ElementalSystemExpanded] REACTION BLOCK: %s "
                "PlayAction Component=%p Target=%p Class=%p\n",
                StrongReactionName(g_ReactionTLS.Reaction),
                actionComponent,
                actionTarget,
                actionClass);
            return nullptr;
        }

        // Fallback in case a reaction path reaches PlayAction without first
        // traversing SetActionClassParameter.
        RecordAllowedReactionIfNeeded("PlayAction");
    }

    return Original_PlayAction
        ? Original_PlayAction(
            actionComponent, actionTarget, actionClass)
        : nullptr;
}

static void LogExactMovementExecHit(
    const char* name,
    volatile LONG* counter,
    void* context)
{
    if (!counter)
        return;

    if (!g_ReactionTLS.Active ||
        g_ReactionTLS.StatusID != ActiveIds::Status_Freeze) {
        return;
    }

    const LONG hit = InterlockedIncrement(counter);
    if (hit <= 16) {
        ModLog(
            "[ElementalSystemExpanded] [ESE-RESOLVE29] EXEC WRAPPER HIT: "
            "%s Context=%p Hit=%ld Wet=%s Control=%s\n",
            name ? name : "<null>",
            context,
            hit,
            g_ReactionTLS.HadWetness ? "YES" : "NO",
            g_ReactionTLS.AllowStrongControl ? "ALLOW" : "BLOCK");
    }
}

static void __fastcall Detour_Exec_SetJumpDisableFlag(
    void* context, void* stack, void* result)
{
    LogExactMovementExecHit(
        "SetJumpDisableFlag",
        &g_ExecHit_SetJumpDisableFlag,
        context);
    if (Original_Exec_SetJumpDisableFlag)
        Original_Exec_SetJumpDisableFlag(context, stack, result);
}

static void __fastcall Detour_Exec_SetStepDisableFlag(
    void* context, void* stack, void* result)
{
    LogExactMovementExecHit(
        "SetStepDisableFlag",
        &g_ExecHit_SetStepDisableFlag,
        context);
    if (Original_Exec_SetStepDisableFlag)
        Original_Exec_SetStepDisableFlag(context, stack, result);
}

static void __fastcall Detour_Exec_SetMoveDisableFlag(
    void* context, void* stack, void* result)
{
    LogExactMovementExecHit(
        "SetMoveDisableFlag",
        &g_ExecHit_SetMoveDisableFlag,
        context);
    if (Original_Exec_SetMoveDisableFlag)
        Original_Exec_SetMoveDisableFlag(context, stack, result);
}

static void __fastcall Detour_SetJumpDisableFlag(
    void* movementComponent,
    uint64_t rawFName,
    bool disabled)
{
    if (IsBlockedReactionScope() && disabled) {
        ModLog(
            "[ElementalSystemExpanded] REACTION BLOCK: %s "
            "SetJumpDisableFlag(true) Component=%p\n",
            StrongReactionName(g_ReactionTLS.Reaction),
            movementComponent);
        return;
    }

    if (Original_SetJumpDisableFlag)
        Original_SetJumpDisableFlag(
            movementComponent, rawFName, disabled);
}

static void __fastcall Detour_SetStepDisableFlag(
    void* movementComponent,
    uint64_t rawFName,
    bool disabled)
{
    if (IsBlockedReactionScope() && disabled) {
        ModLog(
            "[ElementalSystemExpanded] REACTION BLOCK: %s "
            "SetStepDisableFlag(true) Component=%p\n",
            StrongReactionName(g_ReactionTLS.Reaction),
            movementComponent);
        return;
    }

    if (Original_SetStepDisableFlag)
        Original_SetStepDisableFlag(
            movementComponent, rawFName, disabled);
}

static void __fastcall Detour_SetMoveDisableFlag(
    void* movementComponent,
    uint64_t rawFName,
    bool disabled)
{
    if (IsBlockedReactionScope() && disabled) {
        ModLog(
            "[ElementalSystemExpanded] REACTION BLOCK: %s "
            "SetMoveDisableFlag(true) Component=%p\n",
            StrongReactionName(g_ReactionTLS.Reaction),
            movementComponent);
        return;
    }

    if (Original_SetMoveDisableFlag)
        Original_SetMoveDisableFlag(
            movementComponent, rawFName, disabled);
}

static bool ShouldBlockWalkMultiplier(float value)
{
    if (!IsBlockedReactionScope())
        return false;

    if (g_ReactionTLS.StatusID == ActiveIds::Status_Freeze) {
        // Freeze SetFlag true branch: 0.6.
        return value >= 0.55f && value <= 0.65f;
    }

    if (g_ReactionTLS.StatusID == ActiveIds::Status_Electrical) {
        // Electrical reaction runtime observation: 0.0.
        return value >= -0.01f && value <= 0.01f;
    }

    return false;
}

static bool ShouldBlockYawMultiplier(float value)
{
    if (!IsBlockedReactionScope())
        return false;

    // Both strong control reactions use a zero yaw multiplier.
    return value >= -0.01f && value <= 0.01f;
}

static void __fastcall Detour_SetWalkSpeedMultiplier(
    void* movementComponent,
    uint64_t rawFName,
    float value)
{
    if (ShouldBlockWalkMultiplier(value)) {
        ModLog(
            "[ElementalSystemExpanded] REACTION BLOCK: %s "
            "SetWalkSpeedMultiplier(%.3f) Component=%p\n",
            StrongReactionName(g_ReactionTLS.Reaction),
            value,
            movementComponent);
        return;
    }

    if (Original_SetWalkSpeedMultiplier)
        Original_SetWalkSpeedMultiplier(
            movementComponent, rawFName, value);
}

static void __fastcall Detour_SetYawRotatorMultiplier(
    void* movementComponent,
    uint64_t rawFName,
    float value)
{
    if (ShouldBlockYawMultiplier(value)) {
        ModLog(
            "[ElementalSystemExpanded] REACTION BLOCK: %s "
            "SetYawRotatorMultiplier(%.3f) Component=%p\n",
            StrongReactionName(g_ReactionTLS.Reaction),
            value,
            movementComponent);
        return;
    }

    if (Original_SetYawRotatorMultiplier)
        Original_SetYawRotatorMultiplier(
            movementComponent, rawFName, value);
}


static bool ShouldBlockFreezeShooterDisable(bool disabled)
{
    return disabled &&
        IsBlockedReactionScope() &&
        g_ReactionTLS.StatusID == ActiveIds::Status_Freeze;
}

static void __fastcall Detour_SetDisableAimFlag_Layered(
    void* shooterComponent,
    uint8_t layer,
    uint64_t rawFName,
    bool disabled)
{
    if (ShouldBlockFreezeShooterDisable(disabled)) {
        ModLog(
            "[ElementalSystemExpanded] REACTION BLOCK: Freeze "
            "SetDisableAimFlag_Layered(true) Component=%p Layer=%u\n",
            shooterComponent,
            static_cast<unsigned>(layer));
        return;
    }

    if (Original_SetDisableAimFlag_Layered)
        Original_SetDisableAimFlag_Layered(
            shooterComponent, layer, rawFName, disabled);
}

static void __fastcall Detour_SetDisableShootFlag_Layered(
    void* shooterComponent,
    uint8_t layer,
    uint64_t rawFName,
    bool disabled)
{
    if (ShouldBlockFreezeShooterDisable(disabled)) {
        ModLog(
            "[ElementalSystemExpanded] REACTION BLOCK: Freeze "
            "SetDisableShootFlag_Layered(true) Component=%p Layer=%u\n",
            shooterComponent,
            static_cast<unsigned>(layer));
        return;
    }

    if (Original_SetDisableShootFlag_Layered)
        Original_SetDisableShootFlag_Layered(
            shooterComponent, layer, rawFName, disabled);
}

static void __fastcall Detour_SetDisableChangeWeaponFlag_Layered(
    void* shooterComponent,
    uint8_t layer,
    uint64_t rawFName,
    bool disabled)
{
    if (ShouldBlockFreezeShooterDisable(disabled)) {
        ModLog(
            "[ElementalSystemExpanded] REACTION BLOCK: Freeze "
            "SetDisableChangeWeaponFlag_Layered(true) Component=%p Layer=%u\n",
            shooterComponent,
            static_cast<unsigned>(layer));
        return;
    }

    if (Original_SetDisableChangeWeaponFlag_Layered)
        Original_SetDisableChangeWeaponFlag_Layered(
            shooterComponent, layer, rawFName, disabled);
}


struct FHookTransactionSpec {
    const char* Name = nullptr;
    uintptr_t Target = 0;
    void* Detour = nullptr;
    void** Original = nullptr;
};

static void RollbackCreatedHooks(
    const FHookTransactionSpec* specs,
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

static bool InstallHookTransaction(
    const char* capabilityName,
    const FHookTransactionSpec* specs,
    size_t count)
{
    if (!specs || !count)
        return false;

    for (size_t i = 0; i < count; ++i) {
        if (!specs[i].Target || !IsExecutableAddress(specs[i].Target)) {
            ShipLog("[ElementalSystemExpanded] ERROR: transaction %s has invalid target for %s.\n",
                capabilityName, specs[i].Name ? specs[i].Name : "unnamed");
            return false;
        }
    }

    // Distinct reaction gates must never alias the same native function.
    // Detect this before MinHook so a stale/ambiguous resolver result produces
    // an explicit fail-closed diagnostic instead of an opaque MH_CreateHook
    // failure on the second detour.
    for (size_t i = 0; i < count; ++i) {
        for (size_t j = i + 1; j < count; ++j) {
            if (specs[i].Target != specs[j].Target)
                continue;
            ShipLog(
                "[ElementalSystemExpanded] ERROR: transaction %s target alias: %s and %s both resolved to RVA +0x%llX.\n",
                capabilityName,
                specs[i].Name ? specs[i].Name : "unnamed",
                specs[j].Name ? specs[j].Name : "unnamed",
                static_cast<unsigned long long>(specs[i].Target - g_ModuleBase));
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
            ShipLog("[ElementalSystemExpanded] ERROR: transaction %s create failed for %s (%d). Rolling back.\n",
                capabilityName,
                specs[created].Name ? specs[created].Name : "unnamed",
                static_cast<int>(status));
            RollbackCreatedHooks(specs, created);
            return false;
        }
    }

    size_t queued = 0;
    for (; queued < count; ++queued) {
        const MH_STATUS status = MH_QueueEnableHook(
            reinterpret_cast<LPVOID>(specs[queued].Target));
        if (status != MH_OK) {
            ShipLog("[ElementalSystemExpanded] ERROR: transaction %s queue-enable failed for %s (%d). Rolling back.\n",
                capabilityName,
                specs[queued].Name ? specs[queued].Name : "unnamed",
                static_cast<int>(status));
            for (size_t j = 0; j < queued; ++j)
                MH_QueueDisableHook(reinterpret_cast<LPVOID>(specs[j].Target));
            MH_ApplyQueued();
            RollbackCreatedHooks(specs, count);
            return false;
        }
    }

    const MH_STATUS apply = MH_ApplyQueued();
    if (apply != MH_OK) {
        ShipLog("[ElementalSystemExpanded] ERROR: transaction %s apply failed (%d). Rolling back.\n",
            capabilityName, static_cast<int>(apply));
        for (size_t i = 0; i < count; ++i)
            MH_QueueDisableHook(reinterpret_cast<LPVOID>(specs[i].Target));
        MH_ApplyQueued();
        RollbackCreatedHooks(specs, count);
        return false;
    }

    for (size_t i = 0; i < count; ++i) {
        ModLog("[ElementalSystemExpanded] SUCCESS: Transaction %s hooked %s at RVA +0x%llX.\n",
            capabilityName,
            specs[i].Name ? specs[i].Name : "unnamed",
            static_cast<unsigned long long>(specs[i].Target - g_ModuleBase));
    }
    return true;
}


struct FDynamicHookTransactionSpec {
    const char* Name = nullptr;
    void* Target = nullptr;
    void* Detour = nullptr;
    void** Original = nullptr;
    void** StoredTarget = nullptr;
    uintptr_t Slot = 0;
};

static bool EnsureDynamicHookTransaction(
    const char* capabilityName,
    FDynamicHookTransactionSpec* specs,
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
                ShipLog(
                    "[ElementalSystemExpanded] ERROR: dynamic transaction %s target changed for %s Existing=%p New=%p\n",
                    capabilityName,
                    specs[i].Name ? specs[i].Name : "unnamed",
                    *specs[i].StoredTarget,
                    specs[i].Target);
                return false;
            }
            continue;
        }

        if (!IsExecutableAddress(reinterpret_cast<uintptr_t>(specs[i].Target))) {
            ShipLog(
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
            ShipLog(
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
            ShipLog(
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
        ShipLog(
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

// ---------------------------------------------------------
// Exact reflected-wrapper virtual-slot recovery
//
// The short wrapper tails used by the generic resolver are intentionally broad
// enough to survive ordinary compiler drift, but PalServer contains many
// unrelated wrappers with that same tail shape. For the three dry-Freeze
// virtuals we also have complete reflected-wrapper bodies captured from the
// validated client build. Resolve those full wrappers semantically:
//   * wildcard rel32 helper-call displacements;
//   * wildcard the vtable-slot immediate itself;
//   * require all surviving full-wrapper matches to agree on the decoded slot.
//
// The historical slot is never used as an acceptance criterion.
static bool ResolveVirtualSlotFromFullWrapper(
    const char* name,
    uintptr_t moduleBase,
    const uint8_t* wrapperPattern,
    const char* wrapperMask,
    size_t slotImmediateOffset,
    uintptr_t* outSlot)
{
    if (!name || !moduleBase || !wrapperPattern || !wrapperMask || !outSlot ||
        slotImmediateOffset < 2) {
        return false;
    }

    auto readSlot = [&](uintptr_t wrapperStart, uintptr_t* slot) -> bool {
        if (!wrapperStart || !slot ||
            !MatchMaskedBytes(wrapperStart, wrapperPattern, wrapperMask)) {
            return false;
        }

        __try {
            const uint32_t value =
                *reinterpret_cast<const uint32_t*>(
                    wrapperStart + slotImmediateOffset);

            if (value < 0x100 ||
                value > 0x2000 ||
                (value % sizeof(void*)) != 0) {
                return false;
            }

            *slot = static_cast<uintptr_t>(value);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    };

    const size_t dispatchOffset = slotImmediateOffset - 2;

    // Exact fingerprint cache remains useful, but every cached wrapper+slot is
    // revalidated against the current full wrapper body before use.
    const uintptr_t cachedDispatchRva = CachedProfileValue(
        g_BuildProfileCache.VDispatches, name);
    const uintptr_t cachedSlot = CachedProfileValue(
        g_BuildProfileCache.VSlots, name);

    if (cachedDispatchRva && cachedSlot &&
        cachedDispatchRva >= dispatchOffset) {

        const uintptr_t cachedWrapperStart =
            moduleBase + cachedDispatchRva - dispatchOffset;

        uintptr_t verifiedSlot = 0;
        if (readSlot(cachedWrapperStart, &verifiedSlot) &&
            verifiedSlot == cachedSlot) {

            *outSlot = verifiedSlot;

            RecordResolver(
                name,
                "fingerprint-profile+full-wrapper-vslot",
                EResolveConfidence::ProfileStatic,
                moduleBase + cachedDispatchRva,
                "cached full wrapper + decoded slot revalidated against exact build fingerprint");

            CacheResolvedVSlot(
                name,
                verifiedSlot,
                moduleBase + cachedDispatchRva);

            return true;
        }
    }

    const auto matches = FindPatternInExecutableSections(
        moduleBase,
        wrapperPattern,
        wrapperMask,
        16);

    uintptr_t consensusSlot = 0;
    uintptr_t selectedWrapper = 0;
    size_t validCount = 0;
    size_t conflicts = 0;

    char detail[512]{};
    size_t used = 0;

    for (uintptr_t candidate : matches) {
        uintptr_t candidateSlot = 0;
        if (!readSlot(candidate, &candidateSlot))
            continue;

        ++validCount;

        if (!consensusSlot) {
            consensusSlot = candidateSlot;
            selectedWrapper = candidate;
        }
        else if (candidateSlot != consensusSlot) {
            ++conflicts;
        }

        if (used < sizeof(detail) - 48) {
            const uintptr_t rva =
                candidate >= moduleBase ? candidate - moduleBase : 0;

            const int n = snprintf(
                detail + used,
                sizeof(detail) - used,
                "%s+0x%llX/slot+0x%llX",
                used ? ", " : "",
                static_cast<unsigned long long>(rva),
                static_cast<unsigned long long>(candidateSlot));

            if (n > 0) {
                used += (std::min)(
                    static_cast<size_t>(n),
                    sizeof(detail) - used - 1);
            }
        }
    }

    if (!validCount || conflicts != 0 || !consensusSlot || !selectedWrapper) {
        char failure[640]{};
        snprintf(
            failure,
            sizeof(failure),
            "full-wrapper matches=%llu valid=%llu slot-conflicts=%llu candidates=[%s]",
            static_cast<unsigned long long>(matches.size()),
            static_cast<unsigned long long>(validCount),
            static_cast<unsigned long long>(conflicts),
            detail);

        RecordResolver(
            name,
            "full-reflected-wrapper-vslot",
            EResolveConfidence::Failed,
            0,
            failure);

        return false;
    }

    *outSlot = consensusSlot;
    const uintptr_t dispatch = selectedWrapper + dispatchOffset;

    char success[640]{};
    snprintf(
        success,
        sizeof(success),
        "full-wrapper matches=%llu valid=%llu consensus-slot=+0x%llX candidates=[%s]",
        static_cast<unsigned long long>(matches.size()),
        static_cast<unsigned long long>(validCount),
        static_cast<unsigned long long>(consensusSlot),
        detail);

    RecordResolver(
        name,
        validCount == 1
            ? "full-reflected-wrapper-unique-vslot"
            : "full-reflected-wrapper-consensus-vslot",
        EResolveConfidence::Strong,
        dispatch,
        success);

    CacheResolvedVSlot(
        name,
        consensusSlot,
        dispatch);

    return true;
}


// ---------------------------------------------------------
// Reflected UFunction identity -> exec wrapper -> virtual slot
//
// Unreal native UFunctions are registered through FNameNativePtrPair-like
// tables that contain an ANSI function-name pointer immediately followed by
// the exec-wrapper pointer.  This gives us the missing semantic identity that
// a raw full-wrapper AOB cannot provide: many generated wrappers have the same
// parameter-marshalling body, but only the registration entry for the requested
// reflected name points at that exact exec wrapper.
//
// Resolution is deliberately conservative:
//   * exact fingerprint cache is revalidated against the full wrapper body;
//   * otherwise find exact NUL-terminated reflected-name strings in readable
//     image sections;
//   * find in-image pointer pairs { name-string, executable-wrapper };
//   * require the wrapper to match the complete captured body;
//   * decode the final FF 90 [vtable slot] immediate;
//   * all unique matching wrappers must agree on the same slot.
//
// The historical vtable slot is never used to select or accept a candidate.
// If native-registration layout changes or same-name wrappers disagree, the
// resolver fails closed and the older full-wrapper consensus resolver remains
// available as a fallback.
struct FReadableImageSectionRange {
    uintptr_t Rva = 0;
    uintptr_t Size = 0;
    DWORD Characteristics = 0;
};

static bool SnapshotReadableImageSections(
    uintptr_t moduleBase,
    FReadableImageSectionRange* outSections,
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
            if (!(sections[i].Characteristics & IMAGE_SCN_MEM_READ))
                continue;

            uintptr_t rva = sections[i].VirtualAddress;
            uintptr_t size = sections[i].Misc.VirtualSize;
            if (!size)
                size = sections[i].SizeOfRawData;

            if (!size || rva >= imageSize || size > imageSize - rva)
                continue;

            outSections[count++] = { rva, size, sections[i].Characteristics };
        }

        *outCount = count;
        return count != 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        *outCount = 0;
        return false;
    }
}

static bool ReadPointerPairSEH(
    uintptr_t address,
    uintptr_t* first,
    uintptr_t* second)
{
    if (!address || !first || !second)
        return false;

    __try {
        *first = *reinterpret_cast<const uintptr_t*>(address);
        *second = *reinterpret_cast<const uintptr_t*>(
            address + sizeof(uintptr_t));
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        *first = 0;
        *second = 0;
        return false;
    }
}


struct FRegistrationPairHit {
    uintptr_t PairAddress = 0;
    uintptr_t Wrapper = 0;
};

// Leaf SEH scanners: one protected region per PE section, not one VirtualQuery
// per 8-byte candidate. This keeps the registration resolver bounded enough to
// run during startup while retaining fail-closed memory safety.
static size_t FindExactAnsiStringsInRangeSEH(
    uintptr_t begin,
    uintptr_t size,
    const char* needle,
    size_t needleLen,
    uintptr_t* outHits,
    size_t capacity)
{
    if (!begin || !size || !needle || !needleLen || !outHits || !capacity ||
        size < needleLen + 1) {
        return 0;
    }

    size_t count = 0;
    __try {
        const uintptr_t last = begin + size - (needleLen + 1);
        for (uintptr_t at = begin; at <= last && count < capacity; ++at) {
            if (*reinterpret_cast<const char*>(at) != needle[0])
                continue;
            if (memcmp(reinterpret_cast<const void*>(at), needle, needleLen) != 0)
                continue;
            if (*reinterpret_cast<const char*>(at + needleLen) != '\0')
                continue;
            outHits[count++] = at;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return count;
    }
    return count;
}

static size_t FindRegistrationPairsInRangeSEH(
    uintptr_t begin,
    uintptr_t size,
    uintptr_t nameAddress,
    FRegistrationPairHit* outHits,
    size_t capacity)
{
    if (!begin || !size || !nameAddress || !outHits || !capacity ||
        size < sizeof(uintptr_t) * 2) {
        return 0;
    }

    size_t count = 0;
    __try {
        const uintptr_t end = begin + size;
        uintptr_t at =
            (begin + (sizeof(uintptr_t) - 1)) &
            ~(static_cast<uintptr_t>(sizeof(uintptr_t) - 1));

        for (; at + sizeof(uintptr_t) * 2 <= end && count < capacity;
             at += sizeof(uintptr_t)) {
            const uintptr_t storedName =
                *reinterpret_cast<const uintptr_t*>(at);
            if (storedName != nameAddress)
                continue;

            const uintptr_t wrapper =
                *reinterpret_cast<const uintptr_t*>(at + sizeof(uintptr_t));
            outHits[count++] = { at, wrapper };
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return count;
    }
    return count;
}

// Stage 6.7.4.29: resolve a reflected native implementation from its exact
// registration name, not from a generic wrapper-tail AOB near a historical RVA.
// The exact registered exec wrapper is first recovered from the static native
// registration table. Only then is the expected ABI tail searched inside that
// one wrapper and its own rel32 CALL decoded. This prevents same-tail sibling
// aliasing and makes the wrapper identity part of the proof.
static uintptr_t ResolveNativeFromExactRegistrationWrapper(
    const char* resolverName,
    const char* reflectedFunctionName,
    uintptr_t moduleBase,
    const uint8_t* tailPattern,
    const char* tailMask,
    size_t callOffset,
    uintptr_t* outWrapper)
{
    if (outWrapper)
        *outWrapper = 0;
    if (!resolverName || !reflectedFunctionName || !reflectedFunctionName[0] ||
        !moduleBase || !tailPattern || !tailMask)
        return 0;

    const size_t patternLen = strlen(tailMask);
    if (!patternLen || callOffset >= patternLen)
        return 0;

    FReadableImageSectionRange sections[96]{};
    size_t sectionCount = 0;
    uintptr_t imageSize = 0;
    if (!SnapshotReadableImageSections(
            moduleBase, sections, 96, &sectionCount, &imageSize)) {
        return 0;
    }

    const size_t nameLen = strlen(reflectedFunctionName);
    std::vector<uintptr_t> nameStrings;
    nameStrings.reserve(8);

    for (size_t sec = 0; sec < sectionCount && nameStrings.size() < 32; ++sec) {
        if (sections[sec].Characteristics & IMAGE_SCN_MEM_EXECUTE)
            continue;
        const uintptr_t begin = moduleBase + sections[sec].Rva;
        const uintptr_t size = sections[sec].Size;
        if (size < nameLen + 1 || !IsReadableAddress(begin, 1))
            continue;
        uintptr_t hits[32]{};
        const size_t found = FindExactAnsiStringsInRangeSEH(
            begin, size, reflectedFunctionName, nameLen,
            hits, 32 - nameStrings.size());
        for (size_t i = 0; i < found; ++i)
            nameStrings.push_back(hits[i]);
    }

    struct FCandidate {
        uintptr_t Wrapper = 0;
        uintptr_t Tail = 0;
        uintptr_t Callsite = 0;
        uintptr_t Target = 0;
        uintptr_t Pair = 0;
    };
    std::vector<FCandidate> candidates;
    candidates.reserve(8);

    for (uintptr_t nameAddress : nameStrings) {
        for (size_t sec = 0; sec < sectionCount; ++sec) {
            if (sections[sec].Characteristics & IMAGE_SCN_MEM_EXECUTE)
                continue;
            const uintptr_t begin = moduleBase + sections[sec].Rva;
            const uintptr_t size = sections[sec].Size;
            if (size < sizeof(uintptr_t) * 2 || !IsReadableAddress(begin, 1))
                continue;

            FRegistrationPairHit pairs[64]{};
            const size_t pairCount = FindRegistrationPairsInRangeSEH(
                begin, size, nameAddress, pairs, 64);

            for (size_t pi = 0; pi < pairCount; ++pi) {
                const uintptr_t wrapper = pairs[pi].Wrapper;
                if (!wrapper || !IsExecutableAddress(wrapper))
                    continue;

                bool wrapperAlreadyConsidered = false;
                for (const auto& c : candidates) {
                    if (c.Wrapper == wrapper) {
                        wrapperAlreadyConsidered = true;
                        break;
                    }
                }
                if (wrapperAlreadyConsidered)
                    continue;

                uintptr_t matchedTail = 0;
                size_t tailMatches = 0;
                // Reflected exec wrappers in this family are compact. Keep the
                // scan bounded and require a unique ABI tail inside the exact
                // named wrapper.
                for (size_t off = 0; off + patternLen <= 0x180; ++off) {
                    const uintptr_t at = wrapper + off;
                    if (!IsReadableAddress(at, patternLen))
                        break;
                    if (!MatchMaskedBytes(at, tailPattern, tailMask))
                        continue;
                    matchedTail = at;
                    ++tailMatches;
                    if (tailMatches > 1)
                        break;
                }
                if (tailMatches != 1 || !matchedTail)
                    continue;

                const uintptr_t callsite = matchedTail + callOffset;
                const uintptr_t target = ResolveRel32Call(callsite);
                if (!target || !IsExecutableAddress(target))
                    continue;

                candidates.push_back(FCandidate{
                    wrapper,
                    matchedTail,
                    callsite,
                    target,
                    pairs[pi].PairAddress
                });
            }
        }
    }

    if (candidates.size() != 1) {
        char detail[256]{};
        snprintf(
            detail, sizeof(detail),
            "reflected-name=%s nameStrings=%llu exact-wrapper-tail-candidates=%llu",
            reflectedFunctionName,
            static_cast<unsigned long long>(nameStrings.size()),
            static_cast<unsigned long long>(candidates.size()));
        RecordResolver(
            resolverName,
            "exact-registration-name+wrapper-abi-tail+rel32",
            EResolveConfidence::Failed,
            0,
            detail);
        return 0;
    }

    const auto& c = candidates[0];
    if (outWrapper)
        *outWrapper = c.Wrapper;

    char detail[512]{};
    snprintf(
        detail, sizeof(detail),
        "reflected-name=%s wrapper=+0x%llX tail=+0x%llX call=+0x%llX pair=+0x%llX target=+0x%llX",
        reflectedFunctionName,
        static_cast<unsigned long long>(c.Wrapper - moduleBase),
        static_cast<unsigned long long>(c.Tail - moduleBase),
        static_cast<unsigned long long>(c.Callsite - moduleBase),
        static_cast<unsigned long long>(c.Pair >= moduleBase ? c.Pair - moduleBase : 0),
        static_cast<unsigned long long>(c.Target - moduleBase));

    RecordResolver(
        resolverName,
        "exact-registration-name+wrapper-abi-tail+rel32",
        EResolveConfidence::Strong,
        c.Target,
        detail);
    CacheResolvedCallsite(resolverName, c.Callsite);
    CacheResolvedRva(resolverName, c.Target);

    ModLog(
        "[ElementalSystemExpanded] [ESE-RESOLVE29] EXACT: %s "
        "Wrapper=+0x%llX Call=+0x%llX Target=+0x%llX\n",
        resolverName,
        static_cast<unsigned long long>(c.Wrapper - moduleBase),
        static_cast<unsigned long long>(c.Callsite - moduleBase),
        static_cast<unsigned long long>(c.Target - moduleBase));

    return c.Target;
}

static bool ResolveVirtualSlotFromNativeRegistration(
    const char* resolverName,
    const char* reflectedFunctionName,
    uintptr_t moduleBase,
    const uint8_t* wrapperPattern,
    const char* wrapperMask,
    size_t slotImmediateOffset,
    uintptr_t* outSlot)
{
    if (!resolverName || !reflectedFunctionName || !reflectedFunctionName[0] ||
        !moduleBase || !wrapperPattern || !wrapperMask || !outSlot ||
        slotImmediateOffset < 2) {
        return false;
    }

    auto readSlot = [&](uintptr_t wrapperStart, uintptr_t* slot) -> bool {
        if (!wrapperStart || !slot ||
            !MatchMaskedBytes(wrapperStart, wrapperPattern, wrapperMask)) {
            return false;
        }

        __try {
            const uint32_t value =
                *reinterpret_cast<const uint32_t*>(
                    wrapperStart + slotImmediateOffset);

            if (value < 0x100 ||
                value > 0x2000 ||
                (value % sizeof(void*)) != 0) {
                return false;
            }

            *slot = static_cast<uintptr_t>(value);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    };

    const size_t dispatchOffset = slotImmediateOffset - 2;

    // Reuse a fingerprint-keyed result only after verifying both the exact
    // wrapper body and the slot encoded by its final virtual dispatch.
    const uintptr_t cachedDispatchRva = CachedProfileValue(
        g_BuildProfileCache.VDispatches, resolverName);
    const uintptr_t cachedSlot = CachedProfileValue(
        g_BuildProfileCache.VSlots, resolverName);

    if (cachedDispatchRva && cachedSlot &&
        cachedDispatchRva >= dispatchOffset) {
        const uintptr_t wrapperStart =
            moduleBase + cachedDispatchRva - dispatchOffset;
        uintptr_t verifiedSlot = 0;
        if (readSlot(wrapperStart, &verifiedSlot) &&
            verifiedSlot == cachedSlot) {
            *outSlot = verifiedSlot;
            RecordResolver(
                resolverName,
                "fingerprint-profile+registration-wrapper-vslot",
                EResolveConfidence::ProfileStatic,
                moduleBase + cachedDispatchRva,
                "cached exact-name wrapper + decoded slot revalidated against exact build fingerprint");
            CacheResolvedVSlot(
                resolverName,
                verifiedSlot,
                moduleBase + cachedDispatchRva);
            return true;
        }
    }

    FReadableImageSectionRange sections[96]{};
    size_t sectionCount = 0;
    uintptr_t imageSize = 0;
    if (!SnapshotReadableImageSections(
            moduleBase,
            sections,
            96,
            &sectionCount,
            &imageSize)) {
        return false;
    }

    const size_t nameLen = strlen(reflectedFunctionName);
    if (!nameLen)
        return false;

    std::vector<uintptr_t> nameStrings;
    nameStrings.reserve(8);

    // Registration names/tables live in readable data sections. Excluding
    // executable sections avoids rescanning the ~96 MiB .text body for each
    // function name and is semantically appropriate for static registration
    // metadata.
    for (size_t s = 0; s < sectionCount && nameStrings.size() < 32; ++s) {
        if (sections[s].Characteristics & IMAGE_SCN_MEM_EXECUTE)
            continue;

        const uintptr_t begin = moduleBase + sections[s].Rva;
        const uintptr_t size = sections[s].Size;
        if (size < nameLen + 1 || !IsReadableAddress(begin, 1))
            continue;

        uintptr_t hits[32]{};
        const size_t remaining = 32 - nameStrings.size();
        const size_t found = FindExactAnsiStringsInRangeSEH(
            begin,
            size,
            reflectedFunctionName,
            nameLen,
            hits,
            remaining);
        for (size_t i = 0; i < found; ++i)
            nameStrings.push_back(hits[i]);
    }

    if (nameStrings.empty())
        return false;

    struct FCandidate {
        uintptr_t Wrapper = 0;
        uintptr_t Slot = 0;
        uintptr_t PairRva = 0;
    };

    std::vector<FCandidate> candidates;
    candidates.reserve(8);
    size_t registrationPairCount = 0;

    for (uintptr_t nameAddress : nameStrings) {
        for (size_t s = 0; s < sectionCount; ++s) {
            if (sections[s].Characteristics & IMAGE_SCN_MEM_EXECUTE)
                continue;

            const uintptr_t begin = moduleBase + sections[s].Rva;
            const uintptr_t size = sections[s].Size;
            if (size < sizeof(uintptr_t) * 2 || !IsReadableAddress(begin, 1))
                continue;

            FRegistrationPairHit hits[64]{};
            const size_t found = FindRegistrationPairsInRangeSEH(
                begin,
                size,
                nameAddress,
                hits,
                64);

            for (size_t h = 0; h < found; ++h) {
                const uintptr_t wrapper = hits[h].Wrapper;
                if (!wrapper || !IsExecutableAddress(wrapper))
                    continue;

                ++registrationPairCount;

                uintptr_t slot = 0;
                if (!readSlot(wrapper, &slot))
                    continue;

                bool duplicate = false;
                for (const auto& existing : candidates) {
                    if (existing.Wrapper == wrapper) {
                        duplicate = true;
                        break;
                    }
                }
                if (duplicate)
                    continue;

                candidates.push_back(FCandidate{
                    wrapper,
                    slot,
                    hits[h].PairAddress >= moduleBase
                        ? hits[h].PairAddress - moduleBase
                        : 0
                });
            }
        }
    }

    if (candidates.empty())
        return false;

    uintptr_t consensusSlot = candidates[0].Slot;
    bool slotsAgree = consensusSlot != 0;
    for (const auto& candidate : candidates) {
        if (!candidate.Slot || candidate.Slot != consensusSlot) {
            slotsAgree = false;
            break;
        }
    }

    if (!slotsAgree)
        return false;

    const uintptr_t selectedWrapper = candidates[0].Wrapper;
    const uintptr_t dispatch = selectedWrapper + dispatchOffset;
    *outSlot = consensusSlot;

    char detail[640]{};
    snprintf(
        detail,
        sizeof(detail),
        "reflected-name=%s nameStrings=%llu registrationPairs=%llu matchingWrappers=%llu selected-wrapper=+0x%llX slot=+0x%llX pair=+0x%llX all-slots-agree=YES",
        reflectedFunctionName,
        static_cast<unsigned long long>(nameStrings.size()),
        static_cast<unsigned long long>(registrationPairCount),
        static_cast<unsigned long long>(candidates.size()),
        static_cast<unsigned long long>(
            selectedWrapper >= moduleBase ? selectedWrapper - moduleBase : 0),
        static_cast<unsigned long long>(consensusSlot),
        static_cast<unsigned long long>(candidates[0].PairRva));

    RecordResolver(
        resolverName,
        candidates.size() == 1
            ? "native-registration-name+unique-wrapper-vslot"
            : "native-registration-name+wrapper-consensus-vslot",
        EResolveConfidence::Strong,
        dispatch,
        detail);

    CacheResolvedVSlot(
        resolverName,
        consensusSlot,
        dispatch);

    return true;
}



static void SanitizeAliasedShooterProfileEntry()
{
    static constexpr const char* kAim =
        "PalShooterComponent::SetDisableAimFlag_Layered";
    static constexpr const char* kShoot =
        "PalShooterComponent::SetDisableShootFlag_Layered";
    static constexpr const char* kChange =
        "PalShooterComponent::SetDisableChangeWeaponFlag_Layered";

    if (!g_BuildProfileCache.FingerprintMatched) {
        g_ShooterCacheRepairStatus = "no-matched-profile";
        return;
    }

    const uintptr_t aimNative = CachedProfileValue(
        g_BuildProfileCache.Rvas, kAim);
    const uintptr_t shootNative = CachedProfileValue(
        g_BuildProfileCache.Rvas, kShoot);
    const uintptr_t changeNative = CachedProfileValue(
        g_BuildProfileCache.Rvas, kChange);

    const uintptr_t aimCall = CachedProfileValue(
        g_BuildProfileCache.Callsites, kAim);
    const uintptr_t shootCall = CachedProfileValue(
        g_BuildProfileCache.Callsites, kShoot);
    const uintptr_t changeCall = CachedProfileValue(
        g_BuildProfileCache.Callsites, kChange);

    const bool nativeAlias =
        changeNative &&
        ((aimNative && changeNative == aimNative) ||
         (shootNative && changeNative == shootNative));
    const bool callAlias =
        changeCall &&
        ((aimCall && changeCall == aimCall) ||
         (shootCall && changeCall == shootCall));

    if (!nativeAlias && !callAlias) {
        g_ShooterCacheRepairStatus = "checked-no-alias";
        return;
    }

    // Distinct reflected setters must not share the same wrapper callsite or
    // native target. A stale keyed profile can otherwise keep revalidating the
    // wrong shared shooter-tail match forever. Invalidate only ChangeWeapon;
    // Aim and Shoot remain independently fingerprint/profile validated and act
    // as the semantic anchors for the bounded sibling triangulation below.
    g_BuildProfileCache.Rvas.erase(kChange);
    g_BuildProfileCache.Callsites.erase(kChange);

    char status[256]{};
    snprintf(
        status,
        sizeof(status),
        "invalidated-stale-changeweapon-alias native=%s call=%s oldNative=0x%llX oldCall=0x%llX",
        nativeAlias ? "YES" : "NO",
        callAlias ? "YES" : "NO",
        static_cast<unsigned long long>(changeNative),
        static_cast<unsigned long long>(changeCall));
    g_ShooterCacheRepairStatus = status;

    ShipLog(
        "[ElementalSystemExpanded] PROFILE REPAIR: rejected cached ChangeWeapon "
        "shooter alias (nativeAlias=%s callAlias=%s oldNative=+0x%llX oldCall=+0x%llX); "
        "forcing semantic sibling triangulation.\n",
        nativeAlias ? "YES" : "NO",
        callAlias ? "YES" : "NO",
        static_cast<unsigned long long>(changeNative),
        static_cast<unsigned long long>(changeCall));
}

static bool InstallNativeReactionHooks(uintptr_t base)
{
    SanitizeAliasedShooterProfileEntry();
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
            reinterpret_cast<void*>(&Detour_SetActionClassParameter),
            reinterpret_cast<void**>(&Original_SetActionClassParameter)},
        {"PalActionComponent::PlayAction",
            Known104::Rva::PlayAction_Call,
            Known104::Rva::PlayAction_Native,
            kPlayActionTail,kPlayActionMask,31,
            reinterpret_cast<void*>(&Detour_PlayAction),
            reinterpret_cast<void**>(&Original_PlayAction)},
        {"PalCharacterMovementComponent::SetJumpDisableFlag",
            Known104::Rva::SetJumpDisableFlag_Call,
            Known104::Rva::SetJumpDisableFlag_Native,
            kNamedBoolTail,kNamedBoolMask,31,
            reinterpret_cast<void*>(&Detour_SetJumpDisableFlag),
            reinterpret_cast<void**>(&Original_SetJumpDisableFlag)},
        {"PalCharacterMovementComponent::SetStepDisableFlag",
            Known104::Rva::SetStepDisableFlag_Call,
            Known104::Rva::SetStepDisableFlag_Native,
            kNamedBoolTail,kNamedBoolMask,31,
            reinterpret_cast<void*>(&Detour_SetStepDisableFlag),
            reinterpret_cast<void**>(&Original_SetStepDisableFlag)},
        {"PalCharacterMovementComponent::SetMoveDisableFlag",
            Known104::Rva::SetMoveDisableFlag_Call,
            Known104::Rva::SetMoveDisableFlag_Native,
            kNamedBoolTail,kNamedBoolMask,31,
            reinterpret_cast<void*>(&Detour_SetMoveDisableFlag),
            reinterpret_cast<void**>(&Original_SetMoveDisableFlag)},
        {"PalCharacterMovementComponent::SetWalkSpeedMultiplier",
            Known104::Rva::SetWalkSpeedMultiplier_Call,
            Known104::Rva::SetWalkSpeedMultiplier_Native,
            kNamedFloatTail,kNamedFloatMask,32,
            reinterpret_cast<void*>(&Detour_SetWalkSpeedMultiplier),
            reinterpret_cast<void**>(&Original_SetWalkSpeedMultiplier)},
        {"PalCharacterMovementComponent::SetYawRotatorMultiplier",
            Known104::Rva::SetYawRotatorMultiplier_Call,
            Known104::Rva::SetYawRotatorMultiplier_Native,
            kNamedFloatTail,kNamedFloatMask,32,
            reinterpret_cast<void*>(&Detour_SetYawRotatorMultiplier),
            reinterpret_cast<void**>(&Original_SetYawRotatorMultiplier)},
        {"PalShooterComponent::SetDisableAimFlag_Layered",
            Known104::Rva::SetDisableAimFlag_Call,
            Known104::Rva::SetDisableAimFlag_Native,
            kShooterTail,kShooterMask,33,
            reinterpret_cast<void*>(&Detour_SetDisableAimFlag_Layered),
            reinterpret_cast<void**>(&Original_SetDisableAimFlag_Layered)},
        {"PalShooterComponent::SetDisableShootFlag_Layered",
            Known104::Rva::SetDisableShootFlag_Call,
            Known104::Rva::SetDisableShootFlag_Native,
            kShooterTail,kShooterMask,33,
            reinterpret_cast<void*>(&Detour_SetDisableShootFlag_Layered),
            reinterpret_cast<void**>(&Original_SetDisableShootFlag_Layered)},
        {"PalShooterComponent::SetDisableChangeWeaponFlag_Layered",
            Known104::Rva::SetDisableChangeWeaponFlag_Call,
            Known104::Rva::SetDisableChangeWeaponFlag_Native,
            kShooterTail,kShooterMask,33,
            reinterpret_cast<void*>(&Detour_SetDisableChangeWeaponFlag_Layered),
            reinterpret_cast<void**>(&Original_SetDisableChangeWeaponFlag_Layered)}
    };

    // Resolve every dependency first. Do not install a partial reaction layer.
    uintptr_t targets[sizeof(hooks) / sizeof(hooks[0])]{};
    for (size_t i = 0; i < sizeof(hooks) / sizeof(hooks[0]); ++i) {
        const bool isChangeWeapon =
            strcmp(
                hooks[i].Name,
                "PalShooterComponent::SetDisableChangeWeaponFlag_Layered") == 0;

        const bool isJump =
            strcmp(
                hooks[i].Name,
                "PalCharacterMovementComponent::SetJumpDisableFlag") == 0;
        const bool isStep =
            strcmp(
                hooks[i].Name,
                "PalCharacterMovementComponent::SetStepDisableFlag") == 0;
        const bool isMove =
            strcmp(
                hooks[i].Name,
                "PalCharacterMovementComponent::SetMoveDisableFlag") == 0;
        const bool isExactMovementBool = isJump || isStep || isMove;

        // Stage29: these three setters are no longer allowed to resolve from a
        // generic near-AOB, profile-only identity, or sibling topology. Resolve
        // the exact reflected registration name first, validate the named-bool
        // ABI tail inside that exact wrapper, then follow that wrapper's own CALL.
        if (isExactMovementBool) {
            uintptr_t* wrapperOut = nullptr;
            const char* reflectedName = nullptr;
            if (isJump) {
                wrapperOut = &g_ExactWrapper_SetJumpDisableFlag;
                reflectedName = "SetJumpDisableFlag";
            }
            else if (isStep) {
                wrapperOut = &g_ExactWrapper_SetStepDisableFlag;
                reflectedName = "SetStepDisableFlag";
            }
            else {
                wrapperOut = &g_ExactWrapper_SetMoveDisableFlag;
                reflectedName = "SetMoveDisableFlag";
            }

            targets[i] = ResolveNativeFromExactRegistrationWrapper(
                hooks[i].Name,
                reflectedName,
                base,
                hooks[i].Pattern,
                hooks[i].Mask,
                hooks[i].CallOffset,
                wrapperOut);
        }
        else if (!isChangeWeapon) {
            targets[i] = ResolveNativeFromWrapperCall(
                hooks[i].Name,
                base,
                hooks[i].KnownCallRva,
                hooks[i].KnownNativeRva,
                hooks[i].Pattern,
                hooks[i].Mask,
                hooks[i].CallOffset);
        }

        // Yaw's reflected wrapper no longer preserves its historical absolute
        // spacing from WalkSpeed on PalServer. Resolve it instead as the unique
        // immediately-following reflected wrapper whose final native dispatch
        // lands in the same local movement-setter implementation cluster.
        if (!targets[i] &&
            strcmp(
                hooks[i].Name,
                "PalCharacterMovementComponent::SetYawRotatorMultiplier") == 0) {
            targets[i] = ResolveNativeFromForwardSiblingCluster(
                hooks[i].Name,
                base,
                "PalCharacterMovementComponent::SetWalkSpeedMultiplier",
                Known104::Rva::SetWalkSpeedMultiplier_Call,
                Known104::Rva::SetYawRotatorMultiplier_Call,
                Known104::Rva::SetWalkSpeedMultiplier_Native,
                Known104::Rva::SetYawRotatorMultiplier_Native);
        }


        // Aim and Shoot independently preserve their historical endpoint call
        // and native spans on PalServer. Recover ChangeWeapon as the unique
        // middle sibling predicted from both endpoints; never accept the shared
        // shooter-tail AOB alias to Aim.
        if (!targets[i] && isChangeWeapon) {
            targets[i] = ResolveNativeFromShooterSiblingTopology(
                hooks[i].Name,
                base,
                "PalShooterComponent::SetDisableAimFlag_Layered",
                "PalShooterComponent::SetDisableShootFlag_Layered",
                Known104::Rva::SetDisableAimFlag_Call,
                Known104::Rva::SetDisableChangeWeaponFlag_Call,
                Known104::Rva::SetDisableShootFlag_Call,
                Known104::Rva::SetDisableAimFlag_Native,
                Known104::Rva::SetDisableChangeWeaponFlag_Native,
                Known104::Rva::SetDisableShootFlag_Native);
        }
        if (!targets[i])
            return false;
    }

    FHookTransactionSpec transaction[sizeof(hooks) / sizeof(hooks[0])]{};
    for (size_t i = 0; i < sizeof(hooks) / sizeof(hooks[0]); ++i) {
        transaction[i] = FHookTransactionSpec{
            hooks[i].Name,
            targets[i],
            hooks[i].Detour,
            hooks[i].Original
        };
    }

    if (!InstallHookTransaction(
            "WetGatedStrongReactions",
            transaction,
            sizeof(transaction) / sizeof(transaction[0]))) {
        return false;
    }

    // Observation-only hooks on the exact reflected exec wrappers. Failure is
    // non-fatal for gameplay: the native gates above remain the authoritative
    // behavior, while the resolver report still contains exact wrapper proof.
    if (g_ExactWrapper_SetJumpDisableFlag &&
        g_ExactWrapper_SetStepDisableFlag &&
        g_ExactWrapper_SetMoveDisableFlag) {
        FHookTransactionSpec wrapperHooks[] = {
            {
                "ExecWrapper.SetJumpDisableFlag",
                g_ExactWrapper_SetJumpDisableFlag,
                reinterpret_cast<void*>(&Detour_Exec_SetJumpDisableFlag),
                reinterpret_cast<void**>(&Original_Exec_SetJumpDisableFlag)
            },
            {
                "ExecWrapper.SetStepDisableFlag",
                g_ExactWrapper_SetStepDisableFlag,
                reinterpret_cast<void*>(&Detour_Exec_SetStepDisableFlag),
                reinterpret_cast<void**>(&Original_Exec_SetStepDisableFlag)
            },
            {
                "ExecWrapper.SetMoveDisableFlag",
                g_ExactWrapper_SetMoveDisableFlag,
                reinterpret_cast<void*>(&Detour_Exec_SetMoveDisableFlag),
                reinterpret_cast<void**>(&Original_Exec_SetMoveDisableFlag)
            }
        };

        if (!InstallHookTransaction(
                "Stage29ExactMovementExecObservation",
                wrapperHooks,
                sizeof(wrapperHooks) / sizeof(wrapperHooks[0]))) {
            ModLog(
                "[ElementalSystemExpanded] [ESE-RESOLVE29] WARNING: exact "
                "movement exec-wrapper observation hooks unavailable; native "
                "exact-registration gates remain active.\n");
        }
    }

    return true;
}

// ---------------------------------------------------------
// Durable reflected-wrapper virtual-slot extraction
// ---------------------------------------------------------
static bool ResolveDurableVirtualSlots(uintptr_t base)
{
    // Complete reflected-wrapper bodies captured from the validated client
    // build. rel32 helper-call displacements and the final vtable-slot
    // immediate are wildcarded. These signatures identify the exact reflected
    // UFunctions rather than merely a generic virtual-dispatch tail.

    static const uint8_t kComponentTickFull[] = {
        0x48,0x89,0x5C,0x24,0x10,0x48,0x89,0x74,0x24,0x18,0x57,0x48,0x83,0xEC,0x20,0x33,
        0xFF,0x48,0x8B,0xDA,0x89,0x7C,0x24,0x30,0x48,0x8B,0xF1,0xE8,0,0,0,0,
        0x48,0x8B,0xCB,0x48,0x39,0x7B,0x20,0x74,0x10,0x48,0x8B,0x53,0x18,0x4C,0x8D,0x44,
        0x24,0x30,0xE8,0,0,0,0,0xEB,0x1C,0x4C,0x8B,0x83,0x88,0,0,0,0x48,0x8D,
        0x54,0x24,0x30,0x49,0x8B,0x40,0x20,0x48,0x89,0x83,0x88,0,0,0,0xE8,0,0,0,0,
        0x48,0x8B,0x43,0x20,0x48,0x8B,0xCE,0x48,0x85,0xC0,0x40,0x0F,0x95,0xC7,0x48,
        0x03,0xF8,0x83,0x7C,0x24,0x30,0x00,0x48,0x89,0x7B,0x20,0x48,0x8B,0x06,0x0F,
        0x95,0xC2,0xFF,0x90,0,0,0,0
    };
    static const char kComponentTickFullMask[] =
        "xxxxxxxxxxxxxxxxxxxxxxxxxxxx????xxxxxxxxxxxxxxxxxxx????xxxxxxxxxxxxxxxxxxxxxxxxxx????xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx????";

    static const uint8_t kStopMontageFull[] = {
        0x48,0x89,0x5C,0x24,0x10,0x48,0x89,0x74,0x24,0x18,0x57,0x48,0x83,0xEC,0x20,0x33,
        0xFF,0x48,0x8B,0xDA,0x48,0x89,0x7C,0x24,0x30,0x48,0x8B,0xF1,0xE8,0,0,0,0,
        0x48,0x8B,0xCB,0x48,0x39,0x7B,0x20,0x74,0x10,0x48,0x8B,0x53,0x18,0x4C,0x8D,0x44,
        0x24,0x30,0xE8,0,0,0,0,0xEB,0x1C,0x4C,0x8B,0x83,0x88,0,0,0,0x48,0x8D,
        0x54,0x24,0x30,0x49,0x8B,0x40,0x20,0x48,0x89,0x83,0x88,0,0,0,0xE8,0,0,0,0,
        0x48,0x8B,0x43,0x20,0x48,0x8B,0xCE,0x48,0x8B,0x54,0x24,0x30,0x48,0x85,0xC0,
        0x40,0x0F,0x95,0xC7,0x48,0x03,0xF8,0x48,0x89,0x7B,0x20,0x48,0x8B,0x06,0xFF,
        0x90,0,0,0,0
    };
    static const char kStopMontageFullMask[] =
        "xxxxxxxxxxxxxxxxxxxxxxxxxxxxx????xxxxxxxxxxxxxxxxxxx????xxxxxxxxxxxxxxxxxxxxxxxxxx????xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx????";

    static const uint8_t kStatusTickFull[] = {
        0x48,0x89,0x5C,0x24,0x10,0x57,0x48,0x83,0xEC,0x20,0x48,0x8B,0xDA,0xC7,0x44,0x24,
        0x30,0,0,0,0,0x48,0x8B,0xF9,0xE8,0,0,0,0,0x48,0x83,0x7B,0x20,0x00,0x48,
        0x8B,0xCB,0x74,0x10,0x48,0x8B,0x53,0x18,0x4C,0x8D,0x44,0x24,0x30,0xE8,0,0,0,0,
        0xEB,0x1C,0x4C,0x8B,0x83,0x88,0,0,0,0x48,0x8D,0x54,0x24,0x30,0x49,0x8B,0x40,
        0x20,0x48,0x89,0x83,0x88,0,0,0,0xE8,0,0,0,0,0x48,0x8B,0x43,0x20,0x33,0xC9,
        0xF3,0x0F,0x10,0x4C,0x24,0x30,0x48,0x85,0xC0,0x0F,0x95,0xC1,0x48,0x03,0xC8,
        0x48,0x89,0x4B,0x20,0x48,0x8B,0xCF,0x48,0x8B,0x07,0xFF,0x90,0,0,0,0
    };
    static const char kStatusTickFullMask[] =
        "xxxxxxxxxxxxxxxxxxxxxxxxx????xxxxxxxxxxxxxxxxxxxx????xxxxxxxxxxxxxxxxxxxxxxxxxx????xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx????";

    const bool componentTick =
        ResolveVirtualSlotFromNativeRegistration(
            "VSlot.ActorComponent::SetComponentTickEnabled",
            "SetComponentTickEnabled",
            base,
            kComponentTickFull,
            kComponentTickFullMask,
            119,
            &g_VSlot_SetComponentTickEnabled) ||
        ResolveVirtualSlotFromFullWrapper(
            "VSlot.ActorComponent::SetComponentTickEnabled",
            base,
            kComponentTickFull,
            kComponentTickFullMask,
            119,
            &g_VSlot_SetComponentTickEnabled);

    const bool stopMontage =
        ResolveVirtualSlotFromNativeRegistration(
            "VSlot.Character::StopAnimMontage",
            "StopAnimMontage",
            base,
            kStopMontageFull,
            kStopMontageFullMask,
            117,
            &g_VSlot_StopAnimMontage) ||
        ResolveVirtualSlotFromFullWrapper(
            "VSlot.Character::StopAnimMontage",
            base,
            kStopMontageFull,
            kStopMontageFullMask,
            117,
            &g_VSlot_StopAnimMontage);

    const bool statusTick =
        ResolveVirtualSlotFromNativeRegistration(
            "VSlot.PalStatusBase::TickStatus",
            "TickStatus",
            base,
            kStatusTickFull,
            kStatusTickFullMask,
            116,
            &g_VSlot_StatusTick) ||
        ResolveVirtualSlotFromFullWrapper(
            "VSlot.PalStatusBase::TickStatus",
            base,
            kStatusTickFull,
            kStatusTickFullMask,
            116,
            &g_VSlot_StatusTick);

    return componentTick && stopMontage && statusTick;
}

// ---------------------------------------------------------
// Blocked Freeze persistent runtime state
//
// The synchronous TLS gate ends after AddStatus. Freeze's StartLocation pin
// occurs later from BP_Status_Freeze::TickStatus, so remember only those exact
// Freeze status instances for which strong control was denied.
// ---------------------------------------------------------

struct FBlockedFreezeRuntime {
    void* Character = nullptr;
    void* RootComponent = nullptr;
    bool TickObservedLogged = false;
    // 0 = not validated yet, 1 = the dynamically located StartLocation field
    // has been validated for this instance, -1 = validation failed.
    int8_t StartLocationState = 0;
    bool StartLocationRefreshLogged = false;

    // Stage 6.7.4.29 authority-localization diagnostics. Observation-only and
    // capped so the gameplay tick cannot create unbounded debug output.
    uint32_t AuthorityProbeTicks = 0;
    bool AuthorityFirstRootValid = false;
    double AuthorityFirstRootX = 0.0;
    double AuthorityFirstRootY = 0.0;
    double AuthorityFirstRootZ = 0.0;
};

static std::unordered_map<void*, FBlockedFreezeRuntime>
g_BlockedFreezeRuntime;
static SRWLOCK g_BlockedFreezeRuntimeLock = SRWLOCK_INIT;

struct FFreezeTickTLS {
    bool Active = false;
    void* Status = nullptr;
};

static thread_local FFreezeTickTLS g_FreezeTickTLS{};

static void RemoveBlockedFreezeRuntime(void* status)
{
    if (!status)
        return;

    AcquireSRWLockExclusive(&g_BlockedFreezeRuntimeLock);
    g_BlockedFreezeRuntime.erase(status);
    ReleaseSRWLockExclusive(&g_BlockedFreezeRuntimeLock);
}

static bool GetBlockedFreezeRuntime(
    void* status,
    FBlockedFreezeRuntime* outState)
{
    if (!status || !outState)
        return false;

    bool found = false;

    AcquireSRWLockShared(&g_BlockedFreezeRuntimeLock);
    const auto it = g_BlockedFreezeRuntime.find(status);
    if (it != g_BlockedFreezeRuntime.end()) {
        *outState = it->second;
        found = true;
    }
    ReleaseSRWLockShared(&g_BlockedFreezeRuntimeLock);

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
        RemoveBlockedFreezeRuntime(status);
        return false;
    }

    if (statusID != ActiveIds::Status_Freeze || isEndStatus) {
        RemoveBlockedFreezeRuntime(status);
        return false;
    }

    return true;
}

static bool ReadFreezeAIActionPointer(
    void* status,
    void** outAction)
{
    if (!status || !outAction)
        return false;

    // PROVEN FROM BP_Status_ reflection dump for this build:
    // BP_Status_Freeze_C::AIActionFreeze is the UObject property at +0xD0.
    // Stage27 observes this field only; no write is performed.
    constexpr ptrdiff_t kFreezeAIActionOffset = 0xD0;

    void* action = nullptr;
    __try {
        action = *reinterpret_cast<void**>(
            reinterpret_cast<uintptr_t>(status) +
            kFreezeAIActionOffset);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }

    *outAction = action;
    return true;
}

static void MarkBlockedFreezeTickObserved(void* status)
{
    AcquireSRWLockExclusive(&g_BlockedFreezeRuntimeLock);
    const auto it = g_BlockedFreezeRuntime.find(status);
    if (it != g_BlockedFreezeRuntime.end())
        it->second.TickObservedLogged = true;
    ReleaseSRWLockExclusive(&g_BlockedFreezeRuntimeLock);
}

static void PruneEndedBlockedFreezeRuntime(void* status)
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
        RemoveBlockedFreezeRuntime(status);
}


struct FVectorDouble {
    double X;
    double Y;
    double Z;
};

struct FVectorFloat {
    float X;
    float Y;
    float Z;
};

static double AbsDouble(double v)
{
    return (v < 0.0) ? -v : v;
}

static uint32_t ReserveAuthorityProbeTick(
    void* status,
    const FVectorDouble& currentRoot,
    double* outDeltaFromFirstSq)
{
    if (outDeltaFromFirstSq)
        *outDeltaFromFirstSq = -1.0;
    if (!status)
        return 0;

    uint32_t sequence = 0;

    AcquireSRWLockExclusive(&g_BlockedFreezeRuntimeLock);
    const auto it = g_BlockedFreezeRuntime.find(status);
    if (it != g_BlockedFreezeRuntime.end()) {
        FBlockedFreezeRuntime& state = it->second;

        constexpr uint32_t kMaxAuthorityProbeTicks = 24;
        if (state.AuthorityProbeTicks < kMaxAuthorityProbeTicks) {
            ++state.AuthorityProbeTicks;
            sequence = state.AuthorityProbeTicks;

            if (!state.AuthorityFirstRootValid) {
                state.AuthorityFirstRootX = currentRoot.X;
                state.AuthorityFirstRootY = currentRoot.Y;
                state.AuthorityFirstRootZ = currentRoot.Z;
                state.AuthorityFirstRootValid = true;
            }

            if (outDeltaFromFirstSq) {
                const double dx = currentRoot.X - state.AuthorityFirstRootX;
                const double dy = currentRoot.Y - state.AuthorityFirstRootY;
                const double dz = currentRoot.Z - state.AuthorityFirstRootZ;
                *outDeltaFromFirstSq = dx * dx + dy * dy + dz * dz;
            }
        }
    }
    ReleaseSRWLockExclusive(&g_BlockedFreezeRuntimeLock);

    return sequence;
}


static bool IsPlausibleWorldVector(
    const FVectorDouble& v)
{
    if (v.X != v.X || v.Y != v.Y || v.Z != v.Z)
        return false;

    constexpr double kLimit = 1000000000.0;
    return AbsDouble(v.X) < kLimit &&
        AbsDouble(v.Y) < kLimit &&
        AbsDouble(v.Z) < kLimit;
}

static double VectorDistanceSq(
    const FVectorDouble& a,
    const FVectorDouble& b)
{
    const double dx = a.X - b.X;
    const double dy = a.Y - b.Y;
    const double dz = a.Z - b.Z;
    return dx * dx + dy * dy + dz * dz;
}

static void SetBlockedFreezeStartLocationState(
    void* status,
    int8_t state,
    bool refreshLogged)
{
    AcquireSRWLockExclusive(&g_BlockedFreezeRuntimeLock);

    const auto it = g_BlockedFreezeRuntime.find(status);
    if (it != g_BlockedFreezeRuntime.end()) {
        it->second.StartLocationState = state;
        if (refreshLogged)
            it->second.StartLocationRefreshLogged = true;
    }

    ReleaseSRWLockExclusive(&g_BlockedFreezeRuntimeLock);
}

// Per-class/runtime discovery. 0 means unresolved.
// Positive values are byte offsets from BP_Status_Freeze_C instance base.
static ptrdiff_t g_FreezeStartLocationOffset = 0;
static bool g_FreezeStartLocationUsesFloat = false;
static SRWLOCK g_FreezeStartLocationLocatorLock = SRWLOCK_INIT;

static bool g_RootReferenceValidationLogged = false;
static bool g_RootReferenceCrossCheckPassed = false;
static double g_RootReferenceCrossCheckDistSq = -1.0;
static bool g_RootReferenceUsedHiddenFallback = false;
static SRWLOCK g_RootReferenceDiagnosticsLock = SRWLOCK_INIT;

static bool ShouldRunRootReferenceCrossCheck()
{
    bool shouldRun = false;
    AcquireSRWLockShared(&g_RootReferenceDiagnosticsLock);
    shouldRun = !g_RootReferenceValidationLogged;
    ReleaseSRWLockShared(&g_RootReferenceDiagnosticsLock);
    return shouldRun;
}

static void StoreRootReferenceCrossCheck(bool passed, double distSq)
{
    bool changed = false;
    AcquireSRWLockExclusive(&g_RootReferenceDiagnosticsLock);
    if (!g_RootReferenceValidationLogged) {
        g_RootReferenceCrossCheckPassed = passed;
        g_RootReferenceCrossCheckDistSq = distSq;
        g_RootReferenceValidationLogged = true;
        changed = true;
    }
    ReleaseSRWLockExclusive(&g_RootReferenceDiagnosticsLock);
    if (changed)
        MarkRuntimeSnapshotDirty();
}

static void MarkRootReferenceHiddenFallbackUsed()
{
    bool changed = false;
    AcquireSRWLockExclusive(&g_RootReferenceDiagnosticsLock);
    if (!g_RootReferenceUsedHiddenFallback) {
        g_RootReferenceUsedHiddenFallback = true;
        changed = true;
    }
    ReleaseSRWLockExclusive(&g_RootReferenceDiagnosticsLock);
    if (changed)
        MarkRuntimeSnapshotDirty();
}


static bool ReadRootWorldLocation(
    void* rootComponent,
    FVectorDouble* outLocation)
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
                !IsKnown104Fingerprint(g_CurrentFingerprint)) {
                return false;
            }

            // On unknown builds attached roots also fail closed rather than
            // misusing local-space coordinates. The exact 1.0.4 oracle may
            // still use its separately validated hidden world translation.
            if (attachStateKnown && rootIsAttached &&
                !IsKnown104Fingerprint(g_CurrentFingerprint)) {
                return false;
            }
        }

        FVectorDouble relative{};
        bool readOk = false;
        __try {
            relative = *reinterpret_cast<const FVectorDouble*>(
                reinterpret_cast<uintptr_t>(rootComponent) +
                EseGeneratedPalLayout::SceneComponent_RelativeLocation);
            readOk = true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            readOk = false;
        }

        if (readOk && IsPlausibleWorldVector(relative)) {
            // If the generated SDK proves that this root is attached,
            // RelativeLocation is local-space. Preserve known-1.0.4 behavior
            // via the separately validated hidden world field, but never
            // generalize that hidden offset to an unknown executable.
            if (attachStateKnown && rootIsAttached &&
                IsKnown104Fingerprint(g_CurrentFingerprint)) {
                FVectorDouble attachedWorld{};
                bool attachedWorldOk = false;
                __try {
                    attachedWorld = *reinterpret_cast<const FVectorDouble*>(
                        reinterpret_cast<uintptr_t>(rootComponent) +
                        Known104::Offset::RootComponent_WorldLocation);
                    attachedWorldOk = true;
                }
                __except (EXCEPTION_EXECUTE_HANDLER) {
                    attachedWorldOk = false;
                }

                if (!attachedWorldOk ||
                    !IsPlausibleWorldVector(attachedWorld)) {
                    return false;
                }

                *outLocation = attachedWorld;
                MarkRootReferenceHiddenFallbackUsed();
                return true;
            }

            *outLocation = relative;

            // On the preserved 1.0.4 oracle only, independently compare the
            // generated reflected location against the old hidden
            // ComponentToWorld translation. The hidden +0x260 value is never
            // trusted on unknown builds.
            if (ShouldRunRootReferenceCrossCheck() &&
                IsKnown104Fingerprint(g_CurrentFingerprint)) {
                FVectorDouble oldWorld{};
                bool oldOk = false;
                __try {
                    oldWorld = *reinterpret_cast<const FVectorDouble*>(
                        reinterpret_cast<uintptr_t>(rootComponent) +
                        Known104::Offset::RootComponent_WorldLocation);
                    oldOk = true;
                }
                __except (EXCEPTION_EXECUTE_HANDLER) {
                    oldOk = false;
                }

                const double distSq = oldOk
                    ? VectorDistanceSq(relative, oldWorld)
                    : 1.0e30;
                const bool crossCheckPass =
                    oldOk && IsPlausibleWorldVector(oldWorld) && distSq <= 1.0;
                ModLog(
                    "[ElementalSystemExpanded] ROOT POSITION SOURCE: generated "
                    "USceneComponent::RelativeLocation +0x%llX; 1.0.4 hidden-world "
                    "cross-check %s DistSq=%.3f.\n",
                    static_cast<unsigned long long>(
                        EseGeneratedPalLayout::SceneComponent_RelativeLocation),
                    crossCheckPass ? "PASS" : "DIFF",
                    distSq);
                StoreRootReferenceCrossCheck(crossCheckPass, distSq);
            }
            return true;
        }
    }
#endif

    // Compatibility fallback for the preserved known build / older generated
    // headers. Never use the hidden +0x260 field as an authority on an unknown
    // executable.
    if (!IsKnown104Fingerprint(g_CurrentFingerprint))
        return false;

    MarkRootReferenceHiddenFallbackUsed();
    __try {
        *outLocation =
            *reinterpret_cast<const FVectorDouble*>(
                reinterpret_cast<uintptr_t>(rootComponent) +
                Known104::Offset::RootComponent_WorldLocation);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }

    return IsPlausibleWorldVector(*outLocation);
}

static bool IsReadableRange(
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

static bool LocateFreezeStartLocation(
    void* status,
    const FVectorDouble& current,
    ptrdiff_t* outOffset,
    bool* outUsesFloat)
{
    if (!status || !outOffset || !outUsesFloat)
        return false;

    struct FFreezePositionCandidate {
        ptrdiff_t Offset;
        bool UsesFloat;
        double DistanceSq;
        FVectorDouble Value;
    };

    FFreezePositionCandidate candidates[32]{};
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
        if (!IsReadableRange(base + off, 24))
            break;

        FVectorDouble value{};

        __try {
            value =
                *reinterpret_cast<const FVectorDouble*>(
                    base + off);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            continue;
        }

        if (!IsPlausibleWorldVector(value))
            continue;

        const double distSq =
            VectorDistanceSq(value, current);

        // 2 metres is deliberately generous for initial discovery.
        if (distSq <= 40000.0 && candidateCount < 32) {
            candidates[candidateCount++] =
                FFreezePositionCandidate{ off, false, distSq, value };
        }
    }

    // Also inspect 3-float layouts in case this generated BP property was
    // emitted with float precision despite the engine-side root transform.
    for (ptrdiff_t off = kStart; off <= kEnd - 12; off += 4) {
        if (!IsReadableRange(base + off, 12))
            break;

        FVectorFloat f{};

        __try {
            f =
                *reinterpret_cast<const FVectorFloat*>(
                    base + off);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            continue;
        }

        const FVectorDouble value{
            static_cast<double>(f.X),
            static_cast<double>(f.Y),
            static_cast<double>(f.Z)
        };

        if (!IsPlausibleWorldVector(value))
            continue;

        const double distSq =
            VectorDistanceSq(value, current);

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
                    FFreezePositionCandidate{ off, true, distSq, value };
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
        const FFreezePositionCandidate& c = candidates[i];

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
        ShipLog(
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

static bool RefreshBlockedFreezeStartLocation(
    void* status,
    FBlockedFreezeRuntime* ioState)
{
    if (!status || !ioState || !ioState->RootComponent)
        return false;

    FVectorDouble current{};
    if (!ReadRootWorldLocation(
        ioState->RootComponent,
        &current)) {
        return false;
    }

    if (ioState->StartLocationState == 0) {
        ptrdiff_t locatedOffset = 0;
        bool usesFloat = false;
        bool discoveredNow = false;

        AcquireSRWLockExclusive(
            &g_FreezeStartLocationLocatorLock);

        if (g_FreezeStartLocationOffset == 0) {
            if (!LocateFreezeStartLocation(
                status,
                current,
                &locatedOffset,
                &usesFloat)) {
                ReleaseSRWLockExclusive(
                    &g_FreezeStartLocationLocatorLock);

                ioState->StartLocationState = -1;
                SetBlockedFreezeStartLocationState(
                    status, -1, false);
                return false;
            }

            g_FreezeStartLocationOffset =
                locatedOffset;
            g_FreezeStartLocationUsesFloat =
                usesFloat;
            discoveredNow = true;
        }

        locatedOffset =
            g_FreezeStartLocationOffset;
        usesFloat =
            g_FreezeStartLocationUsesFloat;

        ReleaseSRWLockExclusive(
            &g_FreezeStartLocationLocatorLock);

        if (discoveredNow)
            MarkRuntimeSnapshotDirty();

        // Per-instance re-validation of the cached class offset.
        FVectorDouble candidate{};
        bool readOk = false;

        __try {
            if (usesFloat) {
                const FVectorFloat f =
                    *reinterpret_cast<const FVectorFloat*>(
                        reinterpret_cast<uintptr_t>(status) +
                        locatedOffset);

                candidate = FVectorDouble{
                    static_cast<double>(f.X),
                    static_cast<double>(f.Y),
                    static_cast<double>(f.Z)
                };
            }
            else {
                candidate =
                    *reinterpret_cast<const FVectorDouble*>(
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
            ? VectorDistanceSq(candidate, current)
            : 1.0e30;

        if (!readOk ||
            !IsPlausibleWorldVector(candidate) ||
            distSq > 40000.0) {
            ShipLog(
                "[ElementalSystemExpanded] WARNING: STARTLOCATION REVALIDATE FAILED: "
                "Instance=%p Offset=+0x%llX Format=%s DistSq=%.3f; "
                "NO WRITE.\n",
                status,
                static_cast<unsigned long long>(locatedOffset),
                usesFloat ? "float3" : "double3",
                distSq);

            ioState->StartLocationState = -1;
            SetBlockedFreezeStartLocationState(
                status, -1, false);
            return false;
        }

        ioState->StartLocationState = 1;
        SetBlockedFreezeStartLocationState(
            status, 1, false);
    }

    if (ioState->StartLocationState != 1)
        return false;

    ptrdiff_t offset = 0;
    bool usesFloat = false;
    AcquireSRWLockShared(&g_FreezeStartLocationLocatorLock);
    offset = g_FreezeStartLocationOffset;
    usesFloat = g_FreezeStartLocationUsesFloat;
    ReleaseSRWLockShared(&g_FreezeStartLocationLocatorLock);

    // A validated instance must never write through an absent class locator.
    if (offset <= 0) {
        ioState->StartLocationState = -1;
        SetBlockedFreezeStartLocationState(status, -1, false);
        ShipLog(
            "[ElementalSystemExpanded] WARNING: STARTLOCATION REFRESH ERROR: "
            "validated instance has no active locator; refresh disabled for Instance=%p.\n",
            status);
        return false;
    }

    __try {
        if (usesFloat) {
            *reinterpret_cast<FVectorFloat*>(
                reinterpret_cast<uintptr_t>(status) + offset) =
                FVectorFloat{
                    static_cast<float>(current.X),
                    static_cast<float>(current.Y),
                    static_cast<float>(current.Z)
            };
        }
        else {
            *reinterpret_cast<FVectorDouble*>(
                reinterpret_cast<uintptr_t>(status) + offset) =
                current;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        ioState->StartLocationState = -1;
        SetBlockedFreezeStartLocationState(
            status, -1, false);

        ShipLog(
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
        SetBlockedFreezeStartLocationState(
            status, 1, true);
    }

    return true;
}

static bool IsExecutableAddressPtr(void* p)
{
    return p && IsExecutableAddress(
        reinterpret_cast<uintptr_t>(p));
}

static void __fastcall Detour_SetComponentTickEnabledVirtual(
    void* actorComponent,
    bool enabled)
{
    if (IsBlockedReactionScope() &&
        g_ReactionTLS.StatusID == ActiveIds::Status_Freeze &&
        !enabled) {
        ModLog(
            "[ElementalSystemExpanded] REACTION BLOCK: Freeze "
            "SetComponentTickEnabled(false) Component=%p\n",
            actorComponent);
        return;
    }

    if (Original_SetComponentTickEnabledVirtual)
        Original_SetComponentTickEnabledVirtual(
            actorComponent, enabled);
}

static void __fastcall Detour_StopAnimMontageVirtual(
    void* character,
    void* montage)
{
    if (IsBlockedReactionScope() &&
        g_ReactionTLS.StatusID == ActiveIds::Status_Freeze) {

        void* targetCharacter =
            ResolveOwningPalCharacter(
                g_ReactionTLS.DamageReaction);

        if (targetCharacter == character) {
            ModLog(
                "[ElementalSystemExpanded] REACTION BLOCK: Freeze "
                "StopAnimMontage Character=%p Montage=%p\n",
                character,
                montage);
            return;
        }
    }

    if (Original_StopAnimMontageVirtual)
        Original_StopAnimMontageVirtual(
            character, montage);
}

static void __fastcall Detour_FreezeTickVirtual(
    void* status,
    float deltaTime)
{
    FBlockedFreezeRuntime state{};

    if (!GetBlockedFreezeRuntime(status, &state)) {
        if (Original_FreezeTickVirtual)
            Original_FreezeTickVirtual(
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

        MarkBlockedFreezeTickObserved(status);
    }

    // Stage 6.7.4.29 authority localization: sample the authoritative
    // PalServer root across the first few SOFT Freeze ticks. This directly
    // distinguishes a server-owned stationary actor from a client-only pin.
    FVectorDouble authorityRoot{};
    const bool authorityRootOk =
        ReadRootWorldLocation(
            state.RootComponent,
            &authorityRoot);

    double authorityDeltaFromFirstSq = -1.0;
    const uint32_t authoritySequence =
        authorityRootOk
        ? ReserveAuthorityProbeTick(
            status,
            authorityRoot,
            &authorityDeltaFromFirstSq)
        : 0;

    if (authoritySequence != 0) {
        void* aiActionFreeze = nullptr;
        const bool aiActionRead =
            ReadFreezeAIActionPointer(
                status,
                &aiActionFreeze);

        ModLog(
            "[ESE-AUTH] SERVER SOFT TICK #%u Instance=%p Character=%p "
            "Root=(%.6f, %.6f, %.6f) DeltaFromFirstSq=%.9f "
            "AIActionRead=%s AIActionFreeze=%p\n",
            static_cast<unsigned>(authoritySequence),
            status,
            state.Character,
            authorityRoot.X,
            authorityRoot.Y,
            authorityRoot.Z,
            authorityDeltaFromFirstSq,
            aiActionRead ? "YES" : "NO",
            aiActionFreeze);
    }

    // Keep the complete original TickStatus/lifecycle; if the
    // inferred BP StartLocation field validates against the real root position,
    // refresh it to the Pal's current position immediately before each tick.
    RefreshBlockedFreezeStartLocation(
        status,
        &state);

    const FFreezeTickTLS previous =
        g_FreezeTickTLS;

    g_FreezeTickTLS.Active = true;
    g_FreezeTickTLS.Status = status;

    // The original TickStatus call MUST execute for proper duration and
    // cleanup. We no longer skip the Freeze tick.
    if (Original_FreezeTickVirtual)
        Original_FreezeTickVirtual(
            status, deltaTime);

    PruneEndedBlockedFreezeRuntime(status);
    g_FreezeTickTLS = previous;
}

static bool InstallPreAddFreezeVirtualHooks(
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
            componentVtable[g_VSlot_SetComponentTickEnabled / sizeof(void*)];

        // Character::StopAnimMontage wrapper:
        //   call [character vtable + 0x888]
        stopMontageTarget =
            characterVtable[g_VSlot_StopAnimMontage / sizeof(void*)];
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }

    if (!IsExecutableAddressPtr(tickEnabledTarget) ||
        !IsExecutableAddressPtr(stopMontageTarget)) {
        ShipLog(
            "[ElementalSystemExpanded] ERROR: pre-AddStatus Freeze virtual "
            "target invalid. ComponentTick=%p StopMontage=%p\n",
            tickEnabledTarget,
            stopMontageTarget);
        return false;
    }

    AcquireSRWLockExclusive(
        &g_FreezeDynamicHookLock);

    FDynamicHookTransactionSpec hooks[] = {
        {
            "ActorComponent::SetComponentTickEnabled",
            tickEnabledTarget,
            reinterpret_cast<void*>(&Detour_SetComponentTickEnabledVirtual),
            reinterpret_cast<void**>(&Original_SetComponentTickEnabledVirtual),
            &g_SetComponentTickEnabledVirtualTarget,
            g_VSlot_SetComponentTickEnabled
        },
        {
            "Character::StopAnimMontage",
            stopMontageTarget,
            reinterpret_cast<void*>(&Detour_StopAnimMontageVirtual),
            reinterpret_cast<void**>(&Original_StopAnimMontageVirtual),
            &g_StopAnimMontageVirtualTarget,
            g_VSlot_StopAnimMontage
        }
    };

    const bool ok = EnsureDynamicHookTransaction(
        "FreezePreAddVirtuals",
        hooks,
        sizeof(hooks) / sizeof(hooks[0]));

    ReleaseSRWLockExclusive(
        &g_FreezeDynamicHookLock);

    return ok;
}

static bool InstallDynamicFreezeVirtualHooks(
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
            characterVtable[g_VSlot_StopAnimMontage / sizeof(void*)];

        freezeTickTarget =
            statusVtable[g_VSlot_StatusTick / sizeof(void*)];

        // AActor::RootComponent from the active generated/validated layout.
        rootComponent =
            *reinterpret_cast<void**>(
                reinterpret_cast<uintptr_t>(character) + ActiveLayout::Character_RootComponent);

    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }

    size_t rootReadableSpan =
        static_cast<size_t>(ActiveLayout::SceneComponent_RelativeLocation + sizeof(FVectorDouble));
#if ESE_HAS_GENERATED_SCENE_RELATIVE_LOCATION
    if (ActiveLayout::HasGeneratedSceneAttachParent) {
        rootReadableSpan = (std::max)(
            rootReadableSpan,
            static_cast<size_t>(
                ActiveLayout::SceneComponent_AttachParent + sizeof(uintptr_t)));
    }
#endif

    if (!IsExecutableAddressPtr(stopMontageTarget) ||
        !IsExecutableAddressPtr(freezeTickTarget) ||
        !ValidateUObjectLikePointer(rootComponent, rootReadableSpan)) {
        ShipLog(
            "[ElementalSystemExpanded] ERROR: dynamic Freeze target "
            "invalid. StopMontage=%p Tick=%p Root=%p\n",
            stopMontageTarget,
            freezeTickTarget,
            rootComponent);
        return false;
    }

    AcquireSRWLockExclusive(
        &g_FreezeDynamicHookLock);

    FDynamicHookTransactionSpec hooks[] = {
        {
            "Character::StopAnimMontage",
            stopMontageTarget,
            reinterpret_cast<void*>(&Detour_StopAnimMontageVirtual),
            reinterpret_cast<void**>(&Original_StopAnimMontageVirtual),
            &g_StopAnimMontageVirtualTarget,
            g_VSlot_StopAnimMontage
        },
        {
            "Freeze TickStatus",
            freezeTickTarget,
            reinterpret_cast<void*>(&Detour_FreezeTickVirtual),
            reinterpret_cast<void**>(&Original_FreezeTickVirtual),
            &g_FreezeTickVirtualTarget,
            g_VSlot_StatusTick
        }
    };

    const bool ok = EnsureDynamicHookTransaction(
        "FreezePersistentVirtuals",
        hooks,
        sizeof(hooks) / sizeof(hooks[0]));

    ReleaseSRWLockExclusive(
        &g_FreezeDynamicHookLock);

    if (!ok)
        return false;

    AcquireSRWLockExclusive(
        &g_BlockedFreezeRuntimeLock);

    g_BlockedFreezeRuntime[freezeStatus] =
        FBlockedFreezeRuntime{
            character,
            rootComponent,
            false,
            0,
            false
    };

    ReleaseSRWLockExclusive(
        &g_BlockedFreezeRuntimeLock);

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

static constexpr uint8_t kIceConditionVisualEffectID = ActiveIds::VisualEffect_IceCondition;

static void* ResolveVisualEffectComponent(void* character)
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

static int CountVisualEffectID(
    void* visualEffectComponent,
    uint8_t wantedID,
    bool logEntries)
{
    if (!visualEffectComponent)
        return 0;

    FRawTArray list{};

    __try {
        list = *reinterpret_cast<FRawTArray*>(
            reinterpret_cast<uintptr_t>(visualEffectComponent) + ActiveLayout::VisualEffectComponent_ExecutionVisualEffects);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }

    if (!ValidateRawPointerArray(list, 1024, 4096)) {
        if (g_DebugDiagnosticsEnabled) {
            InterlockedIncrement(&g_VfxArrayValidationFailures);
            MarkRuntimeSnapshotDirty();
        }
        ShipLog(
            "[ElementalSystemExpanded] WARNING: VisualEffect array ABI/layout invalid. "
            "Component=%p Data=%p Num=%d Max=%d\n",
            visualEffectComponent,
            reinterpret_cast<void*>(list.Data),
            list.Num,
            list.Max);
        return 0;
    }

    if (g_DebugDiagnosticsEnabled)
        InterlockedIncrement(&g_VfxArrayValidationPasses);

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

            if (!ValidateUObjectLikePointer(
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

static bool RemoveBlockedFreezeIceCondition(
    void* character)
{
    if (!character) {
        ModLog(
            "[ElementalSystemExpanded] ICE VFX: no owning character; "
            "cannot inspect IceCondition.\n");
        return false;
    }

    void* visualEffectComponent =
        ResolveVisualEffectComponent(character);

    if (!visualEffectComponent) {
        ModLog(
            "[ElementalSystemExpanded] ICE VFX: Character=%p has no "
            "VisualEffectComponent at active offset +0x%llX.\n",
            character,
            static_cast<unsigned long long>(
                ActiveLayout::Character_VisualEffectComponent));
        return false;
    }

    const int before = CountVisualEffectID(
        visualEffectComponent,
        kIceConditionVisualEffectID,
        true);

    ModLog(
        "[ElementalSystemExpanded] ICE VFX BEFORE: Character=%p "
        "Component=%p IceCondition(ID=%u) ActiveCount=%d\n",
        character,
        visualEffectComponent,
        static_cast<unsigned>(kIceConditionVisualEffectID),
        before);

    if (before <= 0) {
        ModLog(
            "[ElementalSystemExpanded] ICE VFX NOTE: no active ID 17 was "
            "present immediately after blocked Freeze AddStatus.\n");
        return false;
    }

    if (!Native_RemoveVisualEffect_Local) {
        ShipLog(
            "[ElementalSystemExpanded] ERROR: native "
            "RemoveVisualEffect_Local is unavailable.\n");
        return false;
    }

    Native_RemoveVisualEffect_Local(
        visualEffectComponent,
        kIceConditionVisualEffectID);

    const int after = CountVisualEffectID(
        visualEffectComponent,
        kIceConditionVisualEffectID,
        false);

    ModLog(
        "[ElementalSystemExpanded] ICE VFX REMOVE: "
        "RemoveVisualEffect_Local(ID=%u) called. "
        "Component=%p ActiveCount %d->%d\n",
        static_cast<unsigned>(kIceConditionVisualEffectID),
        visualEffectComponent,
        before,
        after);

    return after < before;
}

static constexpr uint8_t kDarkConditionVisualEffectID = 21;
static constexpr uint8_t kCameraVignetteVisualEffectID = 24;
static constexpr uint8_t kLightVisualEffectLookupID = 57;
static constexpr uint8_t kLightCameraVisualEffectLookupID = 58;

static bool SafeWriteVisualEffectID(
    void* visualEffect,
    uint8_t visualEffectID)
{
    if (!visualEffect)
        return false;

    const size_t requiredSpan =
        static_cast<size_t>(
            ActiveLayout::VisualEffectBase_ID + sizeof(uint8_t));

    if (!ValidateUObjectLikePointer(
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

static void* AddVisualEffectWithLightSubstitution(
    const char* pathName,
    NativeAddVisualEffect_t original,
    void* visualEffectComponent,
    uint8_t visualEffectID,
    const void* dynamicParameter)
{
    if (!original)
        return nullptr;

    const EBlindnessSource source =
        g_BlindnessSourceTLS;

    // CameraVignette is created through AddVisualEffect_Local by
    // BP_Status_Darkness. For Light provenance, redirect class lookup to the
    // custom white camera VFX registered by Lua under lookup ID 58, then
    // restore runtime identity to CameraVignette (24) for vanilla teardown.
    if (visualEffectID == kCameraVignetteVisualEffectID) {
        ModLog(
            "[ElementalSystemExpanded] CAMERA VIGNETTE PATH: "
            "Path=%s Component=%p ID=%u Source=%s Parameter=%p\n",
            pathName ? pathName : "<unknown>",
            visualEffectComponent,
            static_cast<unsigned>(visualEffectID),
            BlindnessSourceName(source),
            dynamicParameter);

        if (source != EBlindnessSource::Light) {
            return original(
                visualEffectComponent,
                visualEffectID,
                dynamicParameter);
        }

        // End any previous CameraVignette through Palworld's own cleanup
        // before constructing the Light replacement.
        if (Native_RemoveVisualEffect_Local) {
            Native_RemoveVisualEffect_Local(
                visualEffectComponent,
                kCameraVignetteVisualEffectID);
        }

        void* lightCameraEffect = original(
            visualEffectComponent,
            kLightCameraVisualEffectLookupID,
            dynamicParameter);

        if (lightCameraEffect &&
            SafeWriteVisualEffectID(
                lightCameraEffect,
                kCameraVignetteVisualEffectID)) {
            ModLog(
                "[ElementalSystemExpanded] LIGHT CAMERA VFX SUBSTITUTE: "
                "Path=%s Component=%p LookupID=%u->RuntimeID=%u "
                "Effect=%p Source=LIGHT Result=OK\n",
                pathName ? pathName : "<unknown>",
                visualEffectComponent,
                static_cast<unsigned>(
                    kLightCameraVisualEffectLookupID),
                static_cast<unsigned>(
                    kCameraVignetteVisualEffectID),
                lightCameraEffect);
            return lightCameraEffect;
        }

        // Fail closed. Remove a partially-created lookup-ID effect if possible,
        // then fall back to vanilla CameraVignette ID 24.
        if (lightCameraEffect && Native_RemoveVisualEffect_Local) {
            Native_RemoveVisualEffect_Local(
                visualEffectComponent,
                kLightCameraVisualEffectLookupID);
        }

        ShipLog(
            "[ElementalSystemExpanded] WARNING: LIGHT CAMERA VFX SUBSTITUTE failed. "
            "Path=%s Component=%p LookupID=%u Effect=%p; "
            "falling back to vanilla CameraVignette ID=%u.\n",
            pathName ? pathName : "<unknown>",
            visualEffectComponent,
            static_cast<unsigned>(
                kLightCameraVisualEffectLookupID),
            lightCameraEffect,
            static_cast<unsigned>(
                kCameraVignetteVisualEffectID));

        return original(
            visualEffectComponent,
            kCameraVignetteVisualEffectID,
            dynamicParameter);
    }

    // Dark and all unrelated visual effects remain completely vanilla.
    if (visualEffectID != kDarkConditionVisualEffectID ||
        source != EBlindnessSource::Light) {
        return original(
            visualEffectComponent,
            visualEffectID,
            dynamicParameter);
    }

    // End any previous DarkCondition through Palworld's own cleanup before
    // constructing the Light replacement.
    if (Native_RemoveVisualEffect_Local) {
        Native_RemoveVisualEffect_Local(
            visualEffectComponent,
            kDarkConditionVisualEffectID);
    }

    // Lua registers BP_VisualEffect_Status_Light_C in
    // UPalVisualEffectDataBase::VisualEffectClassDataAsset under lookup ID 57.
    // Use that ID only for class selection/creation.
    void* lightEffect = original(
        visualEffectComponent,
        kLightVisualEffectLookupID,
        dynamicParameter);

    // Restore the runtime identity to DarkCondition so vanilla status teardown
    // and visual-effect cleanup continue to operate on ID 21.
    if (lightEffect &&
        SafeWriteVisualEffectID(
            lightEffect,
            kDarkConditionVisualEffectID)) {
        ModLog(
            "[ElementalSystemExpanded] LIGHT VFX SUBSTITUTE: "
            "Path=%s Component=%p LookupID=%u->RuntimeID=%u "
            "Effect=%p Source=LIGHT Result=OK\n",
            pathName ? pathName : "<unknown>",
            visualEffectComponent,
            static_cast<unsigned>(
                kLightVisualEffectLookupID),
            static_cast<unsigned>(
                kDarkConditionVisualEffectID),
            lightEffect);
        return lightEffect;
    }

    // Fail closed. Remove a partially-created lookup-ID effect if possible,
    // then fall back to the unmodified vanilla DarkCondition path.
    if (lightEffect && Native_RemoveVisualEffect_Local) {
        Native_RemoveVisualEffect_Local(
            visualEffectComponent,
            kLightVisualEffectLookupID);
    }

    ShipLog(
        "[ElementalSystemExpanded] WARNING: LIGHT VFX SUBSTITUTE failed. "
        "Path=%s Component=%p LookupID=%u Effect=%p; "
        "falling back to vanilla DarkCondition ID=%u.\n",
        pathName ? pathName : "<unknown>",
        visualEffectComponent,
        static_cast<unsigned>(
            kLightVisualEffectLookupID),
        lightEffect,
        static_cast<unsigned>(
            kDarkConditionVisualEffectID));

    return original(
        visualEffectComponent,
        kDarkConditionVisualEffectID,
        dynamicParameter);
}

static void* __fastcall Detour_AddVisualEffect(
    void* visualEffectComponent,
    uint8_t visualEffectID,
    const void* dynamicParameter)
{
    return AddVisualEffectWithLightSubstitution(
        "AddVisualEffect",
        Original_AddVisualEffect,
        visualEffectComponent,
        visualEffectID,
        dynamicParameter);
}

static void* __fastcall Detour_AddVisualEffect_Local(
    void* visualEffectComponent,
    uint8_t visualEffectID,
    const void* dynamicParameter)
{
    return AddVisualEffectWithLightSubstitution(
        "AddVisualEffect_Local",
        Original_AddVisualEffect_Local,
        visualEffectComponent,
        visualEffectID,
        dynamicParameter);
}

static EBlindnessSource ResolveBlindnessSourceFromElement(
    uint8_t sourceElement)
{
    // sourceElement was resolved from the raw FPalDamageInfo while that frame
    // was unquestionably live, before ICD/exchange/status construction. Do not
    // re-read ambient raw-damage TLS later inside AddStatus.
    if (sourceElement == ActiveIds::Element_Normal)
        return EBlindnessSource::Light;
    if (sourceElement == ActiveIds::Element_Dark)
        return EBlindnessSource::Dark;
    return EBlindnessSource::Unknown;
}

static void __fastcall Detour_NativeAddStatusParameterProbe(
    void* statusComponent,
    uint8_t statusID,
    const FStatusDynamicParameterProbe* param)
{
    if (!Original_NativeAddStatusParameterProbe)
        return;

    // Preserve the complete native AddStatus path. Stamp only Palworld's existing
    // generic replicated status parameter while ESE still has authoritative
    // semantic provenance. No custom packet/RPC/replicated structure is added.
    if (param && IsReadableAddress(
            reinterpret_cast<uintptr_t>(param),
            sizeof(FStatusDynamicParameterProbe))) {
        __try {
            auto* mutableParam = const_cast<FStatusDynamicParameterProbe*>(param);

            if (statusID == ActiveIds::Status_Freeze &&
                g_ReactionTLS.Active &&
                g_ReactionTLS.StatusID == ActiveIds::Status_Freeze) {
                // Low byte is authoritative ESE Freeze mode:
                //   1 = SOFT  (dry Freeze, or Wet Freeze while strong-reaction ICD is blocked)
                //   2 = STRONG (Wet -> Freeze with authoritative strong-reaction ICD ready)
                const uint8_t transportMode =
                    g_ReactionTLS.AllowStrongControl ? 2u : 1u;
                mutableParam->GeneralIndex =
                    0x45534500 | static_cast<int32_t>(transportMode);
                ModLog(
                    "[ESE-MP] SERVER FREEZE MODE: Component=%p StatusID=%u "
                    "Param=%p GeneralIndex=0x%08X Mode=%s GeneralFloat=%.3f Wet=%s Control=%s\n",
                    statusComponent,
                    static_cast<unsigned>(statusID),
                    param,
                    static_cast<unsigned>(mutableParam->GeneralIndex),
                    transportMode == 2u ? "STRONG" : "SOFT",
                    mutableParam->GeneralFloatValue,
                    g_ReactionTLS.HadWetness ? "YES" : "NO",
                    g_ReactionTLS.AllowStrongControl ? "ALLOW" : "BLOCK");
            }
            else if (statusID == ActiveIds::Status_Darkness &&
                     g_BlindnessTransportTLS.Active &&
                     g_BlindnessTransportTLS.StatusComponent == statusComponent &&
                     (g_BlindnessTransportTLS.Source == EBlindnessSource::Light ||
                      g_BlindnessTransportTLS.Source == EBlindnessSource::Dark)) {
                // StatusID 25 is intentionally shared by genuine Dark and ESE Light.
                // Stamp only this exact authoritative AddStatus invocation. The
                // source came from the stable sourceElement resolved before entering
                // status construction; no ambient/raw-context re-read is used here.
                // 0x45534C01 = LIGHT, 0x45534C02 = DARK.
                const uint8_t mode =
                    g_BlindnessTransportTLS.Source == EBlindnessSource::Light
                    ? 1u : 2u;
                mutableParam->GeneralIndex =
                    0x45534C00 | static_cast<int32_t>(mode);
                ModLog(
                    "[ESE-LIGHT30] SERVER BLINDNESS MODE: Component=%p StatusID=%u "
                    "Param=%p GeneralIndex=0x%08X Mode=%s GeneralFloat=%.3f\n",
                    statusComponent,
                    static_cast<unsigned>(statusID),
                    param,
                    static_cast<unsigned>(mutableParam->GeneralIndex),
                    mode == 1u ? "LIGHT" : "DARK",
                    mutableParam->GeneralFloatValue);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            ShipLog(
                "[ElementalSystemExpanded] WARNING: Stage 6.7.4.30 could not stamp "
                "status DynamicParameter marker safely. Component=%p StatusID=%u Param=%p\n",
                statusComponent,
                static_cast<unsigned>(statusID),
                param);
        }
    }

    Original_NativeAddStatusParameterProbe(
        statusComponent,
        statusID,
        param);
}

static bool ApplyStatusWithDuration(
    void* damageReaction,
    void* statusComponent,
    uint8_t statusID,
    uint8_t units,
    uint8_t sourceElement,
    float durationOverride,
    bool forceExactDuration = false)
{
    if (!Native_AddStatus || !Original_NativeAddStatus) {
        ShipLog(
            "[ElementalSystemExpanded] ERROR: Native AddStatus unavailable; "
            "StatusID=%u not applied.\n",
            static_cast<unsigned>(statusID));
        return false;
    }

    bool blockedFreezePersistent = false;
    bool freezeHadWetness = false;
    void* reactionCharacter = nullptr;

    {
        FReactionTLSGuard reactionGuard(
            damageReaction,
            statusComponent,
            statusID);

        freezeHadWetness =
            g_ReactionTLS.Active &&
            g_ReactionTLS.StatusID == ActiveIds::Status_Freeze &&
            g_ReactionTLS.HadWetness;

        blockedFreezePersistent =
            g_ReactionTLS.Active &&
            g_ReactionTLS.StatusID == ActiveIds::Status_Freeze &&
            !g_ReactionTLS.AllowStrongControl;

        if (blockedFreezePersistent) {
            reactionCharacter =
                ResolveOwningPalCharacter(
                    damageReaction);

            if (!InstallPreAddFreezeVirtualHooks(
                statusComponent,
                reactionCharacter)) {
                ShipLog(
                    "[ElementalSystemExpanded] WARNING: pre-AddStatus Freeze "
                    "virtual hooks were not fully installed. "
                    "StatusComponent=%p Character=%p\n",
                    statusComponent,
                    reactionCharacter);
            }
        }

        const EBlindnessSource blindnessSource =
            (statusID == ActiveIds::Status_Darkness)
            ? ResolveBlindnessSourceFromElement(sourceElement)
            : EBlindnessSource::Unknown;

        // Presentation and native status-parameter transport deliberately share
        // the same already-resolved semantic source, but use separate scopes.
        FBlindnessSourceScope blindnessScope(blindnessSource);
        FBlindnessTransportScope blindnessTransportScope(
            statusID == ActiveIds::Status_Darkness ? statusComponent : nullptr,
            blindnessSource);
        Native_AddStatus(statusComponent, statusID);

        if (reactionGuard.Previous.Active == false &&
            g_ReactionTLS.Active &&
            g_ReactionTLS.AllowStrongControl &&
            !g_ReactionTLS.ReactionRecorded) {
            ModLog(
                "[ElementalSystemExpanded] REACTION NOTE: %s status completed "
                "without reaching a native action gate; reaction ICD was not consumed.\n",
                StrongReactionName(g_ReactionTLS.Reaction));
        }
    }

    void* status = nullptr;
    float oldDuration = 0.0f;
    float oldTimer = 0.0f;

    if (!FindStatusInstance(
        statusComponent, statusID, &status, &oldDuration, &oldTimer)) {
        ShipLog(
            "[ElementalSystemExpanded] ERROR: AddStatus returned but no status "
            "instance was found. StatusID=%u.\n",
            static_cast<unsigned>(statusID));
        return false;
    }

    if (statusID == ActiveIds::Status_Freeze) {
        if (blockedFreezePersistent) {
            if (!InstallDynamicFreezeVirtualHooks(
                reactionCharacter,
                status)) {
                ShipLog(
                    "[ElementalSystemExpanded] ERROR: Freeze persistent "
                    "native gate installation failed. Instance=%p Character=%p\n",
                    status,
                    reactionCharacter);
            }
        }
        else {
            RemoveBlockedFreezeRuntime(status);
        }
    }

    float desiredDuration = 0.0f;
    const float statusCap = MaxDurationForStatus(statusID);

    if (forceExactDuration &&
        std::isfinite(durationOverride) &&
        durationOverride > 0.0f) {
        // Dragon reaction spread owns an explicit duration independent of the
        // incoming Waza's 1U/2U classification. This intentionally takes
        // precedence over Wet->Freeze's ordinary 2 s status duration while the
        // existing native strong-control gate still executes above.
        desiredDuration = (std::min)(statusCap, durationOverride);
    }
    else if (statusID == ActiveIds::Status_Freeze && freezeHadWetness) {
        // Ordinary Wet -> Freeze is a reaction, not an exchange carryover.
        desiredDuration = kWetFreezeDuration;
    }
    else if (std::isfinite(durationOverride) && durationOverride > 0.0f) {
        // Cross-element carryover may contain more exchange budget than the
        // incoming status is allowed to retain. Darkness/Light are therefore
        // clamped to 3 seconds while ordinary statuses cap at 12.
        desiredDuration = (std::min)(statusCap, durationOverride);
    }
    else {
        // Initial application uses the gauge budget, then applies the per-status
        // cap. Darkness/Light blindness therefore remain 3 seconds maximum for
        // both 1U and 2U hits.
        desiredDuration = (std::min)(
            statusCap,
            GaugeDurationForUnits(units));
    }

    const bool changed = SetStatusDuration(status, desiredDuration);
    if (changed) {
        RememberElementalStatusProvenance(
            statusComponent,
            status,
            statusID,
            sourceElement);
    }

    if (statusID == ActiveIds::Status_Freeze &&
        blockedFreezePersistent) {
        const bool removedIce =
            RemoveBlockedFreezeIceCondition(
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
        "Instance=%p Duration %.3f->%.3f Timer %.3f->%.3f Result=%s%s%s\n",
        static_cast<unsigned>(statusID),
        StatusIDName(statusID),
        static_cast<unsigned>(units),
        status,
        oldDuration,
        desiredDuration,
        oldTimer,
        desiredDuration,
        changed ? "OK" : "FAILED",
        (changed &&
         (statusID == ActiveIds::Status_Burn ||
          statusID == ActiveIds::Status_Wetness))
            ? " BP_TIMER_SYNCED"
            : "",
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
static void __fastcall Detour_AddElementStatusAdditionalValue_OneType(
    void* damageReaction,
    uint8_t effect,
    float vanillaValue)
{
    // All non-elemental effects remain completely vanilla.
    if (!IsTrackedElementalEffect(effect)) {
        if (Original_AddElementStatusAdditionalValue_OneType) {
            Original_AddElementStatusAdditionalValue_OneType(
                damageReaction, effect, vanillaValue);
        }
        return;
    }

    // A successful Dragon reaction spends the triggering Dragon Burn aggregate.
    // Do not rely only on mutating FPalDamageInfo: the native caller may already
    // have cached its effect byte before our raw wrapper writes the marker.
    // The synchronous raw-context gate is authoritative for this one call.
    if (ShouldSuppressDragonBurn(effect)) {
        ModLog(
            "[ElementalSystemExpanded] DRAGON BURN SUPPRESS: "
            "DamageReaction=%p Effect=%u (%s) discarded for prepared reaction.\n",
            damageReaction,
            static_cast<unsigned>(effect),
            ElementalEffectName(effect));
        return;
    }

    const uint8_t statusID = ElementalEffectToStatusID(effect);
    if (!statusID) {
        if (Original_AddElementStatusAdditionalValue_OneType) {
            Original_AddElementStatusAdditionalValue_OneType(
                damageReaction, effect, vanillaValue);
        }
        return;
    }

    void* character = ResolveOwningPalCharacter(damageReaction);
    void* statusComponent = ResolveStatusComponent(damageReaction);

    // PalServer reaction support is capability-split:
    //
    // Electrical needs only the wrapper-backed native action/movement gates.
    // Wet->Freeze with a ready reaction ICD also needs only those native gates
    // because strong vanilla Freeze is intentionally allowed.
    //
    // Dry Freeze (including a Wet hit whose 14s strong-reaction ICD is still
    // active) additionally needs the three exact Freeze virtual gates. Without
    // them we refuse the application rather than risk hard immobilization.
    const bool nativeReactionReady =
        InterlockedCompareExchange(
            &g_ReactionCapabilityReady, 0, 0) != 0;

    const bool dryFreezeVirtualReady =
        InterlockedCompareExchange(
            &g_DryFreezeVirtualCapabilityReady, 0, 0) != 0;

    if (effect == ActiveIds::Effect_Electrical &&
        !nativeReactionReady) {
        ModLog(
            "[ElementalSystemExpanded] PALSERVER REACTION FAIL-CLOSED: "
            "DamageReaction=%p Electrical ignored because native reaction gates "
            "are not validated.\n",
            damageReaction);
        return;
    }

    if (effect == ActiveIds::Effect_Freeze) {
        bool strongWetFreezeReady = false;
        double reactionElapsed = -1.0;

        if (statusComponent && nativeReactionReady &&
            HasActiveStatus(
                statusComponent,
                ActiveIds::Status_Wetness)) {
            strongWetFreezeReady = ReactionICDReady(
                damageReaction,
                EStrongReaction::Freeze,
                &reactionElapsed);
        }

        if (!nativeReactionReady ||
            (!strongWetFreezeReady && !dryFreezeVirtualReady)) {
            ModLog(
                "[ElementalSystemExpanded] PALSERVER DRY FREEZE FAIL-CLOSED: "
                "DamageReaction=%p NativeGates=%s DryVirtuals=%s "
                "WetStrongReady=%s ReactionElapsed=%.3f -> Freeze ignored.\n",
                damageReaction,
                nativeReactionReady ? "YES" : "NO",
                dryFreezeVirtualReady ? "YES" : "NO",
                strongWetFreezeReady ? "YES" : "NO",
                reactionElapsed);
            return;
        }
    }

    if (character &&
        ShouldBlockLightBlindOnNeutralPal(
            character,
            effect)) {
        ModLog(
            "[ElementalSystemExpanded] LIGHT IMMUNITY: "
            "Character=%p has Normal/Light element; "
            "Light-sourced Darkness carrier rejected before ICD/AddStatus.\n",
            character);
        return;
    }

    if (!character || !statusComponent || !Native_AddStatus) {
        ShipLog(
            "[ElementalSystemExpanded] WARNING: CUSTOM FALLBACK: DamageReaction=%p "
            "Effect=%u (%s) VanillaValue=%.6f could not resolve status component; "
            "calling vanilla.\n",
            damageReaction,
            static_cast<unsigned>(effect),
            ElementalEffectName(effect),
            vanillaValue);

        if (Original_AddElementStatusAdditionalValue_OneType) {
            Original_AddElementStatusAdditionalValue_OneType(
                damageReaction, effect, vanillaValue);
        }
        return;
    }

    // Primary source: original integer EffectValue1/2 from FPalDamageInfo.
    // Direct callers outside the verified raw-damage wrapper use a conservative
    // 1U fallback. In particular, sentinel/derived values such as 9999999 must
    // never be interpreted as a real 2U request.
    int32_t rawValue = 1;
    bool rawDragonReactionSpread = false;
    int32_t rawDragonReactionSequence = 0;
    const bool haveRawValue = GetRawUnitsForEffect(
        effect,
        &rawValue,
        &rawDragonReactionSpread,
        &rawDragonReactionSequence);
    const bool isDragonReactionSpread =
        haveRawValue && rawDragonReactionSpread;

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
    if (haveRawValue && !isDragonReactionSpread) {
        units = UnitsFromRawValue(rawValue);
    }
    else if (!haveRawValue) {
        units = 1;
        ShipLog(
            "[ElementalSystemExpanded] WARNING: no raw EffectValue context for "
            "Effect=%u (%s); conservative fallback units=1 from VanillaValue=%.6f.\n",
            static_cast<unsigned>(effect),
            ElementalEffectName(effect),
            vanillaValue);
    }

    const float incomingBudget =
        isDragonReactionSpread
        ? kDragonReactionSpreadDuration
        : GaugeDurationForUnits(units);

    const uint8_t incomingElement = ResolveIncomingElement(effect);

    uint8_t previousBlockedHits = 0;
    double elapsed = 0.0;

    const bool allowed = ElementICDAllows(
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
            ElementalEffectName(effect),
            static_cast<unsigned>(units),
            elapsed,
            static_cast<unsigned>(previousBlockedHits));

        // Do not call vanilla. The elemental buildup registration is discarded.
        return;
    }

    ModLog(
        "[ElementalSystemExpanded] ICD ALLOW: DamageReaction=%p "
        "Effect=%u (%s) RawValue=%d Units=%u Element=%u Spread=%s Seq=%d Elapsed=%.3f\n",
        damageReaction,
        static_cast<unsigned>(effect),
        ElementalEffectName(effect),
        rawValue,
        static_cast<unsigned>(units),
        static_cast<unsigned>(incomingElement),
        isDragonReactionSpread ? "YES" : "NO",
        rawDragonReactionSequence,
        elapsed);

    FCurrentElementalStatus current{};
    const ECurrentElementalStatusQuery currentQuery =
        QueryCurrentElementalStatus(
            statusComponent,
            &current);

    if (currentQuery == ECurrentElementalStatusQuery::Failed) {
        ShipLog(
            "[ElementalSystemExpanded] WARNING: could not safely inspect the current "
            "elemental status; incoming StatusID=%u was discarded rather than risking "
            "an unintended overwrite.\n",
            static_cast<unsigned>(statusID));
        return;
    }

    if (currentQuery == ECurrentElementalStatusQuery::Found) {
        const uint8_t currentElement =
            ResolveCurrentElementSource(
                statusComponent,
                current);

        // Preserve the established elemental reactions. Wet -> Freeze and
        // Wet -> Electrical bypass exchange/erosion completely and flow into
        // the original reaction path below. Wet Freeze therefore remains the
        // fixed 2 s reaction and both strong reactions keep their 14 s ICD.
        const bool wetStrongReaction =
            !isDragonReactionSpread &&
            IsWetStrongReactionPair(current, statusID);

        if (wetStrongReaction) {
            ModLog(
                "[ElementalSystemExpanded] STATUS EXCHANGE BYPASS: Wetness + %s "
                "uses the established strong-reaction path.\n",
                StatusIDName(statusID));
        }
        else if (currentElement == incomingElement) {
            // Ordinary same-element hits extend. Reaction-spread payloads are
            // fixed at exactly 3.0 seconds, so a lingering consumed instance
            // must be reset rather than extended.
            const float addition = incomingBudget;
            const float statusCap =
                MaxDurationForStatus(current.StatusID);
            const float refreshed =
                isDragonReactionSpread
                ? (std::min)(
                    statusCap,
                    kDragonReactionSpreadDuration)
                : (std::min)(
                    statusCap,
                    current.Timer + addition);

            if (!SetStatusDuration(current.Instance, refreshed)) {
                ShipLog(
                    "[ElementalSystemExpanded] WARNING: same-element status "
                    "extension failed. CurrentStatusID=%u IncomingStatusID=%u.\n",
                    static_cast<unsigned>(current.StatusID),
                    static_cast<unsigned>(statusID));
                return;
            }

            RememberElementalStatusProvenance(
                statusComponent,
                current.Instance,
                current.StatusID,
                currentElement);

            ModLog(
                "[ElementalSystemExpanded] STATUS EXTEND: StatusID=%u (%s) "
                "Element=%u Units=%u Addition=%.3f Duration %.3f->%.3f "
                "Timer %.3f->%.3f Cap=%.3f.\n",
                static_cast<unsigned>(current.StatusID),
                StatusIDName(current.StatusID),
                static_cast<unsigned>(currentElement),
                static_cast<unsigned>(units),
                addition,
                current.Duration,
                refreshed,
                current.Timer,
                refreshed,
                statusCap);

            ElementICDRecordSuccess(damageReaction, effect);
            return;
        }
        else {
            const float semanticRemaining =
                (std::max)(0.0f, current.Timer - incomingBudget);
            const float carryover =
                (std::max)(0.0f, incomingBudget - current.Timer);
            float storedRemaining = 0.0f;

            if (!SetStatusRemainingFromErosion(
                    current.Instance,
                    semanticRemaining,
                    &storedRemaining)) {
                ShipLog(
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
                ClearElementalStatusProvenance(statusComponent);
            }

            const bool carryoverApplies =
                carryover >= kMinimumExchangeCarryover;

            ModLog(
                "[ElementalSystemExpanded] STATUS EXCHANGE: CurrentStatusID=%u (%s) "
                "CurrentElement=%u IncomingStatusID=%u (%s) IncomingElement=%u "
                "Units=%u Budget=%.3f Duration %.3f->%.3f Timer %.3f->%.3f "
                "StoredTimer=%.3f Carryover=%.3f Threshold=%.3f ApplyIncoming=%s.\n",
                static_cast<unsigned>(current.StatusID),
                StatusIDName(current.StatusID),
                static_cast<unsigned>(currentElement),
                static_cast<unsigned>(statusID),
                StatusIDName(statusID),
                static_cast<unsigned>(incomingElement),
                static_cast<unsigned>(units),
                incomingBudget,
                current.Duration,
                semanticRemaining,
                current.Timer,
                semanticRemaining,
                storedRemaining,
                carryover,
                kMinimumExchangeCarryover,
                carryoverApplies ? "YES" : "NO");

            if (carryoverApplies) {
                const bool applied =
                    ApplyStatusWithDuration(
                        damageReaction,
                        statusComponent,
                        statusID,
                        units,
                        incomingElement,
                        isDragonReactionSpread
                            ? kDragonReactionSpreadDuration
                            : carryover,
                        isDragonReactionSpread);

                // The hit already successfully eroded the previous status, so
                // it consumes its ICD even if carryover application itself
                // unexpectedly fails.
                ElementICDRecordSuccess(damageReaction, effect);

                if (!applied) {
                    ShipLog(
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
            ElementICDRecordSuccess(damageReaction, effect);
            return;
        }
    }

    const float initialDurationOverride =
        isDragonReactionSpread
        ? kDragonReactionSpreadDuration
        : -1.0f;

    if (ApplyStatusWithDuration(
            damageReaction,
            statusComponent,
            statusID,
            units,
            incomingElement,
            initialDurationOverride,
            isDragonReactionSpread)) {
        ElementICDRecordSuccess(damageReaction, effect);
    }
    else {
        ShipLog(
            "[ElementalSystemExpanded] WARNING: custom StatusID=%u application "
            "failed; ICD state was not marked successful.\n",
            static_cast<unsigned>(statusID));
    }

    // Intentionally do not call Original_AddElementStatusAdditionalValue_OneType
    // for supported elemental effects. Vanilla buildup is completely replaced.
}

// Native AddStatus policy hook. Most statuses pass through unchanged; the two
// obsolete vanilla WetFreeze status IDs are intentionally rejected.
static void __fastcall Detour_NativeAddStatus(
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
            StatusIDName(statusID));
    }

    if (Original_NativeAddStatus) {
        Original_NativeAddStatus(statusComponent, statusID);
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
static void AppendSnapshotFormat(
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

static bool WriteWholeFileDurable(
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

static bool WriteRuntimeSnapshot(const char* reason)
{
    if (!g_DebugDiagnosticsEnabled)
        return false;

    uintptr_t outerOffset = 0;
    bool outerLogged = false;
    AcquireSRWLockShared(&g_UObjectOuterOffsetLock);
    outerOffset = g_RuntimeUObjectOuterOffset;
    outerLogged = g_RuntimeUObjectOuterLogged;
    ReleaseSRWLockShared(&g_UObjectOuterOffsetLock);

    ptrdiff_t startLocationOffset = 0;
    bool startLocationUsesFloat = false;
    AcquireSRWLockShared(&g_FreezeStartLocationLocatorLock);
    startLocationOffset = g_FreezeStartLocationOffset;
    startLocationUsesFloat = g_FreezeStartLocationUsesFloat;
    ReleaseSRWLockShared(&g_FreezeStartLocationLocatorLock);

    bool rootCrossCheckSeen = false;
    bool rootCrossCheckPassed = false;
    double rootCrossCheckDistSq = -1.0;
    bool rootHiddenFallbackUsed = false;
    AcquireSRWLockShared(&g_RootReferenceDiagnosticsLock);
    rootCrossCheckSeen = g_RootReferenceValidationLogged;
    rootCrossCheckPassed = g_RootReferenceCrossCheckPassed;
    rootCrossCheckDistSq = g_RootReferenceCrossCheckDistSq;
    rootHiddenFallbackUsed = g_RootReferenceUsedHiddenFallback;
    ReleaseSRWLockShared(&g_RootReferenceDiagnosticsLock);

    size_t blockedFreezeCount = 0;
    AcquireSRWLockShared(&g_BlockedFreezeRuntimeLock);
    blockedFreezeCount = g_BlockedFreezeRuntime.size();
    ReleaseSRWLockShared(&g_BlockedFreezeRuntimeLock);

    const LONG statusPasses =
        InterlockedCompareExchange(&g_StatusArrayValidationPasses, 0, 0);
    const LONG statusFailures =
        InterlockedCompareExchange(&g_StatusArrayValidationFailures, 0, 0);
    const LONG vfxPasses =
        InterlockedCompareExchange(&g_VfxArrayValidationPasses, 0, 0);
    const LONG vfxFailures =
        InterlockedCompareExchange(&g_VfxArrayValidationFailures, 0, 0);

    const LONG sequence =
        InterlockedIncrement(&g_RuntimeSnapshotWrites);

    SYSTEMTIME utc{};
    GetSystemTime(&utc);

    std::string output;
    output.reserve(4096);

    AppendSnapshotFormat(
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

    AppendSnapshotFormat(
        output,
        "Fingerprint.TimeDateStamp=0x%08lX\n"
        "Fingerprint.SizeOfImage=0x%08lX\n"
        "Fingerprint.TextHash=0x%016llX\n"
        "BuildProfileCache=%s\n",
        static_cast<unsigned long>(g_CurrentFingerprint.TimeDateStamp),
        static_cast<unsigned long>(g_CurrentFingerprint.SizeOfImage),
        static_cast<unsigned long long>(g_CurrentFingerprint.TextHash),
        g_ProfileCacheStatus.c_str());

    AppendSnapshotFormat(
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

    AppendSnapshotFormat(
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
    AppendSnapshotFormat(
        output,
        "RootReference.RelativeLocationOffset=0x%llX\n"
        "RootReference.AttachParentAvailable=%s\n",
        static_cast<unsigned long long>(
            ActiveLayout::SceneComponent_RelativeLocation),
        ActiveLayout::HasGeneratedSceneAttachParent ? "YES" : "NO");
    if (ActiveLayout::HasGeneratedSceneAttachParent) {
        AppendSnapshotFormat(
            output,
            "RootReference.AttachParentOffset=0x%llX\n",
            static_cast<unsigned long long>(
                ActiveLayout::SceneComponent_AttachParent));
    }
#endif

    AppendSnapshotFormat(
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

    AppendSnapshotFormat(
        output,
        "\n[Runtime state]\n"
        "BlockedFreezeEntriesAtSnapshot=%llu\n"
        "ResolvedVSlot.SetComponentTickEnabled=0x%llX\n"
        "ResolvedVSlot.StopAnimMontage=0x%llX\n"
        "ResolvedVSlot.StatusTick=0x%llX\n"
        "SnapshotWriteFailuresBeforeThisAttempt=%ld\n",
        static_cast<unsigned long long>(blockedFreezeCount),
        static_cast<unsigned long long>(
            g_VSlot_SetComponentTickEnabled),
        static_cast<unsigned long long>(
            g_VSlot_StopAnimMontage),
        static_cast<unsigned long long>(
            g_VSlot_StatusTick),
        static_cast<long>(
            InterlockedCompareExchange(
                &g_RuntimeSnapshotWriteFailures, 0, 0)));

    const bool ok = WriteWholeFileDurable(
        ArtifactPathOrFallback(
            g_RuntimeSnapshotPath,
            "ElementalSystemExpanded_runtime_snapshot.txt"),
        output);

    if (!ok) {
        InterlockedIncrement(&g_RuntimeSnapshotWriteFailures);
        return false;
    }

    return true;
}

static void MarkRuntimeSnapshotDirty()
{
    if (!g_DebugDiagnosticsEnabled)
        return;

    InterlockedExchange(&g_RuntimeSnapshotDirty, 1);

    HANDLE wake = g_RuntimeSnapshotWakeEvent;
    if (wake)
        SetEvent(wake);
}

static DWORD WINAPI RuntimeSnapshotWorker(LPVOID)
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

    // Create a baseline snapshot immediately. It will be atomically replaced
    // as soon as deferred runtime evidence appears.
    WriteRuntimeSnapshot("worker-start");
    InterlockedExchange(&g_RuntimeSnapshotDirty, 0);

    HANDLE waits[2] = {
        g_RuntimeSnapshotStopEvent,
        g_RuntimeSnapshotWakeEvent
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
            WriteRuntimeSnapshot("worker-stop");
            return 0;
        }

        const bool explicitlyDirty =
            InterlockedExchange(&g_RuntimeSnapshotDirty, 0) != 0;

        if (wait == WAIT_OBJECT_0 + 1) {
            WriteRuntimeSnapshot(
                explicitlyDirty ? "runtime-milestone" : "worker-wake");
            continue;
        }

        if (wait == WAIT_TIMEOUT) {
            // Periodic persistence captures monotonically changing guard
            // counters even when there has been no discrete milestone.
            WriteRuntimeSnapshot(
                explicitlyDirty ? "periodic-dirty" : "periodic");
            continue;
        }

        // Unexpected wait failure: retry after a short pause instead of
        // terminating the diagnostics thread permanently.
        Sleep(250);
    }
}

static bool StartRuntimeSnapshotWorker()
{
    if (!g_DebugDiagnosticsEnabled)
        return true;

    if (g_RuntimeSnapshotThread)
        return true;

    g_RuntimeSnapshotStopEvent =
        CreateEventA(nullptr, TRUE, FALSE, nullptr);
    g_RuntimeSnapshotWakeEvent =
        CreateEventA(nullptr, FALSE, FALSE, nullptr);

    if (!g_RuntimeSnapshotStopEvent ||
        !g_RuntimeSnapshotWakeEvent) {
        if (g_RuntimeSnapshotStopEvent) {
            CloseHandle(g_RuntimeSnapshotStopEvent);
            g_RuntimeSnapshotStopEvent = nullptr;
        }
        if (g_RuntimeSnapshotWakeEvent) {
            CloseHandle(g_RuntimeSnapshotWakeEvent);
            g_RuntimeSnapshotWakeEvent = nullptr;
        }
        return false;
    }

    DWORD threadId = 0;
    g_RuntimeSnapshotThread =
        CreateThread(
            nullptr,
            0,
            RuntimeSnapshotWorker,
            nullptr,
            0,
            &threadId);

    if (!g_RuntimeSnapshotThread) {
        CloseHandle(g_RuntimeSnapshotStopEvent);
        CloseHandle(g_RuntimeSnapshotWakeEvent);
        g_RuntimeSnapshotStopEvent = nullptr;
        g_RuntimeSnapshotWakeEvent = nullptr;
        return false;
    }

    ModLog(
        "[ElementalSystemExpanded] Persistent diagnostics worker started "
        "(periodic=5000ms, atomic-replace, threadId=%lu).\n",
        static_cast<unsigned long>(threadId));
    return true;
}

static void StopRuntimeSnapshotWorker()
{
    HANDLE thread = g_RuntimeSnapshotThread;
    if (!thread)
        return;

    if (g_RuntimeSnapshotStopEvent)
        SetEvent(g_RuntimeSnapshotStopEvent);

    const DWORD waited = WaitForSingleObject(thread, 5000);
    if (waited != WAIT_OBJECT_0) {
        ShipLog(
            "[ElementalSystemExpanded] WARNING: diagnostics worker did not "
            "stop within 5s; handles left intact.\n");
        return;
    }

    CloseHandle(thread);
    g_RuntimeSnapshotThread = nullptr;

    if (g_RuntimeSnapshotStopEvent) {
        CloseHandle(g_RuntimeSnapshotStopEvent);
        g_RuntimeSnapshotStopEvent = nullptr;
    }
    if (g_RuntimeSnapshotWakeEvent) {
        CloseHandle(g_RuntimeSnapshotWakeEvent);
        g_RuntimeSnapshotWakeEvent = nullptr;
    }
}

// ---------------------------------------------------------
// Entry point
// ---------------------------------------------------------
DWORD WINAPI MainThread(LPVOID lpParam) {
    (void)lpParam;

    const bool artifactPathsReady = InitializeArtifactPaths();
    InitializeDebugDiagnosticsMode();

    ShipLog(
        "[ElementalSystemExpanded] Stage 6.8.2 starting (custom BP_DragonExplosion restored; dedicated IgnoreCanProcessDamage authorization supplied by Lua; native EffectValue=1 spread provenance; Flashover Burn+Stun; generated BasePower/Attacker/AttackType layout hardening; reflected bridge detached-replay validation retained). "
        "Diagnostics=%s%s\n",
        g_DebugDiagnosticsEnabled ? "ON" : "OFF",
        ESE_DEV_FORCE_UNKNOWN_BOOTSTRAP ? " DevForceUnknown=YES" : "");

    if (!artifactPathsReady) {
        ShipLog(
            "[ElementalSystemExpanded] ERROR: DLL artifact path resolution failed; "
            "refusing to use current-working-directory fallbacks. Mod not started.\n");
        return 1;
    }

    ModLog("[ElementalSystemExpanded] Artifact directory: %s (%s)\n",
        g_ArtifactDirectory.c_str(),
        g_ArtifactPathStatus.c_str());

    if (MH_Initialize() != MH_OK) {
        ShipLog("[ElementalSystemExpanded] ERROR: MinHook failed to initialize.\n");
        return 1;
    }

    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA(NULL));
    if (!base) {
        ShipLog("[ElementalSystemExpanded] ERROR: Failed to resolve Palworld module base.\n");
        MH_Uninitialize();
        return 1;
    }

    g_ModuleBase = base;
    FBuildFingerprint fingerprint{};
    if (!QueryBuildFingerprint(base, &fingerprint)) {
        ShipLog(
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

    g_CurrentFingerprint = fingerprint;
    InitializeUpdateDiagnostics(fingerprint);
    LoadBuildProfileCache(fingerprint);

    ModLog(
        "[ElementalSystemExpanded] Build identity: Known104=%s ProfileCache=%s\n",
        IsKnown104Fingerprint(fingerprint) ? "YES" : "NO",
        g_ProfileCacheStatus.c_str());

    auto Fatal = [&](const char* message) -> DWORD {
        ShipLog("[ElementalSystemExpanded] ERROR: %s\n", message);
        // Normal shipping mode keeps diagnostics silent, but a resolver/hook
        // initialization failure is actionable and warrants one-shot reports.
        g_ForceFailureArtifacts = true;
        WriteMigrationReport(fingerprint);
        WriteResolverReport(fingerprint);
        g_ForceFailureArtifacts = false;
        MH_DisableHook(MH_ALL_HOOKS);
        MH_Uninitialize();
        return 1;
        };

    if (!IsKnown104Fingerprint(fingerprint) && !ESE_HAS_GENERATED_PAL_LAYOUT) {
        return Fatal(
            "unknown game build without a generated Pal layout header; "
            "refusing to use embedded 1.0.4 Pal offsets");
    }

    std::string layoutBindingDetail;
    const bool layoutFingerprintBound = GeneratedLayoutFingerprintAvailable();
    const bool layoutFingerprintMatch =
        GeneratedLayoutFingerprintMatches(fingerprint, &layoutBindingDetail);

    bool generatedLayoutBindingOK = true;
    if (ESE_HAS_GENERATED_PAL_LAYOUT) {
        // A bound generated header must always match the running executable. Legacy
        // unbound headers are accepted only for the exact preserved 1.0.4
        // oracle where every critical layout value is checked below.
        generatedLayoutBindingOK = layoutFingerprintBound
            ? layoutFingerprintMatch
            : IsKnown104Fingerprint(fingerprint);
    }
    else {
        generatedLayoutBindingOK = IsKnown104Fingerprint(fingerprint);
    }

    if (generatedLayoutBindingOK && !IsKnown104Fingerprint(fingerprint)) {
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

    RecordResolver(
        "CAPABILITY.GeneratedLayoutBinding",
        layoutFingerprintBound ? "generated-header-fingerprint" : "known-build-oracle-only",
        generatedLayoutBindingOK
        ? (layoutFingerprintBound
            ? EResolveConfidence::Exact
            : EResolveConfidence::Exact)
        : EResolveConfidence::Failed,
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
    if (IsKnown104Fingerprint(fingerprint)) {
        layoutProfileOK =
            ActiveLayout::Character_RootComponent == Known104::Offset::Character_RootComponent &&
            ActiveLayout::Character_CharacterParameterComponent == Known104::Offset::Character_CharacterParameterComponent &&
            ActiveLayout::Character_StaticCharacterParameterComponent == Known104::Offset::Character_StaticCharacterParameterComponent &&
            ActiveLayout::CharacterParameter_ElementType1 == Known104::Offset::CharacterParameter_ElementType1 &&
            ActiveLayout::CharacterParameter_ElementType2 == Known104::Offset::CharacterParameter_ElementType2 &&
            ActiveLayout::StaticCharacterParameter_IsPal == Known104::Offset::StaticCharacterParameter_IsPal &&
            ActiveLayout::DamageInfo_BasePower == Known104::Offset::DamageInfo_BasePower &&
            ActiveLayout::DamageInfo_AttackElement == Known104::Offset::DamageInfo_AttackElement &&
            ActiveLayout::DamageInfo_Attacker == Known104::Offset::DamageInfo_Attacker &&
            ActiveLayout::DamageInfo_AttackType == Known104::Offset::DamageInfo_AttackType &&
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
            ActiveLayout::DamageInfo_BasePower < ActiveLayout::DamageInfo_AttackElement &&
            (ActiveLayout::DamageInfo_BasePower % alignof(int32_t)) == 0 &&
            ActiveLayout::DamageInfo_AttackElement < ActiveLayout::DamageInfo_Attacker &&
            ActiveLayout::DamageInfo_Attacker < ActiveLayout::DamageInfo_AttackType &&
            (ActiveLayout::DamageInfo_Attacker % alignof(void*)) == 0 &&
            ActiveLayout::DamageInfo_AttackType < ActiveLayout::DamageInfo_EffectType1 &&
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

    RecordResolver(
        "CAPABILITY.LayoutProfile",
        ESE_HAS_GENERATED_PAL_LAYOUT ? "generated-sdk-layout" : "embedded-known-build-layout",
        layoutProfileOK
        ? (IsKnown104Fingerprint(fingerprint)
            ? EResolveConfidence::Exact
            : EResolveConfidence::ProfileStatic)
        : EResolveConfidence::Failed,
        0,
        layoutProfileOK
        ? (IsKnown104Fingerprint(fingerprint)
            ? "generated/active layout matches the 1.0.4 oracle"
            : "fingerprint-bound unknown-build generated layout, including Light-immunity target fields, passed structural sanity checks")
        : "layout profile failed sanity/oracle validation");

    if (!layoutProfileOK)
        return Fatal("active layout profile failed validation");

    RecordResolver(
        "CAPABILITY.RuntimeOwnerLink",
        "runtime-owner-backreference",
        EResolveConfidence::Deferred,
        0,
        "deferred: first elemental owner resolution must discover/verify UObject outer link before the capability is proven");

#if ESE_HAS_GENERATED_SCENE_RELATIVE_LOCATION
    RecordResolver(
        "CAPABILITY.RootPositionReference",
        "generated-scene-relative-location",
        EseGeneratedPalLayout::Has_SceneComponent_RelativeLocation
        ? EResolveConfidence::ProfileStatic
        : (IsKnown104Fingerprint(fingerprint)
            ? EResolveConfidence::RuntimeValidated
            : EResolveConfidence::Failed),
        0,
        EseGeneratedPalLayout::Has_SceneComponent_RelativeLocation
        ? (EseGeneratedPalLayout::Has_SceneComponent_AttachParent
            ? "USceneComponent::RelativeLocation + AttachParent generated; unattached-root semantics enforced and Freeze StartLocation proximity validates on use"
            : "USceneComponent::RelativeLocation generated; attachment state unavailable, so StartLocation proximity remains the fail-closed validator")
        : "generated relative location unavailable");
#else
    RecordResolver(
        "CAPABILITY.RootPositionReference",
        "known-build-hidden-world-fallback",
        IsKnown104Fingerprint(fingerprint)
        ? EResolveConfidence::RuntimeValidated
        : EResolveConfidence::Failed,
        0,
        IsKnown104Fingerprint(fingerprint)
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

    g_Resolved_ElementBuildup = ResolveNativeFromWrapperCall(
        "DamageReaction::AddElementStatusAdditionalValue_OneType.wrapper",
        base,
        Known104::Rva::ElementBuildup_Call,
        Known104::Rva::ElementBuildup,
        kBuildupWrapperTail,
        kBuildupWrapperMask,
        33);

    if (!g_Resolved_ElementBuildup) {
        g_Resolved_ElementBuildup = ResolvePatternTarget(
            "DamageReaction::AddElementStatusAdditionalValue_OneType.fallback",
            base,
            Known104::Rva::ElementBuildup,
            kBuildupPattern,
            kBuildupMask,
            true);
    }
    else {
        const intptr_t shift =
            static_cast<intptr_t>(g_Resolved_ElementBuildup - base) -
            static_cast<intptr_t>(Known104::Rva::ElementBuildup);
        g_AnchorRvaShifts.push_back(shift);
        RecomputeRvaShiftHint();
    }

    if (!g_Resolved_ElementBuildup)
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

    g_Resolved_AddStatus = ResolveCallInsideResolvedFunction(
        "PalStatusComponent::AddStatus.from-buildup",
        g_Resolved_ElementBuildup,
        0x300,
        Known104::Rva::AddStatus,
        kAddStatusLocalCall,
        kAddStatusLocalMask,
        18);

    // Independent fallback: exact native prologue, useful if the local buildup
    // sequence is rearranged while AddStatus itself remains unchanged.
    if (!g_Resolved_AddStatus) {
        static const uint8_t kAddStatusPattern[] = {
            0x40,0x55,0x48,0x8D,0x6C,0x24,0xA9,
            0x48,0x81,0xEC,0xB0,0x00,0x00,0x00,
            0x45,0x33,0xC0,0x4C,0x89,0x45,0xC7,0x44,0x89,0x45,0xCF,
            0x0F,0x57,0xC9,0xF3,0x0F,0x11,0x4D,0xD3
        };
        static const char kAddStatusMask[] =
            "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx";

        g_Resolved_AddStatus = ResolvePatternTarget(
            "PalStatusComponent::AddStatus.fallback",
            base,
            Known104::Rva::AddStatus,
            kAddStatusPattern,
            kAddStatusMask,
            true);
    }
    else {
        const intptr_t shift =
            static_cast<intptr_t>(g_Resolved_AddStatus - base) -
            static_cast<intptr_t>(Known104::Rva::AddStatus);
        g_AnchorRvaShifts.push_back(shift);
        RecomputeRvaShiftHint();
    }

    if (!g_Resolved_AddStatus)
        return Fatal("native AddStatus implementation could not be resolved safely");

    g_Resolved_AddStatusParameter =
        ResolveAddStatusParameterSiblingProbe(g_Resolved_AddStatus);
    if (!g_Resolved_AddStatusParameter)
        return Fatal("Stage 6.7.4.24 could not resolve AddStatus parameter sibling safely");

    RecordResolver(
        "PalStatusComponent::AddStatusParameter.6_7_4_23",
        "AddStatus-local-parameter-sibling",
        EResolveConfidence::Strong,
        g_Resolved_AddStatusParameter,
        "observation-only Freeze marker path; unique local call receiving AddStatus local FStatusDynamicParameter");

    RecomputeRvaShiftHint();

    // -----------------------------------------------------
    // PalServer authoritative elemental accumulator recovery
    // -----------------------------------------------------
    // The reflected wrapper path can resolve to a valid native function that is
    // NOT the authoritative dedicated-server combat accumulator.  Runtime
    // AddStatus observation proved that PalServer's real accumulator retains
    // the historical native prologue and the semantic
    //
    //   Effect -> StatusID mapper -> native AddStatus
    //
    // tail.  Recover that implementation independently and require its internal
    // AddStatus call to resolve to the already-proven native AddStatus target.
    //
    // This is semantic validation, not an RVA override.
    const uintptr_t wrapperResolvedElementBuildup =
        g_Resolved_ElementBuildup;

    uintptr_t authoritativeElementBuildup =
        ResolvePatternTarget(
            "DamageReaction::AddElementStatusAdditionalValue_OneType.authoritative-prologue",
            base,
            Known104::Rva::ElementBuildup,
            kBuildupPattern,
            kBuildupMask,
            true);

    bool authoritativeAccumulatorValidated = false;

    if (authoritativeElementBuildup) {
        const uintptr_t authoritativeAddStatus =
            ResolveCallInsideResolvedFunction(
                "PalServer.AuthoritativeElementAccumulator.AddStatus",
                authoritativeElementBuildup,
                0x300,
                Known104::Rva::AddStatus,
                kAddStatusLocalCall,
                kAddStatusLocalMask,
                18);

        authoritativeAccumulatorValidated =
            authoritativeAddStatus != 0 &&
            authoritativeAddStatus == g_Resolved_AddStatus;

        if (authoritativeAccumulatorValidated) {
            // Critical: all downstream raw-FPalDamageInfo discovery and the
            // gameplay detour must use the authoritative combat accumulator,
            // not the reflected/wrapper candidate that PalServer does not call
            // for real combat buildup.
            g_Resolved_ElementBuildup =
                authoritativeElementBuildup;

            char detail[320]{};
            snprintf(
                detail,
                sizeof(detail),
                "authoritative accumulator=+0x%llX; wrapper candidate=+0x%llX; "
                "semantic internal AddStatus=+0x%llX matches independently resolved native AddStatus",
                static_cast<unsigned long long>(
                    authoritativeElementBuildup - base),
                static_cast<unsigned long long>(
                    wrapperResolvedElementBuildup
                        ? wrapperResolvedElementBuildup - base
                        : 0),
                static_cast<unsigned long long>(
                    g_Resolved_AddStatus - base));

            RecordResolver(
                "CAPABILITY.PalServerAuthoritativeElementAccumulator",
                "native-prologue+semantic-effect-to-status-addstatus-tail",
                EResolveConfidence::RuntimeValidated,
                authoritativeElementBuildup,
                detail);

            // The authoritative accumulator is a semantically proven native
            // anchor. Feed its relative movement into the same durability hint
            // system used by the rest of the resolver instead of retaining the
            // reflected-wrapper candidate's misleading shift.
            const intptr_t authoritativeShift =
                static_cast<intptr_t>(
                    authoritativeElementBuildup - base) -
                static_cast<intptr_t>(Known104::Rva::ElementBuildup);
            g_AnchorRvaShifts.push_back(authoritativeShift);
        }
        else {
            RecordResolver(
                "CAPABILITY.PalServerAuthoritativeElementAccumulator",
                "native-prologue+semantic-effect-to-status-addstatus-tail",
                EResolveConfidence::Failed,
                0,
                "native-prologue candidate found but its semantic AddStatus tail did not resolve to the independently proven native AddStatus target");
        }
    }
    else {
        RecordResolver(
            "CAPABILITY.PalServerAuthoritativeElementAccumulator",
            "native-prologue+semantic-effect-to-status-addstatus-tail",
            EResolveConfidence::Failed,
            0,
            "native accumulator prologue could not be resolved uniquely");
    }

    if (!authoritativeAccumulatorValidated) {
        g_ForceFailureArtifacts = true;
        WriteMigrationReport(fingerprint);
        WriteResolverReport(fingerprint);
        g_ForceFailureArtifacts = false;

        return Fatal(
            "PalServer authoritative elemental accumulator could not be semantically validated");
    }

    // The wrapper-derived native target was a real function but not the
    // authoritative dedicated-server combat accumulator. If it contributed an
    // RVA-shift anchor earlier, remove that stale anchor now so subsequent
    // wrapper-backed resolver hints are based only on semantically validated
    // PalServer anchors.
    if (wrapperResolvedElementBuildup &&
        wrapperResolvedElementBuildup != authoritativeElementBuildup) {
        const intptr_t staleWrapperShift =
            static_cast<intptr_t>(
                wrapperResolvedElementBuildup - base) -
            static_cast<intptr_t>(Known104::Rva::ElementBuildup);

        const auto staleIt = std::find(
            g_AnchorRvaShifts.begin(),
            g_AnchorRvaShifts.end(),
            staleWrapperShift);

        if (staleIt != g_AnchorRvaShifts.end())
            g_AnchorRvaShifts.erase(staleIt);
    }

    RecomputeRvaShiftHint();

    // Raw FPalDamageInfo capture is a core semantic dependency, not an optional
    // enhancement. Without it normal 2U attacks would silently degrade to 1U.
    g_DamageEffectCaller = FindEffectCallerStart(base);
    g_Resolved_RawEffectCaller = g_DamageEffectCaller;
    if (g_DamageEffectCaller)
        CacheResolvedRva("RawFPalDamageInfoCaller", g_DamageEffectCaller);
    if (!g_DamageEffectCaller) {
        RecordResolver(
            "CAPABILITY.RawUnits.Resolve",
            "capability",
            EResolveConfidence::Failed,
            0,
            "authoritative accumulator resolved, but raw FPalDamageInfo caller could not be recovered; gameplay disabled to preserve exact 1U/2U semantics");

        g_ForceFailureArtifacts = true;
        WriteMigrationReport(fingerprint);
        WriteResolverReport(fingerprint);
        g_ForceFailureArtifacts = false;

        return Fatal(
            "authoritative PalServer raw FPalDamageInfo caller could not be resolved safely");
    }

    RecordResolver(
        "CAPABILITY.RawUnits.Resolve",
        "capability",
        EResolveConfidence::RuntimeValidated,
        g_DamageEffectCaller,
        "authoritative PalServer raw EffectValue1/2 source resolved; gameplay hook pending");

    RecomputeRvaShiftHint();

    // Matchup is an independent capability. If its unique helper cannot be
    // proven after an update, elemental statuses/reactions may still run.
    const bool matchupReady = InstallElementMatchupTier(base);
    RecordResolver(
        "CAPABILITY.MatchupMatrix",
        "capability",
        matchupReady ? EResolveConfidence::RuntimeValidated : EResolveConfidence::Failed,
        matchupReady ? g_Resolved_MatchupHelper : 0,
        matchupReady ? "enabled" : "disabled; core continues");

    RecomputeRvaShiftHint();

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

    g_Resolved_AddVisualEffect = ResolveNativeFromWrapperCall(
        "PalVisualEffectComponent::AddVisualEffect",
        base,
        Known104::Rva::AddVisualEffect_Call,
        Known104::Rva::AddVisualEffect_Native,
        kAddVfxTail,
        kAddVfxMask,
        42);

    g_Resolved_AddVisualEffectLocal = ResolveNativeFromWrapperCall(
        "PalVisualEffectComponent::AddVisualEffect_Local",
        base,
        Known104::Rva::AddVisualEffectLocal_Call,
        Known104::Rva::AddVisualEffectLocal_Native,
        kAddVfxTail,
        kAddVfxMask,
        42);

    std::vector<FHookTransactionSpec> lightVfxHooks;
    lightVfxHooks.reserve(2);

    if (g_Resolved_AddVisualEffect) {
        lightVfxHooks.push_back(FHookTransactionSpec{
            "PalVisualEffectComponent::AddVisualEffect.light-substitution",
            g_Resolved_AddVisualEffect,
            reinterpret_cast<void*>(&Detour_AddVisualEffect),
            reinterpret_cast<void**>(&Original_AddVisualEffect)
            });
    }
    else {
        ShipLog(
            "[ElementalSystemExpanded] WARNING: AddVisualEffect could not "
            "be resolved; non-local Light VFX substitution disabled.\n");
    }

    if (g_Resolved_AddVisualEffectLocal) {
        lightVfxHooks.push_back(FHookTransactionSpec{
            "PalVisualEffectComponent::AddVisualEffect_Local.light-substitution",
            g_Resolved_AddVisualEffectLocal,
            reinterpret_cast<void*>(&Detour_AddVisualEffect_Local),
            reinterpret_cast<void**>(&Original_AddVisualEffect_Local)
            });
    }
    else {
        ShipLog(
            "[ElementalSystemExpanded] WARNING: AddVisualEffect_Local could not "
            "be resolved; local Light VFX substitution disabled.\n");
    }

    if (!lightVfxHooks.empty()) {
        if (InstallHookTransaction(
            "LightVfxSubstitution",
            lightVfxHooks.data(),
            lightVfxHooks.size())) {
            const uintptr_t capabilityAddress =
                g_Resolved_AddVisualEffect
                ? g_Resolved_AddVisualEffect
                : g_Resolved_AddVisualEffectLocal;
            RecordResolver(
                "CAPABILITY.LightVfxSubstitution",
                "capability",
                EResolveConfidence::RuntimeValidated,
                capabilityAddress,
                "Light-sourced DarkCondition and CameraVignette construction redirect through registered Light VFX classes and restore runtime IDs 21/24");
        }
        else {
            Original_AddVisualEffect = nullptr;
            Original_AddVisualEffect_Local = nullptr;
            ShipLog(
                "[ElementalSystemExpanded] WARNING: Light VFX substitution "
                "hook transaction failed; gameplay continues unchanged.\n");
        }
    }
    else {
        RecordResolver(
            "CAPABILITY.LightVfxSubstitution",
            "capability",
            EResolveConfidence::Failed,
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

    g_Resolved_RemoveVisualEffectLocal = ResolveNativeFromWrapperCall(
        "PalVisualEffectComponent::RemoveVisualEffect_Local",
        base,
        Known104::Rva::RemoveVisualEffectLocal_Call,
        Known104::Rva::RemoveVisualEffectLocal_Native,
        kRemoveVfxTail,
        kRemoveVfxMask,
        27);

    if (!g_Resolved_RemoveVisualEffectLocal)
        return Fatal("RemoveVisualEffect_Local could not be resolved safely");

    Native_RemoveVisualEffect_Local =
        reinterpret_cast<NativeRemoveVisualEffectLocal_t>(
            g_Resolved_RemoveVisualEffectLocal);

    // -----------------------------------------------------
    // Phase C: install core status hooks at resolved addresses.
    //
    // IMPORTANT: do this before attempting the dedicated-server reaction
    // virtual-slot capability. The authoritative status core is independently
    // proven and must not silently remain vanilla merely because one optional
    // reaction dependency is unresolved. Freeze/Electrical are explicitly
    // fail-closed in the detour until the reaction capability becomes ready.
    // -----------------------------------------------------
    Native_AddStatus =
        reinterpret_cast<NativeAddStatus_t>(g_Resolved_AddStatus);

    const FHookTransactionSpec coreHooks[] = {
        {
            "AddElementStatusAdditionalValue_OneType",
            g_Resolved_ElementBuildup,
            reinterpret_cast<void*>(&Detour_AddElementStatusAdditionalValue_OneType),
            reinterpret_cast<void**>(&Original_AddElementStatusAdditionalValue_OneType)
        },
        {
            "PalStatusComponent::AddStatus",
            g_Resolved_AddStatus,
            reinterpret_cast<void*>(&Detour_NativeAddStatus),
            reinterpret_cast<void**>(&Original_NativeAddStatus)
        },
        {
            "PalStatusComponent::AddStatusParameter.6_7_4_23",
            g_Resolved_AddStatusParameter,
            reinterpret_cast<void*>(&Detour_NativeAddStatusParameterProbe),
            reinterpret_cast<void**>(&Original_NativeAddStatusParameterProbe)
        },
        {
            "RawFPalDamageInfoCaller",
            g_DamageEffectCaller,
            reinterpret_cast<void*>(&Detour_DamageEffectCaller),
            reinterpret_cast<void**>(&Original_DamageEffectCaller)
        }
    };

    if (!InstallHookTransaction(
        "ElementStatusCore",
        coreHooks,
        sizeof(coreHooks) / sizeof(coreHooks[0]))) {
        Native_AddStatus = nullptr;
        g_DamageEffectCaller = 0;
        g_Resolved_RawEffectCaller = 0;
        return Fatal("elemental core hook transaction failed");
    }

    RecordResolver(
        "CAPABILITY.RawUnits",
        "capability",
        EResolveConfidence::RuntimeValidated,
        g_Resolved_RawEffectCaller,
        "raw EffectValue1/2 capture enabled");

    RecordResolver(
        "CAPABILITY.ElementStatusCore",
        "capability",
        EResolveConfidence::RuntimeValidated,
        g_Resolved_ElementBuildup,
        "authoritative PalServer buildup replacement + native AddStatus + raw units ready");

    // -----------------------------------------------------
    // Phase D: split native reaction gates from dry-Freeze virtual gates.
    //
    // The wrapper-backed native hooks are enough for Electrical and for the
    // strong Wet->Freeze path. Dry Freeze additionally needs three exact
    // virtuals. Resolve/install these capabilities independently so a moved
    // engine wrapper cannot unnecessarily disable unrelated reaction behavior.
    // -----------------------------------------------------
    const bool nativeReactionReady =
        InstallNativeReactionHooks(base);

    if (nativeReactionReady) {
        InterlockedExchange(
            &g_ReactionCapabilityReady,
            1);

        RecordResolver(
            "CAPABILITY.NativeReactionGates",
            "capability",
            EResolveConfidence::RuntimeValidated,
            0,
            "wrapper-backed native action/movement/shooter reaction gates installed; Electrical and strong Wet reactions available");
    }
    else {
        RecordResolver(
            "CAPABILITY.NativeReactionGates",
            "capability",
            EResolveConfidence::Failed,
            0,
            "native reaction hook transaction unresolved; Electrical and Freeze fail closed");
    }

    bool dryFreezeVirtualReady = false;
    if (nativeReactionReady) {
        g_DryFreezeVSlotProbeStatus =
            "starting-bounded-native-registration-scan";

        // Write the revision/probe state before the registration scan. If a
        // future build exposes unexpected registration metadata and the scan
        // faults or stalls, diagnostics no longer leave an older report on
        // disk pretending the new DLL never ran.
        ShipLog(
            "[ElementalSystemExpanded] ResolverRevision=%s; dry-Freeze vslot resolver starting.\n",
            kResolverRevision);
        if (g_DebugDiagnosticsEnabled)
            WriteResolverReport(fingerprint);

        dryFreezeVirtualReady = ResolveDurableVirtualSlots(base);
        g_DryFreezeVSlotProbeStatus =
            dryFreezeVirtualReady
                ? "complete-ready"
                : "complete-failed-closed";
    }
    else {
        g_DryFreezeVSlotProbeStatus =
            "skipped-native-reaction-gates-not-ready";
    }

    if (dryFreezeVirtualReady) {
        InterlockedExchange(
            &g_DryFreezeVirtualCapabilityReady,
            1);

        RecordResolver(
            "CAPABILITY.DryFreezeVirtualGates",
            "capability",
            EResolveConfidence::RuntimeValidated,
            0,
            "native-registration/full-wrapper identity resolved SetComponentTickEnabled + StopAnimMontage + PalStatusBase::TickStatus virtual slots; dry Freeze enabled");
    }
    else {
        RecordResolver(
            "CAPABILITY.DryFreezeVirtualGates",
            "capability",
            EResolveConfidence::Failed,
            0,
            nativeReactionReady
                ? "native reaction gates ACTIVE; dry-Freeze virtual wrappers unresolved; dry Freeze fails closed while Electrical and ready Wet->Freeze remain enabled"
                : "native reaction gates unavailable; dry Freeze unavailable");
    }

    if (nativeReactionReady) {
        RecordResolver(
            "CAPABILITY.WetGatedStrongReactions",
            "capability",
            EResolveConfidence::RuntimeValidated,
            0,
            dryFreezeVirtualReady
                ? "Electrical + Wet->Freeze + dry Freeze available with Stage 6.6.3 semantics"
                : "Electrical + ready Wet->Freeze available; dry Freeze separately fail-closed");
    }
    else {
        RecordResolver(
            "CAPABILITY.WetGatedStrongReactions",
            "capability",
            EResolveConfidence::Failed,
            0,
            "native reaction gates unresolved; Freeze/Electrical fail closed");
    }

    ShipLog(
        "[ElementalSystemExpanded] PALSERVER REACTION CAPABILITIES: "
        "NativeGates=%s DryFreezeVirtuals=%s. "
        "Electrical=%s WetFreeze=%s DryFreeze=%s.\\n",
        nativeReactionReady ? "READY" : "NOT_READY",
        dryFreezeVirtualReady ? "READY" : "NOT_READY",
        nativeReactionReady ? "ENABLED" : "FAIL_CLOSED",
        nativeReactionReady ? "ENABLED_WHEN_WET_AND_ICD_READY" : "FAIL_CLOSED",
        dryFreezeVirtualReady ? "ENABLED" : "FAIL_CLOSED");

    // Raw FPalDamageInfo capture was resolved and installed as part of the
    // elemental core transaction above.

    WriteBuildProfileCache(fingerprint);
    if (g_DebugDiagnosticsEnabled) {
        WriteMigrationReport(fingerprint);
        WriteResolverReport(fingerprint);

        if (!StartRuntimeSnapshotWorker()) {
            ShipLog(
                "[ElementalSystemExpanded] WARNING: persistent diagnostics worker "
                "could not be started; gameplay remains active, runtime snapshot "
                "persistence unavailable.\n");
        }
    }

    ShipLog(
        "[ElementalSystemExpanded] ACTIVE: Stage 6.3 validated gameplay/presentation baseline; "
        "Stage 6.7.4.11.1 PalServer bounded native-registration vslot resolver + Stage 6.6.3 status semantics + Neutral/Light immunity + elemental status exchange + Darkness/Light 3s cap. NativeReactionGates=%s DryFreezeVirtuals=%s Debug diagnostics=%s.\n",
        nativeReactionReady ? "READY" : "FAIL_CLOSED",
        dryFreezeVirtualReady ? "READY" : "FAIL_CLOSED",
        g_DebugDiagnosticsEnabled ? "ON" : "OFF");

    ShipLog(
        "[ElementalSystemExpanded] PALSERVER GAMEPLAY PATH: corrected authoritative native accumulator + raw FPalDamageInfo caller semantically validated; Stage 6.6.3 status core enabled; reaction-sensitive effects require separately validated PalServer reaction capability.\n");

    ModLog(
        "[ElementalSystemExpanded] Semantics: elemental ICD=%.1fs or 3rd hit; "
        "1U=%.1fs 2U=%.1fs; exchange carryover threshold=%.1fs; "
        "Darkness/Light cap=%.1fs; reaction ICD=%.1fs; WetFreeze=%.1fs.\n",
        kElementICDSeconds,
        kOneUnitDuration,
        kTwoUnitDuration,
        kMinimumExchangeCarryover,
        kDarknessDuration,
        kReactionICDSeconds,
        kWetFreezeDuration);

    ModLog(
        "[ElementalSystemExpanded] Light presentation: DarkCondition runtime ID=%u "
        "uses lookup %u; CameraVignette runtime ID=%u uses lookup %u; "
        "genuine Dark remains vanilla.\n",
        static_cast<unsigned>(kDarkConditionVisualEffectID),
        static_cast<unsigned>(kLightVisualEffectLookupID),
        static_cast<unsigned>(kCameraVignetteVisualEffectID),
        static_cast<unsigned>(kLightCameraVisualEffectLookupID));

    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    (void)lpReserved;

    if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
        g_SelfModule = hModule;
        DisableThreadLibraryCalls(hModule);
    }

    return TRUE;
}

extern "C" {
    __declspec(dllexport) void* start_mod() {
        if (InterlockedCompareExchange(&g_StartRequested, 1, 0) != 0)
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
            InterlockedExchange(&g_StartRequested, 0);
            OutputDebugStringA(
                "[ElementalSystemExpanded] ERROR: failed to create initialization thread.\n");
            return nullptr;
        }

        g_InitThread = thread;
        if (ResumeThread(thread) == static_cast<DWORD>(-1)) {
            g_InitThread = nullptr;
            CloseHandle(thread);
            InterlockedExchange(&g_StartRequested, 0);
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
        HANDLE initThread = g_InitThread;
        if (initThread) {
            WaitForSingleObject(initThread, INFINITE);
            CloseHandle(initThread);
            g_InitThread = nullptr;
        }

        MH_DisableHook(MH_ALL_HOOKS);

        // In debug mode, stop the diagnostics worker before clearing runtime
        // state so no worker thread can outlive this module.
        StopRuntimeSnapshotWorker();

        AcquireSRWLockExclusive(&g_ElementICDLock);
        g_ElementICD.clear();
        ReleaseSRWLockExclusive(&g_ElementICDLock);

        AcquireSRWLockExclusive(&g_ElementalStatusProvenanceLock);
        g_ElementalStatusProvenance.clear();
        ReleaseSRWLockExclusive(&g_ElementalStatusProvenanceLock);

        AcquireSRWLockExclusive(&g_ReactionICDLock);
        g_ReactionLastSuccess.clear();
        ReleaseSRWLockExclusive(&g_ReactionICDLock);

        g_ReactionTLS = {};

        Original_SetActionClassParameter = nullptr;
        Original_PlayAction = nullptr;
        Original_SetJumpDisableFlag = nullptr;
        Original_SetStepDisableFlag = nullptr;
        Original_SetMoveDisableFlag = nullptr;
        Original_SetWalkSpeedMultiplier = nullptr;
        Original_SetYawRotatorMultiplier = nullptr;
        Original_Exec_SetJumpDisableFlag = nullptr;
        Original_Exec_SetStepDisableFlag = nullptr;
        Original_Exec_SetMoveDisableFlag = nullptr;
        g_ExactWrapper_SetJumpDisableFlag = 0;
        g_ExactWrapper_SetStepDisableFlag = 0;
        g_ExactWrapper_SetMoveDisableFlag = 0;
        g_ExecHit_SetJumpDisableFlag = 0;
        g_ExecHit_SetStepDisableFlag = 0;
        g_ExecHit_SetMoveDisableFlag = 0;
        Original_SetDisableAimFlag_Layered = nullptr;
        Original_SetDisableShootFlag_Layered = nullptr;
        Original_SetDisableChangeWeaponFlag_Layered = nullptr;

        AcquireSRWLockExclusive(&g_BlockedFreezeRuntimeLock);
        g_BlockedFreezeRuntime.clear();
        ReleaseSRWLockExclusive(&g_BlockedFreezeRuntimeLock);

        g_FreezeTickTLS = {};
        Original_SetComponentTickEnabledVirtual = nullptr;
        Original_StopAnimMontageVirtual = nullptr;
        Original_FreezeTickVirtual = nullptr;
        g_SetComponentTickEnabledVirtualTarget = nullptr;
        g_StopAnimMontageVirtualTarget = nullptr;
        g_FreezeTickVirtualTarget = nullptr;
        InterlockedExchange(&g_ReactionCapabilityReady, 0);
        InterlockedExchange(&g_DryFreezeVirtualCapabilityReady, 0);

        AcquireSRWLockExclusive(&g_UObjectOuterOffsetLock);
        g_RuntimeUObjectOuterOffset = 0;
        g_RuntimeUObjectOuterLogged = false;
        ReleaseSRWLockExclusive(&g_UObjectOuterOffsetLock);

        AcquireSRWLockExclusive(
            &g_FreezeStartLocationLocatorLock);
        g_FreezeStartLocationOffset = 0;
        g_FreezeStartLocationUsesFloat = false;
        ReleaseSRWLockExclusive(
            &g_FreezeStartLocationLocatorLock);

        AcquireSRWLockExclusive(&g_RootReferenceDiagnosticsLock);
        g_RootReferenceValidationLogged = false;
        g_RootReferenceCrossCheckPassed = false;
        g_RootReferenceCrossCheckDistSq = -1.0;
        g_RootReferenceUsedHiddenFallback = false;
        ReleaseSRWLockExclusive(&g_RootReferenceDiagnosticsLock);
        InterlockedExchange(&g_StatusArrayValidationPasses, 0);
        InterlockedExchange(&g_StatusArrayValidationFailures, 0);
        InterlockedExchange(&g_VfxArrayValidationPasses, 0);
        InterlockedExchange(&g_VfxArrayValidationFailures, 0);
        InterlockedExchange(&g_RuntimeSnapshotDirty, 1);
        InterlockedExchange(&g_RuntimeSnapshotWrites, 0);
        InterlockedExchange(&g_RuntimeSnapshotWriteFailures, 0);
        InterlockedExchange(&g_LightNeutralLayoutWarningLogged, 0);

        Original_CalcElementMatchupTier = nullptr;
        Original_AddVisualEffect = nullptr;
        Original_AddVisualEffect_Local = nullptr;
        Native_RemoveVisualEffect_Local = nullptr;
        Native_AddStatus = nullptr;
        Original_NativeAddStatusParameterProbe = nullptr;
        Original_AddElementStatusAdditionalValue_OneType = nullptr;
        Original_NativeAddStatus = nullptr;
        Original_DamageEffectCaller = nullptr;
        g_DamageEffectCaller = 0;
        Original_ServerBuildupValueProbe = nullptr;
        InterlockedExchange64(
            &g_ServerBuildupProbeSequence,
            0);
        Original_ServerAddStatusCallerProbe = nullptr;
        InterlockedExchange64(
            &g_ServerAddStatusProbeSequence,
            0);
        InterlockedExchange(
            &g_ServerAddStatusOwnerDiagnosticsDone,
            0);

        MH_Uninitialize();

        InterlockedExchange(&g_StartRequested, 0);
    }
}