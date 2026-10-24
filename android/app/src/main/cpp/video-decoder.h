// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#ifndef CHIAKI_JNI_VIDEO_DECODER_H
#define CHIAKI_JNI_VIDEO_DECODER_H

#include <jni.h>

#include <chiaki/thread.h>
#include <chiaki/log.h>

typedef struct AMediaCodec AMediaCodec;
typedef struct ANativeWindow ANativeWindow;

typedef struct android_chiaki_video_decoder_t
{
	ChiakiLog *log;
	ChiakiMutex codec_mutex;
	AMediaCodec *codec;
	ANativeWindow *window;
	uint64_t timestamp_cur;
	ChiakiThread output_thread;
	bool shutdown_output;
	int32_t target_width;
	int32_t target_height;
	ChiakiCodec target_codec;
	uint64_t mc_frames_shown; // MoonCast stats
	uint64_t mc_frames_lost;
	double mc_decode_ms;     // MoonCast stats: average decode time (input queued -> frame out), ms
	uint64_t mc_last_pts;    // MoonCast: the input timestamps are real time (us), kept strictly rising
	int64_t mc_last_present_ns; // MoonCast frame pacing: when the last frame was asked to show
	double mc_render_ms;     // MoonCast stats: average decoded -> on screen (render wait), ms; 0 if unknown
	uint64_t mc_last_out_us; // MoonCast: when the last decoded frame came out
	char mc_ll_name[160];    // MoonCast: the low-latency decoder in use, if any
	uint64_t mc_diag_n, mc_diag_feed_us, mc_diag_bytes; // MoonCast diagnostics (logged every 300 frames)
} AndroidChiakiVideoDecoder;

/** MoonCast frame pacing: 0 lowest delay (show at once), 1 balanced (up to 1 frame later), 2 smoothest (up to 3).
 * [fps] is the stream's frame rate: frames are spaced by its interval. */
void android_chiaki_video_decoder_set_pacing(int mode, int fps);

/** MoonCast: how hard the decoder is pushed for low latency (0 standard, 1 low latency, 2 ultra low latency). */
void android_chiaki_video_decoder_set_latency_mode(int mode);

ChiakiErrorCode android_chiaki_video_decoder_init(AndroidChiakiVideoDecoder *decoder, ChiakiLog *log, int32_t target_width, int32_t target_height, ChiakiCodec codec);
void android_chiaki_video_decoder_fini(AndroidChiakiVideoDecoder *decoder);
void android_chiaki_video_decoder_set_surface(AndroidChiakiVideoDecoder *decoder, JNIEnv *env, jobject surface);
bool android_chiaki_video_decoder_video_sample(uint8_t *buf, size_t buf_size, int32_t frames_lost, bool frame_recovered, void *user);

#endif