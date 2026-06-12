/*
 * Copyright (C) 2026 Sergio Andreozzi
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Scenario-level validation tests for L4S binaries — verifies that the
 * scenario defaults exercise the RFC 9332 coupling cascade (§2.1
 * eq. (1); App. A.1 Fig. 6), where p_C = p'² and
 * p_L = min(k·p', 1) hold approximately across the controller's full
 * operating range. Throughput-ratio equivalence
 * is structurally unobservable with non-responsive UDP CBR flows and
 * is not asserted here.
 */

#include "ns3/test.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace ns3::stratum
{

namespace
{

/// Run a shell command, capture stdout to a string. Returns exit status.
int
RunCapture(const std::string& cmd, std::string& out)
{
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe)
    {
        return -1;
    }
    char buf[1024];
    while (fgets(buf, sizeof(buf), pipe))
    {
        out += buf;
    }
    return pclose(pipe);
}

/// Read one CSV column by name. Returns empty on parse failure.
std::vector<double>
ReadCsvColumn(const std::string& path, const std::string& col_name)
{
    std::ifstream in(path);
    std::vector<double> result;
    if (!in.is_open())
    {
        return result;
    }
    std::string header;
    if (!std::getline(in, header))
    {
        return result;
    }
    // Find col_name's index in the header
    std::stringstream hss(header);
    std::string tok;
    std::vector<std::string> cols;
    while (std::getline(hss, tok, ','))
    {
        cols.push_back(tok);
    }
    int col_idx = -1;
    for (size_t i = 0; i < cols.size(); ++i)
    {
        if (cols[i] == col_name)
        {
            col_idx = static_cast<int>(i);
            break;
        }
    }
    if (col_idx < 0)
    {
        return result;
    }
    std::string line;
    while (std::getline(in, line))
    {
        std::stringstream ss(line);
        std::vector<std::string> row;
        while (std::getline(ss, tok, ','))
        {
            row.push_back(tok);
        }
        if (static_cast<int>(row.size()) > col_idx)
        {
            try
            {
                result.push_back(std::stod(row[col_idx]));
            }
            catch (...)
            {
                // skip malformed row
            }
        }
    }
    return result;
}

double
Mean(const std::vector<double>& v)
{
    if (v.empty())
    {
        return 0.0;
    }
    double s = 0.0;
    for (double x : v)
    {
        s += x;
    }
    return s / static_cast<double>(v.size());
}

} // anonymous namespace

/**
 * S-l4s-piControl-fires-at-nominal-load — verifies that the defaults of
 * diffserv-l4s-s2-equivalence exercise the RFC 9332 coupling cascade
 * (§2.1 eq. (1); App. A.1 Fig. 6 lines 4-5). Under the defaults'
 * non-responsive overload the controller legitimately rides high (clamp
 * episodes included); the assertions are regime-agnostic — engagement
 * fraction plus per-sample formula tracking.
 *
 * Pass criteria (per spec/02-structural.md, recalibrated 2026-06-10 with the
 * eq. (1) coupling-map correction):
 *  - (pPrime > 0).sum() / len(pPrime) > 0.05  — controller engages
 *  - For active samples (pPrime > 0): coupling cascade holds within 10%
 *      pC ≈ pPrime^2
 *      pL ≈ min(k * pPrime, 1)  where k=2
 */
class DsL4sScenarioPiControlFiresTest : public TestCase
{
  public:
    DsL4sScenarioPiControlFiresTest()
        : TestCase("scenario validation: P.I.² controller fires + coupling formula holds")
    {
    }

  private:
    void DoRun() override
    {
        const std::string tmpdir =
            std::filesystem::temp_directory_path().string() + "/v1.7-l4s-s2-fixture";
        std::filesystem::remove_all(tmpdir);
        std::filesystem::create_directories(tmpdir);

        const std::string cmd =
            "./ns3 run \"diffserv-l4s-s2-equivalence --simTime=3 --outDir=" + tmpdir + "\" 2>&1";
        std::string out;
        const int rc = RunCapture(cmd, out);
        NS_TEST_ASSERT_MSG_EQ(rc, 0, "s2-equivalence binary failed: " << out);

        const auto pprime = ReadCsvColumn(tmpdir + "/coupling.csv", "pPrime");
        const auto pc = ReadCsvColumn(tmpdir + "/coupling.csv", "pC");
        const auto pl = ReadCsvColumn(tmpdir + "/coupling.csv", "pL");

        NS_TEST_ASSERT_MSG_GT(pprime.size(), 0u, "coupling.csv has no rows");
        NS_TEST_ASSERT_MSG_EQ(pprime.size(), pc.size(), "pPrime and pC column lengths differ");
        NS_TEST_ASSERT_MSG_EQ(pprime.size(), pl.size(), "pPrime and pL column lengths differ");

        // Controller engagement: non-trivial sample fraction with pPrime > 0.
        size_t nz = 0;
        for (double v : pprime)
        {
            if (v > 0.0)
            {
                ++nz;
            }
        }
        const double nz_frac = static_cast<double>(nz) / static_cast<double>(pprime.size());
        NS_TEST_ASSERT_MSG_GT(nz_frac,
                              0.05,
                              "pPrime non-zero fraction must exceed 0.05 at simTime=3 (got "
                                  << nz_frac << ")");

        // Coupling-formula verification on active samples.
        constexpr double k = 2.0;
        constexpr double tol = 0.10;
        size_t pc_checked = 0;
        size_t pl_checked = 0;
        size_t pc_in_tol = 0;
        size_t pl_in_tol = 0;
        for (size_t i = 0; i < pprime.size(); ++i)
        {
            if (pprime[i] > 0.0)
            {
                // RFC 9332 App. A.1 Fig. 6 lines 4-5: pC = pPrime^2,
                // pL = min(k * pPrime, 1). pC and pL are drop/mark
                // PROBABILITIES — both clamp at 1.0 (with pPrime <= 1
                // the pC clamp is vacuous; kept for symmetry).
                const double pc_expected = std::min(pprime[i] * pprime[i], 1.0);
                const double pl_expected = std::min(k * pprime[i], 1.0);
                const double pc_denom = std::max(pc[i], 1e-6);
                const double pl_denom = std::max(pl[i], 1e-6);
                const double pc_err = std::fabs(pc[i] - pc_expected) / pc_denom;
                const double pl_err = std::fabs(pl[i] - pl_expected) / pl_denom;
                ++pc_checked;
                ++pl_checked;
                if (pc_err < tol)
                {
                    ++pc_in_tol;
                }
                if (pl_err < tol)
                {
                    ++pl_in_tol;
                }
            }
        }
        // CSV dump for coupling-formula analysis (audit-2026-05-23 follow-up #3).
        // Written when env-var L4S_COUPLING_CSV is set.
        if (const char* csvPath = std::getenv("L4S_COUPLING_CSV"); csvPath && *csvPath)
        {
            std::ofstream csv(csvPath);
            csv << "idx,pPrime,pC_actual,pL_actual,pC_expected,pL_expected,"
                << "pC_err,pL_err,pC_in_tol,pL_in_tol\n";
            for (size_t i = 0; i < pprime.size(); ++i)
            {
                if (pprime[i] <= 0.0)
                {
                    continue; // skip inactive samples
                }
                const double pc_expected_i = std::min(pprime[i] * pprime[i], 1.0);
                const double pl_expected_i = std::min(k * pprime[i], 1.0);
                const double pc_denom_i = std::max(pc[i], 1e-6);
                const double pl_denom_i = std::max(pl[i], 1e-6);
                const double pc_err_i = std::fabs(pc[i] - pc_expected_i) / pc_denom_i;
                const double pl_err_i = std::fabs(pl[i] - pl_expected_i) / pl_denom_i;
                csv << i << "," << pprime[i] << "," << pc[i] << "," << pl[i] << "," << pc_expected_i
                    << "," << pl_expected_i << "," << pc_err_i << "," << pl_err_i << ","
                    << (pc_err_i < tol ? 1 : 0) << "," << (pl_err_i < tol ? 1 : 0) << "\n";
            }
            csv.close();
            std::cerr << "L4S coupling CSV written to " << csvPath << "\n";
        }
        // Require >=90% of active samples to obey the formula within 10%.
        NS_TEST_ASSERT_MSG_GT(pc_checked, 0u, "no active samples to verify pC formula on");
        const double pc_frac = static_cast<double>(pc_in_tol) / static_cast<double>(pc_checked);
        const double pl_frac = static_cast<double>(pl_in_tol) / static_cast<double>(pl_checked);
        NS_TEST_ASSERT_MSG_GT(pc_frac,
                              0.90,
                              "pC formula holds on " << pc_frac
                                                     << " of active samples "
                                                        "(need >0.90)");
        NS_TEST_ASSERT_MSG_GT(pl_frac,
                              0.90,
                              "pL formula holds on " << pl_frac
                                                     << " of active samples "
                                                        "(need >0.90)");
    }
};

/**
 * S-l4s-s1-latency-arm-differentiation — verifies that priority wiring is
 * functional in both --mode=l4s-on and --mode=l4s-off.
 *
 * Pass criterion (per spec/02-structural.md, post-d831742):
 *  - mean(owd_ef_on)  < 10 ms
 *  - mean(owd_ef_off) < 10 ms
 *
 * The original "15% mean / 20% P95 reduction" criterion is structurally
 * unachievable with UDP CBR (see findings doc). The relaxed criterion
 * verifies priority routing works in both modes — a weaker but meaningful
 * assertion; full L4S vs no-L4S differentiation is deferred to v1.8+ with
 * responsive flows (TCP Prague / Reno).
 */
class DsL4sScenarioS1LatencyDifferentiationTest : public TestCase
{
  public:
    DsL4sScenarioS1LatencyDifferentiationTest()
        : TestCase("scenario validation: s1-latency EF priority routing functional in both modes")
    {
    }

  private:
    void DoRun() override
    {
        const std::string tmp =
            std::filesystem::temp_directory_path().string() + "/v1.7-l4s-s1-fixture";
        std::filesystem::remove_all(tmp + "-on");
        std::filesystem::remove_all(tmp + "-off");
        std::filesystem::create_directories(tmp + "-on");
        std::filesystem::create_directories(tmp + "-off");

        std::string out;
        int rc;
        rc = RunCapture("./ns3 run \"diffserv-l4s-s1-latency --mode=l4s-on  --simTime=3 "
                        "--outDir=" +
                            tmp + "-on\" 2>&1",
                        out);
        NS_TEST_ASSERT_MSG_EQ(rc, 0, "s1-latency l4s-on binary failed: " << out);
        out.clear();
        rc = RunCapture("./ns3 run \"diffserv-l4s-s1-latency --mode=l4s-off --simTime=3 "
                        "--outDir=" +
                            tmp + "-off\" 2>&1",
                        out);
        NS_TEST_ASSERT_MSG_EQ(rc, 0, "s1-latency l4s-off binary failed: " << out);

        const auto on_ef = ReadCsvColumn(tmp + "-on/l4s-on__owd-ef.csv", "owd_ms");
        const auto off_ef = ReadCsvColumn(tmp + "-off/l4s-off__owd-ef.csv", "owd_ms");

        NS_TEST_ASSERT_MSG_GT(on_ef.size(), 0u, "l4s-on EF OWD CSV has no rows");
        NS_TEST_ASSERT_MSG_GT(off_ef.size(), 0u, "l4s-off EF OWD CSV has no rows");

        const double mean_on = Mean(on_ef);
        const double mean_off = Mean(off_ef);

        NS_TEST_ASSERT_MSG_LT(mean_on,
                              10.0,
                              "l4s-on EF mean OWD must be < 10 ms (got " << mean_on << " ms)");
        NS_TEST_ASSERT_MSG_LT(mean_off,
                              10.0,
                              "l4s-off EF mean OWD must be < 10 ms (got " << mean_off << " ms)");
    }
};

/**
 * S-l4s-s2-coexistence-throughput-equivalence — verifies that DCTCP (L4S path)
 * and TCP Cubic (Classic path) share a 10 Mbps bottleneck equitably under
 * DualPI2 AQM. The L4S-to-Classic throughput ratio must remain within the
 * calibration envelope derived from a 5-seed sweep of the canonical cell
 * (simTime=60, bottleneckMbps=10, queueLimit=1000). Both senders must
 * exhibit congestion-window responsiveness (≥5 reduction events each).
 *
 * Pass criteria (sweep-based; envelope re-derived 2026-06-10 from a
 * 10-seed sweep after the coupling-map correction):
 *  - ratio ∈ [0.60, 1.35]   (post-correction 10-seed sweep: mean 1.032,
 *    observed range [0.719, 1.245]; bounds add headroom for cross-seed
 *    variance)
 *  - cwnd-reduction events per sender ≥ 5
 */
class DsL4sScenarioS2CoexistenceThroughputEquivalenceTest : public TestCase
{
  public:
    DsL4sScenarioS2CoexistenceThroughputEquivalenceTest()
        : TestCase("L4S s2-coexistence: TcpDctcp+TcpCubic throughput equivalence within "
                   "calibration envelope; both senders responsive")
    {
    }

  private:
    void DoRun() override
    {
        const std::string outDir =
            std::filesystem::temp_directory_path().string() + "/test-v18-s2-coexistence";
        std::filesystem::remove_all(outDir);
        std::filesystem::create_directories(outDir);

        const std::string cmd = "./ns3 run --no-build "
                                "\"diffserv-l4s-s2-coexistence --simTime=60 --warmup=10 "
                                "--outDir=" +
                                outDir + "\" 2>&1";
        std::string out;
        const int rc = RunCapture(cmd, out);
        NS_TEST_ASSERT_MSG_EQ(rc, 0, "s2-coexistence binary failed: " << out);

        // Parse flent-per-flow.csv: time_s,bps_L,bps_C (warmup already filtered by binary)
        const auto bpsL = ReadCsvColumn(outDir + "/flent-per-flow.csv", "bps_L");
        const auto bpsC = ReadCsvColumn(outDir + "/flent-per-flow.csv", "bps_C");

        NS_TEST_ASSERT_MSG_GT(bpsL.size(),
                              100u,
                              "Too few post-warmup samples in flent-per-flow.csv;"
                              " binary may not have run full duration");
        NS_TEST_ASSERT_MSG_EQ(bpsL.size(), bpsC.size(), "bps_L and bps_C row count mismatch");

        const double ratio = Mean(bpsL) / std::max(Mean(bpsC), 1.0);

        // Sweep-based envelope (2026-06-10): 10-seed sweep (RngRun 1-10)
        // after the RFC 9332 §2.1 eq. (1) coupling-map correction
        // (p_C = p'^2, p_CL = k*p') and the move to the RFC-default
        // gains (alpha 0.16, beta 3.2): observed mean 1.032, s = 0.145,
        // range [0.719, 1.245]. Bounds = observed range plus ~0.1
        // bracket margin, same procedure as the 2026-05-24 envelope
        // (then: 5 seeds, mean 0.947, range [0.860, 1.097], bounds
        // [0.75, 1.20], calibrated against the pre-correction coupling).
        // The corrected map doubles equilibrium p' for a given classic
        // drop rate and the RFC alpha halves the integral gain, so the
        // 60 s window mean disperses more across seeds at this cell;
        // the per-seed GPRT JFI parity gate (below in this suite) is
        // what pins the operating point to the Linux-aligned reference,
        // while this band guards against one-sided starvation.
        constexpr double kRatioLow = 0.60;
        constexpr double kRatioHigh = 1.35;
        NS_TEST_ASSERT_MSG_GT(ratio,
                              kRatioLow,
                              "L:C throughput ratio below sweep-based envelope [0.60, 1.35];"
                              " got "
                                  << ratio);
        NS_TEST_ASSERT_MSG_LT(ratio,
                              kRatioHigh,
                              "L:C throughput ratio above sweep-based envelope [0.60, 1.35];"
                              " got "
                                  << ratio);

        // Cwnd responsiveness: count ≥30%-drop events per sender within time-ordered trace
        auto countReductionEvents = [](const std::vector<double>& cwnds) -> int {
            int events = 0;
            double peak = 0.0;
            for (double cw : cwnds)
            {
                if (cw > peak)
                {
                    peak = cw;
                }
                if (peak > 0.0 && cw < peak * 0.70)
                {
                    ++events;
                    peak = cw; // reset peak so the same trough isn't double-counted
                }
            }
            return events;
        };

        const auto cwndL = ReadCsvColumn(outDir + "/cwnd-L.csv", "cwnd_bytes");
        const auto cwndC = ReadCsvColumn(outDir + "/cwnd-C.csv", "cwnd_bytes");

        NS_TEST_ASSERT_MSG_GT(cwndL.size(), 0u, "cwnd-L.csv has no rows");
        NS_TEST_ASSERT_MSG_GT(cwndC.size(), 0u, "cwnd-C.csv has no rows");

        const int eventsL = countReductionEvents(cwndL);
        const int eventsC = countReductionEvents(cwndC);

        NS_TEST_ASSERT_MSG_GT(eventsL,
                              4,
                              "L4S cwnd reduction events < 5; sender not responsive."
                              " Got: "
                                  << eventsL);
        NS_TEST_ASSERT_MSG_GT(eventsC,
                              4,
                              "Classic cwnd reduction events < 5; sender not responsive."
                              " Got: "
                                  << eventsC);
    }
};

/**
 * S-l4s-s1-advantage-latency-delta — verifies that both DualPI2 and FqCoDel
 * protect probe-flow P95 OWD to approximately 10 ms under 2 bulk senders,
 * while FIFO lets probe P95 OWD balloon to ≥ 50 ms. Calibration sweeps showed
 * L4S ≈ FqCoDel (~9 ms each); the fixture captures the 26× AQM-vs-no-AQM gap.
 *
 * Pass criteria (calibration-locked):
 *  - P95(l4s)    ≤ 12.0 ms   (observed ~8.9 ms × 1.35 headroom)
 *  - P95(fqcodel)≤ 12.0 ms   (observed ~8.9 ms × 1.35 headroom)
 *  - P95(fifo)   ≥ 50.0 ms   (observed ~238 ms; conservative floor)
 *  - P95(l4s) / P95(fqcodel) ∈ [0.70, 1.30]   (equivalence band)
 */
class DsL4sScenarioS1AdvantageLatencyDeltaTest : public TestCase
{
  public:
    DsL4sScenarioS1AdvantageLatencyDeltaTest()
        : TestCase("L4S s1-advantage: probe P95 OWD — AQM modes (l4s, fqcodel) protect "
                   "to ~10ms; FIFO bloats to >= 50ms (calibration envelope)")
    {
    }

  private:
    void DoRun() override
    {
        const std::string baseOut =
            std::filesystem::temp_directory_path().string() + "/test-v18-s1-advantage";
        std::filesystem::remove_all(baseOut);

        for (const std::string& mode : std::vector<std::string>{"l4s", "fqcodel", "fifo"})
        {
            std::filesystem::create_directories(baseOut + "/" + mode);
            const std::string cmd = "./ns3 run --no-build "
                                    "\"diffserv-l4s-s1-advantage --simTime=60 --warmup=10 "
                                    "--bulkSenders=2 --mode=" +
                                    mode + " --outDir=" + baseOut + "\" 2>&1";
            std::string out;
            const int rc = RunCapture(cmd, out);
            NS_TEST_ASSERT_MSG_EQ(rc, 0, "s1-advantage failed for mode " + mode + ": " << out);
        }

        auto p95 = [](const std::vector<double>& v) -> double {
            if (v.empty())
            {
                return 0.0;
            }
            std::vector<double> sorted(v);
            std::sort(sorted.begin(), sorted.end());
            return sorted[static_cast<size_t>(sorted.size() * 0.95)];
        };

        const double p95L4s = p95(ReadCsvColumn(baseOut + "/l4s/probe-owd.csv", "owd_ms"));
        const double p95Fq = p95(ReadCsvColumn(baseOut + "/fqcodel/probe-owd.csv", "owd_ms"));
        const double p95Fifo = p95(ReadCsvColumn(baseOut + "/fifo/probe-owd.csv", "owd_ms"));

        // Locked envelope (observed: l4s ~8.9 ms, fqcodel ~8.9 ms, fifo ~238 ms)
        constexpr double kAqmCeilingMs = 12.0;
        constexpr double kFifoFloorMs = 50.0;
        constexpr double kAqmEqRatioLow = 0.70;
        constexpr double kAqmEqRatioHigh = 1.30;

        NS_TEST_ASSERT_MSG_LT(p95L4s,
                              kAqmCeilingMs,
                              "L4S probe P95 OWD above 12 ms ceiling; got " << p95L4s);
        NS_TEST_ASSERT_MSG_LT(p95Fq,
                              kAqmCeilingMs,
                              "FqCoDel probe P95 OWD above 12 ms ceiling; got " << p95Fq);
        NS_TEST_ASSERT_MSG_GT(p95Fifo,
                              kFifoFloorMs,
                              "FIFO probe P95 OWD below 50 ms floor — AQM-vs-no-AQM gap not"
                              " visible; got "
                                  << p95Fifo);

        const double aqmRatio = p95L4s / std::max(p95Fq, 0.001);
        NS_TEST_ASSERT_MSG_GT(aqmRatio,
                              kAqmEqRatioLow,
                              "L4S/FqCoDel P95 ratio below equivalence band [0.7, 1.3]; got "
                                  << aqmRatio);
        NS_TEST_ASSERT_MSG_LT(aqmRatio,
                              kAqmEqRatioHigh,
                              "L4S/FqCoDel P95 ratio above equivalence band [0.7, 1.3]; got "
                                  << aqmRatio);
    }
};

/**
 * Smoke test: each of the 5 AQM modes supported by the
 * diffserv-l4s-fqcodel-comparison binary produces a non-trivial
 * probe-owd.csv output (>= 300 rows) when run at N=10 bulk senders and
 * simTime=30 s. This guards against binary-side mode-drop regressions
 * without asserting any numeric physics.
 *
 * Pass criterion: for each mode in {l4s-wred, l4s-coupled-only,
 * l4s-fqcodel-classic, fqcodel, classic-only}:
 *   - binary exits 0
 *   - <outDir>/<mode>/N10/probe-owd.csv exists and is readable
 *   - header line equals "t_us,owd_ms"
 *   - data row count >= 300
 */
class DsL4sScenarioFqCoDelComparisonSmokePerModeTest : public TestCase
{
  public:
    DsL4sScenarioFqCoDelComparisonSmokePerModeTest()
        : TestCase("L4S fqcodel-comparison: each of 5 AQM modes emits >= 300 OWD samples")
    {
    }

  private:
    void DoRun() override
    {
        const std::string tmpDir =
            std::filesystem::temp_directory_path().string() + "/v1.14-fqcodel-cmp-smoke";
        std::filesystem::remove_all(tmpDir);
        std::filesystem::create_directories(tmpDir);

        const std::vector<std::string> modes = {"l4s-wred",
                                                "l4s-coupled-only",
                                                "l4s-fqcodel-classic",
                                                "fqcodel",
                                                "classic-only"};

        for (const auto& m : modes)
        {
            const std::string cmd = "./ns3 run \"diffserv-l4s-fqcodel-comparison"
                                    " --mode=" +
                                    m +
                                    " --bulkSenders=10 --simTime=30"
                                    " --warmup=5 --output=" +
                                    tmpDir + "\" 2>&1";
            std::string out;
            const int rc = RunCapture(cmd, out);
            NS_TEST_ASSERT_MSG_EQ(rc,
                                  0,
                                  "diffserv-l4s-fqcodel-comparison failed for mode=" << m << ": "
                                                                                     << out);

            const std::string csvPath = tmpDir + "/" + m + "/N10/probe-owd.csv";
            std::ifstream csv(csvPath);
            NS_TEST_ASSERT_MSG_EQ(csv.is_open(),
                                  true,
                                  "missing probe-owd.csv for mode=" << m << " at " << csvPath);

            std::string line;
            std::getline(csv, line);
            NS_TEST_ASSERT_MSG_EQ(line,
                                  std::string("t_us,owd_ms"),
                                  "unexpected header in probe-owd.csv for mode=" << m << ": "
                                                                                 << line);

            size_t rows = 0;
            while (std::getline(csv, line))
            {
                ++rows;
            }
            NS_TEST_ASSERT_MSG_GT_OR_EQ(rows,
                                        static_cast<size_t>(300),
                                        "mode=" << m << " probe-owd.csv has " << rows
                                                << " data rows (need >= 300)");
        }

        std::filesystem::remove_all(tmpDir);
    }
};

/**
 * Compositional-safety fixture for the l4s-fqcodel-classic composition.
 *
 * The mixed-traffic cell (5 TcpDctcp ECT(1) + 5 TcpCubic ECT(0), N=10,
 * simTime=30 s) asserts that the DualPI2 PI controller engages within
 * bounded equilibrium when FqCoDel-on-classic handles the classic
 * sub-queue. The L4S lane (DualPI2 + coupled drop) remains architecturally
 * active in this mode per the binary's own docs ("L4S lane still uses the
 * DualPI2 P.I controller + coupled drop"); the test verifies the
 * controller is engaged within reasonable bounds AND all flows reach the
 * sink — NOT that the controller is silent (the original "quiescent"
 * assertion was calibrated against a dormant-controller defect that has
 * since been fixed; bounds were recalibrated on 2026-05-24).
 *
 * Five assertions:
 *   1. L4 lane is functional: probe-owd.csv contains >= 300 rows.
 *   2. coupling-counters.csv schema is honoured and has >= 20 samples.
 *   3. ENGAGEMENT: coupledDropCount within [100, 5000] AND mean(p_prime)
 *      within [0.02, 0.15] — bracket-bounded equilibrium. The lower
 *      bounds guard against regression to the dormant-controller state;
 *      the upper bounds guard against over-engagement. Bounds derived
 *      from 5-seed sweep: coupledDropCount mean 2110 ± 82, meanPPrime
 *      mean 0.078 ± 0.004 (~±2σ + safety margin).
 *   4. All 10 flows reach the sink with goodput > 0.1 Mbps — TCP handshake
 *      and steady-state confirmed on both ECT(1) and ECT(0) lanes. This
 *      is the actual compositional-safety property (no flow starves).
 */
class DsL4sScenarioFqCoDelClassicCompositionalSafetyTest : public TestCase
{
  public:
    DsL4sScenarioFqCoDelClassicCompositionalSafetyTest()
        : TestCase("L4S fqcodel-classic mixed-cell: DualPI2 engages within bounded equilibrium "
                   "(coupled drops + p' inside calibrated brackets; all flows reach sink)")
    {
    }

  private:
    void DoRun() override
    {
        const std::string tmpDir =
            std::filesystem::temp_directory_path().string() + "/v1.14-fqcodel-classic-safety";
        std::filesystem::remove_all(tmpDir);
        std::filesystem::create_directories(tmpDir);

        const std::string cmd = "./ns3 run \"diffserv-l4s-fqcodel-comparison"
                                " --mode=l4s-fqcodel-classic --mixedTraffic=true"
                                " --bulkSenders=10 --simTime=30 --warmup=5"
                                " --output=" +
                                tmpDir + "\" 2>&1";
        std::string out;
        const int rc = RunCapture(cmd, out);
        NS_TEST_ASSERT_MSG_EQ(rc,
                              0,
                              "diffserv-l4s-fqcodel-comparison failed in mixed cell: " << out);

        const std::string cellDir = tmpDir + "/l4s-fqcodel-classic/mixed";

        // --- Assertion 1: L4 lane is functional ---
        {
            std::ifstream csv(cellDir + "/probe-owd.csv");
            NS_TEST_ASSERT_MSG_EQ(csv.is_open(), true, "missing probe-owd.csv");
            std::string line;
            std::getline(csv, line); // header
            NS_TEST_ASSERT_MSG_EQ(line,
                                  std::string("t_us,owd_ms"),
                                  "probe-owd.csv header drift: " << line);
            size_t rows = 0;
            while (std::getline(csv, line))
            {
                ++rows;
            }
            NS_TEST_ASSERT_MSG_GT_OR_EQ(rows,
                                        static_cast<size_t>(300),
                                        "probe-owd.csv has " << rows
                                                             << " rows < 300 "
                                                                "- L4 lane appears non-functional");
        }

        // --- Assertion 2: coupling-counters schema + row floor ---
        const std::string ccPath = cellDir + "/coupling-counters.csv";
        std::ifstream cc(ccPath);
        NS_TEST_ASSERT_MSG_EQ(cc.is_open(), true, "missing coupling-counters.csv at " << ccPath);
        std::string header;
        std::getline(cc, header);
        NS_TEST_ASSERT_MSG_EQ(header,
                              std::string("t_us,p_prime,p_C,coupledDropCount"),
                              "coupling-counters.csv header drift: " << header);

        // --- Read all coupling samples ---
        std::vector<double> pPrimes;
        uint64_t lastDropCount = 0;
        size_t nSamples = 0;
        std::string line;
        while (std::getline(cc, line))
        {
            std::stringstream ss(line);
            std::string field;
            std::getline(ss, field, ','); // t_us
            std::getline(ss, field, ',');
            double pPrime = std::stod(field);
            std::getline(ss, field, ','); // p_C (unused for safety assertion)
            std::getline(ss, field, ',');
            lastDropCount = std::stoull(field);
            pPrimes.push_back(pPrime);
            ++nSamples;
        }
        NS_TEST_ASSERT_MSG_GT_OR_EQ(nSamples,
                                    static_cast<size_t>(20),
                                    "coupling-counters.csv has "
                                        << nSamples
                                        << " samples < 20 (expect ~28-30 at 1 Hz over 30 s)");

        // --- Assertion 3: ENGAGEMENT — bracket-bounded equilibrium ---
        // Bounds derived from 5-seed sweep (2026-06-10, after the RFC 9332
        // §2.1 eq. (1) coupling-map correction p_C = p'^2):
        //   coupledDropCount sweep:  mean 2136 ± 58  → bracket [100, 5000]
        //     (statistically unchanged from the 2026-05-24 pre-correction
        //     sweep's 2110 ± 82 — the classic drop RATE is load-determined,
        //     so correcting the p' -> p_C map rescales p', not the drops)
        //   mean(p_prime) sweep:     mean 0.169 ± 0.004 → bracket [0.04, 0.34]
        //     (pre-correction sweep was 0.078 ± 0.004 with bracket
        //     [0.02, 0.15]; delivering the same equilibrium p_C under
        //     p_C = p'^2 instead of (k*p')^2 doubles p' — same bracket
        //     shape, lower ~= mean/4, upper ~= 2x mean)
        // Lower bounds guard against regression to the dormant-controller
        // defect (Final p' = 0). Upper bounds guard against over-engagement
        // (would indicate controller wind-up). The original
        // `coupledDropCount == 0` assertion was calibrated against the
        // dormant-controller defect, not against architectural intent.
        NS_TEST_ASSERT_MSG_GT_OR_EQ(lastDropCount,
                                    static_cast<uint64_t>(100),
                                    "coupledDropCount = " << lastDropCount
                                                          << " < 100 — DualPI2 not engaging "
                                                             "(possible regression to dormant "
                                                             "controller state)");
        NS_TEST_ASSERT_MSG_LT(lastDropCount,
                              static_cast<uint64_t>(5000),
                              "coupledDropCount = " << lastDropCount
                                                    << " > 5000 — DualPI2 over-engaging "
                                                       "(possible controller wind-up; "
                                                       "5-seed sweep mean 2136 ± 58)");

        double sumPPrime = 0.0;
        for (double v : pPrimes)
        {
            sumPPrime += v;
        }
        const double meanPPrime = pPrimes.empty() ? 0.0 : sumPPrime / pPrimes.size();
        NS_TEST_ASSERT_MSG_GT_OR_EQ(meanPPrime,
                                    0.04,
                                    "mean(p_prime) = " << meanPPrime
                                                       << " < 0.04 — controller not integrating "
                                                          "(possible regression to dormant "
                                                          "controller state)");
        NS_TEST_ASSERT_MSG_LT(meanPPrime,
                              0.34,
                              "mean(p_prime) = " << meanPPrime
                                                 << " > 0.34 — controller over-integrating "
                                                    "(5-seed sweep mean 0.169 ± 0.004)");

        // --- Assertion 4: All flows reach sink ---
        std::ifstream bg(cellDir + "/bulk-goodput.csv");
        NS_TEST_ASSERT_MSG_EQ(bg.is_open(), true, "missing bulk-goodput.csv");
        std::getline(bg, header);
        NS_TEST_ASSERT_MSG_EQ(header,
                              std::string("flow_id,goodput_mbps"),
                              "bulk-goodput.csv header drift: " << header);
        std::vector<double> goodputs;
        while (std::getline(bg, line))
        {
            std::stringstream ss(line);
            std::string field;
            std::getline(ss, field, ','); // flow_id
            std::getline(ss, field, ',');
            goodputs.push_back(std::stod(field));
        }
        NS_TEST_ASSERT_MSG_EQ(goodputs.size(),
                              static_cast<size_t>(10),
                              "bulk-goodput.csv has " << goodputs.size() << " flows, expected 10");
        for (size_t i = 0; i < goodputs.size(); ++i)
        {
            NS_TEST_ASSERT_MSG_GT(goodputs[i],
                                  0.1,
                                  "flow " << i << " goodput = " << goodputs[i]
                                          << " Mbps < 0.1 Mbps - TCP failed to establish "
                                             "on one or both lanes");
        }

        std::filesystem::remove_all(tmpDir);
    }
};

/**
 * Parity validation: the in-tree l4s::QueueDisc and the patches/ns3/-vendored
 * ns3::DualPi2QueueDisc (Veras et al., arXiv:2603.20166v1) produce
 * Jain's Fairness Index agreement within 0.01 at the canonical
 * 40 Mbps × 50 ms cell under identical conditions (DCTCP-ECT(1) +
 * TcpCubic-ECT(0), 30 s sim, 5 s warmup), on every one of three RNG
 * seeds — both the parity delta and the JFI floor are per-seed guards,
 * not means. The mixed scalable + classic workload exercises the
 * coupling cascade, so this is a live cross-check of the RFC 9332
 * App. A coupling arithmetic against the Linux-aligned reference
 * implementation, not just of the PI core.
 */
class DsL4sScenarioDualPi2GprtParityTest : public TestCase
{
  public:
    DsL4sScenarioDualPi2GprtParityTest()
        : TestCase("L4S parity: QueueDisc and ns3::DualPi2QueueDisc agree on "
                   "JFI within 0.01 under identical DCTCP+Cubic conditions, "
                   "on every seed")
    {
    }

  private:
    void RunOneAndParseJfi(const std::string& rootQdisc,
                           uint32_t rngRun,
                           const std::string& outDir,
                           double& jfiOut)
    {
        std::filesystem::remove_all(outDir);
        std::filesystem::create_directories(outDir);

        const std::string cmd = "./ns3 run --no-build "
                                "\"diffserv-l4s-dualpi2-gprt-parity "
                                "--rootQdisc=" +
                                rootQdisc +
                                " --bottleneckRate=40Mbps "
                                "--baseRttMs=50 "
                                "--l4sThresholdMs=1 "
                                "--simTime=30 "
                                "--warmup=5 "
                                "--rngRun=" +
                                std::to_string(rngRun) +
                                " "
                                "--outDir=" +
                                outDir + "\" 2>&1";
        std::string out;
        const int rc = RunCapture(cmd, out);
        NS_TEST_ASSERT_MSG_EQ(rc,
                              0,
                              "parity binary failed (rootQdisc=" << rootQdisc << "): " << out);

        std::ifstream in(outDir + "/summary.txt");
        NS_TEST_ASSERT_MSG_EQ(in.is_open(), true, "summary.txt missing: " << outDir);
        std::string line;
        double jfi = -1.0;
        while (std::getline(in, line))
        {
            if (line.rfind("jfi=", 0) == 0)
            {
                try
                {
                    jfi = std::stod(line.substr(4));
                }
                catch (...)
                {
                    jfi = -1.0;
                }
                break;
            }
        }
        NS_TEST_ASSERT_MSG_GT(jfi, 0.0, "Could not parse jfi from " << outDir << "/summary.txt");
        jfiOut = jfi;
    }

    void DoRun() override
    {
        const std::string baseTmp =
            std::filesystem::temp_directory_path().string() + "/test-l4s-dualpi2-gprt-parity";

        constexpr uint32_t kSeeds[] = {1, 2, 3};
        constexpr double kJfiTolerance = 0.01;
        constexpr double kJfiFloor = 0.8;

        for (uint32_t seed : kSeeds)
        {
            const std::string l4sDir = baseTmp + "/l4s-run" + std::to_string(seed);
            const std::string gprtDir = baseTmp + "/gprt-run" + std::to_string(seed);

            double jfiL4s = -1.0;
            double jfiGprt = -1.0;
            RunOneAndParseJfi("l4s", seed, l4sDir, jfiL4s);
            RunOneAndParseJfi("gprt", seed, gprtDir, jfiGprt);
            const double delta = (jfiL4s > jfiGprt) ? jfiL4s - jfiGprt : jfiGprt - jfiL4s;

            // S-L4S-PARITYSUM,<seed>,<jfi_l4s>,<jfi_gprt>,<delta> — audit harvest
            std::ostringstream paritySum;
            paritySum << "S-L4S-PARITYSUM," << seed << "," << jfiL4s << "," << jfiGprt << ","
                      << delta;
            std::cout << paritySum.str() << std::endl;

            NS_TEST_ASSERT_MSG_LT(delta,
                                  kJfiTolerance,
                                  "JFI parity delta exceeds " << kJfiTolerance << " at rngRun="
                                                              << seed << "; l4s=" << jfiL4s
                                                              << ", gprt=" << jfiGprt
                                                              << ", delta=" << delta);
            NS_TEST_ASSERT_MSG_GT(jfiL4s,
                                  kJfiFloor,
                                  "L4S JFI below floor at rngRun=" << seed << ": " << jfiL4s);
            NS_TEST_ASSERT_MSG_GT(jfiGprt,
                                  kJfiFloor,
                                  "GPRT JFI below floor at rngRun=" << seed << ": " << jfiGprt);
        }

        std::filesystem::remove_all(baseTmp);
    }
};

// CAKE diffserv4 with a per-tin DualPI2 inner must keep a scalable (DCTCP)
// and a classic (Cubic) flow within a fair throughput band, matching the
// bare DualPI2 behaviour the parity test pins. Guards against a regression
// where the composition collapses coupling into one-sided starvation.
class DsL4sScenarioCakeCompositionFairnessTest : public TestCase
{
  public:
    DsL4sScenarioCakeCompositionFairnessTest()
        : TestCase("L4S+CAKE composition: CAKE diffserv4 over a per-tin DualPI2 "
                   "inner holds DCTCP+Cubic within a fair JFI band")
    {
    }

  private:
    void DoRun() override
    {
        const std::string outDir =
            std::filesystem::temp_directory_path().string() + "/test-l4s-cake-composition-fairness";
        std::filesystem::remove_all(outDir);
        std::filesystem::create_directories(outDir);

        const std::string cmd = "./ns3 run --no-build "
                                "\"diffserv-l4s-dualpi2-gprt-parity "
                                "--rootQdisc=cake "
                                "--bottleneckRate=40Mbps "
                                "--baseRttMs=50 "
                                "--l4sThresholdMs=1 "
                                "--simTime=30 "
                                "--warmup=5 "
                                "--rngRun=1 "
                                "--outDir=" +
                                outDir + "\" 2>&1";
        std::string out;
        const int rc = RunCapture(cmd, out);
        NS_TEST_ASSERT_MSG_EQ(rc, 0, "cake composition binary failed: " << out);

        std::ifstream in(outDir + "/summary.txt");
        NS_TEST_ASSERT_MSG_EQ(in.is_open(), true, "summary.txt missing: " << outDir);
        std::string line;
        double jfi = -1.0;
        while (std::getline(in, line))
        {
            if (line.rfind("jfi=", 0) == 0)
            {
                try
                {
                    jfi = std::stod(line.substr(4));
                }
                catch (...)
                {
                    jfi = -1.0;
                }
                break;
            }
        }
        NS_TEST_ASSERT_MSG_GT(jfi, 0.0, "Could not parse jfi from " << outDir << "/summary.txt");
        // Observed JFI ~0.98-1.00 across seeds at this regime (bare DualPI2
        // reaches ~1.0); the 0.90 floor catches a coupling collapse without
        // being brittle to seed-level variation.
        NS_TEST_ASSERT_MSG_GT(jfi, 0.90, "CAKE composition JFI below fair band: " << jfi);

        std::filesystem::remove_all(outDir);
    }
};

// CAKE diffserv4 with a per-tin DualPI2 inner must reach aggregate goodput
// comparable to a bare DualPI2 inner under identical conditions, not just be
// fair. An earlier defect left the per-tin inner at raw construction defaults
// (a shallow 25-packet WRED classic queue with no starvation-safe scheduler),
// well below the bandwidth-delay product, so both responsive flows were held
// to a fraction of the link rate while remaining fair to each other — total
// goodput collapsed to ~38 % of the bare inner. This test runs the bare
// (l4s) and CAKE-composed inners through the same harness and asserts the
// CAKE total stays within a band of the bare total.
class DsL4sScenarioCakeCompositionThroughputParityTest : public TestCase
{
  public:
    DsL4sScenarioCakeCompositionThroughputParityTest()
        : TestCase("L4S+CAKE composition: CAKE diffserv4 over a per-tin DualPI2 "
                   "inner reaches aggregate goodput comparable to a bare DualPI2 inner")
    {
    }

  private:
    // Run one rootQdisc through the parity harness and report total goodput
    // (L + C) via the out-param. The ns-3 test-assertion macros expand to a
    // bare `return;`, so they can only be used from a void-returning method;
    // mirrors RunOneAndParseJfi above.
    void RunTotalGoodput(const std::string& rootQdisc, const std::string& outDir, double& totalOut)
    {
        std::filesystem::remove_all(outDir);
        std::filesystem::create_directories(outDir);

        const std::string cmd = "./ns3 run --no-build "
                                "\"diffserv-l4s-dualpi2-gprt-parity "
                                "--rootQdisc=" +
                                rootQdisc +
                                " --bottleneckRate=40Mbps "
                                "--baseRttMs=50 "
                                "--l4sThresholdMs=1 "
                                "--simTime=30 "
                                "--warmup=5 "
                                "--rngRun=1 "
                                "--outDir=" +
                                outDir + "\" 2>&1";
        std::string out;
        const int rc = RunCapture(cmd, out);
        NS_TEST_ASSERT_MSG_EQ(rc,
                              0,
                              "parity binary failed (rootQdisc=" << rootQdisc << "): " << out);

        std::ifstream in(outDir + "/summary.txt");
        NS_TEST_ASSERT_MSG_EQ(in.is_open(), true, "summary.txt missing: " << outDir);
        std::string line;
        double gL = -1.0;
        double gC = -1.0;
        while (std::getline(in, line))
        {
            if (line.rfind("goodput_L_mbps=", 0) == 0)
            {
                gL = std::stod(line.substr(std::string("goodput_L_mbps=").size()));
            }
            else if (line.rfind("goodput_C_mbps=", 0) == 0)
            {
                gC = std::stod(line.substr(std::string("goodput_C_mbps=").size()));
            }
        }
        NS_TEST_ASSERT_MSG_GT(gL, -0.5, "Could not parse goodput_L_mbps from " << outDir);
        NS_TEST_ASSERT_MSG_GT(gC, -0.5, "Could not parse goodput_C_mbps from " << outDir);
        totalOut = gL + gC;
    }

    void DoRun() override
    {
        const std::string base =
            std::filesystem::temp_directory_path().string() + "/test-l4s-cake-throughput-parity";
        double bareTotal = -1.0;
        double cakeTotal = -1.0;
        RunTotalGoodput("l4s", base + "/l4s", bareTotal);
        RunTotalGoodput("cake", base + "/cake", cakeTotal);

        NS_TEST_ASSERT_MSG_GT(bareTotal,
                              1.0,
                              "Bare DualPI2 total goodput suspiciously low: " << bareTotal);

        // Post-fix the CAKE-composed inner reaches full parity (ratio ~1.0)
        // with the bare inner; the 0.85 floor leaves headroom for seed and
        // sim-length variation while still catching the ~0.38 collapse the
        // raw-default inner produced.
        const double floor = 0.85 * bareTotal;
        NS_TEST_ASSERT_MSG_GT(cakeTotal,
                              floor,
                              "CAKE composition aggregate goodput " << cakeTotal << " Mbps below "
                                                                    << floor << " Mbps (0.85x bare "
                                                                    << bareTotal << " Mbps)");

        std::filesystem::remove_all(base);
    }
};

} // namespace ns3::stratum
