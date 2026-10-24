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
#include <sys/resource.h>
#include <unistd.h>

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

// [level] drops keys for a decoder that refused them: 0 all, 1 no vendor keys, 2 also no vdec-lowlatency,
// 3 priority only, 4 none (as Moonlight's decoder setup retries for PC streams)
static void mc_low_latency_keys(AMediaFormat *f, const char *name, int mode, int level)
{
	if(mode < 1 || level >= 4)
		return;
	AMediaFormat_setInt32(f, "priority", 0);          // real-time
	if(level <= 2)
		AMediaFormat_setInt32(f, "low-latency", 1);   // Android 11's standard key
	if(level <= 1)
		AMediaFormat_setInt32(f, "vdec-lowlatency", 1);   // MediaTek and Amlogic
	if(level >= 1)
		return;
	bool qcom = mc_name_starts(name, "omx.qcom") || mc_name_starts(name, "c2.qti") || mc_name_starts(name, "c2.qcom");
	if(qcom)
	{
		bool omx = mc_name_starts(name, "omx.qcom");
		AMediaFormat_setInt32(f, "vendor.qti-ext-dec-picture-order.enable", mode >= 2 && omx ? 0 : 1);
		AMediaFormat_setInt32(f, "vendor.qti-ext-dec-low-latency.enable", 1);
		AMediaFormat_setInt32(f, "operating-rate", 32767);   // run the decoder flat out (Moonlight does on Qualcomm)
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

// MoonCast: when each frame really reached the screen (Android 13+, looked up at run time). Render wait = decoded ->
// on screen, i.e. waiting for the display's next refresh (and Frame pacing's hold), apart from the decode itself.
typedef void (*mc_rendered_cb)(AMediaCodec *, void *, int64_t, int64_t);
static void mc_on_rendered(AMediaCodec *codec, void *user, int64_t media_us, int64_t system_ns)
{
	(void)codec;
	AndroidChiakiVideoDecoder *decoder = user;
	if(media_us <= 0 || system_ns <= 0)
		return;
	int64_t shown_us = system_ns / 1000;
	int64_t total_us = shown_us - media_us;          // queued -> on screen
	if(total_us <= 0 || total_us > 1000000)
		return;
	double wait = total_us / 1000.0 - decoder->mc_decode_ms;
	if(wait < 0)
		wait = 0;
	decoder->mc_render_ms = decoder->mc_render_ms <= 0 ? wait : decoder->mc_render_ms * 0.95 + wait * 0.05;
}

static void mc_watch_rendered(AndroidChiakiVideoDecoder *decoder)
{
	media_status_t (*set_cb)(AMediaCodec *, mc_rendered_cb, void *) =
		(media_status_t (*)(AMediaCodec *, mc_rendered_cb, void *))dlsym(RTLD_DEFAULT, "AMediaCodec_setOnFrameRenderedCallback");
	if(set_cb)
		set_cb(decoder->codec, mc_on_rendered, decoder);
}

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
	decoder->mc_render_ms = 0;
	decoder->mc_last_out_us = 0;
	decoder->mc_ll_name[0] = 0;
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
	// MoonCast: Snapdragon (and MediaTek) list a separate low-latency decoder next to the default one
	// (c2.qti.hevc.decoder.low_latency on the Odin 2's 8 Gen 2), which the "by type" lookup never picks. It
	// returns each frame as soon as it's decoded; the default one holds frames back. Use it unless Standard.
	if(mc_latency_mode >= 1)
	{
		char *base_buf;
		const char *base = mc_codec_name(decoder->codec, &base_buf);
		const char *suffixes[] = { ".low_latency", ".lowlatency" };
		for(size_t i = 0; i < 2; i++)
		{
			char ll[160];
			snprintf(ll, sizeof(ll), "%s%s", base, suffixes[i]);
			AMediaCodec *c = AMediaCodec_createCodecByName(ll);
			if(c)
			{
				CHIAKI_LOGI(decoder->log, "MoonCast decoder: using %s instead of %s", ll, base);
				AMediaCodec_delete(decoder->codec);
				decoder->codec = c;
				decoder->mc_ll_name[0] = 0;
				strncpy(decoder->mc_ll_name, ll, sizeof(decoder->mc_ll_name) - 1);
				break;
			}
		}
		free(base_buf);
	}

	AMediaFormat *format = AMediaFormat_new();
	AMediaFormat_setString(format, AMEDIAFORMAT_KEY_MIME, mime);
	AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_WIDTH, decoder->target_width);
	AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_HEIGHT, decoder->target_height);
	char *name_buf;
	const char *name = mc_codec_name(decoder->codec, &name_buf);
	char name_copy[128];
	strncpy(name_copy, name, sizeof(name_copy) - 1);
	name_copy[sizeof(name_copy) - 1] = 0;
	free(name_buf);
	media_status_t r = AMEDIA_ERROR_UNKNOWN;
	for(int level = 0; level <= 4; level++)
	{
		if(level > 0)
		{
			// a decoder that refused a key: start again with a fresh decoder and fewer of them
			AMediaFormat_delete(format);
			AMediaCodec_delete(decoder->codec);
			decoder->codec = decoder->mc_ll_name[0] ? AMediaCodec_createCodecByName(decoder->mc_ll_name) : AMediaCodec_createDecoderByType(mime);
			if(!decoder->codec)
				break;
			format = AMediaFormat_new();
			AMediaFormat_setString(format, AMEDIAFORMAT_KEY_MIME, mime);
			AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_WIDTH, decoder->target_width);
			AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_HEIGHT, decoder->target_height);
		}
		mc_low_latency_keys(format, name_copy, mc_latency_mode, level);
		r = AMediaCodec_configure(decoder->codec, format, decoder->window, NULL, 0);
		if(r == AMEDIA_OK)
		{
			CHIAKI_LOGI(decoder->log, "MoonCast decoder: %s, latency mode %d, keys level %d", name_copy, mc_latency_mode, level);
			break;
		}
		CHIAKI_LOGW(decoder->log, "MoonCast decoder: configure failed (%d) at keys level %d", (int)r, level);
		if(mc_latency_mode < 1)
			break;
	}
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

	mc_watch_rendered(decoder);
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
	// MoonCast: this thread hands frames to the screen; give it display priority (as Moonlight's renderer has)
	if(setpriority(PRIO_PROCESS, (id_t)gettid(), -8) != 0)
		CHIAKI_LOGW(decoder->log, "MoonCast decoder: couldn't raise the output thread's priority");

	while(1)
	{
		AMediaCodecBufferInfo info;
		ssize_t status = AMediaCodec_dequeueOutputBuffer(decoder->codec, &info, 50000); // MoonCast (#37): poll, see shutdown_output
		if(status >= 0)
		{
			// MoonCast: decode time = input queued -> decoded frame handed back (what other players call "decode"),
			// measured before the frame is released to the screen; smoothed over about the last 20 frames
			uint64_t now = mc_now_us();
			if(info.size != 0 && info.presentationTimeUs > 0 && now >= (uint64_t)info.presentationTimeUs && now - (uint64_t)info.presentationTimeUs < 1000000)
			{
				double ms = (now - (uint64_t)info.presentationTimeUs) / 1000.0;
				decoder->mc_decode_ms = decoder->mc_decode_ms <= 0 ? ms : decoder->mc_decode_ms * 0.95 + ms * 0.05;
				decoder->mc_last_out_us = now;
			}
			if(info.size != 0)
				mc_present(decoder, (size_t)status);
			else
				AMediaCodec_releaseOutputBuffer(decoder->codec, (size_t)status, false);
			if(info.size != 0)
				decoder->mc_frames_shown++;
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