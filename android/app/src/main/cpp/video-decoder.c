// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "video-decoder.h"

#include <jni.h>

#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <android/native_window_jni.h>

#include <string.h>
#include <strings.h>
#include <time.h>
#include <dlfcn.h>

// MoonCast: the PS5 decoder's latency mode, set by the app before each session (Settings, PlayStation, Decoder).
// The same keys MoonCast's PC player sets (Moonlight's MediaCodecHelper, Artemis' ultra presets).
static int mc_latency_mode = 1;

void android_chiaki_video_decoder_set_latency_mode(int mode)
{
	mc_latency_mode = mode;
}

// MoonCast frame pacing (Settings, PlayStation, Frame pacing), set by the app before each session
static int mc_pacing_mode = 0;
static int64_t mc_pacing_period_ns = 16666667;

void android_chiaki_video_decoder_set_pacing(int mode, int fps)
{
	mc_pacing_mode = mode;
	mc_pacing_period_ns = fps > 0 ? 1000000000LL / fps : 16666667;
}

static int64_t mc_now_ns(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (int64_t)t.tv_sec * 1000000000LL + t.tv_nsec;
}

// Show a decoded frame. Lowest delay: at once. Paced: one stream frame interval after the last one (so frames that
// arrive unevenly over the network show on evenly spaced screen refreshes), but never more than 1 (balanced) or
// 3 (smoothest) intervals late: past that it shows at once again, so the delay can't build up.
static void mc_present(AndroidChiakiVideoDecoder *decoder, size_t index)
{
	if(mc_pacing_mode <= 0)
	{
		AMediaCodec_releaseOutputBuffer(decoder->codec, index, true);
		return;
	}
	int64_t now = mc_now_ns();
	int64_t cap = (mc_pacing_mode >= 2 ? 3 : 1) * mc_pacing_period_ns;
	int64_t target = decoder->mc_last_present_ns + mc_pacing_period_ns;
	if(target < now || target - now > cap)
		target = now;
	decoder->mc_last_present_ns = target;
	AMediaCodec_releaseOutputBufferAtTime(decoder->codec, index, target);
}

static uint64_t mc_now_us(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * 1000000u + (uint64_t)t.tv_nsec / 1000u;
}

static bool mc_name_starts(const char *name, const char *prefix)
{
	return name && strncasecmp(name, prefix, strlen(prefix)) == 0;
}

// The decoder's name (AMediaCodec_getName is Android 9+; looked up at run time so older devices still load us)
static const char *mc_codec_name(AMediaCodec *codec, char **to_free)
{
	*to_free = NULL;
	media_status_t (*get_name)(AMediaCodec *, char **) = (media_status_t (*)(AMediaCodec *, char **))dlsym(RTLD_DEFAULT, "AMediaCodec_getName");
	void (*release_name)(AMediaCodec *, char *) = (void (*)(AMediaCodec *, char *))dlsym(RTLD_DEFAULT, "AMediaCodec_releaseName");
	char *name = NULL;
	if(get_name && release_name && get_name(codec, &name) == AMEDIA_OK && name)
	{
		*to_free = strdup(name);
		release_name(codec, name);
	}
	return *to_free ? *to_free : "unknown";
}

static void mc_low_latency_keys(AMediaFormat *f, const char *name, int mode)
{
	if(mode < 1)
		return;
	AMediaFormat_setInt32(f, "low-latency", 1);       // Android 11's standard key
	AMediaFormat_setInt32(f, "vdec-lowlatency", 1);   // MediaTek and Amlogic
	AMediaFormat_setInt32(f, "priority", 0);          // real-time
	bool qcom = mc_name_starts(name, "omx.qcom") || mc_name_starts(name, "c2.qti") || mc_name_starts(name, "c2.qcom");
	if(qcom)
	{
		bool omx = mc_name_starts(name, "omx.qcom");
		AMediaFormat_setInt32(f, "vendor.qti-ext-dec-picture-order.enable", mode >= 2 && omx ? 0 : 1);
		AMediaFormat_setInt32(f, "vendor.qti-ext-dec-low-latency.enable", 1);
		if(mode >= 2)
		{
			// software output fencing: what cuts latency on Snapdragon 8 Gen 2, 8s Gen 3 and 8 Elite (Artemis)
			AMediaFormat_setInt32(f, "vendor.qti-ext-output-sw-fence-enable.value", 1);
			AMediaFormat_setInt32(f, "vendor.qti-ext-output-fence.enable", 1);
			AMediaFormat_setInt32(f, "vendor.qti-ext-output-fence.fence_type", 1);
		}
	}
	else if(mode >= 2 && (mc_name_starts(name, "omx.mtk") || mc_name_starts(name, "c2.mtk")))
	{
		AMediaFormat_setInt32(f, "vendor.mtk.vdec.cpu.boost.mode", 1);
		AMediaFormat_setInt32(f, "vendor.mtk.vdec.cpu.boost.mode.value", 1);
		AMediaFormat_setInt32(f, "vendor.mtk.vdec.dvfs.mode", 1);
		AMediaFormat_setInt32(f, "vendor.mtk.vdec.dvfs.level", 1);
		AMediaFormat_setInt32(f, "vendor.mtk.vdec.low-latency.mode", 1);
		AMediaFormat_setInt32(f, "vendor.mtk.vdec.disable-idle", 1);
		AMediaFormat_setInt32(f, "vendor.mtk.vdec.preload.frame.count", 1);
		AMediaFormat_setInt32(f, "vendor.mtk.vdec.input.max.queue.depth", 3);
		AMediaFormat_setInt32(f, "vendor.mtk.vdec.output.max.queue.depth", 3);
		AMediaFormat_setInt32(f, "operating-rate", 32767);
	}
	else if(mode >= 2 && mc_name_starts(name, "omx.nvidia"))
	{
		AMediaFormat_setInt32(f, "media.low-latency.enable", 1);
		AMediaFormat_setInt32(f, "vendor.low-latency.enable", 1);
		AMediaFormat_setInt32(f, "vendor.nvidia.disable-output-reorder", 1);
	}
}

#define INPUT_BUFFER_TIMEOUT_MS 10

static void *android_chiaki_video_decoder_output_thread_func(void *user);

ChiakiErrorCode android_chiaki_video_decoder_init(AndroidChiakiVideoDecoder *decoder, ChiakiLog *log, int32_t target_width, int32_t target_height, ChiakiCodec codec)
{
	decoder->log = log;
	decoder->codec = NULL;
	decoder->timestamp_cur = 0;
	decoder->target_width = target_width;
	decoder->target_height = target_height;
	decoder->target_codec = codec;
	decoder->shutdown_output = false;
	decoder->mc_frames_shown = 0;
	decoder->mc_decode_ms = 0;
	decoder->mc_last_pts = 0;
	decoder->mc_last_present_ns = 0;
	decoder->mc_frames_lost = 0;
	return chiaki_mutex_init(&decoder->codec_mutex, false);
}

static void kill_decoder(AndroidChiakiVideoDecoder *decoder)
{
	// MoonCast (#37): clean shutdown. No AMediaCodec call may follow AMediaCodec_stop(): on Android 9 that aborts.
	chiaki_mutex_lock(&decoder->codec_mutex);
	decoder->shutdown_output = true; // video_sample() stops feeding, the output thread ends on its next poll
	ssize_t codec_buf_index = AMediaCodec_dequeueInputBuffer(decoder->codec, 1000);
	if(codec_buf_index >= 0)
	{
		CHIAKI_LOGI(decoder->log, "Video Decoder sending EOS buffer");
		AMediaCodec_queueInputBuffer(decoder->codec, (size_t)codec_buf_index, 0, 0, decoder->timestamp_cur++, AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
	}
	else
		CHIAKI_LOGE(decoder->log, "Failed to get input buffer for shutting down Video Decoder!");
	chiaki_mutex_unlock(&decoder->codec_mutex);
	chiaki_thread_join(&decoder->output_thread, NULL);
	chiaki_mutex_lock(&decoder->codec_mutex);
	AMediaCodec_stop(decoder->codec);
	AMediaCodec_delete(decoder->codec);
	decoder->codec = NULL;
	decoder->shutdown_output = false;
	chiaki_mutex_unlock(&decoder->codec_mutex);
}

void android_chiaki_video_decoder_fini(AndroidChiakiVideoDecoder *decoder)
{
	if(decoder->codec)
		kill_decoder(decoder);
	chiaki_mutex_fini(&decoder->codec_mutex);
}

void android_chiaki_video_decoder_set_surface(AndroidChiakiVideoDecoder *decoder, JNIEnv *env, jobject surface)
{
	chiaki_mutex_lock(&decoder->codec_mutex);

	if(!surface)
	{
		// MoonCast (#37): kill_decoder() takes codec_mutex itself and it isn't recursive, so release it first
		chiaki_mutex_unlock(&decoder->codec_mutex);
		if(decoder->codec)
		{
			kill_decoder(decoder);
			CHIAKI_LOGI(decoder->log, "Decoder shut down after surface was removed");
		}
		if(decoder->window)
		{
			ANativeWindow_release(decoder->window);
			decoder->window = NULL;
		}
		return;
	}

	if(decoder->codec)
	{
#if __ANDROID_API__ >= 23
		CHIAKI_LOGI(decoder->log, "Video decoder already initialized, swapping surface");
		ANativeWindow *new_window = surface ? ANativeWindow_fromSurface(env, surface) : NULL;
		AMediaCodec_setOutputSurface(decoder->codec, new_window);
		ANativeWindow_release(decoder->window);
		decoder->window = new_window;
#else
		CHIAKI_LOGE(decoder->log, "Video Decoder already initialized");
#endif
		goto beach;
	}

	decoder->window = ANativeWindow_fromSurface(env, surface);

	const char *mime = chiaki_codec_is_h265(decoder->target_codec) ? "video/hevc" : "video/avc";
	CHIAKI_LOGI(decoder->log, "Initializing decoder with mime %s", mime);

	decoder->codec = AMediaCodec_createDecoderByType(mime);
	if(!decoder->codec)
	{
		CHIAKI_LOGE(decoder->log, "Failed to create AMediaCodec for mime type %s", mime);
		goto error_surface;
	}

	AMediaFormat *format = AMediaFormat_new();
	AMediaFormat_setString(format, AMEDIAFORMAT_KEY_MIME, mime);
	AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_WIDTH, decoder->target_width);
	AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_HEIGHT, decoder->target_height);
	char *name_buf;
	const char *name = mc_codec_name(decoder->codec, &name_buf);
	mc_low_latency_keys(format, name, mc_latency_mode);
	CHIAKI_LOGI(decoder->log, "MoonCast decoder: %s, latency mode %d", name, mc_latency_mode);

	media_status_t r = AMediaCodec_configure(decoder->codec, format, decoder->window, NULL, 0);
	if(r != AMEDIA_OK && mc_latency_mode >= 1)
	{
		// a decoder that refuses one of the low-latency keys: start again with a fresh decoder and none of them
		CHIAKI_LOGW(decoder->log, "MoonCast decoder: configure with low-latency keys failed (%d), trying without", (int)r);
		AMediaFormat_delete(format);
		AMediaCodec_delete(decoder->codec);
		decoder->codec = AMediaCodec_createDecoderByType(mime);
		format = AMediaFormat_new();
		AMediaFormat_setString(format, AMEDIAFORMAT_KEY_MIME, mime);
		AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_WIDTH, decoder->target_width);
		AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_HEIGHT, decoder->target_height);
		r = decoder->codec ? AMediaCodec_configure(decoder->codec, format, decoder->window, NULL, 0) : AMEDIA_ERROR_UNKNOWN;
	}
	free(name_buf);
	if(!decoder->codec)
	{
		CHIAKI_LOGE(decoder->log, "Failed to create AMediaCodec for mime type %s", mime);
		AMediaFormat_delete(format);
		goto error_surface;
	}
	if(r != AMEDIA_OK)
	{
		CHIAKI_LOGE(decoder->log, "AMediaCodec_configure() failed: %d", (int)r);
		AMediaFormat_delete(format);
		goto error_codec;
	}

	r = AMediaCodec_start(decoder->codec);
	AMediaFormat_delete(format);
	if(r != AMEDIA_OK)
	{
		CHIAKI_LOGE(decoder->log, "AMediaCodec_start() failed: %d", (int)r);
		goto error_codec;
	}

	ChiakiErrorCode err = chiaki_thread_create(&decoder->output_thread, android_chiaki_video_decoder_output_thread_func, decoder);
	if(err != CHIAKI_ERR_SUCCESS)
	{
		CHIAKI_LOGE(decoder->log, "Failed to create output thread for AMediaCodec");
		goto error_codec;
	}

	goto beach;

error_codec:
	AMediaCodec_delete(decoder->codec);
	decoder->codec = NULL;

error_surface:
	ANativeWindow_release(decoder->window);
	decoder->window = NULL;

beach:
	chiaki_mutex_unlock(&decoder->codec_mutex);
}

bool android_chiaki_video_decoder_video_sample(uint8_t *buf, size_t buf_size, int32_t frames_lost, bool frame_recovered, void *user)
{
	bool r = true;
	AndroidChiakiVideoDecoder *decoder = user;
	if(frames_lost > 0)
		decoder->mc_frames_lost += frames_lost;
	chiaki_mutex_lock(&decoder->codec_mutex);

	if(!decoder->codec)
	{
		CHIAKI_LOGE(decoder->log, "Received video data, but decoder is not initialized!");
		goto beach;
	}
	if(decoder->shutdown_output)
		goto beach; // MoonCast (#37): shutting down, the codec takes no more input

	while(buf_size > 0)
	{
		ssize_t codec_buf_index = AMediaCodec_dequeueInputBuffer(decoder->codec, INPUT_BUFFER_TIMEOUT_MS * 1000);
		if(codec_buf_index < 0)
		{
			CHIAKI_LOGE(decoder->log, "Failed to get input buffer");
			r = false;
			goto beach;
		}

		size_t codec_buf_size;
		uint8_t *codec_buf = AMediaCodec_getInputBuffer(decoder->codec, (size_t)codec_buf_index, &codec_buf_size);
		if(!codec_buf) // MoonCast (#37): no input buffer, the codec has failed (e.g. its surface is gone)
		{
			CHIAKI_LOGE(decoder->log, "AMediaCodec_getInputBuffer() returned no buffer");
			r = false;
			goto beach;
		}
		size_t codec_sample_size = buf_size;
		if(codec_sample_size > codec_buf_size)
		{
			//CHIAKI_LOGD(decoder->log, "Sample is bigger than buffer, splitting");
			codec_sample_size = codec_buf_size;
		}
		memcpy(codec_buf, buf, codec_sample_size);
		// MoonCast: the time it's queued (us, strictly rising), so the output side can measure the decode time
		uint64_t pts = mc_now_us();
		if(pts <= decoder->mc_last_pts)
			pts = decoder->mc_last_pts + 1;
		decoder->mc_last_pts = pts;
		media_status_t r = AMediaCodec_queueInputBuffer(decoder->codec, (size_t)codec_buf_index, 0, codec_sample_size, pts, 0);
		if(r != AMEDIA_OK)
		{
			CHIAKI_LOGE(decoder->log, "AMediaCodec_queueInputBuffer() failed: %d", (int)r);
		}
		buf += codec_sample_size;
		buf_size -= codec_sample_size;

	}

beach:
	chiaki_mutex_unlock(&decoder->codec_mutex);
	return r;
}

static void *android_chiaki_video_decoder_output_thread_func(void *user)
{
	AndroidChiakiVideoDecoder *decoder = user;

	while(1)
	{
		AMediaCodecBufferInfo info;
		ssize_t status = AMediaCodec_dequeueOutputBuffer(decoder->codec, &info, 50000); // MoonCast (#37): poll, see shutdown_output
		if(status >= 0)
		{
			if(info.size != 0)
				mc_present(decoder, (size_t)status);
			else
				AMediaCodec_releaseOutputBuffer(decoder->codec, (size_t)status, false);
			if(info.size != 0)
			{
				decoder->mc_frames_shown++;
				// MoonCast: decode time of this frame, smoothed (about the last 20 frames)
				uint64_t now = mc_now_us();
				if(info.presentationTimeUs > 0 && now >= (uint64_t)info.presentationTimeUs && now - (uint64_t)info.presentationTimeUs < 1000000)
				{
					double ms = (now - (uint64_t)info.presentationTimeUs) / 1000.0;
					decoder->mc_decode_ms = decoder->mc_decode_ms <= 0 ? ms : decoder->mc_decode_ms * 0.95 + ms * 0.05;
				}
			}
			if(info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM)
			{
				CHIAKI_LOGI(decoder->log, "AMediaCodec reported EOS");
				break;
			}
		}
		else
		{
			chiaki_mutex_lock(&decoder->codec_mutex);
			bool shutdown = decoder->shutdown_output;
			chiaki_mutex_unlock(&decoder->codec_mutex);
			if(shutdown)
			{
				CHIAKI_LOGI(decoder->log, "Video Decoder Output Thread detected shutdown after reported error");
				break;
			}
		}
	}

	CHIAKI_LOGI(decoder->log, "Video Decoder Output Thread exiting");

	return NULL;
}