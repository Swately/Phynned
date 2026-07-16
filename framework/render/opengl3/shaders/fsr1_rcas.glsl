// pillars/render/opengl3/shaders/fsr1_rcas.glsl
// FSR1 RCAS — Robust Contrast-Adaptive Sharpening, GLSL 330 fragment shader.
//
// Standalone reference; production code embeds this as kRcasFragSrc in FSR1Pass.cpp.
//
// con0.x = f(exp2(-sharpness))   — 1.0 = max sharpening, ~0.25 = min
// con0.y = f(1 / output_width)
// con0.z = f(1 / output_height)
//
#version 330 core
precision highp float;

uniform sampler2D texSrc;
uniform uvec4 con0;

in  vec2 vUV;
out vec4 fragColor;

float luma(vec3 c) { return 0.213 * c.r + 0.715 * c.g + 0.072 * c.b; }

void main() {
    float amp  = uintBitsToFloat(con0.x);
    float rcpW = uintBitsToFloat(con0.y);
    float rcpH = uintBitsToFloat(con0.z);

    vec2 dx = vec2(rcpW, 0.0);
    vec2 dy = vec2(0.0,  rcpH);

    vec3 e = texture(texSrc, vUV     ).rgb;
    vec3 b = texture(texSrc, vUV + dy).rgb;
    vec3 d = texture(texSrc, vUV - dx).rgb;
    vec3 f = texture(texSrc, vUV + dx).rgb;
    vec3 h = texture(texSrc, vUV - dy).rgb;

    float le = luma(e), lb = luma(b), ld = luma(d), lf = luma(f), lh = luma(h);

    float mn = min(le, min(lb, min(ld, min(lf, lh))));
    float mx = max(le, max(lb, max(ld, max(lf, lh))));

    float peak = clamp(min(mn, 2.0 - mx) / mx, 0.0, 1.0);
    float w    = -sqrt(peak) * amp * 0.25;

    vec3  col  = (b + d + f + h) * w + e;
    float norm = 1.0 + 4.0 * w;
    fragColor  = vec4(clamp(col / norm, 0.0, 1.0), 1.0);
}
// Made with my soul - Swately <3
