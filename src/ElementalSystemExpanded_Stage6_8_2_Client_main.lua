-- ElementalSystemExpanded Stage 6.8.2. Client Lua.
-- Stage 6.3 native world/camera substitution remains in the C++ plugin;
-- this script owns mounted-asset registration, Light UI provenance, status
-- icons, damage-popup icons, Waza table mutation, and the authority-gated
-- Dragon-reaction explosion bridge.

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
-- Stage30.5 binds one FPalDamageInfo provenance record to the exact pooled/new
-- WBP_PalDamageText returned by the same UPalDamageDisplayCanvas:GetDamageTextWidget
-- call.  The queue is per-canvas and per-popup, never global per-brush: passive-skill
-- icon writes therefore cannot consume another popup's provenance.
local _pendingDamageMarkersByCanvas = {}
local _damageProvenanceByDamageText = {}
local _damageWidgetReturnWarningLogged = false
local _damagePopupHookStatusLogged = false
local _pendingLightDamageBrushByImage = {}
local _lightIconWriteGenerationByImage = {}
local _lightIconInternalWriteByImage = {}

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
    [9] = 4,  -- Dragon       -> Burn aggregate
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
-- UE4SS LoadAsset() is intentionally not used for our /Game/Mods/ElementalSystemExpanded assets.
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

local LIGHT_ICON_OBJECT_PATH =
    "/Game/Mods/ElementalSystemExpanded/T_icon_StatusEffect_Light.T_icon_StatusEffect_Light"

local function IsUsableLightStatusIcon(icon)
    if not icon or not icon:IsValid() then
        return false
    end

    -- A valid UObject alone is not enough for UMG: reject a texture whose
    -- renderable dimensions have not materialized yet.  This is intentionally
    -- read-only and cheap after the object is resident.
    local okSize, sizeX, sizeY = pcall(function()
        return icon:Blueprint_GetSizeX(), icon:Blueprint_GetSizeY()
    end)

    return okSize and
        (tonumber(sizeX) or 0) > 0 and
        (tonumber(sizeY) or 0) > 0
end

local function GetLightStatusIcon()
    if IsUsableLightStatusIcon(_lightIcon) then
        return _lightIcon
    end

    -- Prefer the synchronous object loader for the UI texture.  AssetRegistry
    -- discovery is excellent for mounted Blueprint classes, but an icon that is
    -- about to become a Slate brush should already be a fully materialized
    -- UTexture2D rather than a just-discovered asset object.
    local icon = nil
    pcall(function()
        icon = StaticFindObject(LIGHT_ICON_OBJECT_PATH)
    end)

    if not IsUsableLightStatusIcon(icon) then
        pcall(function()
            icon = StaticLoadObject(LIGHT_ICON_OBJECT_PATH)
        end)
    end

    -- Keep the validated AssetRegistry route as a fallback for LogicMod mount
    -- timing differences.  It is accepted only after the same size check.
    if not IsUsableLightStatusIcon(icon) then
        icon = GetMountedAsset(
            "/Game/Mods/ElementalSystemExpanded/T_icon_StatusEffect_Light",
            "T_icon_StatusEffect_Light")
    end

    if IsUsableLightStatusIcon(icon) then
        _lightIcon = icon
        return _lightIcon
    end

    _lightIcon = nil
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

    local LightClass = _lightVfxClass
    if not LightClass or not LightClass:IsValid() then
        LightClass = GetMountedAsset(
            "/Game/Mods/ElementalSystemExpanded/LightVFX/BP_VisualEffect_Status_Light",
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

local function DamageObjectAddress(value)
    value = UnwrapHookValue(value)
    if not value or not value:IsValid() then
        return 0
    end
    local address = 0
    pcall(function()
        address = tonumber(value:GetAddress()) or 0
    end)
    return address
end

-- Stage30.7 exact popup-widget resolver.
-- Stage30.3 only needed a boolean ancestry test, so matching "WBP_PalDamageText_C"
-- anywhere in GetFullName() was harmless there.  Stage30.5/30.6 incorrectly reused
-- that helper as an identity resolver: a child UImage's full pathname already
-- contains its owning WBP_PalDamageText_C name, so the child itself could be returned.
--
-- We already know the exact pooled/new WBP_PalDamageText address from
-- GetDamageTextWidget.  Therefore the provenance table itself is the authoritative
-- identity test: walk image -> Outer -> ... and stop only on an address that is
-- currently bound in _damageProvenanceByDamageText.
local function FindBoundDamageTextAncestor(image)
    local current = image
    for _ = 1, 12 do
        if not current or not current:IsValid() then
            break
        end

        local currentAddress = DamageObjectAddress(current)
        if currentAddress ~= 0 then
            local marker = _damageProvenanceByDamageText[currentAddress]
            if marker then
                return current, marker
            end
        end

        local okOuter, outer = pcall(function()
            return current:GetOuter()
        end)
        if not okOuter or not outer or not outer:IsValid() then
            break
        end
        current = outer
    end

    return nil, nil
end

local function ForEachUEArray(arrayValue, callback)
    if not arrayValue or type(callback) ~= "function" then
        return false
    end

    local ok = pcall(function()
        arrayValue:ForEach(function(_, wrappedElement)
            local element = UnwrapHookValue(wrappedElement)
            callback(element)
            return false
        end)
    end)
    return ok
end

local function BuildDamageDisplayMarker(info)
    info = UnwrapHookValue(info)
    if not info then
        return nil
    end

    local attackElement = SafeStructNumber(info, "AttackElementType")
    local attackType = SafeStructNumber(info, "AttackType")
    local effect1 = SafeStructNumber(info, "EffectType1") or 0
    local effect2 = SafeStructNumber(info, "EffectType2") or 0
    local isLightSource = attackElement == 1 and attackType == 0

    -- WBP_PalDamageCanvas_OneShotText::Get_Additional_Effect_ appends the two
    -- direct FPalDamageInfo effects first (EffectType1, then EffectType2), skipping
    -- zero entries, and appends passive-skill effects only afterwards.  Preserve
    -- that direct sequence on the exact popup marker.  Stage30.7 consumes this
    -- sequence on the exact bound WBP_PalDamageText; it never reflects
    -- AdditionalEffectIconArray and never uses a global brush FIFO.
    local directEffects = {}
    if effect1 ~= 0 then
        directEffects[#directEffects + 1] = effect1
    end
    if effect2 ~= 0 then
        directEffects[#directEffects + 1] = effect2
    end

    return {
        attackElement = attackElement,
        attackType = attackType,
        effect1 = effect1,
        effect2 = effect2,
        isLightSource = isLightSource,
        directEffects = directEffects,
        nextDirectEffect = 1,
    }
end

local function GetDamageMarkerQueueState(canvas, create)
    if not canvas or not canvas:IsValid() then
        return nil
    end

    local canvasAddress = DamageObjectAddress(canvas)
    if canvasAddress == 0 then
        return nil
    end

    local state = _pendingDamageMarkersByCanvas[canvasAddress]
    if not state and create then
        state = {
            canvas = canvas,
            markers = {},
        }
        _pendingDamageMarkersByCanvas[canvasAddress] = state
    elseif state then
        state.canvas = canvas
    end
    return state
end

local function EnqueueDamageMarkerForCanvas(canvas, marker)
    local state = GetDamageMarkerQueueState(canvas, true)
    if not state then
        return false
    end

    -- Always enqueue one record per popup, including non-Light/no-effect records.
    -- That keeps the queue aligned with Palworld's own per-canvas popup order.
    marker = marker or {
        attackElement = nil,
        attackType = nil,
        effect1 = 0,
        effect2 = 0,
        isLightSource = false,
        directEffects = {},
        nextDirectEffect = 1,
    }
    state.markers[#state.markers + 1] = marker

    -- Fail closed rather than silently drift if widget acquisition ever stops
    -- consuming the queue.  Normal play should remain far below this bound.
    if #state.markers > 128 then
        state.markers = {}
        Log("WARNING: Damage-popup provenance queue overflow; canvas queue reset fail-closed.")
        return false
    end
    return true
end

local function DequeueDamageMarkerForCanvas(canvas)
    local state = GetDamageMarkerQueueState(canvas, false)
    if not state or #state.markers == 0 then
        return nil
    end

    local marker = table.remove(state.markers, 1)
    if #state.markers == 0 then
        local canvasAddress = DamageObjectAddress(canvas)
        if canvasAddress ~= 0 then
            _pendingDamageMarkersByCanvas[canvasAddress] = nil
        end
    end
    return marker
end

local function BindDamageMarkerToText(damageText, marker)
    local damageTextAddress = DamageObjectAddress(damageText)
    if damageTextAddress == 0 then
        return false
    end

    -- Bind Light and non-Light records alike.  WBP_PalDamageText widgets are pooled,
    -- so every new display transaction must overwrite any prior widget provenance.
    marker.nextDirectEffect = 1
    _damageProvenanceByDamageText[damageTextAddress] = marker
    return true
end

local function ConsumeNextDirectDamageEffect(marker)
    if not marker then
        return nil, nil
    end

    local directEffects = marker.directEffects
    if type(directEffects) ~= "table" then
        return nil, nil
    end

    local cursor = tonumber(marker.nextDirectEffect) or 1
    local effect = directEffects[cursor]
    if effect == nil then
        return nil, nil
    end

    marker.nextDirectEffect = cursor + 1
    return effect, cursor
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

local function NextLightIconWriteGeneration(imageAddress)
    if not imageAddress or imageAddress == 0 then
        return 0
    end

    local nextGeneration =
        (_lightIconWriteGenerationByImage[imageAddress] or 0) + 1
    _lightIconWriteGenerationByImage[imageAddress] = nextGeneration
    return nextGeneration
end

local function ApplyLightIconHard(image, reason)
    if not image or not image:IsValid() then
        return false
    end

    local imageAddress = tonumber(image:GetAddress()) or 0
    if imageAddress == 0 then
        return false
    end

    local icon = GetLightStatusIcon()
    if not icon or not icon:IsValid() then
        Log("ERROR: Light icon texture is unavailable during " .. tostring(reason) .. ".")
        return false
    end

    -- Mark our delayed repair write so the SetBrushFromTexture PRE-hook does not
    -- treat it as a new external widget assignment and invalidate its own token.
    _lightIconInternalWriteByImage[imageAddress] = true
    local ok, err = pcall(function()
        image:SetBrushFromTexture(icon, false)
    end)
    _lightIconInternalWriteByImage[imageAddress] = nil

    if not ok then
        Log("ERROR: Light icon hard-brush write failed during " ..
            tostring(reason) .. ": " .. tostring(err))
        return false
    end

    return true
end

local function ScheduleLightIconReassert(image, imageAddress, generation, ownerAddress, reason)
    if not image or not image:IsValid() or
       not imageAddress or imageAddress == 0 or
       not generation or generation == 0 then
        return
    end

    local function Reassert()
        if _lightIconWriteGenerationByImage[imageAddress] ~= generation then
            return
        end
        if not image or not image:IsValid() then
            return
        end
        if ownerAddress and ownerAddress ~= 0 and
           _lightBlindOwners[ownerAddress] ~= true then
            return
        end
        ApplyLightIconHard(image, reason)
    end

    -- Never perform the recovery write from inside the UMG brush transaction
    -- that triggered it.  One game-thread turn repairs one-frame Slate/resource
    -- races; the bounded delayed repeat protects against the original soft icon
    -- async loader completing shortly after our first hard assignment.
    if type(ExecuteInGameThread) == "function" then
        pcall(function()
            ExecuteInGameThread(Reassert)
        end)
    end

    if type(ExecuteInGameThreadWithDelay) == "function" then
        pcall(function()
            ExecuteInGameThreadWithDelay(100, Reassert)
        end)
    elseif type(ExecuteWithDelay) == "function" then
        pcall(function()
            ExecuteWithDelay(100, function()
                if type(ExecuteInGameThread) == "function" then
                    ExecuteInGameThread(Reassert)
                end
            end)
        end)
    end
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
        "/Game/Mods/ElementalSystemExpanded/LightVFX/BP_VisualEffect_Status_Light." ..
        "BP_VisualEffect_Status_Light_C:OnBeginVisualEffect"
    local endPath =
        "/Game/Mods/ElementalSystemExpanded/LightVFX/BP_VisualEffect_Status_Light." ..
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
                local isDarkIconWrite =
                    string.find(textureName, VANILLA_DARK_ICON_TOKEN, 1, true) ~= nil
                local alreadyTracked =
                    _lightIconWriteGenerationByImage[imageAddress] ~= nil

                local writeGeneration = 0
                if _lightIconInternalWriteByImage[imageAddress] then
                    writeGeneration = _lightIconWriteGenerationByImage[imageAddress] or 0
                elseif isDarkIconWrite or alreadyTracked then
                    -- Track relevant status images and invalidate any pending repair
                    -- when a pooled/tracked image receives a later external texture.
                    writeGeneration = NextLightIconWriteGeneration(imageAddress)
                end

                if isDarkIconWrite then
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
                        return
                    end

                    -- Vanilla still performs the primary brush write exactly once.
                    -- Reassert later, outside this hook transaction, only if no newer
                    -- assignment has reused the image and the Light owner is still live.
                    ScheduleLightIconReassert(
                        image,
                        imageAddress,
                        writeGeneration,
                        ownerAddress,
                        "status-icon deferred repair")
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
    -- Light damage-popup icon substitution — exact widget + direct-sequence provenance
    -- -------------------------------------------------------------
    -- Proven source discriminator: OnAddDamagePopup receives the original
    -- FPalDamageInfo (Light = AttackElementType 1 + AttackType 0 + effect 10).
    -- Queue exactly one marker per popup on that UPalDamageDisplayCanvas and bind it
    -- to the exact WBP_PalDamageText returned by GetDamageTextWidget.  Each bound
    -- marker owns its own EffectType1/EffectType2 cursor.  This removes both the old
    -- global FIFO and Stage30.4/30.5's unproven AdditionalEffectIconArray reflection.
    local okDamagePopup, damagePopupPreId, damagePopupPostId = pcall(function()
        return RegisterHook(
            "/Script/Pal.PalDamageDisplayCanvas:OnAddDamagePopup",
            function(Context, DamageInfo, Defender)
                local canvas = Context:get()
                if not canvas or not canvas:IsValid() then
                    return
                end

                local info = UnwrapHookValue(DamageInfo)
                local marker = BuildDamageDisplayMarker(info)
                EnqueueDamageMarkerForCanvas(canvas, marker)

                if marker and marker.isLightSource then
                    Log(string.format(
                        "DAMAGE30.7 CAPTURE LIGHT canvas=0x%X effect1=%d effect2=%d directCount=%d",
                        DamageObjectAddress(canvas),
                        marker.effect1 or 0,
                        marker.effect2 or 0,
                        #(marker.directEffects or {})))
                end
            end,
            function(Context, DamageInfo, Defender)
            end)
    end)

    local damagePopupHookReady = okDamagePopup and damagePopupPreId ~= nil
    if not damagePopupHookReady then
        Log("ERROR: Light damage popup tracker could not hook OnAddDamagePopup: " ..
            tostring(damagePopupPreId))
    end

    -- CreateOrPopDamageWidget calls this native function once for the exact widget
    -- that will be painted for the next queued popup.  Binding here makes popup
    -- provenance independent of passive-effect icon count and brush-call interleaving.
    local okDamageWidget, damageWidgetPreId, damageWidgetPostId = pcall(function()
        return RegisterHook(
            "/Script/Pal.PalDamageDisplayCanvas:GetDamageTextWidget",
            function(Context, WidgetClass)
            end,
            -- UE4SS native-UFunction post hooks expose the original return value
            -- as the second Lua argument, before the ordinary UFunction params.
            function(Context, ReturnValue, WidgetClass)
                local canvas = Context:get()
                if not canvas or not canvas:IsValid() then
                    return
                end

                local damageText = UnwrapHookValue(ReturnValue)
                if not damageText or not damageText:IsValid() then
                    if not _damageWidgetReturnWarningLogged then
                        _damageWidgetReturnWarningLogged = true
                        Log("WARNING: GetDamageTextWidget post-hook exposed no valid ReturnValue; damage-popup provenance left fail-closed.")
                    end
                    return
                end

                -- This native pool function can theoretically serve other damage-text
                -- subclasses.  Consume a popup marker only for the exact widget family
                -- whose AdditionalEffect[] pipeline we decoded.
                if not string.find(SafeFullName(damageText), "WBP_PalDamageText_C", 1, true) then
                    return
                end

                local marker = DequeueDamageMarkerForCanvas(canvas)
                local damageTextAddress = DamageObjectAddress(damageText)
                if damageTextAddress == 0 then
                    return
                end

                if marker then
                    BindDamageMarkerToText(damageText, marker)
                    if marker.isLightSource then
                        Log(string.format(
                            "DAMAGE30.7 BIND LIGHT canvas=0x%X widget=0x%X directCount=%d",
                            DamageObjectAddress(canvas),
                            damageTextAddress,
                            #(marker.directEffects or {})))
                    end
                else
                    -- Pooled widget reuse without a matching popup marker must never
                    -- inherit an older Light classification.
                    _damageProvenanceByDamageText[damageTextAddress] = nil
                end
            end)
    end)

    local damageWidgetHookReady = okDamageWidget and damageWidgetPreId ~= nil
    if not damageWidgetHookReady then
        Log("ERROR: Light damage widget provenance hook could not hook GetDamageTextWidget: " ..
            tostring(damageWidgetPreId))
    end

    -- Clear widget-bound provenance when Palworld returns a damage-text widget to
    -- its pool.  This makes stale Light state impossible across pooled reuse even if
    -- a later popup is unrelated or its direct additional-effect list is empty.
    local okReleaseDamageWidget, releasePreId, releasePostId = pcall(function()
        return RegisterHook(
            "/Script/Pal.PalDamageDisplayCanvas:ReleaseDamageTextWidget",
            function(Context, Widget)
                local damageText = UnwrapHookValue(Widget)
                local damageTextAddress = DamageObjectAddress(damageText)
                if damageTextAddress ~= 0 then
                    local marker = _damageProvenanceByDamageText[damageTextAddress]
                    if marker and marker.isLightSource then
                        Log(string.format(
                            "DAMAGE30.7 RELEASE LIGHT widget=0x%X nextDirect=%d directCount=%d",
                            damageTextAddress,
                            tonumber(marker.nextDirectEffect) or 0,
                            #(marker.directEffects or {})))
                    end
                    _damageProvenanceByDamageText[damageTextAddress] = nil
                end
            end,
            function(Context, Widget)
            end)
    end)

    if not (okReleaseDamageWidget and releasePreId ~= nil) then
        Log("WARNING: Damage widget provenance cleanup could not hook ReleaseDamageTextWidget: " ..
            tostring(releasePreId))
    end

    local okSoftBrush, softBrushPreId, softBrushPostId = pcall(function()
        return RegisterHook(
            "/Script/UMG.Image:SetBrushFromSoftTexture",
            function(Context, SoftTexture)
                local image = Context:get()
                if not image or not image:IsValid() then
                    return
                end

                local damageText, marker = FindBoundDamageTextAncestor(image)
                if not damageText or not marker then
                    return
                end

                local imageName = SafeFullName(image)
                if not string.find(imageName, "Image_StatusEffect", 1, true) then
                    return
                end

                local imageAddress = DamageObjectAddress(image)
                if imageAddress == 0 then
                    return
                end

                local writeGeneration = NextLightIconWriteGeneration(imageAddress)
                _pendingLightDamageBrushByImage[imageAddress] = nil

                -- Every SetElementEffect soft-brush write for this widget consumes the
                -- next displayed direct effect first.  Get_Additional_Effect_ guarantees
                -- EffectType1/2 precede passive additions, so after the direct cursor is
                -- exhausted all later passive icon writes are ignored by this marker.
                local directEffect, directOrdinal = ConsumeNextDirectDamageEffect(marker)
                if directEffect == nil then
                    return
                end

                if marker.isLightSource then
                    Log(string.format(
                        "DAMAGE30.7 CONSUME LIGHT widget=0x%X image=0x%X ordinal=%d effect=%d",
                        DamageObjectAddress(damageText),
                        imageAddress,
                        directOrdinal or 0,
                        directEffect or 0))
                end

                if not marker.isLightSource or directEffect ~= 10 then
                    return
                end

                _pendingLightDamageBrushByImage[imageAddress] = {
                    iconWriteGeneration = writeGeneration,
                    attackElement = marker.attackElement,
                    attackType = marker.attackType,
                    effect1 = marker.effect1,
                    effect2 = marker.effect2,
                    directOrdinal = directOrdinal,
                }
            end,
            function(Context, SoftTexture)
                local image = Context:get()
                if not image or not image:IsValid() then
                    return
                end

                local imageAddress = DamageObjectAddress(image)
                if imageAddress == 0 then
                    return
                end

                local pendingDamage = _pendingLightDamageBrushByImage[imageAddress]
                if not pendingDamage then
                    return
                end

                _pendingLightDamageBrushByImage[imageAddress] = nil

                local generation = pendingDamage.iconWriteGeneration or 0
                if generation == 0 then
                    return
                end

                Log(string.format(
                    "DAMAGE30.7 ARM LIGHT image=0x%X ordinal=%d generation=%d",
                    imageAddress,
                    pendingDamage.directOrdinal or 0,
                    generation))

                -- Preserve Stage30.3 resource hardening: perform the hard Light write
                -- outside the current soft-brush transaction, with generation guards.
                ScheduleLightIconReassert(
                    image,
                    imageAddress,
                    generation,
                    nil,
                    "damage-icon exact-widget direct-sequence repair")
            end)
    end)

    local damageSoftBrushHookReady = okSoftBrush and softBrushPreId ~= nil
    if not damageSoftBrushHookReady then
        Log("ERROR: Light damage soft-brush tracker could not hook SetBrushFromSoftTexture: " ..
            tostring(softBrushPreId))
    end

    if not _damagePopupHookStatusLogged then
        _damagePopupHookStatusLogged = true
        Log(string.format(
            "DAMAGE30.7 HOOKS capture=%s widget=%s softbrush=%s release=%s",
            damagePopupHookReady and "OK" or "FAIL",
            damageWidgetHookReady and "OK" or "FAIL",
            damageSoftBrushHookReady and "OK" or "FAIL",
            (okReleaseDamageWidget and releasePreId ~= nil) and "OK" or "FAIL"))
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
        -- None has no elemental status. Dragon receives its Burn carrier.
        if not rawElem or rawElem == 0 then
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


-- ---------------------------------------------------------------------------
-- Light world-VFX runtime identity normalization
-- ---------------------------------------------------------------------------
-- ID 57 is used only as ESE lookup/transport provenance. Once Palworld has
-- materialized the Light world VFX on the client, rewrite the instance runtime
-- identity to native Darkness ID 21 so normal native teardown reaches it.
local _lightWorldRuntimeRewriteHookRegistered = false

local function RewriteClientLightWorldRuntimeId(component)
    if not component or not component:IsValid() then
        return false
    end

    local list = nil
    local okList = pcall(function()
        list = component.ExecutionVisualEffects
    end)
    if not okList or not list then
        return false
    end

    local rewrote = false
    pcall(function()
        list:ForEach(function(_, elemParam)
            local effect = UnwrapHookValue(elemParam)
            if not effect or not effect:IsValid() then
                return false
            end

            local id = nil
            pcall(function()
                id = SafeScalarNumber(effect.VisualEffectID)
            end)
            if id ~= LIGHT_VFX_LOOKUP_ID then
                return false
            end

            local fullName = SafeFullName(effect)
            if not string.find(fullName, "BP_VisualEffect_Status_Light_C", 1, true) then
                return false
            end

            local okWrite = pcall(function()
                effect.VisualEffectID = 21
            end)
            if okWrite then
                rewrote = true
            end
            return false
        end)
    end)

    return rewrote
end

local function RegisterLightWorldRuntimeRewriteHook()
    if _lightWorldRuntimeRewriteHookRegistered then
        return true
    end

    local okHook, preId, postId = pcall(function()
        return RegisterHook(
            "/Script/Pal.PalVisualEffectComponent:AddVisualEffect_ToALL",
            function(Context, VisualEffectID, Parameter, IssuerID)
                -- Pre-hook intentionally empty. The instance exists only after
                -- Palworld's native AddVisualEffect_ToALL implementation runs.
            end,
            function(Context, VisualEffectID, Parameter, IssuerID)
                local component = UnwrapHookValue(Context)
                if not component or not component:IsValid() then
                    return
                end

                local id = SafeScalarNumber(VisualEffectID)
                if id ~= LIGHT_VFX_LOOKUP_ID then
                    return
                end

                RewriteClientLightWorldRuntimeId(component)
            end)
    end)

    if okHook and preId ~= nil then
        _lightWorldRuntimeRewriteHookRegistered = true
        return true
    end

    Log("ERROR: Light world VFX runtime-ID rewrite hook registration failed: " ..
        tostring(postId))
    return false
end

RegisterLightWorldRuntimeRewriteHook()

-- -------------------------------------------------------------
-- Possession bootstrap
-- -------------------------------------------------------------
-- Reverted to the proven possession boundary.  Waza mutation is guarded by the
-- single process-global __ESE_WazaMutationApplied flag because poison scaling is
-- intentionally non-idempotent.  Either possession hook may arrive first.
local _esePresentationStartupComplete = false

local function EnsurePossessionPresentation(WorldContextObject, reason)
    if _esePresentationStartupComplete then
        return true
    end
    if not WorldContextObject or not WorldContextObject:IsValid() then
        return false
    end

    GetLightStatusIcon()
    GetLightVFXBP()
    GetLightCameraVFXBP()

    local worldVfxOK = RegisterLightVFXDatabaseClass(WorldContextObject)
    local cameraVfxOK = RegisterLightCameraVFXDatabaseClass(WorldContextObject)
    local iconHooksOK = RegisterLightIconHooks()

    if worldVfxOK and cameraVfxOK and iconHooksOK then
        _esePresentationStartupComplete = true
        Log("Presentation bootstrap complete at " .. tostring(reason) .. ".")
        return true
    end
    return false
end

local function RunPossessionBootstrap(WorldContextObject, reason)
    if not WorldContextObject or not WorldContextObject:IsValid() then
        return
    end

    if not _G.__ESE_WazaMutationApplied then
        local ok = InjectSkillStatusUnits(WorldContextObject)
        if ok then
            Log("Waza mutation triggered by " .. tostring(reason) .. ".")
        end
    end

    EnsurePossessionPresentation(WorldContextObject, reason)
end

local function BootstrapFromControllerHook(Context, reason)
    local controller = Context:get()
    if not controller or not controller:IsValid() then
        Log("ERROR: " .. tostring(reason) .. " provided no valid controller context.")
        return
    end
    RunPossessionBootstrap(controller, reason)
end

-- Original/proven trigger.  On standalone/listen paths this can be the first
-- possession callback observed by Lua.
SafeRegisterHook(
    "/Script/Engine.PlayerController:ServerAcknowledgePossession",
    function(Context)
        BootstrapFromControllerHook(Context, "ServerAcknowledgePossession")
    end)

-- Dedicated-client sibling of the possession handshake.  PalServer sends
-- ClientRestart(NewPawn) to the owning client before/around the acknowledgement
-- path, so direct dedicated-server joins no longer depend on the server RPC wrapper
-- being observed locally.
SafeRegisterHook(
    "/Script/Engine.PlayerController:ClientRestart",
    function(Context, NewPawn)
        BootstrapFromControllerHook(Context, "ClientRestart")
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

