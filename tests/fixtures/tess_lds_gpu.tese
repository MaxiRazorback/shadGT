#version 450
layout(triangles, equal_spacing, cw) in;
layout(location = 0) in vec4 value[];
layout(set = 0, binding = 0, std430) buffer Result { uint words[]; } result;
void main() {
    atomicExchange(result.words[gl_PrimitiveID], floatBitsToUint(value[0].x));
    gl_Position = vec4(gl_TessCoord.xy, 0.0, 1.0);
}
