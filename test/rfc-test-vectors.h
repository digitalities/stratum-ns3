/*
 * Copyright (C) 2026 Sergio Andreozzi
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * RFC 2697 / 2698 test vectors for the diffserv-ns3 meter port.
 * Framework-independent — no ns-3 types.  All values are exact
 * (deterministic arithmetic, no tolerances).
 *
 * Each vector has been verified against the 2001 reference implementation
 * in src/ns-2.29/diffserv/dsPolicy.cc by tracing the algorithm
 * by hand.  If the ns-3 port disagrees with any value here, the port is
 * wrong — fix the port, not the vector.
 *
 * Bucket values represent the state AFTER both applyMeter() and
 * applyPolicer() have run for the given packet.
 *
 * @see specs/02-structural.md  S-1 (TokenBucket), S-2 (srTCM), S-3 (trTCM)
 * @see specs/01-intent.md      I-2.1, I-2.2, I-2.3
 */

#ifndef NS3_STRATUM_RFC_TEST_VECTORS_H
#define NS3_STRATUM_RFC_TEST_VECTORS_H

#include <cstdint>

namespace diffserv_test
{

// ─── Enums and data types ───────────────────────────────────────────────────

enum class Colour : uint8_t
{
    GREEN = 0,
    YELLOW = 1,
    RED = 2
};

/** Sentinel for bucket fields that don't apply to a given meter family. */
static constexpr double NA = -1.0;

static constexpr int kMaxEvents = 16;

/** @brief One packet arrival and the expected meter+policer result. */
struct PacketEvent
{
    double arrival_time_s;    //!< Absolute arrival time (seconds)
    uint32_t size_bytes;      //!< Packet size in bytes
    Colour expected_colour;   //!< Expected colour decision
    double expected_c_bucket; //!< cBucket after meter+policer
    double expected_e_bucket; //!< eBucket after (srTCM); NA otherwise
    double expected_p_bucket; //!< pBucket after (trTCM); NA otherwise
};

// ─── Token Bucket ───────────────────────────────────────────────────────────

struct TokenBucketTestVector
{
    const char* name;
    const char* rfc_citation;
    const char* description;
    double cir_bytes_per_sec;
    uint32_t cbs_bytes;
    double initial_c_bucket;
    double initial_arrival_time;
    int num_events;
    PacketEvent events[kMaxEvents];
};

// ─── srTCM (RFC 2697) ──────────────────────────────────────────────────────

struct SrTcmTestVector
{
    const char* name;
    const char* rfc_citation;
    const char* description;
    double cir_bytes_per_sec;
    uint32_t cbs_bytes;
    uint32_t ebs_bytes;
    double initial_c_bucket;
    double initial_e_bucket;
    double initial_arrival_time;
    int num_events;
    PacketEvent events[kMaxEvents];
};

// ─── trTCM (RFC 2698) ──────────────────────────────────────────────────────

struct TrTcmTestVector
{
    const char* name;
    const char* rfc_citation;
    const char* description;
    double cir_bytes_per_sec;
    double pir_bytes_per_sec;
    uint32_t cbs_bytes;
    uint32_t pbs_bytes;
    double initial_c_bucket;
    double initial_p_bucket;
    double initial_arrival_time;
    int num_events;
    PacketEvent events[kMaxEvents];
};

// ═══════════════════════════════════════════════════════════════════════════
//  TOKEN BUCKET VECTORS  (5 vectors, 14 events)
//  Reference: dsPolicy.cc TBPolicy::applyMeter (line 610) and
//             TBPolicy::applyPolicer (line 634)
// ═══════════════════════════════════════════════════════════════════════════

static constexpr TokenBucketTestVector kTokenBucketVectors[] = {

    // ── TB-1 ────────────────────────────────────────────────────────────────
    {
        "TokenBucket_AllGreenAtCir",
        "N/A (token bucket is pre-RFC; see I-2.3)",
        "Stream at exactly CIR with full bucket. Tokens refill exactly what "
        "the policer drains, so every packet is GREEN and the bucket is "
        "stable at CBS-packetSize after each cycle.",
        /*cir=*/125000.0,
        /*cbs=*/10000,
        /*init_c=*/10000.0,
        /*init_t=*/0.0,
        /*num_events=*/4,
        {
            // t=0.008: tokens=1000, c=10000+1000>CBS→10000, policer:
            // 10000-1000=9000
            {0.008, 1000, Colour::GREEN, 9000.0, NA, NA},
            // t=0.016: tokens=1000, c=9000+1000=10000, policer: 10000-1000=9000
            {0.016, 1000, Colour::GREEN, 9000.0, NA, NA},
            // t=0.024: same steady state
            {0.024, 1000, Colour::GREEN, 9000.0, NA, NA},
            // t=0.032: same steady state
            {0.032, 1000, Colour::GREEN, 9000.0, NA, NA},
        },
    },

    // ── TB-2 ────────────────────────────────────────────────────────────────
    {
        "TokenBucket_BurstDrainsToRed",
        "N/A (token bucket is pre-RFC; see I-2.3)",
        "Fast burst (10x CIR) drains the bucket. Each packet adds only 125 "
        "tokens but removes 1000, so the bucket empties after 5 GREEN "
        "packets and the 6th is RED.",
        /*cir=*/125000.0,
        /*cbs=*/5000,
        /*init_c=*/5000.0,
        /*init_t=*/0.0,
        /*num_events=*/6,
        {
            // t=0.001: tokens=125, c=5000+125>CBS→5000, 5000-1000=4000
            {0.001, 1000, Colour::GREEN, 4000.0, NA, NA},
            // t=0.002: c=4000+125=4125, 4125-1000=3125
            {0.002, 1000, Colour::GREEN, 3125.0, NA, NA},
            // t=0.003: c=3125+125=3250, 3250-1000=2250
            {0.003, 1000, Colour::GREEN, 2250.0, NA, NA},
            // t=0.004: c=2250+125=2375, 2375-1000=1375
            {0.004, 1000, Colour::GREEN, 1375.0, NA, NA},
            // t=0.005: c=1375+125=1500, 1500-1000=500
            {0.005, 1000, Colour::GREEN, 500.0, NA, NA},
            // t=0.006: c=500+125=625, 625-1000<0 → RED, bucket unchanged
            {0.006, 1000, Colour::RED, 625.0, NA, NA},
        },
    },

    // ── TB-3 ────────────────────────────────────────────────────────────────
    {
        "TokenBucket_IdleRefillCapped",
        "N/A (token bucket is pre-RFC; see S-1.3, S-1.4)",
        "Idle period causes bucket to refill. First gap adds 2500 tokens "
        "(under CBS). Second gap would add 5000, but bucket caps at CBS. "
        "Verifies S-1.3 (refill) and S-1.4 (never exceeds CBS).",
        /*cir=*/125000.0,
        /*cbs=*/5000,
        /*init_c=*/1000.0,
        /*init_t=*/0.0,
        /*num_events=*/2,
        {
            // t=0.020: tokens=2500, c=1000+2500=3500≤CBS, 3500-1000=2500
            {0.020, 1000, Colour::GREEN, 2500.0, NA, NA},
            // t=0.060: dt=0.040, tokens=5000, c=2500+5000=7500>CBS→5000,
            // 5000-1000=4000
            {0.060, 1000, Colour::GREEN, 4000.0, NA, NA},
        },
    },

    // ── TB-4 ────────────────────────────────────────────────────────────────
    {
        "TokenBucket_LargePacketRedBucketUnchanged",
        "N/A (see S-1.5)",
        "Packet larger than current bucket → RED, and the bucket is NOT "
        "decremented. This is critical: a RED decision must not corrupt "
        "the bucket state.",
        /*cir=*/125000.0,
        /*cbs=*/5000,
        /*init_c=*/500.0,
        /*init_t=*/0.0,
        /*num_events=*/1,
        {
            // t=0.001: tokens=125, c=500+125=625, 625-1000<0 → RED, c unchanged
            {0.001, 1000, Colour::RED, 625.0, NA, NA},
        },
    },

    // ── TB-5 ────────────────────────────────────────────────────────────────
    {
        "TokenBucket_ExactBucketGreen",
        "N/A (see S-1.5 boundary)",
        "Packet size exactly equals current bucket → GREEN, bucket = 0. "
        "Tests the >= 0 boundary in the policer comparison "
        "(dsPolicy.cc line 639: cBucket - size >= 0).",
        /*cir=*/125000.0,
        /*cbs=*/5000,
        /*init_c=*/875.0,
        /*init_t=*/0.0,
        /*num_events=*/1,
        {
            // t=0.001: tokens=125, c=875+125=1000, 1000-1000=0 ≥ 0 → GREEN
            {0.001, 1000, Colour::GREEN, 0.0, NA, NA},
        },
    },
};

static constexpr int kNumTokenBucketVectors =
    static_cast<int>(sizeof(kTokenBucketVectors) / sizeof(kTokenBucketVectors[0]));

// ═══════════════════════════════════════════════════════════════════════════
//  srTCM VECTORS  (10 vectors, 21 events)
//  RFC 2697 — "A Single Rate Three Color Marker" (Heinanen & Guérin, 1999)
//  Reference: dsPolicy.cc SRTCMPolicy::applyMeter (line 664) and
//             SRTCMPolicy::applyPolicer (line 697)
//
//  All vectors are colour-blind mode (I-2.7).
//
//  Key srTCM property: there is ONE rate (CIR).  Tokens fill cBucket
//  first; only overflow (when cBucket is at CBS) spills into eBucket.
//  This is RFC 2697 §3: "Te is incremented ... when Tc is full (at CBS)."
// ═══════════════════════════════════════════════════════════════════════════

static constexpr SrTcmTestVector kSrTcmVectors[] = {

    // ── SR-1 ────────────────────────────────────────────────────────────────
    {
        "SrTcm_AllGreenUnderCir",
        "RFC 2697 §4 colour-blind",
        "Stream at exactly CIR with full buckets. Every packet is GREEN "
        "from cBucket; eBucket is never touched.",
        /*cir=*/125000.0,
        /*cbs=*/10000,
        /*ebs=*/20000,
        /*init_c=*/10000.0,
        /*init_e=*/20000.0,
        /*init_t=*/0.0,
        /*num_events=*/3,
        {
            // t=0.008: tokens=1000. c=10000+1000>CBS→10000, overflow=1000,
            //   e=20000+1000>EBS→20000. Policer: 10000-1000=9000 → GREEN
            {0.008, 1000, Colour::GREEN, 9000.0, 20000.0, NA},
            // t=0.016: c=9000+1000=10000≤CBS. No overflow. e unchanged.
            {0.016, 1000, Colour::GREEN, 9000.0, 20000.0, NA},
            // t=0.024: same steady state
            {0.024, 1000, Colour::GREEN, 9000.0, 20000.0, NA},
        },
    },

    // ── SR-2 ────────────────────────────────────────────────────────────────
    {
        "SrTcm_BurstFitsCBucket",
        "RFC 2697 §4 colour-blind",
        "Short burst (3 packets) drains cBucket but never exhausts it. "
        "All packets are GREEN from cBucket; eBucket is untouched. Shows "
        "that cBucket alone absorbs the burst.",
        /*cir=*/125000.0,
        /*cbs=*/5000,
        /*ebs=*/5000,
        /*init_c=*/5000.0,
        /*init_e=*/5000.0,
        /*init_t=*/0.0,
        /*num_events=*/3,
        {
            // t=0.001: tokens=125, c=5000+125>CBS→5000, overflow=125→e capped.
            // GREEN.
            {0.001, 1000, Colour::GREEN, 4000.0, 5000.0, NA},
            // t=0.002: c=4000+125=4125. No overflow. GREEN.
            {0.002, 1000, Colour::GREEN, 3125.0, 5000.0, NA},
            // t=0.003: c=3125+125=3250. GREEN.
            {0.003, 1000, Colour::GREEN, 2250.0, 5000.0, NA},
        },
    },

    // ── SR-3 ────────────────────────────────────────────────────────────────
    {
        "SrTcm_BurstGreenYellowRed",
        "RFC 2697 §4 colour-blind",
        "Extended burst at 10x CIR causes complete GREEN→YELLOW→RED "
        "transition. cBucket drains first (GREEN), then eBucket absorbs "
        "excess (YELLOW), then both are empty (RED). The canonical srTCM "
        "test showing all three colour paths.",
        /*cir=*/125000.0,
        /*cbs=*/3000,
        /*ebs=*/3000,
        /*init_c=*/3000.0,
        /*init_e=*/3000.0,
        /*init_t=*/0.0,
        /*num_events=*/7,
        {
            // Packet 1: c=3000+125>CBS→3000. 3000-1000=2000 → GREEN
            {0.001, 1000, Colour::GREEN, 2000.0, 3000.0, NA},
            // Packet 2: c=2000+125=2125. 2125-1000=1125 → GREEN
            {0.002, 1000, Colour::GREEN, 1125.0, 3000.0, NA},
            // Packet 3: c=1125+125=1250. 1250-1000=250 → GREEN
            {0.003, 1000, Colour::GREEN, 250.0, 3000.0, NA},
            // Packet 4: c=250+125=375. 375<1000 → YELLOW. e=3000-1000=2000. c
            // unchanged.
            {0.004, 1000, Colour::YELLOW, 375.0, 2000.0, NA},
            // Packet 5: c=375+125=500. 500<1000 → YELLOW. e=2000-1000=1000.
            {0.005, 1000, Colour::YELLOW, 500.0, 1000.0, NA},
            // Packet 6: c=500+125=625. 625<1000 → check e: 1000-1000=0 ≥ 0 →
            // YELLOW.
            {0.006, 1000, Colour::YELLOW, 625.0, 0.0, NA},
            // Packet 7: c=625+125=750. 750<1000. e=0<1000 → RED. Neither
            // decremented.
            {0.007, 1000, Colour::RED, 750.0, 0.0, NA},
        },
    },

    // ── SR-4 ────────────────────────────────────────────────────────────────
    {
        "SrTcm_IdleOverflowToEBucket",
        "RFC 2697 §3",
        "Idle period generates tokens that fill cBucket to CBS, with the "
        "overflow spilling into eBucket. This is the core srTCM refill "
        "mechanism (RFC 2697 §3: Te incremented when Tc is at CBS). "
        "CIR*dt=5000 but cBucket only needs 2000 to reach CBS, so 3000 "
        "overflow to eBucket.",
        /*cir=*/125000.0,
        /*cbs=*/5000,
        /*ebs=*/5000,
        /*init_c=*/3000.0,
        /*init_e=*/1000.0,
        /*init_t=*/0.0,
        /*num_events=*/1,
        {
            // t=0.040: tokens=5000. c=3000+5000>CBS→5000.
            //   overflow=5000-(5000-3000)=3000. e=1000+3000=4000≤EBS→4000.
            //   Policer: 5000-1000=4000 → GREEN.
            {0.040, 1000, Colour::GREEN, 4000.0, 4000.0, NA},
        },
    },

    // ── SR-5 ────────────────────────────────────────────────────────────────
    {
        "SrTcm_ExactCBucketGreen",
        "RFC 2697 §4 colour-blind",
        "Packet size exactly equals cBucket after meter → GREEN, cBucket=0. "
        "Tests the >= 0 boundary (dsPolicy.cc line 702). One byte less "
        "would also be GREEN; one byte more would be YELLOW.",
        /*cir=*/125000.0,
        /*cbs=*/5000,
        /*ebs=*/5000,
        /*init_c=*/875.0,
        /*init_e=*/3000.0,
        /*init_t=*/0.0,
        /*num_events=*/1,
        {
            // t=0.001: tokens=125. c=875+125=1000. 1000-1000=0 ≥ 0 → GREEN.
            {0.001, 1000, Colour::GREEN, 0.0, 3000.0, NA},
        },
    },

    // ── SR-6 ────────────────────────────────────────────────────────────────
    {
        "SrTcm_ExceedCBucketByOneYellow",
        "RFC 2697 §4 colour-blind",
        "Packet exceeds cBucket by 1 byte → YELLOW (not GREEN). This is "
        "the complementary edge case to SR-5. The 1-byte difference flips "
        "the colour from GREEN to YELLOW.",
        /*cir=*/125000.0,
        /*cbs=*/5000,
        /*ebs=*/5000,
        /*init_c=*/874.0,
        /*init_e=*/3000.0,
        /*init_t=*/0.0,
        /*num_events=*/1,
        {
            // t=0.001: tokens=125. c=874+125=999. 999-1000=-1 < 0.
            //   e=3000-1000=2000 ≥ 0 → YELLOW. c unchanged at 999.
            {0.001, 1000, Colour::YELLOW, 999.0, 2000.0, NA},
        },
    },

    // ── SR-7 ────────────────────────────────────────────────────────────────
    {
        "SrTcm_BothBucketsEmptyRed",
        "RFC 2697 §4 colour-blind",
        "Both buckets nearly empty, packet far exceeds both → RED. Neither "
        "bucket is decremented on a RED decision. The small token refill "
        "(125 bytes) is not enough to cover the 1000-byte packet.",
        /*cir=*/125000.0,
        /*cbs=*/5000,
        /*ebs=*/5000,
        /*init_c=*/0.0,
        /*init_e=*/0.0,
        /*init_t=*/0.0,
        /*num_events=*/1,
        {
            // t=0.001: tokens=125. c=0+125=125. 125<1000.
            //   e=0. 0<1000 → RED. Neither decremented.
            {0.001, 1000, Colour::RED, 125.0, 0.0, NA},
        },
    },

    // ── SR-8 ────────────────────────────────────────────────────────────────
    {
        "SrTcm_RefillTimingTwoPackets",
        "RFC 2697 §3",
        "Two packets separated by a gap. The second packet's bucket state "
        "must reflect CIR*(t2-t1) accumulation from the FIRST packet's "
        "arrival, not from t=0. Catches bugs where arrivalTime is not "
        "updated after applyMeter.",
        /*cir=*/125000.0,
        /*cbs=*/5000,
        /*ebs=*/5000,
        /*init_c=*/2000.0,
        /*init_e=*/1000.0,
        /*init_t=*/0.0,
        /*num_events=*/2,
        {
            // t=0.010: tokens=125000*0.010=1250. c=2000+1250=3250. GREEN.
            // c=2250.
            {0.010, 1000, Colour::GREEN, 2250.0, 1000.0, NA},
            // t=0.030: dt=0.020, tokens=125000*0.020=2500. c=2250+2500=4750.
            // GREEN. c=3750.
            {0.030, 1000, Colour::GREEN, 3750.0, 1000.0, NA},
        },
    },

    // ── SR-9 ────────────────────────────────────────────────────────────────
    {
        "SrTcm_LongIdleBothCapped",
        "RFC 2697 §3",
        "Long idle period (10s) generates far more tokens than both buckets "
        "can hold. cBucket caps at CBS, overflow fills eBucket to EBS. "
        "Verifies that neither bucket exceeds its configured maximum.",
        /*cir=*/125000.0,
        /*cbs=*/5000,
        /*ebs=*/5000,
        /*init_c=*/0.0,
        /*init_e=*/0.0,
        /*init_t=*/0.0,
        /*num_events=*/1,
        {
            // t=10.0: tokens=1250000. c=5000 (capped).
            //   overflow=1250000-5000=1245000. e=5000 (capped). GREEN. c=4000.
            {10.0, 1000, Colour::GREEN, 4000.0, 5000.0, NA},
        },
    },

    // ── SR-10 ───────────────────────────────────────────────────────────────
    {
        "SrTcm_SpecS21Validation",
        "RFC 2697 §3, §4 (validates spec S-2.1 and S-2.2)",
        "Spec S-2.1: after 100ms idle with CIR=1Mbps and CBS=10000, "
        "12500 token-bytes are generated. cBucket fills to CBS=10000, "
        "remaining 2500 overflow to eBucket. S-2.2: a 5000-byte packet "
        "is GREEN, leaving cBucket=5000, eBucket=2500.",
        /*cir=*/125000.0,
        /*cbs=*/10000,
        /*ebs=*/20000,
        /*init_c=*/0.0,
        /*init_e=*/0.0,
        /*init_t=*/0.0,
        /*num_events=*/1,
        {
            // t=0.100: tokens=12500. c=0+12500>CBS→10000.
            //   overflow=12500-10000=2500. e=0+2500=2500≤EBS.
            //   Policer(5000B): 10000-5000=5000 ≥ 0 → GREEN.
            {0.100, 5000, Colour::GREEN, 5000.0, 2500.0, NA},
        },
    },
};

static constexpr int kNumSrTcmVectors =
    static_cast<int>(sizeof(kSrTcmVectors) / sizeof(kSrTcmVectors[0]));

// ═══════════════════════════════════════════════════════════════════════════
//  trTCM VECTORS  (10 vectors, 17 events)
//  RFC 2698 — "A Two Rate Three Color Marker" (Heinanen & Guérin, 1999)
//  Reference: dsPolicy.cc TRTCMPolicy::applyMeter (line 729) and
//             TRTCMPolicy::applyPolicer (line 761)
//
//  All vectors are colour-blind mode (I-2.7).
//
//  Key trTCM properties:
//  - Two independent rates: CIR fills cBucket, PIR fills pBucket.
//  - Policer checks pBucket FIRST: if pBucket < size → RED.
//  - YELLOW: only pBucket decrements.  cBucket is UNTOUCHED.
//  - GREEN: BOTH buckets decrement.
//  - RED: NEITHER bucket decrements.
// ═══════════════════════════════════════════════════════════════════════════

static constexpr TrTcmTestVector kTrTcmVectors[] = {

    // ── TR-1 ────────────────────────────────────────────────────────────────
    {
        "TrTcm_AllGreenUnderCir",
        "RFC 2698 §2 colour-blind",
        "Stream at CIR with full buckets. Both buckets refill faster than "
        "they drain (pBucket at PIR > CIR), so every packet is GREEN.",
        /*cir=*/100000.0,
        /*pir=*/200000.0,
        /*cbs=*/5000,
        /*pbs=*/5000,
        /*init_c=*/5000.0,
        /*init_p=*/5000.0,
        /*init_t=*/0.0,
        /*num_events=*/3,
        {
            // t=0.005: c_tok=500→c capped at 5000. p_tok=1000→p capped at 5000.
            //   p≥500, c≥500 → GREEN. c=4500, p=4500.
            {0.005, 500, Colour::GREEN, 4500.0, NA, 4500.0},
            // t=0.010: c=4500+500=5000. p=4500+1000>PBS→5000. GREEN. c=4500,
            // p=4500.
            {0.010, 500, Colour::GREEN, 4500.0, NA, 4500.0},
            // t=0.015: same steady state
            {0.015, 500, Colour::GREEN, 4500.0, NA, 4500.0},
        },
    },

    // ── TR-2 ────────────────────────────────────────────────────────────────
    {
        "TrTcm_GreenThenYellow",
        "RFC 2698 §2 colour-blind",
        "Burst between CIR and PIR. cBucket (small CBS) exhausts first "
        "while pBucket (large PBS) still has tokens. Shows GREEN→YELLOW "
        "transition. Critically: cBucket is NOT decremented on YELLOW.",
        /*cir=*/100000.0,
        /*pir=*/200000.0,
        /*cbs=*/2000,
        /*pbs=*/10000,
        /*init_c=*/2000.0,
        /*init_p=*/10000.0,
        /*init_t=*/0.0,
        /*num_events=*/4,
        {
            // t=0.001: c=2000+100>CBS→2000. p=10000+200>PBS→10000.
            //   p≥1000, c(2000)≥1000 → GREEN. c=1000, p=9000.
            {0.001, 1000, Colour::GREEN, 1000.0, NA, 9000.0},
            // t=0.002: c=1000+100=1100. p=9000+200=9200.
            //   p≥1000, c(1100)≥1000 → GREEN. c=100, p=8200.
            {0.002, 1000, Colour::GREEN, 100.0, NA, 8200.0},
            // t=0.003: c=100+100=200. p=8200+200=8400.
            //   p≥1000, c(200)<1000 → YELLOW. p=7400. c UNCHANGED at 200.
            {0.003, 1000, Colour::YELLOW, 200.0, NA, 7400.0},
            // t=0.004: c=200+100=300. p=7400+200=7600.
            //   p≥1000, c(300)<1000 → YELLOW. p=6600. c UNCHANGED at 300.
            {0.004, 1000, Colour::YELLOW, 300.0, NA, 6600.0},
        },
    },

    // ── TR-3 ────────────────────────────────────────────────────────────────
    {
        "TrTcm_GreenYellowRed",
        "RFC 2698 §2 colour-blind",
        "Fast burst with small buckets causes complete GREEN→YELLOW→RED "
        "transition. Shows all three policer code paths in one vector.",
        /*cir=*/100000.0,
        /*pir=*/200000.0,
        /*cbs=*/1000,
        /*pbs=*/2000,
        /*init_c=*/1000.0,
        /*init_p=*/2000.0,
        /*init_t=*/0.0,
        /*num_events=*/3,
        {
            // t=0.001: c=1000+100>CBS→1000. p=2000+200>PBS→2000.
            //   p≥1000, c(1000)≥1000 → GREEN. c=0, p=1000.
            {0.001, 1000, Colour::GREEN, 0.0, NA, 1000.0},
            // t=0.002: c=0+100=100. p=1000+200=1200.
            //   p≥1000, c(100)<1000 → YELLOW. p=200. c=100.
            {0.002, 1000, Colour::YELLOW, 100.0, NA, 200.0},
            // t=0.003: c=100+100=200. p=200+200=400.
            //   p(400)<1000 → RED. Neither decremented.
            {0.003, 1000, Colour::RED, 200.0, NA, 400.0},
        },
    },

    // ── TR-4 ────────────────────────────────────────────────────────────────
    {
        "TrTcm_YellowCBucketUnchanged",
        "RFC 2698 §2",
        "CRITICAL: when packet is YELLOW, cBucket MUST NOT be decremented. "
        "Only pBucket is decremented. This is a classic bug in naive "
        "implementations that decrement both buckets unconditionally. "
        "Ref: dsPolicy.cc line 768-769.",
        /*cir=*/100000.0,
        /*pir=*/200000.0,
        /*cbs=*/1000,
        /*pbs=*/10000,
        /*init_c=*/500.0,
        /*init_p=*/5000.0,
        /*init_t=*/0.0,
        /*num_events=*/1,
        {
            // t=0.001: c=500+100=600. p=5000+200=5200.
            //   p(5200)≥1000, c(600)<1000 → YELLOW.
            //   p=5200-1000=4200. c stays at 600 — NOT decremented.
            {0.001, 1000, Colour::YELLOW, 600.0, NA, 4200.0},
        },
    },

    // ── TR-5 ────────────────────────────────────────────────────────────────
    {
        "TrTcm_GreenBothDecrement",
        "RFC 2698 §2",
        "CRITICAL: when packet is GREEN, BOTH cBucket and pBucket must be "
        "decremented by the packet size. Another common bug: only "
        "decrementing cBucket. Ref: dsPolicy.cc line 772-773.",
        /*cir=*/100000.0,
        /*pir=*/200000.0,
        /*cbs=*/5000,
        /*pbs=*/5000,
        /*init_c=*/3000.0,
        /*init_p=*/4000.0,
        /*init_t=*/0.0,
        /*num_events=*/1,
        {
            // t=0.001: c=3000+100=3100. p=4000+200=4200.
            //   p≥1000, c≥1000 → GREEN. c=3100-1000=2100. p=4200-1000=3200.
            {0.001, 1000, Colour::GREEN, 2100.0, NA, 3200.0},
        },
    },

    // ── TR-6 ────────────────────────────────────────────────────────────────
    {
        "TrTcm_RedNeitherDecrement",
        "RFC 2698 §2",
        "CRITICAL: when packet is RED, NEITHER bucket is decremented. "
        "Third common bug: decrementing pBucket on RED. "
        "Ref: dsPolicy.cc line 765-766 — returns immediately with no "
        "mutation.",
        /*cir=*/100000.0,
        /*pir=*/200000.0,
        /*cbs=*/5000,
        /*pbs=*/5000,
        /*init_c=*/200.0,
        /*init_p=*/500.0,
        /*init_t=*/0.0,
        /*num_events=*/1,
        {
            // t=0.001: c=200+100=300. p=500+200=700.
            //   p(700)<1000 → RED. c=300, p=700 — neither touched.
            {0.001, 1000, Colour::RED, 300.0, NA, 700.0},
        },
    },

    // ── TR-7 ────────────────────────────────────────────────────────────────
    {
        "TrTcm_IndependentRefill",
        "RFC 2698 §2",
        "After idle period, cBucket refills at CIR and pBucket at PIR, "
        "independently. pBucket would reach 8000 but caps at PBS=5000. "
        "cBucket reaches 4000 (under CBS). This proves the two buckets "
        "are not coupled (unlike srTCM's overflow model).",
        /*cir=*/100000.0,
        /*pir=*/200000.0,
        /*cbs=*/5000,
        /*pbs=*/5000,
        /*init_c=*/0.0,
        /*init_p=*/0.0,
        /*init_t=*/0.0,
        /*num_events=*/1,
        {
            // t=0.040: c_tok=4000→c=4000. p_tok=8000→p capped at 5000.
            //   p≥500, c≥500 → GREEN. c=3500, p=4500.
            {0.040, 500, Colour::GREEN, 3500.0, NA, 4500.0},
        },
    },

    // ── TR-8 ────────────────────────────────────────────────────────────────
    {
        "TrTcm_PBucketExactlyFitsYellow",
        "RFC 2698 §2 (boundary)",
        "pBucket exactly equals packet size → YELLOW (not RED). Tests the "
        "boundary: dsPolicy.cc line 765 checks (pBucket - size) < 0. "
        "When equal, the result is 0 which is NOT < 0, so not RED.",
        /*cir=*/100000.0,
        /*pir=*/200000.0,
        /*cbs=*/5000,
        /*pbs=*/5000,
        /*init_c=*/200.0,
        /*init_p=*/800.0,
        /*init_t=*/0.0,
        /*num_events=*/1,
        {
            // t=0.001: c=200+100=300. p=800+200=1000.
            //   p(1000)-1000=0, NOT <0 → not RED.
            //   c(300)<1000 → YELLOW. p=1000-1000=0. c=300.
            {0.001, 1000, Colour::YELLOW, 300.0, NA, 0.0},
        },
    },

    // ── TR-9 ────────────────────────────────────────────────────────────────
    {
        "TrTcm_CBucketExactlyFitsGreen",
        "RFC 2698 §2 (boundary)",
        "cBucket exactly equals packet size → GREEN. Both buckets are "
        "decremented. cBucket drops to 0, pBucket decremented by packet "
        "size. Complements TR-8.",
        /*cir=*/100000.0,
        /*pir=*/200000.0,
        /*cbs=*/5000,
        /*pbs=*/5000,
        /*init_c=*/900.0,
        /*init_p=*/5000.0,
        /*init_t=*/0.0,
        /*num_events=*/1,
        {
            // t=0.001: c=900+100=1000. p=5000+200>PBS→5000.
            //   p≥1000, c(1000)≥1000 → GREEN. c=0, p=4000.
            {0.001, 1000, Colour::GREEN, 0.0, NA, 4000.0},
        },
    },

    // ── TR-10 ───────────────────────────────────────────────────────────────
    {
        "TrTcm_LongIdlePBSCapped",
        "RFC 2698 §2",
        "Long idle period caps both buckets at their configured maximums. "
        "cBucket at CBS, pBucket at PBS. No overflow between them — each "
        "bucket is independently capped.",
        /*cir=*/100000.0,
        /*pir=*/200000.0,
        /*cbs=*/5000,
        /*pbs=*/5000,
        /*init_c=*/0.0,
        /*init_p=*/0.0,
        /*init_t=*/0.0,
        /*num_events=*/1,
        {
            // t=10.0: c_tok=1000000→capped at 5000. p_tok=2000000→capped at
            // 5000.
            //   GREEN. c=4500, p=4500.
            {10.0, 500, Colour::GREEN, 4500.0, NA, 4500.0},
        },
    },
};

static constexpr int kNumTrTcmVectors =
    static_cast<int>(sizeof(kTrTcmVectors) / sizeof(kTrTcmVectors[0]));

} // namespace diffserv_test

#endif // NS3_STRATUM_RFC_TEST_VECTORS_H
