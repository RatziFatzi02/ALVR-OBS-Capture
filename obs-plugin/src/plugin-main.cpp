#include <obs-module.h>
#include <obs-source.h>
#include <graphics/graphics.h>
#include <graphics/vec2.h>
#include <util/bmem.h>

#include "CapturePrototypeProtocol.h"
#include "plugin-support.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <limits>

OBS_DECLARE_MODULE()
OBS_MODULE_AUTHOR("RatziFatzi")

namespace {
using namespace alvr_capture_prototype;

constexpr uint32_t DEFAULT_EYE_WIDTH = 2144;
constexpr uint32_t DEFAULT_EYE_HEIGHT = 2144;
constexpr uint32_t DXGI_FORMAT_R8G8B8A8_UNORM_SRGB_VALUE = 29;
constexpr uint32_t MAX_CROP_PIXELS = 32768;
constexpr uint32_t MIN_ZOOM_PERCENT = 100;
constexpr uint32_t MAX_ZOOM_PERCENT = 400;

enum class EyeMode : uint32_t { Left, Right, BothLeftDominant, BothRightDominant, SideBySide };
enum class AspectCropMode : uint32_t { Original, Aspect16x9, Aspect16x10, Aspect4x3, Custom };

struct CropRect {
	uint32_t x = 0;
	uint32_t y = 0;
	uint32_t width = 0;
	uint32_t height = 0;
};

struct SharedHandles {
	uint64_t left = 0;
	uint64_t right = 0;
};

struct CaptureMetadata {
	uint32_t producerProcessId = 0;
	LUID adapterLuid = {};
	uint32_t eyeWidth = 0;
	uint32_t eyeHeight = 0;
	uint32_t dxgiFormat = 0;
	std::array<SharedHandles, SLOT_COUNT> handles = {};
};

struct CaptureSlotTextures {
	gs_texture_t *left = nullptr;
	gs_texture_t *right = nullptr;
};

struct AlvrCaptureData {
	obs_source_t *source = nullptr;
	HANDLE mapping = nullptr;
	CaptureControlBlock *control = nullptr;
	HANDLE producerProcess = nullptr;
	CaptureMetadata metadata = {};
	std::array<CaptureSlotTextures, SLOT_COUNT> textures = {};

	std::atomic<EyeMode> eyeMode = EyeMode::Right;
	std::atomic<AspectCropMode> aspectCropMode = AspectCropMode::Original;
	std::atomic<uint32_t> cropLeft = 0;
	std::atomic<uint32_t> cropRight = 0;
	std::atomic<uint32_t> cropTop = 0;
	std::atomic<uint32_t> cropBottom = 0;
	std::atomic<uint32_t> zoomPercent = MIN_ZOOM_PERCENT;
	std::atomic<uint64_t> settingsVersion = 1;

	// This is a one-frame GPU cache, not a queue. It provides the last complete
	// frame when a non-blocking keyed-mutex acquisition loses a producer handoff.
	gs_texrender_t *frameCache = nullptr;
	gs_effect_t *mixedEyeEffect = nullptr;
	uint32_t frameCacheWidth = 0;
	uint32_t frameCacheHeight = 0;
	uint64_t cachedFrameId = 0;
	uint64_t cachedSettingsVersion = 0;
	bool frameCacheValid = false;

	uint32_t eyeWidth = 0;
	uint32_t eyeHeight = 0;
	uint64_t lastRenderedFrameId = 0;
	uint64_t acquireTimeouts = 0;
	uint64_t renderedFrames = 0;
	ULONGLONG lastCounterLogMs = 0;
	ULONGLONG lastProbeLogMs = 0;
	bool connected = false;
};

bool SameLuid(const LUID &a, const LUID &b)
{
	return a.LowPart == b.LowPart && a.HighPart == b.HighPart;
}

bool SameMetadata(const CaptureMetadata &a, const CaptureMetadata &b)
{
	if (a.producerProcessId != b.producerProcessId || !SameLuid(a.adapterLuid, b.adapterLuid)
	    || a.eyeWidth != b.eyeWidth || a.eyeHeight != b.eyeHeight || a.dxgiFormat != b.dxgiFormat)
		return false;
	for (size_t i = 0; i < SLOT_COUNT; ++i) {
		if (a.handles[i].left != b.handles[i].left || a.handles[i].right != b.handles[i].right)
			return false;
	}
	return true;
}

void DestroyFrameCache(AlvrCaptureData *data)
{
	if (data->frameCache) {
		gs_texrender_destroy(data->frameCache);
		data->frameCache = nullptr;
	}
	data->frameCacheWidth = 0;
	data->frameCacheHeight = 0;
	data->cachedFrameId = 0;
	data->cachedSettingsVersion = 0;
	data->frameCacheValid = false;
}

void DestroyTextures(AlvrCaptureData *data)
{
	for (auto &slot : data->textures) {
		if (slot.left) {
			gs_texture_destroy(slot.left);
			slot.left = nullptr;
		}
		if (slot.right) {
			gs_texture_destroy(slot.right);
			slot.right = nullptr;
		}
	}
}

void DestroyOpenedTextures(std::array<CaptureSlotTextures, SLOT_COUNT> &textures)
{
	for (auto &slot : textures) {
		if (slot.left)
			gs_texture_destroy(slot.left);
		if (slot.right)
			gs_texture_destroy(slot.right);
		slot = {};
	}
}

void LogDisconnected(AlvrCaptureData *data, const char *reason)
{
	if (data->connected)
		blog(LOG_INFO, "[%s] disconnected: %s", PLUGIN_DISPLAY_NAME, reason);
	data->connected = false;
}

void CloseMapping(AlvrCaptureData *data)
{
	if (data->control) {
		UnmapViewOfFile(data->control);
		data->control = nullptr;
	}
	if (data->mapping) {
		CloseHandle(data->mapping);
		data->mapping = nullptr;
	}
	if (data->producerProcess) {
		CloseHandle(data->producerProcess);
		data->producerProcess = nullptr;
	}
}

void ResetConnection(AlvrCaptureData *data, const char *reason)
{
	LogDisconnected(data, reason);
	DestroyFrameCache(data);
	DestroyTextures(data);
	CloseMapping(data);
	data->metadata = {};
	data->eyeWidth = 0;
	data->eyeHeight = 0;
	data->lastRenderedFrameId = 0;
}

bool ReadMetadata(const CaptureControlBlock *control, CaptureMetadata &metadata)
{
	if (!control || control->magic != CONTROL_MAGIC || control->version != CONTROL_VERSION
	    || control->structSize != sizeof(CaptureControlBlock) || control->slotCount != SLOT_COUNT)
		return false;
	MemoryBarrier();
	metadata.producerProcessId = control->producerProcessId;
	metadata.adapterLuid = control->adapterLuid;
	metadata.eyeWidth = control->eyeWidth;
	metadata.eyeHeight = control->eyeHeight;
	metadata.dxgiFormat = control->dxgiFormat;
	for (size_t i = 0; i < SLOT_COUNT; ++i) {
		metadata.handles[i].left = control->slots[i].leftSharedHandle;
		metadata.handles[i].right = control->slots[i].rightSharedHandle;
	}
	MemoryBarrier();
	return control->magic == CONTROL_MAGIC;
}

bool OpenTexture(uint64_t handleValue, gs_texture_t **texture)
{
	if (!handleValue || handleValue > UINT32_MAX) {
		blog(LOG_WARNING, "[%s] invalid legacy shared handle value: 0x%llX", PLUGIN_DISPLAY_NAME,
		     static_cast<unsigned long long>(handleValue));
		return false;
	}
	*texture = gs_texture_open_shared(static_cast<uint32_t>(handleValue));
	return *texture != nullptr;
}

bool OpenTextures(AlvrCaptureData *data, const CaptureMetadata &metadata)
{
	if (metadata.dxgiFormat != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB_VALUE) {
		blog(LOG_WARNING, "[%s] unsupported DXGI format %u; V1 accepts only R8G8B8A8_UNORM_SRGB",
		     PLUGIN_DISPLAY_NAME, metadata.dxgiFormat);
		return false;
	}

	std::array<CaptureSlotTextures, SLOT_COUNT> opened = {};
	for (size_t i = 0; i < SLOT_COUNT; ++i) {
		if (!OpenTexture(metadata.handles[i].left, &opened[i].left)
		    || !OpenTexture(metadata.handles[i].right, &opened[i].right)) {
			DestroyOpenedTextures(opened);
			blog(LOG_WARNING, "[%s] could not open all six shared textures", PLUGIN_DISPLAY_NAME);
			return false;
		}
		if (gs_texture_get_width(opened[i].left) != metadata.eyeWidth
		    || gs_texture_get_height(opened[i].left) != metadata.eyeHeight
		    || gs_texture_get_width(opened[i].right) != metadata.eyeWidth
		    || gs_texture_get_height(opened[i].right) != metadata.eyeHeight) {
			DestroyOpenedTextures(opened);
			blog(LOG_WARNING, "[%s] shared texture dimensions do not match %ux%u", PLUGIN_DISPLAY_NAME,
			     metadata.eyeWidth, metadata.eyeHeight);
			return false;
		}
	}
	data->textures = opened;
	return true;
}

void MarkMappingInvalid(AlvrCaptureData *data)
{
	if (data->connected) {
		LogDisconnected(data, "control mapping is not currently published");
		DestroyFrameCache(data);
		DestroyTextures(data);
		data->lastRenderedFrameId = 0;
	}
}

bool EnsureMapping(AlvrCaptureData *data)
{
	if (!gs_shared_texture_available()) {
		const ULONGLONG now = GetTickCount64();
		if (now - data->lastProbeLogMs >= 5000) {
			blog(LOG_WARNING, "[%s] shared textures are unavailable in this OBS graphics backend", PLUGIN_DISPLAY_NAME);
			data->lastProbeLogMs = now;
		}
		return false;
	}
	if (data->producerProcess && WaitForSingleObject(data->producerProcess, 0) == WAIT_OBJECT_0)
		ResetConnection(data, "producer process exited");

	if (!data->mapping) {
		HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, CONTROL_MAPPING_NAME);
		if (!mapping) {
			const ULONGLONG now = GetTickCount64();
			if (now - data->lastProbeLogMs >= 5000) {
				blog(LOG_INFO, "[%s] disconnected: shared mapping is unavailable", PLUGIN_DISPLAY_NAME);
				data->lastProbeLogMs = now;
			}
			return false;
		}
		auto *control = static_cast<CaptureControlBlock *>(
			MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, sizeof(CaptureControlBlock)));
		if (!control) {
			CloseHandle(mapping);
			return false;
		}
		data->mapping = mapping;
		data->control = control;
	}

	CaptureMetadata metadata = {};
	if (!ReadMetadata(data->control, metadata) || !metadata.eyeWidth || !metadata.eyeHeight) {
		MarkMappingInvalid(data);
		return false;
	}
	const bool metadataChanged = !SameMetadata(data->metadata, metadata);
	if (!data->connected || metadataChanged) {
		DestroyFrameCache(data);
		DestroyTextures(data);
		if (!OpenTextures(data, metadata)) {
			data->connected = false;
			data->metadata = metadata;
			data->eyeWidth = metadata.eyeWidth;
			data->eyeHeight = metadata.eyeHeight;
			return false;
		}
		if (data->producerProcess)
			CloseHandle(data->producerProcess);
		data->producerProcess = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
		                                    metadata.producerProcessId);
		data->metadata = metadata;
		data->eyeWidth = metadata.eyeWidth;
		data->eyeHeight = metadata.eyeHeight;
		data->lastRenderedFrameId = 0;
		data->connected = true;
		blog(LOG_INFO, "[%s] connected: producerPid=%u eye=%ux%u format=%u", PLUGIN_DISPLAY_NAME,
		     metadata.producerProcessId, metadata.eyeWidth, metadata.eyeHeight, metadata.dxgiFormat);
	}
	return data->connected;
}

void LogCounters(AlvrCaptureData *data)
{
	const ULONGLONG now = GetTickCount64();
	if (!data->lastCounterLogMs || now - data->lastCounterLogMs >= 1000) {
		data->lastCounterLogMs = now;
		blog(LOG_DEBUG, "[%s] cached-frames=%llu acquire-timeouts=%llu last-frame=%llu", PLUGIN_DISPLAY_NAME,
		     static_cast<unsigned long long>(data->renderedFrames),
		     static_cast<unsigned long long>(data->acquireTimeouts),
		     static_cast<unsigned long long>(data->lastRenderedFrameId));
	}
}

uint32_t NativeEyeWidth(const AlvrCaptureData *data)
{
	return data->eyeWidth ? data->eyeWidth : DEFAULT_EYE_WIDTH;
}

uint32_t NativeEyeHeight(const AlvrCaptureData *data)
{
	return data->eyeHeight ? data->eyeHeight : DEFAULT_EYE_HEIGHT;
}

constexpr CropRect CenteredAspectCrop(uint32_t sourceWidth, uint32_t sourceHeight, uint32_t aspectWidth,
	                                  uint32_t aspectHeight)
{
	if (static_cast<uint64_t>(sourceWidth) * aspectHeight > static_cast<uint64_t>(sourceHeight) * aspectWidth) {
		const uint32_t width = std::max(1u, static_cast<uint32_t>(
			static_cast<uint64_t>(sourceHeight) * aspectWidth / aspectHeight));
		return {(sourceWidth - width) / 2, 0, width, sourceHeight};
	}
	const uint32_t height = std::max(1u, static_cast<uint32_t>(
		static_cast<uint64_t>(sourceWidth) * aspectHeight / aspectWidth));
	return {0, (sourceHeight - height) / 2, sourceWidth, height};
}

static_assert(CenteredAspectCrop(2144, 2144, 16, 9).height == 1206);
static_assert(CenteredAspectCrop(2144, 2144, 16, 10).height == 1340);
static_assert(CenteredAspectCrop(2144, 2144, 4, 3).height == 1608);

CropRect CustomCrop(const AlvrCaptureData *data, uint32_t width, uint32_t height)
{
	const uint32_t left = std::min(data->cropLeft.load(), width - 1);
	const uint32_t right = std::min(data->cropRight.load(), width - left - 1);
	const uint32_t top = std::min(data->cropTop.load(), height - 1);
	const uint32_t bottom = std::min(data->cropBottom.load(), height - top - 1);
	return {left, top, width - left - right, height - top - bottom};
}

CropRect EffectiveCrop(const AlvrCaptureData *data)
{
	const uint32_t width = NativeEyeWidth(data);
	const uint32_t height = NativeEyeHeight(data);
	switch (data->aspectCropMode.load()) {
	case AspectCropMode::Aspect16x9:
		return CenteredAspectCrop(width, height, 16, 9);
	case AspectCropMode::Aspect16x10:
		return CenteredAspectCrop(width, height, 16, 10);
	case AspectCropMode::Aspect4x3:
		return CenteredAspectCrop(width, height, 4, 3);
	case AspectCropMode::Custom:
		return CustomCrop(data, width, height);
	case AspectCropMode::Original:
	default:
		return {0, 0, width, height};
	}
}

CropRect ZoomedCrop(const AlvrCaptureData *data, const CropRect &crop)
{
	const uint32_t zoom = std::clamp(data->zoomPercent.load(), MIN_ZOOM_PERCENT, MAX_ZOOM_PERCENT);
	const uint32_t width = std::max(1u, static_cast<uint32_t>(static_cast<uint64_t>(crop.width) * 100 / zoom));
	const uint32_t height = std::max(1u, static_cast<uint32_t>(static_cast<uint64_t>(crop.height) * 100 / zoom));
	return {crop.x + (crop.width - width) / 2, crop.y + (crop.height - height) / 2, width, height};
}

bool IsMixedEyeMode(EyeMode mode)
{
	return mode == EyeMode::BothLeftDominant || mode == EyeMode::BothRightDominant;
}

uint32_t OutputWidth(const AlvrCaptureData *data, const CropRect &crop)
{
	if (data->eyeMode.load() == EyeMode::SideBySide)
		return static_cast<uint32_t>(std::min<uint64_t>(std::numeric_limits<uint32_t>::max(),
			static_cast<uint64_t>(crop.width) * 2));
	return crop.width;
}

void DrawCroppedTexture(gs_texture_t *texture, const CropRect &sample, uint32_t x, uint32_t outputWidth,
	                    uint32_t outputHeight)
{
	// Capture RGB is valid even where the compositor publishes alpha zero.
	// Opaque rendering preserves the established correct colour behaviour.
	gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_OPAQUE);
	const bool linearSrgb = gs_get_linear_srgb();
	const bool previousSrgb = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(linearSrgb);
	gs_eparam_t *image = gs_effect_get_param_by_name(effect, "image");
	if (linearSrgb)
		gs_effect_set_texture_srgb(image, texture);
	else
		gs_effect_set_texture(image, texture);
	gs_matrix_push();
	gs_matrix_translate3f(static_cast<float>(x), 0.0f, 0.0f);
	gs_matrix_scale3f(static_cast<float>(outputWidth) / sample.width,
	                  static_cast<float>(outputHeight) / sample.height, 1.0f);
	while (gs_effect_loop(effect, "Draw"))
		gs_draw_sprite_subregion(texture, 0, sample.x, sample.y, sample.width, sample.height);
	gs_matrix_pop();
	gs_enable_framebuffer_srgb(previousSrgb);
}

void DrawMixedEyes(AlvrCaptureData *data, gs_texture_t *left, gs_texture_t *right, const CropRect &sample,
	               uint32_t outputWidth, uint32_t outputHeight)
{
	const EyeMode mode = data->eyeMode.load();
	if (!data->mixedEyeEffect || !IsMixedEyeMode(mode)) {
		DrawCroppedTexture(mode == EyeMode::BothLeftDominant ? left : right, sample, 0, outputWidth, outputHeight);
		return;
	}
	const bool linearSrgb = gs_get_linear_srgb();
	const bool previousSrgb = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(linearSrgb);
	gs_eparam_t *leftImage = gs_effect_get_param_by_name(data->mixedEyeEffect, "left_image");
	gs_eparam_t *rightImage = gs_effect_get_param_by_name(data->mixedEyeEffect, "right_image");
	if (linearSrgb) {
		gs_effect_set_texture_srgb(leftImage, left);
		gs_effect_set_texture_srgb(rightImage, right);
	} else {
		gs_effect_set_texture(leftImage, left);
		gs_effect_set_texture(rightImage, right);
	}
	const vec2 uvMul = {static_cast<float>(sample.width) / NativeEyeWidth(data),
	                    static_cast<float>(sample.height) / NativeEyeHeight(data)};
	const vec2 uvAdd = {static_cast<float>(sample.x) / NativeEyeWidth(data),
	                    static_cast<float>(sample.y) / NativeEyeHeight(data)};
	gs_effect_set_vec2(gs_effect_get_param_by_name(data->mixedEyeEffect, "uv_mul"), &uvMul);
	gs_effect_set_vec2(gs_effect_get_param_by_name(data->mixedEyeEffect, "uv_add"), &uvAdd);
	gs_effect_set_float(gs_effect_get_param_by_name(data->mixedEyeEffect, "left_dominant"),
	                    mode == EyeMode::BothLeftDominant ? 1.0f : 0.0f);
	while (gs_effect_loop(data->mixedEyeEffect, "Draw"))
		gs_draw_sprite(nullptr, 0, outputWidth, outputHeight);
	gs_enable_framebuffer_srgb(previousSrgb);
}

bool EnsureFrameCache(AlvrCaptureData *data, uint32_t width, uint32_t height)
{
	if (data->frameCache && data->frameCacheWidth == width && data->frameCacheHeight == height)
		return true;
	DestroyFrameCache(data);
	data->frameCache = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	if (!data->frameCache) {
		blog(LOG_WARNING, "[%s] could not allocate frame cache", PLUGIN_DISPLAY_NAME);
		return false;
	}
	data->frameCacheWidth = width;
	data->frameCacheHeight = height;
	return true;
}

bool CacheLockedFrame(AlvrCaptureData *data, CaptureSlotTextures &slot, uint64_t frameId)
{
	const CropRect outputCrop = EffectiveCrop(data);
	const CropRect sample = ZoomedCrop(data, outputCrop);
	const uint32_t outputWidth = OutputWidth(data, outputCrop);
	const uint32_t outputHeight = outputCrop.height;
	if (!EnsureFrameCache(data, outputWidth, outputHeight))
		return false;
	gs_texrender_reset(data->frameCache);
	if (!gs_texrender_begin_with_color_space(data->frameCache, outputWidth, outputHeight, GS_CS_SRGB))
		return false;
	const bool previousSrgb = gs_framebuffer_srgb_enabled();
	gs_blend_state_push();
	gs_enable_framebuffer_srgb(true);
	gs_enable_blending(false);
	gs_ortho(0.0f, static_cast<float>(outputWidth), 0.0f, static_cast<float>(outputHeight), -100.0f, 100.0f);
	const EyeMode mode = data->eyeMode.load();
	if (mode == EyeMode::SideBySide) {
		DrawCroppedTexture(slot.left, sample, 0, outputCrop.width, outputCrop.height);
		DrawCroppedTexture(slot.right, sample, outputCrop.width, outputCrop.width, outputCrop.height);
	} else if (IsMixedEyeMode(mode)) {
		DrawMixedEyes(data, slot.left, slot.right, sample, outputWidth, outputHeight);
	} else {
		DrawCroppedTexture(mode == EyeMode::Left ? slot.left : slot.right, sample, 0, outputWidth, outputHeight);
	}
	gs_enable_framebuffer_srgb(previousSrgb);
	gs_blend_state_pop();
	gs_texrender_end(data->frameCache);
	data->cachedFrameId = frameId;
	data->cachedSettingsVersion = data->settingsVersion.load();
	data->frameCacheValid = true;
	data->lastRenderedFrameId = frameId;
	++data->renderedFrames;
	return true;
}

void UpdateFrameCache(AlvrCaptureData *data)
{
	if (!data->control || !data->connected)
		return;
	uint32_t candidateSlot = SLOT_COUNT;
	uint64_t candidateFrameId = 0;
	for (uint32_t i = 0; i < SLOT_COUNT; ++i) {
		const uint64_t frameId = static_cast<uint64_t>(ReadAcquire64(&data->control->slots[i].frameId));
		if (frameId > candidateFrameId) {
			candidateFrameId = frameId;
			candidateSlot = i;
		}
	}
	const CropRect outputCrop = EffectiveCrop(data);
	const uint32_t outputWidth = OutputWidth(data, outputCrop);
	const uint64_t settingsVersion = data->settingsVersion.load();
	const bool needsUpdate = !data->frameCacheValid || candidateFrameId > data->cachedFrameId
	                         || settingsVersion != data->cachedSettingsVersion || outputWidth != data->frameCacheWidth
	                         || outputCrop.height != data->frameCacheHeight;
	if (!needsUpdate || candidateSlot == SLOT_COUNT) {
		LogCounters(data);
		return;
	}
	auto &slot = data->textures[candidateSlot];
	if (!slot.left || !slot.right)
		return;
	if (gs_texture_acquire_sync(slot.left, 1, 0) != GS_SUCCESS) {
		++data->acquireTimeouts;
		LogCounters(data);
		return;
	}
	if (gs_texture_acquire_sync(slot.right, 1, 0) != GS_SUCCESS) {
		gs_texture_release_sync(slot.left, 1);
		++data->acquireTimeouts;
		LogCounters(data);
		return;
	}
	const uint64_t lockedFrameId =
		static_cast<uint64_t>(ReadAcquire64(&data->control->slots[candidateSlot].frameId));
	if (lockedFrameId == candidateFrameId)
		CacheLockedFrame(data, slot, lockedFrameId);
	// GPU work for the cache is already queued. ALVR receives key 0 without
	// waiting for OBS, and the previous cache remains if timeout zero lost a slot.
	gs_texture_release_sync(slot.right, 0);
	gs_texture_release_sync(slot.left, 0);
	LogCounters(data);
}

void DrawCachedFrame(AlvrCaptureData *data)
{
	if (!data->frameCacheValid || !data->frameCache)
		return;
	gs_texture_t *texture = gs_texrender_get_texture(data->frameCache);
	if (!texture)
		return;
	const CropRect outputCrop = EffectiveCrop(data);
	const uint32_t outputWidth = OutputWidth(data, outputCrop);
	const bool linearSrgb = gs_get_linear_srgb();
	const bool previousSrgb = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(linearSrgb);
	gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_OPAQUE);
	gs_eparam_t *image = gs_effect_get_param_by_name(effect, "image");
	if (linearSrgb)
		gs_effect_set_texture_srgb(image, texture);
	else
		gs_effect_set_texture(image, texture);
	while (gs_effect_loop(effect, "Draw"))
		gs_draw_sprite(texture, 0, outputWidth, outputCrop.height);
	gs_enable_framebuffer_srgb(previousSrgb);
}

void LoadMixedEyeEffect(AlvrCaptureData *data)
{
	char *path = obs_module_file("alvr-capture-mixed.effect");
	if (!path) {
		blog(LOG_WARNING, "[%s] could not locate mixed-eye effect", PLUGIN_DISPLAY_NAME);
		return;
	}
	obs_enter_graphics();
	data->mixedEyeEffect = gs_effect_create_from_file(path, nullptr);
	obs_leave_graphics();
	bfree(path);
	if (!data->mixedEyeEffect)
		blog(LOG_WARNING, "[%s] could not load mixed-eye effect; using dominant eye only", PLUGIN_DISPLAY_NAME);
}

const char *SourceName(void *) { return PLUGIN_DISPLAY_NAME; }

EyeMode ParseEyeMode(const char *value, const char *legacyDominantEye = nullptr)
{
	if (value && std::strcmp(value, "left") == 0)
		return EyeMode::Left;
	if (value && std::strcmp(value, "both_left_dominant") == 0)
		return EyeMode::BothLeftDominant;
	if (value && std::strcmp(value, "both_right_dominant") == 0)
		return EyeMode::BothRightDominant;
	// Migrate the V1 combination (eye=dominant + dominant_eye) to fused output.
	if (value && std::strcmp(value, "dominant") == 0)
		return legacyDominantEye && std::strcmp(legacyDominantEye, "left") == 0 ? EyeMode::BothLeftDominant
		                                                                         : EyeMode::BothRightDominant;
	if (value && std::strcmp(value, "side_by_side") == 0)
		return EyeMode::SideBySide;
	return EyeMode::Right;
}

AspectCropMode ParseAspectCropMode(const char *value)
{
	if (value && std::strcmp(value, "16_9") == 0)
		return AspectCropMode::Aspect16x9;
	if (value && std::strcmp(value, "16_10") == 0)
		return AspectCropMode::Aspect16x10;
	if (value && std::strcmp(value, "4_3") == 0)
		return AspectCropMode::Aspect4x3;
	if (value && std::strcmp(value, "custom") == 0)
		return AspectCropMode::Custom;
	return AspectCropMode::Original;
}

uint32_t ParseCropPixels(obs_data_t *settings, const char *name)
{
	return static_cast<uint32_t>(std::clamp<int64_t>(obs_data_get_int(settings, name), 0, MAX_CROP_PIXELS));
}

void ApplySettings(AlvrCaptureData *data, obs_data_t *settings)
{
	data->eyeMode.store(ParseEyeMode(obs_data_get_string(settings, "eye"), obs_data_get_string(settings, "dominant_eye")));
	data->aspectCropMode.store(ParseAspectCropMode(obs_data_get_string(settings, "aspect_crop")));
	data->cropLeft.store(ParseCropPixels(settings, "crop_left"));
	data->cropRight.store(ParseCropPixels(settings, "crop_right"));
	data->cropTop.store(ParseCropPixels(settings, "crop_top"));
	data->cropBottom.store(ParseCropPixels(settings, "crop_bottom"));
	data->zoomPercent.store(static_cast<uint32_t>(std::clamp<int64_t>(obs_data_get_int(settings, "zoom_percent"),
		MIN_ZOOM_PERCENT, MAX_ZOOM_PERCENT)));
	data->settingsVersion.fetch_add(1);
}

void *SourceCreate(obs_data_t *settings, obs_source_t *source)
{
	auto *data = new AlvrCaptureData();
	data->source = source;
	ApplySettings(data, settings);
	LoadMixedEyeEffect(data);
	return data;
}

void SourceDestroy(void *rawData)
{
	auto *data = static_cast<AlvrCaptureData *>(rawData);
	if (!data)
		return;
	obs_enter_graphics();
	ResetConnection(data, "source destroyed");
	if (data->mixedEyeEffect)
		gs_effect_destroy(data->mixedEyeEffect);
	obs_leave_graphics();
	delete data;
}

void SourceUpdate(void *rawData, obs_data_t *settings)
{
	ApplySettings(static_cast<AlvrCaptureData *>(rawData), settings);
}

uint32_t SourceWidth(void *rawData)
{
	auto *data = static_cast<const AlvrCaptureData *>(rawData);
	return OutputWidth(data, EffectiveCrop(data));
}

uint32_t SourceHeight(void *rawData)
{
	return EffectiveCrop(static_cast<const AlvrCaptureData *>(rawData)).height;
}

void SourceRender(void *rawData, gs_effect_t *)
{
	auto *data = static_cast<AlvrCaptureData *>(rawData);
	if (EnsureMapping(data))
		UpdateFrameCache(data);
	DrawCachedFrame(data);
}

enum gs_color_space SourceColorSpace(void *, size_t, const enum gs_color_space *) { return GS_CS_SRGB; }

void SetPropertyVisibility(obs_properties_t *properties, obs_data_t *settings)
{
	const bool custom = ParseAspectCropMode(obs_data_get_string(settings, "aspect_crop")) == AspectCropMode::Custom;
	obs_property_set_visible(obs_properties_get(properties, "crop_left"), custom);
	obs_property_set_visible(obs_properties_get(properties, "crop_right"), custom);
	obs_property_set_visible(obs_properties_get(properties, "crop_top"), custom);
	obs_property_set_visible(obs_properties_get(properties, "crop_bottom"), custom);
}

bool PropertyLayoutModified(obs_properties_t *properties, obs_property_t *, obs_data_t *settings)
{
	SetPropertyVisibility(properties, settings);
	return true;
}

obs_properties_t *SourceProperties(void *rawData)
{
	auto *data = static_cast<AlvrCaptureData *>(rawData);
	obs_properties_t *properties = obs_properties_create();
	obs_property_t *eye = obs_properties_add_list(properties, "eye", "Eye", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(eye, "Left Eye", "left");
	obs_property_list_add_string(eye, "Right Eye", "right");
	obs_property_list_add_string(eye, "Both Eyes - Left Dominant", "both_left_dominant");
	obs_property_list_add_string(eye, "Both Eyes - Right Dominant", "both_right_dominant");
	obs_property_list_add_string(eye, "Side-by-Side", "side_by_side");
	obs_property_t *aspect = obs_properties_add_list(properties, "aspect_crop", "Aspect / Crop Mode",
		OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(aspect, "Original", "original");
	obs_property_list_add_string(aspect, "16:9", "16_9");
	obs_property_list_add_string(aspect, "16:10", "16_10");
	obs_property_list_add_string(aspect, "4:3", "4_3");
	obs_property_list_add_string(aspect, "Custom", "custom");
	obs_property_t *zoom = obs_properties_add_int(properties, "zoom_percent", "Zoom", MIN_ZOOM_PERCENT,
		MAX_ZOOM_PERCENT, 1);
	obs_property_int_set_suffix(zoom, " %");
	obs_property_t *cropLeft = obs_properties_add_int(properties, "crop_left", "Left", 0, MAX_CROP_PIXELS, 1);
	obs_property_t *cropRight = obs_properties_add_int(properties, "crop_right", "Right", 0, MAX_CROP_PIXELS, 1);
	obs_property_t *cropTop = obs_properties_add_int(properties, "crop_top", "Top", 0, MAX_CROP_PIXELS, 1);
	obs_property_t *cropBottom = obs_properties_add_int(properties, "crop_bottom", "Bottom", 0, MAX_CROP_PIXELS, 1);
	obs_property_int_set_suffix(cropLeft, " px");
	obs_property_int_set_suffix(cropRight, " px");
	obs_property_int_set_suffix(cropTop, " px");
	obs_property_int_set_suffix(cropBottom, " px");
	obs_property_set_modified_callback(aspect, PropertyLayoutModified);
	obs_data_t *settings = data ? obs_source_get_settings(data->source) : obs_data_create();
	SetPropertyVisibility(properties, settings);
	obs_data_release(settings);
	return properties;
}

void SourceDefaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "eye", "right");
	obs_data_set_default_string(settings, "dominant_eye", "right");
	obs_data_set_default_string(settings, "aspect_crop", "original");
	obs_data_set_default_int(settings, "zoom_percent", MIN_ZOOM_PERCENT);
	obs_data_set_default_int(settings, "crop_left", 0);
	obs_data_set_default_int(settings, "crop_right", 0);
	obs_data_set_default_int(settings, "crop_top", 0);
	obs_data_set_default_int(settings, "crop_bottom", 0);
}

struct obs_source_info SourceInfo = {};
} // namespace

bool obs_module_load(void)
{
	SourceInfo.id = PLUGIN_NAME;
	SourceInfo.type = OBS_SOURCE_TYPE_INPUT;
	SourceInfo.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_SRGB;
	SourceInfo.get_name = SourceName;
	SourceInfo.create = SourceCreate;
	SourceInfo.destroy = SourceDestroy;
	SourceInfo.update = SourceUpdate;
	SourceInfo.get_width = SourceWidth;
	SourceInfo.get_height = SourceHeight;
	SourceInfo.get_properties = SourceProperties;
	SourceInfo.get_defaults = SourceDefaults;
	SourceInfo.video_render = SourceRender;
	SourceInfo.video_get_color_space = SourceColorSpace;
	obs_register_source(&SourceInfo);
	blog(LOG_INFO, "[%s] plugin loaded (version %s)", PLUGIN_DISPLAY_NAME, PLUGIN_VERSION);
	return true;
}

void obs_module_unload(void) { blog(LOG_INFO, "[%s] plugin unloaded", PLUGIN_DISPLAY_NAME); }

const char *obs_module_description(void)
{
	return "ALVR shared-texture capture source with non-blocking frame cache";
}
