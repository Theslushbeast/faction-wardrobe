# Changelog

## 0.2.2
- **Revert this faction**: deletes a faction's outfit rule; its members go back to their own outfits.
- **Revert all factions** (with a confirmation step); the previous settings stay in `FactionWardrobe.json.bak`.
- Armor handed out for an outfit an NPC no longer has (reverted or changed faction, reused slot) is removed when the NPC loads.

## 0.2.1
- Save & apply and Apply now switch the mod back on if it was off. A red notice with a **Turn back on & apply** button shows while it is off.
- The off switch is now called "Turn off: everyone back in their own outfits" and keeps your rules.

## 0.2.0
- Outfits are real outfit records (`FactionWardrobe.esp`, 128 slots) set as the faction members' default outfit, so the game keeps NPCs dressed by itself (resetinventory, cell resets, reloads). Changes apply live.
- The outfit change is undone around every save, so saves never depend on the mod.
- Saves from 0.1 are cleaned up automatically when each NPC loads.

## 0.1.1
- Re-equips an outfit when it is no longer worn (resetinventory, other mods).

## 0.1.0
- First test build: per-faction, per-sex armor picked in SKSE Menu Framework, equipped directly on NPCs.
