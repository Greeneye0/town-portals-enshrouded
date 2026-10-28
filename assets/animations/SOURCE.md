# Portal casting motion

The custom cast uses Quaternius's **Spell Simple Enter**, **Spell Simple Shoot**,
and **Spell Simple Exit** from the Universal Animation Library.

- Author and original pack: https://quaternius.com/packs/universalanimationlibrary.html
- License: **CC0**, as stated on the author's pack page.
- Downloaded normalized GLB copies:
  - https://market.drawcall.ai/asset/spell-simple-enter (1.0.7)
  - https://market.drawcall.ai/asset/spell-simple-shoot (1.0.8)
  - https://market.drawcall.ai/asset/spell-simple-exit (1.0.7)
- Download date: 2026-10-24.

The ZIPs and original GLBs are retained under `source/`. The downloaded README
files are provenance data, not build instructions.

`tools/retarget-portal-cast.py` reads those GLBs and the locally extracted game
hierarchy with Blender's quaternion math. It creates `portal-cast-retargeted.json`
and `src/portal_cast_data.h`: 89 frames at 60 FPS, with 50 mapped rotations on
the game's 102-joint humanoid hierarchy. Native bone translations, scale, root
motion, legs, and facial joints are preserved at runtime.

The adapter blends the imported rotations into the local player's completed
native animation evaluation, before attachment transforms are calculated.
It does not trigger or replace the game's Point emote or alter packaged game
assets. The native hook is pinned to executable build MD5
`8766c13fb2324aa0c544ea9436c50f52`.

`casting-source.blend` is the original imported release clip.
`casting-retarget-preview.blend` and PNG show the retargeted skeleton at five
times. Those are offline previews; in-game verification is recorded separately.
