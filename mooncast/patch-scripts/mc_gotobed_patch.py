import sys
# MoonCast: expose chiaki_session_goto_bed to Kotlin, so a player can put the PS5 into rest mode when you leave.
p = sys.argv[1]
s = open(p).read()
if "JNI_FCN(sessionGotoBed)" in s:
    print("already patched"); sys.exit(0)
anchor = "JNIEXPORT void JNICALL JNI_FCN(sessionSetLoginPin)"
assert anchor in s, "sessionSetLoginPin not found"
s = s.replace(anchor, """JNIEXPORT jint JNICALL JNI_FCN(sessionGotoBed)(JNIEnv *env, jobject obj, jlong ptr)
{
	AndroidChiakiSession *session = (AndroidChiakiSession *)ptr;
	return session ? chiaki_session_goto_bed(&session->session) : CHIAKI_ERR_UNINITIALIZED;
}

""" + anchor, 1)
open(p, "w").write(s)
print("patched")
