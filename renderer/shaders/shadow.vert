#version 450
// Point-light shadow cube face: reversed depth (near / view distance).
// GPL-2.0-or-later.
layout(location=0) in vec3 in_position;
layout(location=4) in vec3 in_normal;
layout(set=1,binding=0,std140) uniform Shadow { mat4 face_projection; vec4 light_origin; } shadow;
layout(location=0) out float facing;
void main() {
    gl_Position = shadow.face_projection * vec4(in_position, 1.0);
    // Affine across a planar face, so the interpolated sign is exact.
    facing = dot(in_normal, shadow.light_origin.xyz - in_position);
}
