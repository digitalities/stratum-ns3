/*
 * Copyright (C) 2026 Sergio Andreozzi
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 */

#include "ns3/core-module.h"
#include "ns3/internet-stack-helper.h"
#include "ns3/ipv6-address-helper.h"
#include "ns3/ipv6-interface-container.h"
#include "ns3/node-container.h"
#include "ns3/point-to-point-helper.h"
#include "ns3/stratum-flent-csv-sink.h"
#include "ns3/system-path.h"
#include "ns3/test.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

using namespace ns3;
using namespace ns3::stratum;

namespace
{
namespace fs = std::filesystem;

const std::string kSmokeDirA = "/tmp/v1.6-flent-sink-smoke-host-attribution";
const std::string kSmokeDirB = "/tmp/v1.6-flent-sink-smoke-tcp-4up";

/// Returns the ns-3 root directory of the *calling* test session, derived from
/// the running test-runner binary's location. Test-runner lives at
/// `<peer>/build/utils/ns3*-test-runner-default`, so walking up two levels
/// from its directory yields `<peer>`. Using this instead of a hardcoded path
/// makes the smoke harness work in any session's worktree without needing
/// a hardcoded primary-checkout path.
std::string
PeerRoot()
{
    return fs::path(SystemPath::FindSelfDirectory()).parent_path().parent_path().string();
}

bool
RunHostAttributionSmoke()
{
    fs::remove_all(kSmokeDirA);
    std::string cmd = "cd " + PeerRoot() +
                      " && "
                      "./ns3 run \"cake-flent-host-attribution-smoke "
                      "--bw=100Mbps --rtt=40ms --length=2 --output=" +
                      kSmokeDirA + "/\" > /dev/null 2>&1";
    return std::system(cmd.c_str()) == 0;
}

bool
RunTcp4UpSquarewaveSmoke()
{
    fs::remove_all(kSmokeDirB);
    std::string cmd = "cd " + PeerRoot() +
                      " && "
                      "./ns3 run \"cake-tcp-4up-squarewave --bw=100Mbps --rtt=40ms "
                      "--length=2 --output=" +
                      kSmokeDirB + "/\" > /dev/null 2>&1";
    return std::system(cmd.c_str()) == 0;
}

std::string
ReadFirstLine(const fs::path& p)
{
    std::ifstream f(p);
    std::string line;
    std::getline(f, line);
    return line;
}

} // anonymous namespace

/**
 * @ingroup stratum-tests
 *
 * Tests that FlentCsvSink emits a `host` column header in tcp_up_flow*.csv
 * files when the example is run with host-isolation mode.
 */
class TestSFlentSinkHostColumnEmitted : public TestCase
{
  public:
    TestSFlentSinkHostColumnEmitted()
        : TestCase(
              "S-flent-sink-host-column-emitted: tcp_up_flow*.csv carries `host` column header")
    {
    }

  private:
    void DoRun() override
    {
        NS_TEST_ASSERT_MSG_EQ(RunHostAttributionSmoke(),
                              true,
                              "cake-flent-host-attribution-smoke run must succeed");
        for (int i = 0; i < 5; ++i)
        {
            fs::path p = fs::path(kSmokeDirA) / ("tcp_up_flow" + std::to_string(i) + ".csv");
            NS_TEST_ASSERT_MSG_EQ(fs::exists(p),
                                  true,
                                  "tcp_up_flow" + std::to_string(i) + ".csv must exist");
            std::string hdr = ReadFirstLine(p);
            NS_TEST_ASSERT_MSG_EQ(hdr,
                                  std::string("t,bytes_delta,goodput_mbps,host"),
                                  "header must include `host` column");
        }
    }
};

/**
 * @ingroup stratum-tests
 *
 * Tests that flows 0-3 carry host=A and flow 4 carries host=B in every data row
 * of tcp_up_flow*.csv, as attributed by cake-flent-host-attribution-smoke.
 */
class TestSFlentSinkHostAttributionCorrect : public TestCase
{
  public:
    TestSFlentSinkHostAttributionCorrect()
        : TestCase("S-flent-sink-host-attribution-correct: flow0-3 host=A; flow4 host=B")
    {
    }

  private:
    void DoRun() override
    {
        NS_TEST_ASSERT_MSG_EQ(RunHostAttributionSmoke(), true, "smoke must succeed");

        auto checkAllRowsHaveHost = [](const fs::path& p,
                                       const std::string& expected) -> std::pair<int, std::string> {
            std::ifstream f(p);
            std::string header;
            std::getline(f, header); // skip header
            std::string line;
            int rows = 0;
            while (std::getline(f, line))
            {
                std::size_t pos = line.find_last_of(',');
                std::string lastField =
                    (pos == std::string::npos) ? std::string("") : line.substr(pos + 1);
                if (lastField != expected)
                {
                    return std::make_pair(rows, lastField);
                }
                ++rows;
            }
            return std::make_pair(rows, std::string("OK"));
        };

        for (int i = 0; i < 4; ++i)
        {
            auto res = checkAllRowsHaveHost(fs::path(kSmokeDirA) /
                                                ("tcp_up_flow" + std::to_string(i) + ".csv"),
                                            "A");
            NS_TEST_ASSERT_MSG_EQ(res.second,
                                  std::string("OK"),
                                  "tcp_up_flow" + std::to_string(i) +
                                      ".csv row should have host=A; found '" + res.second + "'");
            NS_TEST_ASSERT_MSG_GT(res.first, 0, "must have at least 1 data row");
        }
        auto res4 = checkAllRowsHaveHost(fs::path(kSmokeDirA) / "tcp_up_flow4.csv", "B");
        NS_TEST_ASSERT_MSG_EQ(res4.second,
                              std::string("OK"),
                              "tcp_up_flow4.csv row should have host=B; found '" + res4.second +
                                  "'");
        NS_TEST_ASSERT_MSG_GT(res4.first, 0, "must have at least 1 data row");
    }
};

/**
 * @ingroup stratum-tests
 *
 * Tests that legacy callers (those that do not pass a hostId) still compile and
 * produce CSV files with the `host` column header and empty host field per row.
 */
class TestSFlentSinkBackwardsCompatNoHostid : public TestCase
{
  public:
    TestSFlentSinkBackwardsCompatNoHostid()
        : TestCase(
              "S-flent-sink-backwards-compat-no-hostid: legacy callers emit empty `host` field")
    {
    }

  private:
    void DoRun() override
    {
        NS_TEST_ASSERT_MSG_EQ(RunTcp4UpSquarewaveSmoke(),
                              true,
                              "cake-tcp-4up-squarewave smoke must succeed");
        bool found = false;
        for (auto& entry : fs::directory_iterator(kSmokeDirB))
        {
            if (entry.path().filename().string().find("tcp_up_flow") == 0)
            {
                found = true;
                std::string hdr = ReadFirstLine(entry.path());
                NS_TEST_ASSERT_MSG_EQ(hdr,
                                      std::string("t,bytes_delta,goodput_mbps,host"),
                                      "header must include `host` column even for legacy callers");
                std::ifstream f(entry.path());
                std::string line;
                std::getline(f, line); // skip header
                if (std::getline(f, line))
                {
                    NS_TEST_ASSERT_MSG_EQ(
                        line.back(),
                        ',',
                        "legacy caller data row should end with comma (empty host field), got: '" +
                            line + "'");
                }
                break;
            }
        }
        NS_TEST_ASSERT_MSG_EQ(found, true, "expected at least one tcp_up_flow*.csv in smoke dir");
    }
};

/**
 * @ingroup stratum-tests
 * @brief A FlentUdpProbe client/server pair exchanges echoes over IPv6 and the
 *        client records at least one RTT sample.
 */
class TestSFlentSinkIpv6LatencySample : public TestCase
{
  public:
    TestSFlentSinkIpv6LatencySample()
        : TestCase("SFlentSinkIpv6LatencySample")
    {
    }

  private:
    uint32_t m_samples{0};

    void OnRtt(uint16_t, Time)
    {
        ++m_samples;
    }

    void DoRun() override
    {
        NodeContainer nodes;
        nodes.Create(2);

        PointToPointHelper p2p;
        p2p.SetDeviceAttribute("DataRate", StringValue("10Mbps"));
        p2p.SetChannelAttribute("Delay", StringValue("5ms"));
        NetDeviceContainer devs = p2p.Install(nodes);

        InternetStackHelper internet;
        internet.Install(nodes);

        Ipv6AddressHelper ipv6;
        ipv6.SetBase(Ipv6Address("2001:db8::"), Ipv6Prefix(64));
        Ipv6InterfaceContainer ifs = ipv6.Assign(devs);

        const uint16_t port = 9999;

        Ptr<FlentUdpProbeServer> server = CreateObject<FlentUdpProbeServer>();
        server->SetAttribute("Port", UintegerValue(port));
        server->SetAttribute("Local", AddressValue(Ipv6Address::GetAny()));
        nodes.Get(1)->AddApplication(server);
        server->SetStartTime(Seconds(0.0));

        Ptr<FlentUdpProbeClient> client = CreateObject<FlentUdpProbeClient>();
        // ifs index 1 = node1's interface; address index 1 = the global address (0 = link-local).
        client->SetAttribute("RemoteAddress", AddressValue(ifs.GetAddress(1, 1)));
        client->SetAttribute("RemotePort", UintegerValue(port));
        client->SetAttribute("Interval", TimeValue(MilliSeconds(50)));
        client->TraceConnectWithoutContext(
            "Rtt",
            MakeCallback(&TestSFlentSinkIpv6LatencySample::OnRtt, this));
        nodes.Get(0)->AddApplication(client);
        client->SetStartTime(Seconds(0.1));

        Simulator::Stop(Seconds(1.0));
        Simulator::Run();
        Simulator::Destroy();

        NS_TEST_ASSERT_MSG_GT(m_samples, 0, "at least one RTT sample must be recorded over IPv6");
    }
};

/**
 * @ingroup stratum-tests
 *
 * Test suite for FlentCsvSink host-attribution column.
 */
class DiffservFlentSinkTestSuite : public TestSuite
{
  public:
    DiffservFlentSinkTestSuite()
        : TestSuite("stratum-flent-sink", Type::SYSTEM)
    {
        AddTestCase(new TestSFlentSinkHostColumnEmitted(), TestCase::Duration::EXTENSIVE);
        AddTestCase(new TestSFlentSinkHostAttributionCorrect(), TestCase::Duration::EXTENSIVE);
        AddTestCase(new TestSFlentSinkBackwardsCompatNoHostid(), TestCase::Duration::EXTENSIVE);
        AddTestCase(new TestSFlentSinkIpv6LatencySample(), TestCase::Duration::QUICK);
    }
};

static DiffservFlentSinkTestSuite g_diffservFlentSinkTestSuite;
