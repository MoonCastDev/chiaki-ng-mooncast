import os, sys
# MoonCast: live stream stats for the PS5 Quick menu. Counts shown and lost frames in the Android decoder, keeps the
# delay the PS5 reports in its connection-quality messages, and exposes them with chiaki's measured bitrate and packet
# loss as sessionStats(). Run with the chiaki-ng root as the argument.
root = sys.argv[1]

def patch(rel, pairs, marker):
    p = os.path.join(root, rel)
    s = open(p).read()
    if marker in s:
        print(rel, "already patched"); return
    for old, new in pairs:
        assert s.count(old) == 1, (rel, old[:50])
        s = s.replace(old, new)
    open(p, "w").write(s)
    print(rel, "patched")

patch("lib/include/chiaki/streamconnection.h", [(
    "\tdouble measured_bitrate;\n} ChiakiStreamConnection;",
    "\tdouble measured_bitrate;\n\tdouble mc_rtt; // MoonCast: rtt from the latest connection-quality message\n} ChiakiStreamConnection;")],
    "mc_rtt")

patch("lib/src/streamconnection.c", [(
    "\t\tstream_connection->measured_bitrate = chiaki_stream_stats_bitrate(",
    "\t\tstream_connection->mc_rtt = q.rtt;\n\t\tstream_connection->measured_bitrate = chiaki_stream_stats_bitrate(")],
    "mc_rtt")

patch("android/app/src/main/cpp/video-decoder.h", [(
    "\tChiakiCodec target_codec;\n} AndroidChiakiVideoDecoder;",
    "\tChiakiCodec target_codec;\n\tuint64_t mc_frames_shown; // MoonCast stats\n\tuint64_t mc_frames_lost;\n} AndroidChiakiVideoDecoder;")],
    "mc_frames_shown")

patch("android/app/src/main/cpp/video-decoder.c", [
    ("\tdecoder->shutdown_output = false;\n\treturn chiaki_mutex_init(",
     "\tdecoder->shutdown_output = false;\n\tdecoder->mc_frames_shown = 0;\n\tdecoder->mc_frames_lost = 0;\n\treturn chiaki_mutex_init("),
    ("\t\t\tAMediaCodec_releaseOutputBuffer(decoder->codec, (size_t)status, info.size != 0);\n",
     "\t\t\tAMediaCodec_releaseOutputBuffer(decoder->codec, (size_t)status, info.size != 0);\n\t\t\tif(info.size != 0)\n\t\t\t\tdecoder->mc_frames_shown++;\n"),
    ("\tAndroidChiakiVideoDecoder *decoder = user;\n\tchiaki_mutex_lock(&decoder->codec_mutex);\n",
     "\tAndroidChiakiVideoDecoder *decoder = user;\n\tif(frames_lost > 0)\n\t\tdecoder->mc_frames_lost += frames_lost;\n\tchiaki_mutex_lock(&decoder->codec_mutex);\n"),
], "mc_frames_shown")

patch("android/app/src/main/cpp/chiaki-jni.c", [(
    "JNIEXPORT jint JNICALL JNI_FCN(sessionGotoBed)",
    """/** MoonCast: [measured Mbps, packet loss 0..1, PS5-reported rtt, rtt measured at connect (ms), frames shown, frames lost] */
JNIEXPORT jdoubleArray JNICALL JNI_FCN(sessionStats)(JNIEnv *env, jobject obj, jlong ptr)
{
	AndroidChiakiSession *session = (AndroidChiakiSession *)ptr;
	jdouble v[6] = { 0, 0, 0, 0, 0, 0 };
	if(session)
	{
		ChiakiStreamConnection *sc = &session->session.stream_connection;
		v[0] = sc->measured_bitrate;
		v[1] = sc->congestion_control.packet_loss;
		v[2] = sc->mc_rtt;
		v[3] = session->session.rtt_us / 1000.0;
		v[4] = (jdouble)session->video_decoder.mc_frames_shown;
		v[5] = (jdouble)session->video_decoder.mc_frames_lost;
	}
	jdoubleArray r = E->NewDoubleArray(env, 6);
	if(r)
		E->SetDoubleArrayRegion(env, r, 0, 6, v);
	return r;
}

JNIEXPORT jint JNICALL JNI_FCN(sessionGotoBed)""")],
    "JNI_FCN(sessionStats)")
