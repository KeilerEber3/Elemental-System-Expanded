# Elemental-System-Expanded
Palworld mod

It is a cpp+lua+pak-patch mod. The changes are runtime/content-level: the Lua mutates DT_WazaDataTable, mastery-related data and VFX registration in memory; the C++ keeps ICD/provenance/status state in process memory; the PAKs replace/add cooked assets. None of those are explicitly serialized by ESE into a save file. In other words, it is SAFE TO ADD OR REMOVE IT FROM AN EXISTING SAVE.

Designed in a multiplayer-compatible way and LIKELY compatible with co-op. Verified and works on dedicated servers, all you need as a server owner is to put the mod's package name into server's PalModSettings.ini:

[PalModSettings]
bGlobalEnableMod=True
WorkshopRootDir=D:\FolderG\Steam\steamapps\workshop\content\1623730
ConfigVersion=1.0
ActiveModList=UE4SSExperimentalPW
ActiveModList=ElementalSystemExpanded      ← ALWAYS under UE4SS

Without the move it would not activate Server-side payload that the mod is designated for server. Every guest must have ESE installed. As for listen-server coop - every client and the host must have ESE installed.

Key features:
- Removed build-up mechanics for elemental damage. Every elemental damage hit to a target clear of any elemental status applies the corresponding status instantly. Poison keeps build-up mechanics, but for each poison waza aggregate points were doubled.
- Neutral renamed to Light. Light applies light blindness status similar to Darkness. Light and Dark share ICD and mechanics. Light has increased damage to Dark, but cannot blind Dark. Dark is the only element that is immune to both blindnesses: Dark and Light statuses.
- Ice status' Freeze does not trigger ice mesh freezing upon apply. Electrical status' Electrify does not trigger shock flinch upon apply. There are two elemental reactions currently codded in the mod: Water+Ice, Water+Electrical, no other way around. Once you apply Water to a target, next Electrical hit will produce shock flinch stun reaction. The implementation intentionally does not work in reverse order. Once you apply Water to a target, next Ice hit will freeze target in ice mesh, immobilizing them for 2 seconds. Each of Water reactions has 14 seconds ICD.
- Elemental matchup completely changed to fit my based vision of how it should be. "How it should be" includes removing Dragon element from vanilla elemental matchup entirely. Dragon element is not deleted, it is given a new purpose instead, which is described more detailed below. The matchup itself is self-explanatory once you look at the preview depicting it. Each element has multiple counters and weaknesses now. You may think that Water is the weakest, but Water and only Water gives you the opportunity to execute stunning mechanics.
- Each pal waza now has 1U/2U aggregate: the weak one applies 7.5 seconds duration, the strong one applies 12 seconds duration of the wasa's elemental status. Each hit of the same elemental damage prolongs duration, but it is capped with 12 seconds. Light and Dark unlike others have statuses with only 3 seconds duration. Poison is unchanged.
- Same-element hit → apply/increment status based on gauge, capped with 12 seconds.
- Different element → erodes current remaining duration by 7.5s for 1U or 12s for 2U; if incoming status has at least 2.5s exchange left or more, it is applied for the duration left. Except wet Freeze and wet Electrify, they skip the status erosion entirely.
- Even if status apply is "guaranteed" with the mod, the elemental ICD does not allow to apply the same status on literally each hit, there is 2.5-second/third-hit rule: first consecutive hit gets ignored, second consecutive hit gets ignored, the third one gets registered and applies status, OR 2.5 seconds should have passed.

About Dragon element. So, effectively, vanilla Dragon carries burn aggregate and looks like purple flames, okay, fair enough, then:

- Water + Dragon → Steam Burst — converts water into vapor, AOE, 0.5U Water.
- Earth + Dragon → Lava Burst — violently melts/erupts earth, AOE, 0.5U Fire.
- Leaf + Dragon → Flammable Spray / Wildfire Burst — spray biomass combustion, AOE, 0.5U Fire.
- Fire + Dragon → Flashover — runaway combustion wave, AOE, 0.5U Fire.
- Ice + Dragon → Thermal Shock — violent thermal fracture, AOE, 0.5U Ice.
- Electric + Dragon → Arc Burst — electrical discharge, AOE, 0.5U Electric.
