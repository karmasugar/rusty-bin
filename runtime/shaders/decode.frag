#version 450
/* PC-98 frame decode, the same result as video_render() in video.c: 640x400 target, one fragment per pixel.
 * Frame buffer: displayed page planes B, R, G, I (8000h bytes each), then text VRAM A0000h-A3FFFh (codes, attributes
 * at +2000h). Font buffer: FONT.ROM. Params: palette (already scaled to 0..1), display areas and flags. */

layout(location = 0) out vec4 out_color;

layout(std430, set = 2, binding = 0) readonly buffer Frame { uint frame_words[]; };
layout(std430, set = 2, binding = 1) readonly buffer Font { uint font_words[]; };

layout(std140, set = 3, binding = 0) uniform Params
{
    vec4 palette[16];
    /* x: area 0 start byte, y: area 1 start byte, z: area 0 lines, w: bit 0 400 lines, bit 1 graphics on, bit 2 text on */
    uvec4 display;
};

const uint PLANE_SIZE = 0x8000u;
const uint TEXT_BASE = 4u * PLANE_SIZE;

uint frame_byte(uint index)
{
    return (frame_words[index >> 2] >> ((index & 3u) * 8u)) & 0xFFu;
}

uint font_byte(uint index)
{
    return (font_words[index >> 2] >> ((index & 3u) * 8u)) & 0xFFu;
}

uint font_offset(uint code_high, uint code_low, uint line, bool left)
{
    line &= 0x0Fu;
    if (code_high == 0u) return 0x800u + code_low * 16u + line;
    uint row = code_high + 0x20u;
    uint col = code_low;
    if (row < 0x21u || row > 0x7Cu || col < 0x20u || col > 0x7Fu) return 0u;
    uint base = 0x1800u + ((row - 0x21u) * 96u + (col - 0x20u)) * 32u;
    return base + (left ? 0u : 16u) + line;
}

vec3 graphics_pixel(uint x, uint y)
{
    bool lines_400 = (display.w & 1u) != 0u;
    uint rows = lines_400 ? 400u : 200u;
    uint source_row = lines_400 ? y : y / 2u;
    if ((display.w & 2u) == 0u || source_row >= rows) return vec3(0.0);
    uint first_lines = display.z == 0u ? rows : display.z;
    uint row_offset;
    if (source_row < first_lines) {
        row_offset = (display.x + source_row * 80u) % PLANE_SIZE;
    } else {
        row_offset = (display.y + (source_row - first_lines) * 80u) % PLANE_SIZE;
    }
    uint offset = (row_offset + x / 8u) % PLANE_SIZE;
    uint shift = 7u - (x & 7u);
    uint color = 0u;
    for (uint plane = 0u; plane < 4u; plane++) {
        color |= ((frame_byte(plane * PLANE_SIZE + offset) >> shift) & 1u) << plane;
    }
    return palette[color].rgb;
}

void main()
{
    uint x = uint(gl_FragCoord.x);
    uint y = uint(gl_FragCoord.y);
    vec3 rgb = graphics_pixel(x, y);
    if ((display.w & 4u) != 0u) {
        uint cell = ((y / 16u) * 80u + x / 8u) * 2u;
        uint code_low = frame_byte(TEXT_BASE + cell);
        uint code_high = frame_byte(TEXT_BASE + cell + 1u);
        uint cell_attribute = frame_byte(TEXT_BASE + 0x2000u + cell);
        if ((cell_attribute & 0x01u) != 0u) {
            uint line = y & 15u;
            uint offset = code_high == 0u ? font_offset(0u, code_low, line, true)
                                          : font_offset(code_low & 0x7Fu, code_high, line, (code_low & 0x80u) == 0u);
            uint bits = offset != 0u ? font_byte(offset) : 0u;
            if ((cell_attribute & 0x04u) != 0u) bits = ~bits & 0xFFu;
            if ((bits & (0x80u >> (x & 7u))) != 0u) {
                rgb = vec3((cell_attribute & 0x40u) != 0u ? 1.0 : 0.0, (cell_attribute & 0x80u) != 0u ? 1.0 : 0.0,
                           (cell_attribute & 0x20u) != 0u ? 1.0 : 0.0);
            }
        }
    }
    out_color = vec4(rgb, 1.0);
}
