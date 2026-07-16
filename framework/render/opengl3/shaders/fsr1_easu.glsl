// pillars/render/opengl3/shaders/fsr1_easu.glsl
// FSR1 EASU — Edge-Adaptive Spatial Upsampling, GLSL 330 fragment shader.
//
// This file is the standalone reference source.  The production build embeds
// the shader as a string literal in FSR1Pass.cpp (kEasuFragSrc).
//
// Constants (uvec4, packed floats via floatBitsToUint):
//   con0.xy  = (inW/outW,          inH/outH)
//   con0.zw  = (0.5*inW/outW-0.5,  0.5*inH/outH-0.5)
//   con1.xy  = (1/inW,             1/inH)
//
#version 330 core
precision highp float;

uniform sampler2D texSrc;
uniform uvec4 con0;
uniform uvec4 con1;

in  vec2 vUV;
out vec4 fragColor;

float luma(vec3 c) { return 0.5 * c.r + c.g + 0.5 * c.b; }

vec3 samp(vec2 base, vec2 d, vec2 off) {
    return texture(texSrc, base + d * off).rgb;
}

float fw(float d_par, float d_perp) {
    float wp = max(0.0, 1.0 - abs(d_par)  * 0.5);
    float wq = max(0.0, 1.0 - abs(d_perp) * 2.0);
    return wp * wp * wq * wq;
}

void main() {
    float scX  = uintBitsToFloat(con0.x);
    float scY  = uintBitsToFloat(con0.y);
    float ofX  = uintBitsToFloat(con0.z);
    float ofY  = uintBitsToFloat(con0.w);
    float rcpW = uintBitsToFloat(con1.x);
    float rcpH = uintBitsToFloat(con1.y);

    vec2 pp   = gl_FragCoord.xy * vec2(scX, scY) + vec2(ofX, ofY);
    vec2 fp   = floor(pp);
    vec2 pf   = pp - fp;

    vec2 base = (fp + vec2(0.5)) * vec2(rcpW, rcpH);
    vec2 d    = vec2(rcpW, rcpH);

    vec3 vB = samp(base, d, vec2( 0.0, -1.0));
    vec3 vC = samp(base, d, vec2( 1.0, -1.0));
    vec3 vE = samp(base, d, vec2(-1.0,  0.0));
    vec3 vF = samp(base, d, vec2( 0.0,  0.0));
    vec3 vG = samp(base, d, vec2( 1.0,  0.0));
    vec3 vH = samp(base, d, vec2( 2.0,  0.0));
    vec3 vI = samp(base, d, vec2(-1.0,  1.0));
    vec3 vJ = samp(base, d, vec2( 0.0,  1.0));
    vec3 vK = samp(base, d, vec2( 1.0,  1.0));
    vec3 vL = samp(base, d, vec2( 2.0,  1.0));
    vec3 vN = samp(base, d, vec2( 0.0,  2.0));
    vec3 vO = samp(base, d, vec2( 1.0,  2.0));

    float lB = luma(vB), lC = luma(vC);
    float lE = luma(vE), lF = luma(vF);
    float lG = luma(vG), lH = luma(vH);
    float lI = luma(vI), lJ = luma(vJ);
    float lK = luma(vK), lL = luma(vL);
    float lN = luma(vN), lO = luma(vO);

    // gx positive = brighter on the right; gy positive = brighter below.
    float gx = (lG - lF) + (lK - lJ)
             + 0.5 * ((lC - lB) + (lH - lE) + (lL - lI) + (lO - lN));
    float gy = (lJ - lF) + (lK - lG)
             + 0.5 * ((lI - lE) + (lL - lH) + (lN - lB) + (lO - lC));
    float gl_ = sqrt(gx * gx + gy * gy);

    if (gl_ < 1e-3) {
        fragColor = vec4(texture(texSrc, base + d * pf).rgb, 1.0);
        return;
    }

    vec2 dir = vec2(gx, gy) / gl_;

    vec3  col  = vec3(0.0);
    float wsum = 0.0;

    #define TAP(v, ox, oy) {                                  \
        vec2  rel    = vec2(float(ox), float(oy)) - pf;      \
        float d_par  = rel.x * (-dir.y) + rel.y * dir.x;    \
        float d_perp = rel.x *   dir.x  + rel.y * dir.y;    \
        float wt     = fw(d_par, d_perp);                    \
        col  += (v) * wt;                                     \
        wsum += wt;                                           \
    }

    TAP(vB,  0.0, -1.0)  TAP(vC,  1.0, -1.0)
    TAP(vE, -1.0,  0.0)  TAP(vF,  0.0,  0.0)
    TAP(vG,  1.0,  0.0)  TAP(vH,  2.0,  0.0)
    TAP(vI, -1.0,  1.0)  TAP(vJ,  0.0,  1.0)
    TAP(vK,  1.0,  1.0)  TAP(vL,  2.0,  1.0)
    TAP(vN,  0.0,  2.0)  TAP(vO,  1.0,  2.0)

    #undef TAP

    fragColor = vec4(col / max(wsum, 1e-5), 1.0);
}
// Made with my soul - Swately <3
