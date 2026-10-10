#version 450
/* Scale the decoded 640x400 frame into the window viewport. Modes: 0 nearest, 1 sharp bilinear (integer prescale,
 * then bilinear only at the texel edges), 2 sharp bilinear with scanlines. */

layout(location = 0) out vec4 out_color;

layout(set = 2, binding = 0) uniform sampler2D source;

layout(std140, set = 3, binding = 0) uniform Params
{
    vec4 viewport; /* x, y, width, height in window pixels */
    vec4 settings; /* x, y: source size, z: mode */
};

void main()
{
    vec2 size = settings.xy;
    vec2 texel = (gl_FragCoord.xy - viewport.xy) / viewport.zw * size;
    int mode = int(settings.z);
    vec3 rgb;
    if (mode == 0) {
        rgb = texelFetch(source, ivec2(clamp(texel, vec2(0.0), size - 1.0)), 0).rgb;
    } else {
        vec2 prescale = max(floor(viewport.zw / size), vec2(1.0));
        vec2 region = 0.5 - 0.5 / prescale;
        vec2 distance = fract(texel) - 0.5;
        vec2 blend = (distance - clamp(distance, -region, region)) * prescale + 0.5;
        rgb = texture(source, (floor(texel) + blend) / size).rgb;
        if (mode == 2) {
            float edge = abs(fract(texel.y) - 0.5) * 2.0;
            rgb *= 1.0 - 0.35 * edge * edge;
        }
    }
    out_color = vec4(rgb, 1.0);
}
