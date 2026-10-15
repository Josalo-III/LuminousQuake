#version 450
// The sealed BSP world casts with faces turned away from the light: the far
// side of each occluder. Lit receivers never compare against themselves.
// GPL-2.0-or-later.
layout(location=0) in float facing;
void main() {
    if (facing > 0.0) discard;
}
