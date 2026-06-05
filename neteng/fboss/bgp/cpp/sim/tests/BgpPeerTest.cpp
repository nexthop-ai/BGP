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

/*
 * Tests for BgpPeer — the simulator's per-session peer config object.
 * Builds thrift::BgpPeer / thrift::PeerGroup structs in memory (no config
 * files) and verifies config resolution, peer > peer-group inheritance,
 * peer-type classification, and received-route bookkeeping.
 */

#include <gtest/gtest.h>

#include <folly/IPAddress.h>

#include "configerator/structs/neteng/fboss/bgp/gen-cpp2/bgp_config_types.h"
#include "neteng/fboss/bgp/cpp/common/BgpError.h"
#include "neteng/fboss/bgp/cpp/sim/BgpPeer.h"

namespace facebook::bgp {

namespace {

// Build a minimal valid peer with the non-optional address fields populated.
thrift::BgpPeer makeMinimalPeer() {
  thrift::BgpPeer peer;
  peer.peer_addr() = "10.0.0.1";
  peer.local_addr() = "10.0.0.2";
  peer.next_hop4() = "10.0.0.2";
  peer.next_hop6() = "::1";
  return peer;
}

} // namespace

class BgpPeerTest : public ::testing::Test {};

/*
 * A fully-specified peer should surface every field through its accessors,
 * with no remote switch linked initially.
 */
TEST_F(BgpPeerTest, FullySpecifiedPeer) {
  thrift::BgpPeer peer;
  peer.peer_addr() = "10.0.0.1";
  peer.local_addr() = "10.0.0.2";
  peer.next_hop4() = "1.1.1.1";
  peer.next_hop6() = "2001:db8::1";
  peer.remote_as_4_byte() = 65001;
  peer.local_as_4_byte() = 65002;
  peer.ingress_policy_name() = "ingress_pol";
  peer.egress_policy_name() = "egress_pol";
  peer.description() = "csw01.frc1";
  peer.peer_group_name() = "group_a";
  peer.is_passive() = true;
  peer.is_rr_client() = true;
  peer.is_confed_peer() = false;
  peer.next_hop_self() = true;
  peer.disable_ipv4_afi() = false;
  peer.disable_ipv6_afi() = true;

  BgpPeer p(peer);

  EXPECT_EQ(folly::IPAddress("10.0.0.1"), p.peerIp());
  EXPECT_EQ(folly::IPAddress("10.0.0.2"), p.localIp());
  EXPECT_EQ(65001u, p.remoteAsn());
  EXPECT_EQ(65002u, p.localAsn());
  EXPECT_EQ("1.1.1.1", p.nextHop4());
  EXPECT_EQ("2001:db8::1", p.nextHop6());
  EXPECT_EQ("ingress_pol", p.ingressPolicyName());
  EXPECT_EQ("egress_pol", p.egressPolicyName());
  EXPECT_EQ("csw01.frc1", p.description());
  EXPECT_EQ("group_a", p.peerGroupName());
  EXPECT_TRUE(p.isPassive());
  EXPECT_TRUE(p.isRrClient());
  EXPECT_FALSE(p.isConfedPeer());
  EXPECT_TRUE(p.nextHopSelf());
  EXPECT_FALSE(p.disableIpv4Afi());
  EXPECT_TRUE(p.disableIpv6Afi());
  EXPECT_FALSE(p.isLinked());
  EXPECT_EQ(nullptr, p.getRemoteSwitch());
  // local 65002 != remote 65001 -> EXTERNAL
  EXPECT_EQ(PeerType::EXTERNAL, p.peerType());
}

// A passive peer's subnet-style peer_addr should be parsed as the address.
TEST_F(BgpPeerTest, PassivePeerSubnetAddrIsStripped) {
  thrift::BgpPeer peer = makeMinimalPeer();
  peer.peer_addr() = "10.20.0.0/24";

  BgpPeer p(peer);

  EXPECT_EQ(folly::IPAddress("10.20.0.0"), p.peerIp());
}

/*
 * Cross-width resolution for local AS: the peer sets only the 2-byte local_as
 * while the group sets the 4-byte local_as_4_byte. Production resolves each
 * width across peer > group first, then prefers the resolved 4-byte over the
 * resolved 2-byte (net order peer4 > group4 > peer2 > group2). The group's
 * 4-byte value (200000) therefore wins over the peer's 2-byte value, matching
 * production Config.cpp:591-608.
 */
TEST_F(BgpPeerTest, CrossWidthGroupFourByteWinsOverPeerTwoByte) {
  thrift::BgpPeer peer = makeMinimalPeer();
  peer.local_as() = 100;

  thrift::PeerGroup group;
  group.name() = "g";
  group.local_as_4_byte() = 200000;

  BgpPeer p(peer, &group);

  EXPECT_EQ(200000u, p.localAsn());
}

/*
 * Local AS prefers the 4-byte width over the deprecated 2-byte width. The peer
 * sets both local widths plus a single remote 4-byte; local resolution picks
 * 80000 (4-byte) and logs an ERR for the both-set case (non-fatal), matching
 * production Config.cpp:597-608.
 */
TEST_F(BgpPeerTest, FourBytePreferredOverTwoByteLocal) {
  thrift::BgpPeer peer = makeMinimalPeer();
  peer.remote_as_4_byte() = 70000;
  peer.local_as() = 200;
  peer.local_as_4_byte() = 80000;

  BgpPeer p(peer);

  EXPECT_EQ(70000u, p.remoteAsn());
  EXPECT_EQ(80000u, p.localAsn());
}

/*
 * Remote AS is peer-only and rejects having both widths set: production throws
 * BgpError rather than silently picking one (Config.cpp:613-618).
 */
TEST_F(BgpPeerTest, RemoteAsBothSetThrows) {
  thrift::BgpPeer peer = makeMinimalPeer();
  peer.remote_as() = 100;
  peer.remote_as_4_byte() = 70000;

  EXPECT_THROW(BgpPeer p(peer), facebook::bgp::BgpError);
}

// 2-byte ASN is used when no 4-byte field is present.
TEST_F(BgpPeerTest, TwoByteAsnWhenNoFourByte) {
  thrift::BgpPeer peer = makeMinimalPeer();
  peer.remote_as() = 100;
  peer.local_as() = 200;

  BgpPeer p(peer);

  EXPECT_EQ(100u, p.remoteAsn());
  EXPECT_EQ(200u, p.localAsn());
}

// When the peer omits fields, values fall back to the peer group.
TEST_F(BgpPeerTest, PeerGroupFallback) {
  thrift::BgpPeer peer = makeMinimalPeer();

  thrift::PeerGroup group;
  group.name() = "g";
  group.remote_as_4_byte() = 65010;
  group.local_as_4_byte() = 65020;
  group.ingress_policy_name() = "group_ingress";
  group.egress_policy_name() = "group_egress";
  group.is_passive() = true;
  group.is_rr_client() = true;

  BgpPeer p(peer, &group);

  /*
   * Regression guard: remote AS is peer-only and is NOT inherited from the
   * peer group, even though the group sets remote_as_4_byte. This matches
   * production Config.cpp:609-611 (group remote_as* is render-only).
   */
  EXPECT_EQ(0u, p.remoteAsn());
  // local_as IS inherited from the peer group (peer > group precedence).
  EXPECT_EQ(65020u, p.localAsn());
  EXPECT_EQ("group_ingress", p.ingressPolicyName());
  EXPECT_EQ("group_egress", p.egressPolicyName());
  EXPECT_TRUE(p.isPassive());
  EXPECT_TRUE(p.isRrClient());
}

// When both peer and group set a field, the peer value wins.
TEST_F(BgpPeerTest, PeerOverridesGroup) {
  thrift::BgpPeer peer = makeMinimalPeer();
  peer.remote_as_4_byte() = 65001;
  peer.local_as_4_byte() = 65002;
  peer.ingress_policy_name() = "peer_ingress";
  peer.is_passive() = false;

  thrift::PeerGroup group;
  group.name() = "g";
  group.remote_as_4_byte() = 65010;
  group.local_as_4_byte() = 65020;
  group.ingress_policy_name() = "group_ingress";
  group.is_passive() = true;

  BgpPeer p(peer, &group);

  EXPECT_EQ(65001u, p.remoteAsn());
  // Peer local_as wins over the group's local_as.
  EXPECT_EQ(65002u, p.localAsn());
  EXPECT_EQ("peer_ingress", p.ingressPolicyName());
  EXPECT_FALSE(p.isPassive());
}

// Equal local/remote AS -> INTERNAL (iBGP).
TEST_F(BgpPeerTest, PeerTypeInternal) {
  thrift::BgpPeer peer = makeMinimalPeer();
  peer.remote_as_4_byte() = 65000;
  peer.local_as_4_byte() = 65000;

  BgpPeer p(peer);

  EXPECT_EQ(PeerType::INTERNAL, p.peerType());
}

// Different local/remote AS -> EXTERNAL (eBGP).
TEST_F(BgpPeerTest, PeerTypeExternal) {
  thrift::BgpPeer peer = makeMinimalPeer();
  peer.remote_as_4_byte() = 65000;
  peer.local_as_4_byte() = 65001;

  BgpPeer p(peer);

  EXPECT_EQ(PeerType::EXTERNAL, p.peerType());
}

// A confed peer with differing AS -> CONFED_EXTERNAL (production ConfedEBGP).
TEST_F(BgpPeerTest, PeerTypeConfedExternal) {
  thrift::BgpPeer peer = makeMinimalPeer();
  peer.remote_as_4_byte() = 65001;
  peer.local_as_4_byte() = 65000;
  peer.is_confed_peer() = true;

  BgpPeer p(peer);

  EXPECT_EQ(PeerType::CONFED_EXTERNAL, p.peerType());
}

/*
 * A confed peer that shares the local sub-AS is INTERNAL, not CONFED_EXTERNAL
 * (mirrors production treating intra-confed sessions as internal). Regression
 * guard for the equal-AS-before-confed classification ordering.
 */
TEST_F(BgpPeerTest, PeerTypeConfedIntraIsInternal) {
  thrift::BgpPeer peer = makeMinimalPeer();
  peer.remote_as_4_byte() = 65000;
  peer.local_as_4_byte() = 65000;
  peer.is_confed_peer() = true;

  BgpPeer p(peer);

  EXPECT_EQ(PeerType::INTERNAL, p.peerType());
}

// setLocalAsn updates the local AS and re-derives the peer type.
TEST_F(BgpPeerTest, SetLocalAsnRecomputesPeerType) {
  thrift::BgpPeer peer = makeMinimalPeer();
  peer.remote_as_4_byte() = 65000;

  BgpPeer p(peer);
  // No local AS configured -> 0, so 0 != 65000 -> EXTERNAL.
  EXPECT_EQ(0u, p.localAsn());
  EXPECT_EQ(PeerType::EXTERNAL, p.peerType());

  p.setLocalAsn(65000);

  EXPECT_EQ(65000u, p.localAsn());
  EXPECT_EQ(PeerType::INTERNAL, p.peerType());
}

// addReceivedRoute appends and receivedRoutes returns the stored entries.
TEST_F(BgpPeerTest, ReceivedRoutesRoundTrip) {
  thrift::BgpPeer peer = makeMinimalPeer();
  BgpPeer p(peer);
  EXPECT_TRUE(p.receivedRoutes().empty());

  const folly::CIDRNetwork cidr{folly::IPAddress("10.1.0.0"), 24};
  p.addReceivedRoute(ReceivedRoute{cidr, "switch_b", /*path=*/nullptr});

  ASSERT_EQ(1u, p.receivedRoutes().size());
  EXPECT_EQ("switch_b", p.receivedRoutes().front().fromSwitch);
  EXPECT_EQ(cidr, p.receivedRoutes().front().cidr);
}

/*
 * setLocalAsn fills the effective local ASN from the global switch ASN when no
 * per-peer override is configured.
 */
TEST_F(BgpPeerTest, SetLocalAsnFillsGlobalWhenNoOverride) {
  thrift::BgpPeer peer = makeMinimalPeer();
  BgpPeer p(peer);
  ASSERT_FALSE(p.localAsOverride().has_value());

  p.setLocalAsn(65000);

  EXPECT_EQ(65000u, p.localAsn());
}

// A per-peer local-AS override is preserved over the global switch ASN.
TEST_F(BgpPeerTest, SetLocalAsnPreservesOverride) {
  thrift::BgpPeer peer = makeMinimalPeer();
  peer.local_as_4_byte() = 65002;
  peer.remote_as_4_byte() = 65001;
  BgpPeer p(peer);

  p.setLocalAsn(64000);

  EXPECT_EQ(65002u, p.localAsn());
}

/*
 * override == global-AS logs an ERR (non-fatal) and keeps the override as the
 * effective local ASN (Config.cpp:621-622).
 */
TEST_F(BgpPeerTest, SetLocalAsnOverrideEqualsGlobalLogsErr) {
  thrift::BgpPeer peer = makeMinimalPeer();
  peer.local_as_4_byte() = 65000;
  peer.remote_as_4_byte() = 65001;
  BgpPeer p(peer);

  EXPECT_NO_THROW(p.setLocalAsn(65000));
  EXPECT_EQ(65000u, p.localAsn());
}

// override == remote-AS throws BgpError (Config.cpp:623-625).
TEST_F(BgpPeerTest, SetLocalAsnOverrideEqualsRemoteThrows) {
  thrift::BgpPeer peer = makeMinimalPeer();
  peer.local_as_4_byte() = 65001;
  peer.remote_as_4_byte() = 65001;
  BgpPeer p(peer);

  EXPECT_THROW(p.setLocalAsn(64000), facebook::bgp::BgpError);
}

/*
 * override set with remote-AS == global-AS throws (Peer-Local-AS configured for
 * a non-EBGP peer, Config.cpp:626-627).
 */
TEST_F(BgpPeerTest, SetLocalAsnRemoteEqualsGlobalThrows) {
  thrift::BgpPeer peer = makeMinimalPeer();
  peer.local_as_4_byte() = 65002;
  peer.remote_as_4_byte() = 64000;
  BgpPeer p(peer);

  EXPECT_THROW(p.setLocalAsn(64000), facebook::bgp::BgpError);
}

/*
 * addReceivedRoute replaces the existing entry for the same prefix rather than
 * appending, so re-advertisements keep receivedRoutes_ bounded. The Adj-RIB-In
 * is per-session and keyed by prefix alone (mirrors production), so a peer has
 * exactly one entry per prefix.
 */
TEST_F(BgpPeerTest, ReceivedRoutesDedupByPrefix) {
  thrift::BgpPeer peer = makeMinimalPeer();
  BgpPeer p(peer);

  const folly::CIDRNetwork cidrA{folly::IPAddress("10.1.0.0"), 24};
  const folly::CIDRNetwork cidrB{folly::IPAddress("10.2.0.0"), 24};
  p.addReceivedRoute(ReceivedRoute{cidrA, "switch_b", /*path=*/nullptr});
  // Re-advertising the same prefix updates in place.
  p.addReceivedRoute(ReceivedRoute{cidrA, "switch_b", /*path=*/nullptr});
  EXPECT_EQ(1u, p.receivedRoutes().size());

  // A different prefix is a distinct entry.
  p.addReceivedRoute(ReceivedRoute{cidrB, "switch_b", /*path=*/nullptr});
  EXPECT_EQ(2u, p.receivedRoutes().size());
}

} // namespace facebook::bgp
