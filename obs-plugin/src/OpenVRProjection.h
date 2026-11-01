#pragma once
#include <windows.h>
#include "../../openvr/headers/openvr.h"
#include "StereoProjection.h"

namespace stereo {
// One instance per consumer. No compositor submission or headset-view settings
// are changed. Loading is explicit so OBS's DLL search path is not modified.
class RuntimeProjection {
    HMODULE module = nullptr;
    vr::IVRSystem* system = nullptr;
    using Init = uint32_t (__cdecl*)(vr::EVRInitError*, vr::EVRApplicationType, const char*);
    using Get = void* (__cdecl*)(const char*, vr::EVRInitError*);
    using Shutdown = void (__cdecl*)();
    Shutdown shutdown = nullptr;
    ULONGLONG nextPoll = 0;
    bool initialized = false;
    Projection projection;
    float convergence = 1.f;
public:
    explicit RuntimeProjection(const wchar_t* dllPath) {
        module = LoadLibraryExW(dllPath, nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    }
    ~RuntimeProjection() {
        if (initialized && shutdown) shutdown();
        if (module) FreeLibrary(module);
    }
    void SetConvergence(float meters) {
        meters = std::clamp(meters, .25f, 20.f);
        if (meters != convergence) { convergence = meters; nextPoll = 0; }
    }
    Projection Poll() {
        if (GetTickCount64() < nextPoll) return projection;
        nextPoll = GetTickCount64() + 1000;
        projection = {};
        if (!module) return projection;
        auto get = reinterpret_cast<Get>(GetProcAddress(module, "VR_GetGenericInterface"));
        auto init = reinterpret_cast<Init>(GetProcAddress(module, "VR_InitInternal2"));
        shutdown = reinterpret_cast<Shutdown>(GetProcAddress(module, "VR_ShutdownInternal"));
        if (!get || !init || !shutdown) return projection;
        vr::EVRInitError error = vr::VRInitError_None;
        if (!initialized) {
            init(&error, vr::VRApplication_Background, nullptr);
            if (error != vr::VRInitError_None) return projection;
            initialized = true;
        }
        system = static_cast<vr::IVRSystem*>(get(vr::IVRSystem_Version, &error));
        if (!system || error != vr::VRInitError_None || !system->IsTrackedDeviceConnected(vr::k_unTrackedDeviceIndex_Hmd)) {
            shutdown(); initialized = false; system = nullptr; return projection;
        }
        Bounds b[2];
        for (int i = 0; i < 2; ++i) {
            // ALVR currently supplies parallel eye axes. Refuse unsupported
            // canted projections rather than silently mapping them incorrectly.
            const auto transform = system->GetEyeToHeadTransform(static_cast<vr::EVREye>(i));
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 3; ++c)
                    if (std::abs(transform.m[r][c] - (r == c ? 1.f : 0.f)) > 0.001f) return projection;
            system->GetProjectionRaw(static_cast<vr::EVREye>(i), &b[i].left, &b[i].right, &b[i].top, &b[i].bottom);
        }
        // Project both eye frusta onto a common plane. This also handles
        // symmetric frusta: eye translation contributes the outer slivers.
        for (int i = 0; i < 2; ++i) {
            auto t = system->GetEyeToHeadTransform(static_cast<vr::EVREye>(i));
            const float x = t.m[0][3] / convergence;
            const float y = -t.m[1][3] / convergence;
            b[i].left += x; b[i].right += x;
            b[i].top += y; b[i].bottom += y;
        }
        projection = MakeProjection(b[0], b[1]);
        return projection;
    }
};
}
