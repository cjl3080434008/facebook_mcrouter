/**
 *  Copyright (c) 2014, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 */
#pragma once

#include <sys/time.h>

#include <string>

#include "mcrouter/AccessPoint.h"

namespace facebook { namespace memcache { namespace mcrouter {

class ProxyPool;

struct ProxyClientCommon {
  ProxyPool* pool;
  size_t idx;
  AccessPoint ap;

  /// Always the same for a given (host, port)
  std::string destination_key;

  /// Identifies this particular proxy client.
  /// It's possible to have multiple proxy_clients to the same destination.
  /// In that case, each proxy_client will have a unique proxy_client_key
  /// but destination_key will be the same.
  std::string proxy_client_key;

  int keep_routing_prefix;
  bool devnull_asynclog;
  int rxpriority;
  int txpriority;
  timeval_t server_timeout;

  size_t indexInPool;
  bool useSsl;

  std::string genProxyDestinationKey() const;

  ProxyClientCommon(unsigned index,
                    timeval_t timeout,
                    AccessPoint ap,
                    int keep_routing_prefix,
                    bool devnull_asynclog,
                    ProxyPool* pool,
                    std::string key,
                    int rxpri,
                    int txpri,
                    size_t indexInPool,
                    bool useSsl);
};

}}}  // facebook::memcache::mcrouter
