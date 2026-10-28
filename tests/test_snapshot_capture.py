"""End-to-end GPU regression: capture differently sized colour/depth, then parallax.

Uses both production fragment shaders, with only Vulkan declarations adapted to
OpenGL. Analytic overlapping planes provide independent silhouette/depth truth.
Run with --report-only to inspect an old shader pair without asserting success.
"""
import argparse
import json
import re
from pathlib import Path

import moderngl
import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument('--capture', type=Path, default=ROOT/'src/shaders/portal_snapshot.frag')
parser.add_argument('--window', type=Path, default=ROOT/'src/shaders/portal_composite.frag')
parser.add_argument('--output', type=Path, default=ROOT/'build/snapshot-capture-regression')
parser.add_argument('--report-only', action='store_true')
args = parser.parse_args()
args.output.mkdir(parents=True, exist_ok=True)
ctx = moderngl.create_standalone_context(require=450)
SW, SH, CW, CH, W, H = 1117, 633, 1024, 640, 640, 480
FX, FY, CX, CY, NEAR = .6, 1.1, .68, .95, .05
source = args.capture.read_text()
source = re.sub(r'layout\(set=0,binding=(\d)\) uniform texture2D (\w+);', r'layout(binding=\1) uniform sampler2D \2;', source)
source = re.sub(r'layout\(set=0,binding=2\) uniform sampler imageSampler;', '', source)
source = re.sub(r'layout\(location=\d+\) flat in ', 'uniform ', source)
source = source.replace('sampler2D(frameColor,imageSampler)', 'frameColor').replace('sampler2D(sceneDepth,imageSampler)', 'sceneDepth')
capture_vertex = '''#version 450
layout(location=2) out vec2 ndc;
void main() {
    ndc=vec2(gl_VertexID==2?3:-1,gl_VertexID==1?3:-1);
    gl_Position=vec4(ndc.x,-ndc.y,0,1);
}'''
capture = ctx.program(vertex_shader=capture_vertex, fragment_shader=source)
capture['rows'].write(np.array([[1/FX,0,0,0],[0,1/FY,0,0],[0,0,0,1],[0,0,1/NEAR,0]], dtype='f4').tobytes())
capture['cameraDepth'].value = (0,0,1,0)
capture['snap'].value = (CX,CY,0,0)
capture['frameColor'].value = 0
capture['sceneDepth'].value = 1
cap_vao = ctx.vertex_array(capture, [])
snapshot = ctx.texture((CW,CH), 4, dtype='f2')
snapshot.filter = (moderngl.LINEAR,moderngl.LINEAR)
snapshot.repeat_x = snapshot.repeat_y = False
cap_fb = ctx.framebuffer([snapshot])

window_source = args.window.read_text()
functions = window_source[window_source.index('bool SnapshotUv('):window_source.index('// The spell from the caster')]
functions = functions.replace('sampler2D(destination,imageSampler)','destination')
window = ctx.program(vertex_shader='''#version 450
out vec2 p;
void main() {p=vec2(gl_VertexID==2?3:-1,gl_VertexID==1?3:-1);gl_Position=vec4(p,0,1);}
''', fragment_shader='''#version 450
uniform sampler2D destination;
uniform vec4 snapRows[3];
uniform vec4 snap;
uniform vec3 testEye;
in vec2 p;
out vec4 color;
'''+functions+'''
void main() {color=Window(testEye,vec3(p*vec2(1.8,1.35),0));}
''')
window['snapRows'].write(np.array([[FX/CX,0,0,0],[0,FY/CY,0,0],[0,0,1,0]],dtype='f4').tobytes())
window['snap'].value = (.5,1,1,0)
window['destination'].value = 0
win_vao = ctx.vertex_array(window, [])
output = ctx.texture((W,H),4,dtype='f4')
win_fb = ctx.framebuffer([output])

BLUE = np.array([.02,.65,1.],dtype='f4')
STONE = np.array([.19,.17,.08],dtype='f4')
BACK = np.array([.08,.17,.08],dtype='f4')
def cross(x,y):
    y=y-.8
    return ((abs(x)<.75)&(abs(y)<.25))|((abs(x)<.25)&(abs(y)<.95))
def grave(x,y):
    return (abs(x)<.29)&(y>-.95)&(y<.55-1.7*abs(x))
def scene(dx,dy,eye=(0,0,0),scale=1.):
    # z=4 blue cross; z=2.2 pointed stone; z=12 background.
    x,y,z=eye
    c=cross((x+dx*(4*scale-z))/scale,(y+dy*(4*scale-z))/scale)
    g=grave((x+dx*(2.2*scale-z))/scale,(y+dy*(2.2*scale-z))/scale)
    ids=np.where(g,1,np.where(c,2,0))
    colors=np.where(g[...,None],STONE,np.where(c[...,None],BLUE,BACK))
    depth=np.where(g,2.2*scale,np.where(c,4*scale,12*scale))
    return colors,depth,ids

sx,sy=np.meshgrid((2*(np.arange(SW)+.5)/SW-1)/FX,(1-2*(np.arange(SH)+.5)/SH)/FY)
px,py=np.meshgrid((2*(np.arange(W)+.5)/W-1)*1.8,(2*(np.arange(H)+.5)/H-1)*1.35)
def save(rgb,path,flip=False):
    rgb=np.clip(rgb[...,:3],0,1)
    if flip:rgb=np.flipud(rgb)
    Image.fromarray(np.uint8(rgb*255)).save(path)

reports=[]
for scale in (1.,3.):
    rgb,z,ids=scene(sx,sy,scale=scale)
    color=ctx.texture((SW,SH),3,rgb.astype('f4').tobytes(),dtype='f4')
    depth=ctx.texture((SW,SH),1,(NEAR/z).astype('f4').tobytes(),dtype='f4')
    for t in (color,depth):
        t.filter=(moderngl.LINEAR,moderngl.LINEAR)
        t.repeat_x=t.repeat_y=False
    color.use(0);depth.use(1)
    cap_fb.use();ctx.viewport=(0,0,CW,CH)
    cap_vao.render(vertices=3)
    taken=np.frombuffer(cap_fb.read(components=4,dtype='f4'),dtype='f4').reshape(CH,CW,4)
    assert np.isfinite(taken).all()
    # A background-depth texel must not contain any of the cross's blue.
    contamination=int(np.count_nonzero((taken[...,3]>8*scale)&(taken[...,2]>.10)))
    save(taken,args.output/f'capture-{scale}.png')
    snapshot.use(0)
    for ex,ey,ez in ((0.,0.,-3.),(.8,0.,-3.),(-.8,.3,-3.),(.2,-.2,-1.5),(1.2,.1,3.)):
        window['testEye'].value=(ex,ey,ez)
        win_fb.use();ctx.viewport=(0,0,W,H)
        with ctx.query(time=True) as timing:
            win_vao.render(vertices=3)
        actual=np.frombuffer(win_fb.read(components=4,dtype='f4'),dtype='f4').reshape(H,W,4)
        assert np.isfinite(actual).all()
        side=-1 if ez>=0 else 1
        eye=np.array([ex*side,ey,ez*side-.5])*.1
        dx,dy=side*(px-ex)/abs(ez),(py-ey)/abs(ez)
        truth,truth_depth,truth_id=scene(dx,dy,eye,scale)
        # Exclude a two-native-pixel silhouette band; assess both faint halos
        # and full-intensity copies, rather than only the latter as before.
        border=np.zeros((H,W),dtype=bool)
        for ox,oy in ((4/SW/FX,0),(-4/SW/FX,0),(0,4/SH/FY),(0,-4/SH/FY)):
            _,_,other=scene(dx+ox,dy+oy,eye,scale)
            border|=other!=truth_id
        shown=actual[...,3]>.9
        halo=(actual[...,2]>.10)&(truth_id!=2)&~border&shown
        missing=(actual[...,2]<.5)&(truth_id==2)&~border
        # A single snapshot cannot know colours behind an occluder. Separate
        # those extrapolated pixels from failures on geometry it DID capture.
        # Project the oracle hit back into the original native depth buffer.
        wx=eye[0]+dx*(truth_depth-eye[2])
        wy=eye[1]+dy*(truth_depth-eye[2])
        source_x=np.clip(((wx/truth_depth*FX*.5+.5)*SW).astype(int),0,SW-1)
        source_y=np.clip(((.5-wy/truth_depth*FY*.5)*SH).astype(int),0,SH-1)
        observed=z[source_y,source_x]>=truth_depth*.999
        # Pointed stone should survive in front of the cross.
        lost_stone=(abs(actual[...,0]-STONE[0])>.05)&(truth_id==1)&~border
        if np.any(halo):
            np.savez_compressed(args.output/f'errors-{scale}-{ex}-{ey}-{ez}.npz',
                                mask=halo,actual=actual,truth=truth,ids=truth_id)
        row=dict(scale=scale,eye=[ex,ey,ez],capture_bleed=contamination,
                 gpu_ms=timing.elapsed/1e6,
                 halo_pixels=int((halo&observed).sum()),lost_cross=int((missing&observed).sum()),
                 unseen_fill_errors=int(((halo|missing)&~observed).sum()),
                 lost_stone=int(lost_stone.sum()),cross_pixels=int((truth_id==2).sum()),
                 stone_pixels=int((truth_id==1).sum()))
        reports.append(row)
        compare=np.concatenate([truth,actual[...,:3],np.stack([halo,missing,lost_stone],axis=-1)],axis=1)
        save(compare,args.output/f'view-{scale}-{ex}-{ey}-{ez}.png',flip=True)
    color.release();depth.release()
(args.output/'results.json').write_text(json.dumps(reports,indent=2)+'\n')
print(json.dumps(reports,indent=2))
if not args.report_only:
    assert all(r['capture_bleed']==0 for r in reports), 'Capture spreads foreground colour into background depth'
    assert all(r['halo_pixels']==0 for r in reports), 'Detached foreground halo'
    assert all(r['lost_cross']<r['cross_pixels']*.02 for r in reports), 'Cross disappeared'
    assert all(r['lost_stone']<r['stone_pixels']*.02 for r in reports), 'Pointed stone distorted'
print('Capture and parallax regression complete')
