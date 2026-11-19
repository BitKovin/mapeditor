#version 460
layout(location = 0) in vec4 vColor;
layout(location = 1) in vec2 vFade;
layout(location = 0) out vec4 outColor;

void main() {
    // Grid lines fade out towards the end of their extent; everything else has vFade = 0.
    float d = max(abs(vFade.x), abs(vFade.y));
    float alpha = vColor.a * (1.0 - smoothstep(0.6, 1.0, d));
    outColor = vec4(vColor.rgb, alpha);
}
