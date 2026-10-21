import sys
p = sys.argv[1]
s = open(p).read()
if "mc_curl_easy_init" in s:
    print("already patched"); sys.exit(0)
inc = "#include <curl/curl.h>"
assert s.count(inc) == 1
wrapper = inc + r"""

#ifdef __ANDROID__
#include <stdlib.h>
// MoonCast: Android's libcurl has no CA store, so every PSN HTTPS call failed with "SSL peer certificate
// ... was not OK". The app writes the device's trusted roots (AndroidCAStore) to a PEM file and exports its
// path as MC_CA_BUNDLE; give it to every curl handle here (the AIA chain repair below builds on CURLINFO_CAINFO).
static CURL *mc_curl_easy_init(void)
{
	CURL *c = curl_easy_init();
	const char *ca = getenv("MC_CA_BUNDLE");
	if(c && ca && *ca)
		curl_easy_setopt(c, CURLOPT_CAINFO, ca);
	return c;
}
#define curl_easy_init mc_curl_easy_init
#endif
"""
s = s.replace(inc, wrapper, 1)
open(p, "w").write(s)
print("patched")
