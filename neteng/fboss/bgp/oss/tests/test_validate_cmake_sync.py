# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.


from __future__ import annotations

import unittest

from neteng.fboss.bgp.oss.validate_cmake_sync import (
    get_cmake_sources,
    get_cmake_thrift_targets,
    get_oss_eligible_sources,
    get_repo_root,
    get_thrift_files,
    validate_sources,
    validate_thrift,
)


class ValidateCmakeSyncTest(unittest.TestCase):
    def setUp(self) -> None:
        self.repo_root = get_repo_root()

    def test_cmake_file_exists(self) -> None:
        cmake_path = self.repo_root / "neteng/fboss/bgp/public_tld/CMakeLists.txt"
        self.assertTrue(
            cmake_path.exists(), f"CMakeLists.txt not found at {cmake_path}"
        )

    def test_oss_eligible_sources_not_empty(self) -> None:
        sources = get_oss_eligible_sources(self.repo_root)
        self.assertGreater(len(sources), 0, "No OSS-eligible .cpp files found")

    def test_oss_eligible_sources_exclude_facebook(self) -> None:
        sources = get_oss_eligible_sources(self.repo_root)
        for src in sources:
            self.assertNotIn("/facebook/", src, f"facebook/ file leaked: {src}")

    def test_oss_eligible_sources_exclude_tests(self) -> None:
        sources = get_oss_eligible_sources(self.repo_root)
        for src in sources:
            self.assertNotIn("/tests/", src, f"tests/ file leaked: {src}")

    def test_oss_eligible_sources_exclude_eos_wrapper(self) -> None:
        sources = get_oss_eligible_sources(self.repo_root)
        for src in sources:
            self.assertNotIn("/eos_wrapper/", src, f"eos_wrapper/ file leaked: {src}")

    def test_cmake_sources_not_empty(self) -> None:
        sources = get_cmake_sources(self.repo_root)
        self.assertGreater(
            len(sources), 0, "No sources found in CMakeLists.txt bgppplib"
        )

    def test_all_oss_sources_in_cmake(self) -> None:
        missing, _ = validate_sources(self.repo_root)
        self.assertEqual(
            missing,
            set(),
            "OSS-eligible .cpp files missing from CMakeLists.txt:\n"
            + "\n".join(sorted(missing)),
        )

    def test_no_extra_sources_in_cmake(self) -> None:
        _, extra = validate_sources(self.repo_root)
        self.assertEqual(
            extra,
            set(),
            "CMakeLists.txt lists .cpp files that don't exist on disk:\n"
            + "\n".join(sorted(extra)),
        )

    def test_cmake_thrift_targets_not_empty(self) -> None:
        targets = get_cmake_thrift_targets(self.repo_root)
        self.assertGreater(len(targets), 0, "No thrift targets found in CMakeLists.txt")

    def test_thrift_files_not_empty(self) -> None:
        thrift_files = get_thrift_files(self.repo_root)
        self.assertGreater(len(thrift_files), 0, "No thrift files found on disk")

    def test_all_thrift_files_in_cmake(self) -> None:
        missing, _ = validate_thrift(self.repo_root)
        self.assertEqual(
            missing,
            set(),
            "Thrift files missing from CMakeLists.txt:\n" + "\n".join(sorted(missing)),
        )

    def test_no_extra_thrift_in_cmake(self) -> None:
        _, extra = validate_thrift(self.repo_root)
        self.assertEqual(
            extra,
            set(),
            "CMakeLists.txt references thrift files that don't exist:\n"
            + "\n".join(sorted(extra)),
        )
