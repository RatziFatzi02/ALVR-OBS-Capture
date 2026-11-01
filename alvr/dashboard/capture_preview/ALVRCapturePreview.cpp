#define NOMINMAX
#include <windows.h>
#include <mmsystem.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <deque>
#include <memory>
#include <string>
#include <vector>

#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <wrl.h>

#include "../../server_openvr/cpp/platform/win32/CapturePrototypeProtocol.h"
#include "AudioCaptureProtocol.h"
#include "OpenVRProjection.h"

using Microsoft::WRL::ComPtr;
namespace capture = alvr_capture_prototype;
namespace audio_capture = alvr_audio_capture;

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "winmm.lib")

namespace {

constexpr UINT kTimerId = 1;
constexpr UINT kMenuEyeLeft = 100;
constexpr UINT kMenuEyeRight = 101;
constexpr UINT kMenuEyeBothLeft = 102;
constexpr UINT kMenuEyeBothRight = 103;
constexpr UINT kMenuEyeSideBySide = 104;
constexpr UINT kMenuScaleOriginal = 120;
constexpr UINT kMenuScaleFit = 121;
constexpr UINT kMenuScaleFill = 122;
constexpr UINT kMenuFullscreen = 123;
constexpr UINT kMenuAudioEnabled = 140;
constexpr UINT kMenuAudioDeviceFirst = 200;

enum class EyeMode : uint32_t { Left, Right, BothLeftDominant, BothRightDominant, SideBySide };
enum class ScaleMode : uint32_t { Original, Fit, Fill };

DXGI_FORMAT SrgbViewFormat(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    case DXGI_FORMAT_B8G8R8X8_UNORM:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
        return DXGI_FORMAT_B8G8R8X8_UNORM_SRGB;
    default:
        return format;
    }
}

struct SharedEye {
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<IDXGIKeyedMutex> mutex;
    ComPtr<ID3D11ShaderResourceView> srv;
};

struct SharedSlot {
    SharedEye left;
    SharedEye right;
};

struct ShaderSettings {
    uint32_t eyeMode;
    uint32_t padding[3];
    float leftMap[4];
    float rightMap[4];
};

struct QueuedAudio {
    WAVEHDR header = {};
    std::vector<uint8_t> bytes;
};

class AudioPreview {
public:
    ~AudioPreview() { Reset(); }

    void Toggle() {
        m_enabled = !m_enabled;
        if (!m_enabled) ResetOutput();
    }

    bool enabled() const { return m_enabled; }
    UINT device_id() const { return m_deviceId; }

    void SetDevice(UINT deviceId) {
        m_deviceId = deviceId;
        ResetOutput();
    }

    void Pump() {
        if (!m_enabled) return;
        OpenControl();
        if (!m_control) return;
        if (m_control->magic != audio_capture::CONTROL_MAGIC) {
            CloseControl();
            return;
        }

        DrainCompleted();
        if (m_pending.size() >= 8) return;

        const auto& ring = m_control->rings[0]; // Game/headset audio belongs to the preview.
        const uint64_t latest = static_cast<uint64_t>(ReadAcquire64(&ring.writeSequence));
        if (!latest) return;
        if (!m_nextSequence) m_nextSequence = latest;
        if (latest - m_nextSequence >= audio_capture::SLOT_COUNT) m_nextSequence = latest;

        while (m_nextSequence <= latest && m_pending.size() < 8) {
            const auto& slot = ring.slots[(m_nextSequence - 1) % audio_capture::SLOT_COUNT];
            if (static_cast<uint64_t>(ReadAcquire64(&slot.sequence)) != m_nextSequence) {
                m_nextSequence = latest;
                continue;
            }
            if (!EnsureOutput(slot.sampleRate, slot.channels)) return;
            const uint32_t bytes = std::min(slot.payloadBytes, audio_capture::SLOT_PAYLOAD_BYTES);
            if (!bytes || bytes % (slot.channels * sizeof(int16_t))) {
                ++m_nextSequence;
                continue;
            }

            auto queued = std::make_unique<QueuedAudio>();
            queued->bytes.assign(slot.payload, slot.payload + bytes);
            if (static_cast<uint64_t>(ReadAcquire64(&slot.sequence)) != m_nextSequence) continue;
            queued->header.lpData = reinterpret_cast<LPSTR>(queued->bytes.data());
            queued->header.dwBufferLength = static_cast<DWORD>(queued->bytes.size());
            if (waveOutPrepareHeader(m_waveOut, &queued->header, sizeof(WAVEHDR)) != MMSYSERR_NOERROR
                || waveOutWrite(m_waveOut, &queued->header, sizeof(WAVEHDR)) != MMSYSERR_NOERROR) {
                waveOutUnprepareHeader(m_waveOut, &queued->header, sizeof(WAVEHDR));
                return;
            }
            m_pending.push_back(std::move(queued));
            ++m_nextSequence;
        }
    }

private:
    void OpenControl() {
        if (m_control) return;
        m_mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, audio_capture::CONTROL_MAPPING_NAME);
        if (!m_mapping) return;
        m_control = static_cast<audio_capture::AudioCaptureControlBlock*>(
            MapViewOfFile(m_mapping, FILE_MAP_READ, 0, 0, sizeof(audio_capture::AudioCaptureControlBlock))
        );
        if (!m_control || m_control->magic != audio_capture::CONTROL_MAGIC
            || m_control->version != audio_capture::CONTROL_VERSION
            || m_control->structSize != sizeof(audio_capture::AudioCaptureControlBlock)) {
            if (m_control) UnmapViewOfFile(m_control);
            CloseHandle(m_mapping);
            m_control = nullptr;
            m_mapping = nullptr;
        }
    }

    void CloseControl() {
        if (m_control) UnmapViewOfFile(m_control);
        if (m_mapping) CloseHandle(m_mapping);
        m_control = nullptr;
        m_mapping = nullptr;
        m_nextSequence = 0;
    }

    bool EnsureOutput(uint32_t sampleRate, uint16_t channels) {
        if (!sampleRate || !channels || channels > 2) return false;
        if (m_waveOut && (m_sampleRate != sampleRate || m_channels != channels)) ResetOutput();
        if (m_waveOut) return true;

        WAVEFORMATEX format = {};
        format.wFormatTag = WAVE_FORMAT_PCM;
        format.nChannels = channels;
        format.nSamplesPerSec = sampleRate;
        format.wBitsPerSample = 16;
        format.nBlockAlign = static_cast<WORD>(channels * sizeof(int16_t));
        format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
        if (waveOutOpen(&m_waveOut, m_deviceId, &format, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
            m_waveOut = nullptr;
            return false;
        }
        m_sampleRate = sampleRate;
        m_channels = channels;
        return true;
    }

    void DrainCompleted() {
        while (!m_pending.empty() && (m_pending.front()->header.dwFlags & WHDR_DONE)) {
            waveOutUnprepareHeader(m_waveOut, &m_pending.front()->header, sizeof(WAVEHDR));
            m_pending.pop_front();
        }
    }

    void ResetOutput() {
        if (m_waveOut) {
            waveOutReset(m_waveOut);
            for (auto& queued : m_pending) {
                waveOutUnprepareHeader(m_waveOut, &queued->header, sizeof(WAVEHDR));
            }
            waveOutClose(m_waveOut);
        }
        m_pending.clear();
        m_waveOut = nullptr;
        m_sampleRate = 0;
        m_channels = 0;
        m_nextSequence = 0;
    }

    void Reset() {
        ResetOutput();
        CloseControl();
    }

    bool m_enabled = false;
    UINT m_deviceId = WAVE_MAPPER;
    HANDLE m_mapping = nullptr;
    audio_capture::AudioCaptureControlBlock* m_control = nullptr;
    HWAVEOUT m_waveOut = nullptr;
    uint32_t m_sampleRate = 0;
    uint16_t m_channels = 0;
    uint64_t m_nextSequence = 0;
    std::deque<std::unique_ptr<QueuedAudio>> m_pending;
};

class PreviewWindow {
public:
    int Run(HINSTANCE instance) {
        if (!CreateAppWindow(instance)) return 1;
        ShowWindow(m_window, SW_SHOW);
        UpdateWindow(m_window);
        SetTimer(m_window, kTimerId, 10, nullptr);

        MSG message = {};
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        ReleaseVideo();
        return 0;
    }

private:
    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
        PreviewWindow* self = nullptr;
        if (message == WM_NCCREATE) {
            self = static_cast<PreviewWindow*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            self->m_window = window;
        } else {
            self = reinterpret_cast<PreviewWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        }
        return self ? self->HandleMessage(message, wParam, lParam) : DefWindowProcW(window, message, wParam, lParam);
    }

    bool CreateAppWindow(HINSTANCE instance) {
        const wchar_t* className = L"ALVRCapturePreviewWindow";
        WNDCLASSW windowClass = {};
        windowClass.hInstance = instance;
        windowClass.lpszClassName = className;
        windowClass.lpfnWndProc = WindowProc;
        windowClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
        windowClass.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        if (!RegisterClassW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
        return CreateWindowExW(
                   0, className, L"ALVR Capture Preview", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                   CW_USEDEFAULT, CW_USEDEFAULT, 1100, 760, nullptr, nullptr, instance, this
               )
            != nullptr;
    }

    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
        switch (message) {
        case WM_SIZE:
            ResizeSwapChain();
            return 0;
        case WM_TIMER:
            TryConnectVideo();
            Render();
            m_audio.Pump();
            return 0;
        case WM_RBUTTONUP:
            ShowContextMenu(static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam)));
            return 0;
        case WM_COMMAND:
            HandleMenuCommand(LOWORD(wParam));
            return 0;
        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE && m_fullscreen) {
                ToggleFullscreenCurrentMonitor();
                return 0;
            }
            break;
        case WM_CLOSE:
            DestroyWindow(m_window);
            return 0;
        case WM_DESTROY:
            KillTimer(m_window, kTimerId);
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(m_window, message, wParam, lParam);
        }

        return DefWindowProcW(m_window, message, wParam, lParam);
    }

    void ShowContextMenu(int x, int y) {
        HMENU menu = CreatePopupMenu();
        HMENU eyes = CreatePopupMenu();
        HMENU scaling = CreatePopupMenu();
        HMENU convergence = CreatePopupMenu();
        HMENU audio = CreatePopupMenu();
        HMENU devices = CreatePopupMenu();

        AddMenuItem(eyes, kMenuEyeLeft, L"Left Eye", m_eyeMode == EyeMode::Left);
        AddMenuItem(eyes, kMenuEyeRight, L"Right Eye", m_eyeMode == EyeMode::Right);
        AddMenuItem(eyes, kMenuEyeBothLeft, L"Both Eyes - Left Dominant", m_eyeMode == EyeMode::BothLeftDominant);
        AddMenuItem(eyes, kMenuEyeBothRight, L"Both Eyes - Right Dominant", m_eyeMode == EyeMode::BothRightDominant);
        AddMenuItem(eyes, kMenuEyeSideBySide, L"Side-by-Side", m_eyeMode == EyeMode::SideBySide);
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(eyes), L"Eye mode");
        AddMenuItem(convergence, 150, L"Near (0.5 m)", m_convergence == .5f);
        AddMenuItem(convergence, 151, L"Normal (1 m)", m_convergence == 1.f);
        AddMenuItem(convergence, 152, L"Far (2 m)", m_convergence == 2.f);
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(convergence), L"Stereo alignment distance");

        AddMenuItem(scaling, kMenuScaleOriginal, L"Original size", m_scaleMode == ScaleMode::Original);
        AddMenuItem(scaling, kMenuScaleFit, L"Fit to window", m_scaleMode == ScaleMode::Fit);
        AddMenuItem(scaling, kMenuScaleFill, L"Fill window", m_scaleMode == ScaleMode::Fill);
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(scaling), L"Scaling");
        AddMenuItem(menu, kMenuFullscreen, L"Fullscreen on current monitor", m_fullscreen);

        AddMenuItem(audio, kMenuAudioEnabled, L"Play headset audio", m_audio.enabled());
        AppendMenuW(audio, MF_SEPARATOR, 0, nullptr);
        AddMenuItem(devices, kMenuAudioDeviceFirst, L"Default Windows device", m_audio.device_id() == WAVE_MAPPER);
        const UINT deviceCount = waveOutGetNumDevs();
        for (UINT index = 0; index < deviceCount; ++index) {
            WAVEOUTCAPSW caps = {};
            if (waveOutGetDevCapsW(index, &caps, sizeof(caps)) == MMSYSERR_NOERROR) {
                AddMenuItem(devices, kMenuAudioDeviceFirst + index + 1, caps.szPname, m_audio.device_id() == index);
            }
        }
        AppendMenuW(audio, MF_POPUP, reinterpret_cast<UINT_PTR>(devices), L"Output device");
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(audio), L"Audio");

        POINT point = {x, y};
        ClientToScreen(m_window, &point);
        SetForegroundWindow(m_window);
        TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_LEFTALIGN, point.x, point.y, 0, m_window, nullptr);
        DestroyMenu(menu);
    }

    static void AddMenuItem(HMENU menu, UINT id, const wchar_t* label, bool checked) {
        AppendMenuW(menu, MF_STRING | (checked ? MF_CHECKED : MF_UNCHECKED), id, label);
    }

    void HandleMenuCommand(UINT command) {
        switch (command) {
        case 150: m_convergence = .5f; break;
        case 151: m_convergence = 1.f; break;
        case 152: m_convergence = 2.f; break;
        case kMenuEyeLeft: m_eyeMode = EyeMode::Left; break;
        case kMenuEyeRight: m_eyeMode = EyeMode::Right; break;
        case kMenuEyeBothLeft: m_eyeMode = EyeMode::BothLeftDominant; break;
        case kMenuEyeBothRight: m_eyeMode = EyeMode::BothRightDominant; break;
        case kMenuEyeSideBySide: m_eyeMode = EyeMode::SideBySide; break;
        case kMenuScaleOriginal: m_scaleMode = ScaleMode::Original; break;
        case kMenuScaleFit: m_scaleMode = ScaleMode::Fit; break;
        case kMenuScaleFill: m_scaleMode = ScaleMode::Fill; break;
        case kMenuFullscreen: ToggleFullscreenCurrentMonitor(); break;
        case kMenuAudioEnabled: m_audio.Toggle(); break;
        default:
            if (command == kMenuAudioDeviceFirst) {
                m_audio.SetDevice(WAVE_MAPPER);
            } else if (command > kMenuAudioDeviceFirst) {
                m_audio.SetDevice(command - kMenuAudioDeviceFirst - 1);
            }
            break;
        }
        InvalidateRect(m_window, nullptr, FALSE);
    }

    void ToggleFullscreenCurrentMonitor() {
        if (!m_fullscreen) {
            m_windowedStyle = GetWindowLongPtrW(m_window, GWL_STYLE);
            m_windowedPlacement.length = sizeof(m_windowedPlacement);
            GetWindowPlacement(m_window, &m_windowedPlacement);

            MONITORINFO monitorInfo = {sizeof(monitorInfo)};
            GetMonitorInfoW(MonitorFromWindow(m_window, MONITOR_DEFAULTTONEAREST), &monitorInfo);
            SetWindowLongPtrW(m_window, GWL_STYLE, WS_POPUP | WS_VISIBLE);
            SetWindowPos(
                m_window, nullptr, monitorInfo.rcMonitor.left, monitorInfo.rcMonitor.top,
                monitorInfo.rcMonitor.right - monitorInfo.rcMonitor.left,
                monitorInfo.rcMonitor.bottom - monitorInfo.rcMonitor.top,
                SWP_FRAMECHANGED | SWP_NOOWNERZORDER
            );
            m_fullscreen = true;
        } else {
            SetWindowLongPtrW(m_window, GWL_STYLE, m_windowedStyle);
            SetWindowPlacement(m_window, &m_windowedPlacement);
            SetWindowPos(m_window, nullptr, 0, 0, 0, 0,
                         SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOOWNERZORDER);
            m_fullscreen = false;
        }
    }

    void TryConnectVideo() {
        if (m_control) {
            if (m_control->magic == capture::CONTROL_MAGIC) return;
            ReleaseVideo();
        }
        m_mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, capture::CONTROL_MAPPING_NAME);
        if (!m_mapping) return;
        m_control = static_cast<capture::CaptureControlBlock*>(
            MapViewOfFile(m_mapping, FILE_MAP_READ, 0, 0, sizeof(capture::CaptureControlBlock))
        );
        if (!m_control || m_control->magic != capture::CONTROL_MAGIC
            || m_control->version != capture::CONTROL_VERSION
            || m_control->structSize != sizeof(capture::CaptureControlBlock)
            || m_control->slotCount != capture::SLOT_COUNT) {
            ReleaseVideo();
            return;
        }
        MemoryBarrier();
        m_metadata = *m_control;
        if (!CreateDevice() || !OpenSharedSlots() || !CreatePipeline() || !CreateSwapChain()) ReleaseVideo();
    }

    bool CreateDevice() {
        ComPtr<IDXGIFactory1> factory;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return false;
        ComPtr<IDXGIAdapter1> matching;
        for (UINT index = 0;; ++index) {
            ComPtr<IDXGIAdapter1> adapter;
            if (factory->EnumAdapters1(index, &adapter) == DXGI_ERROR_NOT_FOUND) break;
            DXGI_ADAPTER_DESC1 desc = {};
            if (SUCCEEDED(adapter->GetDesc1(&desc)) && desc.AdapterLuid.HighPart == m_metadata.adapterLuid.HighPart
                && desc.AdapterLuid.LowPart == m_metadata.adapterLuid.LowPart) {
                matching = adapter;
                break;
            }
        }
        if (!matching) return false;
        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        D3D_FEATURE_LEVEL selected;
        return SUCCEEDED(D3D11CreateDevice(
            matching.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            levels, ARRAYSIZE(levels), D3D11_SDK_VERSION, &m_device, &selected, &m_context
        ));
    }

    bool OpenSharedSlots() {
        const auto format = static_cast<DXGI_FORMAT>(m_metadata.dxgiFormat);
        for (uint32_t index = 0; index < capture::SLOT_COUNT; ++index) {
            if (!OpenEye(m_metadata.slots[index].leftSharedHandle, format, m_slots[index].left)
                || !OpenEye(m_metadata.slots[index].rightSharedHandle, format, m_slots[index].right)) {
                return false;
            }
        }
        return true;
    }

    bool OpenEye(uint64_t handle, DXGI_FORMAT format, SharedEye& eye) {
        if (!handle || FAILED(m_device->OpenSharedResource(
                           reinterpret_cast<HANDLE>(static_cast<uintptr_t>(handle)), IID_PPV_ARGS(&eye.texture)
                       ))) {
            return false;
        }
        D3D11_TEXTURE2D_DESC desc = {};
        eye.texture->GetDesc(&desc);
        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = SrgbViewFormat(desc.Format);
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MostDetailedMip = 0;
        srvDesc.Texture2D.MipLevels = desc.MipLevels;
        if (desc.Width != m_metadata.eyeWidth || desc.Height != m_metadata.eyeHeight || desc.Format != format
            || FAILED(eye.texture.As(&eye.mutex))
            || FAILED(m_device->CreateShaderResourceView(eye.texture.Get(), &srvDesc, &eye.srv))) {
            return false;
        }
        return true;
    }

    bool CreatePipeline() {
        static const char* shader = R"(
Texture2D leftImage : register(t0);
Texture2D rightImage : register(t1);
SamplerState samplerState : register(s0);
cbuffer Settings : register(b0) { uint eyeMode; float3 padding; float4 leftMap; float4 rightMap; };
struct Output { float4 position : SV_POSITION; float2 uv : TEXCOORD0; };
Output VS(uint id : SV_VertexID) {
  float2 p[3] = { float2(-1,-1), float2(-1,3), float2(3,-1) };
  float2 u[3] = { float2(0,1), float2(0,-1), float2(2,1) };
  Output o; o.position=float4(p[id],0,1); o.uv=u[id]; return o;
}
float3 LinearToSrgb(float3 sourceColor) {
  float3 low = sourceColor * 12.92;
  float3 high = 1.055 * pow(max(sourceColor, 0.0), 1.0 / 2.4) - 0.055;
  return lerp(high, low, step(sourceColor, 0.0031308));
}
float4 PS(Output input) : SV_TARGET {
  float3 left = leftImage.Sample(samplerState, input.uv).rgb;
  float3 right = rightImage.Sample(samplerState, input.uv).rgb;
  float3 color;
  if (eyeMode == 0) color = left;
  else if (eyeMode == 1) color = right;
  else if (eyeMode == 4) {
    color = input.uv.x < 0.5
      ? leftImage.Sample(samplerState, float2(input.uv.x * 2, input.uv.y)).rgb
      : rightImage.Sample(samplerState, float2((input.uv.x - 0.5) * 2, input.uv.y)).rgb;
  } else {
    float2 lu = input.uv * leftMap.xy + leftMap.zw;
    float2 ru = input.uv * rightMap.xy + rightMap.zw;
    bool lv = all(lu >= 0) && all(lu <= 1);
    bool rv = all(ru >= 0) && all(ru <= 1);
    left = leftImage.Sample(samplerState, saturate(lu)).rgb;
    right = rightImage.Sample(samplerState, saturate(ru)).rgb;
    float2 du = eyeMode == 2 ? lu : ru;
    float w = smoothstep(0.0, 0.015, min(du.x, 1.0-du.x));
    if (!lv && !rv) color = 0;
    else if (!lv) color = right;
    else if (!rv) color = left;
    else color = eyeMode == 2 ? lerp(right, left, w) : lerp(left, right, w);
  }
  return float4(LinearToSrgb(color), 1);
})";
        ComPtr<ID3DBlob> vs;
        ComPtr<ID3DBlob> ps;
        if (FAILED(D3DCompile(shader, std::strlen(shader), nullptr, nullptr, nullptr, "VS", "vs_4_0", 0, 0, &vs, nullptr))
            || FAILED(D3DCompile(shader, std::strlen(shader), nullptr, nullptr, nullptr, "PS", "ps_4_0", 0, 0, &ps, nullptr))
            || FAILED(m_device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &m_vertex))
            || FAILED(m_device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &m_pixel))) {
            return false;
        }
        D3D11_SAMPLER_DESC sampler = {};
        sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        D3D11_BUFFER_DESC constants = {};
        constants.ByteWidth = sizeof(ShaderSettings);
        constants.Usage = D3D11_USAGE_DEFAULT;
        constants.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        return SUCCEEDED(m_device->CreateSamplerState(&sampler, &m_sampler))
            && SUCCEEDED(m_device->CreateBuffer(&constants, nullptr, &m_settings));
    }

    bool CreateSwapChain() {
        if (!m_device || !m_window) return false;
        ComPtr<IDXGIDevice> dxgiDevice;
        ComPtr<IDXGIAdapter> adapter;
        ComPtr<IDXGIFactory> factory;
        if (FAILED(m_device.As(&dxgiDevice)) || FAILED(dxgiDevice->GetAdapter(&adapter))
            || FAILED(adapter->GetParent(IID_PPV_ARGS(&factory)))) {
            return false;
        }
        RECT rect = {};
        GetClientRect(m_window, &rect);
        DXGI_SWAP_CHAIN_DESC desc = {};
        desc.BufferCount = 1;
        desc.BufferDesc.Width = std::max(1L, rect.right - rect.left);
        desc.BufferDesc.Height = std::max(1L, rect.bottom - rect.top);
        desc.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.OutputWindow = m_window;
        desc.SampleDesc.Count = 1;
        desc.Windowed = TRUE;
        desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        if (FAILED(factory->CreateSwapChain(m_device.Get(), &desc, &m_swapChain))) return false;
        return CreateRenderTarget();
    }

    bool CreateRenderTarget() {
        ComPtr<ID3D11Texture2D> backBuffer;
        if (FAILED(m_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer)))
            || FAILED(m_device->CreateRenderTargetView(backBuffer.Get(), nullptr, &m_target))) {
            return false;
        }
        const float black[] = {0, 0, 0, 1};
        m_context->ClearRenderTargetView(m_target.Get(), black);
        m_swapChain->Present(0, 0);
        return true;
    }

    void ResizeSwapChain() {
        if (!m_swapChain || IsIconic(m_window)) return;
        RECT rect = {};
        GetClientRect(m_window, &rect);
        if (rect.right <= rect.left || rect.bottom <= rect.top) return;
        m_target.Reset();
        if (SUCCEEDED(m_swapChain->ResizeBuffers(
                0, static_cast<UINT>(rect.right - rect.left), static_cast<UINT>(rect.bottom - rect.top),
                DXGI_FORMAT_UNKNOWN, 0
            ))) {
            CreateRenderTarget();
        }
    }

    void Render() {
        if (!m_control || !m_target || IsIconic(m_window)) return;
        uint32_t selected = capture::SLOT_COUNT;
        LONG64 selectedFrame = m_lastFrame;
        for (uint32_t index = 0; index < capture::SLOT_COUNT; ++index) {
            const LONG64 frame = ReadAcquire64(&m_control->slots[index].frameId);
            if (frame > selectedFrame) {
                selected = index;
                selectedFrame = frame;
            }
        }
        if (selected == capture::SLOT_COUNT) return;
        auto& slot = m_slots[selected];
        if (slot.left.mutex->AcquireSync(1, 0) != S_OK) return;
        if (slot.right.mutex->AcquireSync(1, 0) != S_OK) {
            slot.left.mutex->ReleaseSync(1);
            return;
        }
        const LONG64 lockedFrame = ReadAcquire64(&m_control->slots[selected].frameId);
        if (lockedFrame == selectedFrame) {
            Draw(slot);
            m_context->Flush();
            m_lastFrame = lockedFrame;
        }
        slot.right.mutex->ReleaseSync(0);
        slot.left.mutex->ReleaseSync(0);
    }

    void Draw(SharedSlot& slot) {
        RECT rect = {};
        GetClientRect(m_window, &rect);
        const float clientWidth = static_cast<float>(std::max(1L, rect.right - rect.left));
        const float clientHeight = static_cast<float>(std::max(1L, rect.bottom - rect.top));
        m_projection->SetConvergence(m_convergence);
        const auto projection = m_projection->Poll();
        const bool mixed = m_eyeMode == EyeMode::BothLeftDominant || m_eyeMode == EyeMode::BothRightDominant;
        const float sourceWidth = (mixed && projection.valid ? projection.widthScale : 1.f) * static_cast<float>(
            m_metadata.eyeWidth * (m_eyeMode == EyeMode::SideBySide ? 2u : 1u)
        );
        const float sourceHeight = static_cast<float>(m_metadata.eyeHeight);
        float scale = 1.0f;
        if (m_scaleMode == ScaleMode::Fit) scale = std::min(clientWidth / sourceWidth, clientHeight / sourceHeight);
        if (m_scaleMode == ScaleMode::Fill) scale = std::max(clientWidth / sourceWidth, clientHeight / sourceHeight);
        const float width = sourceWidth * scale;
        const float height = sourceHeight * scale;
        D3D11_VIEWPORT viewport = {
            (clientWidth - width) / 2.0f, (clientHeight - height) / 2.0f, width, height, 0.0f, 1.0f
        };
        ID3D11RenderTargetView* target = m_target.Get();
        ID3D11ShaderResourceView* images[] = {slot.left.srv.Get(), slot.right.srv.Get()};
        ShaderSettings settings = {};
        settings.eyeMode = static_cast<uint32_t>(m_eyeMode);
        if (mixed && !projection.valid) settings.eyeMode = m_eyeMode == EyeMode::BothLeftDominant ? 0 : 1;
        stereo::EyeMap(projection, 0, settings.leftMap);
        stereo::EyeMap(projection, 1, settings.rightMap);
        m_context->OMSetRenderTargets(1, &target, nullptr);
        m_context->RSSetViewports(1, &viewport);
        m_context->UpdateSubresource(m_settings.Get(), 0, nullptr, &settings, 0, 0);
        m_context->VSSetShader(m_vertex.Get(), nullptr, 0);
        m_context->PSSetShader(m_pixel.Get(), nullptr, 0);
        m_context->PSSetShaderResources(0, 2, images);
        ID3D11SamplerState* sampler = m_sampler.Get();
        m_context->PSSetSamplers(0, 1, &sampler);
        ID3D11Buffer* constants = m_settings.Get();
        m_context->PSSetConstantBuffers(0, 1, &constants);
        m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_context->Draw(3, 0);
        ID3D11ShaderResourceView* empty[] = {nullptr, nullptr};
        m_context->PSSetShaderResources(0, 2, empty);
        m_swapChain->Present(0, 0);
    }

    void ReleaseVideo() {
        m_slots = {};
        m_target.Reset();
        m_swapChain.Reset();
        m_settings.Reset();
        m_sampler.Reset();
        m_pixel.Reset();
        m_vertex.Reset();
        m_context.Reset();
        m_device.Reset();
        if (m_control) UnmapViewOfFile(m_control);
        if (m_mapping) CloseHandle(m_mapping);
        m_control = nullptr;
        m_mapping = nullptr;
        m_lastFrame = 0;
    }

    static std::unique_ptr<stereo::RuntimeProjection> CreateProjection() {
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        std::wstring dll(path);
        dll = dll.substr(0, dll.find_last_of(L"\\/") + 1) + L"openvr_api.dll";
        return std::make_unique<stereo::RuntimeProjection>(dll.c_str());
    }
    std::unique_ptr<stereo::RuntimeProjection> m_projection = CreateProjection();
    HWND m_window = nullptr;
    HANDLE m_mapping = nullptr;
    capture::CaptureControlBlock* m_control = nullptr;
    capture::CaptureControlBlock m_metadata = {};
    ComPtr<ID3D11Device> m_device;
    ComPtr<ID3D11DeviceContext> m_context;
    std::array<SharedSlot, capture::SLOT_COUNT> m_slots;
    ComPtr<ID3D11VertexShader> m_vertex;
    ComPtr<ID3D11PixelShader> m_pixel;
    ComPtr<ID3D11SamplerState> m_sampler;
    ComPtr<ID3D11Buffer> m_settings;
    ComPtr<IDXGISwapChain> m_swapChain;
    ComPtr<ID3D11RenderTargetView> m_target;
    LONG64 m_lastFrame = 0;
    float m_convergence = 1.f;
    EyeMode m_eyeMode = EyeMode::Right;
    ScaleMode m_scaleMode = ScaleMode::Fit;
    bool m_fullscreen = false;
    LONG_PTR m_windowedStyle = 0;
    WINDOWPLACEMENT m_windowedPlacement = {sizeof(WINDOWPLACEMENT)};
    AudioPreview m_audio;
};

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    PreviewWindow preview;
    return preview.Run(instance);
}
