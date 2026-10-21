import sys
# MoonCast: log which ICE path nano-rs uses (direct on the LAN, server-reflexive, or a relay), every ~10 s of stats.
p = sys.argv[1] + "/src/webrtc_backend.rs"
s = open(p).read()
if "mc path" in s:
    print("already patched"); sys.exit(0)
old = '''                StatsReportType::CandidatePair(candidate)
                    if candidate.state.to_string() == "succeeded" =>
                {
'''
new = '''                StatsReportType::CandidatePair(candidate)
                    if candidate.state.to_string() == "succeeded" =>
                {
                    if mc_log_now {
                        crate::nano_log!("mc path pair local={} remote={} nominated={} rtt={:.1}ms",
                            candidate.local_candidate_id, candidate.remote_candidate_id, candidate.nominated,
                            candidate.current_round_trip_time * 1000.0);
                    }
'''
assert s.count(old) == 1
s = s.replace(old, new)
old2 = '''                _ => {}
            }
        }

        if stats.packets_received == 0 {'''
new2 = '''                StatsReportType::LocalCandidate(c) | StatsReportType::RemoteCandidate(c) if mc_log_now => {
                    crate::nano_log!("mc path candidate id={} type={:?} addr={}:{} net={:?}",
                        c.id, c.candidate_type, c.ip, c.port, c.network_type);
                }
                _ => {}
            }
        }

        if stats.packets_received == 0 {'''
assert s.count(old2) == 1
s = s.replace(old2, new2)
old3 = '''        for report in report.reports.values() {
            match report {
                StatsReportType::CandidatePair(candidate)'''
new3 = '''        static MC_STATS_TICK: std::sync::atomic::AtomicU32 = std::sync::atomic::AtomicU32::new(0);
        let mc_log_now = MC_STATS_TICK.fetch_add(1, Ordering::Relaxed) % 10 == 2;
        for report in report.reports.values() {
            match report {
                StatsReportType::CandidatePair(candidate)'''
assert s.count(old3) == 1
s = s.replace(old3, new3)
open(p, "w").write(s)
print("patched")
