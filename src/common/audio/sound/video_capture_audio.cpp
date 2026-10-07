//-------------------------------------------------------------------------
//
// video_capture_audio.cpp
//
//-------------------------------------------------------------------------

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <condition_variable>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <set>
#include <vector>

#include "video_capture_audio.h"

#include "cmdlib.h"
#include "files.h"
#include "printf.h"

namespace
{
	constexpr uint32_t VIDEO_AUDIO_RATE = 48000;
	constexpr uint32_t VIDEO_AUDIO_CHANNELS = 2;
	constexpr uint64_t VIDEO_AUDIO_FRAME_BYTES = VIDEO_AUDIO_CHANNELS * sizeof(int16_t);
	constexpr uint64_t VIDEO_AUDIO_MAX_SOURCE_BYTES = 16ull * 1024ull * 1024ull;
	constexpr uint64_t VIDEO_AUDIO_MAX_SOURCE_CACHE_BYTES = 96ull * 1024ull * 1024ull;
	constexpr uint64_t VIDEO_AUDIO_MAX_STREAM_BYTES = 256ull * 1024ull * 1024ull;
	constexpr uint64_t VIDEO_AUDIO_MAX_STREAM_BLOCK_BYTES = 1024ull * 1024ull;
	constexpr size_t VIDEO_AUDIO_RENDER_BLOCK_FRAMES = 4096;
	constexpr size_t VIDEO_AUDIO_MAX_EFFECTS = 65536;

	static uint64_t AudioClockNS()
	{
		using namespace std::chrono;
		return (uint64_t)duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
	}

	static uint64_t TimeToFrameFloor(uint64_t timeNS, uint64_t startNS)
	{
		if (timeNS <= startNS)
		{
			return 0;
		}
		const uint64_t elapsed = timeNS - startNS;
		if (elapsed > std::numeric_limits<uint64_t>::max() / VIDEO_AUDIO_RATE)
		{
			return std::numeric_limits<uint64_t>::max();
		}
		return elapsed * VIDEO_AUDIO_RATE / 1000000000ull;
	}

	static uint64_t FramesToNS(uint64_t frames, uint32_t rate)
	{
		if (rate == 0 || frames > std::numeric_limits<uint64_t>::max() / 1000000000ull)
		{
			return std::numeric_limits<uint64_t>::max();
		}
		return frames * 1000000000ull / rate;
	}

	static uint64_t ResampledFrameCount(uint64_t sourceFrames, uint32_t sourceRate)
	{
		if (sourceRate == 0 || sourceFrames == 0)
		{
			return 0;
		}
		if (sourceFrames > (std::numeric_limits<uint64_t>::max() - (uint64_t)sourceRate + 1) / VIDEO_AUDIO_RATE)
		{
			return std::numeric_limits<uint64_t>::max();
		}
		return (sourceFrames * VIDEO_AUDIO_RATE + sourceRate - 1) / sourceRate;
	}

	static int16_t ClampSample(float value)
	{
		if (value >= 1.0f) return 32767;
		if (value <= -1.0f) return -32768;
		return (int16_t)std::lrintf(value * 32767.0f);
	}

	static float DecodeSample(const void *samples, size_t index, SampleType type)
	{
		switch (type)
		{
		case SampleType_UInt8:
			return ((int)((const uint8_t *)samples)[index] - 128) * (1.0f / 128.0f);
		case SampleType_Int16:
			return ((const int16_t *)samples)[index] * (1.0f / 32768.0f);
		case SampleType_Float32:
			return ((const float *)samples)[index];
		default:
			return 0.0f;
		}
	}

	static size_t SampleSize(SampleType type)
	{
		switch (type)
		{
		case SampleType_UInt8: return 1;
		case SampleType_Int16: return 2;
		case SampleType_Float32: return 4;
		default: return 0;
		}
	}

	static bool GetChannelCount(ChannelConfig channels, uint32_t &count)
	{
		switch (channels)
		{
		case ChannelConfig_Mono:
			count = 1;
			return true;
		case ChannelConfig_Stereo:
			count = 2;
			return true;
		default:
			count = 0;
			return false;
		}
	}

	static std::atomic<uint64_t> SourceCacheBytes{ 0 };
	static std::atomic<bool> SourceCacheLimitReported{ false };
	static std::atomic<bool> SourceCacheAllocationReported{ false };

	enum EStreamFailure
	{
		STREAM_FAILURE_NONE,
		STREAM_FAILURE_CAPACITY,
		STREAM_FAILURE_ALLOCATION,
	};

	static const char *StreamFailureMessage(EStreamFailure failure)
	{
		switch (failure)
		{
		case STREAM_FAILURE_CAPACITY:
			return "the bounded stream-audio cache overflowed; refusing to emit a silently truncated track";
		case STREAM_FAILURE_ALLOCATION:
			return "the audio stream recorder ran out of memory; refusing to emit an incomplete track";
		default:
			return "the audio stream recorder could not complete the track";
		}
	}

	struct FVideoRecordingAudioSourceImpl
	{
		std::vector<int16_t> Samples;
		uint32_t SampleRate = 0;
		uint32_t Channels = 0;
		uint32_t LoopStart = 0;
		uint32_t LoopEnd = 0;
		uint64_t StorageBytes = 0;

		~FVideoRecordingAudioSourceImpl()
		{
			SourceCacheBytes.fetch_sub(StorageBytes, std::memory_order_relaxed);
		}
	};

	struct FAudioEffect
	{
		uintptr_t SourceID = 0;
		FVideoRecordingAudioSourceRef Source;
		uint64_t StartNS = 0;
		uint64_t StopNS = 0;
		float Gain = 1.0f;
		float Pan = 0.0f;
		float Pitch = 1.0f;
		float StartOffsetSeconds = 0.0f;
		bool Looping = false;
	};

	struct FAudioStreamBlock
	{
		uintptr_t StreamID = 0;
		uint64_t StartNS = 0;
		std::shared_ptr<const std::vector<int16_t>> Samples;
		uint32_t Frames = 0;
		uint32_t SampleRate = 0;
	};

	struct FAudioSnapshot
	{
		uint64_t TakeStartNS = 0;
		uint64_t TakeStopNS = 0;
		FString OutputStem;
		EStreamFailure StreamFailure = STREAM_FAILURE_NONE;
		std::vector<FAudioEffect> Effects;
		std::vector<FAudioStreamBlock> Streams;
	};

	class FVideoRecordingAudioTap
	{
	public:
		void Start(const FString &outputStem, uint64_t startTimeNS)
		{
			std::lock_guard<std::mutex> lock(Mutex);
			OutputStem = outputStem;
			TakeStartNS = startTimeNS;
			TakeStopNS = 0;
			Effects.clear();
			Streams.clear();
			StreamBytes = 0;
			StreamFailure = STREAM_FAILURE_NONE;
			StreamReservationNext = 0;
			StreamPublicationsInFlight.clear();
			EffectLimitReported = false;
			// Publish the new generation before making the take visible. A callback
			// can then never observe Active for this take with the prior epoch.
			Epoch.fetch_add(1, std::memory_order_acq_rel);
			Active.store(true, std::memory_order_release);
			StreamPublicationWake.notify_all();
		}

		void Stop(uint64_t stopTimeNS)
		{
			std::lock_guard<std::mutex> lock(Mutex);
			if (!Active.exchange(false, std::memory_order_acq_rel))
			{
				return;
			}
			TakeStopNS = stopTimeNS >= TakeStartNS ? stopTimeNS : TakeStartNS;
			for (auto &effect : Effects)
			{
				if (effect.StopNS == 0)
				{
					effect.StopNS = TakeStopNS;
				}
			}
			// Existing stream conversions retain their reservation and may still
			// publish into this stopped epoch. Snapshot() waits for precisely those
			// reservations before it commits the final audio range.
			StreamPublicationWake.notify_all();
		}

		void Discard()
		{
			std::lock_guard<std::mutex> lock(Mutex);
			Active.store(false, std::memory_order_release);
			OutputStem = "";
			TakeStartNS = 0;
			TakeStopNS = 0;
			Effects.clear();
			Streams.clear();
			StreamBytes = 0;
			StreamFailure = STREAM_FAILURE_NONE;
			StreamReservationNext = 0;
			StreamPublicationsInFlight.clear();
			Epoch.fetch_add(1, std::memory_order_acq_rel);
			// Any producer from the old epoch will reject itself on publication.
			// Wake a writer snapshot rather than leaving it waiting for a reservation
			// that was deliberately invalidated with this session.
			StreamPublicationWake.notify_all();
		}

		bool IsActive() const
		{
			return Active.load(std::memory_order_acquire);
		}

		uint64_t GetEpoch() const
		{
			return Epoch.load(std::memory_order_acquire);
		}

		void RecordEffectStart(uint64_t epoch, uintptr_t sourceID, const FVideoRecordingAudioSourceRef &source,
			float gain, float pan, float pitch, bool looping, float startOffsetSeconds)
		{
			if (source == nullptr || !Active.load(std::memory_order_acquire))
			{
				return;
			}
			const uint64_t now = AudioClockNS();
			std::lock_guard<std::mutex> lock(Mutex);
			if (!Active.load(std::memory_order_relaxed) || Epoch.load(std::memory_order_relaxed) != epoch || Effects.size() >= VIDEO_AUDIO_MAX_EFFECTS)
			{
				if (!EffectLimitReported)
				{
					Printf(TEXTCOLOR_YELLOW "Video audio reached its voice-event cap; later effects will be omitted.\n");
					EffectLimitReported = true;
				}
				return;
			}
			for (auto it = Effects.rbegin(); it != Effects.rend(); ++it)
			{
				if (it->SourceID == sourceID && it->StopNS == 0)
				{
					it->StopNS = now;
					break;
				}
			}
			FAudioEffect effect;
			effect.SourceID = sourceID;
			effect.Source = source;
			effect.StartNS = now;
			effect.Gain = std::max(0.0f, gain);
			effect.Pan = std::max(-1.0f, std::min(1.0f, pan));
			effect.Pitch = std::max(0.0001f, pitch);
			effect.Looping = looping;
			effect.StartOffsetSeconds = std::max(0.0f, startOffsetSeconds);
			Effects.push_back(std::move(effect));
		}

		void RecordEffectStop(uint64_t epoch, uintptr_t sourceID)
		{
			if (!Active.load(std::memory_order_acquire))
			{
				return;
			}
			const uint64_t now = AudioClockNS();
			std::lock_guard<std::mutex> lock(Mutex);
			if (!Active.load(std::memory_order_relaxed) || Epoch.load(std::memory_order_relaxed) != epoch)
			{
				return;
			}
			for (auto it = Effects.rbegin(); it != Effects.rend(); ++it)
			{
				if (it->SourceID == sourceID && it->StopNS == 0)
				{
					it->StopNS = now;
					return;
				}
			}
		}

		void RecordStream(uint64_t epoch, uintptr_t streamID, const void *samples, size_t bytes,
			SampleType type, ChannelConfig channels, int sampleRate, float gain, uint64_t startNS)
		{
			if (samples == nullptr || sampleRate <= 0 || !Active.load(std::memory_order_acquire))
			{
				return;
			}
			uint32_t sourceChannels = 0;
			const size_t sampleSize = SampleSize(type);
			if (!GetChannelCount(channels, sourceChannels) || sampleSize == 0 || bytes / sampleSize / sourceChannels == 0)
			{
				return;
			}
			const size_t frames = bytes / (sampleSize * sourceChannels);
			if (frames > std::numeric_limits<uint32_t>::max() ||
				frames > std::numeric_limits<size_t>::max() / VIDEO_AUDIO_CHANNELS)
			{
				return;
			}
			const uint64_t storageBytes = (uint64_t)frames * VIDEO_AUDIO_FRAME_BYTES;
			if (storageBytes > VIDEO_AUDIO_MAX_STREAM_BLOCK_BYTES || storageBytes > VIDEO_AUDIO_MAX_STREAM_BYTES)
			{
				MarkStreamFailure(epoch, STREAM_FAILURE_CAPACITY);
				return;
			}

			// Reserve before doing the potentially slow sample conversion. A writer
			// snapshot captures the current reservation watermark and waits only for
			// reservations at or below it, so an already-running stream callback is
			// included in an AVI rollover/final take without making later callbacks
			// block that snapshot.
			uint64_t reservation = 0;
			{
				std::lock_guard<std::mutex> lock(Mutex);
				if (!ReserveStreamPublicationLocked(epoch, reservation))
				{
					return;
				}
			}

			// Convert/copy the bounded block before taking the tap mutex. The audio
			// worker only holds the mutex for a short vector publication; writer
			// snapshots happen after stop (or between AVI parts) and never perform
			// disk I/O while holding it. This avoids throwing away a whole take for
			// a harmless effect-event collision.
			std::shared_ptr<std::vector<int16_t>> converted;
			try
			{
				converted = std::make_shared<std::vector<int16_t>>();
				converted->resize(frames * VIDEO_AUDIO_CHANNELS);
				for (size_t frame = 0; frame < frames; ++frame)
				{
					const float left = DecodeSample(samples, frame * sourceChannels, type) * gain;
					const float right = sourceChannels == 2 ? DecodeSample(samples, frame * sourceChannels + 1, type) * gain : left;
					(*converted)[frame * 2] = ClampSample(left);
					(*converted)[frame * 2 + 1] = ClampSample(right);
				}
			}
			catch (const std::bad_alloc &)
			{
				// This runs on the stream worker. Mark the take incomplete, but
				// never let recorder memory pressure terminate live playback.
				std::lock_guard<std::mutex> lock(Mutex);
				if (Epoch.load(std::memory_order_relaxed) == epoch)
				{
					MarkStreamFailureLocked(epoch, STREAM_FAILURE_ALLOCATION);
					ResolveStreamPublicationLocked(epoch, reservation);
				}
				return;
			}

			std::lock_guard<std::mutex> lock(Mutex);
			if (Epoch.load(std::memory_order_relaxed) != epoch)
			{
				// Start()/Discard() changed the generation and invalidated its old
				// reservations. Never leak a stale callback into the new take.
				return;
			}
			if (StreamBytes > VIDEO_AUDIO_MAX_STREAM_BYTES - storageBytes)
			{
				MarkStreamFailureLocked(epoch, STREAM_FAILURE_CAPACITY);
				ResolveStreamPublicationLocked(epoch, reservation);
				return;
			}
			try
			{
				Streams.push_back({ streamID, startNS, std::move(converted), (uint32_t)frames, (uint32_t)sampleRate });
			}
			catch (const std::bad_alloc &)
			{
				MarkStreamFailureLocked(epoch, STREAM_FAILURE_ALLOCATION);
				ResolveStreamPublicationLocked(epoch, reservation);
				return;
			}
			StreamBytes += storageBytes;
			// Active may now be false because Stop() occurred while the conversion
			// was in flight. The reservation proves this block belongs to the same
			// take, so publish it before releasing a final snapshot barrier.
			ResolveStreamPublicationLocked(epoch, reservation);
		}

		bool Snapshot(FAudioSnapshot &snapshot, FString &error, uint64_t requestedEndNS = 0) const
		{
			std::unique_lock<std::mutex> lock(Mutex);
			const uint64_t epoch = Epoch.load(std::memory_order_relaxed);
			const uint64_t watermark = StreamReservationNext;
			StreamPublicationWake.wait(lock, [this, epoch, watermark]()
			{
				if (Epoch.load(std::memory_order_relaxed) != epoch)
				{
					return true;
				}
				return StreamPublicationsInFlight.empty() ||
					*StreamPublicationsInFlight.begin() > watermark;
			});
			if (Epoch.load(std::memory_order_relaxed) != epoch)
			{
				error = "the audio capture session changed while finalizing";
				return false;
			}
			if (TakeStartNS == 0)
			{
				error = "the audio capture has no timeline";
				return false;
			}
			const uint64_t snapshotEndNS = TakeStopNS != 0 ? TakeStopNS : requestedEndNS;
			if (snapshotEndNS <= TakeStartNS || (TakeStopNS == 0 && requestedEndNS == 0))
			{
				error = "the audio capture has no completed timeline";
				return false;
			}
			snapshot.TakeStartNS = TakeStartNS;
			snapshot.TakeStopNS = snapshotEndNS;
			snapshot.OutputStem = OutputStem;
			snapshot.StreamFailure = StreamFailure;
			snapshot.Effects = Effects;
			snapshot.Streams = Streams;
			return true;
		}

		private:
			bool ReserveStreamPublicationLocked(uint64_t epoch, uint64_t &reservation)
			{
				if (!Active.load(std::memory_order_relaxed) || Epoch.load(std::memory_order_relaxed) != epoch ||
					StreamReservationNext == std::numeric_limits<uint64_t>::max())
				{
					return false;
				}
				const uint64_t candidate = ++StreamReservationNext;
				try
				{
					StreamPublicationsInFlight.insert(candidate);
				}
				catch (const std::bad_alloc &)
				{
					MarkStreamFailureLocked(epoch, STREAM_FAILURE_ALLOCATION);
					return false;
				}
				reservation = candidate;
				return true;
			}

			void ResolveStreamPublicationLocked(uint64_t epoch, uint64_t reservation)
			{
				if (Epoch.load(std::memory_order_relaxed) != epoch)
				{
					return;
				}
				auto publication = StreamPublicationsInFlight.find(reservation);
				if (publication != StreamPublicationsInFlight.end())
				{
					StreamPublicationsInFlight.erase(publication);
					StreamPublicationWake.notify_all();
				}
			}

			void MarkStreamFailure(uint64_t epoch, EStreamFailure failure)
		{
			std::lock_guard<std::mutex> lock(Mutex);
			MarkStreamFailureLocked(epoch, failure);
		}

		void MarkStreamFailureLocked(uint64_t epoch, EStreamFailure failure)
		{
			if (Epoch.load(std::memory_order_relaxed) == epoch && StreamFailure == STREAM_FAILURE_NONE)
			{
				StreamFailure = failure;
			}
		}

		mutable std::mutex Mutex;
		mutable std::condition_variable StreamPublicationWake;
		std::atomic<bool> Active{ false };
		std::atomic<uint64_t> Epoch{ 0 };
		FString OutputStem;
		uint64_t TakeStartNS = 0;
		uint64_t TakeStopNS = 0;
		std::vector<FAudioEffect> Effects;
		std::vector<FAudioStreamBlock> Streams;
		uint64_t StreamBytes = 0;
		uint64_t StreamReservationNext = 0;
		std::set<uint64_t> StreamPublicationsInFlight;
		EStreamFailure StreamFailure = STREAM_FAILURE_NONE;
		bool EffectLimitReported = false;
	};

	// The recorder is a separate translation-unit static whose destructor uses
	// this tap as a shutdown backstop. C++ provides no destruction ordering
	// guarantee between those translation units, so a normal static tap could be
	// destroyed before the recorder and leave that backstop calling through a
	// dead mutex/CV/vector. The tap contains only bounded, session-owned state;
	// intentionally retaining this tiny control object until process exit makes
	// every late shutdown call safe. Active session storage is still released by
	// I_DiscardVideoRecordingAudio during normal recorder finalization.
	static FVideoRecordingAudioTap &GetVideoRecordingAudioTap()
	{
		static FVideoRecordingAudioTap *tap = new FVideoRecordingAudioTap;
		return *tap;
	}

	static bool WriteAll(FileWriter *file, const void *data, size_t bytes)
	{
		return file != nullptr && file->Write(data, bytes) == bytes;
	}

	static bool WriteU16(FileWriter *file, uint16_t value)
	{
		const uint8_t bytes[2] = { uint8_t(value), uint8_t(value >> 8) };
		return WriteAll(file, bytes, sizeof(bytes));
	}

	static bool WriteU32(FileWriter *file, uint32_t value)
	{
		const uint8_t bytes[4] = { uint8_t(value), uint8_t(value >> 8), uint8_t(value >> 16), uint8_t(value >> 24) };
		return WriteAll(file, bytes, sizeof(bytes));
	}

	static uint64_t EventStartFrame(const FAudioEffect &effect, const FAudioSnapshot &snapshot)
	{
		return TimeToFrameFloor(effect.StartNS, snapshot.TakeStartNS);
	}

	static uint64_t EventEndFrame(const FAudioEffect &effect, const FAudioSnapshot &snapshot)
	{
		const uint64_t takeEnd = TimeToFrameFloor(snapshot.TakeStopNS, snapshot.TakeStartNS);
		uint64_t end = effect.StopNS != 0 ? TimeToFrameFloor(effect.StopNS, snapshot.TakeStartNS) : takeEnd;
		return std::min(end, takeEnd);
	}

	static void MixEffect(const FAudioEffect &effect, uint64_t effectStartFrame,
		uint64_t blockStartFrame, size_t blockFrames, float *mix)
	{
		const auto source = std::static_pointer_cast<const FVideoRecordingAudioSourceImpl>(effect.Source);
		if (source == nullptr || source->SampleRate == 0 || source->Channels == 0 || source->Samples.empty())
		{
			return;
		}
		const uint64_t sourceFrames = source->Samples.size() / source->Channels;
		if (sourceFrames == 0) return;
		const double firstSource = (double)effect.StartOffsetSeconds * source->SampleRate;
		const double step = (double)effect.Pitch * source->SampleRate / VIDEO_AUDIO_RATE;
		const float leftGain = effect.Gain * std::sqrt(0.5f * (1.0f - effect.Pan));
		const float rightGain = effect.Gain * std::sqrt(0.5f * (1.0f + effect.Pan));
		const uint64_t loopStart = std::min<uint64_t>(source->LoopStart, sourceFrames);
		const uint64_t loopEnd = source->LoopEnd > loopStart && source->LoopEnd <= sourceFrames ? source->LoopEnd : sourceFrames;

		for (size_t index = 0; index < blockFrames; ++index)
		{
			const uint64_t outputFrame = blockStartFrame + index;
			if (outputFrame < effectStartFrame) continue;
			double position = firstSource + (double)(outputFrame - effectStartFrame) * step;
			if (effect.Looping)
			{
				if (loopEnd <= loopStart) continue;
				if (position >= loopEnd)
				{
					position = loopStart + std::fmod(position - loopStart, (double)(loopEnd - loopStart));
				}
			}
			else if (position >= sourceFrames)
			{
				break;
			}
			if (position < 0.0) continue;
			const uint64_t sampleFrame = std::min<uint64_t>((uint64_t)position, sourceFrames - 1);
			const float left = source->Samples[sampleFrame * source->Channels] * (1.0f / 32768.0f);
			const float right = source->Channels == 2 ? source->Samples[sampleFrame * source->Channels + 1] * (1.0f / 32768.0f) : left;
			mix[index * 2] += left * leftGain;
			mix[index * 2 + 1] += right * rightGain;
		}
	}

	static bool RenderSnapshot(const FAudioSnapshot &snapshot, uint64_t startTimeNS, uint64_t endTimeNS,
		FVideoRecordingAudioSink sink, void *userdata, FString &error)
	{
		if (sink == nullptr || endTimeNS <= startTimeNS || startTimeNS < snapshot.TakeStartNS || endTimeNS > snapshot.TakeStopNS)
		{
			error = "the requested audio range is outside the recorded take";
			return false;
		}
		const uint64_t firstFrame = TimeToFrameFloor(startTimeNS, snapshot.TakeStartNS);
		const uint64_t lastFrame = TimeToFrameFloor(endTimeNS, snapshot.TakeStartNS);
		if (lastFrame <= firstFrame)
		{
			return true;
		}

		struct FPreparedEffect { const FAudioEffect *Effect; uint64_t Start; uint64_t End; };
		struct FPreparedStream { const FAudioStreamBlock *Stream; uint64_t Start; uint64_t End; };
		std::vector<FPreparedEffect> effects;
		effects.reserve(snapshot.Effects.size());
		for (const auto &effect : snapshot.Effects)
		{
			const uint64_t start = EventStartFrame(effect, snapshot);
			uint64_t end = EventEndFrame(effect, snapshot);
			if (!effect.Looping && effect.Source != nullptr)
			{
				const auto source = std::static_pointer_cast<const FVideoRecordingAudioSourceImpl>(effect.Source);
				if (source != nullptr && source->SampleRate != 0 && source->Channels != 0)
				{
					const uint64_t sourceFrames = source->Samples.size() / source->Channels;
					const double sourceStart = (double)effect.StartOffsetSeconds * source->SampleRate;
					const double playable = std::max(0.0, (double)sourceFrames - sourceStart);
					const uint64_t natural = start + (uint64_t)std::ceil(playable * VIDEO_AUDIO_RATE /
						(std::max(0.0001f, effect.Pitch) * source->SampleRate));
					end = std::min(end, natural);
				}
			}
			if (end > start && end > firstFrame && start < lastFrame)
			{
				effects.push_back({ &effect, start, end });
			}
		}
		std::sort(effects.begin(), effects.end(), [](const FPreparedEffect &a, const FPreparedEffect &b) { return a.Start < b.Start; });

		std::vector<FPreparedStream> streams;
		streams.reserve(snapshot.Streams.size());
		for (const auto &stream : snapshot.Streams)
		{
			if (stream.Samples == nullptr || stream.Frames == 0 || stream.SampleRate == 0) continue;
			const uint64_t start = TimeToFrameFloor(stream.StartNS, snapshot.TakeStartNS);
			const uint64_t duration = ResampledFrameCount(stream.Frames, stream.SampleRate);
			const uint64_t end = duration > std::numeric_limits<uint64_t>::max() - start ?
				std::numeric_limits<uint64_t>::max() : start + duration;
			if (end > firstFrame && start < lastFrame)
			{
				streams.push_back({ &stream, start, end });
			}
		}
		std::sort(streams.begin(), streams.end(), [](const FPreparedStream &a, const FPreparedStream &b) { return a.Start < b.Start; });

		std::vector<float> mix(VIDEO_AUDIO_RENDER_BLOCK_FRAMES * VIDEO_AUDIO_CHANNELS);
		std::vector<int16_t> output(VIDEO_AUDIO_RENDER_BLOCK_FRAMES * VIDEO_AUDIO_CHANNELS);
		std::vector<FPreparedEffect> activeEffects;
		std::vector<FPreparedStream> activeStreams;
		size_t nextEffect = 0;
		size_t nextStream = 0;
		for (uint64_t frame = firstFrame; frame < lastFrame; )
		{
			const size_t count = (size_t)std::min<uint64_t>(VIDEO_AUDIO_RENDER_BLOCK_FRAMES, lastFrame - frame);
			const uint64_t blockEnd = frame + count;
			while (nextEffect < effects.size() && effects[nextEffect].Start < blockEnd)
			{
				activeEffects.push_back(effects[nextEffect++]);
			}
			activeEffects.erase(std::remove_if(activeEffects.begin(), activeEffects.end(), [frame](const FPreparedEffect &effect) { return effect.End <= frame; }), activeEffects.end());
			while (nextStream < streams.size() && streams[nextStream].Start < blockEnd)
			{
				activeStreams.push_back(streams[nextStream++]);
			}
			activeStreams.erase(std::remove_if(activeStreams.begin(), activeStreams.end(), [frame](const FPreparedStream &stream) { return stream.End <= frame; }), activeStreams.end());

			std::fill(mix.begin(), mix.begin() + count * VIDEO_AUDIO_CHANNELS, 0.0f);
			for (const auto &effect : activeEffects)
			{
				MixEffect(*effect.Effect, effect.Start, frame, count, mix.data());
			}
			for (const auto &stream : activeStreams)
			{
				const uint64_t start = std::max(frame, stream.Start);
				const uint64_t end = std::min(blockEnd, stream.End);
				for (uint64_t sample = start; sample < end; ++sample)
				{
					const double sourcePosition = (double)(sample - stream.Start) * stream.Stream->SampleRate / VIDEO_AUDIO_RATE;
					const uint64_t sourceFrame = (uint64_t)sourcePosition;
					if (sourceFrame >= stream.Stream->Frames)
					{
						continue;
					}
					const uint64_t nextFrame = std::min<uint64_t>(sourceFrame + 1, stream.Stream->Frames - 1);
					const float fraction = (float)(sourcePosition - sourceFrame);
					const size_t destination = (size_t)(sample - frame) * 2;
					const size_t source = (size_t)sourceFrame * 2;
					const size_t next = (size_t)nextFrame * 2;
					const auto &samples = *stream.Stream->Samples;
					const float left = samples[source] + (samples[next] - samples[source]) * fraction;
					const float right = samples[source + 1] + (samples[next + 1] - samples[source + 1]) * fraction;
					mix[destination] += left * (1.0f / 32768.0f);
					mix[destination + 1] += right * (1.0f / 32768.0f);
				}
			}
			for (size_t sample = 0; sample < count * VIDEO_AUDIO_CHANNELS; ++sample)
			{
				output[sample] = ClampSample(mix[sample]);
			}
			if (!sink(output.data(), count, userdata))
			{
				error = "could not write the mixed audio output";
				return false;
			}
			frame = blockEnd;
		}
		return true;
	}

	struct FWavWriter
	{
		FileWriter *File = nullptr;
		bool Failed = false;
	};

	static bool WavSink(const int16_t *samples, size_t frames, void *userdata)
	{
		auto *writer = static_cast<FWavWriter *>(userdata);
		const size_t bytes = frames * VIDEO_AUDIO_FRAME_BYTES;
		writer->Failed = writer->Failed || !WriteAll(writer->File, samples, bytes);
		return !writer->Failed;
	}
}

// The public opaque type intentionally has the same allocation/layout as the
// private implementation. Keeping it private in the header avoids exposing
// recorder internals to individual sound backends.
struct FVideoRecordingAudioSource : FVideoRecordingAudioSourceImpl {};

FVideoRecordingAudioSourceRef I_CreateVideoRecordingAudioSource(const void *samples, size_t bytes,
	SampleType type, ChannelConfig channels, int sampleRate, uint32_t loopStart, uint32_t loopEnd)
{
	if (samples == nullptr || sampleRate <= 0)
	{
		return {};
	}
	uint32_t channelCount = 0;
	const size_t sampleSize = SampleSize(type);
	if (!GetChannelCount(channels, channelCount) || sampleSize == 0 || bytes > VIDEO_AUDIO_MAX_SOURCE_BYTES)
	{
		return {};
	}
	const size_t frames = bytes / (sampleSize * channelCount);
	if (frames == 0 || frames > std::numeric_limits<size_t>::max() / channelCount)
	{
		return {};
	}
	const uint64_t storageBytes = (uint64_t)frames * channelCount * sizeof(int16_t);
	uint64_t used = SourceCacheBytes.load(std::memory_order_relaxed);
	while (used <= VIDEO_AUDIO_MAX_SOURCE_CACHE_BYTES && storageBytes <= VIDEO_AUDIO_MAX_SOURCE_CACHE_BYTES - used)
	{
		if (SourceCacheBytes.compare_exchange_weak(used, used + storageBytes, std::memory_order_acq_rel))
		{
			// Reserve first so concurrent loaders cannot pass the hard cap, then
			// transfer that reservation to the source only after all allocations
			// and conversion have succeeded. A bad_alloc must not strand bytes in
			// the global cache budget or take down a game already low on memory.
			try
			{
				auto source = std::make_shared<FVideoRecordingAudioSource>();
				source->SampleRate = (uint32_t)sampleRate;
				source->Channels = channelCount;
				source->Samples.resize(frames * channelCount);
				for (size_t index = 0; index < frames * channelCount; ++index)
				{
					source->Samples[index] = ClampSample(DecodeSample(samples, index, type));
				}
				source->LoopStart = std::min<uint32_t>(loopStart, (uint32_t)frames);
				source->LoopEnd = loopEnd > source->LoopStart && loopEnd <= frames ? loopEnd : (uint32_t)frames;
				source->StorageBytes = storageBytes;
				return source;
			}
			catch (const std::bad_alloc &)
			{
				SourceCacheBytes.fetch_sub(storageBytes, std::memory_order_acq_rel);
				if (!SourceCacheAllocationReported.exchange(true, std::memory_order_acq_rel))
				{
					Printf(TEXTCOLOR_YELLOW "Video audio could not cache a decoded source because memory is exhausted; that effect will be omitted.\n");
				}
				return {};
			}
		}
	}
	if (!SourceCacheLimitReported.exchange(true, std::memory_order_acq_rel))
	{
		Printf(TEXTCOLOR_YELLOW "Video audio reached its bounded decoded-source cache; some effects will be omitted.\n");
	}
	return {};
}

uint64_t I_GetVideoRecordingAudioSourceBytes(const FVideoRecordingAudioSourceRef &source)
{
	const auto implementation = std::static_pointer_cast<const FVideoRecordingAudioSourceImpl>(source);
	return implementation != nullptr ? implementation->StorageBytes : 0;
}

void I_StartVideoRecordingAudio(const FString &outputStem, uint64_t startTimeNS)
{
	GetVideoRecordingAudioTap().Start(outputStem, startTimeNS);
}

void I_StopVideoRecordingAudio(uint64_t stopTimeNS)
{
	GetVideoRecordingAudioTap().Stop(stopTimeNS);
}

void I_DiscardVideoRecordingAudio()
{
	GetVideoRecordingAudioTap().Discard();
}

bool I_IsVideoRecordingAudioActive()
{
	return GetVideoRecordingAudioTap().IsActive();
}

uint64_t I_GetVideoRecordingAudioEpoch()
{
	return GetVideoRecordingAudioTap().GetEpoch();
}

uint32_t I_GetVideoRecordingAudioSampleRate()
{
	return VIDEO_AUDIO_RATE;
}

uint16_t I_GetVideoRecordingAudioChannels()
{
	return VIDEO_AUDIO_CHANNELS;
}

uint16_t I_GetVideoRecordingAudioBitsPerSample()
{
	return 16;
}

void I_RecordVideoRecordingAudioEffectStart(uint64_t epoch, uintptr_t sourceID,
	const FVideoRecordingAudioSourceRef &source, float gain, float pan,
	float pitch, bool looping, float startOffsetSeconds)
{
	GetVideoRecordingAudioTap().RecordEffectStart(epoch, sourceID, source, gain, pan, pitch, looping, startOffsetSeconds);
}

void I_RecordVideoRecordingAudioEffectStop(uint64_t epoch, uintptr_t sourceID)
{
	GetVideoRecordingAudioTap().RecordEffectStop(epoch, sourceID);
}

void I_RecordVideoRecordingAudioStream(uint64_t epoch, uintptr_t streamID, const void *samples,
	size_t bytes, SampleType type, ChannelConfig channels, int sampleRate,
	float gain, uint64_t streamStartNS)
{
	GetVideoRecordingAudioTap().RecordStream(epoch, streamID, samples, bytes, type, channels, sampleRate, gain, streamStartNS);
}

bool I_RenderVideoRecordingAudio(uint64_t startTimeNS, uint64_t endTimeNS,
	FVideoRecordingAudioSink sink, void *userdata, FString &error)
{
	try
	{
		FAudioSnapshot snapshot;
		if (!GetVideoRecordingAudioTap().Snapshot(snapshot, error, endTimeNS))
		{
			return false;
		}
		if (snapshot.StreamFailure != STREAM_FAILURE_NONE)
		{
			error = StreamFailureMessage(snapshot.StreamFailure);
			return false;
		}
		return RenderSnapshot(snapshot, startTimeNS, endTimeNS, sink, userdata, error);
	}
	catch (const std::bad_alloc &)
	{
		error = "the audio mixer ran out of memory while finalizing the recording";
		return false;
	}
}

bool I_WriteVideoRecordingAudioWav(FString &writtenPath, FString &error)
{
	writtenPath = "";
	FString path;
	std::unique_ptr<FileWriter> file;
	bool created = false;
	try
	{
		FAudioSnapshot snapshot;
		if (!GetVideoRecordingAudioTap().Snapshot(snapshot, error))
		{
			return false;
		}
		if (snapshot.StreamFailure != STREAM_FAILURE_NONE)
		{
			error = StreamFailureMessage(snapshot.StreamFailure);
			return false;
		}
		if (snapshot.OutputStem.IsEmpty())
		{
			error = "the audio capture has no output name";
			return false;
		}
		path = snapshot.OutputStem;
		path += "_audio.wav";
		const uint64_t frames = TimeToFrameFloor(snapshot.TakeStopNS, snapshot.TakeStartNS);
		if (frames == 0 || frames > (0xffffffffull - 36ull) / VIDEO_AUDIO_FRAME_BYTES)
		{
			error = "the audio timeline is outside the WAV size limit";
			return false;
		}
		const uint64_t bytes = frames * VIDEO_AUDIO_FRAME_BYTES;
		// The sequence selector reserved this name, but another process can
		// create it while a long take is in progress. Check immediately before
		// opening so FileWriter's truncating mode never clobbers that sidecar.
		if (FileExists(path.GetChars()))
		{
			error = "the WAV audio sidecar name was claimed while recording";
			return false;
		}
		file.reset(FileWriter::Open(path.GetChars()));
		if (file == nullptr)
		{
			error = "could not create the WAV audio sidecar";
			return false;
		}
		created = true;
		const bool header = WriteAll(file.get(), "RIFF", 4) && WriteU32(file.get(), (uint32_t)(36 + bytes)) &&
			WriteAll(file.get(), "WAVEfmt ", 8) && WriteU32(file.get(), 16) && WriteU16(file.get(), 1) &&
			WriteU16(file.get(), VIDEO_AUDIO_CHANNELS) && WriteU32(file.get(), VIDEO_AUDIO_RATE) &&
			WriteU32(file.get(), VIDEO_AUDIO_RATE * VIDEO_AUDIO_FRAME_BYTES) && WriteU16(file.get(), VIDEO_AUDIO_FRAME_BYTES) &&
			WriteU16(file.get(), 16) && WriteAll(file.get(), "data", 4) && WriteU32(file.get(), (uint32_t)bytes);
		FWavWriter writer{ file.get() };
		const bool rendered = header && RenderSnapshot(snapshot, snapshot.TakeStartNS, snapshot.TakeStopNS, WavSink, &writer, error);
		const bool closed = file->CloseChecked();
		file.reset();
		if (!rendered || writer.Failed || !closed)
		{
			if (error.IsEmpty()) error = "could not finalize the WAV audio sidecar";
			std::remove(path.GetChars());
			return false;
		}
		writtenPath = path;
		return true;
	}
	catch (const std::bad_alloc &)
	{
		if (file != nullptr)
		{
			file->Close();
			file.reset();
		}
		if (created && !path.IsEmpty())
		{
			std::remove(path.GetChars());
		}
		error = "the audio mixer ran out of memory while writing the WAV sidecar";
		return false;
	}
}
