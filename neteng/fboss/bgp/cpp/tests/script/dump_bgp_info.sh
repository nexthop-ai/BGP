#!/bin/bash
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

#
# Run fboss cli to dump BGP related information into different files.
# Used for manual testing to compare behavior change in bgp++.
#
# $1 - HostName, default to localhost
#
# Examples
#
#   mkdir old && cp dump_bgp_info.sh old/ && ./old/dump_bgp_info.sh rsw1ky.11.frc2
#   # deploy to rsw1ky.11.frc2
#   mkdir new && cp dump_bgp_info.sh ew/ && ./new/dump_bgp_info.sh rsw1ky.11.frc2
#   diff -r old/ new/ > diff.out
#

HOST="$1"

if [ ! -n "${HOST}" ]; then
  HOST="localhost"
fi

echo "dumping bgp info for ${HOST}..."

fboss -H "${HOST}" bgp summary | cut -c -135 > neighbors.txt

fboss -H "${HOST}" bgp table > rib.txt

neighbors=$(fboss -H ${HOST} bgp summary | grep ESTABLISHED | cut -d ' ' -f1)
for n in ${neighbors[*]}; do
  echo "processing neighbor ${n}...";
  fboss -H "${HOST}" bgp prefilter-received "${n}" > "${n}_preIn.txt"
  fboss -H "${HOST}" bgp postfilter-received "${n}" > "${n}_postIn.txt"
  fboss -H "${HOST}" bgp prefilter-advertised "${n}" > "${n}_preOut.txt"
  fboss -H "${HOST}" bgp postfilter-advertised "${n}" > "${n}_postOut.txt"
done
