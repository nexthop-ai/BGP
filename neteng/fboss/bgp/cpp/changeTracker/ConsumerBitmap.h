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

#pragma once

#include <cstdint>
#include <vector>

/**
 * Bitmap to represent consumers (up to 4096 consumers)
 * Using a vector of uint64_t, each representing 64 bits
 * 4096 / 64 = 64 uint64_t values needed
 */
using ConsumerBitmap = std::vector<uint64_t>;

/**
 * Helper functions for manipulating the consumer bitmap
 */
namespace BitmapUtils {
// Number of bits in a uint64_t
constexpr size_t kBitsPerWord = 64;
// Set a bit in the bitmap
inline void setBit(ConsumerBitmap& bitmap, size_t position) {
  size_t index = position / kBitsPerWord;
  size_t bitOffset = position % kBitsPerWord;

  if (index >= bitmap.size()) {
    bitmap.resize(index + 1, 0);
  }

  // Safe access after ensuring index is valid
  if (index < bitmap.size()) {
    bitmap[index] |= (1ULL << bitOffset);
  }
}

// Clear a bit in the bitmap
inline void clearBit(ConsumerBitmap& bitmap, size_t position) {
  size_t index = position / kBitsPerWord;
  size_t bitOffset = position % kBitsPerWord;

  if (index < bitmap.size()) {
    bitmap[index] &= ~(1ULL << bitOffset);
  }
}

// Check if a bit is set in the bitmap
inline bool isBitSet(const ConsumerBitmap& bitmap, size_t position) {
  size_t index = position / kBitsPerWord;
  size_t bitOffset = position % kBitsPerWord;

  if (index >= bitmap.size()) {
    return false;
  }

  return (bitmap[index] & (1ULL << bitOffset)) != 0;
}

// Check if all bits are cleared in the bitmap
inline bool isAllBitsCleared(const ConsumerBitmap& bitmap) {
  for (const auto& bits : bitmap) {
    if (bits != 0) {
      return false;
    }
  }
  return true;
}

// Clear all bits in the bitmap efficiently
inline void clearAllBits(ConsumerBitmap& bitmap) {
  std::fill(bitmap.begin(), bitmap.end(), 0);
}

// Create a bitmap with all bits set for registered consumers
inline ConsumerBitmap createFullBitmap(size_t numConsumers) {
  ConsumerBitmap bitmap;
  size_t numWords = (numConsumers + kBitsPerWord - 1) / kBitsPerWord;
  bitmap.resize(numWords, ~0ULL); // Set all bits to 1

  // Handle the last word if numConsumers is not a multiple of kBitsPerWord
  size_t remainingBits = numConsumers % kBitsPerWord;
  if (remainingBits > 0 && numWords > 0) {
    // Create a mask with only the required bits set
    bitmap[numWords - 1] = (1ULL << remainingBits) - 1;
  }

  return bitmap;
}

// Bitwise OR two bitmaps
inline ConsumerBitmap orBitmaps(
    const ConsumerBitmap& bitmap1,
    const ConsumerBitmap& bitmap2) {
  ConsumerBitmap result;
  size_t maxSize = std::max(bitmap1.size(), bitmap2.size());
  result.resize(maxSize, 0);

  for (size_t i = 0; i < bitmap1.size(); ++i) {
    result[i] = bitmap1[i];
  }

  for (size_t i = 0; i < bitmap2.size(); ++i) {
    result[i] |= bitmap2[i];
  }

  return result;
}
} // namespace BitmapUtils
