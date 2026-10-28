# Builds src/portal_locations.h: the game's named locations (the "location discovered" areas: towns, camps, ruins,
# lakes, peaks ...) with their trigger shapes, so the portal can say "Fort Kelvin" instead of "Springlands".
#
# Game data (all static, from enshrouded.kfc via the map mod's kfclib):
#   - Location entities are ecs::TemplateResource templates (bundle 0x8619a0ad) named 8kMapLabel_* that carry
#     ecs::Location {isInLocation} + ecs::LocaTagComponent {locaTag GUID} + ecs::MapMarker {markerId GUID, type GUID}
#     + IsPlayerInRange / IsLocalPlayerInRange {rangeMargin} / KnowledgeTriggerRange. Name loca id = FNV-1a(GUID).
#   - The shape is per placement, in SceneEntityChunkResource.spawnData (bundle 0x6d579599, +0x34):
#     entries[i] {spawnMask u16 (keen::ecs::EntitySpawnValue bits), spawnIndexStart u16}; indices[start] = u8 component
#     count, u16 first component (absent when the count is 0), then one index per value array (u8, or u16 when that
#     array has > 255 entries);
#     bits 0..2 (TriggerRange, TriggerRange2, TriggerRange3) share ONE index into triggerRanges (+0x10):
#     SceneEntitySpawnEntryTriggerRange {triggerRange, triggerRange2, triggerRange3 f32; triggerOffset ecs::Offset
#     {localOffset f3, worldOffset f3, orientationOffset quat}}. Only TriggerRange set = sphere (the builder's
#     triggerShapeSphere); all three = box (triggerShapeBox, triggerRangeX/Y/Z), taken as half extents.
#   - Placement transform: SceneEntityChunkSpawnEntry {templateIndex u16, position x/y/z (frac u16, int u16),
#     orientation0/1 (smallest-three quat: bits 30-31 = index of the dropped largest component in x,y,z,w order, then
#     three 10-bit sign-magnitude fields, bits 20-29 / 10-19 / 0-9 = the remaining components in x,y,z,w order,
#     value = +-mag/511 / sqrt(2)), scale u16 (/8192)}.
#   - Per-placement ecs::SceneKnowledgeTrigger op {context u8, knowledgeId u32 @+4}: the PLAYER knowledge the game sets
#     when the player enters the range (= discovered).
# Run: python make_locations.py   (needs the map mod's tools/resources: kfclib.py, types.pkl, loca_lang1.bin)
import sys, os, struct, pickle, math, re

RES = r'C:\mods\enshrouded\UnlockMapMarkers\tools\resources'
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'src', 'portal_locations.h')
sys.path.insert(0, RES)
os.chdir(RES)
from kfclib import *  # noqa: E402

types = pickle.load(open('types.pkl', 'rb'))
byI = {}
for t in types:
    byI.setdefault(t['ihash'], t['qual']); byI.setdefault(t['qhash'], t['qual'])

def fnv(g):
    h = 0x811c9dc5
    for b in g: h = ((h ^ b) * 0x01000193) & 0xffffffff
    return h

d = open('loca_lang1.bin', 'rb').read(); rel, cnt = struct.unpack_from('<iI', d, 0); english = {}
for i in range(cnt):
    o = rel + i * 0x18; tid = struct.unpack_from('<I', d, o)[0]; r, n = struct.unpack_from('<iI', d, o + 4)
    english[tid] = d[o + 4 + r:o + 4 + r + n].decode('utf-8', 'replace')
def plain(s): return re.sub(r'<[^>]*>', '', s).strip()  # drop <location> style tags

def tcomps(b, off=0x10):
    rel, n = struct.unpack_from('<iI', b, off); base = off + rel
    for i in range(n):
        o = base + i * 12; th, a, sz = struct.unpack_from('<IIi', b, o)
        yield byI.get(th, ''), b[o + 4 + a:o + 4 + a + sz]

# Location templates: GUID -> (debug name, name loca id)
ZERO = bytes(16)
loc_templates = {}
for r in bundles.get(0x8619a0ad, []):
    g, blob = get_res_guid_and_blob(r); b = bytes(blob)
    rel, n = struct.unpack_from('<iI', b, 0)
    c = dict(tcomps(b))
    if 'keen::ecs::Location' not in c: continue
    tag = c.get('keen::ecs::LocaTagComponent', b'')[:16]
    if len(tag) < 16 or tag == ZERO: tag = c.get('keen::ecs::MapMarker', b'')[:16]
    if len(tag) < 16 or tag == ZERO: continue
    loc_templates[bytes(g)] = (b[rel:rel + n].decode('utf-8', 'replace') if n else '', fnv(tag))

def quat_unpack(o0, o1):
    v = o0 | (o1 << 16)
    big = v >> 30
    f = [(v >> 20) & 0x3ff, (v >> 10) & 0x3ff, v & 0x3ff]
    vals = [(-1 if x & 0x200 else 1) * (x & 0x1ff) / 511.0 / math.sqrt(2.0) for x in f]
    q = [0.0] * 4; k = 0
    for i in range(4):
        if i == big: continue
        q[i] = vals[k]; k += 1
    q[big] = math.sqrt(max(0.0, 1.0 - sum(x * x for x in vals)))
    return q  # x, y, z, w

def qmul(a, b):
    ax, ay, az, aw = a; bx, by, bz, bw = b
    return [aw * bx + ax * bw + ay * bz - az * by, aw * by - ax * bz + ay * bw + az * bx,
            aw * bz + ax * by - ay * bx + az * bw, aw * bw - ax * bx - ay * by - az * bz]

def qrot(q, v):
    x, y, z, w = q
    tx, ty, tz = 2 * (y * v[2] - z * v[1]), 2 * (z * v[0] - x * v[2]), 2 * (x * v[1] - y * v[0])
    return [v[0] + w * tx + (y * tz - z * ty), v[1] + w * ty + (z * tx - x * tz), v[2] + w * tz + (x * ty - y * tx)]

MAIN = bytes.fromhex('dbea9f50604c5f429c7cdeeefd9b6920')  # the overworld scene
rows, skipped = [], 0
for r in bundles.get(0x6d579599, []):
    if kfc[resource_keys_off + r * 32:resource_keys_off + r * 32 + 16] != MAIN: continue
    _, bl = get_res_guid_and_blob(r); b = bytes(bl)
    def sarr(off):
        rel, n = struct.unpack_from('<iI', b, off); return off + rel, n
    S = 0x34
    ta, tc = sarr(0); ea, ec = sarr(0x18); na, nn = sarr(S); ca, cn = sarr(S + 8); tra, trn = sarr(S + 0x10)
    ia, ic = sarr(S + 0x50)
    for j in range(ec):
        w = struct.unpack_from('<10H', b, ea + j * 20)
        if w[0] >= tc: continue
        t = loc_templates.get(b[ta + w[0] * 16:ta + w[0] * 16 + 16])
        if not t: continue
        if j >= nn: skipped += 1; continue
        mask, st = struct.unpack_from('<HH', b, na + j * 4)
        if not mask & 1: skipped += 1; continue
        # indices: u8 component count, u16 first component (only when the count is not 0), then the value indices
        ccount = b[ia + st]; p = ia + st + 1; cfirst = 0
        if ccount: cfirst = struct.unpack_from('<H', b, p)[0]; p += 2
        ti = struct.unpack_from('<H', b, p)[0] if trn > 255 else b[p]
        if ti >= trn: print('bad trigger index', t[0], hex(mask), b[ia + st:ia + st + 8].hex(), trn); skipped += 1; continue
        tr = struct.unpack_from('<3f3f3f4f', b, tra + ti * 0x34)
        ranges, local, world, qoff = tr[0:3], tr[3:6], tr[6:9], list(tr[9:13])
        know = 0
        for k in range(ccount):
            o = ca + (cfirst + k) * 12; th, a, sz = struct.unpack_from('<IIi', b, o)
            if byI.get(th, '').endswith('SceneKnowledgeTrigger'):
                kd = b[o + 4 + a:o + 4 + a + sz]
                for q in range(len(kd) // 16):
                    ctx, kid = struct.unpack_from('<B3xI', kd, q * 16)
                    if kid and not know: know = kid
        pos = [w[2] + w[1] / 65536, w[4] + w[3] / 65536, w[6] + w[5] / 65536]
        q = quat_unpack(w[7], w[8]); scale = w[9] / 8192.0
        lo = qrot(q, [x * scale for x in local])
        center = [pos[i] + lo[i] + world[i] for i in range(3)]
        isBox = (mask & 6) == 6 and ranges[1] > 0 and ranges[2] > 0
        rot = qmul(q, qoff) if isBox else [0.0, 0.0, 0.0, 1.0]
        ext = [x * scale for x in ranges] if isBox else [ranges[0] * scale] * 3
        if ext[0] <= 0: skipped += 1; continue
        rows.append(dict(dbg=t[0], loca=t[1], name=plain(english.get(t[1], '')), box=isBox, c=center, q=rot,
                         e=ext, know=know, pos=pos))

rows = [r for r in rows if r['name']]
rows.sort(key=lambda r: (r['name'], r['c'][0], r['c'][2]))
def f(x):
    s = ('%.3f' % x).rstrip('0').rstrip('.')
    if s in ('-0', ''): s = '0'
    return s + ('.f' if '.' not in s else 'f')
def esc(s): return s.replace('\\', '\\\\').replace('"', '\\"')
L = []
L.append('// Generated by tools/make_locations.py from the game\'s scene data (enshrouded.kfc). Do not edit; re-run it.')
L.append('// The game\'s named locations (the areas whose "location discovered" banner names a town, camp, ruin, lake ...):')
L.append('// 8kMapLabel_* scene entities with ecs::Location, their name loca id, and the trigger shape the game tests')
L.append('// (IsPlayerInRange: a sphere, or a rotated box with half extents). Overworld scene only (no dungeons).')
L.append('#pragma once')
L.append('#include <cstddef>')
L.append('#include <cstdint>')
L.append('')
L.append('struct GameLocation {')
L.append('    uint32_t nameLoca;      // loca id of the name (FNV-1a of the LocaTag GUID); text may carry <location> tags')
L.append('    const char* english;    // English name (tags stripped), for when the loca lookup is unavailable')
L.append('    float cx, cy, cz;       // shape centre, world metres (y up)')
L.append('    float qx, qy, qz, qw;   // box rotation (identity for spheres)')
L.append('    float ex, ey, ez;       // box half extents; sphere radius in ex (= ey = ez)')
L.append('    uint8_t isBox;')
L.append('    uint32_t knowledgeId;   // player knowledge the game sets on entering (discovered); 0 = none')
L.append('};')
L.append('')
L.append('constexpr size_t kGameLocationCount = %d;' % len(rows))
L.append('constexpr GameLocation kGameLocations[kGameLocationCount] = {')
for r in rows:
    c, q, e = r['c'], r['q'], r['e']
    L.append('    {0x%08xu, "%s", %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %d, 0x%08xu}, // %s' % (
        r['loca'], esc(r['name']), f(c[0]), f(c[1]), f(c[2]), f(q[0]), f(q[1]), f(q[2]), f(q[3]),
        f(e[0]), f(e[1]), f(e[2]), 1 if r['box'] else 0, r['know'], r['dbg']))
L.append('};')
L.append('''
// Is the world point inside location l's trigger shape? margin scales the shape (the game leaves at x1.1-1.2).
inline bool GameLocationContains(const GameLocation& l, float x, float y, float z, float margin = 1.0f) {
    float dx = x - l.cx, dy = y - l.cy, dz = z - l.cz;
    if (!l.isBox) {
        float r = l.ex * margin;
        return dx * dx + dy * dy + dz * dz <= r * r;
    }
    // into box space: rotate by the inverse (conjugate) quaternion
    float qx = -l.qx, qy = -l.qy, qz = -l.qz, qw = l.qw;
    float tx = 2.f * (qy * dz - qz * dy), ty = 2.f * (qz * dx - qx * dz), tz = 2.f * (qx * dy - qy * dx);
    float lx = dx + qw * tx + (qy * tz - qz * ty);
    float ly = dy + qw * ty + (qz * tx - qx * tz);
    float lz = dz + qw * tz + (qx * ty - qy * tx);
    return (lx < 0 ? -lx : lx) <= l.ex * margin && (ly < 0 ? -ly : ly) <= l.ey * margin &&
           (lz < 0 ? -lz : lz) <= l.ez * margin;
}

// The narrowest (smallest-volume) location containing the point, or -1 (then fall back to the region).
// accept (optional) filters candidates, e.g. to only discovered locations.
inline int FindGameLocation(float x, float y, float z, bool (*accept)(const GameLocation&) = nullptr) {
    int best = -1;
    float bestVolume = 0.f;
    for (size_t i = 0; i < kGameLocationCount; ++i) {
        const GameLocation& l = kGameLocations[i];
        if (!GameLocationContains(l, x, y, z)) continue;
        if (accept && !accept(l)) continue;
        float v = l.isBox ? 8.f * l.ex * l.ey * l.ez : 4.18879f * l.ex * l.ex * l.ex;
        if (best < 0 || v < bestVolume) { best = static_cast<int>(i); bestVolume = v; }
    }
    return best;
}''')
open(OUT, 'w', encoding='utf-8', newline='\n').write('\n'.join(L) + '\n')
print('locations', len(rows), 'boxes', sum(r['box'] for r in rows), 'skipped', skipped,
      'with knowledge', sum(1 for r in rows if r['know']), '->', os.path.abspath(OUT))
