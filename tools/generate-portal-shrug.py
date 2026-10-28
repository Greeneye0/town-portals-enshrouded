"""Generate the shrug Town Portal plays when there is no room for a portal (with the voice line).

Same skeleton space and maths as tools/generate-portal-channel.py (+Z forward, +Y up, left arm
at -X): the shoulders lift (collarbones 39 and 69) with a small double bounce, the head tilts
to one side and back, and both hands come up in front with the elbows at the sides, palms up
and fingers open (two-bone IK from the shoulders). Legs and spine keep the native animation.
The runtime blends it in and out (see portal_cast.cpp).

Run: python -I tools/generate-portal-shrug.py  ->  src/portal_shrug_data.h
"""
import json, math
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
native = json.loads((ROOT / 'tools/native-animation/87b29df6-6518-43ef-9298-e3f85a6204b9_358470de_0.json').read_text())
parents = native['jointParentIndices']
joints = native['defaultPoseJoints'][:len(parents)]

def qmul(a, b):
    ax, ay, az, aw = a; bx, by, bz, bw = b
    return (aw*bx + ax*bw + ay*bz - az*by, aw*by - ax*bz + ay*bw + az*bx,
            aw*bz + ax*by - ay*bx + az*bw, aw*bw - ax*bx - ay*by - az*bz)
def qinv(q): return (-q[0], -q[1], -q[2], q[3])
def rot(q, v):
    x, y, z, w = q; vx, vy, vz = v
    t = (2*(y*vz - z*vy), 2*(z*vx - x*vz), 2*(x*vy - y*vx))
    return (vx + w*t[0] + y*t[2] - z*t[1], vy + w*t[1] + z*t[0] - x*t[2], vz + w*t[2] + x*t[1] - y*t[0])
def norm(v):
    l = math.sqrt(sum(c*c for c in v)); return tuple(c/l for c in v)
def cross(a, b): return (a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0])
def dot(a, b): return sum(x*y for x, y in zip(a, b))
def sub(a, b): return tuple(x-y for x, y in zip(a, b))
def axis_angle(axis, angle):
    s = math.sin(angle/2); a = norm(axis); return (a[0]*s, a[1]*s, a[2]*s, math.cos(angle/2))
def turn(axis, degrees): return axis_angle(axis, math.radians(degrees))
def swing(a, b):
    a, b = norm(a), norm(b); c = cross(a, b); d = dot(a, b)
    if d < -0.9999: return axis_angle((0, 1, 0), math.pi)
    q = (c[0], c[1], c[2], 1 + d); l = math.sqrt(sum(x*x for x in q)); return tuple(x/l for x in q)
def quat_from_basis(x, y, z):
    m = ((x[0], y[0], z[0]), (x[1], y[1], z[1]), (x[2], y[2], z[2]))
    tr = m[0][0] + m[1][1] + m[2][2]
    if tr > 0:
        w = math.sqrt(1 + tr) * 2
        return ((m[2][1]-m[1][2])/w, (m[0][2]-m[2][0])/w, (m[1][0]-m[0][1])/w, w/4)
    i = max(range(3), key=lambda k: m[k][k]); j, k = (i+1) % 3, (i+2) % 3
    w = math.sqrt(1 + m[i][i] - m[j][j] - m[k][k]) * 2
    q = [0.0, 0.0, 0.0, (m[k][j]-m[j][k])/w]
    q[i] = w/4; q[j] = (m[j][i]+m[i][j])/w; q[k] = (m[k][i]+m[i][k])/w
    return tuple(q)
def frame(fingers, palm):
    f = norm(fingers); n = norm(sub(palm, tuple(f[k]*dot(palm, f) for k in range(3))))
    return f, n, cross(f, n)

restLocal = [tuple(j['orientation'][a] for a in 'xyzw') for j in joints]
restPos = [tuple(j['position'][a] for a in 'xyz') for j in joints]
G = []; P = []
for i, p in enumerate(parents):
    if p == 65535: G.append(restLocal[i]); P.append(restPos[i])
    else:
        G.append(qmul(G[p], restLocal[i]))
        P.append(tuple(P[p][k] + rot(G[p], restPos[i])[k] for k in range(3)))
assert P[5][2] > P[4][2] and P[44][0] < 0 < P[74][0], 'skeleton axes changed'

FORWARD, UP = (0.0, 0.0, 1.0), (0.0, 1.0, 0.0)
ARMS = [(-1, 40, 41, 42, 43, 44), (1, 70, 71, 72, 73, 74)]
CLAVICLES = {-1: 39, 1: 69}
NECK, HEAD = 16, 17
FPS, DURATION = 60, 1.7

def smooth(x): x = min(1.0, max(0.0, x)); return x*x*(3 - 2*x)

def lift(t):
    """How far the shoulders are up (0..1): up fast, a small double bounce, then down."""
    up = smooth(t/0.3) * (1 - smooth((t - 1.25)/0.4))
    bounce = 0.18*math.sin(2*math.pi*(t - 0.3)/0.45) if 0.3 < t < 1.2 else 0.0
    return max(0.0, up*(1 + bounce))

def extras(t):
    """World-space rotations about each joint (applied parent first, as in the channel)."""
    a = lift(t)
    out = {CLAVICLES[-1]: turn(FORWARD, -9*a), CLAVICLES[1]: turn(FORWARD, 9*a)}
    out[NECK] = turn((1, 0, 0), -4*a)
    out[HEAD] = qmul(turn(FORWARD, 8*a), turn((1, 0, 0), -5*a))
    return out

def posed(ex):
    R, Gp, Pp = [], [], []
    for i, p in enumerate(parents):
        Rp = R[p] if p != 65535 else (0.0, 0.0, 0.0, 1.0)
        R.append(qmul(ex[i], Rp) if i in ex else Rp)
        Gp.append(qmul(R[i], G[i]))
        Pp.append(P[i] if p == 65535 else tuple(Pp[p][k] + rot(R[p], sub(P[i], P[p]))[k] for k in range(3)))
    return Gp, Pp

def palm_up(hand, middle, side):
    """Fingers forward and a little out, palm facing up."""
    restF, restN, restS = frame(sub(P[middle], P[hand]), (-side, 0.0, 0.0))
    goalF, goalN, goalS = frame((side*0.45, 0.15, 1.0), UP)
    return qmul(quat_from_basis(goalF, goalN, goalS), qinv(quat_from_basis(restF, restN, restS)))

def hand_target(side, t, shoulderY):
    """Hands out in front at waist height, a little apart; they rise with the shoulders."""
    return (side*0.36, shoulderY - 0.19, 0.19)

def solve_elbow(shoulder, target, side, upperLen, lowerLen):
    toTarget = sub(target, shoulder)
    dist = min(math.sqrt(dot(toTarget, toTarget)), upperLen + lowerLen - 1e-4)
    forward = norm(toTarget)
    along = (upperLen*upperLen + dist*dist - lowerLen*lowerLen) / (2*dist)
    height = math.sqrt(max(0.0, upperLen*upperLen - along*along))
    pole = (side*0.1, -1.0, -0.15)                     # elbows down by the ribs, a little back and out
    pole = norm(sub(pole, tuple(forward[k]*dot(pole, forward) for k in range(3))))
    elbow = tuple(shoulder[k] + forward[k]*along + pole[k]*height for k in range(3))
    return elbow, tuple(shoulder[k] + forward[k]*dist for k in range(3))

def arm_pose(t, PG, PP):
    out = {}
    for side, upper, twistU, lower, twistL, hand in ARMS:
        upperLen = math.sqrt(dot(sub(P[lower], P[upper]), sub(P[lower], P[upper])))
        lowerLen = math.sqrt(dot(sub(P[hand], P[lower]), sub(P[hand], P[lower])))
        shoulder = PP[upper]
        elbow, wrist = solve_elbow(shoulder, hand_target(side, t, shoulder[1]), side, upperLen, lowerLen)
        g = {upper: PG[upper]}
        vU = rot(qinv(G[upper]), sub(P[lower], P[upper]))
        g[upper] = qmul(swing(rot(g[upper], vU), norm(sub(elbow, shoulder))), g[upper])
        g[twistU] = qmul(g[upper], restLocal[twistU])
        g[lower] = qmul(g[twistU], restLocal[lower])
        vL = rot(qinv(G[lower]), sub(P[hand], P[lower]))
        g[lower] = qmul(swing(rot(g[lower], vL), norm(sub(wrist, elbow))), g[lower])
        g[twistL] = qmul(g[lower], restLocal[twistL])
        g[hand] = qmul(palm_up(hand, hand + 9, side), G[hand])
        out.update(g)
    return out

FINGERS = set(range(45, 64)) | set(range(75, 94))
indices = sorted(set(CLAVICLES.values()) | {NECK, HEAD} | set(j for arm in ARMS for j in arm[1:]) | FINGERS)
frames = []
for f in range(int(round(DURATION*FPS)) + 1):
    t = f / FPS
    PG, PP = posed(extras(t))
    final = list(PG)
    for j, q in arm_pose(t, PG, PP).items(): final[j] = q
    pose = []
    for i in indices:
        if i in FINGERS: pose.append(restLocal[i]); continue
        pose.append(qmul(qinv(final[parents[i]]), final[i]))
    frames.append(pose)

lines = ['// Generated by tools/generate-portal-shrug.py: 1.7 s shrug, hands up palms up, played with the',
         '// "no room" voice line; authored in the game skeleton\'s own space (+Z forward, +Y up, left arm at -X).',
         '#pragma once', '#include <cstdint>', 'namespace portal::casting::shrug {',
         f'constexpr uint32_t kFps={FPS};', f'constexpr uint32_t kFrames={len(frames)};',
         f'constexpr float kDuration={DURATION:.9f}f;',
         f'constexpr uint16_t kJoints[]={{{",".join(str(i) for i in indices)}}};',
         'constexpr float kRotations[kFrames][sizeof(kJoints)/sizeof(kJoints[0])][4]={']
for pose in frames:
    lines.append('{' + ','.join('{' + ','.join(f'{v:.9f}f' for v in q) + '}' for q in pose) + '},')
lines += ['};', '}']
(ROOT / 'src/portal_shrug_data.h').write_text('\n'.join(lines) + '\n')

PG, PP = posed(extras(0.7))
g = arm_pose(0.7, PG, PP)
def fk(side_joints):
    side, upper, twistU, lower, twistL, hand = side_joints
    elbow = tuple(PP[upper][k] + rot(g[upper], rot(qinv(G[upper]), sub(P[lower], P[upper])))[k] for k in range(3))
    wrist = tuple(elbow[k] + rot(g[lower], rot(qinv(G[lower]), sub(P[hand], P[lower])))[k] for k in range(3))
    return [round(c, 3) for c in elbow], [round(c, 3) for c in wrist]
print(json.dumps({'frames': len(frames), 'joints': len(indices), 'leftElbowHand': fk(ARMS[0]), 'rightElbowHand': fk(ARMS[1]),
                  'shoulders': [[round(c, 3) for c in PP[40]], [round(c, 3) for c in PP[70]]]}))
