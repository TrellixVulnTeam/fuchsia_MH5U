# Copyright 2019 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

# These are a set of temporarily allowed exceptions to the area dependency
# rules. This data is a mapping from areas to target prefixes.  Any target
# within an area is allowed to depend on any target whose label starts with a
# prefix in that area's list.
exceptions = {
    '//src/cobalt': ['//src/connectivity/wlan/lib/mlme/cpp:mlme',],
    '//src/connectivity/network':
        [
            '//src/go/grand_unified_binary:grand_unified_binary',
            '//src/connectivity/network/testing/netemul/lib/',
        ],
    '//src/connectivity/network/netstack3':
        ['//src/connectivity/network/testing/netemul/lib/',],
    '//src/connectivity/network/testing/netemul':
        ['//garnet/lib/process:process',],
    '//src/connectivity/wlan':
        [
            '//garnet/lib/wlan/',
            '//src/connectivity/wlan/drivers',
        ],
    '//src/connectivity/wlan/drivers': ['//garnet/lib/wlan/',],
    '//src/connectivity/wlan/tools/wlantool': ['//garnet/lib/wlan/',],
    '//src/connectivity/wlan/wlancfg': ['//garnet/lib/wlan/',],
    '//src/connectivity/wlan/wlanstack':
        [
            '//garnet/lib/wlan/',
            '//src/connectivity/wlan/testing/wlantap-client',
        ],
    '//src/developer/debug': ['//garnet/lib/process:process',],
    '//src/developer/feedback_agent': ['//src/ui/lib/escher/test',],
    '//src/modular':
        [
            '//garnet/testing/benchmarking',
            '//src/modular/bin',
            '//peridot/lib',
            '//peridot/public',
        ],
    '//src/stories/bin/discovermgr': ['//src/sys/component_index/fidl',],
    '//src/sys': ['//examples/components/',],
    '//src/testing/host-target-testing': ['//garnet/go/src',],
    '//src/ui': ['//garnet/bin/ui',],
    '//src/ui/examples/escher': ['//garnet/bin/ui',],
    '//src/virtualization':
        [
            '//src/graphics/lib/magma',
            '//garnet/packages/experimental:debian_guest',
            '//garnet/packages/experimental:zircon_guest',
            '//garnet/packages/prod:guest_runner',
        ],
}
