#version 450
layout(location = 0) out vec4 value;
layout(push_constant) uniform Push { float base; } pc;
void main() {
    value = vec4(pc.base + float(gl_VertexIndex + 1), 0.0, 0.0, 1.0);
    gl_Position = vec4(0.0, 0.0, 0.0, 1.0);
}
