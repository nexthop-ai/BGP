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

#include "neteng/fboss/bgp/cpp/tests/e2e/E2ETestFixture.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <folly/coro/BlockingWait.h>

#include "fboss/agent/AddressUtil.h"
#include "neteng/fboss/bgp/cpp/lib/fibers/BgpSerializer.h"
#include "neteng/fboss/bgp/cpp/lib/fibers/FiberBgpPeer.h"
#include "neteng/fboss/bgp/cpp/tests/AdjRibInUtils.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {
namespace {

constexpr int kRouteCount = 1200;
constexpr int kMaxDescriptorReads = 2048;
constexpr int kMaxEmptyFlushes = 50;

/*
 * Byte-level packing constants for the update-group serialization path.
 *
 * For V4 announcements the update group always wraps prefixes in
 * MP_REACH_NLRI. The per-message NLRI packer (BgpMessageSerializer::
 * serializeNlri) inspects the worst-case prefix length passed in by the
 * caller; for the mpAnnounced path that is `kMaxV6PrefixLen` (17 bytes)
 * because the V6-aware getMaxPrefixLen() is called with `v4=false`. The
 * packer's stop condition is `nlri_used + (maxPrefixLen + 1) >= bufSize`,
 * so the effective per-message reserve is 18 bytes regardless of whether
 * the prefixes themselves are V4 or V6.
 *
 * Per-prefix NLRI wire size:
 *   - V4 /24: 1 length byte + 3 prefix bytes = 4 bytes
 *   - V4 /32: 1 length byte + 4 prefix bytes = 5 bytes
 */
constexpr size_t kMpV6BasedPackerReserve =
    kMaxV6PrefixLen + sizeof(uint8_t); // 18
constexpr size_t kV4Slash24NlriBytes = 4;
constexpr size_t kV4Slash32NlriBytes = 5;

constexpr int kMaxPackingV4Slash24Count = 4000;
constexpr int kMaxPackingV4Slash32Count = 3000;
constexpr int kSmallTailV4Slash24Count = 1010;

BgpPeerId makePeerId(const folly::IPAddress& peerAddr) {
  return BgpPeerId{peerAddr, peerAddr.asV4().toLongHBO()};
}

std::shared_ptr<const BgpUpdate2> makeMultiPrefixUpdate() {
  auto update = createBgpUpdateAnnouncement(
      /*isV4=*/true,
      folly::CIDRNetwork{folly::IPAddress("44.0.0.0"), 24},
      "10.44.0.1",
      {64512, 64513},
      {"4400:1"},
      /*addPathId=*/0,
      /*localPref=*/150,
      /*med=*/25);

  for (int i = 1; i < kRouteCount; ++i) {
    RiggedIPPrefix prefix;
    prefix.prefix() = facebook::network::toIPPrefix(
        folly::CIDRNetwork{
            folly::IPAddress(
                "44." + std::to_string(i / 256) + "." +
                std::to_string(i % 256) + ".0"),
            24});
    update->v4Announced2()->push_back(std::move(prefix));
  }

  return update;
}

folly::CIDRNetwork lastRoutePrefix() {
  const int lastIndex = kRouteCount - 1;
  return folly::CIDRNetwork{
      folly::IPAddress(
          "44." + std::to_string(lastIndex / 256) + "." +
          std::to_string(lastIndex % 256) + ".0"),
      24};
}

/*
 * Build a single BgpUpdate2 announcing `count` V4 /24 prefixes that all share
 * the same path attributes. Prefixes count up from baseSecondOctet.0.0.0/24
 * so different tests can pick non-overlapping address ranges.
 *
 * Identical attributes are the precondition for maximum packing: every
 * prefix lands in the same update group and the group serializer is free to
 * pack them into the fewest possible UPDATE messages.
 */
std::shared_ptr<const BgpUpdate2> makeIdenticalAttrV4Slash24Update(
    int count,
    int baseSecondOctet,
    const std::string& nexthop,
    const std::string& communityTag) {
  auto update = createBgpUpdateAnnouncement(
      /*isV4=*/true,
      folly::CIDRNetwork{
          folly::IPAddress(std::to_string(baseSecondOctet) + ".0.0.0"), 24},
      nexthop,
      {64512, 64513},
      {communityTag},
      /*addPathId=*/0,
      /*localPref=*/150,
      /*med=*/25);

  for (int i = 1; i < count; ++i) {
    RiggedIPPrefix prefix;
    prefix.prefix() = facebook::network::toIPPrefix(
        folly::CIDRNetwork{
            folly::IPAddress(
                std::to_string(baseSecondOctet) + "." +
                std::to_string(i / 256) + "." + std::to_string(i % 256) + ".0"),
            24});
    update->v4Announced2()->push_back(std::move(prefix));
  }
  return update;
}

folly::CIDRNetwork v4Slash24PrefixAt(int baseSecondOctet, int idx) {
  return folly::CIDRNetwork{
      folly::IPAddress(
          std::to_string(baseSecondOctet) + "." + std::to_string(idx / 256) +
          "." + std::to_string(idx % 256) + ".0"),
      24};
}

/*
 * Build a single BgpUpdate2 announcing `count` V4 /32 prefixes that all share
 * the same path attributes. Hosts count up from baseSecondOctet.0.0.0/32.
 */
std::shared_ptr<const BgpUpdate2> makeIdenticalAttrV4Slash32Update(
    int count,
    int baseSecondOctet,
    const std::string& nexthop,
    const std::string& communityTag) {
  auto firstAddr = std::to_string(baseSecondOctet) + ".0.0.0";
  auto update = createBgpUpdateAnnouncement(
      /*isV4=*/true,
      folly::CIDRNetwork{folly::IPAddress(firstAddr), 32},
      nexthop,
      {64512, 64513},
      {communityTag},
      /*addPathId=*/0,
      /*localPref=*/150,
      /*med=*/25);

  for (int i = 1; i < count; ++i) {
    RiggedIPPrefix prefix;
    prefix.prefix() = facebook::network::toIPPrefix(
        folly::CIDRNetwork{
            folly::IPAddress(
                std::to_string(baseSecondOctet) + "." +
                std::to_string((i >> 16) & 0xff) + "." +
                std::to_string((i >> 8) & 0xff) + "." +
                std::to_string(i & 0xff)),
            32});
    update->v4Announced2()->push_back(std::move(prefix));
  }
  return update;
}

folly::CIDRNetwork v4Slash32PrefixAt(int baseSecondOctet, int idx) {
  return folly::CIDRNetwork{
      folly::IPAddress(
          std::to_string(baseSecondOctet) + "." +
          std::to_string((idx >> 16) & 0xff) + "." +
          std::to_string((idx >> 8) & 0xff) + "." + std::to_string(idx & 0xff)),
      32};
}

BgpCapabilities makeDefaultCaps() {
  BgpCapabilities caps;
  caps.as4byte() = true;
  caps.mpExtV4Unicast() = true;
  caps.mpExtV6Unicast() = true;
  return caps;
}

/*
 * Byte-precise validator for the "maximum packing" property of a serialized
 * UpdateDescriptor IOBuf chain. The validator is parameterized by the
 * per-prefix wire size and the per-message NLRI reserve the packer uses, so
 * the same routine applies to V4 /24, V4 /32, and (in principle) V6 paths.
 *
 * The validator does NOT need to know the per-message attribute length up
 * front. It derives the per-message header+attrs overhead from the chain
 * data itself, then proves byte-for-byte that:
 *
 *   1. Every chain element respects the BGP wire limit (kMaxBgpMsgLen).
 *   2. Every non-tail element has the SAME byte length (the packer's
 *      per-message NLRI capacity is identical across iterations, so uniform
 *      packing is the only correct outcome).
 *   3. The derived pre-NLRI overhead is integer-consistent across the chain
 *      (a non-integer derivation would mean the chain has internal byte-
 *      accounting bugs).
 *   4. NLRI bytes in every element are exact multiples of `prefixWireBytes`.
 *   5. The total prefix count derived from the chain matches the expected
 *      total exactly.
 *   6. Per-message slack lies in the narrow window
 *      [reserve - prefixWireBytes + 1, reserve]. Slack above `reserve` would
 *      mean the packer could have fit another prefix; slack below
 *      `reserve - prefixWireBytes + 1` would mean the packer stopped before
 *      its own stop condition fired. Either is a packing bug.
 *   7. The tail element holds at least 1 prefix and at most `nFull` prefixes
 *      (otherwise the chain could have been one element shorter or the
 *      non-tail packing was sub-optimal).
 *   8. The chain length equals the minimum number of UPDATEs required to
 *      hold `totalPrefixes` at `nFull` prefixes per full UPDATE.
 */
void verifyMaxPackingChain(
    const folly::IOBuf* head,
    size_t totalPrefixes,
    size_t prefixWireBytes,
    size_t packerReserveBytes,
    folly::StringPiece caseLabel) {
  SCOPED_TRACE(caseLabel);
  ASSERT_NE(nullptr, head);
  ASSERT_GT(totalPrefixes, 0u);
  ASSERT_GT(prefixWireBytes, 0u);
  ASSERT_GE(packerReserveBytes, prefixWireBytes);

  std::vector<size_t> elementLengths;
  const auto* current = head;
  do {
    elementLengths.push_back(current->length());
    current = current->next();
  } while (current != head);

  const size_t chainCount = elementLengths.size();
  ASSERT_GE(chainCount, 2u)
      << "Maximum-packing test expects a multi-element chain; got "
      << chainCount;

  // Invariant 1: every element fits within the BGP wire limit.
  for (size_t i = 0; i < chainCount; ++i) {
    EXPECT_LE(elementLengths[i], kMaxBgpMsgLen)
        << "Element " << i << " exceeds BGP wire limit";
  }

  // Invariant 2: all non-tail elements must have identical length.
  const size_t fullLength = elementLengths[0];
  for (size_t i = 0; i + 1 < chainCount; ++i) {
    ASSERT_EQ(fullLength, elementLengths[i])
        << "Non-tail chain element " << i
        << " differs from element 0; serializer should pack each non-tail "
        << "UPDATE to the same byte count under identical attributes";
  }
  const size_t tailLength = elementLengths.back();

  /*
   * Invariant 3: derive per-message overhead (preNLRI) from the data.
   *
   *   fullLength = preNLRI + nFull * prefixWireBytes
   *   tailLength = preNLRI + nTail * prefixWireBytes
   *   nFull * (chainCount - 1) + nTail = totalPrefixes
   *
   * => preNLRI = (fullLength * (C-1) + tailLength - totalPrefixes * ps) / C
   *
   * The right-hand side MUST be an exact integer; otherwise the chain layout
   * is internally inconsistent.
   */
  const int64_t totalNLRI = static_cast<int64_t>(totalPrefixes) *
      static_cast<int64_t>(prefixWireBytes);
  const int64_t sumLengths =
      static_cast<int64_t>(fullLength) * static_cast<int64_t>(chainCount - 1) +
      static_cast<int64_t>(tailLength);
  ASSERT_GE(sumLengths, totalNLRI)
      << "Sum of element lengths (" << sumLengths
      << ") is less than total NLRI bytes (" << totalNLRI
      << "); chain layout violates basic byte accounting";
  ASSERT_EQ(0, (sumLengths - totalNLRI) % static_cast<int64_t>(chainCount))
      << "Derived pre-NLRI overhead is not an integer; chain layout is "
      << "internally inconsistent";
  const size_t preNLRI =
      static_cast<size_t>((sumLengths - totalNLRI) / chainCount);

  // Invariant 4: per-element NLRI byte counts are exact multiples of prefix
  // size.
  ASSERT_GT(fullLength, preNLRI);
  ASSERT_GE(tailLength, preNLRI);
  ASSERT_EQ(0u, (fullLength - preNLRI) % prefixWireBytes)
      << "Full-element NLRI bytes (" << (fullLength - preNLRI)
      << ") are not a multiple of prefix wire size " << prefixWireBytes;
  ASSERT_EQ(0u, (tailLength - preNLRI) % prefixWireBytes)
      << "Tail-element NLRI bytes (" << (tailLength - preNLRI)
      << ") are not a multiple of prefix wire size " << prefixWireBytes;

  const size_t nFull = (fullLength - preNLRI) / prefixWireBytes;
  const size_t nTail = (tailLength - preNLRI) / prefixWireBytes;
  // Invariant 5: total prefix count matches.
  EXPECT_EQ(totalPrefixes, nFull * (chainCount - 1) + nTail)
      << "Prefix count mismatch after decoding chain layout: nFull=" << nFull
      << ", nTail=" << nTail << ", C=" << chainCount;

  /*
   * Invariant 6: per-message slack ∈ [reserve - prefixWireBytes + 1, reserve].
   *
   * The packer's loop is `while (nlri_used + reserve < bufSize)`. It stops at
   * the smallest `nlri_used` satisfying `nlri_used + reserve >= bufSize`.
   * Since `nlri_used` advances by exactly `prefixWireBytes` per iteration,
   * the final `nlri_used` lies in `[bufSize - reserve, bufSize - reserve +
   * prefixWireBytes - 1]`, so `slack = bufSize - nlri_used` lies in
   * `[reserve - prefixWireBytes + 1, reserve]`. The fully assembled message
   * length adds the constant pre-NLRI overhead, so the same window applies
   * to `kMaxBgpMsgLen - elementLength` for non-tail elements.
   */
  const size_t fullSlack = kMaxBgpMsgLen - fullLength;
  EXPECT_LE(fullSlack, packerReserveBytes)
      << "Non-tail slack " << fullSlack << " exceeds packer reserve "
      << packerReserveBytes
      << "; packer could have fit another prefix in each UPDATE";
  EXPECT_GE(fullSlack + prefixWireBytes, packerReserveBytes + 1)
      << "Non-tail slack " << fullSlack << " is below the packer's minimum "
      << "stop-condition slack (" << (packerReserveBytes - prefixWireBytes + 1)
      << "); packer stopped before its own stop condition fired";

  // Invariant 7: tail holds [1, nFull] prefixes.
  ASSERT_GT(nTail, 0u)
      << "Tail element holds 0 prefixes; chain should have one fewer element";
  EXPECT_LE(nTail, nFull) << "Tail holds " << nTail
                          << " prefixes, more than nFull=" << nFull
                          << "; non-tail packing was sub-optimal";

  // Invariant 8: chain length is the minimum required.
  ASSERT_GT(nFull, 0u);
  const size_t minChainCount = (totalPrefixes + nFull - 1) / nFull;
  EXPECT_EQ(chainCount, minChainCount)
      << "Chain has " << chainCount
      << " elements, but the minimum required for nFull=" << nFull
      << " per message and " << totalPrefixes << " total prefixes is "
      << minChainCount;
}

} // namespace

class UpdateGroupSerializedIobufChainTest : public E2ETestFixture {
 protected:
  void SetUp() override {
    addPeer(kDefaultPeerSpec3);
    addPeer(kDefaultPeerSpec4);
    setDefaultQueueSizes(/*capacity=*/4096, /*highWm=*/3500, /*lowWm=*/1000);
    createRib();
    createPeerManager(
        /*enableUpdateGroup=*/true,
        /*enableEgressBackpressure=*/true,
        /*enableSerializeGroupPdu=*/true);
  }

  std::optional<FiberBgpPeer::InputMessageT> popNextMessage(
      const BgpPeerId& peerId) {
    auto queues = getPeerQueues(peerId);
    if (!queues) {
      return std::nullopt;
    }

    int emptyFlushes = 0;
    while (queues->boundedAdjRibOutQ->empty()) {
      if (emptyFlushes++ >= kMaxEmptyFlushes) {
        return std::nullopt;
      }
      rib_->getEventBase().runInEventBaseThreadAndWait([] {});
      peerManager_->getEventBase().runInEventBaseThreadAndWait([] {});
    }

    return folly::coro::blockingWait(queues->boundedAdjRibOutQ->pop());
  }

  std::optional<UpdateDescriptor> findMultiNodeUpdateDescriptor(
      const BgpPeerId& peerId) {
    for (int i = 0; i < kMaxDescriptorReads; ++i) {
      auto message = popNextMessage(peerId);
      if (!message) {
        return std::nullopt;
      }

      if (std::holds_alternative<BgpEndOfRib>(*message)) {
        continue;
      }

      auto* descriptor = std::get_if<UpdateDescriptor>(&*message);
      if (descriptor == nullptr) {
        ADD_FAILURE() << "Expected serialized update-group path to queue "
                      << "UpdateDescriptor messages";
        return std::nullopt;
      }

      EXPECT_NE(nullptr, descriptor->serializedGroupPDU);
      if (descriptor->serializedGroupPDU &&
          descriptor->serializedGroupPDU->countChainElements() > 1) {
        return *descriptor;
      }
    }

    return std::nullopt;
  }
};

TEST_F(
    UpdateGroupSerializedIobufChainTest,
    SerializerPreservesMultiNodeSerializedGroupPduChain) {
  const auto peerId3 = makePeerId(kPeerAddr3);
  const auto peerId4 = makePeerId(kPeerAddr4);

  bringUpPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr4);

  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId4);
  ASSERT_TRUE(waitForEoR(peerId3));
  ASSERT_TRUE(waitForEoR(peerId4));

  auto queues = getPeerQueues(peerId3);
  ASSERT_TRUE(queues.has_value());
  folly::coro::blockingWait(queues->adjRibInQ->push(makeMultiPrefixUpdate()));
  ASSERT_TRUE(waitForRouteInShadowRib(lastRoutePrefix(), kPeerAddr3));

  auto descriptor = findMultiNodeUpdateDescriptor(peerId4);
  ASSERT_TRUE(descriptor.has_value())
      << "Expected at least one packed UpdateDescriptor with a multi-node "
      << "serializedGroupPDU chain";
  ASSERT_NE(nullptr, descriptor->serializedGroupPDU);

  const auto originalChainElements =
      descriptor->serializedGroupPDU->countChainElements();
  const auto originalDataLength =
      descriptor->serializedGroupPDU->computeChainDataLength();
  ASSERT_GT(originalChainElements, 1);

  BgpSerializer serializer(makeDefaultCaps());
  auto serialized = serializer(*descriptor);
  ASSERT_NE(nullptr, serialized);

  EXPECT_EQ(originalChainElements, serialized->countChainElements());
  EXPECT_EQ(originalDataLength, serialized->computeChainDataLength());
}

/*
 * Byte-precise maximum-packing regression for V4 /24 announcements.
 *
 * Announces kMaxPackingV4Slash24Count (4000) /24 prefixes through one
 * BgpUpdate2 with shared attributes. The update group serializer wraps them
 * in MP_REACH_NLRI and splits across multiple BGP UPDATE messages. The chain
 * MUST be packed byte-optimally: see verifyMaxPackingChain() for the full
 * list of byte-exact invariants checked. The cloning path through
 * BgpSerializer must then preserve every byte end-to-end.
 */
TEST_F(
    UpdateGroupSerializedIobufChainTest,
    SerializerByteExactMaxPacksV4Slash24Prefixes) {
  const auto peerId3 = makePeerId(kPeerAddr3);
  const auto peerId4 = makePeerId(kPeerAddr4);

  bringUpPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr4);
  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId4);
  ASSERT_TRUE(waitForEoR(peerId3));
  ASSERT_TRUE(waitForEoR(peerId4));

  auto queues = getPeerQueues(peerId3);
  ASSERT_TRUE(queues.has_value());
  folly::coro::blockingWait(
      queues->adjRibInQ->push(makeIdenticalAttrV4Slash24Update(
          kMaxPackingV4Slash24Count,
          /*baseSecondOctet=*/55,
          "10.55.0.1",
          "5500:1")));
  ASSERT_TRUE(waitForRouteInShadowRib(
      v4Slash24PrefixAt(/*baseSecondOctet=*/55, kMaxPackingV4Slash24Count - 1),
      kPeerAddr3));

  auto descriptor = findMultiNodeUpdateDescriptor(peerId4);
  ASSERT_TRUE(descriptor.has_value())
      << "Expected packed UpdateDescriptor with multi-node chain for "
      << kMaxPackingV4Slash24Count << " same-attribute /24 prefixes";
  ASSERT_NE(nullptr, descriptor->serializedGroupPDU);

  verifyMaxPackingChain(
      descriptor->serializedGroupPDU.get(),
      kMaxPackingV4Slash24Count,
      kV4Slash24NlriBytes,
      kMpV6BasedPackerReserve,
      "V4 /24 x 4000 prefixes");

  const auto chainElements =
      descriptor->serializedGroupPDU->countChainElements();
  const auto dataLength =
      descriptor->serializedGroupPDU->computeChainDataLength();

  BgpSerializer serializer(makeDefaultCaps());
  auto serialized = serializer(*descriptor);
  ASSERT_NE(nullptr, serialized);
  EXPECT_EQ(chainElements, serialized->countChainElements());
  EXPECT_EQ(dataLength, serialized->computeChainDataLength());
}

/*
 * Byte-precise maximum-packing test with a different per-prefix wire size.
 *
 * V4 /32 prefixes are 5 bytes on the wire (1 length + 4 prefix) instead of 4
 * bytes for /24. This exercises the packer's stop-condition arithmetic with
 * a different `prefixWireBytes` divisor, and verifies the slack window
 * narrows accordingly: for ps=5 and reserve=18 the allowed slack is
 * [reserve - ps + 1, reserve] = [14, 18].
 */
TEST_F(
    UpdateGroupSerializedIobufChainTest,
    SerializerByteExactMaxPacksV4Slash32Prefixes) {
  const auto peerId3 = makePeerId(kPeerAddr3);
  const auto peerId4 = makePeerId(kPeerAddr4);

  bringUpPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr4);
  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId4);
  ASSERT_TRUE(waitForEoR(peerId3));
  ASSERT_TRUE(waitForEoR(peerId4));

  auto queues = getPeerQueues(peerId3);
  ASSERT_TRUE(queues.has_value());
  folly::coro::blockingWait(
      queues->adjRibInQ->push(makeIdenticalAttrV4Slash32Update(
          kMaxPackingV4Slash32Count,
          /*baseSecondOctet=*/66,
          "10.66.0.1",
          "6600:1")));
  ASSERT_TRUE(waitForRouteInShadowRib(
      v4Slash32PrefixAt(/*baseSecondOctet=*/66, kMaxPackingV4Slash32Count - 1),
      kPeerAddr3));

  auto descriptor = findMultiNodeUpdateDescriptor(peerId4);
  ASSERT_TRUE(descriptor.has_value())
      << "Expected packed UpdateDescriptor with multi-node chain for "
      << kMaxPackingV4Slash32Count << " same-attribute /32 prefixes";
  ASSERT_NE(nullptr, descriptor->serializedGroupPDU);

  verifyMaxPackingChain(
      descriptor->serializedGroupPDU.get(),
      kMaxPackingV4Slash32Count,
      kV4Slash32NlriBytes,
      kMpV6BasedPackerReserve,
      "V4 /32 x 3000 prefixes");

  const auto chainElements =
      descriptor->serializedGroupPDU->countChainElements();
  const auto dataLength =
      descriptor->serializedGroupPDU->computeChainDataLength();

  BgpSerializer serializer(makeDefaultCaps());
  auto serialized = serializer(*descriptor);
  ASSERT_NE(nullptr, serialized);
  EXPECT_EQ(chainElements, serialized->countChainElements());
  EXPECT_EQ(dataLength, serialized->computeChainDataLength());
}

/*
 * Boundary corner: smallest prefix count that still requires splitting into
 * multiple BGP UPDATE messages. kSmallTailV4Slash24Count = 1010 is just above
 * one message's worth of /24 prefixes (1003 with our attribute set, but the
 * count is robust across reasonable attrLen variation - any K_max >= 506
 * yields a 2-element chain). The tail therefore holds only a handful of
 * prefixes (~7 with attrLen=30), exercising the small-tail corner that the
 * 4000-prefix tests do not reach.
 */
TEST_F(
    UpdateGroupSerializedIobufChainTest,
    SerializerByteExactMaxPacksV4Slash24WithSmallTail) {
  const auto peerId3 = makePeerId(kPeerAddr3);
  const auto peerId4 = makePeerId(kPeerAddr4);

  bringUpPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr4);
  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId4);
  ASSERT_TRUE(waitForEoR(peerId3));
  ASSERT_TRUE(waitForEoR(peerId4));

  auto queues = getPeerQueues(peerId3);
  ASSERT_TRUE(queues.has_value());
  folly::coro::blockingWait(
      queues->adjRibInQ->push(makeIdenticalAttrV4Slash24Update(
          kSmallTailV4Slash24Count,
          /*baseSecondOctet=*/77,
          "10.77.0.1",
          "7700:1")));
  ASSERT_TRUE(waitForRouteInShadowRib(
      v4Slash24PrefixAt(/*baseSecondOctet=*/77, kSmallTailV4Slash24Count - 1),
      kPeerAddr3));

  auto descriptor = findMultiNodeUpdateDescriptor(peerId4);
  ASSERT_TRUE(descriptor.has_value())
      << "Expected 2-element chain for " << kSmallTailV4Slash24Count
      << " same-attribute /24 prefixes; packer should not collapse them "
      << "into a single oversized UPDATE";
  ASSERT_NE(nullptr, descriptor->serializedGroupPDU);

  verifyMaxPackingChain(
      descriptor->serializedGroupPDU.get(),
      kSmallTailV4Slash24Count,
      kV4Slash24NlriBytes,
      kMpV6BasedPackerReserve,
      "V4 /24 x 1010 prefixes (small-tail boundary)");

  const auto chainElements =
      descriptor->serializedGroupPDU->countChainElements();
  const auto dataLength =
      descriptor->serializedGroupPDU->computeChainDataLength();

  BgpSerializer serializer(makeDefaultCaps());
  auto serialized = serializer(*descriptor);
  ASSERT_NE(nullptr, serialized);
  EXPECT_EQ(chainElements, serialized->countChainElements());
  EXPECT_EQ(dataLength, serialized->computeChainDataLength());
}

} // namespace bgp
} // namespace facebook
