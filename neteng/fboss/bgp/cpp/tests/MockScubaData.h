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
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "rfe/scubadata/ScubaData.h"

namespace facebook::bgp {

class MockScubaData : public rfe::ScubaData {
 public:
  explicit MockScubaData() : ScubaData("") {}

  MOCK_METHOD(
      size_t,
      addSample,
      (const rfe::ScubaDataSample& sample,
       int bucket,
       rfe::ScubaWriteMode writeType,
       scribe::api::thrift::MessageMetadata&& meta),
      (override));
};

} // namespace facebook::bgp
