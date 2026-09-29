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
    // MAX_PATH-sized stack buffers are too small for extended Windows paths,
    // while a 32 KiB local array causes MSVC C6262-style large-stack warnings.
    // Keep the extended-path capacity, but allocate the scratch buffer on heap.
    static constexpr DWORD kModulePathCapacity = 32768;
    std::vector<char> modulePath(kModulePathCapacity, '\0');

    const DWORD n = GetModuleFileNameA(
        g_SelfModule,
        modulePath.data(),
        kModulePathCapacity);

    if (!n || n >= kModulePathCapacity) {
        g_ArtifactPathStatus = "dll-path-resolution-failed";
        return false;
    }

    std::string full(modulePath.data(), modulePath.data() + n);
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
static uintptr_t g_Resolved_OnBeginStatusWrapper = 0;
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

static bool QueryBuildFingerprint(uintptr_t base, FBuildFingerprint* out)
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
            out->TextHash = Fnv1a64(text, out->TextSize);
            return true;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }

    return false;
}

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
                static_cast<long long>(g_RvaShiftHint));
            RecordResolver(name, "wrapper-vslot-aob", EResolveConfidence::Failed, 0, detail);
            AppendDiagnosticWindow(name, detail, expectedDispatchRva);
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

static uintptr_t FindEffectCallerStart(uintptr_t moduleBase)
{
    if (!moduleBase || !g_Resolved_ElementBuildup)
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
        if (!candidate || !MatchMaskedBytes(candidate, kCallerPrologue, kCallerMask))
            return false;
        return CountRel32CallsToTarget(
            candidate, 0x300, g_Resolved_ElementBuildup) >= 2;
        };

    const uintptr_t cachedRva = CachedProfileValue(
        g_BuildProfileCache.Rvas,
        "RawFPalDamageInfoCaller");
    if (cachedRva) {
        const uintptr_t cached = moduleBase + cachedRva;
        if (qualifies(cached)) {
            RecordResolver(
                "RawFPalDamageInfoCaller",
                "fingerprint-profile+prologue+dual-call",
                EResolveConfidence::ProfileStatic,
                cached,
                "cached raw caller revalidated against exact build fingerprint");
            CacheResolvedRva("RawFPalDamageInfoCaller", cached);
            return cached;
        }
    }

    const uintptr_t known = moduleBase + Known104::Rva::RawEffectCaller;
    if (qualifies(known)) {
        const bool knownOracle = IsKnown104Fingerprint(g_CurrentFingerprint);
        RecordResolver(
            "RawFPalDamageInfoCaller",
            "known-rva+prologue+dual-call",
            knownOracle ? EResolveConfidence::Exact : EResolveConfidence::Strong,
            known,
            knownOracle
            ? "exact 1.0.4 prologue + two calls to resolved buildup target"
            : "historical RVA used only as hint; prologue + dual-call semantics independently verified");
        CacheResolvedRva("RawFPalDamageInfoCaller", known);
        return known;
    }

    const auto matches = FindPatternInExecutableSections(
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
        RecordResolver(
            "RawFPalDamageInfoCaller",
            "unique-prologue+dual-call",
            EResolveConfidence::Failed,
            0,
            detail);
        const intptr_t shifted = static_cast<intptr_t>(Known104::Rva::RawEffectCaller) + g_RvaShiftHint;
        AppendDiagnosticWindow(
            "RawFPalDamageInfoCaller",
            detail,
            shifted > 0 ? static_cast<uintptr_t>(shifted) : Known104::Rva::RawEffectCaller);
        return 0;
    }

    const intptr_t shift = static_cast<intptr_t>(unique - moduleBase) -
        static_cast<intptr_t>(Known104::Rva::RawEffectCaller);
    g_AnchorRvaShifts.push_back(shift);
    RecomputeRvaShiftHint();

    char detail[96]{};
    snprintf(detail, sizeof(detail), "shift=%+lld dual-call verified",
        static_cast<long long>(shift));
    RecordResolver(
        "RawFPalDamageInfoCaller",
        "unique-prologue+dual-call",
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

static AddElementStatusAdditionalValue_OneType_t
Original_AddElementStatusAdditionalValue_OneType = nullptr;

static NativeAddStatus_t Original_NativeAddStatus = nullptr;
static NativeAddStatus_t Native_AddStatus = nullptr;

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

static bool StatusComponentContainsExactStatus(
    void* statusComponent,
    void* statusObject)
{
    if (!statusComponent || !statusObject)
        return false;

    FRawTArray list{};
    __try {
        list = *reinterpret_cast<FRawTArray*>(
            reinterpret_cast<uintptr_t>(statusComponent) +
            ActiveLayout::StatusComponent_ExecutionStatusList);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }

    if (!ValidateRawPointerArray(list, 1024, 4096))
        return false;

    const int32_t limit = (list.Num < 64) ? list.Num : 64;
    for (int32_t i = 0; i < limit; ++i) {
        void* entry = nullptr;
        __try {
            entry = *reinterpret_cast<void**>(
                list.Data + static_cast<uintptr_t>(i) * sizeof(void*));
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
        if (entry == statusObject)
            return true;
    }
    return false;
}

static bool TryResolveReplicatedStatusContextWithOuterOffset(
    void* statusObject,
    uintptr_t outerOffset,
    void** outStatusComponent,
    void** outCharacter)
{
    if (outStatusComponent) *outStatusComponent = nullptr;
    if (outCharacter) *outCharacter = nullptr;
    if (!statusObject || !outerOffset)
        return false;

    uintptr_t statusComponent = 0;
    if (!ReadPointerAtOffset(
            reinterpret_cast<uintptr_t>(statusObject),
            outerOffset,
            &statusComponent) ||
        !statusComponent ||
        !StatusComponentContainsExactStatus(
            reinterpret_cast<void*>(statusComponent),
            statusObject)) {
        return false;
    }

    uintptr_t character = 0;
    if (!ReadPointerAtOffset(
            statusComponent,
            outerOffset,
            &character) ||
        !character) {
        return false;
    }

    uintptr_t reverseStatusComponent = 0;
    if (!ReadPointerAtOffset(
            character,
            ActiveLayout::Character_StatusComponent,
            &reverseStatusComponent) ||
        reverseStatusComponent != statusComponent) {
        return false;
    }

    if (outStatusComponent)
        *outStatusComponent = reinterpret_cast<void*>(statusComponent);
    if (outCharacter)
        *outCharacter = reinterpret_cast<void*>(character);
    return true;
}

static bool ResolveReplicatedStatusContext(
    void* statusObject,
    void** outStatusComponent,
    void** outCharacter)
{
    if (outStatusComponent) *outStatusComponent = nullptr;
    if (outCharacter) *outCharacter = nullptr;
    if (!statusObject)
        return false;

    uintptr_t cached = 0;
    AcquireSRWLockShared(&g_UObjectOuterOffsetLock);
    cached = g_RuntimeUObjectOuterOffset;
    ReleaseSRWLockShared(&g_UObjectOuterOffsetLock);

    if (cached && TryResolveReplicatedStatusContextWithOuterOffset(
            statusObject,
            cached,
            outStatusComponent,
            outCharacter)) {
        return true;
    }

    uintptr_t hint = Known104::Offset::UObject_OuterPrivate;
#if ESE_HAS_GENERATED_ENGINE_OPTIONALS
    if (EseGeneratedPalLayout::Has_UObject_OuterPrivate)
        hint = EseGeneratedPalLayout::UObject_OuterPrivate;
#endif

    void* statusComponent = nullptr;
    void* character = nullptr;
    if (TryResolveReplicatedStatusContextWithOuterOffset(
            statusObject,
            hint,
            &statusComponent,
            &character)) {
        AcquireSRWLockExclusive(&g_UObjectOuterOffsetLock);
        if (!g_RuntimeUObjectOuterOffset)
            g_RuntimeUObjectOuterOffset = hint;
        ReleaseSRWLockExclusive(&g_UObjectOuterOffsetLock);
        if (outStatusComponent) *outStatusComponent = statusComponent;
        if (outCharacter) *outCharacter = character;
        return true;
    }

    uintptr_t candidates[32]{};
    void* candidateStatusComponents[32]{};
    void* candidateCharacters[32]{};
    int count = 0;

    for (uintptr_t off = 0x10; off <= 0x100; off += sizeof(uintptr_t)) {
        if (off == hint)
            continue;
        void* candidateStatusComponent = nullptr;
        void* candidateCharacter = nullptr;
        if (TryResolveReplicatedStatusContextWithOuterOffset(
                statusObject,
                off,
                &candidateStatusComponent,
                &candidateCharacter)) {
            if (count < 32) {
                candidates[count] = off;
                candidateStatusComponents[count] = candidateStatusComponent;
                candidateCharacters[count] = candidateCharacter;
                ++count;
            }
        }
    }

    if (count != 1) {
        ShipLog(
            "[ElementalSystemExpanded] WARNING: replicated Freeze owner context %s. "
            "Instance=%p Candidates=%d.\n",
            count == 0 ? "FAILED" : "AMBIGUOUS",
            statusObject,
            count);
        return false;
    }

    AcquireSRWLockExclusive(&g_UObjectOuterOffsetLock);
    if (!g_RuntimeUObjectOuterOffset)
        g_RuntimeUObjectOuterOffset = candidates[0];
    ReleaseSRWLockExclusive(&g_UObjectOuterOffsetLock);

    if (outStatusComponent) *outStatusComponent = candidateStatusComponents[0];
    if (outCharacter) *outCharacter = candidateCharacters[0];

    ModLog(
        "[ESE-MP] CLIENT STATUS CONTEXT: Instance=%p OuterOffset=+0x%llX "
        "StatusComponent=%p Character=%p.\n",
        statusObject,
        static_cast<unsigned long long>(candidates[0]),
        candidateStatusComponents[0],
        candidateCharacters[0]);
    return true;
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


enum class EEseReplicatedFreezeMode : uint8_t {
    None = 0,
    Soft = 1,
    Strong = 2
};

static constexpr int32_t kEseFreezeModeMagicBase = 0x45534500;
static constexpr int32_t kEseFreezeModeMagicMask = 0xFFFFFF00;

static EEseReplicatedFreezeMode DecodeReplicatedFreezeMode(int32_t generalIndex)
{
    if ((generalIndex & kEseFreezeModeMagicMask) != kEseFreezeModeMagicBase)
        return EEseReplicatedFreezeMode::None;

    const uint8_t mode = static_cast<uint8_t>(generalIndex & 0xFF);
    if (mode == static_cast<uint8_t>(EEseReplicatedFreezeMode::Soft))
        return EEseReplicatedFreezeMode::Soft;
    if (mode == static_cast<uint8_t>(EEseReplicatedFreezeMode::Strong))
        return EEseReplicatedFreezeMode::Strong;
    return EEseReplicatedFreezeMode::None;
}

struct FReplicatedFreezeBeginTLS {
    bool Active = false;
    bool Soft = false;
    void* Status = nullptr;
    void* StatusComponent = nullptr;
    void* Character = nullptr;
};

static thread_local FReplicatedFreezeBeginTLS g_ReplicatedFreezeBeginTLS{};

static bool IsReplicatedSoftFreezeScope()
{
    return g_ReplicatedFreezeBeginTLS.Active &&
        g_ReplicatedFreezeBeginTLS.Soft;
}

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

enum class EReplicatedBlindnessMode : uint8_t {
    None = 0,
    Light = 1,
    Dark = 2
};

static EReplicatedBlindnessMode DecodeReplicatedBlindnessMode(
    int32_t generalIndex)
{
    if ((generalIndex & 0xFFFFFF00) != 0x45534C00)
        return EReplicatedBlindnessMode::None;
    const uint8_t mode = static_cast<uint8_t>(generalIndex & 0xFF);
    if (mode == 1u)
        return EReplicatedBlindnessMode::Light;
    if (mode == 2u)
        return EReplicatedBlindnessMode::Dark;
    return EReplicatedBlindnessMode::None;
}

// Replicated Light presentation may be emitted after the reflected OnBegin
// wrapper returns.  Keep only the two one-shot presentation obligations that
// belong to the exact VisualEffectComponent: world DarkCondition(21) and local
// CameraVignette(24).  Genuine Dark clears any stale Light entry.
struct FPendingReplicatedLightPresentation {
    void* StatusObject = nullptr;
    bool WorldPending = false;
    bool CameraPending = false;
};

static std::unordered_map<void*, FPendingReplicatedLightPresentation>
    g_PendingReplicatedLightPresentation;
static SRWLOCK g_PendingReplicatedLightPresentationLock = SRWLOCK_INIT;

static void SetPendingReplicatedLightPresentation(
    void* visualEffectComponent,
    void* statusObject,
    bool isLight)
{
    if (!visualEffectComponent)
        return;

    AcquireSRWLockExclusive(&g_PendingReplicatedLightPresentationLock);
    if (isLight) {
        g_PendingReplicatedLightPresentation[visualEffectComponent] = {
            statusObject, true, true
        };
    }
    else {
        g_PendingReplicatedLightPresentation.erase(visualEffectComponent);
    }
    ReleaseSRWLockExclusive(&g_PendingReplicatedLightPresentationLock);
}

static bool ConsumePendingReplicatedLightPresentation(
    void* visualEffectComponent,
    uint8_t visualEffectID)
{
    if (!visualEffectComponent ||
        (visualEffectID != 21u && visualEffectID != 24u)) {
        return false;
    }

    bool consume = false;
    AcquireSRWLockExclusive(&g_PendingReplicatedLightPresentationLock);
    const auto it = g_PendingReplicatedLightPresentation.find(
        visualEffectComponent);
    if (it != g_PendingReplicatedLightPresentation.end()) {
        if (visualEffectID == 21u && it->second.WorldPending) {
            it->second.WorldPending = false;
            consume = true;
        }
        else if (visualEffectID == 24u && it->second.CameraPending) {
            it->second.CameraPending = false;
            consume = true;
        }

        if (!it->second.WorldPending && !it->second.CameraPending)
            g_PendingReplicatedLightPresentation.erase(it);
    }
    ReleaseSRWLockExclusive(&g_PendingReplicatedLightPresentationLock);
    return consume;
}


static SRWLOCK g_FreezeDynamicHookLock = SRWLOCK_INIT;

// Stage 6.7.4.27: replicated SOFT Freeze can continue its begin graph after
// OnBeginStatus returns. Keep a character-scoped window open through the first
// complete Freeze tick and recognize calls made from that exact TickStatus.
static bool ObjectBelongsToCharacter(void* object, void* character);
static bool IsObjectInReplicatedSoftFreezeBeginWindow(void* object);
static bool IsObjectInReplicatedSoftFreezeTickScope(void* object);
static bool RemoveBlockedFreezeIceCondition(void* character);

static bool IsReplicatedSoftFreezeObjectScope(void* object)
{
    // Tighten Stage25/26's short OnBegin TLS: it is no longer global. Even
    // during TLS only objects belonging to the exact SOFT-frozen Pal qualify.
    if (IsReplicatedSoftFreezeScope()) {
        if (!object || !g_ReplicatedFreezeBeginTLS.Character)
            return false;

        if (ObjectBelongsToCharacter(
                object,
                g_ReplicatedFreezeBeginTLS.Character)) {
            return true;
        }
    }

    return IsObjectInReplicatedSoftFreezeBeginWindow(object) ||
        IsObjectInReplicatedSoftFreezeTickScope(object);
}

static bool IsBlockedReactionScope()
{
    if (IsReplicatedSoftFreezeScope())
        return true;

    return g_ReactionTLS.Active &&
        !g_ReactionTLS.AllowStrongControl &&
        (g_ReactionTLS.StatusID == ActiveIds::Status_Freeze ||
            g_ReactionTLS.StatusID == ActiveIds::Status_Electrical);
}

static bool IsBlockedFreezeScope()
{
    if (IsReplicatedSoftFreezeScope())
        return true;

    return g_ReactionTLS.Active &&
        !g_ReactionTLS.AllowStrongControl &&
        g_ReactionTLS.StatusID == ActiveIds::Status_Freeze;
}

static const char* BlockedReactionName()
{
    return IsReplicatedSoftFreezeScope()
        ? "Freeze(remote-soft)"
        : StrongReactionName(g_ReactionTLS.Reaction);
}

static void* __fastcall Detour_SetActionClassParameter(
    void* aiActionComponent,
    void* newActionClass,
    const void* dynamicParameter)
{
    if (IsReplicatedSoftFreezeObjectScope(aiActionComponent)) {
        ModLog(
            "[ESE-MP] CLIENT SOFT BLOCK: SetActionClassParameter Component=%p Class=%p\n",
            aiActionComponent,
            newActionClass);
        return nullptr;
    }

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
    if (IsReplicatedSoftFreezeObjectScope(actionComponent)) {
        ModLog(
            "[ESE-MP] CLIENT SOFT BLOCK: PlayAction Component=%p Target=%p Class=%p\n",
            actionComponent,
            actionTarget,
            actionClass);
        return nullptr;
    }

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

static void __fastcall Detour_SetJumpDisableFlag(
    void* movementComponent,
    uint64_t rawFName,
    bool disabled)
{
    if (disabled && IsReplicatedSoftFreezeObjectScope(movementComponent)) {
        ModLog(
            "[ESE-MP] CLIENT SOFT BLOCK: SetJumpDisableFlag(true) Component=%p\n",
            movementComponent);
        return;
    }

    if (IsBlockedReactionScope() && disabled) {
        ModLog(
            "[ElementalSystemExpanded] REACTION BLOCK: %s "
            "SetJumpDisableFlag(true) Component=%p\n",
            BlockedReactionName(),
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
    if (disabled && IsReplicatedSoftFreezeObjectScope(movementComponent)) {
        ModLog(
            "[ESE-MP] CLIENT SOFT BLOCK: SetStepDisableFlag(true) Component=%p\n",
            movementComponent);
        return;
    }

    if (IsBlockedReactionScope() && disabled) {
        ModLog(
            "[ElementalSystemExpanded] REACTION BLOCK: %s "
            "SetStepDisableFlag(true) Component=%p\n",
            BlockedReactionName(),
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
    if (disabled && IsReplicatedSoftFreezeObjectScope(movementComponent)) {
        ModLog(
            "[ESE-MP] CLIENT SOFT BLOCK: SetMoveDisableFlag(true) Component=%p\n",
            movementComponent);
        return;
    }

    if (IsBlockedReactionScope() && disabled) {
        ModLog(
            "[ElementalSystemExpanded] REACTION BLOCK: %s "
            "SetMoveDisableFlag(true) Component=%p\n",
            BlockedReactionName(),
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

    if (IsReplicatedSoftFreezeScope() ||
        g_ReactionTLS.StatusID == ActiveIds::Status_Freeze) {
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
    if (value >= 0.55f && value <= 0.65f &&
        IsReplicatedSoftFreezeObjectScope(movementComponent)) {
        ModLog(
            "[ESE-MP] CLIENT SOFT BLOCK: SetWalkSpeedMultiplier(%.3f) Component=%p\n",
            value, movementComponent);
        return;
    }

    if (ShouldBlockWalkMultiplier(value)) {
        ModLog(
            "[ElementalSystemExpanded] REACTION BLOCK: %s "
            "SetWalkSpeedMultiplier(%.3f) Component=%p\n",
            BlockedReactionName(),
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
    if (value >= -0.01f && value <= 0.01f &&
        IsReplicatedSoftFreezeObjectScope(movementComponent)) {
        ModLog(
            "[ESE-MP] CLIENT SOFT BLOCK: SetYawRotatorMultiplier(%.3f) Component=%p\n",
            value, movementComponent);
        return;
    }

    if (ShouldBlockYawMultiplier(value)) {
        ModLog(
            "[ElementalSystemExpanded] REACTION BLOCK: %s "
            "SetYawRotatorMultiplier(%.3f) Component=%p\n",
            BlockedReactionName(),
            value,
            movementComponent);
        return;
    }

    if (Original_SetYawRotatorMultiplier)
        Original_SetYawRotatorMultiplier(
            movementComponent, rawFName, value);
}


static bool ShouldBlockFreezeShooterDisable(
    void* shooterComponent,
    bool disabled)
{
    if (!disabled)
        return false;
    if (IsReplicatedSoftFreezeObjectScope(shooterComponent))
        return true;
    return IsBlockedFreezeScope();
}

static void __fastcall Detour_SetDisableAimFlag_Layered(
    void* shooterComponent,
    uint8_t layer,
    uint64_t rawFName,
    bool disabled)
{
    if (ShouldBlockFreezeShooterDisable(shooterComponent, disabled)) {
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
    if (ShouldBlockFreezeShooterDisable(shooterComponent, disabled)) {
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
    if (ShouldBlockFreezeShooterDisable(shooterComponent, disabled)) {
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

static bool InstallNativeReactionHooks(uintptr_t base)
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
        targets[i] = ResolveNativeFromWrapperCall(
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

    FHookTransactionSpec transaction[sizeof(hooks) / sizeof(hooks[0])]{};
    for (size_t i = 0; i < sizeof(hooks) / sizeof(hooks[0]); ++i) {
        transaction[i] = FHookTransactionSpec{
            hooks[i].Name,
            targets[i],
            hooks[i].Detour,
            hooks[i].Original
        };
    }

    return InstallHookTransaction(
        "WetGatedStrongReactions",
        transaction,
        sizeof(transaction) / sizeof(transaction[0]));
}

// ---------------------------------------------------------
// Durable reflected-wrapper virtual-slot extraction
// ---------------------------------------------------------
static bool ResolveDurableVirtualSlots(uintptr_t base)
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
    ok = ok && ResolveVirtualSlot(
        "VSlot.ActorComponent::SetComponentTickEnabled",
        base,
        Known104::Rva::SetComponentTickEnabled_Dispatch,
        Known104::VSlot::SetComponentTickEnabled,
        kComponentTickTail,
        kComponentTickMask,
        34,
        &g_VSlot_SetComponentTickEnabled);

    ok = ok && ResolveVirtualSlot(
        "VSlot.Character::StopAnimMontage",
        base,
        Known104::Rva::StopAnimMontage_Dispatch,
        Known104::VSlot::StopAnimMontage,
        kStopMontageTail,
        kStopMontageMask,
        31,
        &g_VSlot_StopAnimMontage);

    ok = ok && ResolveVirtualSlot(
        "VSlot.PalStatusBase::TickStatus",
        base,
        Known104::Rva::StatusTick_Dispatch,
        Known104::VSlot::StatusTick,
        kStatusTickTail,
        kStatusTickMask,
        33,
        &g_VSlot_StatusTick);

    return ok;
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
    bool ReplicatedClientBeginWindow = false;

    // Stage 6.7.4.27 retains Stage26 position snapshots and adds authority/action correlation; cap detailed snapshots so
    // debug logging cannot turn the per-frame Freeze Tick into unbounded spam.
    uint32_t PositionProbeTicks = 0;
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

static void CloseReplicatedSoftFreezeBeginWindow(void* status)
{
    if (!status)
        return;
    AcquireSRWLockExclusive(&g_BlockedFreezeRuntimeLock);
    const auto it = g_BlockedFreezeRuntime.find(status);
    if (it != g_BlockedFreezeRuntime.end())
        it->second.ReplicatedClientBeginWindow = false;
    ReleaseSRWLockExclusive(&g_BlockedFreezeRuntimeLock);
}

static bool ObjectBelongsToCharacter(void* object, void* character)
{
    if (!object || !character)
        return false;
    if (object == character)
        return true;

    uintptr_t outerOffset = 0;
    AcquireSRWLockShared(&g_UObjectOuterOffsetLock);
    outerOffset = g_RuntimeUObjectOuterOffset;
    ReleaseSRWLockShared(&g_UObjectOuterOffsetLock);
    if (!outerOffset)
        return false;

    uintptr_t current = reinterpret_cast<uintptr_t>(object);
    const uintptr_t wanted = reinterpret_cast<uintptr_t>(character);
    for (int depth = 0; depth < 6 && current; ++depth) {
        uintptr_t outer = 0;
        if (!ReadPointerAtOffset(current, outerOffset, &outer) || !outer)
            return false;
        if (outer == wanted)
            return true;
        current = outer;
    }
    return false;
}

static bool IsObjectInReplicatedSoftFreezeBeginWindow(void* object)
{
    if (!object)
        return false;
    bool blocked = false;
    AcquireSRWLockShared(&g_BlockedFreezeRuntimeLock);
    for (const auto& kv : g_BlockedFreezeRuntime) {
        const FBlockedFreezeRuntime& state = kv.second;
        if (!state.ReplicatedClientBeginWindow || !state.Character)
            continue;
        if (ObjectBelongsToCharacter(object, state.Character)) {
            blocked = true;
            break;
        }
    }
    ReleaseSRWLockShared(&g_BlockedFreezeRuntimeLock);
    return blocked;
}

static bool IsObjectInReplicatedSoftFreezeTickScope(void* object)
{
    if (!object || !g_FreezeTickTLS.Active || !g_FreezeTickTLS.Status)
        return false;
    FBlockedFreezeRuntime state{};
    if (!GetBlockedFreezeRuntime(g_FreezeTickTLS.Status, &state) ||
        !state.Character)
        return false;
    return ObjectBelongsToCharacter(object, state.Character);
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

static bool ReadResolvedFreezeStartLocation(
    void* status,
    FVectorDouble* outLocation)
{
    if (!status || !outLocation)
        return false;

    ptrdiff_t offset = 0;
    bool usesFloat = false;

    AcquireSRWLockShared(&g_FreezeStartLocationLocatorLock);
    offset = g_FreezeStartLocationOffset;
    usesFloat = g_FreezeStartLocationUsesFloat;
    ReleaseSRWLockShared(&g_FreezeStartLocationLocatorLock);

    if (offset <= 0)
        return false;

    bool ok = false;
    __try {
        if (usesFloat) {
            const FVectorFloat value =
                *reinterpret_cast<const FVectorFloat*>(
                    reinterpret_cast<uintptr_t>(status) + offset);
            *outLocation = FVectorDouble{
                static_cast<double>(value.X),
                static_cast<double>(value.Y),
                static_cast<double>(value.Z)
            };
        }
        else {
            *outLocation =
                *reinterpret_cast<const FVectorDouble*>(
                    reinterpret_cast<uintptr_t>(status) + offset);
        }
        ok = IsPlausibleWorldVector(*outLocation);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }

    return ok;
}

static uint32_t ReserveFreezePositionProbeTick(void* status)
{
    static constexpr uint32_t kMaxPositionProbeTicks = 12;
    if (!status)
        return 0;

    uint32_t sequence = 0;
    AcquireSRWLockExclusive(&g_BlockedFreezeRuntimeLock);
    const auto it = g_BlockedFreezeRuntime.find(status);
    if (it != g_BlockedFreezeRuntime.end() &&
        it->second.PositionProbeTicks < kMaxPositionProbeTicks) {
        sequence = ++it->second.PositionProbeTicks;
    }
    ReleaseSRWLockExclusive(&g_BlockedFreezeRuntimeLock);
    return sequence;
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
    if (!enabled && IsReplicatedSoftFreezeObjectScope(actorComponent)) {
        ModLog(
            "[ESE-MP] CLIENT SOFT BLOCK: SetComponentTickEnabled(false) Component=%p\n",
            actorComponent);
        return;
    }

    if (IsBlockedFreezeScope() && !enabled) {
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
    if (IsReplicatedSoftFreezeObjectScope(character)) {
        ModLog(
            "[ESE-MP] CLIENT SOFT BLOCK: StopAnimMontage Character=%p Montage=%p\n",
            character,
            montage);
        return;
    }

    if (g_ReactionTLS.Active &&
        !g_ReactionTLS.AllowStrongControl &&
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

    // Stage 6.7.4.27 also fixes the already-isolated dry presentation bug.
    // The replicated client never traverses the authoritative post-AddStatus
    // IceCondition cleanup. By the first TickStatus VFX17 has been created, so
    // remove it once through Palworld's verified local cleanup path.
    if (state.ReplicatedClientBeginWindow) {
        const bool iceRemoved =
            RemoveBlockedFreezeIceCondition(
                state.Character);
        ModLog(
            "[ESE-MP] CLIENT SOFT ICE CLEANUP: Instance=%p Character=%p Result=%s\n",
            status,
            state.Character,
            iceRemoved ? "REMOVED_OR_ENDED" : "NOT_PRESENT_OR_FAILED");
    }

    // Retain the Stage26 discriminator. Snapshot the root and StartLocation
    // around the validated refresh + ORIGINAL Freeze TickStatus.
    // This does not change control flow or write anything except the pre-existing
    // 6.6.3 StartLocation refresh below.
    const uint32_t positionProbeSequence =
        ReserveFreezePositionProbeTick(status);

    FVectorDouble probeRootPreRefresh{};
    FVectorDouble probeStartPreRefresh{};
    const bool probeRootPreRefreshOk =
        positionProbeSequence != 0 &&
        ReadRootWorldLocation(
            state.RootComponent,
            &probeRootPreRefresh);
    const bool probeStartPreRefreshOk =
        positionProbeSequence != 0 &&
        ReadResolvedFreezeStartLocation(
            status,
            &probeStartPreRefresh);

    // Keep the complete original TickStatus/lifecycle; if the
    // inferred BP StartLocation field validates against the real root position,
    // refresh it to the Pal's current position immediately before each tick.
    RefreshBlockedFreezeStartLocation(
        status,
        &state);

    FVectorDouble probeRootBeforeOriginal{};
    FVectorDouble probeStartBeforeOriginal{};
    const bool probeRootBeforeOriginalOk =
        positionProbeSequence != 0 &&
        ReadRootWorldLocation(
            state.RootComponent,
            &probeRootBeforeOriginal);
    const bool probeStartBeforeOriginalOk =
        positionProbeSequence != 0 &&
        ReadResolvedFreezeStartLocation(
            status,
            &probeStartBeforeOriginal);

    if (positionProbeSequence != 0) {
        void* probeAIActionFreeze = nullptr;
        const bool probeAIActionRead =
            ReadFreezeAIActionPointer(
                status,
                &probeAIActionFreeze);

        ModLog(
            "[ESE-PIN] TICK PRE #%u Instance=%p DeltaTime=%.6f "
            "RootPreRead=%s RootPre=(%.6f, %.6f, %.6f) "
            "StartPreRead=%s StartPre=(%.6f, %.6f, %.6f) "
            "RootCallRead=%s RootCall=(%.6f, %.6f, %.6f) "
            "StartCallRead=%s StartCall=(%.6f, %.6f, %.6f) "
            "AIActionRead=%s AIActionFreeze=%p\n",
            static_cast<unsigned>(positionProbeSequence),
            status,
            deltaTime,
            probeRootPreRefreshOk ? "YES" : "NO",
            probeRootPreRefresh.X,
            probeRootPreRefresh.Y,
            probeRootPreRefresh.Z,
            probeStartPreRefreshOk ? "YES" : "NO",
            probeStartPreRefresh.X,
            probeStartPreRefresh.Y,
            probeStartPreRefresh.Z,
            probeRootBeforeOriginalOk ? "YES" : "NO",
            probeRootBeforeOriginal.X,
            probeRootBeforeOriginal.Y,
            probeRootBeforeOriginal.Z,
            probeStartBeforeOriginalOk ? "YES" : "NO",
            probeStartBeforeOriginal.X,
            probeStartBeforeOriginal.Y,
            probeStartBeforeOriginal.Z,
            probeAIActionRead ? "YES" : "NO",
            probeAIActionFreeze);
    }

    const FFreezeTickTLS previous =
        g_FreezeTickTLS;

    g_FreezeTickTLS.Active = true;
    g_FreezeTickTLS.Status = status;

    // The original TickStatus call MUST execute for proper duration and
    // cleanup. We no longer skip the Freeze tick.
    if (Original_FreezeTickVirtual)
        Original_FreezeTickVirtual(
            status, deltaTime);

    if (positionProbeSequence != 0) {
        FVectorDouble probeRootAfterOriginal{};
        FVectorDouble probeStartAfterOriginal{};
        const bool probeRootAfterOriginalOk =
            ReadRootWorldLocation(
                state.RootComponent,
                &probeRootAfterOriginal);
        const bool probeStartAfterOriginalOk =
            ReadResolvedFreezeStartLocation(
                status,
                &probeStartAfterOriginal);

        const double rootDeltaSq =
            probeRootBeforeOriginalOk && probeRootAfterOriginalOk
            ? VectorDistanceSq(
                probeRootBeforeOriginal,
                probeRootAfterOriginal)
            : -1.0;
        const double startDeltaSq =
            probeStartBeforeOriginalOk && probeStartAfterOriginalOk
            ? VectorDistanceSq(
                probeStartBeforeOriginal,
                probeStartAfterOriginal)
            : -1.0;

        ModLog(
            "[ESE-PIN] TICK POST #%u Instance=%p "
            "RootPostRead=%s RootPost=(%.6f, %.6f, %.6f) RootDeltaSq=%.9f "
            "StartPostRead=%s StartPost=(%.6f, %.6f, %.6f) StartDeltaSq=%.9f\n",
            static_cast<unsigned>(positionProbeSequence),
            status,
            probeRootAfterOriginalOk ? "YES" : "NO",
            probeRootAfterOriginal.X,
            probeRootAfterOriginal.Y,
            probeRootAfterOriginal.Z,
            rootDeltaSq,
            probeStartAfterOriginalOk ? "YES" : "NO",
            probeStartAfterOriginal.X,
            probeStartAfterOriginal.Y,
            probeStartAfterOriginal.Z,
            startDeltaSq);
    }

    // Keep the replicated begin window through the first complete original
    // TickStatus. Some client Freeze initialization occurs after OnBeginStatus
    // returns. After this first tick, only exact Freeze Tick TLS remains.
    if (state.ReplicatedClientBeginWindow) {
        CloseReplicatedSoftFreezeBeginWindow(status);
        ModLog(
            "[ESE-MP] CLIENT SOFT BEGIN WINDOW CLOSED: Instance=%p after first original TickStatus.\n",
            status);
    }

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
    void* freezeStatus,
    bool replicatedClientBeginWindow = false)
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
            false,
            replicatedClientBeginWindow
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

    EBlindnessSource source =
        g_BlindnessSourceTLS;

    // Dedicated-client Darkness presentation is not guaranteed to remain inside
    // the reflected OnBegin wrapper.  Consume the exact per-component Light
    // obligation transported on StatusID 25 if the synchronous TLS has already
    // unwound.  Consume even when TLS is still Light so no stale marker survives.
    const bool pendingReplicatedLight =
        ConsumePendingReplicatedLightPresentation(
            visualEffectComponent, visualEffectID);
    if (source == EBlindnessSource::Unknown && pendingReplicatedLight)
        source = EBlindnessSource::Light;

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

static EBlindnessSource ResolveBlindnessSourceFromRawContext()
{
    uint8_t attackElement = 0;
    if (!GetRawAttackElement(&attackElement))
        return EBlindnessSource::Unknown;

    if (attackElement == ActiveIds::Element_Normal)
        return EBlindnessSource::Light;
    if (attackElement == ActiveIds::Element_Dark)
        return EBlindnessSource::Dark;

    return EBlindnessSource::Unknown;
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
            ? ResolveBlindnessSourceFromRawContext()
            : EBlindnessSource::Unknown;

        FBlindnessSourceScope blindnessScope(blindnessSource);
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
// Stage 6.7.4.26 replicated SOFT/STRONG mode reconstruction + Freeze Tick position probe
// ---------------------------------------------------------
static constexpr size_t kStatusDynamicParameterSize = 0x40;

using NativeOnBeginStatusWrapperProbe_t =
void(__fastcall*)(void* StatusObject, void* ParamOrContext, void* ExtraContext);

static NativeOnBeginStatusWrapperProbe_t
Original_OnBeginStatusWrapperProbe = nullptr;

static bool SnapshotStatusDynamicParameterProbe(
    void* statusObject,
    uint8_t* outStatusID,
    float* outDuration,
    float* outTimer,
    int32_t* outGeneralIndex,
    float* outGeneralFloat,
    uint8_t outBytes[kStatusDynamicParameterSize])
{
    if (outStatusID) *outStatusID = 0;
    if (outDuration) *outDuration = 0.0f;
    if (outTimer) *outTimer = 0.0f;
    if (outGeneralIndex) *outGeneralIndex = 0;
    if (outGeneralFloat) *outGeneralFloat = 0.0f;
    if (outBytes) memset(outBytes, 0, kStatusDynamicParameterSize);
    if (!statusObject)
        return false;

    if (ActiveLayout::StatusBase_StatusID < kStatusDynamicParameterSize)
        return false;
    // SDK relation: DynamicParameter is the contiguous 0x40-byte field
    // immediately preceding statusID. Derive it from the generated/current
    // statusID offset instead of pinning +0x50 independently.
    const uintptr_t dynamicParameterOffset =
        ActiveLayout::StatusBase_StatusID - kStatusDynamicParameterSize;

    const uintptr_t p = reinterpret_cast<uintptr_t>(statusObject);
    const size_t need = (std::max)(
        static_cast<size_t>(ActiveLayout::StatusBase_DurationTimer + sizeof(float)),
        static_cast<size_t>(dynamicParameterOffset + kStatusDynamicParameterSize));
    if (!IsReadableAddress(p, need))
        return false;

    __try {
        if (outStatusID)
            *outStatusID = *reinterpret_cast<const uint8_t*>(p + ActiveLayout::StatusBase_StatusID);
        if (outDuration)
            *outDuration = *reinterpret_cast<const float*>(p + ActiveLayout::StatusBase_Duration);
        if (outTimer)
            *outTimer = *reinterpret_cast<const float*>(p + ActiveLayout::StatusBase_DurationTimer);
        if (outGeneralIndex)
            *outGeneralIndex = *reinterpret_cast<const int32_t*>(p + dynamicParameterOffset);
        if (outGeneralFloat)
            *outGeneralFloat = *reinterpret_cast<const float*>(p + dynamicParameterOffset + 0x0C);
        if (outBytes)
            memcpy(outBytes, reinterpret_cast<const void*>(p + dynamicParameterOffset), kStatusDynamicParameterSize);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static void LogOnBeginOrderingProbe(const char* phase, void* statusObject)
{
    uint8_t statusID = 0;
    float duration = 0.0f;
    float timer = 0.0f;
    int32_t generalIndex = 0;
    float generalFloat = 0.0f;
    uint8_t bytes[kStatusDynamicParameterSize]{};

    const bool ok = SnapshotStatusDynamicParameterProbe(
        statusObject,
        &statusID,
        &duration,
        &timer,
        &generalIndex,
        &generalFloat,
        bytes);

    if (!ok)
        return;

    // Only elemental statuses are useful here; this also keeps OnBegin logs small.
    if (statusID < 19 || statusID > 25)
        return;

    char hex[(kStatusDynamicParameterSize * 2) + 1]{};
    static const char digits[] = "0123456789ABCDEF";
    for (size_t i = 0; i < kStatusDynamicParameterSize; ++i) {
        hex[i * 2] = digits[(bytes[i] >> 4) & 0xF];
        hex[(i * 2) + 1] = digits[bytes[i] & 0xF];
    }

    const EEseReplicatedFreezeMode freezeMode =
        DecodeReplicatedFreezeMode(generalIndex);
    const EReplicatedBlindnessMode blindnessMode =
        DecodeReplicatedBlindnessMode(generalIndex);
    const char* modeName =
        freezeMode == EEseReplicatedFreezeMode::Soft ? "SOFT" :
        freezeMode == EEseReplicatedFreezeMode::Strong ? "STRONG" :
        blindnessMode == EReplicatedBlindnessMode::Light ? "LIGHT" :
        blindnessMode == EReplicatedBlindnessMode::Dark ? "DARK" :
        "NONE";

    ModLog(
        "[ESE-ORDER] ONBEGIN %s Thread=%lu Instance=%p StatusID=%u (%s) "
        "Duration=%.6f Timer=%.6f GeneralIndex=0x%08X Mode=%s GeneralFloat=%.6f "
        "Bytes=%s\n",
        phase ? phase : "?",
        static_cast<unsigned long>(GetCurrentThreadId()),
        statusObject,
        static_cast<unsigned>(statusID),
        StatusIDName(statusID),
        duration,
        timer,
        static_cast<unsigned>(generalIndex),
        modeName,
        generalFloat,
        hex);
}

static bool RestoreReplicatedStatusTimerAfterBegin(
    void* statusObject,
    float authoritativeTimer)
{
    if (!statusObject || !std::isfinite(authoritativeTimer) ||
        authoritativeTimer <= kErosionExpiryTick || authoritativeTimer > 60.0f) {
        return false;
    }

    const uintptr_t p = reinterpret_cast<uintptr_t>(statusObject);
    if (!IsReadableAddress(
            p + ActiveLayout::StatusBase_DurationTimer,
            sizeof(float))) {
        return false;
    }

    __try {
        *reinterpret_cast<float*>(
            p + ActiveLayout::StatusBase_DurationTimer) = authoritativeTimer;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static void __fastcall Detour_OnBeginStatusWrapperProbe(
    void* statusObject,
    void* paramOrContext,
    void* extraContext)
{
    uint8_t statusID = 0;
    float preDuration = 0.0f;
    float preTimer = 0.0f;
    int32_t generalIndex = 0;
    float generalFloat = 0.0f;
    uint8_t bytes[kStatusDynamicParameterSize]{};

    const bool snapshotOk = SnapshotStatusDynamicParameterProbe(
        statusObject,
        &statusID,
        &preDuration,
        &preTimer,
        &generalIndex,
        &generalFloat,
        bytes);

    const EReplicatedBlindnessMode blindnessMode =
        snapshotOk && statusID == ActiveIds::Status_Darkness
        ? DecodeReplicatedBlindnessMode(generalIndex)
        : EReplicatedBlindnessMode::None;

    if (blindnessMode != EReplicatedBlindnessMode::None) {
        const bool isLight =
            blindnessMode == EReplicatedBlindnessMode::Light;

        void* blindnessStatusComponent = nullptr;
        void* blindnessCharacter = nullptr;
        void* blindnessVfxComponent = nullptr;
        if (ResolveReplicatedStatusContext(
                statusObject,
                &blindnessStatusComponent,
                &blindnessCharacter)) {
            blindnessVfxComponent =
                ResolveVisualEffectComponent(blindnessCharacter);
            SetPendingReplicatedLightPresentation(
                blindnessVfxComponent, statusObject, isLight);
        }

        ModLog(
            "[ESE-LIGHT30] CLIENT BLINDNESS BEGIN PRE: Instance=%p Mode=%s "
            "Timer=%.6f GeneralIndex=0x%08X VfxComponent=%p Pending=%s\n",
            statusObject,
            isLight ? "LIGHT" : "DARK",
            preTimer,
            static_cast<unsigned>(generalIndex),
            blindnessVfxComponent,
            blindnessVfxComponent ? "ARMED" : "UNRESOLVED");
        LogOnBeginOrderingProbe("PRE", statusObject);

        {
            FBlindnessSourceScope blindnessScope(
                isLight
                    ? EBlindnessSource::Light
                    : EBlindnessSource::Dark);
            if (Original_OnBeginStatusWrapperProbe) {
                Original_OnBeginStatusWrapperProbe(
                    statusObject,
                    paramOrContext,
                    extraContext);
            }
        }


        LogOnBeginOrderingProbe("POST-BEFORE-RESTORE", statusObject);
        const bool timerRestored =
            RestoreReplicatedStatusTimerAfterBegin(
                statusObject,
                preTimer);
        ModLog(
            "[ESE-LIGHT30] CLIENT BLINDNESS BEGIN POST: Instance=%p Mode=%s "
            "AuthoritativePreTimer=%.6f TimerRestore=%s\n",
            statusObject,
            isLight ? "LIGHT" : "DARK",
            preTimer,
            timerRestored ? "YES" : "NO");
        LogOnBeginOrderingProbe("POST-RESTORED", statusObject);
        return;
    }

    const EEseReplicatedFreezeMode mode =
        snapshotOk && statusID == ActiveIds::Status_Freeze
        ? DecodeReplicatedFreezeMode(generalIndex)
        : EEseReplicatedFreezeMode::None;

    if (mode == EEseReplicatedFreezeMode::None) {
        LogOnBeginOrderingProbe("PRE", statusObject);
        if (Original_OnBeginStatusWrapperProbe) {
            Original_OnBeginStatusWrapperProbe(
                statusObject,
                paramOrContext,
                extraContext);
        }
        LogOnBeginOrderingProbe("POST", statusObject);
        return;
    }

    const bool soft = mode == EEseReplicatedFreezeMode::Soft;
    void* statusComponent = nullptr;
    void* character = nullptr;
    bool softGatesReady = false;
    bool persistentReady = false;

    if (soft) {
        if (ResolveReplicatedStatusContext(
                statusObject,
                &statusComponent,
                &character) &&
            statusComponent &&
            character &&
            InstallPreAddFreezeVirtualHooks(
                statusComponent,
                character)) {
            softGatesReady = true;
            persistentReady = InstallDynamicFreezeVirtualHooks(
                character,
                statusObject,
                true);
            if (!persistentReady) {
                ShipLog(
                    "[ElementalSystemExpanded] ERROR: CLIENT SOFT Freeze could not arm "
                    "character-scoped begin window / persistent Tick gate before OnBegin. "
                    "Instance=%p Character=%p.\n",
                    statusObject,
                    character);
            }
        }
        else {
            ShipLog(
                "[ElementalSystemExpanded] WARNING: CLIENT SOFT Freeze pre-begin "
                "context/gates unavailable; preserving vanilla client behavior. "
                "Instance=%p StatusComponent=%p Character=%p.\n",
                statusObject,
                statusComponent,
                character);
        }
    }

    void* preAIActionFreeze = nullptr;
    const bool preAIActionRead =
        ReadFreezeAIActionPointer(
            statusObject,
            &preAIActionFreeze);

    ModLog(
        "[ESE-AUTH] CLIENT ONBEGIN PRE: Instance=%p Mode=%s "
        "AIActionRead=%s AIActionFreeze=%p Timer=%.6f\n",
        statusObject,
        soft ? "SOFT" : "STRONG",
        preAIActionRead ? "YES" : "NO",
        preAIActionFreeze,
        preTimer);

    ModLog(
        "[ESE-MP] CLIENT FREEZE BEGIN PRE: Instance=%p Mode=%s "
        "Timer=%.6f StatusComponent=%p Character=%p Gates=%s.\n",
        statusObject,
        soft ? "SOFT" : "STRONG",
        preTimer,
        statusComponent,
        character,
        soft ? (softGatesReady ? "READY" : "FAILED_OPEN") : "VANILLA");
    LogOnBeginOrderingProbe("PRE", statusObject);

    const FReplicatedFreezeBeginTLS previous =
        g_ReplicatedFreezeBeginTLS;

    if (!soft || (softGatesReady && persistentReady)) {
        g_ReplicatedFreezeBeginTLS.Active = true;
        g_ReplicatedFreezeBeginTLS.Soft = soft;
        g_ReplicatedFreezeBeginTLS.Status = statusObject;
        g_ReplicatedFreezeBeginTLS.StatusComponent = statusComponent;
        g_ReplicatedFreezeBeginTLS.Character = character;
    }

    if (Original_OnBeginStatusWrapperProbe) {
        Original_OnBeginStatusWrapperProbe(
            statusObject,
            paramOrContext,
            extraContext);
    }

    g_ReplicatedFreezeBeginTLS = previous;

    void* postAIActionFreeze = nullptr;
    const bool postAIActionRead =
        ReadFreezeAIActionPointer(
            statusObject,
            &postAIActionFreeze);
    ModLog(
        "[ESE-AUTH] CLIENT ONBEGIN POST: Instance=%p Mode=%s "
        "AIActionRead=%s AIActionFreeze=%p\n",
        statusObject,
        soft ? "SOFT" : "STRONG",
        postAIActionRead ? "YES" : "NO",
        postAIActionFreeze);

    LogOnBeginOrderingProbe("POST-BEFORE-RESTORE", statusObject);

    const bool timerRestored =
        RestoreReplicatedStatusTimerAfterBegin(
            statusObject,
            preTimer);

    if (!soft) {
        RemoveBlockedFreezeRuntime(statusObject);
    }

    ModLog(
        "[ESE-MP] CLIENT FREEZE BEGIN READY: Instance=%p Mode=%s "
        "AuthoritativePreTimer=%.6f TimerRestore=%s Persistent=%s.\n",
        statusObject,
        soft ? "SOFT" : "STRONG",
        preTimer,
        timerRestored ? "YES" : "NO",
        soft ? ((softGatesReady && persistentReady) ? "READY_BEGIN_WINDOW" : "FAILED_OPEN") : "VANILLA");
    LogOnBeginOrderingProbe("POST-RESTORED", statusObject);
}

static uintptr_t ResolveOnBeginStatusWrapperProbe(uintptr_t moduleBase)
{
    // Reflected UPalStatusBase::OnBeginStatus wrapper shape recovered from the
    // supplied 1.0.4 dump. It updates the wrapper frame through RDX and then
    // tail-dispatches statusObject->vtable[+0x2B0]. No RVA is assumed.
    static const uint8_t pattern[] = {
        0x48,0x8B,0x42,0x20,
        0x45,0x33,0xC0,
        0x48,0x85,0xC0,
        0x41,0x0F,0x95,0xC0,
        0x4C,0x03,0xC0,
        0x4C,0x89,0x42,0x20,
        0x48,0x8B,0x01,
        0x48,0xFF,0xA0,0xB0,0x02,0x00,0x00
    };
    static const char mask[] = "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx";

    const auto matches = FindPatternInExecutableSections(
        moduleBase,
        pattern,
        mask,
        4);

    if (matches.size() != 1) {
        char detail[128]{};
        snprintf(detail, sizeof(detail),
            "global exact wrapper matches=%llu",
            static_cast<unsigned long long>(matches.size()));
        RecordResolver(
            "PalStatusBase::OnBeginStatus.6_7_4_24",
            "global-exact-wrapper-aob",
            EResolveConfidence::Failed,
            0,
            detail);
        return 0;
    }

    RecordResolver(
        "PalStatusBase::OnBeginStatus.6_7_4_24",
        "global-exact-wrapper-aob",
        EResolveConfidence::Strong,
        matches[0],
        "unique exact wrapper ending in vslot +0x2B0; replicated Freeze semantic reconstruction");
    CacheResolvedRva("PalStatusBase::OnBeginStatus.6_7_4_24", matches[0]);
    return matches[0];
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

    g_Resolved_OnBeginStatusWrapper =
        ResolveOnBeginStatusWrapperProbe(base);
    if (!g_Resolved_OnBeginStatusWrapper)
        return Fatal("Stage 6.7.4.24 could not resolve PalStatusBase::OnBeginStatus wrapper safely");

    RecomputeRvaShiftHint();

    // Raw FPalDamageInfo capture is a core semantic dependency, not an optional
    // enhancement. Without it normal 2U attacks would silently degrade to 1U.
    g_DamageEffectCaller = FindEffectCallerStart(base);
    g_Resolved_RawEffectCaller = g_DamageEffectCaller;
    if (g_DamageEffectCaller)
        CacheResolvedRva("RawFPalDamageInfoCaller", g_DamageEffectCaller);
    if (!g_DamageEffectCaller)
        return Fatal("raw FPalDamageInfo caller could not be resolved safely");

    RecordResolver(
        "CAPABILITY.RawUnits.Resolve",
        "capability",
        EResolveConfidence::RuntimeValidated,
        g_DamageEffectCaller,
        "raw EffectValue1/2 source resolved; hook pending");

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

    if (!ResolveDurableVirtualSlots(base))
        return Fatal("required Freeze virtual slots could not be resolved safely");

    // -----------------------------------------------------
    // Phase C: install core status hooks at resolved addresses.
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
            "PalStatusBase::OnBeginStatus.6_7_4_24",
            g_Resolved_OnBeginStatusWrapper,
            reinterpret_cast<void*>(&Detour_OnBeginStatusWrapperProbe),
            reinterpret_cast<void**>(&Original_OnBeginStatusWrapperProbe)
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
        "buildup replacement + native AddStatus + raw units ready");

    // -----------------------------------------------------
    // Phase D: resolve/install complete reaction dependency set.
    // -----------------------------------------------------
    if (!InstallNativeReactionHooks(base))
        return Fatal("native reaction dependency set could not be resolved/installed transactionally");

    RecordResolver(
        "CAPABILITY.WetGatedStrongReactions",
        "capability",
        EResolveConfidence::RuntimeValidated,
        0,
        "all wrapper-backed reaction hooks installed");

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
        "Stage 6.6.3 final shipping + Neutral/Light immunity + elemental status exchange + Darkness/Light 3s cap. Debug diagnostics=%s.\n",
        g_DebugDiagnosticsEnabled ? "ON" : "OFF");

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
        Original_AddElementStatusAdditionalValue_OneType = nullptr;
        Original_NativeAddStatus = nullptr;
        Original_OnBeginStatusWrapperProbe = nullptr;
        AcquireSRWLockExclusive(&g_PendingReplicatedLightPresentationLock);
        g_PendingReplicatedLightPresentation.clear();
        ReleaseSRWLockExclusive(&g_PendingReplicatedLightPresentationLock);
        Original_DamageEffectCaller = nullptr;
        g_DamageEffectCaller = 0;

        MH_Uninitialize();

        InterlockedExchange(&g_StartRequested, 0);
    }
}