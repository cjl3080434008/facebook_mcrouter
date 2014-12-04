# Copyright (c) 2014, Facebook, Inc.
#  All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree. An additional grant
# of patent rights can be found in the PATENTS file in the same directory.

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function
from __future__ import unicode_literals

import unittest
import threading
import time

from mcrouter.test.MCProcess import *
from mcrouter.test.McrouterTestCase import McrouterTestCase

class TestMcrouterRoutingPrefixAscii(McrouterTestCase):
    config = './mcrouter/test/routing_prefix_test_ascii.json'
    extra_args = []

    def setUp(self):
        # The order here must corresponds to the order of hosts in the .json
        self.allhosts = []
        for i in range(0, 4):
            self.allhosts.append(self.add_server(Memcached()))

    def get_mcrouter(self):
        return self.add_mcrouter(
            self.config, '/a/a/', extra_args=self.extra_args)

    def test_routing_prefix(self):
        mcr = self.get_mcrouter()
        nclusters = len(self.allhosts)

        # first try setting a key to the local cluster
        mcr.set("testkeylocal", "testvalue")
        self.assertEqual(self.allhosts[0].get("testkeylocal"), "testvalue")
        for i in range(1, nclusters):
            self.assertIsNone(self.allhosts[i].get("testkeylocal"))

        mcr.set("/*/*/testkey-routing", "testvalue")
        # /*/*/ is all-fastest, and some requests might complete asynchronously.
        # As a workaround, just wait
        time.sleep(1)

        local = self.allhosts[0].get("testkey-routing", True)
        self.assertEqual(local["value"], "testvalue")
        # make sure the key got set as "/*/*/key"
        for i in range(1, nclusters):
            local = self.allhosts[i].get("/*/*/testkey-routing", True)
            self.assertEqual(local["value"], "testvalue")

class TestMcrouterRoutingPrefixUmbrella(TestMcrouterRoutingPrefixAscii):
    config = './mcrouter/test/routing_prefix_test_umbrella.json'
