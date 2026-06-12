/*
 * Copyright (C) 2026 Sergio Andreozzi
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Instrumentation test suite for diffserv-example-1.
 * Smoke-runs the example and asserts that per-class trace files are
 * emitted with well-formed, non-empty content.
 */

#include "ns3/core-module.h"
#include "ns3/system-path.h"
#include "ns3/test.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

using namespace ns3;

namespace
{

namespace fs = std::filesystem;

// Output directory for all smoke runs in this suite.
constexpr const char* kSmokeOutDir = "/tmp/v1.5-test-smoke-example-1";

// Scheduler and packet-size used for all smoke runs.
constexpr const char* kSmokeScheduler = "PQ";
constexpr int kSmokePktSize = 512;
constexpr int kSmokeSimTime = 6;

/// Returns the run subdirectory created by the example for the smoke params.
/// Format mirrors diffserv-example-1 runDir logic: <outputDir>/<sched>-<pktsize4d>.
std::string
SmokeRunDir()
{
    // Zero-pad packet size to 4 digits (matching the example's formatting).
    char padded[5];
    std::snprintf(padded, sizeof(padded), "%04d", kSmokePktSize);
    return std::string(kSmokeOutDir) + "/" + kSmokeScheduler + "-" + padded;
}

/// Returns the ns-3 root directory of the *calling* test session, derived from
/// the running test-runner binary's location. The test-runner lives at
/// `<peer>/build/utils/ns3*-test-runner-default`, so walking up two levels
/// from its directory yields `<peer>` — the directory containing the `./ns3`
/// wrapper script. Using this instead of a hardcoded path makes the smoke
/// harness work in any session's worktree, including the per-session ns-3-dev
/// peers used during parallel development.
std::string
PeerRoot()
{
    return fs::path(SystemPath::FindSelfDirectory()).parent_path().parent_path().string();
}

/// Invokes the example as a subprocess and returns true on exit-code 0.
bool
RunSmoke()
{
    fs::remove_all(kSmokeOutDir);
    std::string cmd = "cd " + PeerRoot() +
                      " && "
                      "./ns3 run \"diffserv-example-1 --scheduler=" +
                      std::string(kSmokeScheduler) +
                      " --packetSize=" + std::to_string(kSmokePktSize) +
                      " --simTime=" + std::to_string(kSmokeSimTime) +
                      " --outputDir=" + std::string(kSmokeOutDir) + "\" > /dev/null 2>&1";
    return std::system(cmd.c_str()) == 0;
}

/// Checks that a trace file exists, contains at least one row, and has
/// monotone non-decreasing time stamps.  Returns {row_count, is_monotone}.
std::pair<int, bool>
CheckMonotone(const fs::path& p)
{
    std::ifstream f(p);
    double lastT = -1.0;
    double t = 0.0;
    double v = 0.0;
    int rows = 0;
    while (f >> t >> v)
    {
        if (lastT >= 0.0 && t < lastT)
        {
            return {rows, false};
        }
        lastT = t;
        ++rows;
    }
    return {rows, true};
}

/// Parses a CSV with header. Returns row count, header string, and the
/// set of distinct classId values found in the given column index.
struct CsvSummary
{
    int rows = 0;
    std::string header;
    std::set<uint32_t> classIds;
};

CsvSummary
SummariseCsv(const fs::path& p, int classIdColIndex)
{
    CsvSummary s;
    std::ifstream f(p);
    std::getline(f, s.header);
    std::string line;
    while (std::getline(f, line))
    {
        std::vector<std::string> tok;
        size_t start = 0;
        size_t pos;
        while ((pos = line.find(',', start)) != std::string::npos)
        {
            tok.push_back(line.substr(start, pos - start));
            start = pos + 1;
        }
        tok.push_back(line.substr(start));
        if (static_cast<int>(tok.size()) > classIdColIndex)
        {
            s.classIds.insert(static_cast<uint32_t>(std::stoi(tok[classIdColIndex])));
        }
        ++s.rows;
    }
    return s;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Fixture: per-class OWD files emitted and well-formed
// ---------------------------------------------------------------------------

/**
 * Smoke-runs diffserv-example-1 and verifies that OWD-ef.tr and OWD-be.tr
 * are produced in the run directory, are non-empty, and have monotone time.
 */
class TestSExample1PerClassOwd : public TestCase
{
  public:
    TestSExample1PerClassOwd()
        : TestCase("S-example-1-perclass-owd: OWD-ef.tr and OWD-be.tr emitted and well-formed")
    {
    }

  private:
    void DoRun() override
    {
        NS_TEST_ASSERT_MSG_EQ(RunSmoke(), true, "Example smoke run must succeed");

        const fs::path runDir = SmokeRunDir();
        const fs::path efPath = runDir / "OWD-ef.tr";
        const fs::path bePath = runDir / "OWD-be.tr";

        NS_TEST_ASSERT_MSG_EQ(fs::exists(efPath), true, "OWD-ef.tr must exist after smoke run");
        NS_TEST_ASSERT_MSG_EQ(fs::exists(bePath), true, "OWD-be.tr must exist after smoke run");

        auto ef = CheckMonotone(efPath);
        NS_TEST_ASSERT_MSG_GT(ef.first, 0, "OWD-ef.tr must contain at least one row");
        NS_TEST_ASSERT_MSG_EQ(ef.second,
                              true,
                              "OWD-ef.tr time field must be monotone non-decreasing");

        auto be = CheckMonotone(bePath);
        NS_TEST_ASSERT_MSG_GT(be.first, 0, "OWD-be.tr must contain at least one row");
        NS_TEST_ASSERT_MSG_EQ(be.second,
                              true,
                              "OWD-be.tr time field must be monotone non-decreasing");
    }
};

// ---------------------------------------------------------------------------
// Fixture: per-window FlowRate.csv schema and content
// ---------------------------------------------------------------------------

/**
 * Smoke-runs diffserv-example-1 and verifies that FlowRate.csv is produced
 * with the correct header schema, at least 10 data rows, and class IDs for
 * both EF (0) and BE (1).
 */
class TestSExample1PerClassFlowRate : public TestCase
{
  public:
    TestSExample1PerClassFlowRate()
        : TestCase("S-example-1-perclass-flowrate: FlowRate.csv emitted with valid schema")
    {
    }

  private:
    void DoRun() override
    {
        NS_TEST_ASSERT_MSG_EQ(RunSmoke(), true, "Example smoke run must succeed");

        fs::path p = fs::path(SmokeRunDir()) / "FlowRate.csv";
        NS_TEST_ASSERT_MSG_EQ(fs::exists(p), true, "FlowRate.csv must exist");

        auto s = SummariseCsv(p, /*classIdColIndex=*/1);
        NS_TEST_ASSERT_MSG_EQ(s.header,
                              std::string("time,classId,rate_kbps"),
                              "Header must match documented schema");
        NS_TEST_ASSERT_MSG_GT_OR_EQ(
            s.rows,
            10,
            "At least 10 data rows expected after a 6-second sim at 100 ms cadence");
        NS_TEST_ASSERT_MSG_EQ(s.classIds.count(0), 1u, "EF class (0) must be present");
        NS_TEST_ASSERT_MSG_EQ(s.classIds.count(1), 1u, "BE class (1) must be present");
    }
};

// ---------------------------------------------------------------------------
// Fixture: MeterColour.csv schema and non-zero colour events
// ---------------------------------------------------------------------------

/**
 * Smoke-runs diffserv-example-1 and verifies that MeterColour.csv is produced
 * with the correct header schema, at least 10 data rows, and at least one
 * window with a non-zero colour count (the meter actually fired).
 */
class TestSExample1MeterColourAggregate : public TestCase
{
  public:
    TestSExample1MeterColourAggregate()
        : TestCase("S-example-1-metercolour-aggregate: MeterColour.csv emitted with valid schema")
    {
    }

  private:
    void DoRun() override
    {
        NS_TEST_ASSERT_MSG_EQ(RunSmoke(), true, "Example smoke run must succeed");

        fs::path p = fs::path(SmokeRunDir()) / "MeterColour.csv";
        NS_TEST_ASSERT_MSG_EQ(fs::exists(p), true, "MeterColour.csv must exist");

        auto s = SummariseCsv(p, /*classIdColIndex=*/1);
        NS_TEST_ASSERT_MSG_EQ(s.header,
                              std::string("time,classId,green,yellow,red"),
                              "Header must match documented schema");
        NS_TEST_ASSERT_MSG_GT_OR_EQ(
            s.rows,
            10,
            "At least 10 data rows expected after a 6-second sim at 100 ms cadence");

        // Verify at least one row has non-zero colour counts (meter fired).
        std::ifstream f(p);
        std::string line;
        std::getline(f, line); // skip header
        uint64_t totalEvents = 0;
        while (std::getline(f, line))
        {
            // parse: time,classId,green,yellow,red — sum cols 2,3,4
            size_t c1 = line.find(',');
            size_t c2 = line.find(',', c1 + 1);
            size_t c3 = line.find(',', c2 + 1);
            size_t c4 = line.find(',', c3 + 1);
            const uint32_t g = std::stoi(line.substr(c2 + 1, c3 - c2 - 1));
            const uint32_t y = std::stoi(line.substr(c3 + 1, c4 - c3 - 1));
            const uint32_t r = std::stoi(line.substr(c4 + 1));
            totalEvents += g + y + r;
        }
        NS_TEST_ASSERT_MSG_GT(totalEvents,
                              0u,
                              "Total colour events across all windows must be > 0 (meter fired)");
    }
};

// ---------------------------------------------------------------------------
// Test suite registration
// ---------------------------------------------------------------------------

/**
 * Test suite for diffserv-example-1 per-class instrumentation.
 */
class DiffservExample1InstrumentationTestSuite : public TestSuite
{
  public:
    DiffservExample1InstrumentationTestSuite()
        : TestSuite("stratum-example-1-instrumentation", Type::SYSTEM)
    {
        AddTestCase(new TestSExample1PerClassOwd(), TestCase::Duration::EXTENSIVE);
        AddTestCase(new TestSExample1PerClassFlowRate(), TestCase::Duration::EXTENSIVE);
        AddTestCase(new TestSExample1MeterColourAggregate(), TestCase::Duration::EXTENSIVE);
    }
};

static DiffservExample1InstrumentationTestSuite g_diffservExample1InstrumentationTestSuite;
