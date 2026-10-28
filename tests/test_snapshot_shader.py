"""Render the production snapshot ray tracer against known geometry, outside the game.

Requires numpy, Pillow and moderngl. Only the Vulkan resource declarations are
adapted for OpenGL; SnapshotUv/Window and their helpers are compiled unchanged.
The oracle intersects actual foreground/background planes, not the depth march.
"""
import argparse
import json
from pathlib import Path

import moderngl
import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument('--shader', type=Path, default=ROOT / 'src/shaders/portal_composite.frag')
parser.add_argument('--output', type=Path, default=ROOT / 'build/snapshot-regression')
parser.add_argument('--report-only', action='store_true')
args = parser.parse_args()
args.output.mkdir(parents=True, exist_ok=True)

ctx = moderngl.create_standalone_context(require=450)
source = args.shader.read_text()
functions = source[source.index('bool SnapshotUv('):source.index('// The spell from the caster')]
functions = functions.replace('sampler2D(destination,imageSampler)', 'destination')
vertex = '''#version 450
out vec2 p;
void main() {
    p = vec2((gl_VertexID == 2) ? 3 : -1, (gl_VertexID == 1) ? 3 : -1);
    gl_Position = vec4(p, 0, 1);
}'''
fragment = '''#version 450
uniform sampler2D destination;
uniform vec4 snapRows[3];
uniform vec4 snap;
uniform vec3 testEye;
in vec2 p;
out vec4 color;
''' + functions + '''
void main() { color = Window(testEye, vec3(p * vec2(1.8, 1.35), 0)); }
'''
program = ctx.program(vertex_shader=vertex, fragment_shader=fragment)
program['snapRows'].write(np.array([[1,0,0,0], [0,1,0,0], [0,0,1,0]], dtype='f4').tobytes())
program['snap'].value = (.5, 1, 1, 0)
program['destination'].value = 0
vao = ctx.vertex_array(program, [])
W, H = 640, 480
SW, SH = 1024, 1024
target = ctx.texture((W, H), 4, dtype='f4')
fb = ctx.framebuffer([target])
fb.use()
ctx.viewport = (0, 0, W, H)

def cross(x, y):
    return ((np.abs(x) < .65) & (np.abs(y) < .23)) | ((np.abs(x) < .23) & (np.abs(y) < .95))

def panel(pixels):
    rgb = np.clip(pixels[..., :3], 0, 1)
    return Image.fromarray(np.uint8(np.flipud(rgb) * 255))

sx, sy = np.meshgrid(2*(np.arange(SW)+.5)/SW-1, 1-2*(np.arange(SH)+.5)/SH)
px, py = np.meshgrid((2*(np.arange(W)+.5)/W-1)*1.8, (2*(np.arange(H)+.5)/H-1)*1.35)
summary = []
for background, slope in ((b,s) for b in (30.,60000.) for s in ((0.,0.),(.7,.3),(-.6,.5))):
    data = np.zeros((SH, SW, 4), dtype='f4')
    data[...] = (.24, .08, .04, background)
    plane_depth = 3. / (1.-slope[0]*sx-slope[1]*sy)
    object_mask = (plane_depth > 0.) & cross(sx*plane_depth, sy*plane_depth)
    data[object_mask] = (.02, .65, 1., 3.)
    data[...,3][object_mask] = plane_depth[object_mask]
    # Match the game's half-float snapshot, including depth quantization.
    tex = ctx.texture((SW, SH), 4, data.astype('f2').tobytes(), dtype='f2')
    tex.filter = (moderngl.LINEAR, moderngl.LINEAR)
    tex.repeat_x = tex.repeat_y = False
    tex.use(0)
    for ex, ey, ez in ((0.,0.,-3.),(1.5,0.,-3.),(-1.5,.5,-3.),(.25,-.2,-3.),(0.,0.,-1.5),(1.5,0.,3.)):
        eye = np.array([ex, ey, ez], dtype='f4')
        program['testEye'].value = tuple(eye)
        vao.render(vertices=3)
        result = np.frombuffer(fb.read(components=4, dtype='f4'), dtype='f4').reshape(H, W, 4)
        assert np.isfinite(result).all(), 'Non-finite snapshot pixels'
        # The production window deliberately retains only 10% of eye translation.
        side = -1 if ez >= 0 else 1
        E = (eye * [side,1,side] - [0, 0, .5]) * .1
        dx, dy = side*(px-ex)/abs(ez), (py-ey)/abs(ez)
        # D.z comes from point.z-eye.z (the window-start shift cancels).
        distance = (3.+slope[0]*E[0]+slope[1]*E[1]-E[2]) / (1.-slope[0]*dx-slope[1]*dy)
        fx, fy = E[0]+dx*distance, E[1]+dy*distance
        expected = (distance > 0) & cross(fx, fy)
        # Exclude only a two-source-pixel band around the true silhouette.
        edge = np.zeros_like(expected)
        for ox, oy in ((.014,0),(-.014,0),(0,.014),(0,-.014)):
            edge |= cross(fx+ox, fy+oy) != expected
        valid = ~edge
        shown = result[...,3] > .9
        blue = result[...,2] > .5
        ghosts = int(np.count_nonzero(blue & shown & ~expected & valid))
        missing = int(np.count_nonzero(~blue & expected & valid))
        # Unknown newly exposed background may be mist, but the visible foreground
        # must remain and it must never be pasted onto the background a second time.
        row = dict(background=background, slope=slope, eye=[ex,ey,ez], ghost_pixels=ghosts,
                   lost_foreground=missing, foreground_pixels=int(expected.sum()),
                   mist_pixels=int(np.count_nonzero(~shown)))
        summary.append(row)
        oracle = np.zeros_like(result)
        oracle[...] = (.24, .08, .04, 1.)
        oracle[expected] = (.02, .65, 1., 1.)
        error = np.zeros_like(result)
        error[blue & shown & ~expected & valid] = (1, 0, 0, 1)
        error[~blue & expected & valid] = (1, 1, 0, 1)
        error[~shown] = (.25, .25, .25, 1)
        comparison = Image.new('RGB', (W*3,H))
        for i, pixels in enumerate((oracle,result,error)):
            comparison.paste(panel(pixels), (i*W,0))
        comparison.save(args.output / f'cross-{int(background)}-{slope[0]}-{slope[1]}-{ex}-{ey}-{ez}.png')
    tex.release()
report = dict(renderer=ctx.info['GL_RENDERER'], cases=summary)
(args.output/'results.json').write_text(json.dumps(report, indent=2)+'\n')
print(json.dumps(report, indent=2))
if not args.report_only:
    assert all(r['ghost_pixels'] == 0 for r in summary), 'Foreground duplicated into revealed background'
    assert all(r['lost_foreground'] < r['foreground_pixels'] * .02 for r in summary), 'Foreground erased'
print('Snapshot shader regression complete')
