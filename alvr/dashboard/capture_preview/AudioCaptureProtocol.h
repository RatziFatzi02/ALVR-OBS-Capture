#pragma once

#include <cstddef>
#include <cstdint>
#include <windows.h>

namespace alvr_audio_capture {

static const wchar_t CONTROL_MAPPING_NAME[] = L"Local\\ALVR-Audio-Capture-v1";
static constexpr uint32_t CONTROL_MAGIC = 0x50434141; // "AACP"
static constexpr uint32_t CONTROL_VERSION = 1;
static constexpr uint32_t STREAM_COUNT = 2;
static constexpr uint32_t SLOT_COUNT = 128;
static constexpr uint32_t SLOT_PAYLOAD_BYTES = 16 * 1024;
static constexpr uint16_t SAMPLE_FORMAT_S16 = 1;
static constexpr uint32_t FLAG_DISCONTINUITY = 1;
static constexpr uint32_t STREAM_GAME_AUDIO = 1;
static constexpr uint32_t STREAM_MICROPHONE = 2;

constexpr uint64_t CatchUpSequence(uint64_t requested, uint64_t latest)
{
	return requested <= latest && latest - requested >= SLOT_COUNT ? latest : requested;
}

struct alignas(8) AudioCaptureSlot {
	volatile LONG64 sequence;
	uint64_t timestampQpc;
	uint32_t payloadBytes;
	uint32_t frames;
	uint32_t sampleRate;
	uint16_t channels;
	uint16_t sampleFormat;
	uint32_t flags;
	uint32_t reserved;
	uint8_t payload[SLOT_PAYLOAD_BYTES];
};

struct alignas(8) AudioCaptureRing {
	uint32_t streamKind;
	volatile LONG active;
	volatile LONG sampleRate;
	volatile LONG channels;
	volatile LONG64 formatGeneration;
	volatile LONG64 writeSequence;
	uint64_t reserved[4];
	AudioCaptureSlot slots[SLOT_COUNT];
};

struct alignas(8) AudioCaptureControlBlock {
	volatile LONG magic;
	uint32_t version;
	uint32_t structSize;
	uint32_t producerProcessId;
	uint64_t qpcFrequency;
	volatile LONG64 connectionGeneration;
	uint32_t streamCount;
	uint32_t slotCount;
	uint32_t slotPayloadBytes;
	uint32_t sampleFormat;
	uint64_t reserved[2];
	AudioCaptureRing rings[STREAM_COUNT];
};

static_assert(sizeof(AudioCaptureSlot) == 16424);
static_assert(sizeof(AudioCaptureRing) == 2102336);
static_assert(sizeof(AudioCaptureControlBlock) == 4204736);
static_assert(offsetof(AudioCaptureSlot, payload) == 40);
static_assert(offsetof(AudioCaptureRing, slots) == 64);
static_assert(offsetof(AudioCaptureControlBlock, rings) == 64);
static_assert(CatchUpSequence(1, SLOT_COUNT) == 1);
static_assert(CatchUpSequence(1, SLOT_COUNT + 1) == SLOT_COUNT + 1);
static_assert(CatchUpSequence(42, 43) == 42);

} // namespace alvr_audio_capture


