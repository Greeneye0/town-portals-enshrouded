#version 450
layout(set=0,binding=0) uniform texture2D destination;
layout(set=0,binding=1) uniform texture2D sceneDepth;
layout(set=0,binding=2) uniform sampler imageSampler;
layout(location=0) in vec4 plane;
layout(location=1) flat in vec2 options;
layout(location=2) in vec2 ndc;
layout(location=3) flat in vec4 rows[4];
// The owner's colour (their minimap icon, made vivid: brightest channel colors::kValue).
layout(location=7) flat in vec3 tint;
// The frame's anti-aliasing jitter in texture coordinates: where this pixel's
// view ray landed in the (jittered) depth buffer.
layout(location=8) flat in vec2 depthShift;
// 1 when drawn into the game's HDR scene colour at render resolution, before its
// anti-aliasing and upscaling; 0 when drawn over the finished frame.
layout(location=9) flat in vec4 inFrame;
// The other side's snapshot (bound as destination): rows x, y, w from window space to its
// NDC; snap x the camera's distance in front of the exit, y the exposure scale, z 1 when bound,
// w 1 when it was taken in the Shroud.
layout(location=10) flat in vec4 snapRows[3];
layout(location=13) flat in vec4 snap;
// The cast effect: the caster's hand in portal metres (xyz), effect 1-3 (w; 0 none).
layout(location=14) flat in vec4 castInfo;
layout(location=0) out vec4 color;
// The lighter portal for low graphics settings (options.x +100, portal_quality.h): the picture
// traced in fewer depth steps, the steam marched in fewer, fewer embers.
bool gLight=false;

// Portal-space metres per unit of the CPU basis (horizontal, vertical, normal).
const vec3 kMetres=vec3(1.02*1.16,1.42*1.16,1.0);
// Full-size lens: rim radius 0.96 in the shader's portal units, 0.16 m thick each side.
const float kRim=0.96, kThickness=0.16;

// How much of a pixel at this reverse-Z depth is in front of the scene, 0..1:
// the four nearest depth texels compared and blended bilinearly. A single-texel
// test stair-steps along outlines; a wider filter leaves a see-through band
// round characters. depthUv already has the frame's jitter applied, so the
// outline stays put while the game's camera jitters.
float Visible(float d,vec2 depthUv) {
    ivec2 size=textureSize(sampler2D(sceneDepth,imageSampler),0);
    vec2 pos=depthUv*vec2(size)-0.5;
    ivec2 base=ivec2(floor(pos));
    vec2 f=fract(pos);
    float v[4];
    for(int i=0;i<4;++i) {
        float scene=texelFetch(sampler2D(sceneDepth,imageSampler),clamp(base+ivec2(i&1,i>>1),ivec2(0),size-1),0).r;
        v[i]=step(scene,d+0.000001);
    }
    return mix(mix(v[0],v[1],f.x),mix(v[2],v[3],f.x),f.y);
}

// Where a point in window space (metres: the exit's right, up and normal, from snap.x in
// front of it) lands in the snapshot, and its depth along the snapshot camera (w).
bool SnapshotUv(vec3 s,out vec2 uv,out float w) {
    vec4 h=vec4(s,1.0);
    vec3 clip=vec3(dot(snapRows[0],h),dot(snapRows[1],h),dot(snapRows[2],h));
    uv=vec2(0);w=clip.z;
    if(clip.z<=0.05) return false;
    vec2 n=clip.xy/clip.z;
    if(abs(n.x)>1.0||abs(n.y)>1.0) return false;
    uv=vec2(0.5+0.5*n.x,0.5-0.5*n.y);
    return true;
}
float SnapshotDepth(vec2 uv) {
    ivec2 size=textureSize(sampler2D(destination,imageSampler),0);
    return texelFetch(sampler2D(destination,imageSampler),clamp(ivec2(uv*vec2(size)),ivec2(0),size-1),0).a;
}
// Filter colour only on the surface whose depth we hit. Ordinary bilinear
// colour blends foreground and background at an edge, while nearest depth
// belongs to just one of them; reprojection pulls that fringe off the object.
vec3 SnapshotColor(vec2 uv,float depth) {
    ivec2 size=textureSize(sampler2D(destination,imageSampler),0);
    vec2 pos=uv*vec2(size)-0.5, f=fract(pos);
    ivec2 base=ivec2(floor(pos));
    vec3 rgb=vec3(0);float weight=0.0;
    float tolerance=max(0.025,depth*0.01);
    for(int i=0;i<4;++i) {
        vec4 tap=texelFetch(sampler2D(destination,imageSampler),clamp(base+ivec2(i&1,i>>1),ivec2(0),size-1),0);
        float w=((i&1)==0?1.0-f.x:f.x)*(i<2?1.0-f.y:f.y);
        w*=1.0-smoothstep(tolerance,2.0*tolerance,abs(tap.a-depth));
        rgb+=tap.rgb*w;weight+=w;
    }
    if(weight>1e-5)return rgb/weight;
    return texelFetch(sampler2D(destination,imageSampler),clamp(ivec2(uv*vec2(size)),ivec2(0),size-1),0).rgb;
}
// The view through the portal: the viewer's ray, carried to the other side the way the
// portal links them (turned about up), traced through the snapshot's depth until it passes
// behind a surface. Moving the head shifts near things more than far ones, like a window.
// eye and point are in portal space (metres); returns colour and how much of it to show.
vec4 Window(vec3 eye,vec3 point) {
    // Viewed from behind, the same view turned round.
    float side=eye.z>=0.0?-1.0:1.0;
    vec3 eyeOut=vec3(side*eye.x,eye.y,side*eye.z-snap.x);
    vec3 D=vec3(side*point.x,point.y,side*point.z-snap.x)-eyeOut;
    if(D.z<1e-4) return vec4(0);
    D/=D.z;
    // Where the snapshot camera stood, in window space: the point with clip x, y, w all zero.
    mat3 M=transpose(mat3(snapRows[0].xyz,snapRows[1].xyz,snapRows[2].xyz));
    vec3 camera=-inverse(M)*vec3(snapRows[0].w,snapRows[1].w,snapRows[2].w);
    // Parallax: rays start from the viewer's eye carried through the portal, pulled most of the
    // way back to the snapshot camera. Near things still shift as the head moves, but rays seldom
    // pass behind what the camera could see (which smears the nearer surface along them).
    const float kParallax=0.1;
    vec3 E=mix(camera,eyeOut,kParallax);
    // Each sample proposes a camera-depth plane. Intersect the ray with that
    // plane, then check its actual pixel; sampling only when already behind
    // depth skips narrow tips between steps and mistakes silhouette jumps for
    // continuous surfaces. Keep the nearest validated candidate, not the first
    // background plane encountered in the photograph.
    const int kSteps=gLight?14:48;
    const float kNear=0.3, kFar=400.0;
    float ratio=pow(kFar/kNear,1.0/float(kSteps));
    float distance=kNear, nearest=kFar+1.0;
    float ew=dot(snapRows[2],vec4(E,1.0));
    float dw=dot(snapRows[2].xyz,D);
    if(dw<=1e-6)return vec4(0);
    vec2 uv=vec2(0);
    bool inside=false,hit=false;
    for(int i=0;i<=kSteps;++i) {
        vec2 u;float w;
        if(SnapshotUv(E+D*distance,u,w)) {
            inside=true;
            float surface=SnapshotDepth(u);
            for(int k=0;k<3;++k) {
                float candidate=(surface-ew)/dw;
                if(candidate<kNear||candidate>kFar||candidate>=nearest)break;
                vec2 at;float atW;
                if(!SnapshotUv(E+D*candidate,at,atW))break;
                float actual=SnapshotDepth(at);
                float difference=abs(actual-surface);
                if(difference<=max(0.015,surface*0.003)) {
                    uv=at;nearest=candidate;hit=true;break;
                }
                // Only refine along the same sloping surface. A different
                // depth layer is another candidate, not a wall to interpolate.
                if(difference>max(0.05,surface*0.05))break;
                surface=actual;
            }
        } else if(inside) break;                  // left the snapshot's frame
        if(distance>=nearest)break;
        distance*=ratio;
    }
    vec2 texel=vec2(0.5)/vec2(textureSize(sampler2D(destination,imageSampler),0));
    if(hit) return vec4(SnapshotColor(clamp(uv,texel,1.0-texel),SnapshotDepth(uv))*snap.y,1.0);
    // No surface: where the ray heads (far away). Inside the picture that is sky or distant
    // land. Outside it (ground just past the exit, which the snapshot camera stood beyond, or
    // above or beside its frame) the nearest stretch of the picture's edge, blurred along the
    // edge and fading into the portal's mist the further out the ray points.
    vec3 far=E+D*kFar;vec4 h=vec4(far,1.0);
    vec3 clip=vec3(dot(snapRows[0],h),dot(snapRows[1],h),dot(snapRows[2],h));
    if(clip.z<=1e-5) return vec4(0);
    vec2 n=clip.xy/clip.z;
    float outside=max(abs(n.x),abs(n.y))-1.0;
    if(outside<=0.0) {
        vec2 distant=vec2(0.5+0.5*n.x,0.5-0.5*n.y);
        float blocker=SnapshotDepth(distant);
        if(blocker>=clip.z) return vec4(SnapshotColor(distant,blocker)*snap.y,1.0);
        // This part of the background was hidden by a closer object in the
        // snapshot. Pasting its unshifted colour here draws the object twice.
        // Extend nearby BACKGROUND along the reprojection direction instead;
        // if no deeper sample exists, leave the hole to the portal's mist.
        float ew=dot(snapRows[2],vec4(E,1.0));
        float dw=dot(snapRows[2].xyz,D);
        vec2 at;float atW;
        if(dw>1e-6&&SnapshotUv(E+D*((blocker-ew)/dw),at,atW)) {
            vec2 delta=at-distant;
            for(int i=0;i<8;++i) {
                vec2 candidate=distant+delta*(1.0+float(i)*0.5);
                if(any(lessThan(candidate,texel))||any(greaterThan(candidate,1.0-texel)))break;
                float behind=SnapshotDepth(candidate);
                if(behind>blocker+max(0.1,blocker*0.05)) {
                    return vec4(SnapshotColor(candidate,behind)*snap.y,1.0);
                }
            }
        }
        return vec4(0);
    }
    vec2 edge=clamp(n,-1.0,1.0);
    // A wide soft blur along the edge it left by (so a bright seam there does not become a
    // streak), from a little inside the picture, dimming the further out the ray points.
    vec2 along=abs(n.x)-1.0>abs(n.y)-1.0?vec2(0.0,1.0):vec2(1.0,0.0);
    vec2 inward=-sign(n)*(vec2(1.0)-along);
    float spread=0.3+0.7*min(outside,1.0);
    vec3 rgb=vec3(0);
    for(int k=-6;k<=6;++k) {
        for(int j=0;j<2;++j) {
            vec2 e=clamp(edge+along*spread*float(k)/6.0+inward*(0.02+0.06*float(j)),-1.0,1.0);
            rgb+=textureLod(sampler2D(destination,imageSampler),clamp(vec2(0.5+0.5*e.x,0.5-0.5*e.y),texel,1.0-texel),0.0).rgb;
        }
    }
    rgb/=26.0;
    return vec4(rgb*snap.y*(1.0-0.5*smoothstep(0.0,1.0,outside)),0.85*(1.0-smoothstep(0.0,0.6,outside)));
}

// The spell from the caster's hand to the forming portal, drawn in the game's frame (hidden
// behind whatever stands in front of it). Ray: eye + ray*t in portal metres; sceneT is how far
// along it the scene surface is. form 0..1 over the channel, time in seconds, rim the ring's
// radius in portal units. Three looks to choose from: 1 ribbon, 2 motes, 3 arcs.
float Hash(float x) { return fract(sin(x*12.9898)*43758.5453); }
// Closest approach of the ray to a segment: distance, and t along the ray.
float RaySegment(vec3 O,vec3 V,vec3 P0,vec3 P1,out float t) {
    vec3 S=P1-P0, w=O-P0;
    float b=dot(V,S), c=dot(S,S), d=dot(V,w), e=dot(S,w);
    float den=c-b*b;
    float u=den>1e-9?clamp((e-b*d)/den,0.0,1.0):0.0;
    t=max(b*u-d,0.0);
    return length(O+V*t-(P0+S*u));
}
float RayPoint(vec3 O,vec3 V,vec3 P,out float t) {
    t=max(dot(P-O,V),0.0);
    return length(O+V*t-P);
}
vec3 Bezier(vec3 a,vec3 m,vec3 b,float s) { return mix(mix(a,m,s),mix(m,b,s),s); }
vec3 RimPoint(float angle,float rim) { return vec3(cos(angle)*kMetres.x,sin(angle)*kMetres.y,0.0)*rim; }

vec3 CastEffect(vec3 eye,vec3 ray,float sceneT,float form,float time,float rim) {
    vec3 H=castInfo.xyz, C=vec3(0.0);
    vec3 axis=normalize(C-H+1e-6);
    vec3 n1=normalize(cross(axis,vec3(0.0,1.0,0.0))+vec3(1e-5));
    vec3 n2=cross(axis,n1);
    vec3 white=vec3(1.0,0.95,0.85);
    float strength=smoothstep(0.0,0.08,form)*(1.0-smoothstep(0.93,1.0,form));
    float core=0.0, glow=0.0, tt;
    #define SEEN(t_) (1.0-smoothstep(sceneT-0.05,sceneT+0.05,(t_)))
    // A gathering orb at the hand, whatever the effect.
    float d=RayPoint(eye,ray,H,tt);
    float flick=0.85+0.15*sin(time*31.0);
    core+=exp(-pow(d/0.025,2.0))*1.2*flick*SEEN(tt);
    glow+=exp(-pow(d/0.09,2.0))*0.8*flick*SEEN(tt);
    int effect=int(castInfo.w+0.5);
    if(effect==1) {
        // Ribbon: two strands twisting round an arc from the hand into the portal's heart,
        // pulses of light running along them toward the portal.
        vec3 M=(H+C)*0.5+vec3(0.0,0.45,0.0);
        for(int k=0;k<2;++k) {
            vec3 prev=H;
            for(int i=1;i<=16;++i) {
                float s=float(i)/16.0;
                float r=0.14*sin(3.14159*s);
                float a=s*14.0-time*7.0+float(k)*3.14159;
                vec3 cur=Bezier(H,M,C,s)+(n1*cos(a)+n2*sin(a))*r;
                d=RaySegment(eye,ray,prev,cur,tt);
                float pulse=0.45+0.55*pow(0.5+0.5*sin(s*24.0-time*16.0),3.0);
                core+=exp(-pow(d/0.011,2.0))*pulse*SEEN(tt);
                glow+=exp(-pow(d/0.055,2.0))*0.35*pulse*SEEN(tt);
                prev=cur;
            }
        }
    } else if(effect==2) {
        // Mist: thin translucent wisps spiralling round the path from the hand into the
        // portal, widening into a funnel as they reach it (like the game's own teleport mist,
        // lying along the path), streaky rather than solid.
        {
            vec3 Mm=(H+C)*0.5+vec3(0.0,0.3,0.0);
            for(int k=0;k<5;++k) {
                float fk=float(k);
                vec3 prev=H;
                for(int i=1;i<=24;++i) {
                    float s=float(i)/24.0;
                    float r=0.05+0.45*s*s*(0.8+0.4*Hash(fk+3.0));
                    float a=s*(11.0+2.0*Hash(fk))-time*(2.6+0.8*Hash(fk+7.0))+fk*1.2566;
                    vec3 cur=Bezier(H,Mm,C,s)+(n1*cos(a)+n2*sin(a))*r;
                    d=RaySegment(eye,ray,prev,cur,tt);
                    // Dashes of brightness drifting toward the portal; each wisp fades in from
                    // the hand and out into the portal.
                    float streak=0.5+0.5*sin(s*37.0-time*8.0+fk*4.0)*sin(s*15.0+time*2.1+fk*1.7);
                    float along=smoothstep(0.0,0.15,s)*(1.0-smoothstep(0.85,1.0,s));
                    float width=0.012+0.018*s;
                    glow+=exp(-pow(d/width,2.0))*0.45*pow(streak,2.0)*along*SEEN(tt);
                    glow+=exp(-pow(d/(width*3.0),2.0))*0.04*along*SEEN(tt);
                    prev=cur;
                }
            }
        }
        // Motes: sparks streaming along curved paths from the hand to points round the
        // growing rim, spiralling as they go, each with a short comet tail.
        for(int i=0;i<22;++i) {
            float fi=float(i);
            float ph=fract(time*0.55+fi*0.618);
            float a=fi*2.39996+time*0.4;
            vec3 T=RimPoint(a,rim);
            vec3 M=(H+T)*0.5+vec3(0.0,0.45,0.0)+n1*(Hash(fi)-0.5)*0.6;
            float spin=fi*1.7+time*4.0;
            vec3 wob=(n1*cos(spin)+n2*sin(spin))*0.09;
            vec3 pos=Bezier(H,M,T,ph)+wob*sin(3.14159*ph);
            float back=max(ph-0.025,0.0);
            vec3 tail=Bezier(H,M,T,back)+wob*sin(3.14159*back);
            float fade=sin(3.14159*ph);
            float size=0.016*(0.7+0.6*Hash(fi+3.0));
            d=RaySegment(eye,ray,tail,pos,tt);
            core+=exp(-pow(d/size,2.0))*0.7*fade*SEEN(tt);
            glow+=exp(-pow(d/(size*4.0),2.0))*0.15*fade*SEEN(tt);
        }
    } else if(effect==3) {
        // Arcs: jagged lightning from the hand to the rim, jumping to new points.
        for(int k=0;k<3;++k) {
            float seed=floor(time*12.0)+float(k)*17.0;
            vec3 T=RimPoint(Hash(seed)*6.2832,rim);
            vec3 prev=H;
            float flash=0.55+0.45*Hash(seed+99.0);
            for(int i=1;i<=10;++i) {
                float s=float(i)/10.0;
                vec3 base=mix(H,T,s)+vec3(0.0,0.25*sin(3.14159*s),0.0);
                vec3 off=(n1*(Hash(seed+float(i)*1.3)-0.5)+n2*(Hash(seed+float(i)*2.7)-0.5))*0.35*sin(3.14159*s);
                vec3 cur=i==10?T:base+off;
                d=RaySegment(eye,ray,prev,cur,tt);
                core+=exp(-pow(d/0.008,2.0))*1.4*flash*SEEN(tt);
                glow+=exp(-pow(d/0.05,2.0))*0.4*flash*SEEN(tt);
                prev=cur;
            }
        }
    }
    #undef SEEN
    return (white*min(core,4.0)*2.0+tint*min(glow,2.0)*1.6)*strength;
}

// Smooth value noise. The lattice hash keeps its precision at large coordinates (a sin()
// hash went blocky as the drift grew), and each octave is sampled in a rotated frame so no
// grid lines up with the portal.
float Hash3(vec3 p) {
    p=fract(p*0.1031);
    p+=dot(p,p.zyx+31.32);
    return fract((p.x+p.y)*p.z);
}
float Noise3(vec3 x) {
    vec3 i=floor(x), f=fract(x);
    f=f*f*f*(f*(f*6.0-15.0)+10.0);
    return mix(mix(mix(Hash3(i),Hash3(i+vec3(1,0,0)),f.x),mix(Hash3(i+vec3(0,1,0)),Hash3(i+vec3(1,1,0)),f.x),f.y),
               mix(mix(Hash3(i+vec3(0,0,1)),Hash3(i+vec3(1,0,1)),f.x),mix(Hash3(i+vec3(0,1,1)),Hash3(i+vec3(1,1,1)),f.x),f.y),f.z);
}
const mat3 kTurn=mat3(0.80,0.36,-0.48, -0.60,0.48,-0.64, 0.0,0.80,0.60);
float Fbm(vec3 p) { return 0.65*Noise3(kTurn*p)+0.35*Noise3(kTurn*(kTurn*p)*2.03+vec3(7.1,3.3,1.7)); }

// An open portal's aura: steam rising off its rim, heaviest off the top and drifting up
// above it, with embers floating up from the rim, in its owner's colour (like the vapour over
// a vent). Ray as for CastEffect; strength 0..1 (it grows with the portal and shrinks as it
// closes).
vec3 PortalAura(vec3 eye,vec3 ray,float sceneT,float time,float strength) {
    vec3 white=vec3(1.0,0.95,0.85);
    const float rx=kMetres.x*kRim, ry=kMetres.y*kRim;   // the rim's half-width and half-height (m)
    // The steam: a slab round the portal, marched in a few steps where the ray crosses it.
    const vec3 lo=vec3(-2.0,-1.7,-0.4), hi=vec3(2.0,2.8,0.4);
    vec3 inv=1.0/vec3(abs(ray.x)>1e-6?ray.x:1e-6,abs(ray.y)>1e-6?ray.y:1e-6,abs(ray.z)>1e-6?ray.z:1e-6);
    vec3 ta=(lo-eye)*inv, tb=(hi-eye)*inv;
    vec3 tmin=min(ta,tb), tmax=max(ta,tb);
    float enter=max(max(tmin.x,tmin.y),max(tmin.z,0.0)), leave=min(min(tmax.x,tmax.y),min(tmax.z,sceneT));
    float steam=0.0;
    if(leave>enter) {
        const int kMarch=gLight?4:12;
        float step=(leave-enter)/float(kMarch);
        for(int i=0;i<kMarch;++i) {
            vec3 q=eye+ray*(enter+step*(float(i)+0.5));
            // Separate plumes, each rising from a point on the rim (mostly its upper part)
            // with its own height, width and sway; each fades in, wafts for a while, fades
            // out and starts again somewhere else.
            float plumes=0.0;
            for(int k=0;k<5;++k) {
                float fk=float(k);
                float cycle=time*0.09+Hash(fk*3.1);
                float life=sin(3.14159*fract(cycle));
                float seed=fk*7.3+floor(cycle)*1.37;
                float a=mix(-0.6,3.74,Hash(seed));                   // round the upper rim
                vec3 o=vec3(cos(a)*rx,sin(a)*ry,0.0);
                float h=q.y-o.y;
                float len=0.7+0.8*Hash(seed+1.0);
                if(h<0.0||h>len) continue;
                float sway=sin(h*1.7+time*0.6+fk*2.0)*0.2*h+sin(time*0.37+fk)*0.08*h;
                float lateral=length(vec2(q.x-o.x-sway,q.z));
                float width=0.06+0.22*h/len;
                float body=exp(-pow(lateral/width,2.0))*pow(1.0-h/len,1.5)*smoothstep(0.0,0.12,h);
                float texture=Fbm(q*vec3(2.6,1.6,2.6)-vec3(0.0,time*0.6,fk*3.0));
                plumes+=body*smoothstep(0.3,0.75,texture)*life;
            }
            float thin=1.0-smoothstep(0.05,0.33,abs(q.z));
            // Gone well before the edges of the slab, so it never ends in a straight line.
            float inside=(1.0-smoothstep(1.4,1.85,abs(q.x)))*(1.0-smoothstep(2.3,2.7,q.y));
            steam+=plumes*thin*inside*step;
        }
    }
    // Embers floating up from points round the rim, drifting above the portal.
    float embers=0.0, tt;
    for(int i=0;i<(gLight?6:14);++i) {
        float fi=float(i);
        float ph=fract(time*0.18+Hash(fi));
        float a=Hash(fi+1.0)*6.2832;
        vec3 e=vec3(cos(a)*rx+sin(time*0.7+fi)*0.08,sin(a)*ry+ph*1.1,(Hash(fi+2.0)-0.5)*0.3);
        float d=RayPoint(eye,ray,e,tt);
        float seen=1.0-smoothstep(sceneT-0.05,sceneT+0.05,tt);
        embers+=exp(-pow(d/0.011,2.0))*sin(3.14159*ph)*seen;
    }
    return (tint*steam*2.2+white*min(embers,2.0))*strength;
}

void main() {
    // options.x: formation 0..1, +10 when the live destination image is bound,
    // +20 for every endpoint after the first. Each endpoint is a full-screen
    // triangle into the same image: the first replaces what it does not cover,
    // later ones blend and discard there so they do not erase the earlier portal.
    // +40: in the game's frame, the portal's light alone, drawn as a multiply of the
    // scene's colour (scene * (1 + light)) before the surface. +100: the lighter portal.
    float encoded=options.x;
    gLight=encoded>99.5; if(gLight) encoded-=100.0;
    bool lightOnly=encoded>39.5; if(lightOnly) encoded-=40.0;
    bool overlay=encoded>19.5; if(overlay) encoded-=20.0;
    bool live=encoded>5.0;
    float form=clamp(encoded-(live?10.0:0.0),0.0,1.0), t=options.y;
    bool hdr=inFrame.x>0.5;
    vec2 screen=gl_FragCoord.xy/(hdr?vec2(textureSize(sampler2D(sceneDepth,imageSampler),0)):vec2(2560,1440));
    vec2 depthUv=screen+depthShift;
    ivec2 size=textureSize(sampler2D(sceneDepth,imageSampler),0);
    float depth=texelFetch(sampler2D(sceneDepth,imageSampler),clamp(ivec2(depthUv*size),ivec2(0),size-1),0).r;
    // Surface: grows out of the spark after the first 15% of the channel.
    float grow=smoothstep(0.15,1.0,form);
    float s=max(grow,0.001);

    // This pixel's view ray in portal space: point(d) = (A + d*B).xyz / (A + d*B).w
    // for native reverse-Z depth d (1 near, 0 far).
    vec4 A, B=vec4(rows[0].z,rows[1].z,rows[2].z,rows[3].z);
    for(int r=0;r<4;++r) A[r]=rows[r].x*ndc.x+rows[r].y*ndc.y+rows[r].w;
    #define AT(d) (((A.xyz+(d)*B.xyz)/(A.w+(d)*B.w))*kMetres)
    #define AT_AT(o,d) ((((A.xyz+vec3(rows[0].x,rows[1].x,rows[2].x)*(o).x+vec3(rows[0].y,rows[1].y,rows[2].y)*(o).y)+(d)*B.xyz)/((A.w+rows[3].x*(o).x+rows[3].y*(o).y)+(d)*B.w))*kMetres)

    // Ambient light: the owner's colour out of the two faces, fading about 7 m
    // away; little reaches sideways past the rim.
    vec3 light=vec3(0);
    bool solid=depth>0.0 && abs(A.w+depth*B.w)>1e-7;
    vec3 p=solid?AT(depth):vec3(0);
    if(solid) {
        // The surface's facing, from depth two texels away on each side, taking the side
        // closer in depth so outlines do not mix a character with what is behind it.
        vec2 texel=1.0/vec2(size);
        vec2 du=vec2(2.0*texel.x,0.0), dv=vec2(0.0,2.0*texel.y);
        #define DEPTH_AT(uv) texelFetch(sampler2D(sceneDepth,imageSampler),clamp(ivec2((uv)*size),ivec2(0),size-1),0).r
        vec3 xp=AT_AT(vec2(2.0*du.x,0.0),DEPTH_AT(depthUv+du)), xm=AT_AT(vec2(-2.0*du.x,0.0),DEPTH_AT(depthUv-du));
        vec3 yp=AT_AT(vec2(0.0,-2.0*dv.y),DEPTH_AT(depthUv+dv)), ym=AT_AT(vec2(0.0,2.0*dv.y),DEPTH_AT(depthUv-dv));
        vec3 ex=dot(xp-p,xp-p)<dot(p-xm,p-xm)?xp-p:p-xm;
        vec3 ey=dot(yp-p,yp-p)<dot(p-ym,p-ym)?yp-p:p-ym;
        vec3 normal=cross(ex,ey);
        float d2=dot(p,p);
        float facing=mix(0.12,1.0,smoothstep(0.05,0.6,abs(p.z)/sqrt(d2+1e-4)));
        // Only surfaces turned toward the portal catch its light: not the back of a
        // character standing in front of it.
        float lambert=1.0, groundFacing=0.0;
        if(dot(normal,normal)>1e-12) {
            vec3 n=normalize(normal);
            vec3 toCamera=AT(min(depth+0.001,1.0))-p;
            if(dot(n,toCamera)<0.0) n=-n;
            lambert=smoothstep(0.0,0.35,dot(n,-p/sqrt(d2+1e-4)));
            // Grass blades are mostly vertical: an upward-normal gate removes
            // nearly all of them. Light them from a low point at the portal's
            // foot instead, with a small diffuse floor for crossed foliage.
            vec3 groundToLight=vec3(-p.x,-1.02-p.y,-p.z);
            groundFacing=0.18+0.82*smoothstep(0.0,0.65,
                dot(n,normalize(groundToLight+vec3(0.0,1e-5,0.0))));
        }
        light=tint*0.62*(0.5/(1.0+d2*0.45))*(1.0-smoothstep(16.0,49.0,d2))*facing*lambert*grow*(0.92+0.08*sin(t*2.3));
        // A broad, soft pool around the foot of the portal. Portal origin is
        // 1.62 m above its ground point. Reconstructing actual scene surfaces
        // keeps the light on floor/grass instead of drawing a flat glowing disk.
        // The height gate keeps this contribution off upper bodies. Low walls
        // and grass facing the source catch it, as they do near a small lamp.
        float groundRadius=length(p.xz);
        float groundPool=exp(-groundRadius*groundRadius/2.8)*
            (1.0-smoothstep(2.3,3.2,groundRadius));
        float groundHeight=1.0-smoothstep(0.45,1.25,abs(p.y+1.62));
        // Multiplication preserves the ground's texture, but the old gain was
        // too weak on moonlit grass. The light-only draw supplies its tunable
        // gain in snap.y; the surface draw keeps normal snapshot exposure there.
        float groundGain=lightOnly?snap.y:1.25;
        light+=tint*groundGain*groundPool*groundHeight*groundFacing*grow;
    }
    if(lightOnly) {
        light*=inFrame.z;
        if(dot(light,light)<1e-8) discard;
        color=vec4(light,0);
        return;
    }
    // In the game's frame the light has its own multiplying draw.
    if(hdr) light=vec3(0);
    // Light alone is premultiplied with zero alpha, so it adds to the scene.
    #define REJECT {if(overlay&&dot(light,light)<1e-8) discard; color=vec4(light,0); return;}

    // Lens: an ellipsoid with the portal's outline and a little thickness, so it
    // keeps a shape from the side instead of collapsing to a glowing line.
    vec3 axes=vec3(kMetres.xy*kRim,kThickness)*s;
    vec3 a=A.xyz*kMetres/axes, b=B.xyz*kMetres/axes;
    float qa=dot(b,b)-B.w*B.w, qb=2.0*(dot(a,b)-A.w*B.w), qc=dot(a,a)-A.w*A.w;
    float hit=-1.0;
    if(abs(qa)>1e-12) {
        float disc=qb*qb-4.0*qa*qc;
        if(disc>=0.0) {
            float root=sqrt(disc), d0=(-qb-root)/(2.0*qa), d1=(-qb+root)/(2.0*qa);
            // The nearer surface has the larger reverse-Z depth.
            if(d0>0.0&&d0<=1.0&&A.w+d0*B.w>0.0)hit=max(hit,d0);
            if(d1>0.0&&d1<=1.0&&A.w+d1*B.w>0.0)hit=max(hit,d1);
        }
    }
    vec4 surface=vec4(0);
    vec3 window=vec3(0);                          // premultiplied, added after the colour scale
    float cover=hit>0.0?Visible(hit,depthUv):0.0;
    if(cover>0.0) {
        vec3 p=AT(hit);
        // Toward the camera: depth increases along the ray toward it.
        vec3 dir=normalize(AT(min(hit+0.001,1.0))-p+1e-9);
        vec3 normal=normalize(p/(axes*axes));
        float facing=abs(dot(normal,dir));
        vec2 uv=p.xy/kMetres.xy;            // full-size portal units, rim at kRim
        float radius=length(uv)/s, angle=atan(uv.y,uv.x);
        // Same palette as the cached HUD texture (portal_render.cpp BuildFrames).
        float flow=0.5+0.5*sin(radius*22.0-angle*3.0-t*2.0+0.65*sin(angle*6.0+t));
        float strands=pow(0.5+0.5*sin(angle*5.0+t-radius*29.0),8.0)*radius*radius*radius;
        vec3 interior=tint*(0.16+0.28*flow+0.2*strands);
        if(live) interior=texture(sampler2D(destination,imageSampler),screen).rgb;
        // The other side through the lens, once it has formed; the swirl stays as a faint
        // film over it and the rim stays energy.
        float see=0.0;vec3 view=vec3(0);
        if(hdr&&snap.z>0.5&&abs(B.w)>1e-9) {
            vec4 w=Window(B.xyz/B.w*kMetres,p);
            see=w.a*smoothstep(0.55,1.0,form)*(1.0-smoothstep(0.78,0.95,radius));
            view=w.rgb;
            // Taken in the Shroud: the game's dark blue grade for it comes after the snapshot,
            // so give it a similar one here (seen from the Shroud the game grades it anyway).
            if(snap.w>0.5) {
                float luma=dot(view,vec3(0.2126,0.7152,0.0722));
                view=mix(vec3(luma),view,0.35)*vec3(0.42,0.58,0.85)*0.55;
            }
            interior*=1.0-0.85*see;
        }
        // Rim where the lens curves away from the viewer; it flares while forming.
        float edge=1.0-facing;
        float rimLight=pow(edge,5.0)*1.2+pow(edge,1.5)*0.1+exp(-max(kRim-radius,0.0)*30.0)*0.9;
        float flare=1.0+1.5*(1.0-grow);
        vec3 rim=(tint+vec3(0.35)*rimLight)*rimLight*flare*(0.87+0.13*sin(angle*17.0-t*2.0));
        // Soften the silhouette pixels.
        float alpha=(1.0-smoothstep(0.985,1.0,edge))*cover;
        surface=vec4((interior+rim)*alpha,alpha);
        window=view*see*alpha;
    }

    // Effects on the portal plane, faded as it turns edge-on: a soft halo just
    // outside the rim, and while the portal forms a ring of sparks traced
    // round and round its growing edge, like a conjured circle.
    float sparks=0.0, halo=0.0;
    float planeCover=plane.z>0.0&&plane.w>=0.0&&plane.w<=1.0?Visible(plane.w,depthUv):0.0;
    if(planeCover>0.0) {
        vec2 uv=plane.xy/plane.z*1.16;
        float reach=length(uv), angle=atan(uv.y,uv.x);
        if(reach<=1.7) {
            vec3 p=AT(plane.w);
            float side=smoothstep(0.05,0.3,abs(normalize(AT(min(plane.w+0.001,1.0))-p+1e-9).z));
            float outside=reach/s-kRim;
            if(hit<0.0&&outside>0.0) halo=exp(-outside*40.0)*0.45*side;
            float charge=(1.0-smoothstep(0.85,1.0,form))*smoothstep(0.0,0.05,form)*side;
            if(charge>0.0) {
                float ring=kRim*max(s,0.12), off=reach-ring;
                float head=t*6.2832*1.4;                     // 1.4 turns a second
                float behind=mod(head-angle,6.2832);
                float trail=exp(-behind*1.2);
                float line=exp(-off*off/0.0006);
                // Sparkle: random cells along the ring flicker and drift outward.
                float cell=floor((angle+3.1416)*9.55), tick=floor(t*24.0);
                float h=fract(sin(cell*12.9898+tick*78.233)*43758.5453);
                float drift=off-0.04*fract(h*7.0);
                float sparkle=step(0.72,h)*exp(-drift*drift/0.0015)*trail;
                sparks=(line*(0.18+1.6*trail)+sparkle*1.2)*charge;
            }
        }
    }
    sparks*=planeCover;halo*=planeCover;
    vec3 energy=vec3(1.0,0.72,0.32)*sparks*1.5+tint*halo;
    // The spell from the caster's hand while the portal forms (drawn in the game's frame only).
    vec3 spell=vec3(0);
    if(hdr&&castInfo.w>0.5&&abs(B.w)>1e-9) {
        vec3 eye=B.xyz/B.w*kMetres;
        vec3 ray=normalize(AT(0.05)-eye);
        float sceneT=solid?length(p-eye):1e9;
        spell=CastEffect(eye,ray,sceneT,form,t,kRim*max(s,0.12));
    }
    // The open portal's aura (not while it is still forming from the caster's spell).
    if(hdr&&abs(B.w)>1e-9&&castInfo.w<0.5) {
        vec3 eye=B.xyz/B.w*kMetres;
        vec3 ray=normalize(AT(0.05)-eye);
        float sceneT=solid?length(p-eye):1e9;
        spell+=PortalAura(eye,ray,sceneT,t,smoothstep(0.6,1.0,form));
    }
    if(surface.a+sparks+halo<0.002) {
        if(dot(spell,spell)>1e-8) { color=vec4(spell,0.0); return; }
        REJECT
    }
    // Premultiplied: surface over the scene, sparks, halo and light added.
    color=vec4(surface.rgb+energy+light*(1.0-surface.a),clamp(surface.a+0.25*sparks,0.0,1.0));
    // In the game's frame the colour still goes through the game's tone mapping, which
    // lifts darks and drains colour; inFrame: x saturation, y surface brightness, z light
    // strength, w contrast (a power on the surface colour).
    if(hdr) {
        vec3 body=color.rgb;
        float peak=max(max(body.r,body.g),max(body.b,1e-4));
        body*=pow(peak,inFrame.w-1.0);
        body=max(mix(vec3(dot(body,vec3(0.2126,0.7152,0.0722))),body,inFrame.x),vec3(0));
        color.rgb=body*inFrame.y+window+spell;
    }
}
