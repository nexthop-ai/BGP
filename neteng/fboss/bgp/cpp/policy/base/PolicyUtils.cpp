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

#include "neteng/fboss/bgp/cpp/policy/base/PolicyUtils.h"

namespace facebook {
namespace routing {

bool CompareNumValue(
    const PolicyComparisonOperator& op,
    int value_to_compare,
    int value) {
  switch (op) {
    case PolicyComparisonOperator::EQ:
      return value_to_compare == value;
    case PolicyComparisonOperator::GE:
      return value_to_compare <= value;
    case PolicyComparisonOperator::LE:
      return value_to_compare >= value;
    case PolicyComparisonOperator::NE:
      return value_to_compare != value;
    case PolicyComparisonOperator::GT:
      return value_to_compare < value;
    case PolicyComparisonOperator::LT:
      return value_to_compare > value;
    default:
      return false;
  }
}

} // namespace routing
} // namespace facebook
