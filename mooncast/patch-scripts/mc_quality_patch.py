import sys
p = sys.argv[1]  # android/app/src/main/cpp/chiaki-jni.c
s = open(p).read()
if "packet_loss_max = 0.05" in s:
    print("already patched"); sys.exit(0)
a = "\tconnect_info.video_profile_auto_downgrade = true;"
assert s.count(a) == 1
s = s.replace(a, a + """
	// MoonCast: the Android binding left these zeroed. packet_loss_max 0 made chiaki always report 0% loss to the
	// console ("Clamping reported packet loss: measured=12.1% reported_max=0.0%"), so the PS5 never lowered its
	// bitrate on a lossy link. Same values as chiaki-ng desktop: report real loss up to 5%, and request a fresh
	// keyframe (IDR) when FEC can't repair a frame instead of decoding corrupt frames until the next one.
	connect_info.packet_loss_max = 0.05;
	connect_info.enable_idr_on_fec_failure = true;""", 1)
open(p, "w").write(s)
print("patched")
