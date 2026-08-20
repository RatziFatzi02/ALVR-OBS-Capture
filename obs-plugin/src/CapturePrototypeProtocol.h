#pragma once

#include <stdint.h>
#include <windows.h>

// Private, local-only protocol shared with the ALVR v20.14.1 capture producer.
// This is intentionally a minimal copy of the producer-side header, not a
// public ALVR API.
namespace alvr_capture_prototype {

static const wchar_t CONTROL_MAPPING_NAME[] = L"Local\\ALVR-Capture-Prototype-v1";
static const uint32_t CONTROL_MAGIC = 0x50435641; // "AVCP"
static const uint32_t CONTROL_VERSION = 1;
static const uint32_t SLOT_COUNT = 3;

struct CaptureSlotMetadata {
    uint64_t leftSharedHandle;
    uint64_t rightSharedHandle;
    volatile LONG64 frameId;
};

struct CaptureControlBlock {
    uint32_t magic;
    uint32_t version;
    uint32_t structSize;
    uint32_t producerProcessId;

    LUID adapterLuid;
    uint32_t eyeWidth;
    uint32_t eyeHeight;
    uint32_t dxgiFormat;
    uint32_t slotCount;

    CaptureSlotMetadata slots[SLOT_COUNT];
};

} // namespace alvr_capture_prototype

static_assert(sizeof(alvr_capture_prototype::CaptureSlotMetadata) == 24);
static_assert(sizeof(alvr_capture_prototype::CaptureControlBlock) == 112);
