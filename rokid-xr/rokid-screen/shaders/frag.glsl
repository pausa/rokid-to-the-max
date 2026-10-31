#version 450
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;
layout(binding = 0) uniform sampler2D tex;

void
main()
{
	// outside the letterboxed source area the vertex shader clamps uv
	// below 0 or above 1; render black there.
	if (any(lessThan(v_uv, vec2(0.0))) || any(greaterThan(v_uv, vec2(1.0)))) {
		out_color = vec4(0.0, 0.0, 0.0, 1.0);
	} else {
		out_color = vec4(texture(tex, v_uv).bgr, 1.0);
	}
}
