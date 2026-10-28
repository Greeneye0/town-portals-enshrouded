#version 450
// Stores one frame as a portal snapshot: colour, and in alpha each pixel's depth along the
// camera (clip w, metres), so the portal can trace rays through it.
layout(set=0,binding=0) uniform texture2D frameColor;
layout(set=0,binding=1) uniform texture2D sceneDepth;
layout(set=0,binding=2) uniform sampler imageSampler;
layout(location=2) in vec2 ndc;
// Rows of the frame (NDC x, y, depth, 1) to window space matrix.
layout(location=3) flat in vec4 rows[4];
// Window space to clip w: the camera depth stored.
layout(location=12) flat in vec4 cameraDepth;
// x, y: the part of the frame stored (its NDC scale).
layout(location=13) flat in vec4 snap;
layout(location=0) out vec4 color;

void main() {
    vec2 frame=ndc*snap.xy;                       // NDC, y up
    vec2 uv=vec2(0.5+0.5*frame.x,0.5-0.5*frame.y);
    ivec2 size=textureSize(sampler2D(sceneDepth,imageSampler),0);
    ivec2 pixel=clamp(ivec2(uv*vec2(size)),ivec2(0),size-1);
    float d=texelFetch(sampler2D(sceneDepth,imageSampler),pixel,0).r;
    // A captured texel stores ONE surface depth. Filtering colour across a
    // silhouette bakes foreground blue into background-depth texels; parallax
    // then leaves a detached blue halo. Resample only colour from this surface,
    // with colour and depth fetched from exactly the same native pixels. The
    // portal is anti-aliased later, after its geometry has been reprojected.
    vec2 position=uv*vec2(size)-0.5, f=fract(position);
    ivec2 base=ivec2(floor(position));
    vec3 rgb=vec3(0);float weight=0.0;
    for(int i=0;i<4;++i) {
        ivec2 at=clamp(base+ivec2(i&1,i>>1),ivec2(0),size-1);
        float depth=texelFetch(sampler2D(sceneDepth,imageSampler),at,0).r;
        float w=((i&1)==0?1.0-f.x:f.x)*(i<2?1.0-f.y:f.y);
        float tolerance=max(1e-8,d*0.01);
        w*=1.0-smoothstep(tolerance,2.0*tolerance,abs(depth-d));
        rgb+=texelFetch(sampler2D(frameColor,imageSampler),at,0).rgb*w;
        weight+=w;
    }
    rgb=weight>1e-5?rgb/weight:texelFetch(sampler2D(frameColor,imageSampler),pixel,0).rgb;
    float z=60000.0;                              // sky: never reached
    if(d>0.0) {
        // Reconstruct the depth at the native sample's centre, not at the
        // rescaled output pixel (those differ when snapshot sizes/crops differ).
        vec2 depthFrame=(vec2(pixel)+0.5)/vec2(size)*vec2(2.0,-2.0)+vec2(-1.0,1.0);
        vec4 h;
        for(int r=0;r<4;++r) h[r]=rows[r].x*depthFrame.x+rows[r].y*depthFrame.y+rows[r].z*d+rows[r].w;
        if(abs(h.w)>1e-9) z=clamp(dot(cameraDepth,vec4(h.xyz/h.w,1.0)),0.0,60000.0);
    }
    color=vec4(max(rgb,vec3(0)),z);
}
