#include <graphics/graphics.h>
#include <graphics/vec2.h>
#include <graphics/vec4.h>
#include <util/bmem.h>
#include <cstdio>
#include <cstdint>
#include <vector>
int main(int argc, char** argv) {
    if (argc != 3) return 2;
    graphics_t* graphics = nullptr;
    if (gs_create(&graphics, argv[1], 0) != 0) return 3;
    gs_enter_context(graphics);
    char* errors = nullptr;
    auto effect = gs_effect_create_from_file(argv[2], &errors);
    if (!effect) { std::printf("Effect failed: %s\n", errors ? errors : "unknown"); bfree(errors); return 4; }
    std::vector<uint32_t> red(128*64, 0xff0000ff), blue(128*64, 0xffff0000);
    const uint8_t* rd = reinterpret_cast<const uint8_t*>(red.data());
    const uint8_t* bl = reinterpret_cast<const uint8_t*>(blue.data());
    auto left = gs_texture_create(128,64,GS_RGBA,1,&rd,0);
    auto right = gs_texture_create(128,64,GS_RGBA,1,&bl,0);
    auto target = gs_texrender_create(GS_RGBA,GS_ZS_NONE);
    auto stage = gs_stagesurface_create(128,64,GS_RGBA);
    int result = 0;
    for (int dominant = 0; dominant < 2; ++dominant) {
        if (!gs_texrender_begin(target,128,64)) return 5;
        gs_ortho(0,128,0,64,-100,100);
        gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
        gs_effect_set_texture(gs_effect_get_param_by_name(effect,"left_image"),left);
        gs_effect_set_texture(gs_effect_get_param_by_name(effect,"right_image"),right);
        vec2 mul = {1,1}, add = {0,0};
        vec4 lm, rm;
        vec4_set(&lm, 2.8f/2.2f, 1, 0, 0);
        vec4_set(&rm, 2.8f/2.2f, 1, -.6f/2.2f, 0);
        gs_effect_set_vec2(gs_effect_get_param_by_name(effect,"uv_mul"), &mul);
        gs_effect_set_vec2(gs_effect_get_param_by_name(effect,"uv_add"), &add);
        gs_effect_set_vec4(gs_effect_get_param_by_name(effect,"left_map"), &lm);
        gs_effect_set_vec4(gs_effect_get_param_by_name(effect,"right_map"), &rm);
        gs_effect_set_float(gs_effect_get_param_by_name(effect,"left_dominant"),float(dominant));
        while (gs_effect_loop(effect,"Draw")) gs_draw_sprite(nullptr,0,128,64);
        gs_texrender_end(target);
        gs_stage_texture(stage,gs_texrender_get_texture(target));
        uint8_t* pixels = nullptr; uint32_t stride = 0;
        if (!gs_stagesurface_map(stage,&pixels,&stride)) return 6;
        for (int x : {8,64,119}) {
            auto pixel = pixels + 32*stride + x*4;
            bool wantRed = x == 8 || (dominant && x == 64);
            if (pixel[wantRed ? 0 : 2] < 250 || pixel[wantRed ? 2 : 0] > 5 || pixel[3] < 250) {
                std::printf("FAIL dominant=%d x=%d rgba=%u,%u,%u,%u\n",dominant,x,pixel[0],pixel[1],pixel[2],pixel[3]); result = 7;
            }
        }
        gs_stagesurface_unmap(stage);
        gs_texrender_reset(target);
    }
    gs_stagesurface_destroy(stage); gs_texrender_destroy(target);
    gs_texture_destroy(left); gs_texture_destroy(right); gs_effect_destroy(effect);
    gs_leave_context(); gs_destroy(graphics);
    if (!result) std::puts("PASS: actual OBS effect GPU rendering, both dominance modes and secondary outer edges");
    return result;
}
