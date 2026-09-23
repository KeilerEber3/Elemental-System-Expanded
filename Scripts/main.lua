-- ElementalSystemExpanded consolidated Light presentation Lua baseline.
-- Stage 6.3 native world/camera substitution remains in the C++ plugin;
-- this script owns mounted-asset registration, Light UI provenance, status
-- icons, damage-popup icons, and Waza table mutation only.

local UEHelpers = require("UEHelpers")

local _assetRegistryHelpers = nil
local _lightIcon = nil
local _lightVfxClass = nil
local _lightCameraVfxClass = nil

-- Presentation-only Light blindness tracking. The custom Light visual effect is
-- unique to Light-sourced Darkness, so its lifecycle is a reliable marker for UI.
local _lightBlindOwners = {}
local _lightIconProvenanceStack = {}
local _lightIconHooksRegistered = false

-- Damage-popup Light presentation tracking. WBP_PalDamageCanvas_OneShotText
-- builds the displayed AdditionalEffect[] list from FPalDamageInfo.EffectType1/2,
-- then WBP_PalDamageText assigns each icon through UImage::SetBrushFromSoftTexture.
-- We cannot reliably introspect UE4SS's TSoftObjectPtr userdata, so we pair those
-- soft-brush calls with the original native FPalDamageInfo order instead.
local _damageEffectQueue = {}
local _pendingLightDamageBrushByImage = {}

local VANILLA_DARK_ICON_TOKEN = "T_icon_StatusEffect_Dark"

-- Logging Pipeline
local function Log(msg)
    print("[ElementalSystemExpanded]: " .. tostring(msg) .. "\n")
end

-- Direct raw enum fallback mapping (EPalElementType -> EPalAdditionalEffectType)
-- Internally element 1 remains EPalElementType::Normal; presentation/localization calls it Light.
-- Light and Dark intentionally share Darkness (10) as the blindness carrier.
local RawElementToStatusMap = {
    [1] = 10, -- Normal/Light -> Darkness carrier
    [2] = 4,  -- Fire         -> Burn
    [3] = 5,  -- Water        -> Wetness
    [4] = 9,  -- Leaf         -> IvyCling
    [5] = 7,  -- Electricity  -> Electrical
    [6] = 6,  -- Ice          -> Freeze
    [7] = 8,  -- Earth        -> Muddy
    [8] = 10, -- Dark         -> Darkness
    [0] = 0,  -- None         -> None
}

local POISON_EFFECT = 3
local MAX_EFFECT_VALUE = 9999

-- Lookup-only visual effect ID used to make Palworld construct our Light VFX class.
-- 57 is vanilla DebugRefresh, chosen because it is a valid EPalVisualEffectID but
-- not part of normal status gameplay. Native code rewrites the created instance's
-- VisualEffectID back to DarkCondition (21) immediately after construction.
local LIGHT_VFX_LOOKUP_ID = 57

-- Lookup-only ID for the custom Light camera vignette. 58 is the
-- EPalVisualEffectID_MAX sentinel, not a normal gameplay VFX entry. Native
-- code will use it only to construct the custom class, then restore the
-- returned instance's VisualEffectID to CameraVignette (24).
local LIGHT_CAMERA_VFX_LOOKUP_ID = 58
local _palUtility = nil

-- Wrapper to prevent silent pcall failure and verify target function existence
local function SafeRegisterHook(targetFunction, callback)
    local success, err = pcall(function()
        return RegisterHook(targetFunction, callback)
    end)

    if success and err then
        return true
    else
        Log("ERROR | Failed to register hook on " .. targetFunction .. " | Error: " .. tostring(err))
        return false
    end
end

-- LogicMod-mounted custom content is resolved through AssetRegistryHelpers.
-- UE4SS LoadAsset() is intentionally not used for our /Game/Mods/PalworldMod assets.
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

local function GetLightStatusIcon()
    if not _lightIcon or not _lightIcon:IsValid() then
        _lightIcon = GetMountedAsset(
            "/Game/Mods/PalworldMod/T_icon_StatusEffect_Light",
            "T_icon_StatusEffect_Light")
    end
    return _lightIcon
end

local function GetLightVFXBP()
    if not _lightVfxClass or not _lightVfxClass:IsValid() then
        _lightVfxClass = GetMountedAsset(
            "/Game/Mods/PalworldMod/LightVFX/BP_VisualEffect_Status_Light",
            "BP_VisualEffect_Status_Light_C")
    end
    return _lightVfxClass
end

local function GetLightCameraVFXBP()
    if not _lightCameraVfxClass or not _lightCameraVfxClass:IsValid() then
        _lightCameraVfxClass = GetMountedAsset(
            "/Game/Mods/PalworldMod/LightVFX/BP_VisualEffect_CameraLightVignette",
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

    local LightClass = _lightVfxClass
    if not LightClass or not LightClass:IsValid() then
        LightClass = GetMountedAsset(
            "/Game/Mods/PalworldMod/LightVFX/BP_VisualEffect_Status_Light",
            "BP_VisualEffect_Status_Light_C")
        _lightVfxClass = LightClass
    end

    if not LightClass or not LightClass:IsValid() then
        Log("ERROR: Light VFX database registration has no valid Light class.")
        return false
    end

    local PalUtility = GetPalUtility()
    if not PalUtility then
        return false
    end

    local VisualEffectDatabase = PalUtility:GetVisualEffectDatabase(WorldContextObject)
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
        Log("ERROR: Light camera VFX database registration has no valid Light camera class.")
        return false
    end

    local PalUtility = GetPalUtility()
    if not PalUtility then
        return false
    end

    local VisualEffectDatabase = PalUtility:GetVisualEffectDatabase(WorldContextObject)
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

local function UnwrapHookValue(value)
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

    if ok then
        return result
    end

    return value
end

local function GetLightVFXOwner(effect)
    if not effect or not effect:IsValid() then
        return nil
    end

    local ok, owner = pcall(function()
        return effect:GetOwner()
    end)

    if ok and owner and owner:IsValid() then
        return owner
    end

    return nil
end

local function PushLightIconProvenance(address)
    if address and address ~= 0 then
        _lightIconProvenanceStack[#_lightIconProvenanceStack + 1] = address
    end
end

local function RemoveLightIconProvenance(address)
    if not address or address == 0 then
        return
    end

    for i = #_lightIconProvenanceStack, 1, -1 do
        if _lightIconProvenanceStack[i] == address then
            table.remove(_lightIconProvenanceStack, i)
            return
        end
    end
end

local function CurrentLightIconProvenanceOwner()
    return _lightIconProvenanceStack[#_lightIconProvenanceStack]
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

-- Boss status icons can resolve their Pal actor directly: the
-- WBP_BossEnemyHPGauge ancestry stores it as TargetCharacter. Normal
-- WBP_EnemyGauge icons intentionally do not attempt handle/parent resolution;
-- their validated path uses the synchronous Light VFX provenance window.
local function FindEnemyGaugeAncestor(image)
    local current = image
    for _ = 1, 8 do
        if not current or not current:IsValid() then
            break
        end

        if string.find(SafeFullName(current), "WBP_EnemyGauge_C", 1, true) then
            return current
        end

        local okOuter, outer = pcall(function()
            return current:GetOuter()
        end)
        if not okOuter or not outer or not outer:IsValid() then
            break
        end
        current = outer
    end
    return nil
end

local function ResolveBossStatusIconActor(image)
    if not image or not image:IsValid() then
        return nil
    end

    local current = image
    for _ = 1, 12 do
        if not current or not current:IsValid() then
            break
        end

        local targetCharacter = nil
        pcall(function()
            targetCharacter = current.TargetCharacter
        end)
        if targetCharacter and targetCharacter:IsValid() then
            return targetCharacter
        end

        local okOuter, outer = pcall(function()
            return current:GetOuter()
        end)
        if not okOuter or not outer or not outer:IsValid() then
            break
        end
        current = outer
    end

    return nil
end

local function SafeScalarNumber(value)
    value = UnwrapHookValue(value)
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

local function SafeStructNumber(structValue, fieldName)
    if not structValue then
        return nil
    end

    local raw = nil
    pcall(function()
        raw = structValue[fieldName]
    end)
    return SafeScalarNumber(raw)
end

local function FindDamageTextAncestor(image)
    local current = image
    for _ = 1, 10 do
        if not current or not current:IsValid() then
            break
        end

        if string.find(SafeFullName(current), "WBP_PalDamageText_C", 1, true) then
            return current
        end

        local okOuter, outer = pcall(function()
            return current:GetOuter()
        end)
        if not okOuter or not outer or not outer:IsValid() then
            break
        end
        current = outer
    end
    return nil
end

local function SetHookParamValue(param, newValue)
    local didSet = false
    local ok, err = pcall(function()
        if param.set then
            param:set(newValue)
            didSet = true
            return
        end
        if param.Set then
            param:Set(newValue)
            didSet = true
            return
        end
    end)

    return ok and didSet, err
end

local function RegisterLightIconHooks()
    if _lightIconHooksRegistered then
        return true
    end

    local LightClass = GetLightVFXBP()
    local LightIcon = GetLightStatusIcon()

    if not LightClass or not LightClass:IsValid() or
       not LightIcon or not LightIcon:IsValid() then
        return false
    end

    local beginPath =
        "/Game/Mods/PalworldMod/LightVFX/BP_VisualEffect_Status_Light." ..
        "BP_VisualEffect_Status_Light_C:OnBeginVisualEffect"
    local endPath =
        "/Game/Mods/PalworldMod/LightVFX/BP_VisualEffect_Status_Light." ..
        "BP_VisualEffect_Status_Light_C:OnEndVisualEffect"

    local okBegin = SafeRegisterHook(beginPath, function(Context)
        local effect = Context:get()
        local owner = GetLightVFXOwner(effect)

        if owner then
            local address = tonumber(owner:GetAddress()) or 0
            if address ~= 0 then
                _lightBlindOwners[address] = true
                PushLightIconProvenance(address)
            end
        end
    end)

    local okEnd = SafeRegisterHook(endPath, function(Context)
        local effect = Context:get()
        local owner = GetLightVFXOwner(effect)

        if owner then
            local address = tonumber(owner:GetAddress()) or 0
            if address ~= 0 then
                RemoveLightIconProvenance(address)
                _lightBlindOwners[address] = nil
            end
        end
    end)

    if not okBegin or not okEnd then
        Log("ERROR: Light VFX lifecycle icon hooks are incomplete.")
        return false
    end

    -- UImage::SetBrushFromTexture is the stable native interception boundary.
    -- Replace the Texture parameter in the PRE-hook rather than making a second
    -- SetBrushFromTexture call from the post-hook. This avoids a nested brush write
    -- and should eliminate the rare white-square race seen on the boss HUD.
    local okBrush, brushPreId, brushPostId = pcall(function()
        return RegisterHook(
            "/Script/UMG.Image:SetBrushFromTexture",
            function(Context, Texture)
                local texture = UnwrapHookValue(Texture)
                if not texture or not texture:IsValid() then
                    return
                end

                local image = Context:get()
                if not image or not image:IsValid() then
                    return
                end

                local imageAddress = tonumber(image:GetAddress()) or 0
                if imageAddress == 0 then
                    return
                end

                local textureName = SafeFullName(texture)

                if string.find(textureName, VANILLA_DARK_ICON_TOKEN, 1, true) then
                    local ownerAddress = 0
                    local isLight = false

                    -- Normal overhead Pal gauges cannot expose their bound actor cleanly
                    -- through UE4SS because GetBindedHandle is an out-parameter BP call.
                    -- Fortunately the Light VFX provenance scope is synchronous: the
                    -- normal gauge writes Image_StatusIconEffect between our BEGIN/END
                    -- hooks.  For this exact widget family, use that active owner rather
                    -- than guessing a handle/property.  Genuine Darkness has no active
                    -- Light provenance scope and is left untouched.
                    local enemyGauge = FindEnemyGaugeAncestor(image)
                    if enemyGauge then
                        local activeOwner = CurrentLightIconProvenanceOwner()
                        if not activeOwner then
                            return
                        end

                        ownerAddress = activeOwner
                        isLight = true
                    else
                        -- Boss gauge path: WBP_BossEnemyHPGauge exposes TargetCharacter
                        -- directly, so retain the actor-specific resolver there.
                        local actor = ResolveBossStatusIconActor(image)
                        if not actor then
                            return
                        end

                        ownerAddress = tonumber(actor:GetAddress()) or 0
                        if ownerAddress == 0 then
                            return
                        end

                        isLight = _lightBlindOwners[ownerAddress] == true
                    end

                    if not isLight then
                        return
                    end

                    local icon = GetLightStatusIcon()
                    if not icon or not icon:IsValid() then
                        Log("ERROR: Light status icon asset is invalid at brush substitution time.")
                        return
                    end

                    local okSet, setErr = SetHookParamValue(Texture, icon)
                    if not okSet then
                        Log("ERROR: Light status icon parameter substitution failed: " .. tostring(setErr))
                    end
                    return
                end

            end,
            function(Context, Texture)
            end)
    end)

    if not okBrush or brushPreId == nil then
        Log("ERROR | Failed to register native Light icon brush hook: " ..
            tostring(brushPreId))
        return false
    end

    -- -------------------------------------------------------------
    -- Light damage-popup icon substitution
    -- -------------------------------------------------------------
    -- Capture direct additional effects at the native UI entry point, before the
    -- Blueprint canvas reduces FPalDamageInfo to AdditionalEffect[].  The Blueprint
    -- preserves EffectType1 then EffectType2 order, so enqueue one marker per non-zero
    -- direct effect.  Light = Normal element (1) + Waza (0) + Darkness carrier (10).
    local okDamagePopup, damagePopupPreId, damagePopupPostId = pcall(function()
        return RegisterHook(
            "/Script/Pal.PalDamageDisplayCanvas:OnAddDamagePopup",
            function(Context, DamageInfo, Defender)
                local info = UnwrapHookValue(DamageInfo)
                if not info then
                    return
                end

                local attackElement = SafeStructNumber(info, "AttackElementType")
                local attackType = SafeStructNumber(info, "AttackType")
                local effect1 = SafeStructNumber(info, "EffectType1") or 0
                local effect2 = SafeStructNumber(info, "EffectType2") or 0
                local isLightSource = attackElement == 1 and attackType == 0

                local function EnqueueEffect(effect)
                    if not effect or effect == 0 then
                        return
                    end
                    _damageEffectQueue[#_damageEffectQueue + 1] = {
                        effect = effect,
                        isLight = isLightSource and effect == 10,
                    }
                end

                EnqueueEffect(effect1)
                EnqueueEffect(effect2)

                -- Bounded safety in case a popup is suppressed before its widget writes.
                while #_damageEffectQueue > 64 do
                    table.remove(_damageEffectQueue, 1)
                end

            end,
            function(Context, DamageInfo, Defender)
            end)
    end)

    if not (okDamagePopup and damagePopupPreId ~= nil) then
        Log("WARNING: Light damage popup tracker could not hook OnAddDamagePopup: " ..
            tostring(damagePopupPreId))
    end

    -- WBP_PalDamageText uses SetBrushFromSoftTexture for AdditionalEffect icons.
    -- UE4SS exposes the TSoftObjectPtr as opaque userdata, so do not inspect it.
    -- Instead consume the direct-effect marker in the same order the Blueprint uses.
    -- When that marker is Light-sourced Darkness, arm this exact UImage address;
    -- the post-hook replaces the soft-brush result with the already-loaded Light icon.
    local okSoftBrush, softBrushPreId, softBrushPostId = pcall(function()
        return RegisterHook(
            "/Script/UMG.Image:SetBrushFromSoftTexture",
            function(Context, SoftTexture)
                local image = Context:get()
                if not image or not image:IsValid() then
                    return
                end

                local damageText = FindDamageTextAncestor(image)
                if not damageText then
                    return
                end

                local imageName = SafeFullName(image)
                if not string.find(imageName, "Image_StatusEffect", 1, true) then
                    return
                end

                local imageAddress = tonumber(image:GetAddress()) or 0
                if imageAddress == 0 then
                    return
                end

                -- Any new soft assignment supersedes stale state for a pooled widget.
                _pendingLightDamageBrushByImage[imageAddress] = nil

                local head = _damageEffectQueue[1]
                if not head then
                    return
                end
                table.remove(_damageEffectQueue, 1)

                if head.isLight then
                    _pendingLightDamageBrushByImage[imageAddress] = head
                end
            end,
            function(Context, SoftTexture)
                -- After vanilla finishes its soft-brush write, overwrite only an
                -- armed Light damage icon with the already-loaded hard Texture2D.
                -- This calls a different UFunction than the current post-hook, so it
                -- does not recreate the status-icon SetBrushFromTexture recursion bug.
                local image = Context:get()
                if not image or not image:IsValid() then
                    return
                end

                local imageAddress = tonumber(image:GetAddress()) or 0
                if imageAddress == 0 then
                    return
                end

                local pendingDamage = _pendingLightDamageBrushByImage[imageAddress]
                if not pendingDamage then
                    return
                end

                -- Clear before the hard write so pooled widgets cannot retain stale
                -- Light state and so any nested hook dispatch sees no pending marker.
                _pendingLightDamageBrushByImage[imageAddress] = nil

                local icon = GetLightStatusIcon()
                if not icon or not icon:IsValid() then
                    Log("ERROR: Light damage icon asset is invalid at soft-brush post substitution time.")
                    return
                end

                local okSwap, swapErr = pcall(function()
                    image:SetBrushFromTexture(icon, false)
                end)

                if not okSwap then
                    Log("ERROR: Light damage icon soft-brush post substitution failed: " .. tostring(swapErr))
                end
            end)
    end)

    if not (okSoftBrush and softBrushPreId ~= nil) then
        Log("WARNING: Light damage soft-brush tracker could not hook SetBrushFromSoftTexture: " ..
            tostring(softBrushPreId))
    end

    _lightIconHooksRegistered = true
    return true
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

    local tablePath = "/Game/Pal/DataTable/Waza/DT_WazaDataTable.DT_WazaDataTable"
    local WazaTable = StaticFindObject(tablePath) or StaticLoadObject(tablePath)

    if not WazaTable or not WazaTable:IsValid() then
        Log("ERROR: DT_WazaDataTable could not be found or loaded.")
        return false
    end

    local PalSkillSet = BuildPalSkillSet(WorldContextObject)
    if not PalSkillSet then
        -- Fail closed: do not set __ESE_WazaMutationApplied. A later possession
        -- event may retry after the game databases are fully initialized.
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
        -- None has no elemental status. Dragon remains untouched for now.
        if not rawElem or rawElem == 0 or rawElem == 9 then
            return
        end

        -- Normal (displayed as Light by this mod) is special:
        -- only Waza IDs referenced by Pal mastery data are eligible.
        -- Player/basic/internal Normal attacks such as Human_Punch therefore
        -- remain ordinary Normal and receive no blindness carrier.
        if rawElem == 1 then
            local rawWazaType = tonumber(RowData.WazaType)
            if not rawWazaType or not PalSkillSet[rawWazaType] then
                normalNonPalSkippedCount = normalNonPalSkippedCount + 1
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

    -- Poison multiplication is not idempotent, so only mark success after the
    -- complete table pass has finished.
    _G.__ESE_WazaMutationApplied = true

    Log(string.format(
        "Waza mutation complete: elemental=%d lightPalSkills=%d normalNonPalSkipped=%d poisonSlotsDoubled=%d.",
        elementalModifiedCount,
        lightModifiedCount,
        normalNonPalSkippedCount,
        poisonModifiedCount))
    return true
end

-- Game-thread deferred trigger. Possession can fire repeatedly; the mutation is
-- guarded globally, while custom asset resolution may retry until the LogicMod
-- content is visible in the Asset Registry.
SafeRegisterHook("/Script/Engine.PlayerController:ServerAcknowledgePossession", function(Context)
    local PlayerController = Context:get()
    if not PlayerController or not PlayerController:IsValid() then
        Log("ERROR: ServerAcknowledgePossession provided no valid PlayerController context.")
        return
    end

    InjectSkillStatusUnits(PlayerController)
    GetLightStatusIcon()
    GetLightVFXBP()
    GetLightCameraVFXBP()
    RegisterLightVFXDatabaseClass(PlayerController)
    RegisterLightCameraVFXDatabaseClass(PlayerController)
    RegisterLightIconHooks()
end)