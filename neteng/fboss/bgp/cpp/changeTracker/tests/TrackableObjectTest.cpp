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

#include <gtest/gtest.h>
#include <memory>

#include "neteng/fboss/bgp/cpp/changeTracker/ChangeItem.h"
#include "neteng/fboss/bgp/cpp/changeTracker/TrackableObject.h"

namespace {

// Simple test class to use with TrackableObject
class TestObject {
 public:
  explicit TestObject(int value) : value_(value) {}

  int getValue() const {
    return value_;
  }

  void setValue(int value) {
    value_ = value;
  }

 private:
  int value_;
};

// Test that the initial state of TrackableObject is correct
TEST(TrackableObjectTest, InitialState) {
  // Create a TrackableObject with a test object
  auto trackableObject =
      std::make_unique<TrackableObject<TestObject>>(TestObject(42));

  // Verify initial state
  EXPECT_EQ(42, trackableObject->get().getValue());
  EXPECT_FALSE(trackableObject->isOnChangeList());
  EXPECT_EQ(nullptr, trackableObject->getChangeItem());
}

// Test that the get() method returns the correct object
TEST(TrackableObjectTest, GetObject) {
  // Create a TrackableObject with a test object
  auto trackableObject =
      std::make_unique<TrackableObject<TestObject>>(TestObject(42));

  // Verify get() returns the correct object
  EXPECT_EQ(42, trackableObject->get().getValue());

  // Modify the object and verify get() returns the updated object
  trackableObject->get().setValue(100);
  EXPECT_EQ(100, trackableObject->get().getValue());
}

// Test that the setChangeItem() and clearChangeItem() methods work correctly
TEST(TrackableObjectTest, ChangeItemManagement) {
  // Create a TrackableObject with a test object
  auto trackableObject =
      std::make_unique<TrackableObject<TestObject>>(TestObject(42));

  // Create a ConsumerBitmap for the ChangeItem
  ConsumerBitmap bitmap;
  BitmapUtils::setBit(bitmap, 0);

  // Create a ChangeItem
  auto changeItem =
      std::make_unique<ChangeItem<TestObject>>(trackableObject.get(), bitmap);

  // Verify initial state
  EXPECT_FALSE(trackableObject->isOnChangeList());
  EXPECT_EQ(nullptr, trackableObject->getChangeItem());

  // Set the change item
  trackableObject->setChangeItem(std::move(changeItem));

  // Verify the change item was set
  EXPECT_TRUE(trackableObject->isOnChangeList());
  EXPECT_NE(nullptr, trackableObject->getChangeItem());

  // Clear the change item
  trackableObject->clearChangeItem();

  // Verify the change item was cleared
  EXPECT_FALSE(trackableObject->isOnChangeList());
  EXPECT_EQ(nullptr, trackableObject->getChangeItem());
}

// Test that copy and move operations are all deleted
TEST(TrackableObjectTest, DeletedCopyAndMoveOperations) {
  static_assert(
      !std::is_copy_constructible<TrackableObject<TestObject>>::value,
      "TrackableObject should not be copy constructible");
  static_assert(
      !std::is_copy_assignable<TrackableObject<TestObject>>::value,
      "TrackableObject should not be copy assignable");
  static_assert(
      !std::is_move_constructible<TrackableObject<TestObject>>::value,
      "TrackableObject should not be move constructible");
  static_assert(
      !std::is_move_assignable<TrackableObject<TestObject>>::value,
      "TrackableObject should not be move assignable");
}

} // namespace
