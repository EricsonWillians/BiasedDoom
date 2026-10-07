//-------------------------------------------------------------------------
//
// video_capture_audio.h
//
// A bounded, deterministic audio tap for the lossless video recorder. The
// normal sound renderer remains the authority for playback; this interface
// keeps enough decoded source data and stream output to build a synchronized
// PCM recording without ever reading back from an operating-system device.
//
//-------------------------------------------------------------------------

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

#include "zstring.h"
#include <zmusic.h>

struct FVideoRecordingAudioSource;
using FVideoRecordingAudioSourceRef = std::shared_ptr<const FVideoRecordingAudioSource>;

// Create a recorder-owned, decoded copy of an OpenAL source buffer. The
// implementation is capped globally and returns an empty reference when a
// source cannot be represented safely. Callers must retain the reference only
// for as long as their backend buffer exists.
FVideoRecordingAudioSourceRef I_CreateVideoRecordingAudioSource(const void *samples,
	size_t bytes, SampleType type, ChannelConfig channels, int sampleRate,
	uint32_t loopStart, uint32_t loopEnd);
uint64_t I_GetVideoRecordingAudioSourceBytes(const FVideoRecordingAudioSourceRef &source);

// The video recorder owns the lifetime of a capture session. A PNG sequence
// emits a WAV sidecar; AVI asks Render() for the same PCM frames and muxes them
// into its own audio stream.
void I_StartVideoRecordingAudio(const FString &outputStem, uint64_t startTimeNS);
void I_StopVideoRecordingAudio(uint64_t stopTimeNS);
void I_DiscardVideoRecordingAudio();
bool I_IsVideoRecordingAudioActive();
uint64_t I_GetVideoRecordingAudioEpoch();
uint32_t I_GetVideoRecordingAudioSampleRate();
uint16_t I_GetVideoRecordingAudioChannels();
uint16_t I_GetVideoRecordingAudioBitsPerSample();

// A sound effect is tied to one backend source ID. Starting the same ID again
// closes its prior segment so source reuse cannot make an old voice bleed into
// the next one. Gains are linear and already include the renderer's master
// category volume. Pan is -1 (left) through +1 (right).
void I_RecordVideoRecordingAudioEffectStart(uint64_t epoch, uintptr_t sourceID,
	const FVideoRecordingAudioSourceRef &source, float gain, float pan,
	float pitch, bool looping, float startOffsetSeconds);
void I_RecordVideoRecordingAudioEffectStop(uint64_t epoch, uintptr_t sourceID);

// Streaming music/custom audio arrives in the renderer's final stream format.
// streamStartNS is the logical presentation time of its first frame, not the
// later file-writer completion time. The caller can invoke this from the
// OpenAL stream worker; it is bounded and does no disk I/O.
void I_RecordVideoRecordingAudioStream(uint64_t epoch, uintptr_t streamID, const void *samples,
	size_t bytes, SampleType type, ChannelConfig channels, int sampleRate,
	float gain, uint64_t streamStartNS);

using FVideoRecordingAudioSink = bool (*)(const int16_t *samples, size_t frames, void *userdata);

// Mix one monotonic-clock range to 48 kHz stereo PCM. The supplied sink is
// called in small blocks so AVI muxing and WAV writing never need a take-sized
// allocation. Silence is valid output when the engine has no usable sound
// backend; that preserves the video timeline and container validity.
bool I_RenderVideoRecordingAudio(uint64_t startTimeNS, uint64_t endTimeNS,
	FVideoRecordingAudioSink sink, void *userdata, FString &error);

// Writes a synchronized PCM WAV sidecar for a completed PNG sequence. The
// `writtenPath` is empty when the recording was discarded or had no valid
// time range; video finalization must remain successful if this optional
// companion cannot be written.
bool I_WriteVideoRecordingAudioWav(FString &writtenPath, FString &error);
