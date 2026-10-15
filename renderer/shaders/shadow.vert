#version 450
// Point-light shadow cube face: reversed depth (near / view distance).
// GPL-2.0-or-later.
layout(location=0) in vec3 in_position;
layout(set=1,binding=0,std140) uniform Shadow { mat4 face_projection; } shadow;
void main() {
    gl_Position = shadow.face_projection * vec4(in_position, 1.0);
}
