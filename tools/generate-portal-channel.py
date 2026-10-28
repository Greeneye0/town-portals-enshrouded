"""Generate the 3-second portal channel animation for the game's humanoid hierarchy.

Plain Python (no Blender). Works directly in the skeleton's own model space,
measured from its rest pose: toes sit at +Z from the ankles, so the character
faces +Z, up is +Y and the left arm (joints 39-44) is at -X. The earlier
Quaternius retarget converted glTF with a 180-degree turn about Y, which fixed
left/right but flipped forward, so its spell thrust went out behind the player.

The gesture is a conjurer's circle: the left hand is held out in front of the
chest while the right hand traces quick circles in front of it (two-bone IK
from the shoulder, elbows down and out). Twist joints, fingers, spine and legs
keep the native animation. The runtime blends this in and out (see
portal_cast.cpp).

Run: python -I tools/generate-portal-channel.py
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
def swing(a, b):
    """Shortest rotation taking unit vector a to unit vector b."""
    a, b = norm(a), norm(b); c = cross(a, b); d = dot(a, b)
    if d < -0.9999: return axis_angle((0, 1, 0), math.pi)
    q = (c[0], c[1], c[2], 1 + d); l = math.sqrt(sum(x*x for x in q)); return tuple(x/l for x in q)

restLocal = [tuple(j['orientation'][a] for a in 'xyzw') for j in joints]
restPos = [tuple(j['position'][a] for a in 'xyz') for j in joints]
G = []; P = []
for i, p in enumerate(parents):
    if p == 65535: G.append(restLocal[i]); P.append(restPos[i])
    else:
        G.append(qmul(G[p], restLocal[i]))
        P.append(tuple(P[p][k] + rot(G[p], restPos[i])[k] for k in range(3)))

# Sanity: the axes this pose relies on.
assert P[5][2] > P[4][2] and P[9][2] > P[8][2], 'toes must be at +Z (forward)'
assert P[44][0] < 0 < P[74][0], 'left hand must be at -X'

FORWARD, UP = (0.0, 0.0, 1.0), (0.0, 1.0, 0.0)
# side = -1 for the left arm (at -X), +1 for the right arm.
ARMS = [(-1, 40, 41, 42, 43, 44), (1, 70, 71, 72, 73, 74)]
FPS, DURATION = 60, 3.0

TURNS_PER_SECOND = 1.4

def quat_from_basis(x, y, z):
    """Rotation whose columns are the orthonormal vectors x, y, z."""
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
    """Orthonormal basis from a finger direction and a palm normal."""
    f = norm(fingers); n = norm(sub(palm, tuple(f[k]*dot(palm, f) for k in range(3))))
    return f, n, cross(f, n)

# Palms out: fingers up (tipped a little forward), palm facing away from the
# player. In the rest pose the fingers hang down and each palm faces the body;
# the middle knuckle (53/83) gives the finger direction.
def palm_rotation(hand, middle, side):
    restF, restN, restS = frame(sub(P[middle], P[hand]), (-side, 0.0, 0.0))
    goalF, goalN, goalS = frame((0.0, 1.0, 0.25), FORWARD)
    a = quat_from_basis(restF, restN, restS); b = quat_from_basis(goalF, goalN, goalS)
    return qmul(b, qinv(a))

def turn(axis, degrees): return axis_angle(axis, math.radians(degrees))

# Stance: a low lunge. The pelvis drops (the runtime lowers its translation by
# PELVIS_DROP), the body turns so the right shoulder leads and the chest leans in, the
# head turns back toward the portal, and two-bone leg IK plants the feet flat in a long
# stride: right foot forward, left foot back, knees bent.
PELVIS = 1
PELVIS_DROP = 0.15
STANCE = {PELVIS: turn(UP, -12),
          16: turn(UP, 10), 17: qmul(turn(UP, 12), turn((1, 0, 0), -4))}
for j in range(10, 16): STANCE[j] = qmul(turn(UP, -3.5), turn((1, 0, 0), 2.2))
# (hip, knee, ankle, foot target on the ground at rest ankle height)
LEGS = [(6, 7, 8, (0.17, P[8][1], 0.36)), (2, 3, 4, (-0.19, P[4][1], -0.40))]

def posed(extras, drop=0.0):
    """Globals and positions with the extra rotations applied (parents come first)."""
    R, Gp, Pp = [], [], []
    for i, p in enumerate(parents):
        Rp = R[p] if p != 65535 else (0.0, 0.0, 0.0, 1.0)
        R.append(qmul(extras[i], Rp) if i in extras else Rp)
        Gp.append(qmul(R[i], G[i]))
        if p == 65535: Pp.append(P[i])
        else:
            here = tuple(Pp[p][k] + rot(R[p], sub(P[i], P[p]))[k] for k in range(3))
            if i == PELVIS: here = (here[0], here[1] - drop, here[2])
            Pp.append(here)
    return Gp, Pp

POSED_G, POSED_P = posed(STANCE, PELVIS_DROP)

def leg_pose():
    out = {}
    for hip, knee, ankle, target in LEGS:
        upperLen = math.sqrt(dot(sub(P[knee], P[hip]), sub(P[knee], P[hip])))
        lowerLen = math.sqrt(dot(sub(P[ankle], P[knee]), sub(P[ankle], P[knee])))
        side = 1 if P[hip][0] > 0 else -1
        kneePos, footPos = solve_joint(POSED_P[hip], target, (0.15*side, 0.0, 1.0), upperLen, lowerLen)
        g = {hip: POSED_G[hip]}
        vU = rot(qinv(G[hip]), sub(P[knee], P[hip]))
        g[hip] = qmul(swing(rot(g[hip], vU), norm(sub(kneePos, POSED_P[hip]))), g[hip])
        g[knee] = qmul(g[hip], restLocal[knee])
        vL = rot(qinv(G[knee]), sub(P[ankle], P[knee]))
        g[knee] = qmul(swing(rot(g[knee], vL), norm(sub(footPos, kneePos))), g[knee])
        # Feet flat, turned with the pelvis.
        g[ankle] = qmul(STANCE[PELVIS], G[ankle])
        out.update(g)
    return out

def hand_target(side, t):
    """Model-space hand position (metres) at time t."""
    if side < 0:
        # The ring hand, raised near the face with a small sway.
        return (-0.12, 1.38 + 0.01*math.sin(2*math.pi*0.9*t), 0.24)
    # The forward hand, pushed out at chest height, traces the circle; the radius opens
    # over the first 0.3 s.
    radius = 0.10 * min(1.0, t/0.3)
    angle = 2*math.pi*TURNS_PER_SECOND*t
    return (0.05 + radius*math.cos(angle), 1.17 + radius*math.sin(angle), 0.50)

def solve_joint(root, target, pole, upperLen, lowerLen):
    """Two-bone IK: middle joint and end positions for a target, bending toward pole."""
    toTarget = sub(target, root)
    dist = min(math.sqrt(dot(toTarget, toTarget)), upperLen + lowerLen - 1e-4)
    forward = norm(toTarget)
    along = (upperLen*upperLen + dist*dist - lowerLen*lowerLen) / (2*dist)
    height = math.sqrt(max(0.0, upperLen*upperLen - along*along))
    pole = norm(sub(pole, tuple(forward[k]*dot(pole, forward) for k in range(3))))
    mid = tuple(root[k] + forward[k]*along + pole[k]*height for k in range(3))
    return mid, tuple(root[k] + forward[k]*dist for k in range(3))

def solve_elbow(shoulder, target, side, upperLen, lowerLen):
    """Two-bone IK: elbow and hand positions for a reachable target."""
    toTarget = sub(target, shoulder)
    dist = min(math.sqrt(dot(toTarget, toTarget)), upperLen + lowerLen - 1e-4)
    forward = norm(toTarget)
    along = (upperLen*upperLen + dist*dist - lowerLen*lowerLen) / (2*dist)
    height = math.sqrt(max(0.0, upperLen*upperLen - along*along))
    # Elbows down and out; the raised ring-hand elbow swings wider.
    pole = (0.6, -1.0, -0.3) if side > 0 else (-0.9, -0.7, -0.3)
    pole = norm(sub(pole, tuple(forward[k]*dot(pole, forward) for k in range(3))))
    elbow = tuple(shoulder[k] + forward[k]*along + pole[k]*height for k in range(3))
    hand = tuple(shoulder[k] + forward[k]*dist for k in range(3))
    return elbow, hand

def arm_pose(t):
    """Global rotations for both arms at time t (seconds), from the posed shoulders."""
    out = {}
    for side, upper, twistU, lower, twistL, hand in ARMS:
        upperLen = math.sqrt(dot(sub(P[lower], P[upper]), sub(P[lower], P[upper])))
        lowerLen = math.sqrt(dot(sub(P[hand], P[lower]), sub(P[hand], P[lower])))
        shoulder = POSED_P[upper]
        elbow, wrist = solve_elbow(shoulder, hand_target(side, t), side, upperLen, lowerLen)
        upperDir = norm(sub(elbow, shoulder))
        lowerDir = norm(sub(wrist, elbow))
        g = {}
        g[upper] = POSED_G[upper]
        # Bone vector from this joint to the next main joint, in its own frame.
        vU = rot(qinv(G[upper]), sub(P[lower], P[upper]))
        g[upper] = qmul(swing(rot(g[upper], vU), upperDir), g[upper])
        g[twistU] = qmul(g[upper], restLocal[twistU])
        g[lower] = qmul(g[twistU], restLocal[lower])
        vL = rot(qinv(G[lower]), sub(P[hand], P[lower]))
        g[lower] = qmul(swing(rot(g[lower], vL), lowerDir), g[lower])
        g[twistL] = qmul(g[lower], restLocal[twistL])
        # Palm out, facing away from the player.
        g[hand] = qmul(palm_rotation(hand, hand + 9, side), G[hand])
        out.update(g)
    return out

# Open hands: the fingers (thumb to little finger, 45-63 and 75-93) take their straight rest
# pose instead of the native grip, so the hands are spread even while holding a weapon.
FINGERS = set(range(45, 64)) | set(range(75, 94))
LEG_POSE = leg_pose()
indices = sorted(set(j for arm in ARMS for j in arm[1:]) | FINGERS | set(STANCE) | set(LEG_POSE))
frames = []
for f in range(int(round(DURATION*FPS)) + 1):
    t = f / FPS
    final = list(POSED_G)
    for j, q in LEG_POSE.items(): final[j] = q
    for j, q in arm_pose(t).items(): final[j] = q
    pose = []
    for i in indices:
        if i in FINGERS:
            pose.append(restLocal[i]); continue
        pose.append(qmul(qinv(final[parents[i]]), final[i]))
    frames.append(pose)

lines = ['// Generated by tools/generate-portal-channel.py: 3 s conjuring-circle portal channel,',
         '// authored in the game skeleton\'s own space (+Z forward, +Y up, left arm at -X).',
         '#pragma once', '#include <cstdint>', 'namespace portal::casting::data {',
         f'constexpr uint32_t kFps={FPS};', f'constexpr uint32_t kFrames={len(frames)};',
         f'constexpr float kDuration={DURATION:.9f}f;',
         f'constexpr uint16_t kPelvisJoint={PELVIS};',
         f'constexpr float kPelvisDrop={PELVIS_DROP:.9f}f; // metres the pelvis translation is lowered',
         f'constexpr uint16_t kJoints[]={{{",".join(str(i) for i in indices)}}};',
         'constexpr float kRotations[kFrames][sizeof(kJoints)/sizeof(kJoints[0])][4]={']
for pose in frames:
    lines.append('{' + ','.join('{' + ','.join(f'{v:.9f}f' for v in q) + '}' for q in pose) + '},')
lines += ['};', '}']
(ROOT / 'src/portal_cast_data.h').write_text('\n'.join(lines) + '\n')

# Report where the hands end up (model space) as a quick check.
def fk_hand(g, side_joints):
    side, upper, twistU, lower, twistL, hand = side_joints
    elbow = tuple(POSED_P[upper][k] + rot(g[upper], rot(qinv(G[upper]), sub(P[lower], P[upper])))[k] for k in range(3))
    return [round(elbow[k] + rot(g[lower], rot(qinv(G[lower]), sub(P[hand], P[lower])))[k], 3) for k in range(3)]
g = arm_pose(0.5)
print(json.dumps({'frames': len(frames), 'joints': len(indices),
                  'leftHand': fk_hand(g, ARMS[0]), 'leftTarget': [round(c, 3) for c in hand_target(-1, 0.5)],
                  'rightHand': fk_hand(g, ARMS[1]), 'rightTarget': [round(c, 3) for c in hand_target(1, 0.5)],
                  'feet': [[round(c, 3) for c in solve_joint(POSED_P[h], tg, (0, 0, 1), 0.418, 0.417)[1]] for h, _, _, tg in LEGS],
                  'hips': [[round(c, 3) for c in POSED_P[h]] for h, _, _, _ in LEGS],
                  'shoulders': [[round(c, 3) for c in POSED_P[40]], [round(c, 3) for c in POSED_P[70]]]}))
