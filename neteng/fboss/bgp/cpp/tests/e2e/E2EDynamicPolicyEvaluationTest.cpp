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

/**
 * E2EDynamicPolicyEvaluationTest.cpp
 *
 * E2E tests for runtime dynamic policy evaluation APIs:
 * - setPeersPolicy
 * - setPeerGroupsPolicy
 * - unsetPeersPolicy
 *
 * These tests verify the complete drain workflow through the real BGP pipeline.
 * Routes flow through AdjRibIn → RIB → AdjRibOut and we verify the effect of
 * runtime policy changes on route propagation.
 *
 * Key difference from existing E2E tests: Existing tests set policies at setup
 * time (before createRib). This test changes policies at runtime via
 * BgpServiceBB thrift APIs after peers are established and routes are
 * flowing.
 *
 * Mocked: FIB (TestFib), SessionManager (MockSessionManager)
 * Real: RIB, PeerManager, AdjRib, PolicyManager, BgpServiceBB
 */

#include <gtest/gtest.h>

#include <folly/coro/BlockingWait.h>
#include <folly/logging/xlog.h>

#include "fboss/lib/CommonUtils.h"
#include "neteng/fboss/bgp/cpp/config/ConfigManager.h"
#include "neteng/fboss/bgp/cpp/facebook/BgpServiceBB.h"
#include "neteng/fboss/bgp/cpp/tests/PolicyUtils.h"
#include "neteng/fboss/bgp/cpp/tests/Utils.h"
#include "neteng/fboss/bgp/cpp/tests/e2e/E2ETestFixture.h"

using namespace facebook::bgp;

namespace facebook::bgp {

namespace {

constexpr auto kDrainPolicyName = "DRAIN_POLICY";
constexpr auto kUndrainPolicyName = "UNDRAIN_POLICY";

} // namespace

/**
 * E2EDynamicPolicyEvaluationTest
 *
 * Test fixture for runtime dynamic policy evaluation APIs.
 * Configures peers with drain/undrain policies, brings up sessions, injects
 * routes, then exercises the drain APIs and verifies route behavior.
 */
class E2EDynamicPolicyEvaluationTest : public E2ETestFixture {
 protected:
  void SetUp() override {
    setupDrainPolicies();

    addPeer(kDefaultPeerSpec3);
    addPeer(kDefaultPeerSpec4);
    addPeer(kDefaultPeerSpec5);

    createRib();
    createPeerManagerWithBgpService();
  }

  /**
   * Set up drain (DENY) and undrain (ACCEPT) policies in PolicyManager.
   * These policies will be used for runtime drain/undrain operations.
   */
  void setupDrainPolicies() {
    auto drainStatement = createBgpPolicyStatement(
        kDrainPolicyName,
        {createBgpPolicyTerm(
            "deny-all",
            "Deny all routes (drain)",
            {},
            {createBgpPolicyAction(bgp_policy::BgpPolicyActionType::DENY)},
            bgp_policy::FlowControlAction::NEXT_TERM)});

    auto undrainStatement = createBgpPolicyStatement(
        kUndrainPolicyName,
        {createBgpPolicyTerm(
            "accept-all",
            "Accept all routes (undrain)",
            {},
            {createBgpPolicyAction(bgp_policy::BgpPolicyActionType::PERMIT)},
            bgp_policy::FlowControlAction::NEXT_TERM)});

    bgp_policy::BgpPolicies policies;
    policies.bgp_policy_statements()->emplace_back(drainStatement);
    policies.bgp_policy_statements()->emplace_back(undrainStatement);

    setPolicyConfig(policies);
  }

  /**
   * Create PeerManager and BgpServiceBB instance for runtime API calls.
   * This extends the base createPeerManager to also create the service layer.
   * Both PeerManager and BgpServiceBB share the same ConfigManager so that
   * config version tracking is consistent across policy updates.
   */
  void createPeerManagerWithBgpService() {
    createPeerManager(/*enableUpdateGroup=*/false,
                      /*enableEgressBackpressure=*/true);

    watchdog_ = std::make_unique<Watchdog>(config_);
    bgpService_ = std::make_unique<BgpServiceBB>(
        *peerManager_,
        configManager_,
        *rib_,
        *watchdog_,
        /*nlWrapper=*/nullptr,
        false);
  }

  /**
   * Bring up all 3 peers and complete EoR exchange.
   */
  void bringUpAllPeersWithEor() {
    bringUpPeer(kPeerAddr3);
    bringUpPeer(kPeerAddr4);
    bringUpPeer(kPeerAddr5);

    BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
    BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};
    BgpPeerId peerId5{kPeerAddr5, kPeerAddr5.asV4().toLongHBO()};

    sendEoRToPeer(peerId3);
    sendEoRToPeer(peerId4);
    sendEoRToPeer(peerId5);

    EXPECT_TRUE(waitForEoR(peerId3));
    EXPECT_TRUE(waitForEoR(peerId4));
    EXPECT_TRUE(waitForEoR(peerId5));
  }

  /**
   * Inject a test route from peer3 and verify it's propagated.
   */
  void injectAndVerifyTestRoute() {
    addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001");

    auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");
    EXPECT_TRUE(waitForRouteInShadowRib(prefix));
    EXPECT_TRUE(
        verifyRouteAdd("v4", "10.0.0.0", 8, kPeerAddr5, kNextHopV4_5.str()));
  }

  /**
   * Helper to call setPeersPolicy via BgpServiceBB
   */
  neteng::fboss::bgp::thrift::BgpPolicyChangeResult setPeerPolicy(
      const std::string& peerAddr,
      const std::string& policyName,
      bgp_policy::DIRECTION direction = bgp_policy::DIRECTION::OUT) {
    std::map<std::string, std::map<bgp_policy::DIRECTION, std::string>>
        peersPolicy;
    peersPolicy[peerAddr][direction] = policyName;

    return folly::coro::blockingWait(bgpService_->co_setPeersPolicy(
        std::make_unique<decltype(peersPolicy)>(std::move(peersPolicy))));
  }

  /**
   * Helper to call unsetPeersPolicy via BgpServiceBB
   */
  neteng::fboss::bgp::thrift::BgpPolicyChangeResult unsetPeerPolicy(
      const std::string& peerAddr,
      bgp_policy::DIRECTION direction = bgp_policy::DIRECTION::OUT) {
    std::map<std::string, std::set<bgp_policy::DIRECTION>> peersToUnset;
    peersToUnset[peerAddr].insert(direction);

    return folly::coro::blockingWait(bgpService_->co_unsetPeersPolicy(
        std::make_unique<decltype(peersToUnset)>(std::move(peersToUnset))));
  }

  std::unique_ptr<Watchdog> watchdog_;
  std::unique_ptr<BgpServiceBB> bgpService_;
};

/**
 * Test: SetPeersPolicyRejectsBeforeInitialization
 *
 * Verify that setPeersPolicy rejects the API call when BGP is not
 * initialized (before peers are brought up and EoR is exchanged).
 */
TEST_F(
    E2EDynamicPolicyEvaluationTest,
    SetPeersPolicyRejectsBeforeInitialization) {
  auto result = setPeerPolicy(kPeerAddr5.str(), kDrainPolicyName);
  EXPECT_EQ(
      result, neteng::fboss::bgp::thrift::BgpPolicyChangeResult::INPUT_ERROR);
}

/**
 * Test: SetPeerGroupsPolicyRejectsBeforeInitialization
 *
 * Verify that setPeerGroupsPolicy rejects the API call when BGP is not
 * initialized (before peers are brought up and EoR is exchanged).
 */
TEST_F(
    E2EDynamicPolicyEvaluationTest,
    SetPeerGroupsPolicyRejectsBeforeInitialization) {
  std::map<std::string, std::map<bgp_policy::DIRECTION, std::string>>
      peerGroupsPolicy;
  peerGroupsPolicy["test-peer-group-1"][bgp_policy::DIRECTION::OUT] =
      kDrainPolicyName;

  auto result = folly::coro::blockingWait(bgpService_->co_setPeerGroupsPolicy(
      std::make_unique<decltype(peerGroupsPolicy)>(
          std::move(peerGroupsPolicy))));
  EXPECT_EQ(
      result, neteng::fboss::bgp::thrift::BgpPolicyChangeResult::INPUT_ERROR);
}

/**
 * Test: SetPeersPolicyDrainsRoutes
 *
 * Verify that applying a DENY policy to a peer via setPeersPolicy causes
 * routes to be withdrawn from that peer via policy re-evaluation.
 *
 * Sequence:
 * T0: Inject routes from peer3, verify propagation to peer5
 * T1: setPeersPolicy(peer5, DRAIN_POLICY) - apply DENY egress policy
 * T2: Verify routes are withdrawn from peer5
 */
TEST_F(E2EDynamicPolicyEvaluationTest, SetPeersPolicyDrainsRoutes) {
  bringUpAllPeersWithEor();

  injectAndVerifyTestRoute();

  auto result = setPeerPolicy(kPeerAddr5.str(), kDrainPolicyName);
  EXPECT_EQ(
      result,
      neteng::fboss::bgp::thrift::BgpPolicyChangeResult::POLICIES_APPLIED);

  EXPECT_TRUE(verifyRouteWithdraw("v4", "10.0.0.0", 8, kPeerAddr5));
}

/**
 * Test: UnsetPeersPolicyRestoresRoutes
 *
 * Verify that unsetting a peer-level policy causes routes to be restored
 * when the peer was previously drained.
 *
 * Sequence:
 * T0: Inject routes, verify propagation
 * T1: setPeersPolicy(peer5, DRAIN_POLICY) - drain peer5 at peer level
 * T2: Verify peer5 routes withdrawn
 * T3: unsetPeersPolicy(peer5) - remove peer-level policy
 * T4: Verify peer5 routes restored
 */
TEST_F(E2EDynamicPolicyEvaluationTest, UnsetPeersPolicyRestoresRoutes) {
  bringUpAllPeersWithEor();

  injectAndVerifyTestRoute();

  auto result1 = setPeerPolicy(kPeerAddr5.str(), kDrainPolicyName);
  EXPECT_EQ(
      result1,
      neteng::fboss::bgp::thrift::BgpPolicyChangeResult::POLICIES_APPLIED);
  EXPECT_TRUE(verifyRouteWithdraw("v4", "10.0.0.0", 8, kPeerAddr5));

  auto result2 = unsetPeerPolicy(kPeerAddr5.str());
  EXPECT_EQ(
      result2,
      neteng::fboss::bgp::thrift::BgpPolicyChangeResult::POLICIES_APPLIED);

  EXPECT_TRUE(
      verifyRouteAdd("v4", "10.0.0.0", 8, kPeerAddr5, kNextHopV4_5.str()));
}

/**
 * Test: DrainUndrainCyclePreservesRoutes
 *
 * Verify the complete drain/undrain cycle works correctly.
 * Routes should be withdrawn on drain and restored on undrain.
 *
 * Sequence:
 * T0: Inject routes, verify propagation
 * T1: Drain peer5 via setPeersPolicy
 * T2: Verify routes withdrawn
 * T3: Undrain peer5 via setPeersPolicy (change to ACCEPT policy)
 * T4: Verify routes restored
 */
TEST_F(E2EDynamicPolicyEvaluationTest, DrainUndrainCyclePreservesRoutes) {
  bringUpAllPeersWithEor();

  injectAndVerifyTestRoute();

  auto result1 = setPeerPolicy(kPeerAddr5.str(), kDrainPolicyName);
  EXPECT_EQ(
      result1,
      neteng::fboss::bgp::thrift::BgpPolicyChangeResult::POLICIES_APPLIED);
  EXPECT_TRUE(verifyRouteWithdraw("v4", "10.0.0.0", 8, kPeerAddr5));

  auto result2 = setPeerPolicy(kPeerAddr5.str(), kUndrainPolicyName);
  EXPECT_EQ(
      result2,
      neteng::fboss::bgp::thrift::BgpPolicyChangeResult::POLICIES_APPLIED);

  EXPECT_TRUE(
      verifyRouteAdd("v4", "10.0.0.0", 8, kPeerAddr5, kNextHopV4_5.str()));
}

/**
 * Test: MultiplePeersDrainIndependently
 *
 * Verify that draining one peer does not affect other peers.
 * Each peer should be drained/undrained independently.
 *
 * Sequence:
 * T0: Inject routes, verify propagation to peer4 and peer5
 * T1: Drain peer5 only
 * T2: Verify peer5 routes withdrawn, peer4 still receiving routes
 */
TEST_F(E2EDynamicPolicyEvaluationTest, MultiplePeersDrainIndependently) {
  bringUpAllPeersWithEor();

  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001");
  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));

  EXPECT_TRUE(
      verifyRouteAdd("v4", "10.0.0.0", 8, kPeerAddr4, kNextHopV4_4.str()));
  EXPECT_TRUE(
      verifyRouteAdd("v4", "10.0.0.0", 8, kPeerAddr5, kNextHopV4_5.str()));

  auto result = setPeerPolicy(kPeerAddr5.str(), kDrainPolicyName);
  EXPECT_EQ(
      result,
      neteng::fboss::bgp::thrift::BgpPolicyChangeResult::POLICIES_APPLIED);

  EXPECT_TRUE(verifyRouteWithdraw("v4", "10.0.0.0", 8, kPeerAddr5));

  addRoute("v4", "20.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001");
  auto prefix2 = folly::IPAddress::createNetwork("20.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix2));

  EXPECT_TRUE(
      verifyRouteAdd("v4", "20.0.0.0", 8, kPeerAddr4, kNextHopV4_4.str()));
}

} // namespace facebook::bgp
