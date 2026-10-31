#version 450
// Fullscreen triangle: gl_VertexIndex 0,1,2 cover the viewport.
vec2 pos[3] = vec2[3](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));

layout(location = 0) out vec2 v_uv;
// Push constants: uv scale/offset for letterboxing (uv = uv0 + uv_coord * uv1)
layout(push_constant) uniform Push {
	layout(offset = 0) vec4 uv; // xy = offset, zw = scale
} pc;

void
main()
{
	gl_Position = vec4(pos[gl_VertexIndex], 0.0, 1.0);
	vec2 uv = pos[gl_VertexIndex] * 0.5 + 0.5;
	v_uv = pc.uv.xy + uv * pc.uv.zw;
}
