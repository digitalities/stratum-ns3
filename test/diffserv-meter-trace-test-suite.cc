/*
 * Copyright (C) 2026 Sergio Andreozzi
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Test suite for Meter::MeterColour trace source (NVI pattern).
 */

#include "ns3/simulator.h"
#include "ns3/stratum-constants.h"
#include "ns3/stratum-dumb-meter.h"
#include "ns3/stratum-fw-meter.h"
#include "ns3/stratum-meter.h"
#include "ns3/stratum-policy-entry.h"
#include "ns3/stratum-sr-tcm-meter.h"
#include "ns3/stratum-token-bucket-meter.h"
#include "ns3/stratum-tr-tcm-meter.h"
#include "ns3/stratum-tsw2cm-meter.h"
#include "ns3/stratum-tsw3cm-meter.h"
#include "ns3/test.h"
#include "ns3/type-id.h"

#include <vector>

using namespace ns3;
using ns3::stratum::Colour;
using ns3::stratum::DumbMeter;
using ns3::stratum::FWMeter;
using ns3::stratum::Meter;
using ns3::stratum::PolicyEntry;
using ns3::stratum::SrTcmMeter;
using ns3::stratum::TokenBucketMeter;
using ns3::stratum::TrTcmMeter;
using ns3::stratum::Tsw2cmMeter;
using ns3::stratum::Tsw3cmMeter;

// ---------------------------------------------------------------------------
// Shared helpers
// ---------------------------------------------------------------------------

namespace
{

/// Records every MeterColour trace fire so tests can assert sequences.
struct ColourTraceRecorder
{
    struct Event
    {
        Colour colour;
        uint32_t classId;
        double timeSec;
    };

    std::vector<Event> events;

    void OnTrace(Colour c, uint32_t cid, ns3::Time t)
    {
        events.push_back({c, cid, t.GetSeconds()});
    }
};

/// Convert Colour enum to int so NS_TEST_ASSERT_MSG_EQ can stream it.
inline int
CI(Colour c)
{
    return static_cast<int>(c);
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Base-class TypeId registration test
// ---------------------------------------------------------------------------

class MeterBaseTraceRegisteredTest : public TestCase
{
  public:
    MeterBaseTraceRegisteredTest()
        : TestCase("Meter::GetTypeId advertises MeterColour trace source")
    {
    }

  private:
    void DoRun() override
    {
        TypeId tid = Meter::GetTypeId();
        bool found = false;
        std::string callbackSig;
        for (uint32_t i = 0; i < tid.GetTraceSourceN(); ++i)
        {
            auto info = tid.GetTraceSource(i);
            if (info.name == "MeterColour")
            {
                found = true;
                callbackSig = info.callback;
                break;
            }
        }
        NS_TEST_ASSERT_MSG_EQ(found,
                              true,
                              "Meter::GetTypeId must advertise MeterColour trace source");
        NS_TEST_ASSERT_MSG_EQ(callbackSig,
                              std::string("ns3::stratum::Meter::MeterColourTracedCallback"),
                              "MeterColour signature must match documented type");
    }
};

// ---------------------------------------------------------------------------
// sr-TCM: GREEN -> YELLOW -> RED cascade
// ---------------------------------------------------------------------------

class TestSMeterTraceSrTcm : public TestCase
{
  public:
    TestSMeterTraceSrTcm()
        : TestCase("S-meter-trace-srtcm: SrTcmMeter fires MeterColour GREEN YELLOW RED")
    {
    }

  private:
    void DoRun() override
    {
        Ptr<SrTcmMeter> meter = CreateObject<SrTcmMeter>();

        ColourTraceRecorder rec;
        meter->TraceConnectWithoutContext("MeterColour",
                                          MakeCallback(&ColourTraceRecorder::OnTrace, &rec));

        PolicyEntry entry;
        entry.policyIndex = 7;
        // Small CBS and EBS so they drain quickly.
        entry.cir = 1000.0; // 1 KB/s
        entry.cbs = 500.0;  // 500 B committed burst
        entry.ebs = 500.0;  // 500 B excess burst
        entry.cBucket = 500.0;
        entry.eBucket = 500.0;
        entry.arrivalTime = 0.0;

        // Packet 1: 400 B at t=0 — cBucket=500 >= 400 -> GREEN
        meter->ApplyMeter(entry, 0.0, 400);
        meter->ApplyPolicer(entry, 400);

        // Packet 2: 400 B at t=0.001 s — tiny refill (~1B); cBucket drained by pkt1 (100 left),
        // refill ~1B; still < 400, so spills to eBucket -> YELLOW
        meter->ApplyMeter(entry, 0.001, 400);
        meter->ApplyPolicer(entry, 400);

        // Packet 3: 400 B at t=0.002 s — both buckets should be depleted -> RED
        meter->ApplyMeter(entry, 0.002, 400);
        meter->ApplyPolicer(entry, 400);

        NS_TEST_ASSERT_MSG_EQ(rec.events.size(), 3u, "Expected 3 trace fires");
        NS_TEST_ASSERT_MSG_EQ(CI(rec.events[0].colour),
                              CI(Colour::GREEN),
                              "Packet 1 must be GREEN");
        NS_TEST_ASSERT_MSG_EQ(CI(rec.events[1].colour),
                              CI(Colour::YELLOW),
                              "Packet 2 must be YELLOW");
        NS_TEST_ASSERT_MSG_EQ(CI(rec.events[2].colour), CI(Colour::RED), "Packet 3 must be RED");

        // All fires must carry the correct policyIndex
        for (const auto& ev : rec.events)
        {
            NS_TEST_ASSERT_MSG_EQ(ev.classId, 7u, "classId must match entry.policyIndex");
        }
    }
};

// ---------------------------------------------------------------------------
// tr-TCM: GREEN -> YELLOW -> RED cascade (RFC 2698)
// ---------------------------------------------------------------------------

class TestSMeterTraceTrTcm : public TestCase
{
  public:
    TestSMeterTraceTrTcm()
        : TestCase("S-meter-trace-trtcm: TrTcmMeter fires MeterColour GREEN YELLOW RED")
    {
    }

  private:
    void DoRun() override
    {
        Ptr<TrTcmMeter> meter = CreateObject<TrTcmMeter>();

        ColourTraceRecorder rec;
        meter->TraceConnectWithoutContext("MeterColour",
                                          MakeCallback(&ColourTraceRecorder::OnTrace, &rec));

        PolicyEntry entry;
        entry.policyIndex = 3;
        entry.cir = 1000.0; // 1 KB/s
        entry.pir = 2000.0; // 2 KB/s
        entry.cbs = 500.0;
        entry.pbs = 800.0;
        entry.cBucket = 500.0;
        entry.pBucket = 800.0;
        entry.arrivalTime = 0.0;

        // Packet 1: 400B at t=0 -> both buckets >= 400: GREEN
        meter->ApplyMeter(entry, 0.0, 400);
        meter->ApplyPolicer(entry, 400);

        // Packet 2: 400B at t=0.001 s
        // tiny refill; cBucket was 100 after pkt1 (+1 refill) -> ~101 < 400: YELLOW
        // pBucket was 400 after pkt1 (+2 refill) -> ~402 >= 400: not RED
        meter->ApplyMeter(entry, 0.001, 400);
        meter->ApplyPolicer(entry, 400);

        // Packet 3: 400B at t=0.002 s
        // cBucket still < 400, pBucket now also drained: RED
        meter->ApplyMeter(entry, 0.002, 400);
        meter->ApplyPolicer(entry, 400);

        NS_TEST_ASSERT_MSG_EQ(rec.events.size(), 3u, "Expected 3 trace fires");
        NS_TEST_ASSERT_MSG_EQ(CI(rec.events[0].colour),
                              CI(Colour::GREEN),
                              "Packet 1 must be GREEN");
        // Packets 2 and 3: at least one non-GREEN between them
        bool hasNonGreen = false;
        for (size_t i = 1; i < rec.events.size(); ++i)
        {
            if (rec.events[i].colour != Colour::GREEN)
            {
                hasNonGreen = true;
            }
        }
        NS_TEST_ASSERT_MSG_EQ(hasNonGreen, true, "Packets 2-3 must include at least one non-GREEN");
        for (const auto& ev : rec.events)
        {
            NS_TEST_ASSERT_MSG_EQ(ev.classId, 3u, "classId must match policyIndex");
        }
    }
};

// ---------------------------------------------------------------------------
// TSW2CM: trace fires, probabilistic — assert count only
// ---------------------------------------------------------------------------

class TestSMeterTraceTsw2cm : public TestCase
{
  public:
    TestSMeterTraceTsw2cm()
        : TestCase(
              "S-meter-trace-tsw2cm: Tsw2cmMeter fires N trace events for N ApplyPolicer calls")
    {
    }

  private:
    void DoRun() override
    {
        Ptr<Tsw2cmMeter> meter = CreateObject<Tsw2cmMeter>();
        // Fix RNG so test is deterministic: stream 0 produces reproducible values
        meter->AssignStreams(0);

        ColourTraceRecorder rec;
        meter->TraceConnectWithoutContext("MeterColour",
                                          MakeCallback(&ColourTraceRecorder::OnTrace, &rec));

        PolicyEntry entry;
        entry.policyIndex = 5;
        entry.cir = 125000.0; // 1 Mbps in bytes/s
        entry.avgRate = 0.0;
        entry.winLen = 1.0;
        entry.arrivalTime = 0.0;

        constexpr int kN = 10;
        double now = 0.0;
        for (int i = 0; i < kN; ++i)
        {
            now += 0.001;
            meter->ApplyMeter(entry, now, 1000);
            meter->ApplyPolicer(entry, 1000);
        }

        NS_TEST_ASSERT_MSG_EQ(rec.events.size(),
                              static_cast<size_t>(kN),
                              "TSW2CM must emit one trace event per ApplyPolicer call");
        for (const auto& ev : rec.events)
        {
            NS_TEST_ASSERT_MSG_EQ(ev.classId, 5u, "classId must match policyIndex");
        }
    }
};

// ---------------------------------------------------------------------------
// TSW3CM: trace fires, probabilistic — assert count only
// ---------------------------------------------------------------------------

class TestSMeterTraceTsw3cm : public TestCase
{
  public:
    TestSMeterTraceTsw3cm()
        : TestCase(
              "S-meter-trace-tsw3cm: Tsw3cmMeter fires N trace events for N ApplyPolicer calls")
    {
    }

  private:
    void DoRun() override
    {
        Ptr<Tsw3cmMeter> meter = CreateObject<Tsw3cmMeter>();
        meter->AssignStreams(0);

        ColourTraceRecorder rec;
        meter->TraceConnectWithoutContext("MeterColour",
                                          MakeCallback(&ColourTraceRecorder::OnTrace, &rec));

        PolicyEntry entry;
        entry.policyIndex = 2;
        entry.cir = 125000.0;
        entry.pir = 250000.0;
        entry.avgRate = 0.0;
        entry.winLen = 1.0;
        entry.arrivalTime = 0.0;

        constexpr int kN = 10;
        double now = 0.0;
        for (int i = 0; i < kN; ++i)
        {
            now += 0.001;
            meter->ApplyMeter(entry, now, 1000);
            meter->ApplyPolicer(entry, 1000);
        }

        NS_TEST_ASSERT_MSG_EQ(rec.events.size(),
                              static_cast<size_t>(kN),
                              "TSW3CM must emit one trace event per ApplyPolicer call");
        for (const auto& ev : rec.events)
        {
            NS_TEST_ASSERT_MSG_EQ(ev.classId, 2u, "classId must match policyIndex");
        }
    }
};

// ---------------------------------------------------------------------------
// FW: GREEN -> YELLOW transition
// ---------------------------------------------------------------------------

class TestSMeterTraceFw : public TestCase
{
  public:
    TestSMeterTraceFw()
        : TestCase("S-meter-trace-fw: FWMeter fires MeterColour GREEN then YELLOW")
    {
    }

  private:
    void DoRun() override
    {
        Ptr<FWMeter> meter = CreateObject<FWMeter>();

        ColourTraceRecorder rec;
        meter->TraceConnectWithoutContext("MeterColour",
                                          MakeCallback(&ColourTraceRecorder::OnTrace, &rec));

        PolicyEntry entry;
        entry.policyIndex = 1;
        entry.cir = 1000.0; // byte threshold: GREEN while bytesSent <= 1000
        entry.arrivalTime = 0.0;

        // Packet 1: 400 B -> bytesSent=400 <= 1000 -> GREEN
        meter->ApplyMeter(entry, 0.0, 400);
        meter->ApplyPolicer(entry, 400);

        // Packet 2: 400 B -> bytesSent=800 <= 1000 -> GREEN
        meter->ApplyMeter(entry, 0.001, 400);
        meter->ApplyPolicer(entry, 400);

        // Packet 3: 400 B -> bytesSent=1200 > 1000 -> YELLOW
        meter->ApplyMeter(entry, 0.002, 400);
        meter->ApplyPolicer(entry, 400);

        NS_TEST_ASSERT_MSG_EQ(rec.events.size(), 3u, "Expected 3 trace fires");
        NS_TEST_ASSERT_MSG_EQ(CI(rec.events[0].colour),
                              CI(Colour::GREEN),
                              "Packet 1 must be GREEN");
        NS_TEST_ASSERT_MSG_EQ(CI(rec.events[1].colour),
                              CI(Colour::GREEN),
                              "Packet 2 must be GREEN");
        NS_TEST_ASSERT_MSG_EQ(CI(rec.events[2].colour),
                              CI(Colour::YELLOW),
                              "Packet 3 must be YELLOW");
        for (const auto& ev : rec.events)
        {
            NS_TEST_ASSERT_MSG_EQ(ev.classId, 1u, "classId must match policyIndex");
        }
    }
};

// ---------------------------------------------------------------------------
// TokenBucket: GREEN -> RED
// ---------------------------------------------------------------------------

class TestSMeterTraceTokenBucket : public TestCase
{
  public:
    TestSMeterTraceTokenBucket()
        : TestCase("S-meter-trace-tokenbucket: TokenBucketMeter fires MeterColour GREEN then RED")
    {
    }

  private:
    void DoRun() override
    {
        Ptr<TokenBucketMeter> meter = CreateObject<TokenBucketMeter>();

        ColourTraceRecorder rec;
        meter->TraceConnectWithoutContext("MeterColour",
                                          MakeCallback(&ColourTraceRecorder::OnTrace, &rec));

        PolicyEntry entry;
        entry.policyIndex = 4;
        entry.cir = 1000.0; // 1 KB/s
        entry.cbs = 500.0;  // 500 B bucket
        entry.cBucket = 500.0;
        entry.arrivalTime = 0.0;

        // Packet 1: 400 B at t=0 — bucket=500 >= 400: GREEN, bucket -> 100
        meter->ApplyMeter(entry, 0.0, 400);
        meter->ApplyPolicer(entry, 400);

        // Packet 2: 400 B at t=0.001 s — tiny refill (~1B), bucket=101 < 400: RED
        meter->ApplyMeter(entry, 0.001, 400);
        meter->ApplyPolicer(entry, 400);

        NS_TEST_ASSERT_MSG_EQ(rec.events.size(), 2u, "Expected 2 trace fires");
        NS_TEST_ASSERT_MSG_EQ(CI(rec.events[0].colour),
                              CI(Colour::GREEN),
                              "Packet 1 must be GREEN");
        NS_TEST_ASSERT_MSG_EQ(CI(rec.events[1].colour), CI(Colour::RED), "Packet 2 must be RED");
        for (const auto& ev : rec.events)
        {
            NS_TEST_ASSERT_MSG_EQ(ev.classId, 4u, "classId must match policyIndex");
        }
    }
};

// ---------------------------------------------------------------------------
// Dumb: always GREEN
// ---------------------------------------------------------------------------

class TestSMeterTraceDumb : public TestCase
{
  public:
    TestSMeterTraceDumb()
        : TestCase("S-meter-trace-dumb: DumbMeter fires 5 MeterColour events all GREEN")
    {
    }

  private:
    void DoRun() override
    {
        Ptr<DumbMeter> meter = CreateObject<DumbMeter>();

        ColourTraceRecorder rec;
        meter->TraceConnectWithoutContext("MeterColour",
                                          MakeCallback(&ColourTraceRecorder::OnTrace, &rec));

        PolicyEntry entry;
        entry.policyIndex = 0;

        for (int i = 0; i < 5; ++i)
        {
            meter->ApplyMeter(entry, static_cast<double>(i), 1000);
            meter->ApplyPolicer(entry, 1000);
        }

        NS_TEST_ASSERT_MSG_EQ(rec.events.size(), 5u, "Expected 5 trace fires");
        for (size_t i = 0; i < rec.events.size(); ++i)
        {
            NS_TEST_ASSERT_MSG_EQ(CI(rec.events[i].colour),
                                  CI(Colour::GREEN),
                                  "DumbMeter always GREEN");
            NS_TEST_ASSERT_MSG_EQ(rec.events[i].classId, 0u, "classId must be 0");
        }
    }
};

// ---------------------------------------------------------------------------
// Test suite registration
// ---------------------------------------------------------------------------

class DiffservMeterTraceTestSuite : public TestSuite
{
  public:
    DiffservMeterTraceTestSuite()
        : TestSuite("stratum-meter-trace", Type::UNIT)
    {
        AddTestCase(new MeterBaseTraceRegisteredTest(), TestCase::Duration::QUICK);
        AddTestCase(new TestSMeterTraceSrTcm(), TestCase::Duration::QUICK);
        AddTestCase(new TestSMeterTraceTrTcm(), TestCase::Duration::QUICK);
        AddTestCase(new TestSMeterTraceTsw2cm(), TestCase::Duration::QUICK);
        AddTestCase(new TestSMeterTraceTsw3cm(), TestCase::Duration::QUICK);
        AddTestCase(new TestSMeterTraceFw(), TestCase::Duration::QUICK);
        AddTestCase(new TestSMeterTraceTokenBucket(), TestCase::Duration::QUICK);
        AddTestCase(new TestSMeterTraceDumb(), TestCase::Duration::QUICK);
    }
};

static DiffservMeterTraceTestSuite g_diffservMeterTraceTestSuite;
