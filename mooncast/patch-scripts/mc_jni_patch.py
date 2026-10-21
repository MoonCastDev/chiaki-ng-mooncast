import sys
p = sys.argv[1]
s = open(p).read()
if "mc_psn_connect" in s:
    print("already patched"); sys.exit(0)

helpers = r"""
// ---- MoonCast: Remote Play over the internet (PSN hole-punch, no VPN / no port-forward) ----
// Optional ConnectInfo field: absent in older Kotlin bindings -> NULL (LAN behaviour unchanged).
static jobject mc_opt_field(JNIEnv *env, jobject obj, jclass cls, const char *name, const char *sig)
{
	jfieldID fid = E->GetFieldID(env, cls, name, sig);
	if(E->ExceptionCheck(env)) { E->ExceptionClear(env); return NULL; }
	return fid ? E->GetObjectField(env, obj, fid) : NULL;
}

static int mc_hex_nibble(char c)
{
	if(c >= '0' && c <= '9') return c - '0';
	if(c >= 'a' && c <= 'f') return c - 'a' + 10;
	if(c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

// Same steps as chiaki-ng's StreamSession::InitiatePsnConnection + ConnectPsnConnection: PSN brokers the
// connection and only the control hole is punched here - chiaki_session punches the data hole itself.
static ChiakiHolepunchSession mc_psn_connect(ChiakiLog *log, const char *token, const char *duid_hex, bool ps5)
{
	size_t hex_len = strlen(duid_hex);
	uint8_t duid[64];
	if(hex_len == 0 || hex_len % 2 || hex_len / 2 > sizeof(duid))
	{
		CHIAKI_LOGE(log, "MoonCast PSN: invalid console duid");
		return NULL;
	}
	for(size_t i = 0; i < hex_len / 2; i++)
	{
		int hi = mc_hex_nibble(duid_hex[2 * i]), lo = mc_hex_nibble(duid_hex[2 * i + 1]);
		if(hi < 0 || lo < 0)
		{
			CHIAKI_LOGE(log, "MoonCast PSN: invalid console duid");
			return NULL;
		}
		duid[i] = (uint8_t)((hi << 4) | lo);
	}
	ChiakiHolepunchSession hs = chiaki_holepunch_session_init(token, log);
	if(!hs)
	{
		CHIAKI_LOGE(log, "MoonCast PSN: hole-punch session init failed (token?)");
		return NULL;
	}
	ChiakiErrorCode err = chiaki_holepunch_upnp_discover(hs);
	if(err != CHIAKI_ERR_SUCCESS)
		CHIAKI_LOGW(log, "MoonCast PSN: no UPnP gateway (%s), continuing", chiaki_error_string(err));
	err = chiaki_holepunch_session_create(hs);
	if(err == CHIAKI_ERR_SUCCESS)
		err = holepunch_session_create_offer(hs);
	if(err == CHIAKI_ERR_SUCCESS)
		err = chiaki_holepunch_session_start(hs, duid, ps5 ? CHIAKI_HOLEPUNCH_CONSOLE_TYPE_PS5 : CHIAKI_HOLEPUNCH_CONSOLE_TYPE_PS4);
	if(err == CHIAKI_ERR_SUCCESS)
		err = chiaki_holepunch_session_punch_hole(hs, CHIAKI_HOLEPUNCH_PORT_TYPE_CTRL);
	if(err != CHIAKI_ERR_SUCCESS)
	{
		CHIAKI_LOGE(log, "MoonCast PSN: hole-punch failed: %s", chiaki_error_string(err));
		chiaki_holepunch_session_fini(hs);
		return NULL;
	}
	CHIAKI_LOGI(log, "MoonCast PSN: control hole punched, starting session over the internet");
	return hs;
}

"""
anchor = "JNIEXPORT void JNICALL JNI_FCN(sessionCreate)"
assert s.count(anchor) == 1
s = s.replace(anchor, helpers + anchor, 1)

block = r"""	// MoonCast: Remote Play over the internet when the app passes a PSN token + console duid
	{
		jstring psn_token_s = mc_opt_field(env, connect_info_obj, connect_info_class, "psnToken", "Ljava/lang/String;");
		jstring duid_s = mc_opt_field(env, connect_info_obj, connect_info_class, "duid", "Ljava/lang/String;");
		jbyteArray psn_account_id_a = mc_opt_field(env, connect_info_obj, connect_info_class, "psnAccountId", "[B");
		if(psn_token_s && duid_s)
		{
			const char *tok = E->GetStringUTFChars(env, psn_token_s, NULL);
			const char *duid = E->GetStringUTFChars(env, duid_s, NULL);
			connect_info.holepunch_session = mc_psn_connect(log, tok, duid, connect_info.ps5);
			E->ReleaseStringUTFChars(env, psn_token_s, tok);
			E->ReleaseStringUTFChars(env, duid_s, duid);
			if(!connect_info.holepunch_session)
			{
				err = CHIAKI_ERR_HOST_UNREACH;
				android_chiaki_video_decoder_fini(&session->video_decoder);
				android_chiaki_audio_decoder_fini(&session->audio_decoder);
				android_chiaki_audio_output_free(session->audio_output);
				free(session);
				session = NULL;
				goto beach;
			}
			if(psn_account_id_a && E->GetArrayLength(env, psn_account_id_a) == CHIAKI_PSN_ACCOUNT_ID_SIZE)
			{
				jbyte *acc = E->GetByteArrayElements(env, psn_account_id_a, NULL);
				memcpy(connect_info.psn_account_id, acc, CHIAKI_PSN_ACCOUNT_ID_SIZE);
				E->ReleaseByteArrayElements(env, psn_account_id_a, acc, JNI_ABORT);
			}
		}
	}

	err = chiaki_session_init(&session->session, &connect_info, log);"""
a2 = "\terr = chiaki_session_init(&session->session, &connect_info, log);"
assert s.count(a2) == 1
s = s.replace(a2, block, 1)
if "remote/holepunch.h" not in s:
    s = s.replace("#include <chiaki/regist.h>", "#include <chiaki/regist.h>\n#include <chiaki/remote/holepunch.h>", 1)
open(p, "w").write(s)
print("patched")
