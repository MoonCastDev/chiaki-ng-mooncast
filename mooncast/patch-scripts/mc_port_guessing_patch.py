import sys
p = sys.argv[1]  # android/app/src/main/cpp/chiaki-jni.c (already patched by mc_jni_patch.py)
s = open(p).read()
if "force_port_guessing(hs, true)" in s:
    print("already patched"); sys.exit(0)

a = """	ChiakiHolepunchSession hs = chiaki_holepunch_session_init(token, log);
	if(!hs)
	{
		CHIAKI_LOGE(log, "MoonCast PSN: hole-punch session init failed (token?)");
		return NULL;
	}"""
assert s.count(a) == 1
s = s.replace(a, a + """
	// Port guessing (chiaki-ng's fix for port-rewriting NATs): when the STUN test sees a router that rewrites our
	// port, advertise a spread of guessed ports and open many sockets so the console's packets hit one of them.
	// Some JAL<->home punches got no candidate response at all; this targets that. Defaults: 75 guesses, 250 sockets.
	chiaki_holepunch_session_force_port_guessing(hs, true);""", 1)

b = """	if(err == CHIAKI_ERR_SUCCESS)
		err = holepunch_session_create_offer(hs);"""
assert s.count(b) == 1
s = s.replace(b, b + """
	{
		int32_t inc = 0; bool rnd = false;
		if(chiaki_holepunch_session_get_stun_allocation(hs, &inc, &rnd))
			CHIAKI_LOGI(log, "MoonCast PSN: our NAT allocation increment %d, random %d", (int)inc, (int)rnd);
	}""", 1)
open(p, "w").write(s)
print("patched")
