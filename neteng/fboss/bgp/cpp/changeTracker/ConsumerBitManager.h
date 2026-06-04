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

#include <cstddef>
#include <set>
#include <utility>

/**
 * Error codes for ConsumerBitManager operations.
 */
enum class ConsumerBitError {
  SUCCESS = 0,
  INVALID_BIT_TOO_BIG,
  INVALID_BIT_ALREADY_FREED
};

/**
 * ConsumerBitManager manages bit positions for consumers using intervals.
 * It ensures that freed bit positions are not immediately reused
 * unless all other positions are used.
 * NOTE: This class is not thread-safe. It is expected to be used within a
 * thread. If it is used in multiple threads, synchronization is required.
 *
 * Allocation is O(1) and freeing is O(log n) where n is the number of
 * distinct intervals of available bit positions.
 */
class ConsumerBitManager {
 public:
  /**
   * Constructor.
   * @param maxBits The maximum number of bits to manage (default: 4096)
   */
  explicit ConsumerBitManager(size_t maxBits = 4096);

  /**
   * Get a new bit position.
   * @return A bit position that is not currently in use.
   * @throws std::runtime_error if all bit positions are in use.
   */
  size_t getConsumerBit();

  /**
   * Free a bit position.
   * @param bitPosition The bit position to free.
   * @return ConsumerBitError::SUCCESS if the bit was successfully freed,
   *         ConsumerBitError::INVALID_BIT_TOO_BIG if the bit position is
   * invalid, ConsumerBitError::INVALID_BIT_ALREADY_FREED if the bit is already
   * free.
   */
  ConsumerBitError freeConsumerBit(size_t bitPosition);

  /**
   * Check if a bit position is in use.
   * @param bitPosition The bit position to check.
   * @return true if the bit position is in use, false otherwise.
   */
  bool isBitInUse(size_t bitPosition) const;

  /**
   * Get the number of bit positions currently in use.
   * @return The number of bit positions in use.
   */
  size_t getNumBitsInUse() const;

  /**
   * Get the number of bit positions available.
   * @return The number of bit positions available.
   */
  size_t getNumBitsAvailable() const;

  /**
   * Get the maximum number of bit positions that can be managed.
   * @return The maximum number of bit positions.
   */
  size_t getMaxBits() const;

 private:
  // Helper method to find the interval containing a bit position
  std::set<std::pair<size_t, size_t>>::const_iterator findInterval(
      size_t bitPosition) const;

  // Maximum number of bits to manage
  const size_t maxBits_;

  // Number of bits currently in use
  size_t numBitsInUse_;

  // Intervals of available bit positions (start, end), inclusive
  std::set<std::pair<size_t, size_t>> availableIntervals_;
};
