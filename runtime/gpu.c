/* GPU frame output with the SDL3 GPU API (SPIR-V shaders from runtime/shaders, built by CMakeLists.txt with glslc).
 * Each frame the raw display state (displayed page planes, text VRAM: 24000h bytes) is uploaded to a storage buffer;
 * the font (FONT.ROM with the user-defined glyphs) is uploaded again only when the program changed a glyph.
 * Pass 1 (decode.frag) makes the 640x400 frame in a texture, with the same result as video_render(). Pass 2
 * (present.frag) scales it into the window, letterboxed.
 * RUSTY_FILTER = sharp (default) / nearest / scanlines. RUSTY_ASPECT = 16:10 (default, square pixels) / 4:3.
 * RUSTY_GPU_CHECK=<frames>: every <frames> frames read the decoded frame back and compare it with video_render();
 * the differences are logged ("gpu check"). With RUSTY_HEADLESS the check runs without a window. */
#include "gpu.h"

#include "rt.h"

#include <stdlib.h>
#include <string.h>

#define WIDTH 640
#define HEIGHT 400
#define PLANES_SIZE (4 * 0x8000)
#define TEXT_SIZE 0x4000
#define FRAME_SIZE (PLANES_SIZE + TEXT_SIZE)

static const uint32_t fullscreen_vert[] =
#include "fullscreen.vert.inc"
    ;
static const uint32_t decode_frag[] =
#include "decode.frag.inc"
    ;
static const uint32_t present_frag[] =
#include "present.frag.inc"
    ;

typedef struct {
    float palette[16][4];
    uint32_t display[4];
} DecodeParams;

typedef struct {
    float viewport[4];
    float settings[4];
} PresentParams;

static SDL_GPUDevice *device;
static SDL_Window *target_window;
static SDL_GPUGraphicsPipeline *decode_pipeline;
static SDL_GPUGraphicsPipeline *present_pipeline;
static SDL_GPUBuffer *frame_buffer;
static SDL_GPUBuffer *font_buffer;
static SDL_GPUTransferBuffer *frame_upload;
static SDL_GPUTransferBuffer *font_upload;
static SDL_GPUTransferBuffer *check_download;
static SDL_GPUTexture *frame_texture;
static SDL_GPUSampler *sampler;
static int filter_mode = 1;
static float aspect = 16.0f / 10.0f;
static long check_every;
static long frame_count;
static uint32_t font_version;
static int font_uploaded;

static SDL_GPUShader *create_shader(const uint32_t *code, size_t size, SDL_GPUShaderStage stage, Uint32 samplers,
                                    Uint32 storage_buffers, Uint32 uniform_buffers)
{
    SDL_GPUShaderCreateInfo info = {0};
    info.code = (const Uint8 *)code;
    info.code_size = size;
    info.entrypoint = "main";
    info.format = SDL_GPU_SHADERFORMAT_SPIRV;
    info.stage = stage;
    info.num_samplers = samplers;
    info.num_storage_buffers = storage_buffers;
    info.num_uniform_buffers = uniform_buffers;
    SDL_GPUShader *shader = SDL_CreateGPUShader(device, &info);
    if (!shader) rt_log("gpu: shader: %s", SDL_GetError());
    return shader;
}

static SDL_GPUGraphicsPipeline *create_pipeline(SDL_GPUShader *fragment, SDL_GPUTextureFormat format)
{
    SDL_GPUShader *vertex = create_shader(fullscreen_vert, sizeof(fullscreen_vert), SDL_GPU_SHADERSTAGE_VERTEX, 0, 0, 0);
    if (!vertex || !fragment) return 0;
    SDL_GPUColorTargetDescription target = {0};
    target.format = format;
    SDL_GPUGraphicsPipelineCreateInfo info = {0};
    info.vertex_shader = vertex;
    info.fragment_shader = fragment;
    info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    info.target_info.color_target_descriptions = &target;
    info.target_info.num_color_targets = 1;
    SDL_GPUGraphicsPipeline *pipeline = SDL_CreateGPUGraphicsPipeline(device, &info);
    if (!pipeline) rt_log("gpu: pipeline: %s", SDL_GetError());
    SDL_ReleaseGPUShader(device, vertex);
    SDL_ReleaseGPUShader(device, fragment);
    return pipeline;
}

static void upload_font(SDL_GPUCopyPass *copy, const VideoState *state)
{
    memcpy(SDL_MapGPUTransferBuffer(device, font_upload, true), state->font, state->font_size);
    SDL_UnmapGPUTransferBuffer(device, font_upload);
    SDL_GPUTransferBufferLocation source = {.transfer_buffer = font_upload};
    SDL_GPUBufferRegion destination = {.buffer = font_buffer, .size = state->font_size};
    SDL_UploadToGPUBuffer(copy, &source, &destination, true);
    font_version = state->font_version;
    font_uploaded = 1;
}

static void read_settings(void)
{
    const char *filter = getenv("RUSTY_FILTER");
    if (filter && strcmp(filter, "nearest") == 0) filter_mode = 0;
    if (filter && strcmp(filter, "scanlines") == 0) filter_mode = 2;
    const char *ratio = getenv("RUSTY_ASPECT");
    if (ratio && strcmp(ratio, "4:3") == 0) aspect = 4.0f / 3.0f;
    if (getenv("RUSTY_GPU_CHECK")) check_every = atol(getenv("RUSTY_GPU_CHECK"));
}

int gpu_init(SDL_Window *window)
{
    read_settings();
    device = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV, getenv("RUSTY_GPU_DEBUG") != 0, 0);
    if (!device) {
        rt_log("gpu: no device: %s", SDL_GetError());
        return 0;
    }
    if (window) {
        if (!SDL_ClaimWindowForGPUDevice(device, window)) {
            rt_log("gpu: claim window: %s", SDL_GetError());
            SDL_DestroyGPUDevice(device);
            device = 0;
            return 0;
        }
        target_window = window;
        if (SDL_WindowSupportsGPUPresentMode(device, window, SDL_GPU_PRESENTMODE_MAILBOX)) {
            SDL_SetGPUSwapchainParameters(device, window, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, SDL_GPU_PRESENTMODE_MAILBOX);
        }
    }
    SDL_GPUShader *decode = create_shader(decode_frag, sizeof(decode_frag), SDL_GPU_SHADERSTAGE_FRAGMENT, 0, 2, 1);
    decode_pipeline = create_pipeline(decode, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM);
    if (window) {
        SDL_GPUShader *present = create_shader(present_frag, sizeof(present_frag), SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 0, 1);
        present_pipeline = create_pipeline(present, SDL_GetGPUSwapchainTextureFormat(device, window));
    }
    if (!decode_pipeline || (window && !present_pipeline)) return 0;

    SDL_GPUTextureCreateInfo texture_info = {0};
    texture_info.type = SDL_GPU_TEXTURETYPE_2D;
    texture_info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    texture_info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    texture_info.width = WIDTH;
    texture_info.height = HEIGHT;
    texture_info.layer_count_or_depth = 1;
    texture_info.num_levels = 1;
    frame_texture = SDL_CreateGPUTexture(device, &texture_info);
    SDL_GPUBufferCreateInfo buffer_info = {.usage = SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ, .size = FRAME_SIZE};
    frame_buffer = SDL_CreateGPUBuffer(device, &buffer_info);
    SDL_GPUTransferBufferCreateInfo upload_info = {.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, .size = FRAME_SIZE};
    frame_upload = SDL_CreateGPUTransferBuffer(device, &upload_info);
    VideoState state;
    video_state(&state);
    SDL_GPUBufferCreateInfo font_info = {.usage = SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ, .size = state.font_size};
    font_buffer = SDL_CreateGPUBuffer(device, &font_info);
    SDL_GPUTransferBufferCreateInfo font_upload_info = {.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
                                                        .size = state.font_size};
    font_upload = SDL_CreateGPUTransferBuffer(device, &font_upload_info);
    SDL_GPUTransferBufferCreateInfo download_info = {.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD,
                                                     .size = WIDTH * HEIGHT * 4};
    if (check_every > 0) check_download = SDL_CreateGPUTransferBuffer(device, &download_info);
    SDL_GPUSamplerCreateInfo sampler_info = {0};
    sampler_info.min_filter = SDL_GPU_FILTER_LINEAR;
    sampler_info.mag_filter = SDL_GPU_FILTER_LINEAR;
    sampler_info.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    sampler_info.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler_info.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler_info.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler = SDL_CreateGPUSampler(device, &sampler_info);
    int check_ready = check_every <= 0 || check_download;
    if (!frame_texture || !frame_buffer || !frame_upload || !font_buffer || !font_upload || !sampler || !check_ready) {
        rt_log("gpu: resources: %s", SDL_GetError());
        return 0;
    }
    rt_log("gpu: %s, %s", SDL_GetGPUDeviceDriver(device), window ? "window" : "no window (check only)");
    return 1;
}

static DecodeParams upload_state(SDL_GPUCommandBuffer *commands)
{
    VideoState state;
    video_state(&state);
    uint8_t *mapped = SDL_MapGPUTransferBuffer(device, frame_upload, true);
    memcpy(mapped, state.planes, PLANES_SIZE);
    memcpy(mapped + PLANES_SIZE, state.text, TEXT_SIZE);
    SDL_UnmapGPUTransferBuffer(device, frame_upload);
    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(commands);
    SDL_GPUTransferBufferLocation source = {.transfer_buffer = frame_upload};
    SDL_GPUBufferRegion destination = {.buffer = frame_buffer, .size = FRAME_SIZE};
    SDL_UploadToGPUBuffer(copy, &source, &destination, true);
    if (!font_uploaded || state.font_version != font_version) upload_font(copy, &state);
    SDL_EndGPUCopyPass(copy);

    DecodeParams params = {0};
    for (int i = 0; i < 16; i++) {
        for (int k = 0; k < 3; k++) params.palette[i][k] = (float)(state.palette[i][k] * 17) / 255.0f;
        params.palette[i][3] = 1.0f;
    }
    params.display[0] = state.area_start[0];
    params.display[1] = state.area_start[1];
    params.display[2] = (uint32_t)state.area0_lines;
    params.display[3] = (state.lines_400 ? 1u : 0u) | (state.graphics_on ? 2u : 0u) | (state.text_on ? 4u : 0u);
    return params;
}

static void decode_pass(SDL_GPUCommandBuffer *commands)
{
    DecodeParams params = upload_state(commands);
    SDL_PushGPUFragmentUniformData(commands, 0, &params, sizeof(params));
    SDL_GPUColorTargetInfo target = {0};
    target.texture = frame_texture;
    target.load_op = SDL_GPU_LOADOP_DONT_CARE;
    target.store_op = SDL_GPU_STOREOP_STORE;
    target.cycle = true;
    SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(commands, &target, 1, 0);
    SDL_BindGPUGraphicsPipeline(pass, decode_pipeline);
    SDL_GPUBuffer *buffers[2] = {frame_buffer, font_buffer};
    SDL_BindGPUFragmentStorageBuffers(pass, 0, buffers, 2);
    SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
    SDL_EndGPURenderPass(pass);
}

static void present_pass(SDL_GPUCommandBuffer *commands, SDL_GPUTexture *swapchain, Uint32 width, Uint32 height)
{
    float scale_w = (float)width;
    float scale_h = scale_w / aspect;
    if (scale_h > (float)height) {
        scale_h = (float)height;
        scale_w = scale_h * aspect;
    }
    SDL_GPUViewport viewport = {((float)width - scale_w) / 2, ((float)height - scale_h) / 2, scale_w, scale_h, 0, 1};
    viewport.x = SDL_floorf(viewport.x);
    viewport.y = SDL_floorf(viewport.y);
    PresentParams params = {{viewport.x, viewport.y, viewport.w, viewport.h}, {WIDTH, HEIGHT, (float)filter_mode, 0}};
    SDL_PushGPUFragmentUniformData(commands, 0, &params, sizeof(params));
    SDL_GPUColorTargetInfo target = {0};
    target.texture = swapchain;
    target.clear_color = (SDL_FColor){0, 0, 0, 1};
    target.load_op = SDL_GPU_LOADOP_CLEAR;
    target.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(commands, &target, 1, 0);
    SDL_BindGPUGraphicsPipeline(pass, present_pipeline);
    SDL_SetGPUViewport(pass, &viewport);
    SDL_GPUTextureSamplerBinding binding = {frame_texture, sampler};
    SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);
    SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
    SDL_EndGPURenderPass(pass);
}

static void compare_with_cpu(void)
{
    const uint8_t *gpu = SDL_MapGPUTransferBuffer(device, check_download, false);
    const uint8_t *cpu = video_render();
    long differences = 0;
    int first_x = -1;
    int first_y = -1;
    for (int i = 0; i < WIDTH * HEIGHT; i++) {
        if (memcmp(&gpu[i * 4], &cpu[i * 3], 3) == 0) continue;
        if (differences++ == 0) {
            first_x = i % WIDTH;
            first_y = i / WIDTH;
        }
    }
    SDL_UnmapGPUTransferBuffer(device, check_download);
    if (differences) {
        rt_log("gpu check frame %ld: %ld pixels differ (first at %d,%d)", frame_count, differences, first_x, first_y);
    } else {
        rt_log("gpu check frame %ld: same as the CPU render", frame_count);
    }
}

void gpu_frame(void)
{
    if (!device) return;
    frame_count++;
    int check = check_every > 0 && frame_count % check_every == 0;
    SDL_GPUCommandBuffer *commands = SDL_AcquireGPUCommandBuffer(device);
    decode_pass(commands);
    if (check) {
        SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(commands);
        SDL_GPUTextureRegion source = {.texture = frame_texture, .w = WIDTH, .h = HEIGHT, .d = 1};
        SDL_GPUTextureTransferInfo destination = {.transfer_buffer = check_download};
        SDL_DownloadFromGPUTexture(copy, &source, &destination);
        SDL_EndGPUCopyPass(copy);
    }
    if (target_window) {
        SDL_GPUTexture *swapchain = 0;
        Uint32 width = 0;
        Uint32 height = 0;
        if (SDL_WaitAndAcquireGPUSwapchainTexture(commands, target_window, &swapchain, &width, &height) && swapchain) {
            present_pass(commands, swapchain, width, height);
        }
    }
    if (!check) {
        SDL_SubmitGPUCommandBuffer(commands);
        return;
    }
    SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(commands);
    SDL_WaitForGPUFences(device, true, &fence, 1);
    SDL_ReleaseGPUFence(device, fence);
    compare_with_cpu();
}
