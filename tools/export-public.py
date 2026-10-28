"""Copy the public repository's files into ..\\TownPortals-public (github.com/Greeneye0/town-portals-enshrouded).

The working folder also holds research notes, reverse-engineering dumps, probes and old builds;
only what builds and documents the mod goes public: sources, tests, build scripts, the assets
the build ships, the generators, and the public README/LICENSE (tools/public). Files the
export no longer lists are removed there; its .git is left alone. Run: python export-public.py
"""
import pathlib
import shutil

root = pathlib.Path(__file__).resolve().parents[1]
out = root.parent / "TownPortals-public"
KEEP = [
    "src", "tests", "build.bat", "build-dev.bat", "deploy.py", "mod.json",
    "assets/voice", "assets/biomes", "assets/sounds/portal-summon.wav", "assets/sounds/portal-travel.wav",
    "assets/animations/SOURCE.md",  # the sounds' Pixabay original is not redistributed
    "assets/animations/portal-cast-retargeted.json",
    "assets/map/map-source-en.png", "assets/map/map-source-blank.png", "assets/map/map.jpg",
    "assets/map/map-en.jpg", "assets/map/backdrop.jpg", "assets/map/altar.png",
    "assets/map/altar-base.png", "assets/map/altar-flame.png",
    "tools/build-portal-shaders.py", "tools/build-portal-sounds.py", "tools/build-portal-fizzle.py",
    "tools/generate-portal-channel.py", "tools/generate-portal-shrug.py", "tools/make_locations.py",
    "tools/map/make_map.py", "tools/map/erase_labels.py", "tools/map/make_altar_icon.py",
    "tools/map/make_backdrop.py", "tools/biome-shots/export_defaults.py",
    "tools/package-release.py", "tools/release-readme.txt", "tools/export-public.py",
    "tools/make_nexus_images.py",
]
SKIP = ("__pycache__", ".pyc")
GITIGNORE = """# Builds, packages and runtime files
/bin/
/bin-dev/
/build/
/dist/
__pycache__/
*.obj
*.pdb
*.exp
*.lib
*.dll
*.exe
*.spv
*.log
"""

out.mkdir(exist_ok=True)
for item in out.iterdir():
    if item.name != ".git":
        shutil.rmtree(item) if item.is_dir() else item.unlink()
for rel in KEEP:
    src = root / rel
    dst = out / rel
    if src.is_dir():
        shutil.copytree(src, dst, ignore=shutil.ignore_patterns(*["__pycache__", "*.pyc"]))
    else:
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, dst)
for name in ("README.md", "LICENSE"):
    shutil.copy2(root / "tools/public" / name, out / name)
(out / ".gitignore").write_text(GITIGNORE, encoding="utf-8")
files = [p for p in out.rglob("*") if p.is_file() and ".git" not in p.relative_to(out).parts]
print(f"{out}: {len(files)} files, {sum(p.stat().st_size for p in files) / 1048576:.1f} MB")
