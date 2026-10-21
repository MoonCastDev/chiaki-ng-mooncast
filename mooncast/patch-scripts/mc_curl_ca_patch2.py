import sys
root = sys.argv[1]  # chiaki-ng/lib/src

# 1) aia_openssl.c: the chain-repair trust store asks curl for CURLINFO_CAINFO, which is curl's *built-in*
#    default (empty on Android), not CURLOPT_CAINFO -> fall back to the app's MC_CA_BUNDLE.
p = root + "/aia_openssl.c"
s = open(p).read()
if "MC_CA_BUNDLE" not in s:
    a = """		goto error;
#ifdef _WIN32"""
    assert s.count(a) == 1
    s = s.replace(a, """		goto error;
#ifdef __ANDROID__
	if(!file && !path)
		file = getenv("MC_CA_BUNDLE");   // MoonCast: Android curl has no built-in CA path
#endif
#ifdef _WIN32""", 1)
    if "#include <stdlib.h>" not in s:
        s = "#include <stdlib.h>\n" + s
    open(p, "w").write(s)
    print("aia_openssl.c patched")
else:
    print("aia_openssl.c already patched")

# 2) holepunch.c: the CAINFO_BLOB it builds (which *replaces* CAINFO) starts from CURLINFO_CAINFO's file ->
#    same fallback, so the repaired chain keeps the device roots.
p = root + "/remote/holepunch.c"
s = open(p).read()
if "MC_CA_BUNDLE (aia blob)" not in s:
    a = "CURLcode ca_info_res = curl_easy_getinfo(curl, CURLINFO_CAINFO, &ca_path);"
    assert s.count(a) == 1
    s = s.replace(a, a + """
#ifdef __ANDROID__
                if(ca_info_res == CURLE_OK && !ca_path)
                    ca_path = getenv("MC_CA_BUNDLE");   // MoonCast: MC_CA_BUNDLE (aia blob)
#endif""", 1)
    open(p, "w").write(s)
    print("holepunch.c patched")
else:
    print("holepunch.c already patched")
