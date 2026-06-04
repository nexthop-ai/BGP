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

#include "neteng/fboss/bgp/cpp/changeTracker/ConsumerBitmap.h"

using namespace testing;

class ConsumerBitmapTest : public Test {
 protected:
  void SetUp() override {}
  void TearDown() override {}
};

TEST_F(ConsumerBitmapTest, SetBitTest) {
  // Test setting bits in an empty bitmap
  ConsumerBitmap bitmap;
  EXPECT_TRUE(bitmap.empty());

  // Set bit 0
  BitmapUtils::setBit(bitmap, 0);
  EXPECT_EQ(bitmap.size(), 1);
  EXPECT_EQ(bitmap[0], 1ULL);

  // Set bit 1
  BitmapUtils::setBit(bitmap, 1);
  EXPECT_EQ(bitmap.size(), 1);
  EXPECT_EQ(bitmap[0], 3ULL); // 11 in binary

  // Set bit 63 (still in first word)
  BitmapUtils::setBit(bitmap, 63);
  EXPECT_EQ(bitmap.size(), 1);
  EXPECT_EQ(bitmap[0], 3ULL | (1ULL << 63));

  // Set bit 64 (should create a new word)
  BitmapUtils::setBit(bitmap, 64);
  EXPECT_EQ(bitmap.size(), 2);
  EXPECT_EQ(bitmap[1], 1ULL);

  // Set bit 128 (should create a third word)
  BitmapUtils::setBit(bitmap, 128);
  EXPECT_EQ(bitmap.size(), 3);
  EXPECT_EQ(bitmap[2], 1ULL);

  // Set bit at a very high position
  BitmapUtils::setBit(bitmap, 4000);
  EXPECT_EQ(bitmap.size(), 63); // 4000/64 + 1 = 63
  EXPECT_EQ(bitmap[62], 1ULL << (4000 % 64));
}

TEST_F(ConsumerBitmapTest, ClearBitTest) {
  // Create a bitmap with some bits set
  ConsumerBitmap bitmap(3, 0);
  bitmap[0] = 0xF; // 1111 in binary
  bitmap[1] = 0xF0; // 11110000 in binary
  bitmap[2] = 0xF00; // 111100000000 in binary

  // Clear bit 0
  BitmapUtils::clearBit(bitmap, 0);
  EXPECT_EQ(bitmap[0], 0xE); // 1110 in binary

  // Clear bit 1
  BitmapUtils::clearBit(bitmap, 1);
  EXPECT_EQ(bitmap[0], 0xC); // 1100 in binary

  // Clear bit 4 (in the first word)
  BitmapUtils::clearBit(bitmap, 4);
  EXPECT_EQ(bitmap[0], 0xC); // No change, bit was already 0

  // Clear bit 68 (in the second word)
  BitmapUtils::clearBit(bitmap, 68);
  EXPECT_EQ(bitmap[1], 0xE0); // 11100000 in binary

  // Clear bit at a position beyond the bitmap size
  BitmapUtils::clearBit(bitmap, 200);
  EXPECT_EQ(bitmap.size(), 3); // Size should not change
}

TEST_F(ConsumerBitmapTest, IsBitSetTest) {
  // Create a bitmap with specific bits set
  ConsumerBitmap bitmap(3, 0);
  bitmap[0] = 0x5; // 101 in binary
  bitmap[1] = 0xA; // 1010 in binary
  bitmap[2] = 0x5; // 101 in binary

  // Check bits in the first word
  EXPECT_TRUE(BitmapUtils::isBitSet(bitmap, 0));
  EXPECT_FALSE(BitmapUtils::isBitSet(bitmap, 1));
  EXPECT_TRUE(BitmapUtils::isBitSet(bitmap, 2));

  // Check bits in the second word
  EXPECT_FALSE(BitmapUtils::isBitSet(bitmap, 64));
  EXPECT_TRUE(BitmapUtils::isBitSet(bitmap, 65));
  EXPECT_FALSE(BitmapUtils::isBitSet(bitmap, 66));
  EXPECT_TRUE(BitmapUtils::isBitSet(bitmap, 67));

  // Check bits in the third word
  EXPECT_TRUE(BitmapUtils::isBitSet(bitmap, 128));
  EXPECT_FALSE(BitmapUtils::isBitSet(bitmap, 129));
  EXPECT_TRUE(BitmapUtils::isBitSet(bitmap, 130));

  // Check bit at a position beyond the bitmap size
  EXPECT_FALSE(BitmapUtils::isBitSet(bitmap, 200));
}

TEST_F(ConsumerBitmapTest, IsAllBitsClearedTest) {
  // Empty bitmap should have all bits cleared
  ConsumerBitmap emptyBitmap;
  EXPECT_TRUE(BitmapUtils::isAllBitsCleared(emptyBitmap));

  // Bitmap with all zeros should have all bits cleared
  ConsumerBitmap zeroBitmap(3, 0);
  EXPECT_TRUE(BitmapUtils::isAllBitsCleared(zeroBitmap));

  // Set a bit and verify allBitsCleared returns false
  BitmapUtils::setBit(zeroBitmap, 0);
  EXPECT_FALSE(BitmapUtils::isAllBitsCleared(zeroBitmap));

  // Clear the bit and verify allBitsCleared returns true again
  BitmapUtils::clearBit(zeroBitmap, 0);
  EXPECT_TRUE(BitmapUtils::isAllBitsCleared(zeroBitmap));

  // Set a bit in a different word
  BitmapUtils::setBit(zeroBitmap, 65);
  EXPECT_FALSE(BitmapUtils::isAllBitsCleared(zeroBitmap));

  // Clear all bits
  zeroBitmap[1] = 0;
  EXPECT_TRUE(BitmapUtils::isAllBitsCleared(zeroBitmap));
}

TEST_F(ConsumerBitmapTest, CreateFullBitmapTest) {
  // Create a bitmap for 0 consumers
  ConsumerBitmap bitmap0 = BitmapUtils::createFullBitmap(0);
  EXPECT_TRUE(bitmap0.empty());

  // Create a bitmap for 1 consumer
  ConsumerBitmap bitmap1 = BitmapUtils::createFullBitmap(1);
  EXPECT_EQ(bitmap1.size(), 1);
  EXPECT_EQ(bitmap1[0], 1ULL);
  EXPECT_TRUE(BitmapUtils::isBitSet(bitmap1, 0));

  // Create a bitmap for 64 consumers (exactly one word)
  ConsumerBitmap bitmap64 = BitmapUtils::createFullBitmap(64);
  EXPECT_EQ(bitmap64.size(), 1);
  EXPECT_EQ(bitmap64[0], ~0ULL); // All bits set
  for (size_t i = 0; i < 64; ++i) {
    EXPECT_TRUE(BitmapUtils::isBitSet(bitmap64, i));
  }

  // Create a bitmap for 65 consumers (spans two words)
  ConsumerBitmap bitmap65 = BitmapUtils::createFullBitmap(65);
  EXPECT_EQ(bitmap65.size(), 2);
  EXPECT_EQ(bitmap65[0], ~0ULL); // All bits set in first word
  EXPECT_EQ(bitmap65[1], 1ULL); // Only first bit set in second word
  for (size_t i = 0; i < 65; ++i) {
    EXPECT_TRUE(BitmapUtils::isBitSet(bitmap65, i));
  }
  EXPECT_FALSE(BitmapUtils::isBitSet(bitmap65, 65));

  // Create a bitmap for 200 consumers (spans 4 words)
  ConsumerBitmap bitmap200 = BitmapUtils::createFullBitmap(200);
  EXPECT_EQ(bitmap200.size(), 4); // 200/64 = 3.125, so 4 words needed
  for (size_t i = 0; i < 200; ++i) {
    EXPECT_TRUE(BitmapUtils::isBitSet(bitmap200, i));
  }
  EXPECT_FALSE(BitmapUtils::isBitSet(bitmap200, 200));
}

TEST_F(ConsumerBitmapTest, CombinedOperationsTest) {
  // Test a sequence of operations to ensure they work together correctly
  ConsumerBitmap bitmap;

  // Set some bits
  BitmapUtils::setBit(bitmap, 0);
  BitmapUtils::setBit(bitmap, 64);
  BitmapUtils::setBit(bitmap, 128);

  // Verify bits are set
  EXPECT_TRUE(BitmapUtils::isBitSet(bitmap, 0));
  EXPECT_TRUE(BitmapUtils::isBitSet(bitmap, 64));
  EXPECT_TRUE(BitmapUtils::isBitSet(bitmap, 128));
  EXPECT_FALSE(BitmapUtils::isAllBitsCleared(bitmap));

  // Clear some bits
  BitmapUtils::clearBit(bitmap, 0);
  BitmapUtils::clearBit(bitmap, 128);

  // Verify bits are cleared
  EXPECT_FALSE(BitmapUtils::isBitSet(bitmap, 0));
  EXPECT_TRUE(BitmapUtils::isBitSet(bitmap, 64));
  EXPECT_FALSE(BitmapUtils::isBitSet(bitmap, 128));
  EXPECT_FALSE(BitmapUtils::isAllBitsCleared(bitmap));

  // Clear the last set bit
  BitmapUtils::clearBit(bitmap, 64);

  // Verify all bits are cleared
  EXPECT_FALSE(BitmapUtils::isBitSet(bitmap, 64));
  EXPECT_TRUE(BitmapUtils::isAllBitsCleared(bitmap));
}

TEST_F(ConsumerBitmapTest, ClearAllBitsTest) {
  // Create a bitmap with some bits set
  ConsumerBitmap bitmap(3, 0);
  bitmap[0] = 0xFF; // All bits set in first word
  bitmap[1] = 0xAA; // 10101010 pattern in second word
  bitmap[2] = 0x55; // 01010101 pattern in third word

  // Verify bits are set
  EXPECT_TRUE(BitmapUtils::isBitSet(bitmap, 0));
  EXPECT_TRUE(BitmapUtils::isBitSet(bitmap, 7));
  EXPECT_TRUE(BitmapUtils::isBitSet(bitmap, 65));
  EXPECT_TRUE(BitmapUtils::isBitSet(bitmap, 130));
  EXPECT_FALSE(BitmapUtils::isAllBitsCleared(bitmap));

  // Clear all bits
  BitmapUtils::clearAllBits(bitmap);

  // Verify all bits are cleared
  EXPECT_TRUE(BitmapUtils::isAllBitsCleared(bitmap));
  EXPECT_FALSE(BitmapUtils::isBitSet(bitmap, 0));
  EXPECT_FALSE(BitmapUtils::isBitSet(bitmap, 7));
  EXPECT_FALSE(BitmapUtils::isBitSet(bitmap, 65));
  EXPECT_FALSE(BitmapUtils::isBitSet(bitmap, 130));

  // Verify the size is maintained
  EXPECT_EQ(bitmap.size(), 3);

  // Test with empty bitmap
  ConsumerBitmap emptyBitmap;
  BitmapUtils::clearAllBits(emptyBitmap);
  EXPECT_TRUE(emptyBitmap.empty());
  EXPECT_TRUE(BitmapUtils::isAllBitsCleared(emptyBitmap));
}

TEST_F(ConsumerBitmapTest, EdgeCasesTest) {
  ConsumerBitmap bitmap;

  // Test with bit position at uint64_t boundary
  BitmapUtils::setBit(bitmap, 63);
  BitmapUtils::setBit(bitmap, 64);
  EXPECT_TRUE(BitmapUtils::isBitSet(bitmap, 63));
  EXPECT_TRUE(BitmapUtils::isBitSet(bitmap, 64));
  EXPECT_EQ(bitmap.size(), 2);

  // Test with very large bit positions
  BitmapUtils::setBit(bitmap, 4095); // Max supported consumer ID
  EXPECT_TRUE(BitmapUtils::isBitSet(bitmap, 4095));
  EXPECT_EQ(bitmap.size(), 64); // 4095/64 + 1 = 64

  // Test clearing bits at boundaries
  BitmapUtils::clearBit(bitmap, 63);
  BitmapUtils::clearBit(bitmap, 64);
  EXPECT_FALSE(BitmapUtils::isBitSet(bitmap, 63));
  EXPECT_FALSE(BitmapUtils::isBitSet(bitmap, 64));

  // Test with an empty bitmap
  ConsumerBitmap emptyBitmap;
  EXPECT_FALSE(BitmapUtils::isBitSet(emptyBitmap, 0));
  EXPECT_TRUE(BitmapUtils::isAllBitsCleared(emptyBitmap));
}
