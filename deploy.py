import os
import shutil
import sys

GAME_DIR = r"C:\Program Files (x86)\Steam\steamapps\common\Enshrouded"
MOD_ID = "town-portal"
DEST_DIR = os.path.join(GAME_DIR, "greeneye", MOD_ID)
HERE = os.path.dirname(os.path.abspath(__file__))
SRC_DLL = os.path.join(HERE, "bin", "loader", f"{MOD_ID}.dll")
SRC_JSON = os.path.join(HERE, "bin", "loader", "mod.json")

def main():
    if not os.path.exists(SRC_DLL) or not os.path.exists(SRC_JSON):
        print(f"ERROR: Build artifacts not found in {os.path.dirname(SRC_DLL)}. Run build.bat first.")
        return 1
    os.makedirs(DEST_DIR, exist_ok=True)
    shutil.copy2(SRC_DLL, os.path.join(DEST_DIR, f"{MOD_ID}.dll"))
    shutil.copy2(SRC_JSON, os.path.join(DEST_DIR, "mod.json"))
    voice = os.path.join(HERE, "assets", "voice")
    if os.path.isdir(voice):
        # Replace, not merge: voices and lines the build dropped must not linger.
        shutil.rmtree(os.path.join(DEST_DIR, "voice"), ignore_errors=True)
        shutil.copytree(voice, os.path.join(DEST_DIR, "voice"))
    biomes = os.path.join(HERE, "assets", "biomes")
    if os.path.isdir(biomes):
        # The default altar-portal pictures (tools/biome-shots/export_defaults.py).
        shutil.rmtree(os.path.join(DEST_DIR, "biomes"), ignore_errors=True)
        shutil.copytree(biomes, os.path.join(DEST_DIR, "biomes"))
    # The loading-screen map (tools/map/make_map.py).
    os.makedirs(os.path.join(DEST_DIR, "map"), exist_ok=True)
    lettered = sorted(n for n in os.listdir(os.path.join(HERE, "assets", "map")) if n.startswith("map-") and n.endswith(".jpg"))
    for name in ("map.jpg", *lettered, "backdrop.jpg", "altar.png", "altar-base.png", "altar-flame.png"):
        shutil.copy2(os.path.join(HERE, "assets", "map", name), os.path.join(DEST_DIR, "map", name))
    print(f"Deployed {MOD_ID}.dll, mod.json, voice lines, default pictures and map -> {DEST_DIR}")
    return 0

if __name__ == "__main__":
    sys.exit(main())
