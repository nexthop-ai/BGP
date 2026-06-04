/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#define AdjRib_TEST_FRIENDS                                                   \
  FRIEND_TEST(AdjRibOutboundFixture, VerifyUpdateAttributes);                 \
  FRIEND_TEST(AdjRibOutboundFixture, VerifyLocalAsSessionUpdateAttributes);   \
  FRIEND_TEST(                                                                \
      AdjRibOutboundFixture, VerifyUpdateAttributesWithAcceptAllPolicyTest);  \
  FRIEND_TEST(AdjRibOutboundFixture, VerifyRemovePrivateAs);                  \
  FRIEND_TEST(AdjRibOutboundFixture, UpdateAttributesOutTest);                \
  FRIEND_TEST(AdjRibOutboundFixture, UpdateAsPathAttributesTest);             \
  FRIEND_TEST(AdjRibOutboundFixture, UpdateLocalPrefTest);                    \
  FRIEND_TEST(AdjRibOutboundFixture, UpdateLocalPrefIBgpTest);                \
  FRIEND_TEST(AdjRibOutboundFixture, UpdateMedTest);                          \
  FRIEND_TEST(AdjRibOutboundFixture, UpdateMedWithPolicySetTest);             \
  FRIEND_TEST(AdjRibOutboundFixture, UpdateOriginAndClusterListEBgpTest);     \
  FRIEND_TEST(AdjRibOutboundFixture, UpdateOriginAndClusterListRRClientTest); \
  FRIEND_TEST(                                                                \
      AdjRibOutboundFixture, UpdateOriginAndClusterListRRClientIBgpTest);     \
  FRIEND_TEST(AdjRibOutboundFixture, UpdateGroupKeyCreationTest);

#include <folly/coro/BlockingWait.h>
#include <folly/logging/xlog.h>

#include "neteng/fboss/bgp/cpp/adjrib/AdjRibCommon.h"
#include "neteng/fboss/bgp/cpp/tests/AdjRibOutUtils.h"

namespace facebook::bgp {
using namespace bgp::thrift;
using namespace nettools::bgplib;
using namespace testing;

void loadBgpBestpathFeatures(
    bool enableMedComparison = false,
    bool enabledMedMissingAsWorst = false) {
  BgpConfig thriftConfig;
  BgpSettingConfig tBgpSettingConfig;
  tBgpSettingConfig.enable_med_comparison() = enableMedComparison;
  tBgpSettingConfig.enable_med_missing_as_worst() = enabledMedMissingAsWorst;
  thriftConfig.bgp_setting_config() = std::move(tBgpSettingConfig);
  facebook::bgp::FeatureFlags::LoadFromThriftConfig(thriftConfig);
}

// Verify attribute changes in case of local-as session.
TEST_F(AdjRibOutboundFixture, VerifyLocalAsSessionUpdateAttributes) {
  // Verify that we do not modify the nexthop to self in case of
  // Confed EBGP without next_hop_self
  setupAdjRib(
      kLocalAs2, // Global AS
      kLocalAs3, // Local AS
      kRemoteAs2, // EBGP
      kIsRrClientFalse,
      kIsConfedPeerFalse,
      NextHopSelfConfigured(false),
      kV4Nexthop1,
      kV6Nexthop1,
      false);

  BgpUpdate2 inputUpdate = buildBgpUpdateAttributes(kV4Nexthop2);
  auto inputAttrs = std::make_shared<facebook::bgp::BgpPath>(
      BgpPathFields(*BgpUpdate2toBgpPathC(inputUpdate)));
  EXPECT_EQ(kLocalPref, inputAttrs->getLocalPref());
  EXPECT_EQ(1, inputAttrs->getAsPath()->at(0).asSequence.size());
  EXPECT_EQ(kAsSeqAsNum, inputAttrs->getAsPath()->at(0).asSequence[0]);
  EXPECT_EQ(kV4Nexthop2, inputAttrs->getNexthop());

  {
    auto outputAttrs = inputAttrs->clone();
    updateAttributesOutWithoutNexthop(
        {kV4Prefix1, kDefaultPathID, eBgpPeer_, inputAttrs},
        inputAttrs->clone(),
        outputAttrs,
        PostPolicyInfo{});
    EXPECT_EQ(std::nullopt, outputAttrs->getLocalPref());
    EXPECT_EQ(kV4Nexthop2, outputAttrs->getNexthop());
    EXPECT_FALSE(outputAttrs->isPublished());
    // Verify AS-PATH is updated (with local-as prepended).
    EXPECT_NE(inputAttrs->getAsPath(), outputAttrs->getAsPath());
    EXPECT_EQ(2, outputAttrs->getAsPath()->at(0).asSequence.size());
    EXPECT_EQ(kLocalAs3, outputAttrs->getAsPath()->at(0).asSequence[0]);
    EXPECT_EQ(kAsSeqAsNum, outputAttrs->getAsPath()->at(0).asSequence[1]);
  }

  // Verify that a new as-seq with local-as created for local-origined route
  // case.
  {
    auto newAsPath = inputAttrs->getAsPath().get();
    newAsPath.clear();
    inputAttrs->setAsPath(std::move(newAsPath));
    auto outputAttrs = inputAttrs->clone();
    updateAttributesOutWithoutNexthop(
        {kV4Prefix1, kDefaultPathID, localPeerV4_, inputAttrs},
        inputAttrs->clone(),
        outputAttrs,
        PostPolicyInfo{});
    EXPECT_EQ(1, outputAttrs->getAsPath()->at(0).asSequence.size());
    EXPECT_EQ(kLocalAs3, outputAttrs->getAsPath()->at(0).asSequence[0]);
  }
}

// Verify various cases when we change attributes
TEST_F(AdjRibOutboundFixture, VerifyUpdateAttributes) {
  // Verify that we modify the nexthop to configured nexthop V4 (IBGP peer)
  {
    setupAdjRib(
        kLocalAs1,
        kLocalAs1,
        kRemoteAs1,
        kIsRrClientFalse,
        kIsConfedPeerFalse,
        NextHopSelfConfigured(true),
        kV4Nexthop1,
        kV6Nexthop1,
        false);
    BgpUpdate2 inputUpdate = buildBgpUpdateAttributes(kV4Nexthop2);
    auto inputAttrs = std::make_shared<facebook::bgp::BgpPath>(
        BgpPathFields(*BgpUpdate2toBgpPathC(inputUpdate)));
    EXPECT_EQ(kV4Nexthop2, inputAttrs->getNexthop());
    inputAttrs->publish();
    auto outputAttrs = inputAttrs->clone();
    updateAttributesOutWithoutNexthop(
        {kV4Prefix1, kDefaultPathID, eBgpPeer_, inputAttrs},
        inputAttrs->clone(),
        outputAttrs,
        PostPolicyInfo{});
    EXPECT_FALSE(outputAttrs->isPublished());
    // Check AS-Path is not modified
    EXPECT_EQ(inputAttrs->getAsPath(), outputAttrs->getAsPath());
  }
  {
    // Verify that we modify the nexthop to configured nexthop V6 (IBGP
    // peer)
    setupAdjRib(
        kLocalAs1,
        kLocalAs1,
        kRemoteAs1,
        kIsRrClientFalse,
        kIsConfedPeerFalse,
        NextHopSelfConfigured(true),
        kV4Nexthop1,
        kV6Nexthop1,
        false);
    BgpUpdate2 inputUpdate = buildBgpUpdateAttributes(kV6Nexthop2);
    auto inputAttrs = std::make_shared<facebook::bgp::BgpPath>(
        BgpPathFields(*BgpUpdate2toBgpPathC(inputUpdate)));
    EXPECT_EQ(kV6Nexthop2, inputAttrs->getNexthop());
    inputAttrs->publish();
    auto outputAttrs = inputAttrs->clone();
    updateAttributesOutWithoutNexthop(
        {kV6Prefix1, kDefaultPathID, eBgpPeer_, inputAttrs},
        inputAttrs->clone(),
        outputAttrs,
        PostPolicyInfo{});
    EXPECT_EQ(kV6Nexthop2, outputAttrs->getNexthop());
    EXPECT_FALSE(outputAttrs->isPublished());
  }
  {
    // Verify that if next_hop_self is not configured, we do not update
    // nexthop (IBGP)
    setupAdjRib(
        kLocalAs1,
        kLocalAs1,
        kRemoteAs1,
        kIsRrClientFalse,
        kIsConfedPeerFalse,
        NextHopSelfConfigured(false),
        kEmptyV4Nexthop,
        kEmptyV6Nexthop,
        false);
    BgpUpdate2 inputUpdate = buildBgpUpdateAttributes(kV4Nexthop2);
    auto inputAttrs = std::make_shared<facebook::bgp::BgpPath>(
        BgpPathFields(*BgpUpdate2toBgpPathC(inputUpdate)));
    EXPECT_EQ(kV4Nexthop2, inputAttrs->getNexthop());
    inputAttrs->publish();
    auto outputAttrs = inputAttrs->clone();
    updateAttributesOutWithoutNexthop(
        {kV4Prefix1, kDefaultPathID, eBgpPeer_, inputAttrs},
        inputAttrs->clone(),
        outputAttrs,
        PostPolicyInfo{});
    EXPECT_EQ(kV4Nexthop2, outputAttrs->getNexthop());
  }
  {
    // Verify that we modify the nexthop to implicit self in case of EBGP
    // if no user configured nexthop
    setupAdjRib(
        kLocalAs1,
        kLocalAs1,
        kRemoteAs2, // EBGP
        kIsRrClientFalse,
        kIsConfedPeerFalse,
        NextHopSelfConfigured(false),
        kEmptyV4Nexthop, // no user configured nexthop
        kEmptyV6Nexthop,
        false);
    BgpUpdate2 inputUpdate = buildBgpUpdateAttributes(kV4Nexthop2);
    auto inputAttrs = std::make_shared<facebook::bgp::BgpPath>(
        BgpPathFields(*BgpUpdate2toBgpPathC(inputUpdate)));
    EXPECT_EQ(1, inputAttrs->getAsPath()->at(0).asSequence.size());
    EXPECT_EQ(kAsSeqAsNum, inputAttrs->getAsPath()->at(0).asSequence[0]);
    EXPECT_EQ(kV4Nexthop2, inputAttrs->getNexthop());
    auto outputAttrs = inputAttrs->clone();
    updateAttributesOutWithoutNexthop(
        {kV4Prefix1, kDefaultPathID, eBgpPeer_, inputAttrs},
        inputAttrs->clone(),
        outputAttrs,
        PostPolicyInfo{});
    EXPECT_EQ(kV4Nexthop2, outputAttrs->getNexthop());
    EXPECT_FALSE(outputAttrs->isPublished());

    // Verify AS-PATH is updated (existing as-sequence segment is updated)
    EXPECT_NE(inputAttrs->getAsPath(), outputAttrs->getAsPath());
    EXPECT_EQ(2, outputAttrs->getAsPath()->at(0).asSequence.size());
    EXPECT_EQ(kLocalAs1, outputAttrs->getAsPath()->at(0).asSequence[0]);
    EXPECT_EQ(kAsSeqAsNum, outputAttrs->getAsPath()->at(0).asSequence[1]);

    // Verify that a new Segment is created if there is no AS segment
    // i.e. originating local routes to EBGP peer
    auto newAsPath = inputAttrs->getAsPath().get();
    newAsPath.clear();
    inputAttrs->setAsPath(std::move(newAsPath));
    outputAttrs = inputAttrs->clone();
    updateAttributesOutWithoutNexthop(
        {kV4Prefix1, kDefaultPathID, eBgpPeer_, inputAttrs},
        inputAttrs->clone(),
        outputAttrs,
        PostPolicyInfo{});
    EXPECT_EQ(1, outputAttrs->getAsPath()->at(0).asSequence.size());
    EXPECT_EQ(kLocalAs1, outputAttrs->getAsPath()->at(0).asSequence[0]);
  }
  {
    // Verify that we do not modify the nexthop to self in case of
    // Confed EBGP without next_hop_self
    setupAdjRib(
        kLocalAs1,
        kLocalAs1,
        kRemoteAs2, // EBGP
        kIsRrClientFalse,
        kIsConfedPeerTrue,
        NextHopSelfConfigured(false),
        kV4Nexthop1,
        kV6Nexthop1,
        false);
    BgpUpdate2 inputUpdate = buildBgpUpdateAttributes(kV4Nexthop2);
    auto inputAttrs = std::make_shared<facebook::bgp::BgpPath>(
        BgpPathFields(*BgpUpdate2toBgpPathC(inputUpdate)));
    EXPECT_EQ(kLocalPref, inputAttrs->getLocalPref());
    EXPECT_EQ(1, inputAttrs->getAsPath()->at(0).asSequence.size());
    EXPECT_EQ(kAsSeqAsNum, inputAttrs->getAsPath()->at(0).asSequence[0]);
    EXPECT_EQ(kV4Nexthop2, inputAttrs->getNexthop());
    auto outputAttrs = inputAttrs->clone();
    updateAttributesOutWithoutNexthop(
        {kV4Prefix1, kDefaultPathID, eBgpPeer_, inputAttrs},
        inputAttrs->clone(),
        outputAttrs,
        PostPolicyInfo{});
    EXPECT_NE(std::nullopt, outputAttrs->getLocalPref());
    EXPECT_EQ(kLocalPref, *outputAttrs->getLocalPref());
    EXPECT_EQ(kV4Nexthop2, outputAttrs->getNexthop());
    // Nexthop and LocalPref are not changed but AS-PATH is chagned.
    EXPECT_FALSE(outputAttrs->isPublished());

    // Verify AS-PATH is updated (a new segment is created)
    EXPECT_NE(inputAttrs->getAsPath(), outputAttrs->getAsPath());
    EXPECT_EQ(2, outputAttrs->getAsPath()->size());
    EXPECT_EQ(1, outputAttrs->getAsPath()->at(0).asConfedSequence.size());
    EXPECT_EQ(kLocalAs1, outputAttrs->getAsPath()->at(0).asConfedSequence[0]);
    EXPECT_EQ(1, outputAttrs->getAsPath()->at(1).asSequence.size());
    EXPECT_EQ(kAsSeqAsNum, outputAttrs->getAsPath()->at(1).asSequence[0]);

    // Verify that a new Segment is created if there is no AS segment
    // i.e. originating local routes to EBGP peer
    auto newAsPath = inputAttrs->getAsPath().get();
    newAsPath.clear();
    inputAttrs->setAsPath(newAsPath);
    outputAttrs = inputAttrs->clone();
    updateAttributesOutWithoutNexthop(
        {kV4Prefix1, kDefaultPathID, eBgpPeer_, inputAttrs},
        inputAttrs->clone(),
        outputAttrs,
        PostPolicyInfo{});
    EXPECT_EQ(1, outputAttrs->getAsPath()->at(0).asConfedSequence.size());
    EXPECT_EQ(kLocalAs1, outputAttrs->getAsPath()->at(0).asConfedSequence[0]);

    // Verify that existing confed sequence segment is updated
    newAsPath.clear();
    BgpAttrAsPathSegmentC segment;
    segment.asConfedSequence.push_back(kAsSeqAsNum);
    newAsPath.push_back(segment);
    inputAttrs->setAsPath(newAsPath);
    outputAttrs = inputAttrs->clone();
    updateAttributesOutWithoutNexthop(
        {kV4Prefix1, kDefaultPathID, eBgpPeer_, inputAttrs},
        inputAttrs->clone(),
        outputAttrs,
        PostPolicyInfo{});
    EXPECT_EQ(1, outputAttrs->getAsPath()->size());
    EXPECT_EQ(2, outputAttrs->getAsPath()->at(0).asConfedSequence.size());
    EXPECT_EQ(kLocalAs1, outputAttrs->getAsPath()->at(0).asConfedSequence[0]);
    EXPECT_EQ(kAsSeqAsNum, outputAttrs->getAsPath()->at(0).asConfedSequence[1]);
  }
  {
    // Verify that we modify the nexthop to self in case of Confed EBGP with
    // next_hop_self
    setupAdjRib(
        kLocalAs1,
        kLocalAs1,
        kRemoteAs2, // EBGP
        kIsRrClientFalse,
        kIsConfedPeerTrue,
        NextHopSelfConfigured(true),
        kEmptyV4Nexthop,
        kEmptyV6Nexthop,
        false);
    BgpUpdate2 inputUpdate = buildBgpUpdateAttributes(kV4Nexthop2);
    auto inputAttrs = std::make_shared<facebook::bgp::BgpPath>(
        BgpPathFields(*BgpUpdate2toBgpPathC(inputUpdate)));
    EXPECT_EQ(1, inputAttrs->getAsPath()->at(0).asSequence.size());
    EXPECT_EQ(kV4Nexthop2, inputAttrs->getNexthop());
    auto outputAttrs = inputAttrs->clone();
    updateAttributesOutWithoutNexthop(
        {kV4Prefix1, kDefaultPathID, eBgpPeer_, inputAttrs},
        inputAttrs->clone(),
        outputAttrs,
        PostPolicyInfo{});
    EXPECT_EQ(
        kLocalAddr1,
        adjRib_->getNewNexthopFromAttributesOut(
            kV4Prefix1.first.isV4(), outputAttrs)); // local address
    EXPECT_FALSE(outputAttrs->isPublished());

    // Verify AS-PATH is updated (a new segment is created)
    // Repeat from the test without next_hop_self
    EXPECT_NE(inputAttrs->getAsPath(), outputAttrs->getAsPath());
    EXPECT_EQ(2, outputAttrs->getAsPath()->size());
    EXPECT_EQ(1, outputAttrs->getAsPath()->at(0).asConfedSequence.size());
    EXPECT_EQ(kLocalAs1, outputAttrs->getAsPath()->at(0).asConfedSequence[0]);
    EXPECT_EQ(1, outputAttrs->getAsPath()->at(1).asSequence.size());
    EXPECT_EQ(kAsSeqAsNum, outputAttrs->getAsPath()->at(1).asSequence[0]);
  }
  {
    // Verify that we do not modify the nexthop in case of Confed IBGP
    setupAdjRib(
        kLocalAs1,
        kLocalAs1,
        kRemoteAs1,
        kIsRrClientFalse,
        kIsConfedPeerTrue,
        NextHopSelfConfigured(false),
        kV4Nexthop1,
        kV6Nexthop1,
        false);
    BgpUpdate2 inputUpdate = buildBgpUpdateAttributes(kV4Nexthop2);
    auto inputAttrs = std::make_shared<facebook::bgp::BgpPath>(
        BgpPathFields(*BgpUpdate2toBgpPathC(inputUpdate)));
    EXPECT_EQ(1, inputAttrs->getAsPath()->at(0).asSequence.size());
    EXPECT_EQ(kV4Nexthop2, inputAttrs->getNexthop());
    auto outputAttrs = inputAttrs->clone();
    updateAttributesOutWithoutNexthop(
        {kV4Prefix1, kDefaultPathID, eBgpPeer_, inputAttrs},
        inputAttrs->clone(),
        outputAttrs,
        PostPolicyInfo{});
    EXPECT_EQ(kV4Nexthop2, outputAttrs->getNexthop());
    // Check AS-Path is not modified
    EXPECT_EQ(inputAttrs->getAsPath(), outputAttrs->getAsPath());
  }
  {
    // Verify that we keep MED in case of EBGP if set by outbound policy and
    // enableMedComparison is enabled
    loadBgpBestpathFeatures(true);
    auto bgpAction = createBgpPolicyMedAction(kMed2);

    const std::string policyName = "Policy Statement";
    const auto& policyConfig = createBgpPolicies(policyName, {}, {bgpAction});
    auto policyManager = std::make_shared<PolicyManager>(
        policyConfig, createTestBgpGlobalConfig());

    setupAdjRib(
        kLocalAs1,
        kLocalAs1,
        kRemoteAs2, // EBGP
        kIsRrClientFalse,
        kIsConfedPeerFalse,
        NextHopSelfConfigured(false),
        kV4Nexthop1,
        kV6Nexthop1,
        false,
        AfiIpv4Negotiated(true),
        AfiIpv6Negotiated(true),
        V4OverV6Nexthop(false),
        EnhancedRouteRefreshNegotiated(false),
        RouteRefreshNegotiated(false),
        (RemovePrivateAsConfigured(false)),
        policyManager,
        policyName);

    // build some dummy entry for test
    auto ribMsg = std::get<RibOutAnnouncement>(
        createRibSingleAnnounce(kV4Prefix1, kV4Nexthop1, eBgpPeer_, true));
    auto update = ribMsg.entries[0];
    adjRib_->pathIdGenerator_ = std::make_unique<PathIdGenerator>(false);
    adjRib_->processRibAnnouncedEntry(update);
    auto adjRibEntry = adjRib_->getRibEntry(/*ingress=*/false, kV4Prefix1);
    auto outputAttrs = adjRibEntry->getPostAttr();

    EXPECT_THAT(outputAttrs->getMed(), Eq(kMed2));
    EXPECT_THAT(outputAttrs->getIsMedSet(), IsTrue());
    EXPECT_THAT(outputAttrs->isPublished(), IsTrue());
  }
  {
    // Verify that we strip MED in case of EBGP if not set by outbound policy
    // and enableMedComparison is enabled
    loadBgpBestpathFeatures(true);
    setupAdjRib(
        kLocalAs1,
        kLocalAs1,
        kRemoteAs2, // EBGP
        kIsRrClientFalse,
        kIsConfedPeerFalse,
        NextHopSelfConfigured(false),
        kV4Nexthop1,
        kV6Nexthop1,
        false);
    // build some dummy entry for test
    auto ribMsg = std::get<RibOutAnnouncement>(
        createRibSingleAnnounce(kV4Prefix1, kV4Nexthop1, eBgpPeer_, true));
    auto update = ribMsg.entries[0];
    auto mutableAttrs = update.attrs->clone();
    mutableAttrs->setMed(kMed2);
    update.attrs = mutableAttrs;
    EXPECT_EQ(kMed2, update.attrs->getMed());
    adjRib_->pathIdGenerator_ = std::make_unique<PathIdGenerator>(false);
    adjRib_->processRibAnnouncedEntry(update);
    auto adjRibEntry = adjRib_->getRibEntry(false, kV4Prefix1);
    auto outputAttrs = adjRibEntry->getPostAttr();

    EXPECT_THAT(outputAttrs->getIsMedSet(), IsFalse());
    EXPECT_THAT(outputAttrs->isPublished(), IsTrue());
  }
  {
    // Verify that we strip MED in case of EBGP
    setupAdjRib(
        kLocalAs1,
        kLocalAs1,
        kRemoteAs2, // EBGP
        kIsRrClientFalse,
        kIsConfedPeerFalse,
        NextHopSelfConfigured(false),
        kV4Nexthop1,
        kV6Nexthop1,
        false);
    BgpUpdate2 inputUpdate = buildBgpUpdateAttributes(kV4Nexthop2);
    auto inputAttrs = std::make_shared<facebook::bgp::BgpPath>(
        BgpPathFields(*BgpUpdate2toBgpPathC(inputUpdate)));
    EXPECT_EQ(kMed, inputAttrs->getMed());
    inputAttrs->publish();
    auto outputAttrs = inputAttrs->clone();
    updateAttributesOutWithoutNexthop(
        {kV4Prefix1, kDefaultPathID, eBgpPeer_, inputAttrs},
        inputAttrs->clone(),
        outputAttrs,
        PostPolicyInfo{});
    EXPECT_FALSE(outputAttrs->getIsMedSet());
    EXPECT_FALSE(outputAttrs->isPublished());
  }
  {
    // Verify that we add our originatorId and clusterList when advertising
    // eBgp learnt routes to RRC, IBGP
    setupAdjRib(
        kLocalAs1,
        kLocalAs1,
        kRemoteAs1,
        kIsRrClientTrue,
        kIsConfedPeerFalse,
        NextHopSelfConfigured(false),
        kV4Nexthop1,
        kV6Nexthop1,
        false);
    BgpUpdate2 inputUpdate = buildBgpUpdateAttributes(kV4Nexthop2);
    auto inputAttrs = std::make_shared<facebook::bgp::BgpPath>(
        BgpPathFields(*BgpUpdate2toBgpPathC(inputUpdate)));
    inputAttrs->setOriginatorId(0);
    inputAttrs->setClusterList({});
    EXPECT_EQ(0, inputAttrs->getOriginatorId());
    EXPECT_TRUE(inputAttrs->getClusterList().nullOrEmpty());
    inputAttrs->publish();
    auto outputAttrs = inputAttrs->clone();
    updateAttributesOutWithoutNexthop(
        {kV4Prefix1, kDefaultPathID, eBgpPeer_, inputAttrs},
        inputAttrs->clone(),
        outputAttrs,
        PostPolicyInfo{});
    EXPECT_EQ(kLocalAddr1.asV4().toLongHBO(), outputAttrs->getOriginatorId());
    EXPECT_THAT(
        outputAttrs->getClusterList().get(),
        ElementsAre(kLocalAddr1.asV4().toLongHBO()));
    EXPECT_FALSE(outputAttrs->isPublished());
  }
  {
    // Verify that we add our originatorId and clusterList when advertising
    // local routes to RRC, IBGP
    setupAdjRib(
        kLocalAs1,
        kLocalAs1,
        kRemoteAs1,
        kIsRrClientTrue,
        kIsConfedPeerFalse,
        NextHopSelfConfigured(false),
        kV4Nexthop1,
        kV6Nexthop1,
        false);
    BgpUpdate2 inputUpdate = buildBgpUpdateAttributes(kEmptyV4Nexthop);
    auto inputAttrs = std::make_shared<facebook::bgp::BgpPath>(
        BgpPathFields(*BgpUpdate2toBgpPathC(inputUpdate)));
    inputAttrs->setOriginatorId(0);
    inputAttrs->setClusterList({});
    EXPECT_EQ(0, inputAttrs->getOriginatorId());
    EXPECT_TRUE(inputAttrs->getClusterList().nullOrEmpty());
    inputAttrs->publish();
    auto outputAttrs = inputAttrs->clone();
    updateAttributesOutWithoutNexthop(
        {kV4Prefix1, kDefaultPathID, localPeerV4_, inputAttrs},
        inputAttrs->clone(),
        outputAttrs,
        PostPolicyInfo{});
    EXPECT_EQ(kLocalAddr1.asV4().toLongHBO(), outputAttrs->getOriginatorId());
    EXPECT_THAT(
        outputAttrs->getClusterList().get(),
        ElementsAre(kLocalAddr1.asV4().toLongHBO()));
    EXPECT_FALSE(outputAttrs->isPublished());
  }
  {
    // Verify that we add peer's originatorId and clusterList when
    // advertising iBgp learnt routes to RRC, IBGP
    setupAdjRib(
        kLocalAs1,
        kLocalAs1,
        kRemoteAs1,
        kIsRrClientTrue,
        kIsConfedPeerFalse,
        NextHopSelfConfigured(false),
        kV4Nexthop1,
        kV6Nexthop1,
        false);
    BgpUpdate2 inputUpdate = buildBgpUpdateAttributes(kV4Nexthop2);
    auto inputAttrs = std::make_shared<facebook::bgp::BgpPath>(
        BgpPathFields(*BgpUpdate2toBgpPathC(inputUpdate)));
    inputAttrs->setOriginatorId(0);
    inputAttrs->setClusterList({});
    EXPECT_EQ(0, inputAttrs->getOriginatorId());
    EXPECT_TRUE(inputAttrs->getClusterList().nullOrEmpty());
    inputAttrs->publish();
    auto outputAttrs = inputAttrs->clone();
    updateAttributesOutWithoutNexthop(
        {kV4Prefix1, kDefaultPathID, iBgpPeer_, inputAttrs},
        inputAttrs->clone(),
        outputAttrs,
        PostPolicyInfo{});
    EXPECT_EQ(kPeerRouterId2, outputAttrs->getOriginatorId());
    EXPECT_THAT(
        outputAttrs->getClusterList().get(),
        ElementsAre(kLocalAddr1.asV4().toLongHBO()));
    EXPECT_FALSE(outputAttrs->isPublished());
  }
  {
    // Verify that we do not modify originatorId if it existed when
    // advertising iBgp learnt routes to RRC, IBGP
    setupAdjRib(
        kLocalAs1,
        kLocalAs1,
        kRemoteAs1,
        kIsRrClientTrue,
        kIsConfedPeerFalse,
        NextHopSelfConfigured(false),
        kV4Nexthop1,
        kV6Nexthop1,
        false);
    BgpUpdate2 inputUpdate = buildBgpUpdateAttributes(kV4Nexthop2);
    auto inputAttrs = std::make_shared<facebook::bgp::BgpPath>(
        BgpPathFields(*BgpUpdate2toBgpPathC(inputUpdate)));
    EXPECT_EQ(kOriginatorId, inputAttrs->getOriginatorId());
    EXPECT_THAT(inputAttrs->getClusterList().get(), ElementsAre(kOriginatorId));
    inputAttrs->publish();
    auto outputAttrs = inputAttrs->clone();
    updateAttributesOutWithoutNexthop(
        {kV4Prefix1, kDefaultPathID, iBgpPeer_, inputAttrs},
        inputAttrs->clone(),
        outputAttrs,
        PostPolicyInfo{});
    EXPECT_EQ(kOriginatorId, outputAttrs->getOriginatorId());
    EXPECT_THAT(
        outputAttrs->getClusterList().get(),
        ElementsAre(kLocalAddr1.asV4().toLongHBO(), kOriginatorId));
    EXPECT_FALSE(outputAttrs->isPublished());
  }
  {
    // Verify that we add our originatorId and clusterList when advertising
    // eBgp learnt routes to RRC, EBGP
    setupAdjRib(
        kLocalAs1, // AS 1
        kLocalAs1, // AS 1
        kRemoteAs2, // AS 2
        kIsRrClientTrue,
        kIsConfedPeerFalse,
        NextHopSelfConfigured(false),
        kV4Nexthop1,
        kV6Nexthop1,
        false);
    BgpUpdate2 inputUpdate = buildBgpUpdateAttributes(kV4Nexthop2);
    auto inputAttrs = std::make_shared<facebook::bgp::BgpPath>(
        BgpPathFields(*BgpUpdate2toBgpPathC(inputUpdate)));
    inputAttrs->setOriginatorId(0);
    inputAttrs->setClusterList({});
    EXPECT_EQ(0, inputAttrs->getOriginatorId());
    EXPECT_TRUE(inputAttrs->getClusterList().nullOrEmpty());
    inputAttrs->publish();
    auto outputAttrs = inputAttrs->clone();
    updateAttributesOutWithoutNexthop(
        {kV4Prefix1, kDefaultPathID, eBgpPeer_, inputAttrs},
        inputAttrs->clone(),
        outputAttrs,
        PostPolicyInfo{});
    EXPECT_EQ(kLocalAddr1.asV4().toLongHBO(), outputAttrs->getOriginatorId());
    EXPECT_THAT(
        outputAttrs->getClusterList().get(),
        ElementsAre(kLocalAddr1.asV4().toLongHBO()));
    EXPECT_FALSE(outputAttrs->isPublished());
  }
  {
    // Verify that we add our originatorId and clusterList when advertising
    // local routes to RRC, EBGP
    setupAdjRib(
        kLocalAs1,
        kLocalAs1,
        kRemoteAs2,
        kIsRrClientTrue,
        kIsConfedPeerFalse,
        NextHopSelfConfigured(false),
        kV4Nexthop1,
        kV6Nexthop1,
        false);
    BgpUpdate2 inputUpdate = buildBgpUpdateAttributes(kEmptyV4Nexthop);
    auto inputAttrs = std::make_shared<facebook::bgp::BgpPath>(
        BgpPathFields(*BgpUpdate2toBgpPathC(inputUpdate)));
    inputAttrs->setOriginatorId(0);
    inputAttrs->setClusterList({});
    EXPECT_EQ(0, inputAttrs->getOriginatorId());
    EXPECT_TRUE(inputAttrs->getClusterList().nullOrEmpty());
    inputAttrs->publish();
    auto outputAttrs = inputAttrs->clone();
    updateAttributesOutWithoutNexthop(
        {kV4Prefix1, kDefaultPathID, localPeerV4_, inputAttrs},
        inputAttrs->clone(),
        outputAttrs,
        PostPolicyInfo{});
    EXPECT_EQ(kLocalAddr1.asV4().toLongHBO(), outputAttrs->getOriginatorId());
    EXPECT_THAT(
        outputAttrs->getClusterList().get(),
        ElementsAre(kLocalAddr1.asV4().toLongHBO()));
    EXPECT_FALSE(outputAttrs->isPublished());
  }
  {
    // Verify that we add peer's originatorId and clusterList when
    // advertising iBgp learnt routes to RRC, EBGP
    setupAdjRib(
        kLocalAs1,
        kLocalAs1,
        kRemoteAs2,
        kIsRrClientTrue,
        kIsConfedPeerFalse,
        NextHopSelfConfigured(false),
        kV4Nexthop1,
        kV6Nexthop1,
        false);
    BgpUpdate2 inputUpdate = buildBgpUpdateAttributes(kV4Nexthop2);
    auto inputAttrs = std::make_shared<facebook::bgp::BgpPath>(
        BgpPathFields(*BgpUpdate2toBgpPathC(inputUpdate)));
    inputAttrs->setOriginatorId(0);
    inputAttrs->setClusterList({});
    EXPECT_EQ(0, inputAttrs->getOriginatorId());
    EXPECT_TRUE(inputAttrs->getClusterList().nullOrEmpty());
    inputAttrs->publish();
    auto outputAttrs = inputAttrs->clone();
    updateAttributesOutWithoutNexthop(
        {kV4Prefix1, kDefaultPathID, iBgpPeer_, inputAttrs},
        inputAttrs->clone(),
        outputAttrs,
        PostPolicyInfo{});
    EXPECT_EQ(kPeerRouterId2, outputAttrs->getOriginatorId());
    EXPECT_THAT(
        outputAttrs->getClusterList().get(),
        ElementsAre(kLocalAddr1.asV4().toLongHBO()));
    EXPECT_FALSE(outputAttrs->isPublished());
  }
  {
    // Verify that we strip originatorId and clusterList when advertising
    // routes to EBGP
    setupAdjRib(
        kLocalAs1,
        kLocalAs1,
        kRemoteAs2,
        kIsRrClientFalse,
        kIsConfedPeerFalse,
        NextHopSelfConfigured(false),
        kV4Nexthop1,
        kV6Nexthop1,
        false);
    // originatorId and clusterList are set as kOriginatorId
    BgpUpdate2 inputUpdate = buildBgpUpdateAttributes(kV4Nexthop2);
    auto inputAttrs = std::make_shared<facebook::bgp::BgpPath>(
        BgpPathFields(*BgpUpdate2toBgpPathC(inputUpdate)));
    inputAttrs->publish();
    auto outputAttrs = inputAttrs->clone();
    updateAttributesOutWithoutNexthop(
        {kV4Prefix1, kDefaultPathID, iBgpPeer_, inputAttrs},
        inputAttrs->clone(),
        outputAttrs,
        PostPolicyInfo{});
    EXPECT_EQ(0, outputAttrs->getOriginatorId());
    EXPECT_TRUE(outputAttrs->getClusterList().nullOrEmpty());
    EXPECT_FALSE(outputAttrs->isPublished());
  }
  {
    // Verify that we strip originatorId and clusterList when advertising
    // routes to confed EBGP
    setupAdjRib(
        kLocalAs1,
        kLocalAs1,
        kRemoteAs2,
        kIsRrClientFalse,
        kIsConfedPeerTrue,
        NextHopSelfConfigured(false),
        kV4Nexthop1,
        kV6Nexthop1,
        false);
    // originatorId and clusterList are set as kOriginatorId
    BgpUpdate2 inputUpdate = buildBgpUpdateAttributes(kV4Nexthop2);
    auto inputAttrs = std::make_shared<facebook::bgp::BgpPath>(
        BgpPathFields(*BgpUpdate2toBgpPathC(inputUpdate)));
    inputAttrs->publish();
    auto outputAttrs = inputAttrs->clone();
    updateAttributesOutWithoutNexthop(
        {kV4Prefix1, kDefaultPathID, iBgpPeer_, inputAttrs},
        inputAttrs->clone(),
        outputAttrs,
        PostPolicyInfo{});
    EXPECT_EQ(0, outputAttrs->getOriginatorId());
    EXPECT_TRUE(outputAttrs->getClusterList().nullOrEmpty());
    EXPECT_FALSE(outputAttrs->isPublished());
  }
  {
    // Verify that we strip localPref in case of EBGP
    setupAdjRib(
        kLocalAs1,
        kLocalAs1,
        kRemoteAs2, // EBGP
        kIsRrClientFalse,
        kIsConfedPeerFalse,
        NextHopSelfConfigured(false),
        kV4Nexthop1,
        kV6Nexthop1,
        false);
    BgpUpdate2 inputUpdate = buildBgpUpdateAttributes(kV4Nexthop2);
    auto inputAttrs = std::make_shared<facebook::bgp::BgpPath>(
        BgpPathFields(*BgpUpdate2toBgpPathC(inputUpdate)));
    EXPECT_EQ(kLocalPref, inputAttrs->getLocalPref());
    auto outputAttrs = inputAttrs->clone();
    updateAttributesOutWithoutNexthop(
        {kV4Prefix1, kDefaultPathID, iBgpPeer_, inputAttrs},
        inputAttrs->clone(),
        outputAttrs,
        PostPolicyInfo{});
    EXPECT_EQ(std::nullopt, outputAttrs->getLocalPref());
    EXPECT_FALSE(outputAttrs->isPublished());
  }
  {
    // Verify that we do not strip localPref in case of Confed EBGP
    setupAdjRib(
        kLocalAs1,
        kLocalAs1,
        kRemoteAs2, // EBGP
        kIsRrClientFalse,
        kIsConfedPeerTrue,
        NextHopSelfConfigured(false),
        kV4Nexthop1,
        kV6Nexthop1,
        false);
    BgpUpdate2 inputUpdate = buildBgpUpdateAttributes(kV4Nexthop2);
    auto inputAttrs = std::make_shared<facebook::bgp::BgpPath>(
        BgpPathFields(*BgpUpdate2toBgpPathC(inputUpdate)));
    EXPECT_EQ(kLocalPref, inputAttrs->getLocalPref());
    auto outputAttrs = inputAttrs->clone();
    updateAttributesOutWithoutNexthop(
        {kV4Prefix1, kDefaultPathID, iBgpPeer_, inputAttrs},
        inputAttrs->clone(),
        outputAttrs,
        PostPolicyInfo{});
    EXPECT_EQ(kLocalPref, outputAttrs->getLocalPref());
    // AS-Path must have changed
    EXPECT_FALSE(outputAttrs->isPublished());
    EXPECT_NE(inputAttrs->getAsPath(), outputAttrs->getAsPath());
  }
}

/**
 * This test is a sanity check to verify that the original input
 * attrs are preserved if the policy is accept all and there are
 * no additional modifications needed to postPolicyAttrs.
 *
 * updateAttributesOutWithoutNexthop has additional logic
 * to mutate the postPolicyAttrs for EBgpPeers or if the peer is RrClient;
 * this test is for the scenario where we are not EBgpPeer and not RrClient.
 *
 * This means that the output of updateAttributesWithoutNexthop on post
 * policy results should return a ptr to an object that is equal to the
 * pre policy attrs.
 *
 * This test also indirectly checks that the policy cache will return
 * the expected post policy attributes given the same input policy,
 * same pfx, but different BgpPaths.
 */
TEST_F(AdjRibOutboundFixture, VerifyUpdateAttributesWithAcceptAllPolicyTest) {
  // Set up egress policy accept all on AdjRib
  // isRrClient = false AdjRib
  setupAdjRib(
      setupAcceptAllPolicy(kEgressPolicyName), // policyManager
      kEgressPolicyName,
      false /* sessionEstablished */,
      false /* sendAddPath */);
  // pathIdGenerator is needed to call tryInsertRibOutEntry.
  adjRib_->pathIdGenerator_ = std::make_unique<PathIdGenerator>(true);

  // Build two different paths which will be used for the same prefix.
  auto preAttrs1 = std::make_shared<BgpPath>(*buildBgpPathFields(1, 1, 1, 1));
  auto preAttrs2 = std::make_shared<BgpPath>(*buildBgpPathFields(2, 2, 2, 2));
  // buildBgpPathFields starts asPath sequence from 0.
  // In this step, remove zeroes from AsPath with localAs, as they will be
  // replaced in getPostOutPolicyAttributes.
  replaceZerosInAsPath(preAttrs1, adjRib_->peeringParams_.localAs);
  replaceZerosInAsPath(preAttrs2, adjRib_->peeringParams_.localAs);

  // Create RibOutAnnouncementEntry for iBgpPeer with preAttrs1.
  auto update1 = RibOutAnnouncementEntry(
      kV4Prefix1, kPlaceholderPathID, iBgpPeer_, preAttrs1);
  auto adjRibEntry = adjRib_->tryInsertRibOutEntry(
      update1.prefix, update1.attrs->getNexthop(), kPlaceholderPathID);
  // Apply policy on preAttrsClone.
  auto outputAttrs1 = preAttrs1->clone();
  auto postAttrs1 = adjRib_->getPostOutPolicyAttributes(
      update1,
      adjRibEntry,
      outputAttrs1,
      BgpPeerId(update1.peer.addr, update1.peer.routerId).str());
  // Update attributes.
  updateAttributesOutWithoutNexthop(
      update1, postAttrs1, outputAttrs1, PostPolicyInfo{});

  // preAttrs and postAttrs pointers should be different due to cloning.
  EXPECT_NE(preAttrs1, postAttrs1);
  EXPECT_NE(preAttrs1, outputAttrs1);

  // preAttrs should have same values as postAttrs and outputAttrs1 and clone
  // because no policy modification or EBgpPeer, rrClient logic.
  EXPECT_TRUE(*preAttrs1 == *postAttrs1);
  EXPECT_TRUE(*preAttrs1 == *outputAttrs1);

  // For posterity, to check there are no policy caching issues,
  // create RibOutAnnouncementEntry that has different path preAttrs2
  // for the same prefix.
  auto update2 = RibOutAnnouncementEntry(
      kV4Prefix1, kPlaceholderPathID, iBgpPeer_, preAttrs2);
  // Expect the adjRibEntry to already exist.
  EXPECT_EQ(
      adjRibEntry,
      adjRib_->tryInsertRibOutEntry(
          update2.prefix, update2.attrs->getNexthop(), kPlaceholderPathID));
  // Apply policy on preAttrs clone.
  auto outputAttrs2 = preAttrs2->clone();
  auto postAttrs2 = adjRib_->getPostOutPolicyAttributes(
      update2,
      adjRibEntry,
      outputAttrs2,
      BgpPeerId(update2.peer.addr, update2.peer.routerId).str());
  // Update attributes.
  updateAttributesOutWithoutNexthop(
      update2, postAttrs2, outputAttrs2, PostPolicyInfo{});

  // preAttrs and postAttrs pointers should be different due to cloning.
  EXPECT_NE(preAttrs2, postAttrs2);
  EXPECT_NE(preAttrs2, outputAttrs2);

  // preAttrs should have same values as outputAttrs2 and clone.
  // because no policy modification or EBgpPeer, rrClient logic.
  EXPECT_TRUE(*preAttrs2 == *outputAttrs2);
  // preAttrs2 is different from postAttrs2,
  // because postAttrs2 is equal to postAttrs1.
  EXPECT_FALSE(*preAttrs2 == *postAttrs2);

  // Check that the two output attrs are different as sanity check.
  EXPECT_FALSE(*outputAttrs1 == *outputAttrs2);
  // The two input attrs are considered 'same' under policy
  // evaluation, so they should have the same postPolicy attrs.
  EXPECT_TRUE(*postAttrs1 == *postAttrs2);
}

TEST_F(AdjRibOutboundFixture, VerifyRemovePrivateAs) {
  // Define some ASN arrays which we will use as input for our testing.  The
  // first two have only private ASNs, the third has private ASNs but one
  // public ASN in the list.  The fourth has all private ASNs, but one is
  // the ASN of our peer.
  std::array<uint32_t, 2> seg1Arr = {64512, 65534};
  std::array<uint32_t, 3> seg2Arr = {65000, 65000, 65000};
  std::array<uint32_t, 4> seg3Arr = {65000, 64511, 65000, 65000};
  std::array<uint32_t, 3> seg4Arr = {
      65000, static_cast<uint32_t>(kRemotePrivateAs2), 65000};

  // Create asSequence segments from the input arrays
  BgpAttrAsPathSegment asSeq1;
  BgpAttrAsPathSegment asSeq2;
  BgpAttrAsPathSegment asSeq3;
  BgpAttrAsPathSegment asSeq4;
  asSeq1.asSequence()->assign(seg1Arr.begin(), seg1Arr.end());
  asSeq2.asSequence()->assign(seg2Arr.begin(), seg2Arr.end());
  asSeq3.asSequence()->assign(seg3Arr.begin(), seg3Arr.end());
  asSeq4.asSequence()->assign(seg4Arr.begin(), seg4Arr.end());

  // Create an asConfedSequence and an asConfedSet from the third array
  // (which includes a public ASN)
  BgpAttrAsPathSegment asConfedSeq3;
  BgpAttrAsPathSegment asConfedSet3;
  BgpAttrAsPathSegment asConfedSet4;
  asConfedSeq3.asConfedSequence()->assign(seg3Arr.begin(), seg3Arr.end());
  asConfedSet3.asConfedSet()->insert(seg3Arr.begin(), seg3Arr.end());
  asConfedSet4.asConfedSet()->insert(7001);

  // Set up an EBGP Peer with removePrivateAs true
  setupAdjRib(
      kLocalPrivateAs1,
      kLocalPrivateAs1,
      kRemotePrivateAs2, // EBGP
      kIsRrClientFalse,
      kIsConfedPeerFalse,
      NextHopSelfConfigured(false),
      kEmptyV4Nexthop, // no user configured nexthop
      kEmptyV6Nexthop,
      false,
      AfiIpv4Negotiated(true),
      AfiIpv6Negotiated(true),
      V4OverV6Nexthop(false),
      EnhancedRouteRefreshNegotiated(false),
      RouteRefreshNegotiated(false),
      RemovePrivateAsConfigured(true));

  //
  // First perform some tests with removePrivateAs true
  //
  {
    // Receive input update with single AsSequence segment, all of the ASNs
    // within being private.
    std::vector<BgpAttrAsPathSegment> asPath;
    asPath.push_back(asSeq1);
    BgpUpdate2 inputUpdate =
        buildBgpUpdateAttributesAsPath(kV4Nexthop2, asPath);
    auto inputAttrs = std::make_shared<facebook::bgp::BgpPath>(
        BgpPathFields(*BgpUpdate2toBgpPathC(inputUpdate)));
    EXPECT_EQ(inputAttrs->getAsPath()->size(), 1);
    EXPECT_EQ(inputAttrs->getAsPath()->at(0).asSequence.size(), seg1Arr.size());

    // Call updateAttributesOut
    auto outputAttrs = inputAttrs->clone();
    updateAttributesOutWithoutNexthop(
        {kV4Prefix1, kDefaultPathID, eBgpPeer_, inputAttrs},
        inputAttrs->clone(),
        outputAttrs,
        PostPolicyInfo{});

    // Verify that we strip received private asns, but prepend our own
    EXPECT_EQ(outputAttrs->getAsPath()->size(), 1);
    EXPECT_EQ(outputAttrs->getAsPath()->at(0).asSequence.size(), 1);
    EXPECT_EQ(outputAttrs->getAsPath()->at(0).asSequence[0], kLocalPrivateAs1);
  }
  {
    // Receive input update with multiple AsSeqence segments, all with
    // private ASNs.
    std::vector<BgpAttrAsPathSegment> asPath;
    asPath.push_back(asSeq1);
    asPath.push_back(asSeq2);
    BgpUpdate2 inputUpdate =
        buildBgpUpdateAttributesAsPath(kV4Nexthop2, asPath);
    auto inputAttrs = std::make_shared<facebook::bgp::BgpPath>(
        BgpPathFields(*BgpUpdate2toBgpPathC(inputUpdate)));
    EXPECT_EQ(inputAttrs->getAsPath()->size(), 2);
    EXPECT_EQ(inputAttrs->getAsPath()->at(0).asSequence.size(), seg1Arr.size());
    EXPECT_EQ(inputAttrs->getAsPath()->at(1).asSequence.size(), seg2Arr.size());

    // Call updateAttributesOut
    auto outputAttrs = inputAttrs->clone();
    updateAttributesOutWithoutNexthop(
        {kV4Prefix1, kDefaultPathID, eBgpPeer_, inputAttrs},
        inputAttrs->clone(),
        outputAttrs,
        PostPolicyInfo{});

    // Verify that we strip received private asns, but prepend our own
    EXPECT_EQ(outputAttrs->getAsPath()->size(), 1);
    EXPECT_EQ(outputAttrs->getAsPath()->at(0).asSequence.size(), 1);
    EXPECT_EQ(outputAttrs->getAsPath()->at(0).asSequence[0], kLocalPrivateAs1);
  }
  {
    // Receive input update with multiple AsSequence segments one of which
    // has a public ASN.
    std::vector<BgpAttrAsPathSegment> asPath;
    asPath.push_back(asSeq1);
    asPath.push_back(asSeq2);
    asPath.push_back(asSeq3);
    BgpUpdate2 inputUpdate =
        buildBgpUpdateAttributesAsPath(kV4Nexthop2, asPath);
    auto inputAttrs = std::make_shared<facebook::bgp::BgpPath>(
        BgpPathFields(*BgpUpdate2toBgpPathC(inputUpdate)));
    EXPECT_EQ(inputAttrs->getAsPath()->size(), 3);
    EXPECT_EQ(inputAttrs->getAsPath()->at(0).asSequence.size(), seg1Arr.size());
    EXPECT_EQ(inputAttrs->getAsPath()->at(1).asSequence.size(), seg2Arr.size());
    EXPECT_EQ(inputAttrs->getAsPath()->at(2).asSequence.size(), seg3Arr.size());

    // Call updateAttributesOut
    auto outputAttrs = inputAttrs->clone();
    updateAttributesOutWithoutNexthop(
        {kV4Prefix1, kDefaultPathID, eBgpPeer_, inputAttrs},
        inputAttrs->clone(),
        outputAttrs,
        PostPolicyInfo{});

    // Verify none of the asns stripped.  Additionally, the first segment
    // should also have our local ASN prepended
    EXPECT_EQ(outputAttrs->getAsPath()->size(), 3);
    EXPECT_EQ(
        outputAttrs->getAsPath()->at(0).asSequence.size(), seg1Arr.size() + 1);
    EXPECT_EQ(
        outputAttrs->getAsPath()->at(1).asSequence.size(), seg2Arr.size());
    EXPECT_EQ(
        outputAttrs->getAsPath()->at(2).asSequence.size(), seg3Arr.size());
    EXPECT_EQ(outputAttrs->getAsPath()->at(0).asSequence[0], kLocalPrivateAs1);
  }
  {
    // Receive input update with two AsSequence segments with all private
    // ANSs, and one ConfedAsSequence segment with a public ASN.
    // ConfedAsSequence is at the end of the list.  In BGP terminology, the
    // two AsSequence segments were added "after" the ConfedAs
    std::vector<BgpAttrAsPathSegment> asPath;
    asPath.push_back(asSeq1);
    asPath.push_back(asSeq2);
    asPath.push_back(asConfedSeq3);
    BgpUpdate2 inputUpdate =
        buildBgpUpdateAttributesAsPath(kV4Nexthop2, asPath);
    auto inputAttrs = std::make_shared<facebook::bgp::BgpPath>(
        BgpPathFields(*BgpUpdate2toBgpPathC(inputUpdate)));
    EXPECT_EQ(inputAttrs->getAsPath()->size(), 3);
    EXPECT_EQ(inputAttrs->getAsPath()->at(0).asSequence.size(), seg1Arr.size());
    EXPECT_EQ(inputAttrs->getAsPath()->at(1).asSequence.size(), seg2Arr.size());
    EXPECT_EQ(
        inputAttrs->getAsPath()->at(2).asConfedSequence.size(), seg3Arr.size());

    // Call updateAttributesOut
    auto outputAttrs = inputAttrs->clone();
    updateAttributesOutWithoutNexthop(
        {kV4Prefix1, kDefaultPathID, eBgpPeer_, inputAttrs},
        inputAttrs->clone(),
        outputAttrs,
        PostPolicyInfo{});

    // Verify that we remove the ConfedAsSequence.  Verify that we strip the
    // other two segments because they were added after the confed segment
    // and had all private ASNs.  Output should only contain only our local
    // ASN in its AsPath
    EXPECT_EQ(outputAttrs->getAsPath()->size(), 1);
    EXPECT_EQ(outputAttrs->getAsPath()->at(0).asSequence.size(), 1);
    EXPECT_EQ(outputAttrs->getAsPath()->at(0).asSequence[0], kLocalPrivateAs1);
  }
  {
    // Receive input update with two AsSequence segments with all private
    // ANSs, and one ConfedAsSequence segment with a public ASN.
    // ConfedAsSequence is in the middle of the list.
    std::vector<BgpAttrAsPathSegment> asPath;
    asPath.push_back(asSeq1);
    asPath.push_back(asConfedSeq3);
    asPath.push_back(asSeq2);
    BgpUpdate2 inputUpdate =
        buildBgpUpdateAttributesAsPath(kV4Nexthop2, asPath);
    auto inputAttrs = std::make_shared<facebook::bgp::BgpPath>(
        BgpPathFields(*BgpUpdate2toBgpPathC(inputUpdate)));
    EXPECT_EQ(inputAttrs->getAsPath()->size(), 3);
    EXPECT_EQ(inputAttrs->getAsPath()->at(0).asSequence.size(), seg1Arr.size());
    EXPECT_EQ(
        inputAttrs->getAsPath()->at(1).asConfedSequence.size(), seg3Arr.size());
    EXPECT_EQ(inputAttrs->getAsPath()->at(2).asSequence.size(), seg2Arr.size());

    // Call updateAttributesOut
    auto outputAttrs = inputAttrs->clone();
    updateAttributesOutWithoutNexthop(
        {kV4Prefix1, kDefaultPathID, eBgpPeer_, inputAttrs},
        inputAttrs->clone(),
        outputAttrs,
        PostPolicyInfo{});

    // Verify that we remove all confed segment, also that we remove
    // the AsSequence segment after all confed portion are removed.
    // Verify that we prepend our ASN to this segment.
    EXPECT_EQ(outputAttrs->getAsPath()->size(), 1);
    EXPECT_EQ(outputAttrs->getAsPath()->at(0).asSequence.size(), 1);
    EXPECT_EQ(outputAttrs->getAsPath()->at(0).asSequence[0], kLocalPrivateAs1);
  }
  {
    // Receive input update with two AsSequence segments with all private
    // ANSs, and one ConfedAsSet segment with a public ASN.  ConfedAsSet is
    // in the head of the list.
    // In this case confed as will be remove first -> remove private as will
    // take place and remove the rest private as sequence
    std::vector<BgpAttrAsPathSegment> asPath;
    asPath.push_back(asConfedSet4);
    asPath.push_back(asSeq1);
    BgpUpdate2 inputUpdate =
        buildBgpUpdateAttributesAsPath(kV4Nexthop2, asPath);
    auto inputAttrs = std::make_shared<facebook::bgp::BgpPath>(
        BgpPathFields(*BgpUpdate2toBgpPathC(inputUpdate)));
    EXPECT_EQ(inputAttrs->getAsPath()->size(), 2);
    EXPECT_EQ(
        inputAttrs->getAsPath()->at(0).asConfedSet.size(),
        asConfedSet4.asConfedSet()->size());
    EXPECT_EQ(inputAttrs->getAsPath()->at(1).asSequence.size(), seg1Arr.size());

    // Call updateAttributesOut
    auto outputAttrs = inputAttrs->clone();
    updateAttributesOutWithoutNexthop(
        {kV4Prefix1, kDefaultPathID, eBgpPeer_, inputAttrs},
        inputAttrs->clone(),
        outputAttrs,
        PostPolicyInfo{});

    // Verify that we remove all confed segment, also that we remove
    // the AsSequence segment after all confed portion are removed.
    // Verify that we prepend our ASN to this segment.
    EXPECT_EQ(outputAttrs->getAsPath()->size(), 1);
    EXPECT_EQ(outputAttrs->getAsPath()->at(0).asSequence.size(), 1);
    EXPECT_EQ(outputAttrs->getAsPath()->at(0).asSequence[0], kLocalPrivateAs1);
  }
  {
    // Receive input update with two AsSequence segments and one
    // ConfedAsSequence (in the middle).  The nearer AsSequence has all
    // private ANSs, but the farther has a public ASN.
    std::vector<BgpAttrAsPathSegment> asPath;
    asPath.push_back(asSeq1);
    asPath.push_back(asConfedSeq3);
    asPath.push_back(asSeq3);
    BgpUpdate2 inputUpdate =
        buildBgpUpdateAttributesAsPath(kV4Nexthop2, asPath);
    auto inputAttrs = std::make_shared<facebook::bgp::BgpPath>(
        BgpPathFields(*BgpUpdate2toBgpPathC(inputUpdate)));
    EXPECT_EQ(inputAttrs->getAsPath()->size(), 3);
    EXPECT_EQ(inputAttrs->getAsPath()->at(0).asSequence.size(), seg1Arr.size());
    EXPECT_EQ(
        inputAttrs->getAsPath()->at(1).asConfedSequence.size(), seg3Arr.size());
    EXPECT_EQ(inputAttrs->getAsPath()->at(2).asSequence.size(), seg3Arr.size());

    // Call updateAttributesOut
    auto outputAttrs = inputAttrs->clone();
    updateAttributesOutWithoutNexthop(
        {kV4Prefix1, kDefaultPathID, eBgpPeer_, inputAttrs},
        inputAttrs->clone(),
        outputAttrs,
        PostPolicyInfo{});

    // Verify that none of the asns in the AsSequences are stripped
    // (ConfedAsSequence will be stripped).  Verify that local ASN is
    // prepended to first segment
    EXPECT_EQ(outputAttrs->getAsPath()->size(), 2);
    EXPECT_EQ(
        outputAttrs->getAsPath()->at(0).asSequence.size(), seg1Arr.size() + 1);
    EXPECT_EQ(
        outputAttrs->getAsPath()->at(1).asSequence.size(), seg3Arr.size());
    EXPECT_EQ(outputAttrs->getAsPath()->at(0).asSequence[0], kLocalPrivateAs1);
  }
  {
    // Receive input update with two AsSequence segments.  All of the ASNs
    // are private, but one happens to be the ASN of our peer.
    std::vector<BgpAttrAsPathSegment> asPath;
    asPath.push_back(asSeq1);
    asPath.push_back(asSeq4);
    BgpUpdate2 inputUpdate =
        buildBgpUpdateAttributesAsPath(kV4Nexthop2, asPath);
    auto inputAttrs = std::make_shared<facebook::bgp::BgpPath>(
        BgpPathFields(*BgpUpdate2toBgpPathC(inputUpdate)));
    EXPECT_EQ(inputAttrs->getAsPath()->size(), 2);
    EXPECT_EQ(inputAttrs->getAsPath()->at(0).asSequence.size(), seg1Arr.size());
    EXPECT_EQ(inputAttrs->getAsPath()->at(1).asSequence.size(), seg4Arr.size());

    // Call updateAttributesOut
    auto outputAttrs = inputAttrs->clone();
    updateAttributesOutWithoutNexthop(
        {kV4Prefix1, kDefaultPathID, eBgpPeer_, inputAttrs},
        inputAttrs->clone(),
        outputAttrs,
        PostPolicyInfo{});

    // Verify we will remove private ASNs and append our own ASN.
    EXPECT_EQ(outputAttrs->getAsPath()->size(), 1);
    EXPECT_EQ(outputAttrs->getAsPath()->at(0).asSequence.size(), 1);
    EXPECT_EQ(outputAttrs->getAsPath()->at(0).asSequence[0], kLocalPrivateAs1);
  }

  //
  // Now perform some tests with removePrivateAs false
  //
  setupAdjRib(
      kLocalPrivateAs1,
      kLocalPrivateAs1,
      kRemoteAs2, // EBGP
      kIsRrClientFalse,
      kIsConfedPeerFalse,
      NextHopSelfConfigured(false),
      kEmptyV4Nexthop, // no user configured nexthop
      kEmptyV6Nexthop,
      false,
      AfiIpv4Negotiated(true),
      AfiIpv6Negotiated(true),
      V4OverV6Nexthop(false),
      RemovePrivateAsConfigured(false));

  {
    // Receive input update with single AsSequence segment, all of the ASNs
    // within being private.
    std::vector<BgpAttrAsPathSegment> asPath;
    asPath.push_back(asSeq1);
    BgpUpdate2 inputUpdate =
        buildBgpUpdateAttributesAsPath(kV4Nexthop2, asPath);
    auto inputAttrs = std::make_shared<facebook::bgp::BgpPath>(
        BgpPathFields(*BgpUpdate2toBgpPathC(inputUpdate)));
    EXPECT_EQ(inputAttrs->getAsPath()->size(), 1);
    EXPECT_EQ(inputAttrs->getAsPath()->at(0).asSequence.size(), seg1Arr.size());

    // Call updateAttributesOut
    auto outputAttrs = inputAttrs->clone();
    updateAttributesOutWithoutNexthop(
        {kV4Prefix1, kDefaultPathID, eBgpPeer_, inputAttrs},
        inputAttrs->clone(),
        outputAttrs,
        PostPolicyInfo{});

    // Verify that input ASNs are not stripped.  Verify that we prepend our
    // own ASN to the front of the sequence.
    EXPECT_EQ(outputAttrs->getAsPath()->size(), 1);
    EXPECT_EQ(
        outputAttrs->getAsPath()->at(0).asSequence.size(), seg1Arr.size() + 1);
    EXPECT_EQ(outputAttrs->getAsPath()->at(0).asSequence[0], kLocalPrivateAs1);
  }
  {
    // Receive input update with AsSequence and ConfedAsSequence segments.
    // ConfedAsSequence is at the end of the list.
    std::vector<BgpAttrAsPathSegment> asPath;
    asPath.push_back(asSeq1);
    asPath.push_back(asSeq2);
    asPath.push_back(asConfedSeq3);
    BgpUpdate2 inputUpdate =
        buildBgpUpdateAttributesAsPath(kV4Nexthop2, asPath);
    auto inputAttrs = std::make_shared<facebook::bgp::BgpPath>(
        BgpPathFields(*BgpUpdate2toBgpPathC(inputUpdate)));
    EXPECT_EQ(inputAttrs->getAsPath()->size(), 3);
    EXPECT_EQ(inputAttrs->getAsPath()->at(0).asSequence.size(), seg1Arr.size());
    EXPECT_EQ(inputAttrs->getAsPath()->at(1).asSequence.size(), seg2Arr.size());
    EXPECT_EQ(
        inputAttrs->getAsPath()->at(2).asConfedSequence.size(), seg3Arr.size());

    // Call updateAttributesOut
    auto outputAttrs = inputAttrs->clone();
    updateAttributesOutWithoutNexthop(
        {kV4Prefix1, kDefaultPathID, eBgpPeer_, inputAttrs},
        inputAttrs->clone(),
        outputAttrs,
        PostPolicyInfo{});

    // Verify that ConfedAsSequence segment is removed.  Verify that the
    // private ASNs in the AsSequence are not stripped.  Verify that we
    // prepend our own ASN to the front of first sequence.
    EXPECT_EQ(outputAttrs->getAsPath()->size(), 2);
    EXPECT_EQ(
        outputAttrs->getAsPath()->at(0).asSequence.size(), seg1Arr.size() + 1);
    EXPECT_EQ(
        outputAttrs->getAsPath()->at(1).asSequence.size(), seg2Arr.size());
    EXPECT_EQ(outputAttrs->getAsPath()->at(0).asSequence[0], kLocalPrivateAs1);
  }
  {
    // Receive input update with AsSequence and ConfedAsSequence segments
    // ConfedAsSequence is at the beginning of the list.
    std::vector<BgpAttrAsPathSegment> asPath;
    asPath.push_back(asConfedSeq3);
    asPath.push_back(asSeq1);
    asPath.push_back(asSeq2);
    BgpUpdate2 inputUpdate =
        buildBgpUpdateAttributesAsPath(kV4Nexthop2, asPath);
    auto inputAttrs = std::make_shared<facebook::bgp::BgpPath>(
        BgpPathFields(*BgpUpdate2toBgpPathC(inputUpdate)));
    EXPECT_EQ(inputAttrs->getAsPath()->size(), 3);
    EXPECT_EQ(
        inputAttrs->getAsPath()->at(0).asConfedSequence.size(), seg3Arr.size());
    EXPECT_EQ(inputAttrs->getAsPath()->at(1).asSequence.size(), seg1Arr.size());
    EXPECT_EQ(inputAttrs->getAsPath()->at(2).asSequence.size(), seg2Arr.size());

    // Call updateAttributesOut
    auto outputAttrs = inputAttrs->clone();
    updateAttributesOutWithoutNexthop(
        {kV4Prefix1, kDefaultPathID, eBgpPeer_, inputAttrs},
        inputAttrs->clone(),
        outputAttrs,
        PostPolicyInfo{});

    // Verify that ConfedAsSequence segment is removed.  Verify that the
    // private ASNs in the AsSequence are not stripped.  Verify that we
    // prepend our own ASN to the front of first sequence.
    EXPECT_EQ(outputAttrs->getAsPath()->size(), 2);
    EXPECT_EQ(
        outputAttrs->getAsPath()->at(0).asSequence.size(), seg1Arr.size() + 1);
    EXPECT_EQ(
        outputAttrs->getAsPath()->at(1).asSequence.size(), seg2Arr.size());
    EXPECT_EQ(outputAttrs->getAsPath()->at(0).asSequence[0], kLocalPrivateAs1);
  }

  //
  // Now perform some IBGP tests with removePrivateAs true
  //
  setupAdjRib(
      kLocalPrivateAs1,
      kLocalPrivateAs1,
      kLocalPrivateAs1, // IBGP
      kIsRrClientFalse,
      kIsConfedPeerFalse,
      NextHopSelfConfigured(false),
      kEmptyV4Nexthop, // no user configured nexthop
      kEmptyV6Nexthop,
      false,
      AfiIpv4Negotiated(true),
      AfiIpv6Negotiated(true),
      V4OverV6Nexthop(false),
      RemovePrivateAsConfigured(true));

  {
    // Receive input update with single AsSequence segment, all of the ASNs
    // within being private.
    std::vector<BgpAttrAsPathSegment> asPath;
    asPath.push_back(asSeq1);
    BgpUpdate2 inputUpdate =
        buildBgpUpdateAttributesAsPath(kV4Nexthop2, asPath);
    auto inputAttrs = std::make_shared<facebook::bgp::BgpPath>(
        BgpPathFields(*BgpUpdate2toBgpPathC(inputUpdate)));
    EXPECT_EQ(inputAttrs->getAsPath()->size(), 1);
    EXPECT_EQ(inputAttrs->getAsPath()->at(0).asSequence.size(), seg1Arr.size());

    // Call updateAttributesOut
    auto outputAttrs = inputAttrs->clone();
    updateAttributesOutWithoutNexthop(
        {kV4Prefix1, kDefaultPathID, eBgpPeer_, inputAttrs},
        inputAttrs->clone(),
        outputAttrs,
        PostPolicyInfo{});

    // Verify that we do not remove any ASNs, even though they are all
    // private
    EXPECT_EQ(outputAttrs->getAsPath(), inputAttrs->getAsPath());
  }
  {
    // Receive input update with AsSequence and ConfedAsSequence segments.
    // ConfedAsSequence is at the end of the list.
    std::vector<BgpAttrAsPathSegment> asPath;
    asPath.push_back(asSeq1);
    asPath.push_back(asSeq2);
    asPath.push_back(asConfedSeq3);
    BgpUpdate2 inputUpdate =
        buildBgpUpdateAttributesAsPath(kV4Nexthop2, asPath);
    auto inputAttrs = std::make_shared<facebook::bgp::BgpPath>(
        BgpPathFields(*BgpUpdate2toBgpPathC(inputUpdate)));
    EXPECT_EQ(inputAttrs->getAsPath()->size(), 3);
    EXPECT_EQ(inputAttrs->getAsPath()->at(0).asSequence.size(), seg1Arr.size());
    EXPECT_EQ(inputAttrs->getAsPath()->at(1).asSequence.size(), seg2Arr.size());
    EXPECT_EQ(
        inputAttrs->getAsPath()->at(2).asConfedSequence.size(), seg3Arr.size());

    // Call updateAttributesOut
    auto outputAttrs = inputAttrs->clone();
    updateAttributesOutWithoutNexthop(
        {kV4Prefix1, kDefaultPathID, eBgpPeer_, inputAttrs},
        inputAttrs->clone(),
        outputAttrs,
        PostPolicyInfo{});

    // Verify that we do not remove private ASNs, confed segment, because
    // this is IBGP
    EXPECT_EQ(outputAttrs->getAsPath(), inputAttrs->getAsPath());
  }
  {
    // Receive input update with AsSequence and ConfedAsSequence segments
    // ConfedAsSequence is at the beginning of the list.
    std::vector<BgpAttrAsPathSegment> asPath;
    asPath.push_back(asConfedSeq3);
    asPath.push_back(asSeq1);
    asPath.push_back(asSeq2);
    BgpUpdate2 inputUpdate =
        buildBgpUpdateAttributesAsPath(kV4Nexthop2, asPath);
    auto inputAttrs = std::make_shared<facebook::bgp::BgpPath>(
        BgpPathFields(*BgpUpdate2toBgpPathC(inputUpdate)));
    EXPECT_EQ(inputAttrs->getAsPath()->size(), 3);
    EXPECT_EQ(
        inputAttrs->getAsPath()->at(0).asConfedSequence.size(), seg3Arr.size());
    EXPECT_EQ(inputAttrs->getAsPath()->at(1).asSequence.size(), seg1Arr.size());
    EXPECT_EQ(inputAttrs->getAsPath()->at(2).asSequence.size(), seg2Arr.size());

    // Call updateAttributesOut
    auto outputAttrs = inputAttrs->clone();
    updateAttributesOutWithoutNexthop(
        {kV4Prefix1, kDefaultPathID, eBgpPeer_, inputAttrs},
        inputAttrs->clone(),
        outputAttrs,
        PostPolicyInfo{});

    // Verify that we do not remove private ASNs, confed segment, because
    // this is IBGP
    EXPECT_EQ(outputAttrs->getAsPath(), inputAttrs->getAsPath());
  }
}

/*
 * Unit test the function updateAttributesOut
 * Currently cover the cases when the postOutAttrs is nullptr, return
 * nullptr and when the postOutAttrs is published, check would fail
 */
TEST_F(AdjRibOutboundFixture, UpdateAttributesOutTest) {
  // create some entries
  auto ribMsg = std::get<RibOutAnnouncement>(
      createRibSingleAnnounce(kV4Prefix1, kV4Nexthop1, localPeerV4_, true));
  auto& update = ribMsg.entries[0];
  auto postOutAttrs = std::make_shared<facebook::bgp::BgpPath>(BgpPathFields(
      *BgpUpdate2toBgpPathC(buildBgpUpdateAttributes(kV4Nexthop1))));

  setupAdjRibForOutUnitTest();

  // nothing to update
  {
    std::shared_ptr<facebook::bgp::BgpPath> updatedAttr;
    updateAttributesOutWithoutNexthop(
        update,
        nullptr, /* policyResultAttrs */
        updatedAttr, /* attrsToUpdate */
        PostPolicyInfo{});
    EXPECT_EQ(updatedAttr, nullptr);
  }

  // published postOutAttrs is not allowed
  {
    postOutAttrs->publish();

    EXPECT_DEATH(
        updateAttributesOutWithoutNexthop(
            update,
            postOutAttrs /* policyResultAttrs */,
            postOutAttrs /* attrsToUpdate */,
            PostPolicyInfo{}),
        "");
  }
}

TEST_F(AdjRibOutboundFixture, UpdateAsPathAttributesTest) {
  // localAS != remoteAS, eBGP session
  setupAdjRib(
      kLocalAs1, /* globalAs */
      kLocalAs1, /* localAs */
      kRemoteAs2, /* remoteAs */
      false, /* isRrClient */
      false, /* isConfedPeer */
      false, /* nexthopSelf */
      kV4Nexthop1, /* v4Nexthop */
      kV6Nexthop1, /* v6Nexthop */
      false /* call sessionEstablished */);

  BgpUpdate2 inputUpdate = buildBgpUpdateAttributes(kV4Nexthop2);
  auto inputAttrs = std::make_shared<facebook::bgp::BgpPath>(
      BgpPathFields(*BgpUpdate2toBgpPathC(inputUpdate)));
  EXPECT_EQ(1, inputAttrs->getAsPath()->at(0).asSequence.size());
  EXPECT_EQ(kAsSeqAsNum, inputAttrs->getAsPath()->at(0).asSequence[0]);

  auto attrsToUpdate = inputAttrs->clone();
  updateAsPathAttributesCommon(adjRib_->peeringParams_, attrsToUpdate);

  EXPECT_EQ(2, attrsToUpdate->getAsPath()->at(0).asSequence.size());
  EXPECT_EQ(kLocalAs1, attrsToUpdate->getAsPath()->at(0).asSequence[0]);
  EXPECT_EQ(kAsSeqAsNum, attrsToUpdate->getAsPath()->at(0).asSequence[1]);
}

TEST_F(AdjRibOutboundFixture, UpdateLocalPrefTest) {
  // localAS != remoteAS, eBGP session
  setupAdjRib(
      kLocalAs1, /* globalAs */
      kLocalAs1, /* localAs */
      kRemoteAs2, /* remoteAs */
      false, /* isRrClient */
      false, /* isConfedPeer */
      false, /* nexthopSelf */
      kV4Nexthop1, /* v4Nexthop */
      kV6Nexthop1, /* v6Nexthop */
      false /* call sessionEstablished */);

  BgpUpdate2 inputUpdate = buildBgpUpdateAttributes(kV4Nexthop2);
  auto inputAttrs = std::make_shared<facebook::bgp::BgpPath>(
      BgpPathFields(*BgpUpdate2toBgpPathC(inputUpdate)));
  EXPECT_EQ(kLocalPref, inputAttrs->getLocalPref());

  auto attrsToUpdate = inputAttrs->clone();
  updateLocalPrefCommon(adjRib_->peeringParams_, attrsToUpdate);

  EXPECT_EQ(std::nullopt, attrsToUpdate->getLocalPref());
}

TEST_F(AdjRibOutboundFixture, UpdateLocalPrefIBgpTest) {
  // localAS == remoteAS, iBGP session
  setupAdjRib(
      kLocalAs1, /* globalAs */
      kLocalAs1, /* localAs */
      kRemoteAs1, /* remoteAs */
      true, /* isRrClient */
      false, /* isConfedPeer */
      false, /* nexthopSelf */
      kV4Nexthop1, /* v4Nexthop */
      kV6Nexthop1, /* v6Nexthop */
      false /* call sessionEstablished */);

  BgpUpdate2 inputUpdate = buildBgpUpdateAttributes(kV4Nexthop2);
  auto inputAttrs = std::make_shared<facebook::bgp::BgpPath>(
      BgpPathFields(*BgpUpdate2toBgpPathC(inputUpdate)));
  EXPECT_EQ(kLocalPref, inputAttrs->getLocalPref());

  auto attrsToUpdate = inputAttrs->clone();
  updateLocalPrefCommon(adjRib_->peeringParams_, attrsToUpdate);

  EXPECT_EQ(kLocalPref, attrsToUpdate->getLocalPref());
}

TEST_F(AdjRibOutboundFixture, UpdateMedTest) {
  loadBgpBestpathFeatures(true);

  // localAS != remoteAS, eBGP session
  setupAdjRib(
      kLocalAs1, /* globalAs */
      kLocalAs1, /* localAs */
      kRemoteAs2, /* remoteAs */
      false, /* isRrClient */
      false, /* isConfedPeer */
      false, /* nexthopSelf */
      kV4Nexthop1, /* v4Nexthop */
      kV6Nexthop1, /* v6Nexthop */
      false /* call sessionEstablished */);

  BgpUpdate2 inputUpdate = buildBgpUpdateAttributes(kV4Nexthop2);
  auto inputAttrs = std::make_shared<facebook::bgp::BgpPath>(
      BgpPathFields(*BgpUpdate2toBgpPathC(inputUpdate)));
  EXPECT_EQ(kMed, inputAttrs->getMed());

  auto attrsToUpdate = inputAttrs->clone();
  PostPolicyInfo postPolicyInfo;
  postPolicyInfo.isMedSetByPolicy = false;
  updateMedCommon(adjRib_->peeringParams_, attrsToUpdate, postPolicyInfo);

  EXPECT_FALSE(attrsToUpdate->getIsMedSet());
}

TEST_F(AdjRibOutboundFixture, UpdateMedWithPolicySetTest) {
  loadBgpBestpathFeatures(true);

  // localAS != remoteAS, eBGP session
  setupAdjRib(
      kLocalAs1, /* globalAs */
      kLocalAs1, /* localAs */
      kRemoteAs2, /* remoteAs */
      false, /* isRrClient */
      false, /* isConfedPeer */
      false, /* nexthopSelf */
      kV4Nexthop1, /* v4Nexthop */
      kV6Nexthop1, /* v6Nexthop */
      false /* call sessionEstablished */);

  BgpUpdate2 inputUpdate = buildBgpUpdateAttributes(kV4Nexthop2);
  auto inputAttrs = std::make_shared<facebook::bgp::BgpPath>(
      BgpPathFields(*BgpUpdate2toBgpPathC(inputUpdate)));
  EXPECT_EQ(kMed, inputAttrs->getMed());

  auto attrsToUpdate = inputAttrs->clone();
  PostPolicyInfo postPolicyInfo;
  postPolicyInfo.isMedSetByPolicy = true;
  updateMedCommon(adjRib_->peeringParams_, attrsToUpdate, postPolicyInfo);

  EXPECT_EQ(kMed, attrsToUpdate->getMed());
}

TEST_F(AdjRibOutboundFixture, UpdateOriginAndClusterListEBgpTest) {
  // localAS != remoteAS, eBGP session
  setupAdjRib(
      kLocalAs1, /* globalAs */
      kLocalAs1, /* localAs */
      kRemoteAs2, /* remoteAs */
      false, /* isRrClient */
      true, /* isConfedPeer */
      false, /* nexthopSelf */
      kV4Nexthop1, /* v4Nexthop */
      kV6Nexthop1, /* v6Nexthop */
      false /* call sessionEstablished */);

  BgpUpdate2 inputUpdate = buildBgpUpdateAttributes(kV4Nexthop2);
  auto inputAttrs = std::make_shared<facebook::bgp::BgpPath>(
      BgpPathFields(*BgpUpdate2toBgpPathC(inputUpdate)));
  inputAttrs->setOriginatorId(kOriginatorId);
  BgpAttrClusterListC clusterList{{kOriginatorId}};
  inputAttrs->setClusterList(std::move(clusterList));

  auto attrsToUpdate = inputAttrs->clone();
  RibOutAnnouncementEntry update(
      kV4Prefix1, kDefaultPathID, eBgpPeer_, inputAttrs);
  updateOriginAndClusterListCommon(
      adjRib_->getPeeringParams(), update, attrsToUpdate);

  EXPECT_EQ(0, attrsToUpdate->getOriginatorId());
  EXPECT_TRUE(attrsToUpdate->getClusterList().nullOrEmpty());
}

TEST_F(AdjRibOutboundFixture, UpdateOriginAndClusterListRRClientTest) {
  // localAS != remoteAS, eBGP session
  setupAdjRib(
      kLocalAs1, /* globalAs */
      kLocalAs1, /* localAs */
      kRemoteAs2, /* remoteAs */
      true, /* isRrClient */
      false, /* isConfedPeer */
      false, /* nexthopSelf */
      kV4Nexthop1, /* v4Nexthop */
      kV6Nexthop1, /* v6Nexthop */
      false /* call sessionEstablished */);

  BgpUpdate2 inputUpdate = buildBgpUpdateAttributes(kV4Nexthop2);
  auto inputAttrs = std::make_shared<facebook::bgp::BgpPath>(
      BgpPathFields(*BgpUpdate2toBgpPathC(inputUpdate)));
  inputAttrs->setOriginatorId(0);
  inputAttrs->setClusterList({});

  auto attrsToUpdate = inputAttrs->clone();
  RibOutAnnouncementEntry update(
      kV4Prefix1, kDefaultPathID, eBgpPeer_, inputAttrs);
  updateOriginAndClusterListCommon(
      adjRib_->getPeeringParams(), update, attrsToUpdate);

  EXPECT_EQ(kLocalAddr1.asV4().toLongHBO(), attrsToUpdate->getOriginatorId());
  EXPECT_EQ(1, attrsToUpdate->getClusterList()->size());
  EXPECT_EQ(
      kLocalAddr1.asV4().toLongHBO(), attrsToUpdate->getClusterList()->at(0));
}

TEST_F(AdjRibOutboundFixture, UpdateOriginAndClusterListRRClientIBgpTest) {
  // localAS == remoteAS, iBGP session
  setupAdjRib(
      kLocalAs1, /* globalAs */
      kLocalAs1, /* localAs */
      kRemoteAs1, /* remoteAs */
      true, /* isRrClient */
      false, /* isConfedPeer */
      false, /* nexthopSelf */
      kV4Nexthop1, /* v4Nexthop */
      kV6Nexthop1, /* v6Nexthop */
      false /* call sessionEstablished */);

  BgpUpdate2 inputUpdate = buildBgpUpdateAttributes(kV4Nexthop2);
  auto inputAttrs = std::make_shared<facebook::bgp::BgpPath>(
      BgpPathFields(*BgpUpdate2toBgpPathC(inputUpdate)));
  inputAttrs->setOriginatorId(0);
  inputAttrs->setClusterList({});

  auto attrsToUpdate = inputAttrs->clone();
  RibOutAnnouncementEntry update(
      kV4Prefix1, kDefaultPathID, iBgpPeer_, inputAttrs);
  updateOriginAndClusterListCommon(
      adjRib_->getPeeringParams(), update, attrsToUpdate);

  EXPECT_EQ(kPeerRouterId2, attrsToUpdate->getOriginatorId());
  EXPECT_EQ(1, attrsToUpdate->getClusterList()->size());
  EXPECT_EQ(
      kLocalAddr1.asV4().toLongHBO(), attrsToUpdate->getClusterList()->at(0));
}

TEST_F(AdjRibOutboundFixture, UpdateGroupKeyCreationTest) {
  // localAS == remoteAS, iBGP session
  setupAdjRib(
      kLocalAs1, /* globalAs */
      kLocalAs1, /* localAs */
      kRemoteAs1, /* remoteAs */
      true, /* isRrClient */
      false, /* isConfedPeer */
      false, /* nexthopSelf */
      kV4Nexthop1, /* v4Nexthop */
      kV6Nexthop1, /* v6Nexthop */
      true /* call sessionEstablished */);

  fm_->addTask([&] {
    auto key = UpdateGroupKey::buildUpdateGroupKey(
        "", /* egress policy name */
        "", /* route filter stmt name */
        std::chrono::seconds(0), /* outdelay */
        BgpSessionType::IBGP,
        true, /* isAfiIpv4Negotiated */
        true, /* isAfiIpv6Negotiated */
        false, /* isConfedPeer */
        true, /* isRrClient */
        AdvertiseLinkBandwidth::DISABLE,
        ReceiveLinkBandwidth::ACCEPT,
        0, /* linkBandwidthBps */
        false, /* removePrivateAsn */
        false, /* sendAddPath */
        true, /* as4ByteCapable */
        false, /* extNhEncodingCapable */
        "", /* peerGroupName */
        false /* peerOverride */
    );
    EXPECT_EQ(key, adjRib_->updateGroupKey_);

    // terminate the tasks
    terminateAdjRib();
  });
  evb_.loop();
}

} // namespace facebook::bgp
