#define NOMINMAX
#include "OpenVRProjection.h"
#include <cassert>
#include <cstdio>
#include <d3dcompiler.h>
#include <fstream>
#include <sstream>
#pragma comment(lib, "d3dcompiler.lib")
int main() {
    auto p = stereo::MakeProjection({-1.4f, .8f, -1, 1}, {-.8f, 1.4f, -1, 1});
    assert(p.valid && p.widthScale > 1.27f && p.widthScale < 1.28f);
    float l[4], r[4]; stereo::EyeMap(p, 0, l); stereo::EyeMap(p, 1, r);
    assert(std::abs(l[2]) < 0.0001f && r[2] < 0);
    assert(l[0] + l[2] > 1 && std::abs(r[0] + r[2] - 1) < .0001f);
    assert(std::abs((.5f*l[0]+l[2]) - (.5f*r[0]+r[2])) > .2f);
    assert(!stereo::MakeProjection({0,0,0,0}, {}).valid);
    assert(!stereo::MakeProjection({-2,-1,-1,1}, {1,2,-1,1}).valid);
    std::ifstream file("ALVRCapturePreview.cpp"); std::stringstream buf; buf << file.rdbuf();
    auto source = buf.str(); auto begin = source.find("Texture2D leftImage");
    auto end = source.find(")\";", begin);
    auto shader = source.substr(begin, end-begin);
    for (auto entry : {"VS", "PS"}) {
        ID3DBlob *code = nullptr, *errors = nullptr;
        auto hr = D3DCompile(shader.data(), shader.size(), nullptr, nullptr, nullptr, entry,
            entry[0] == 'V' ? "vs_4_0" : "ps_4_0", 0, 0, &code, &errors);
        if (errors) { std::printf("%s\n", static_cast<char*>(errors->GetBufferPointer())); errors->Release(); }
        if (FAILED(hr)) return 2;
        code->Release();
    }
    std::puts("PASS: projection union, UV mapping, invalid bounds, production preview shaders");
    wchar_t dll[MAX_PATH]; GetFullPathNameW(L"openvr_api.dll", MAX_PATH, dll, nullptr);
    stereo::RuntimeProjection runtime(dll); auto live = runtime.Poll();
    std::printf("Live projection valid=%d widthScale=%f left=[%f,%f,%f,%f] right=[%f,%f,%f,%f]\n",
        live.valid, live.widthScale, live.eyes[0].left,live.eyes[0].right,live.eyes[0].top,live.eyes[0].bottom,
        live.eyes[1].left,live.eyes[1].right,live.eyes[1].top,live.eyes[1].bottom);
    return live.valid ? 0 : 3;
}
