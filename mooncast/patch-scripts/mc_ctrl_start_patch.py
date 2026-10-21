import sys
p = sys.argv[1]  # lib/src/session.c
s = open(p).read()
if "SESSION_EXPECT_CTRL_START_REMOTE_MS" in s:
    print("already patched"); sys.exit(0)
d = "#define SESSION_EXPECT_CTRL_START_MS    10000"
assert s.count(d) == 1
s = s.replace(d, d + """
// MoonCast: over PSN (hole-punch) the console only sends the ctrl session id after the data channel exists, so the
// wait above always ran its full 10 s before the data punch. Login (or a PIN request) arrives in ~0.25 s.
#define SESSION_EXPECT_CTRL_START_REMOTE_MS 2000""", 1)
w = "err = chiaki_cond_timedwait_pred(&session->state_cond, &session->state_mutex, SESSION_EXPECT_CTRL_START_MS, session_check_state_pred_ctrl_start, session);\n\tCHECK_STOP(quit_ctrl);\n\n\tif(session->ctrl_failed)"
assert s.count(w) == 1, s.count(w)
s = s.replace(w, w.replace("SESSION_EXPECT_CTRL_START_MS,",
    "session->holepunch_session ? SESSION_EXPECT_CTRL_START_REMOTE_MS : SESSION_EXPECT_CTRL_START_MS,"), 1)
open(p, "w").write(s)
print("patched")
