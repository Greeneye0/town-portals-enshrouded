"""Build the public release archive: dist/TownPortals-<version>.zip with client/ and server/.

client/: the Greeneye loader (dbgcore.dll) and greeneye/town-portal with the mod, its voice
lines, default pictures, loading-screen map and share-snapshots.enabled (snapshot sharing on).
server/: the loader and the mod for a dedicated server. No personal settings, saved snapshots
or logs. The archive is checked against a manifest after writing.
"""
import hashlib
import json
import pathlib
import shutil
import zipfile

root = pathlib.Path(__file__).resolve().parents[1]
version = json.loads((root / "mod.json").read_text())["version"]
name = f"TownPortals-{version}"
destination = root / "dist" / name
if destination.exists():
    shutil.rmtree(destination)
destination.mkdir(parents=True)
dll = root / "bin/loader/town-portal.dll"
assert json.loads((root / "bin/loader/mod.json").read_text())["version"] == version, "build.bat first"
# The Greeneye loader as installed beside the game and the dedicated server (1.2.0, identical).
loader = pathlib.Path(r"C:\Program Files (x86)\Steam\steamapps\common\Enshrouded\dbgcore.dll")
for side in ("client", "server"):
    folder = destination / side / "greeneye/town-portal"
    folder.mkdir(parents=True)
    shutil.copy2(dll, folder / dll.name)
    shutil.copy2(root / "bin/loader/mod.json", folder / "mod.json")
    shutil.copy2(loader, destination / side / "dbgcore.dll")
client = destination / "client/greeneye/town-portal"
(client / "share-snapshots.enabled").touch()
for source in sorted((root / "assets/voice").rglob("*.wav")):
    target = client / "voice" / source.relative_to(root / "assets/voice")
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, target)
biomes = sorted((root / "assets/biomes").glob("*.jpg"))
assert len(biomes) == 16, f"expected 16 default pictures in assets/biomes, found {len(biomes)}"
(client / "biomes").mkdir()
for source in biomes:
    shutil.copy2(source, client / "biomes" / source.name)
# The map lettered per language: map-<code>.jpg (tools/map/letter_map.py).
lettered = tuple(sorted(p.name for p in (root / "assets/map").glob("map-*.jpg")))
map_files = ("map.jpg", *lettered, "backdrop.jpg", "altar.png", "altar-base.png", "altar-flame.png")
(client / "map").mkdir()
for file in map_files:
    shutil.copy2(root / "assets/map" / file, client / "map" / file)
shutil.copy2(root / "tools/release-readme.txt", destination / "README.txt")

manifest = {
    str(p.relative_to(destination)).replace("\\", "/"): hashlib.sha256(p.read_bytes()).hexdigest()
    for p in sorted(destination.rglob("*")) if p.is_file()
}
(destination / "SHA256.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
archive = destination.parent / (name + ".zip")  # not with_suffix: the version has dots
archive.unlink(missing_ok=True)
with zipfile.ZipFile(archive, "x", compression=zipfile.ZIP_DEFLATED, compresslevel=6) as z:
    for p in sorted(destination.rglob("*")):
        if p.is_file():
            z.write(p, p.relative_to(destination).as_posix())
with zipfile.ZipFile(archive) as z:
    assert z.testzip() is None
    names = z.namelist()
    for filename, digest in manifest.items():
        assert hashlib.sha256(z.read(filename)).hexdigest() == digest, filename
    for side in ("client", "server"):
        assert z.read(f"{side}/greeneye/town-portal/town-portal.dll") == dll.read_bytes()
    assert sorted(n for n in names if "/biomes/" in n) == sorted(f"client/greeneye/town-portal/biomes/{b.name}" for b in biomes)
    assert all(f"client/greeneye/town-portal/map/{f}" in names for f in map_files)
    assert not any(n.endswith((".ini", ".tps", ".bin", ".log")) for n in names)
print(f"Verified {archive} ({archive.stat().st_size:,} bytes; {len(manifest)} files)")
print(f"town-portal.dll SHA256: {hashlib.sha256(dll.read_bytes()).hexdigest()}")
