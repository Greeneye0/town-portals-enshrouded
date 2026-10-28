#version 450
layout(location=0) in vec2 position;
layout(location=6) in vec4 coordinates;
layout(location=7) in vec2 settings;
// Rows of the screen (NDC x, y, depth, 1) to portal-local matrix.
layout(location=8) in vec4 local0;
layout(location=9) in vec4 local1;
layout(location=10) in vec4 local2;
layout(location=11) in vec4 local3;
layout(location=12) in vec4 colour;
layout(location=13) in vec4 frame;
// The snapshot of the other side (see portal_composite.inl Snapshot): rows x, y and w taking
// window space (metres in front of the exit) to the snapshot's NDC, then distance, exposure
// scale and 1 when one is bound. Capturing one: info x, y is the part of the frame stored.
layout(location=1) in vec4 snapshotX;
layout(location=2) in vec4 snapshotY;
layout(location=3) in vec4 snapshotW;
layout(location=4) in vec4 snapshotInfo;
// The cast effect: the caster's hand in portal metres, effect number (0 none).
layout(location=5) in vec4 castIn;
layout(location=0) out vec4 plane;
layout(location=1) flat out vec2 options;
layout(location=2) out vec2 ndc;
layout(location=3) flat out vec4 rows[4];
layout(location=7) flat out vec3 tint;
layout(location=8) flat out vec2 depthShift;
layout(location=9) flat out vec4 inFrame;
layout(location=10) flat out vec4 snapRows[3];
layout(location=13) flat out vec4 snap;
layout(location=14) flat out vec4 castInfo;
void main() {
    gl_Position=vec4(position.x,-position.y,0,1);
    plane=coordinates;
    options=settings;
    ndc=position;
    rows[0]=local0;rows[1]=local1;rows[2]=local2;rows[3]=local3;
    tint=colour.rgb;
    // NDC jitter to depth-buffer texture coordinates (y runs down the image).
    // frame: over the finished frame (z 0) the jitter in x, y; drawn into the game's
    // frame (z = saturation, at least 1) the surface brightness (w), light strength (x)
    // and contrast (y).
    bool hdr=frame.z>0.5;
    depthShift=hdr?vec2(0):vec2(frame.x,-frame.y)*0.5;
    inFrame=hdr?vec4(frame.z,frame.w,frame.x,frame.y):vec4(0,1,1,1);
    snapRows[0]=snapshotX;snapRows[1]=snapshotY;snapRows[2]=snapshotW;
    snap=snapshotInfo;
    castInfo=castIn;
}
