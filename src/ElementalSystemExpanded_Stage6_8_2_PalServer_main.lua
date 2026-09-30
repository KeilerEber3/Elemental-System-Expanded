-- ElementalSystemExpanded Stage 6.8.2. PalServer Lua.
-- Dedicated PalServer Lua bootstrap.
--
-- This file intentionally contains no UMG/status-widget/damage-popup presentation
-- hooks.  PalServer is gameplay authority only.
--
-- Server-Lua responsibilities:
--   1) mutate this process's authoritative DT_WazaDataTable exactly once;
--   2) register the custom Light world/camera VFX classes in Palworld's native
--      VisualEffectClassDataAsset so the PalServer C++ native lookup/substitution
--      path can use IDs 57/58 without custom ESE networking;
--   3) trigger those one-shot data mutations from the proven Stage30.4 possession
--      boundaries;
--   4) spawn authoritative Dragon-reaction explosion actors from the reflected
--      native C++ -> BP_ESEBridge dispatch.
--
-- Client presentation remains entirely in the client process.

local UEHelpers = require("UEHelpers")

local _assetRegistryHelpers = nil
local _lightVfxClass = nil
local _lightCameraVfxClass = nil
local _palUtility = nil

local LIGHT_VFX_LOOKUP_ID = 57
local LIGHT_CAMERA_VFX_LOOKUP_ID = 58

local POISON_EFFECT = 3
local MAX_EFFECT_VALUE = 9999

-- Direct raw enum fallback mapping (EPalElementType -> EPalAdditionalEffectType).
-- Internally element 1 remains EPalElementType::Normal; ESE presents Pal skills
-- using that element as Light. Light and Dark intentionally share Darkness (10)
-- as their native status carrier.
local RawElementToStatusMap = {
    [1] = 10, -- Normal/Light -> Darkness carrier
    [2] = 4,  -- Fire         -> Burn
    [3] = 5,  -- Water        -> Wetness
    [4] = 9,  -- Leaf         -> IvyCling
    [5] = 7,  -- Electricity  -> Electrical
    [6] = 6,  -- Ice          -> Freeze
    [7] = 8,  -- Earth        -> Muddy
    [8] = 10, -- Dark         -> Darkness
    [9] = 4,  -- Dragon       -> Burn aggregate
    [0] = 0,  -- None         -> None
}

local function Log(msg)
    print("[ElementalSystemExpanded]: " .. tostring(msg) .. "\n")
end

local function SafeRegisterHook(targetFunction, callback)
    local success, result = pcall(function()
        return RegisterHook(targetFunction, callback)
    end)

    if success and result then
        return true
    end

    Log("ERROR | Failed to register hook on " ..
        tostring(targetFunction) .. " | Error: " .. tostring(result))
    return false
end

local function SafeFullName(obj)
    if not obj then
        return ""
    end

    local ok, value = pcall(function()
        return obj:GetFullName()
    end)

    if ok and value then
        return tostring(value)
    end
    return ""
end

-- ---------------------------------------------------------------------------
-- Mounted Light VFX class registration
-- ---------------------------------------------------------------------------

local function GetAssetRegistryHelpers()
    if _assetRegistryHelpers and _assetRegistryHelpers:IsValid() then
        return _assetRegistryHelpers
    end

    _assetRegistryHelpers =
        StaticFindObject("/Script/AssetRegistry.Default__AssetRegistryHelpers")

    if not _assetRegistryHelpers or not _assetRegistryHelpers:IsValid() then
        Log("ERROR: AssetRegistryHelpers could not be found.")
        return nil
    end

    return _assetRegistryHelpers
end

local function GetMountedAsset(PackageName, AssetName)
    local Helpers = GetAssetRegistryHelpers()
    if not Helpers then
        return nil
    end

    local AssetData = {
        ["PackageName"] = UEHelpers.FindOrAddFName(PackageName),
        ["AssetName"]   = UEHelpers.FindOrAddFName(AssetName),
    }

    local Asset = Helpers:GetAsset(AssetData)
    if Asset and Asset:IsValid() then
        return Asset
    end

    Log(string.format(
        "ERROR: AssetRegistry failed: Package='%s' Asset='%s'",
        PackageName,
        AssetName))
    return nil
end

local function GetLightVFXBP()
    if not _lightVfxClass or not _lightVfxClass:IsValid() then
        _lightVfxClass = GetMountedAsset(
            "/Game/Mods/ElementalSystemExpanded/LightVFX/BP_VisualEffect_Status_Light",
            "BP_VisualEffect_Status_Light_C")
    end
    return _lightVfxClass
end

local function GetLightCameraVFXBP()
    if not _lightCameraVfxClass or not _lightCameraVfxClass:IsValid() then
        _lightCameraVfxClass = GetMountedAsset(
            "/Game/Mods/ElementalSystemExpanded/LightVFX/BP_VisualEffect_CameraLightVignette",
            "BP_VisualEffect_CameraLightVignette_C")
    end
    return _lightCameraVfxClass
end

local function GetPalUtility()
    if _palUtility and _palUtility:IsValid() then
        return _palUtility
    end

    _palUtility = StaticFindObject("/Script/Pal.Default__PalUtility")
    if not _palUtility or not _palUtility:IsValid() then
        Log("ERROR: Default__PalUtility could not be found.")
        return nil
    end

    return _palUtility
end

local function RegisterLightVFXDatabaseClass(WorldContextObject)
    if _G.__ESE_LightVfxDatabaseRegistered then
        return true
    end

    if not WorldContextObject or not WorldContextObject:IsValid() then
        Log("ERROR: Light VFX database registration has no valid world context.")
        return false
    end

    local LightClass = GetLightVFXBP()
    if not LightClass or not LightClass:IsValid() then
        Log("ERROR: Light VFX database registration has no valid Light class.")
        return false
    end

    local PalUtility = GetPalUtility()
    if not PalUtility then
        return false
    end

    local VisualEffectDatabase =
        PalUtility:GetVisualEffectDatabase(WorldContextObject)

    if not VisualEffectDatabase or not VisualEffectDatabase:IsValid() then
        Log("ERROR: PalUtility:GetVisualEffectDatabase returned no valid database.")
        return false
    end

    local ok, err = pcall(function()
        local ClassMap = VisualEffectDatabase.VisualEffectClassDataAsset
        ClassMap:Add(LIGHT_VFX_LOOKUP_ID, LightClass)
    end)

    if not ok then
        Log("ERROR: Light VFX database registration failed: " .. tostring(err))
        return false
    end

    _G.__ESE_LightVfxDatabaseRegistered = true
    return true
end

-- PalServer C++ still contains the shared native CameraVignette substitution helper.
-- The dedicated process has no visible camera, but keeping lookup 58 registered
-- preserves the already-validated native lifecycle/fallback behavior without
-- installing any server-side UI logic.
local function RegisterLightCameraVFXDatabaseClass(WorldContextObject)
    if _G.__ESE_LightCameraVfxDatabaseRegistered then
        return true
    end

    if not WorldContextObject or not WorldContextObject:IsValid() then
        Log("ERROR: Light camera VFX database registration has no valid world context.")
        return false
    end

    local LightCameraClass = GetLightCameraVFXBP()
    if not LightCameraClass or not LightCameraClass:IsValid() then
        Log("ERROR: Light camera VFX database registration has no valid class.")
        return false
    end

    local PalUtility = GetPalUtility()
    if not PalUtility then
        return false
    end

    local VisualEffectDatabase =
        PalUtility:GetVisualEffectDatabase(WorldContextObject)

    if not VisualEffectDatabase or not VisualEffectDatabase:IsValid() then
        Log("ERROR: Light camera VFX registration could not get VisualEffectDatabase.")
        return false
    end

    local ok, err = pcall(function()
        local ClassMap = VisualEffectDatabase.VisualEffectClassDataAsset
        ClassMap:Add(LIGHT_CAMERA_VFX_LOOKUP_ID, LightCameraClass)
    end)

    if not ok then
        Log("ERROR: Light camera VFX database registration failed: " .. tostring(err))
        return false
    end

    _G.__ESE_LightCameraVfxDatabaseRegistered = true
    return true
end

-- ---------------------------------------------------------------------------
-- Authoritative Waza-table mutation
-- ---------------------------------------------------------------------------

local function DoublePoisonSlot(RowData, effectField, valueField)
    if RowData[effectField] ~= POISON_EFFECT then
        return false
    end

    local oldValue = tonumber(RowData[valueField]) or 0
    local newValue = math.min(oldValue * 2, MAX_EFFECT_VALUE)
    RowData[valueField] = newValue
    return newValue ~= oldValue
end

local function InjectElementalStatus(RowData, targetStatusValue)
    local units = (RowData.Power and RowData.Power > 120) and 2 or 1

    -- Poison is orthogonal to elemental statuses. Preserve a Poison slot whenever
    -- one exists and place the elemental carrier in the other slot. If both slots
    -- are Poison, slot 2 becomes elemental and slot 1 remains Poison.
    if RowData.EffectType1 == POISON_EFFECT then
        RowData.EffectType2 = targetStatusValue
        RowData.EffectValue2 = units
        return 2
    end

    RowData.EffectType1 = targetStatusValue
    RowData.EffectValue1 = units
    return 1
end

local function BuildPalSkillSet(WorldContextObject)
    if not WorldContextObject or not WorldContextObject:IsValid() then
        Log("ERROR: Cannot build Pal skill set without a valid world context.")
        return nil
    end

    local PalUtility = GetPalUtility()
    if not PalUtility then
        return nil
    end

    local WazaDatabase = PalUtility:GetWazaDatabase(WorldContextObject)
    if not WazaDatabase or not WazaDatabase:IsValid() then
        Log("ERROR: PalUtility:GetWazaDatabase returned no valid database.")
        return nil
    end

    local LevelTable = WazaDatabase.WazaMasterLevel_DataTable
    local TamagoTable = WazaDatabase.WazaMasterTamago_DataTable

    if not LevelTable or not LevelTable:IsValid() then
        Log("ERROR: WazaMasterLevel_DataTable is unavailable.")
        return nil
    end

    if not TamagoTable or not TamagoTable:IsValid() then
        Log("ERROR: WazaMasterTamago_DataTable is unavailable.")
        return nil
    end

    local PalSkillSet = {}
    local uniqueCount = 0

    local function AddWazaID(rawID)
        local id = tonumber(rawID)
        if id == nil then
            return
        end

        if not PalSkillSet[id] then
            PalSkillSet[id] = true
            uniqueCount = uniqueCount + 1
        end
    end

    LevelTable:ForEachRow(function(RowName, RowData)
        AddWazaID(RowData.WazaID)
    end)

    TamagoTable:ForEachRow(function(RowName, RowData)
        AddWazaID(RowData.WazaID)
    end)

    if uniqueCount == 0 then
        Log("ERROR: Pal skill set resolved to zero Waza IDs; refusing Light mutation.")
        return nil
    end

    return PalSkillSet
end

local function InjectSkillStatusUnits(WorldContextObject)
    if _G.__ESE_WazaMutationApplied then
        return true
    end

    local tablePath =
        "/Game/Pal/DataTable/Waza/DT_WazaDataTable.DT_WazaDataTable"
    local WazaTable =
        StaticFindObject(tablePath) or StaticLoadObject(tablePath)

    if not WazaTable or not WazaTable:IsValid() then
        Log("ERROR: DT_WazaDataTable could not be found or loaded.")
        return false
    end

    local PalSkillSet = BuildPalSkillSet(WorldContextObject)
    if not PalSkillSet then
        -- Fail closed: do not mark the mutation complete. A later possession
        -- event may retry after Palworld's databases are ready.
        return false
    end

    local elementalModifiedCount = 0
    local poisonModifiedCount = 0
    local lightModifiedCount = 0
    local normalNonPalSkippedCount = 0

    WazaTable:ForEachRow(function(RowName, RowData)
        -- Poison adjustment is independent of element and must happen only once.
        if DoublePoisonSlot(RowData, "EffectType1", "EffectValue1") then
            poisonModifiedCount = poisonModifiedCount + 1
        end
        if DoublePoisonSlot(RowData, "EffectType2", "EffectValue2") then
            poisonModifiedCount = poisonModifiedCount + 1
        end

        local rawElem = tonumber(RowData.Element)
        -- None has no elemental status. Dragon receives its Burn carrier.
        if not rawElem or rawElem == 0 then
            return
        end

        -- Normal is ESE Light only for real Pal Waza IDs. Player/basic/internal
        -- Normal attacks such as Human_Punch remain ordinary Normal.
        if rawElem == 1 then
            local rawWazaType = tonumber(RowData.WazaType)
            if not rawWazaType or not PalSkillSet[rawWazaType] then
                normalNonPalSkippedCount =
                    normalNonPalSkippedCount + 1
                return
            end
        end

        local targetStatusValue = RawElementToStatusMap[rawElem]
        if not targetStatusValue or targetStatusValue == 0 then
            return
        end

        InjectElementalStatus(RowData, targetStatusValue)
        elementalModifiedCount = elementalModifiedCount + 1
        if rawElem == 1 then
            lightModifiedCount = lightModifiedCount + 1
        end
    end)

    -- Poison multiplication is not idempotent, so commit only after the complete
    -- table pass succeeds. Both Stage30.4 possession hooks share this flag.
    _G.__ESE_WazaMutationApplied = true

    Log(string.format(
        "Waza mutation complete: elemental=%d lightPalSkills=%d " ..
        "normalNonPalSkipped=%d poisonSlotsDoubled=%d.",
        elementalModifiedCount,
        lightModifiedCount,
        normalNonPalSkippedCount,
        poisonModifiedCount))
    return true
end

-- ---------------------------------------------------------------------------
-- Proven Stage30.4 possession bootstrap
-- ---------------------------------------------------------------------------

local function EnsureServerNativeData(WorldContextObject, reason)
    if not WorldContextObject or not WorldContextObject:IsValid() then
        return false
    end

    local worldVfxOK =
        RegisterLightVFXDatabaseClass(WorldContextObject)
    local cameraVfxOK =
        RegisterLightCameraVFXDatabaseClass(WorldContextObject)

    if worldVfxOK and cameraVfxOK then
        return true
    end

    Log("Server native-data bootstrap incomplete at " .. tostring(reason) .. ".")
    return false
end

local function RunServerBootstrap(WorldContextObject, reason)
    if not WorldContextObject or not WorldContextObject:IsValid() then
        return
    end

    if not _G.__ESE_WazaMutationApplied then
        local ok = InjectSkillStatusUnits(WorldContextObject)
        if ok then
            Log("Waza mutation triggered by " .. tostring(reason) .. ".")
        end
    end

    EnsureServerNativeData(WorldContextObject, reason)
end

local function BootstrapFromControllerHook(Context, reason)
    local controller = Context:get()
    if not controller or not controller:IsValid() then
        Log("ERROR: " .. tostring(reason) ..
            " provided no valid controller context.")
        return
    end

    RunServerBootstrap(controller, reason)
end

local function IsPalPlayerControllerObject(controller)
    if not controller or not controller:IsValid() then
        return false
    end

    local name = SafeFullName(controller)
    if name == "" then
        return false
    end

    if string.find(name, "PalPlayerController", 1, true) then
        return true
    end

    -- Defensive fallback for Blueprint subclasses whose generated name omits the
    -- native Pal prefix. Never accept AI controllers.
    return string.find(name, "PlayerController", 1, true) ~= nil and
           string.find(name, "AIController", 1, true) == nil
end

-- Preserve the proven Stage30.4 pair. Whichever authority-side possession
-- boundary is observed first can perform the one-shot Waza/database bootstrap.
SafeRegisterHook(
    "/Script/Engine.PlayerController:ServerAcknowledgePossession",
    function(Context)
        BootstrapFromControllerHook(
            Context,
            "ServerAcknowledgePossession")
    end)

SafeRegisterHook(
    "/Script/Engine.Controller:ReceivePossess",
    function(Context, PossessedPawn)
        local controller = Context:get()
        if not IsPalPlayerControllerObject(controller) then
            return
        end

        RunServerBootstrap(
            controller,
            "Controller.ReceivePossess")
    end)

-- ---------------------------------------------------------------------------
-- Dragon reaction reflected bridge
-- ---------------------------------------------------------------------------
-- C++ owns reaction detection/consumption and calls the reflected
-- BP_ESEBridge::DispatchDragonReaction UFunction through the proven detached
-- Blueprint execution-stub replay. Lua hooks that exact UFunction and performs
-- only the delayed authoritative explosion
-- spawn/configuration. Reaction AOE uses a valid positive native EffectValue=1;
-- exact 3.0 s provenance lives only in C++'s in-process spread context.
-- Dedicated/listen authority additionally authorizes only the authenticated synthetic AOE
-- packet through Palworld's dedicated CanProcessDamage gate.
--
-- BP path supplied by the project:
--   /Game/Mods/ElementalSystemExpanded/BP_ESEBridge
--

local DRAGON_SPAWN_DELAY_MS = 50
local DRAGON_SPREAD_EFFECT_VALUE = 1

local DRAGON_BRIDGE_PACKAGE =
    "/Game/Mods/ElementalSystemExpanded/BP_ESEBridge"
local DRAGON_BRIDGE_ASSET = "BP_ESEBridge_C"
local DRAGON_BRIDGE_FUNCTION =
    "/Game/Mods/ElementalSystemExpanded/BP_ESEBridge.BP_ESEBridge_C:DispatchDragonReaction"

-- Must exactly match the C++ reflected bridge bootstrap tuple. These values never
-- enter gameplay; they are used only by a harmless empty bridge-function call
-- so native C++ can capture the live UObject + UFunction pair without object
-- enumeration or textual pointer lookup.
local DRAGON_BRIDGE_BOOTSTRAP_REACTION = 0x45534231
local DRAGON_BRIDGE_BOOTSTRAP_UNITS = 0x01234567
local DRAGON_BRIDGE_BOOTSTRAP_POWER = 12345.25
local DRAGON_BRIDGE_BOOTSTRAP_ATTACKTYPE = 0x00001357
local DRAGON_BRIDGE_BOOTSTRAP_SEQUENCE = 0x02468ACE
local DRAGON_BRIDGE_LOCAL_VALIDATION_REACTION = 0x45534232
local DRAGON_BRIDGE_DETACHED_VALIDATION_REACTION = 0x45534233

local DRAGON_EXPLOSION_PACKAGE =
    "/Game/Mods/ElementalSystemExpanded/DragonReactions/BP_DragonExplosion"
local DRAGON_EXPLOSION_ASSET = "BP_DragonExplosion_C"

local _dragonBridgeClass = nil
local _dragonBridgeInstance = nil
local _dragonBridgeHookInstalled = false
local _dragonBridgeLiveObserved = false
local _dragonExplosionClass = nil
local _dragonGameplayStatics = nil
local _dragonRuntimePreflightReady = false
local _dragonRuntimePreflightLastFailure = nil
local _dragonSeenSequences = {}
local _dragonSequenceOrder = {}
local DRAGON_SEQUENCE_CACHE_MAX = 128

-- Must match /DragonReactions/E_DragonExplosionVisual.
local DRAGON_VISUAL_FIRE     = 1
local DRAGON_VISUAL_WATER    = 2
local DRAGON_VISUAL_ICE      = 3
local DRAGON_VISUAL_ELECTRIC = 4

local function DragonUnwrap(value)
    if value == nil then
        return nil
    end

    local ok, result = pcall(function()
        if value.get then
            return value:get()
        end
        if value.Get then
            return value:Get()
        end
        return value
    end)

    return ok and result or value
end

local function DragonScalarNumber(value)
    value = DragonUnwrap(value)
    if value == nil then
        return nil
    end

    local direct = tonumber(value)
    if direct ~= nil then
        return direct
    end

    local result = nil
    pcall(function()
        if value.GetValue then
            result = tonumber(value:GetValue())
        elseif value.get then
            result = tonumber(value:get())
        end
    end)
    return result
end

local function DragonFiniteNumber(value)
    local n = DragonScalarNumber(value)
    if n == nil or n ~= n or n == math.huge or n == -math.huge then
        return nil
    end
    return n
end

local function DragonActorAuthority(actor)
    if not actor or not actor:IsValid() then
        return false
    end

    local ok, authority = pcall(function()
        return actor:HasAuthority()
    end)
    return ok and authority == true
end

local function DragonHasDelayedScheduler()
    return type(ExecuteInGameThreadWithDelay) == "function" or
           type(ExecuteWithDelay) == "function"
end

local function GetDragonGameplayStatics()
    if _dragonGameplayStatics and _dragonGameplayStatics:IsValid() then
        return _dragonGameplayStatics
    end

    _dragonGameplayStatics =
        StaticFindObject("/Script/Engine.Default__GameplayStatics")

    if not _dragonGameplayStatics or not _dragonGameplayStatics:IsValid() then
        Log("DRAGON: GameplayStatics could not be resolved.")
        return nil
    end

    return _dragonGameplayStatics
end

local function GetDragonBridgeClass()
    if _dragonBridgeClass and _dragonBridgeClass:IsValid() then
        return _dragonBridgeClass
    end

    _dragonBridgeClass = GetMountedAsset(
        DRAGON_BRIDGE_PACKAGE,
        DRAGON_BRIDGE_ASSET)

    if not _dragonBridgeClass or not _dragonBridgeClass:IsValid() then
        Log("DRAGON BRIDGE: BP_ESEBridge_C could not be resolved at " ..
            DRAGON_BRIDGE_PACKAGE)
        return nil
    end

    Log("DRAGON BRIDGE: resolved " ..
        tostring(_dragonBridgeClass:GetFullName()))
    return _dragonBridgeClass
end

local function GetDragonExplosionClass()
    if _dragonExplosionClass and _dragonExplosionClass:IsValid() then
        return _dragonExplosionClass
    end

    _dragonExplosionClass = GetMountedAsset(
        DRAGON_EXPLOSION_PACKAGE,
        DRAGON_EXPLOSION_ASSET)

    if not _dragonExplosionClass or not _dragonExplosionClass:IsValid() then
        Log("DRAGON: BP_DragonExplosion_C could not be resolved.")
        return nil
    end

    Log("DRAGON: resolved " .. tostring(_dragonExplosionClass:GetFullName()))
    return _dragonExplosionClass
end

local function DragonReactionConfig(reaction)
    -- Returns Element, Effect1, Visual, Effect2, EffectValue2.
    if reaction == 1 then
        -- Wet + Dragon -> Steam Burst -> Water/Wetness.
        return 3, 5, DRAGON_VISUAL_WATER, 0, 0
    elseif reaction == 2 then
        -- Muddy + Dragon -> Lava Burst -> Fire/Burn.
        return 2, 4, DRAGON_VISUAL_FIRE, 0, 0
    elseif reaction == 3 then
        -- IvyCling + Dragon -> Wildfire -> Fire/Burn.
        return 2, 4, DRAGON_VISUAL_FIRE, 0, 0
    elseif reaction == 4 then
        -- Burn + Dragon -> Flashover -> Fire/Burn + native Stun.
        return 2, 4, DRAGON_VISUAL_FIRE, 1, 100
    elseif reaction == 5 then
        -- Freeze + Dragon -> Thermal Shock -> Ice/Freeze.
        return 6, 6, DRAGON_VISUAL_ICE, 0, 0
    elseif reaction == 6 then
        -- Electrical + Dragon -> Arc Burst -> Electricity/Electrical.
        return 5, 7, DRAGON_VISUAL_ELECTRIC, 0, 0
    end

    return nil
end



-- Dedicated synthetic-damage authorization.
--
-- Runtime proved that ESE-created reaction explosions reach
-- PalUtility::ProcessDamageAndPlayEffectsByDamageInfo with a complete
-- FPalDamageInfo but dedicated rejects the synthetic second hit through the
-- native CanProcessDamage gate. FPalDamageInfo exposes IgnoreCanProcessDamage
-- even though FPalMakeDamageInfo does not, so arm a short-lived ESE-owned
-- provenance context and set exactly that one field in the ProcessDamage PRE
-- hook. Ordinary Palworld damage is never intentionally modified.
local DRAGON_CANPROCESS_BYPASS_TTL_MS = 750
local _dragonCanProcessBypassContexts = {}

local function DragonRawField(info, field)
    if not info then
        return nil
    end
    local raw = nil
    local ok = pcall(function()
        raw = info[field]
    end)
    if not ok then
        return nil
    end
    return DragonUnwrap(raw)
end

local function DragonStructNumber(info, field)
    return DragonScalarNumber(DragonRawField(info, field))
end

local function ClearDragonCanProcessBypassContext(sequence)
    local ctx = _dragonCanProcessBypassContexts[sequence]
    if not ctx then
        return
    end
    _dragonCanProcessBypassContexts[sequence] = nil
end

local function ArmDragonCanProcessBypassContext(
    attacker,
    reaction,
    attackPower,
    attackType,
    sequence,
    element,
    effect1,
    effect2,
    effectValue2)

    if _G.__ESE_DragonCanProcessBypassHookInstalled ~= true or
       not DragonHasDelayedScheduler() then
        return false
    end

    if not attacker or not attacker:IsValid() then
        return false
    end

    local attackerName = SafeFullName(attacker)
    if attackerName == "" then
        return false
    end

    local ctx = {
        sequence = math.floor(sequence or 0),
        reaction = math.floor(reaction or 0),
        attackerName = attackerName,
        power = math.floor(attackPower or 0),
        attackType = math.floor(attackType or 0),
        element = math.floor(element or 0),
        effect1 = math.floor(effect1 or 0),
        value1 = DRAGON_SPREAD_EFFECT_VALUE,
        effect2 = math.floor(effect2 or 0),
        value2 = math.floor(effectValue2 or 0),
    }

    if ctx.sequence <= 0 or ctx.power <= 0 or
       ctx.element <= 0 or ctx.effect1 <= 0 then
        return false
    end

    _dragonCanProcessBypassContexts[ctx.sequence] = ctx

    local function Expire()
        if _dragonCanProcessBypassContexts[ctx.sequence] == ctx then
            ClearDragonCanProcessBypassContext(ctx.sequence)
        end
    end

    local scheduled = false
    if type(ExecuteInGameThreadWithDelay) == "function" then
        local ok = pcall(function()
            ExecuteInGameThreadWithDelay(
                DRAGON_CANPROCESS_BYPASS_TTL_MS,
                Expire)
        end)
        scheduled = ok
    elseif type(ExecuteWithDelay) == "function" then
        local ok = pcall(function()
            ExecuteWithDelay(
                DRAGON_CANPROCESS_BYPASS_TTL_MS,
                function()
                    if type(ExecuteInGameThread) == "function" then
                        ExecuteInGameThread(Expire)
                    else
                        Expire()
                    end
                end)
        end)
        scheduled = ok
    end

    if not scheduled then
        _dragonCanProcessBypassContexts[ctx.sequence] = nil
        Log("DRAGON CANPROCESS BYPASS ARM FAILED: no usable expiry scheduler. Seq=" ..
            tostring(ctx.sequence))
        return false
    end

    return true
end

local function TryApplyDragonCanProcessBypass(AttackerParam, DamageInfoParam)
    local info = DragonUnwrap(DamageInfoParam)
    if not info then
        return
    end

    if DragonRawField(info, "bIsExplosionDamage") ~= true then
        return
    end

    local attacker = DragonUnwrap(AttackerParam)
    local attackerName = SafeFullName(attacker)
    if attackerName == "" then
        return
    end

    local power = math.floor(DragonStructNumber(info, "BasePower") or 0)
    local attackType = math.floor(DragonStructNumber(info, "AttackType") or 0)
    local element = math.floor(DragonStructNumber(info, "AttackElementType") or 0)
    local effect1 = math.floor(DragonStructNumber(info, "EffectType1") or 0)
    local value1 = math.floor(DragonStructNumber(info, "EffectValue1") or 0)
    local effect2 = math.floor(DragonStructNumber(info, "EffectType2") or 0)
    local value2 = math.floor(DragonStructNumber(info, "EffectValue2") or 0)

    local bestSequence = 0
    local best = nil
    for sequence, ctx in pairs(_dragonCanProcessBypassContexts) do
        if ctx.attackerName == attackerName and
           ctx.power == power and
           ctx.attackType == attackType and
           ctx.element == element and
           ctx.effect1 == effect1 and
           ctx.value1 == value1 and
           ctx.effect2 == effect2 and
           ctx.value2 == value2 and
           sequence > bestSequence then
            bestSequence = sequence
            best = ctx
        end
    end

    if not best then
        return
    end

    local okWrite, writeErr = pcall(function()
        info["IgnoreCanProcessDamage"] = true
    end)
    local after = DragonRawField(info, "IgnoreCanProcessDamage")

    if okWrite and after == true then
        return
    end

    Log(string.format(
        "DRAGON CANPROCESS BYPASS WRITE FAILED: Seq=%d Error=%s After=%s",
        best.sequence,
        tostring(writeErr),
        tostring(after)))
end

local function RememberDragonSequence(sequence)
    if not sequence then
        return false
    end
    sequence = math.floor(sequence)
    if sequence <= 0 then
        return false
    end
    if _dragonSeenSequences[sequence] then
        return false
    end

    _dragonSeenSequences[sequence] = true
    table.insert(_dragonSequenceOrder, sequence)
    if #_dragonSequenceOrder > DRAGON_SEQUENCE_CACHE_MAX then
        local old = table.remove(_dragonSequenceOrder, 1)
        _dragonSeenSequences[old] = nil
    end
    return true
end

local function SpawnDragonReactionExplosion(
    attacker,
    defender,
    reaction,
    units,
    attackPower,
    attackType,
    sequence)

    if not defender or not defender:IsValid() then
        Log("DRAGON: deferred spawn canceled; Defender is no longer valid. Seq=" ..
            tostring(sequence))
        return false
    end

    if not DragonActorAuthority(defender) then
        return false
    end

    if not attacker or not attacker:IsValid() then
        Log("DRAGON: deferred spawn canceled; Attacker is no longer valid. Seq=" ..
            tostring(sequence))
        return false
    end

    local worldContext = attacker
    if not worldContext or not worldContext:IsValid() then
        worldContext = defender
    end

    local explosionClass = GetDragonExplosionClass()
    local gameplayStatics = GetDragonGameplayStatics()
    if not explosionClass or not gameplayStatics then
        return false
    end

    local element, effect1, visual, effect2, effectValue2 =
        DragonReactionConfig(reaction)
    if not element then
        return false
    end

    local radius = (units >= 2) and 800.0 or 500.0

    local spawnTransform = nil
    local okTransform = pcall(function()
        spawnTransform = defender:GetTransform()
    end)
    if not okTransform or not spawnTransform then
        Log("DRAGON: defender transform unavailable at deferred spawn. Seq=" ..
            tostring(sequence))
        return false
    end

    local deferredActor = nil
    local okBegin, beginErr = pcall(function()
        deferredActor = gameplayStatics:BeginDeferredActorSpawnFromClass(
            worldContext,
            explosionClass,
            spawnTransform,
            1,
            attacker)
    end)

    if not okBegin or not deferredActor or not deferredActor:IsValid() then
        Log("DRAGON: BeginDeferredActorSpawnFromClass failed after delay: " ..
            tostring(beginErr) .. " Seq=" .. tostring(sequence))
        return false
    end

    local explosion = nil
    local okFinish, finishErr = pcall(function()
        explosion = gameplayStatics:FinishSpawningActor(
            deferredActor,
            spawnTransform)
    end)

    if not okFinish or not explosion or not explosion:IsValid() then
        Log("DRAGON: FinishSpawningActor failed after delay: " ..
            tostring(finishErr) .. " Seq=" .. tostring(sequence))
        return false
    end

    -- ConfigureExplosion can synchronously enable collision and invoke the
    -- native damage path, so the dedicated CanProcess bypass must already be
    -- armed before entering the reflected ConfigureExplosion function.
    if not ArmDragonCanProcessBypassContext(
            attacker,
            reaction,
            attackPower,
            attackType,
            sequence,
            element,
            effect1,
            effect2,
            effectValue2) then
        Log("DRAGON: reaction carrier discarded because CanProcess bypass context could not arm. Seq=" ..
            tostring(sequence))
        pcall(function()
            explosion:K2_DestroyActor()
        end)
        return false
    end

    local okConfigure, configureErr = pcall(function()
        -- BP input order:
        -- Attacker, Power, Radius, Element, Effect1, Value1,
        -- AttackType, VisualType, Effect2, Value2.
        -- Value1 is now an ordinary positive native magnitude. C++ decides
        -- exact 3.0 s spread semantics from its separate in-process context.
        explosion:ConfigureExplosion(
            attacker,
            attackPower,
            radius,
            element,
            effect1,
            DRAGON_SPREAD_EFFECT_VALUE,
            attackType,
            visual,
            effect2,
            effectValue2)
    end)

    if not okConfigure then
        ClearDragonCanProcessBypassContext(sequence)
        Log("DRAGON: ConfigureExplosion failed: " .. tostring(configureErr) ..
            " Seq=" .. tostring(sequence))
        pcall(function()
            explosion:K2_DestroyActor()
        end)
        return false
    end

    pcall(function()
        explosion:ForceNetUpdate()
    end)

    Log(string.format(
        "DRAGON: spawned Seq=%d reaction=%d units=%d radius=%.1f " ..
        "element=%d effect1=%d value1=%d effect2=%d power=%.3f visual=%d delayMs=%d",
        sequence,
        reaction,
        units,
        radius,
        element,
        effect1,
        DRAGON_SPREAD_EFFECT_VALUE,
        effect2,
        attackPower,
        visual,
        DRAGON_SPAWN_DELAY_MS))

    return true
end

local function QueueDragonReactionExplosion(
    attacker,
    defender,
    reaction,
    units,
    attackPower,
    attackType,
    sequence)

    local function DeferredSpawn()
        SpawnDragonReactionExplosion(
            attacker,
            defender,
            reaction,
            units,
            attackPower,
            attackType,
            sequence)
    end

    -- Preserve the validated teardown spacing. The bridge itself is synchronous,
    -- but the explosion remains delayed so the consumed 0.001 s status sentinel
    -- can be removed by native TickStatus before same-status spread reaches the
    -- primary target again.
    if type(ExecuteInGameThreadWithDelay) == "function" then
        local ok, err = pcall(function()
            ExecuteInGameThreadWithDelay(
                DRAGON_SPAWN_DELAY_MS,
                DeferredSpawn)
        end)
        if ok then
            return true
        end
        Log("DRAGON: ExecuteInGameThreadWithDelay failed: " .. tostring(err))
    end

    if type(ExecuteWithDelay) == "function" then
        local ok, err = pcall(function()
            ExecuteWithDelay(
                DRAGON_SPAWN_DELAY_MS,
                function()
                    if type(ExecuteInGameThread) == "function" then
                        ExecuteInGameThread(DeferredSpawn)
                    else
                        DeferredSpawn()
                    end
                end)
        end)
        if ok then
            return true
        end
        Log("DRAGON: ExecuteWithDelay fallback failed: " .. tostring(err))
    end

    if type(ExecuteInGameThread) == "function" then
        local ok, err = pcall(function()
            ExecuteInGameThread(function()
                ExecuteInGameThread(DeferredSpawn)
            end)
        end)
        if ok then
            Log("DRAGON: WARNING: no delayed game-thread helper; using two-turn fallback.")
            return true
        end
        Log("DRAGON: ExecuteInGameThread fallback failed: " .. tostring(err))
    end

    Log("DRAGON: ERROR: no safe deferred-execution helper; reaction spawn discarded.")
    return false
end

local HandleDragonBridgeDispatch = nil

local function InstallDragonBridgeHook()
    if _dragonBridgeHookInstalled then
        return true
    end

    if not GetDragonBridgeClass() then
        return false
    end

    local ok, hookIdOrErr = pcall(function()
        return RegisterHook(
            DRAGON_BRIDGE_FUNCTION,
            function(
                Context,
                Attacker,
                Defender,
                Reaction,
                Units,
                AttackPower,
                AttackType,
                Sequence)
                if HandleDragonBridgeDispatch then
                    HandleDragonBridgeDispatch(
                        Context,
                        Attacker,
                        Defender,
                        Reaction,
                        Units,
                        AttackPower,
                        AttackType,
                        Sequence)
                end
            end)
    end)

    if not ok or hookIdOrErr == nil then
        Log("DRAGON BRIDGE: RegisterHook failed for " ..
            DRAGON_BRIDGE_FUNCTION .. " | " .. tostring(hookIdOrErr))
        return false
    end

    _dragonBridgeHookInstalled = true
    Log("DRAGON BRIDGE: DispatchDragonReaction hook registered.")
    return true
end

local function DragonRuntimePreflight()
    if _dragonRuntimePreflightReady then
        return true
    end

    local function Fail(reason)
        if _dragonRuntimePreflightLastFailure ~= reason then
            Log("DRAGON RUNTIME PREFLIGHT FAIL-CLOSED: " .. tostring(reason))
            _dragonRuntimePreflightLastFailure = reason
        end
        return false
    end

    if _G.__ESE_DragonCanProcessBypassHookInstalled ~= true then
        return Fail("CanProcess PRE-hook is not installed")
    end

    if not DragonHasDelayedScheduler() then
        return Fail("no delayed scheduler is available")
    end

    if not GetDragonGameplayStatics() then
        return Fail("GameplayStatics is unavailable")
    end

    if not GetDragonExplosionClass() then
        return Fail("BP_DragonExplosion_C is unavailable")
    end

    if not InstallDragonBridgeHook() then
        return Fail("BP_ESEBridge hook/class is unavailable")
    end

    _dragonRuntimePreflightReady = true
    _dragonRuntimePreflightLastFailure = nil
    Log("DRAGON RUNTIME PREFLIGHT: READY; ESB1 bootstrap permitted.")
    return true
end

local function EnsureDragonBridgeInstance(worldContext)
    if _dragonBridgeInstance and _dragonBridgeInstance:IsValid() then
        return _dragonBridgeInstance
    end

    if not worldContext or not worldContext:IsValid() or
       not DragonActorAuthority(worldContext) then
        return nil
    end

    local bridgeClass = GetDragonBridgeClass()
    local gameplayStatics = GetDragonGameplayStatics()
    if not bridgeClass or not gameplayStatics or not InstallDragonBridgeHook() then
        return nil
    end

    local transform = nil
    local okTransform = pcall(function()
        transform = worldContext:GetTransform()
    end)
    if not okTransform or not transform then
        Log("DRAGON BRIDGE: world-context transform unavailable.")
        return nil
    end

    local deferredActor = nil
    local okBegin, beginErr = pcall(function()
        deferredActor = gameplayStatics:BeginDeferredActorSpawnFromClass(
            worldContext,
            bridgeClass,
            transform,
            1,
            worldContext)
    end)
    if not okBegin or not deferredActor or not deferredActor:IsValid() then
        Log("DRAGON BRIDGE: BeginDeferredActorSpawnFromClass failed: " ..
            tostring(beginErr))
        return nil
    end

    local bridge = nil
    local okFinish, finishErr = pcall(function()
        bridge = gameplayStatics:FinishSpawningActor(
            deferredActor,
            transform)
    end)
    if not okFinish or not bridge or not bridge:IsValid() then
        Log("DRAGON BRIDGE: FinishSpawningActor failed: " ..
            tostring(finishErr))
        return nil
    end

    _dragonBridgeInstance = bridge
    Log("DRAGON BRIDGE: spawned authority-local instance " ..
        tostring(bridge:GetFullName()))
    return bridge
end

local function IssueDragonBridgeBootstrap(worldContext)
    if _dragonBridgeLiveObserved then
        return true
    end

    -- ESB1 is the native bridge authorization bootstrap. Do not issue it until
    -- every Lua-side facility required to complete a reaction is already ready.
    -- If an update breaks any of these dependencies, C++ never captures/validates
    -- the bridge and therefore leaves eligible Dragon hits as ordinary Burn.
    if not DragonRuntimePreflight() then
        return false
    end

    local bridge = EnsureDragonBridgeInstance(worldContext)
    if not bridge then
        return false
    end

    local ok, err = pcall(function()
        bridge:DispatchDragonReaction(
            worldContext,
            worldContext,
            DRAGON_BRIDGE_BOOTSTRAP_REACTION,
            DRAGON_BRIDGE_BOOTSTRAP_UNITS,
            DRAGON_BRIDGE_BOOTSTRAP_POWER,
            DRAGON_BRIDGE_BOOTSTRAP_ATTACKTYPE,
            DRAGON_BRIDGE_BOOTSTRAP_SEQUENCE)
    end)

    if not ok then
        Log("DRAGON BRIDGE: bootstrap call failed: " .. tostring(err))
        return false
    end

    return true
end

HandleDragonBridgeDispatch = function(
    Context,
    AttackerParam,
    DefenderParam,
    ReactionParam,
    UnitsParam,
    AttackPowerParam,
    AttackTypeParam,
    SequenceParam)

    local reaction = DragonFiniteNumber(ReactionParam)
    local units = DragonFiniteNumber(UnitsParam)
    local attackPower = DragonFiniteNumber(AttackPowerParam)
    local attackType = DragonFiniteNumber(AttackTypeParam)
    local sequence = DragonFiniteNumber(SequenceParam)

    if reaction == DRAGON_BRIDGE_BOOTSTRAP_REACTION and
       units == DRAGON_BRIDGE_BOOTSTRAP_UNITS and
       attackPower == DRAGON_BRIDGE_BOOTSTRAP_POWER and
       attackType == DRAGON_BRIDGE_BOOTSTRAP_ATTACKTYPE and
       sequence == DRAGON_BRIDGE_BOOTSTRAP_SEQUENCE then
        return
    end

    if reaction == DRAGON_BRIDGE_LOCAL_VALIDATION_REACTION or
       reaction == DRAGON_BRIDGE_DETACHED_VALIDATION_REACTION then
        return
    end

    reaction = reaction and math.floor(reaction) or 0
    units = units and math.floor(units) or 0

    -- BP_ESEBridge exposes AttackPower as a reflected
    -- DoubleProperty, but BP_DragonExplosion::ConfigureExplosion expects an Int.
    -- Preserve the fractional bootstrap sentinel above; normalize only real gameplay
    -- dispatches before they cross the Lua -> Blueprint IntProperty boundary.
    attackPower = attackPower and math.floor(attackPower) or nil

    attackType = attackType and math.floor(attackType) or 0
    sequence = sequence and math.floor(sequence) or 0

    if reaction < 1 or reaction > 6 or
       (units ~= 1 and units ~= 2) or
       not attackPower or attackPower <= 0 or
       sequence <= 0 then
        Log(string.format(
            "DRAGON BRIDGE: rejected invalid dispatch reaction=%s units=%s power=%s attackType=%s sequence=%s",
            tostring(reaction),
            tostring(units),
            tostring(attackPower),
            tostring(attackType),
            tostring(sequence)))
        return
    end

    local attacker = DragonUnwrap(AttackerParam)
    local defender = DragonUnwrap(DefenderParam)
    if not attacker or not attacker:IsValid() or
       not defender or not defender:IsValid() or
       not DragonActorAuthority(defender) then
        Log("DRAGON BRIDGE: rejected dispatch with invalid/non-authority actors. Seq=" ..
            tostring(sequence))
        return
    end

    if not RememberDragonSequence(sequence) then
        Log("DRAGON BRIDGE: duplicate/invalid sequence ignored: " ..
            tostring(sequence))
        return
    end

    _dragonBridgeLiveObserved = true

    if QueueDragonReactionExplosion(
            attacker,
            defender,
            reaction,
            units,
            attackPower,
            attackType,
            sequence) then
        Log(string.format(
            "DRAGON BRIDGE: queued Seq=%d reaction=%d units=%d power=%.3f delayMs=%d",
            sequence,
            reaction,
            units,
            attackPower,
            DRAGON_SPAWN_DELAY_MS))
    end
end

-- Bootstrap-only compatibility hook. It does NOT decode reaction metadata from
-- FPalDamageInfo. Its sole job is to run after an authoritative native damage
-- call so C++ has already had an opportunity to lazily arm the temporary
-- Blueprint-execution-stub capture hook; Lua then loads/spawns BP_ESEBridge and issues the
-- harmless bootstrap UFunction call. Once a real C++ bridge dispatch is seen,
-- this becomes a cheap no-op.
local function DragonBridgeBootstrapFromDamagePost(
    AttackerParam,
    DefenderParam)

    if _dragonBridgeLiveObserved then
        return
    end

    local defender = DragonUnwrap(DefenderParam)
    if not defender or not defender:IsValid() or
       not DragonActorAuthority(defender) then
        return
    end

    local attacker = DragonUnwrap(AttackerParam)
    local worldContext = attacker
    if not worldContext or not worldContext:IsValid() or
       not DragonActorAuthority(worldContext) then
        worldContext = defender
    end

    IssueDragonBridgeBootstrap(worldContext)
end

if not _G.__ESE_DragonBridgeBootstrapDamageHookInstalled then
    local okHook, preId, postId = pcall(function()
        return RegisterHook(
            "/Script/Pal.PalUtility:ProcessDamageAndPlayEffectsByDamageInfo",
            function(
                Context,
                Attacker,
                Defender,
                DamageInfo,
                bIsEnableHitEffect,
                ExceedHitCount)
                -- Native C++ raw-damage work runs inside the original call.
            end,
            function(
                Context,
                Attacker,
                Defender,
                DamageInfo,
                bIsEnableHitEffect,
                ExceedHitCount)
                DragonBridgeBootstrapFromDamagePost(
                    Attacker,
                    Defender)
            end)
    end)

    if okHook and postId ~= nil then
        _G.__ESE_DragonBridgeBootstrapDamageHookInstalled = true
        Log("DRAGON BRIDGE: bootstrap damage post-hook registered.")
    else
        Log("ERROR: DRAGON BRIDGE bootstrap damage post-hook failed: " ..
            tostring(preId))
    end
end

-- Dedicated-safe synthetic reaction damage gate. This hook mutates exactly one
-- FPalDamageInfo flag, and only while a matching Dragon reaction context is
-- active. The normal Palworld damage/status pipeline remains responsible for the
-- actual damage, elemental buildup, native Stun, hit reactions, and replication.
if not _G.__ESE_DragonCanProcessBypassHookInstalled then
    local okHook, hookId = pcall(function()
        return RegisterHook(
            "/Script/Pal.PalUtility:ProcessDamageAndPlayEffectsByDamageInfo",
            function(
                Context,
                Attacker,
                Defender,
                DamageInfo,
                bIsEnableHitEffect,
                ExceedHitCount)
                TryApplyDragonCanProcessBypass(Attacker, DamageInfo)
            end)
    end)

    if okHook and hookId ~= nil then
        _G.__ESE_DragonCanProcessBypassHookInstalled = true
        Log("DRAGON CANPROCESS BYPASS: ProcessDamage PRE-hook registered.")
    else
        Log("ERROR: DRAGON CANPROCESS BYPASS hook registration failed: " ..
            tostring(hookId))
    end
end

