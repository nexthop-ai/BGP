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

#include "neteng/fboss/bgp/if/gen-cpp2/BgpStructs_types.h"

namespace facebook {
namespace nettools {
namespace bgplib {

class BgpAttributesSmartSet;

/**
 * A wrapper on top of share_ptr<BgpAttributes>, provides a efficient storage
 * of BgpAttributes. Can be used by any std containers such as unordered_map,
 * unordered_set, RadixTree, etc.
 * attrsSetPtr points to the set that contains the object. The destructor
 * removes the object if it's the last reference to the BgpAttributes.
 * hash() is overridden to compute the actual BgpAttributes's object hash.
 * operator== is overridden to compare the actual BgpAttributes fields.
 */
class BgpAttributesWrapper {
 public:
  BgpAttributesWrapper();

  explicit BgpAttributesWrapper(
      std::shared_ptr<bgplib::BgpAttributes> attrsPtr,
      BgpAttributesSmartSet* setPtr = nullptr);

  // Copy constructor
  BgpAttributesWrapper(const BgpAttributesWrapper& copy);

  // Move constructor
  BgpAttributesWrapper(BgpAttributesWrapper&& copy) noexcept;

  ~BgpAttributesWrapper();

  // Comparator
  bool operator==(const BgpAttributesWrapper& other) const;

  // Comparator != with BgpAttributesSmartPtr&
  bool operator!=(const BgpAttributesWrapper& other) const;

  // Assignment operator
  BgpAttributesWrapper& operator=(const BgpAttributesWrapper& other);

  // Move assignment operator
  BgpAttributesWrapper& operator=(BgpAttributesWrapper&& other);

  // Boolean operator
  explicit operator bool() const;

  // Getter function
  bgplib::BgpAttributes get() const;

 private:
  std::shared_ptr<bgplib::BgpAttributes> attrsPtr_;
  BgpAttributesSmartSet* attrsSetPtr_{nullptr};

  // Remove it from the BgpAttributesSmartSet if no other reference
  void freeFromSet();
};

} // namespace bgplib
} // namespace nettools
} // namespace facebook

namespace std {

/**
 * Make BgpAttributesWrapper hashable
 */
template <>
struct hash<facebook::nettools::bgplib::BgpAttributesWrapper> {
  size_t operator()(
      facebook::nettools::bgplib::BgpAttributesWrapper const&) const;
};

} // namespace std
