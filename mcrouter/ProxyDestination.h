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

#include <atomic>
#include <memory>
#include <string>

#include <folly/IntrusiveList.h>

#include "mcrouter/AccessPoint.h"
#include "mcrouter/config.h"
#include "mcrouter/ExponentialSmoothData.h"
#include "mcrouter/lib/McMsgRef.h"
#include "mcrouter/TkoLog.h"

using asox_timer_t = void*;

namespace facebook { namespace memcache {

class McReply;

namespace mcrouter {

class ProxyClientCommon;
class ProxyClientOwner;
class ProxyClientShared;
class ProxyDestinationMap;
class dynamic_stat_t;
class proxy_t;

enum proxy_client_state_t {
  PROXY_CLIENT_UNKNOWN = 0,   // bug
  PROXY_CLIENT_NEW,           // never connected
  PROXY_CLIENT_UP,            // currently connected
  PROXY_CLIENT_TKO,           // waiting for retry timeout
  PROXY_CLIENT_NUM_STATES
};

struct ProxyDestinationStats {
  bool is_up{false};
  ExponentialSmoothData avgLatency;
  uint64_t results[mc_nres] = {0};

  explicit ProxyDestinationStats(const McrouterOptions& opts);
};

struct ProxyDestination {
  static const uint64_t kDeadBeef = 0xdeadbeefdeadbeefULL;

  proxy_t* proxy{nullptr}; ///< for convenience
  const AccessPoint accessPoint;
  const std::string destinationKey;///< always the same for a given (host, port)
  const int rxpriority{0};
  const int txpriority{0};
  const timeval_t server_timeout{0};
  const std::string pdstnKey;///< consists of ap, server_timeout
  uint64_t magic{0}; ///< to allow asserts that pdstn is still alive
  const uint64_t proxy_magic{0}; ///< to allow asserts that proxy is still alive

  ProxyClientOwner* owner{nullptr};
  std::shared_ptr<ProxyClientShared> shared;

  const bool use_ssl{false};

  static std::shared_ptr<ProxyDestination> create(proxy_t* proxy,
                                                  const ProxyClientCommon& ro,
                                                  std::string pdstnKey);

  ~ProxyDestination();

  // returns non-zero on error
  int send(McMsgRef request, void* req_ctx, uint64_t senderId);
  // returns 1 if okay to send req using this client
  int may_send(const McMsgRef& req);

  /**
   * Returns one of the three states that the server could be in:
   * new, up, or total knockout (tko): means we're out for the count,
   * i.e. we had a timeout or connection failure and haven't had time
   * to recover.
   */
  proxy_client_state_t state() const;

  /**
   * @return stats for ProxyDestination
   */
  const ProxyDestinationStats& stats() const;

  void resetInactive();

  void on_up();
  void on_down();
  void on_reply(const McMsgRef& req, McReply reply, void* req_ctx);

  // on probe timer
  void on_timer(const asox_timer_t timer);

  size_t getPendingRequestCount() const;
  size_t getInflightRequestCount() const;

 private:
  std::unique_ptr<DestinationMcClient> client_;

  ProxyDestinationStats stats_;

  int probe_delay_next_ms{0};
  bool sending_probes{false};
  McMsgRef probe_req;
  asox_timer_t probe_timer{nullptr};
  size_t consecutiveErrors_{0};
  size_t probesSent_{0};

  char resetting{0}; // If 1 when inside on_down, the call was due to a forced
                     // mc_client_reset and not a remote connection failure.

  // tko behaviour
  char marked_tko{0};

  void start_sending_probes();
  void stop_sending_probes();

  void schedule_next_probe();

  void reset_fields();

  void handle_tko(const McReply& reply,
                  bool is_probe_req,
                  int consecutive_errors);

  void mark_tko();
  void unmark_tko();

  void initializeClient();

  ProxyDestination(proxy_t* proxy,
                   const ProxyClientCommon& ro,
                   std::string pdstnKey);

  // for no-network mode (debug/performance measurement only)
  void sendFakeReply(const McMsgRef& request, void* req_ctx);

  void onTkoEvent(TkoLogEvent event, mc_res_t result) const;

  void* stateList_{nullptr};
  folly::IntrusiveListHook stateListHook_;

  std::weak_ptr<ProxyDestination> selfPtr_;

  friend class ProxyDestinationMap;
};

}}}  // facebook::memcache::mcrouter
