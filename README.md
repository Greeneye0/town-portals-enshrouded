# Town Portals for Enshrouded

*Saving your ass since 2026.*

Open a portal home from anywhere. Press **T**: after a short cast, a portal opens in front of you
and its twin beside your preferred Flame Altar. Step through, travel home, and come back the
same way. Multiplayer, every text in Enshrouded's 15 languages.

Download and full player notes: [Nexus Mods](https://www.nexusmods.com/enshrouded/mods/) ·
the release zip on this repository's Releases page.

## Features

- **Portals with a view.** Each end shows a snapshot of where it leads, with depth as you move
  (taken when you travel; not a live image). Before the first trip, the altar's portal shows a
  landmark of that region, by day or by night. Your altar's picture is saved for the next login.
- **Multiplayer.** Other players can use your portal, and see its pictures (shared between
  clients). Dedicated servers run the server companion.
- **A travel map.** While you travel, a painted world map shows the trip: your portal, your
  altar, and a path that grows as the destination loads.
- **Lighter portals** on low graphics settings (or `PortalQuality=Low`).
- **15 languages**, following the game's own language; voice lines when a portal can't open.

When portals close, settings, install steps and known limitations:
[tools/release-readme.txt](tools/release-readme.txt) (the README shipped in the zip).

## Building

Windows, Visual Studio 2022 Build Tools (x64), Python 3 with Pillow and NumPy.

```
build.bat        release build: bin\loader\town-portal.dll, then the offline tests
build-dev.bat    development build (bin-dev\loader) with the diagnostics bridge
python deploy.py copy the release build and its assets into the game folder (game closed)
python tools\package-release.py   the release zip in dist\
```

The mod runs under the Greeneye Loader (API 2, `src/greeneye.h`). Native addresses are found
by signature at start-up and checked against the installed game; a game update can need new
signatures (`tests/test_patches.cpp` checks them offline).

Generated sources: `tools/build-portal-shaders.py` (Vulkan SPIR-V from `src/shaders`, needs
glslang), `tools/map/*.py` (the loading-screen map, its labels and warp),
`tools/biome-shots/export_defaults.py` (the default pictures).

## Layout

- `src/main.cpp` gameplay: casting, travel, prompts, the portal list and multiplayer messages
- `src/portal_render.cpp` HUD: prompts, notices, the loading screen and its map
- `src/portal_scene_probe.cpp`, `portal_composite.inl`, `portal_snapshot_gpu.inl`: the in-frame
  portal renderer, snapshots and their GPU transfers
- `src/portal_snapshot_share.cpp`, `portal_snapshot_cache.cpp`: sharing pictures and the saved
  home picture
- `tests/` offline tests, run by `build.bat`

## Credits

- Testing: Lomack, our test guinea pig. Speedy recovery from the teleport accident!
- Cast animation: Quaternius, Universal Animation Library (CC0); see `assets/animations/SOURCE.md`.
- Portal sounds: made from "Whoosh Sci-Fi Portal" by virtualzero (Pixabay).
- Voice lines are AI-generated; the banner art and the loading-screen map are AI-generated.

## License

MIT, see [LICENSE](LICENSE). Enshrouded is © Keen Games; this is an unofficial fan mod.
