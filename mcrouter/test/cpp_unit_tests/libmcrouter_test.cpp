/**
 *  Copyright (c) 2014, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 */
#include <stdio.h>

#include <memory>

#include <gflags/gflags.h>
#include <gtest/gtest.h>

#include <folly/dynamic.h>
#include <folly/FileUtil.h>
#include <folly/io/async/EventBase.h>
#include <folly/Memory.h>

#include "mcrouter/_router.h"
#include "mcrouter/config.h"
#include "mcrouter/proxy.h"
#include "mcrouter/ProxyThread.h"
#include "mcrouter/router.h"
#include "mcrouter/lib/network/test/TestUtil.h"
#include "mcrouter/test/cpp_unit_tests/mcrouter_test_client.h"
#include "mcrouter/test/cpp_unit_tests/MemcacheLocal.h"

using namespace facebook::memcache::mcrouter;
using namespace facebook::memcache::test;
using namespace folly;

using facebook::memcache::McrouterOptions;

/*
 * * * * * * * * * SPECIAL NOTE : SPECIAL NOTE : SPECIAL NOTE * * * * * * * * *
 *
 * This tester program tests the functional behavior of libmcrouter, which
 * communicates with a running memcached server. To deterministically test the
 * functional behavior, libmcrouter is configured to use a local memcached
 * server instead of the production servers. The configuration string for
 * libmcrouter is generated by generateConfigString(char *buf, int port)
 * function, which assumes memcached is running in the localhost.
 *
 * Before starting the tests, we need the "Local Memcached" up, and generate
 * the configuration string for libmcrouer. The try block in the main(..)
 * function takes care of these tasks.
 *
 * * * * * * * * RECOMMENDED PRACTICES : RECOMMENDED PRACTICES * * * * * * * * *
 *
 *
 * Before calling RUN_ALL_TESTS(..) first check memcacheLocal variable
 * as a gate. This variable should not be null when the tests are run.
 *
 */

std::string kDefaultRoute = "/oregon/prn2c07";
std::string configString;
std::unique_ptr<MemcacheLocal> memcacheLocal = nullptr; // local memcached
std::string kInvalidPoolConfig =
  "mcrouter/test/cpp_unit_tests/files/libmcrouter_invalid_pools.json";

namespace {

/**
 * @return File descriptor
 */
int connectToLocalPort(uint16_t port) {
  struct addrinfo hints;
  struct addrinfo* res;

  memset(&hints, 0, sizeof hints);
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;

  CHECK(!getaddrinfo("localhost", folly::to<std::string>(port).data(),
                     &hints, &res));
  auto client_socket =
    socket(res->ai_family, res->ai_socktype, res->ai_protocol);

  CHECK(client_socket >= 0);
  CHECK(!connect(client_socket, res->ai_addr, res->ai_addrlen));

  freeaddrinfo(res);

  return client_socket;
}

void checkRequestReply(int fd,
                       folly::StringPiece request,
                       folly::StringPiece reply) {
  size_t n = folly::writeFull(fd, request.data(), request.size());
  CHECK(n == request.size());

  char replyBuf[1000];
  n = folly::readFull(fd, replyBuf, reply.size());
  CHECK(n == reply.size());

  EXPECT_TRUE(folly::StringPiece(replyBuf, n) == reply);
}

}  // namespace

TEST(libmcrouter, sanity) {
  auto opts = defaultTestOptions();
  opts.config_str = configString;
  opts.default_route = kDefaultRoute;

  MCRouterTestClient client("sanity", opts);

  int nkeys = 100;
  dynamic keys = {};
  dynamic kv_pairs = dynamic::object;
  keys.resize(nkeys);
  for (dynamic i = 0; i < nkeys; i ++) {
    keys[i] = "rajeshn-testkey" + i.asString();
    kv_pairs[keys[i]] = "value" + i.asString();
  }

  // clean everything out
  dynamic delete_results = dynamic::object;
  client.del(keys, true, delete_results);

  // start the test
  dynamic set_results = dynamic::object;
  EXPECT_TRUE(client.set(kv_pairs, set_results) == nkeys);

  for (auto& res : set_results.values()) {
    EXPECT_TRUE(res.asBool());
  }

  dynamic get_results = dynamic::object;
  EXPECT_TRUE(client.get(keys, get_results) == nkeys);

  // make sure we get what we set
  for (auto& res_pair : get_results.items()) {
    auto pos = kv_pairs.find(res_pair.first);
    EXPECT_FALSE(pos == get_results.items().end());
    EXPECT_TRUE(kv_pairs[res_pair.first] == res_pair.second);
  }

  delete_results = dynamic::object;
  EXPECT_TRUE(client.del(keys, true, delete_results) == nkeys);

  for (auto& res : delete_results.values()) {
    EXPECT_TRUE(res.asBool());
  }

  delete_results = dynamic::object;
  EXPECT_TRUE(client.del(keys, true, delete_results) == 0);

  for (auto& res : delete_results.values()) {
    EXPECT_FALSE(res.asBool());
  }
}

static std::atomic<int> on_reply_count{0};
void on_reply(mcrouter_client_t *client,
              mcrouter_msg_t *router_req,
              void* context) {
  on_reply_count++;
  mc_msg_decref(router_req->req);
}

static std::atomic<int> on_cancel_count{0};
void on_cancel(mcrouter_client_t* client,
               void* request_context,
               void* client_context) {
  on_cancel_count++;
}

TEST(libmcrouter, premature_disconnect) {
  auto opts = defaultTestOptions();
  opts.config_str = configString;
  opts.default_route = kDefaultRoute;
  mcrouter_t *router = mcrouter_new(opts);

  for (int i = 0; i < 10; i++) {
    on_reply_count = 0;
    on_cancel_count = 0;

    mcrouter_client_t *client = mcrouter_client_new(
      router,
      {on_reply, on_cancel, nullptr},
      nullptr, 0);

    const char key[] = "adi:unit_test:key";
    mc_msg_t *mc_msg = mc_msg_new(sizeof(key));
    mc_msg->key.str = (char*) &mc_msg[1];
    strcpy(mc_msg->key.str, key);
    mc_msg->key.len = strlen(key);
    mc_msg->op = mc_op_get;
    mcrouter_msg_t router_msg;
    router_msg.req = mc_msg;
    mcrouter_send(client, &router_msg, 1);
    mc_msg_decref(mc_msg);
    mcrouter_client_disconnect(client);

    usleep(50000);

    EXPECT_EQ(0, on_reply_count);
    EXPECT_EQ(1, on_cancel_count);
  }

  sleep(2);
  mcrouter_free(router);
}

TEST(libmcrouter, invalid_pools) {
  auto opts = defaultTestOptions();
  std::string configStr;
  EXPECT_TRUE(folly::readFile(kInvalidPoolConfig.data(), configStr));
  opts.config_str = configStr;
  opts.default_route = "/a/b/";
  mcrouter_t *router = mcrouter_new(opts);
  EXPECT_TRUE(router == nullptr);
}

TEST(libmcrouter, standalone) {
  auto opts = defaultTestOptions();
  opts.standalone = true;
  opts.config_str = configString;
  opts.default_route = kDefaultRoute;
  mcrouter_t *router = mcrouter_init("standalone_test", opts);

  folly::EventBase evb;
  for (size_t i = 0; i < router->opts.num_proxies; ++i) {
    router->getProxy(i)->attachEventBase(&evb);
  }

  mcrouter_client_t *client = mcrouter_client_new(router,
                                                  {on_reply, nullptr},
                                                  nullptr,
                                                  0);

  const char key[] = "mcrouter_test:standalone:key:1";
  mc_msg_t *mc_msg = mc_msg_new(sizeof(key));
  mc_msg->key.str = (char*) &mc_msg[1];
  strcpy(mc_msg->key.str, key);
  mc_msg->key.len = strlen(key);
  mc_msg->op = mc_op_get;
  mcrouter_msg_t msg;
  msg.req = mc_msg;

  on_reply_count = 0;
  mcrouter_send(client, &msg, 1);
  while (on_reply_count == 0) {
    mcrouterLoopOnce(&evb);
  }
  EXPECT_TRUE(on_reply_count == 1);

  /* Allocating a new router in standalone mode should return a new router! */
  mcrouter_t *router2 = mcrouter_init("standalone_test", opts);
  EXPECT_TRUE(router != router2);

  mcrouter_client_disconnect(client);
  mcrouter_free(router);
  mcrouter_free(router2);
}

TEST(libmcrouter, listenSock) {
  /* Create a listen socket, pass it to a child mcrouter and
     check that communication through the socket works */
  auto listen_socket = facebook::memcache::createListenSocket();
  auto port = facebook::memcache::getListenPort(listen_socket);

  std::vector<std::string> args{MCROUTER_INSTALL_PATH "mcrouter/mcrouter",
        "--listen-sock-fd", folly::to<std::string>(listen_socket),
        "--config-str", configString,
        "-R", kDefaultRoute};
  auto testArgs = defaultTestCommandLineArgs();
  args.insert(args.end(), testArgs.begin(), testArgs.end());
  folly::Subprocess mcr(args);

  const std::string kSetRequest = "set testkey 0 0 1\r\nv\r\n";
  const std::string kStoredReply = "STORED\r\n";
  const std::string kGetRequest = "get testkey\r\n";
  const std::string kGetReply = "VALUE testkey 0 1\r\nv\r\n";

  auto mcr_socket = connectToLocalPort(port);
  checkRequestReply(mcr_socket, kSetRequest, kStoredReply);
  checkRequestReply(mcr_socket, kGetRequest, kGetReply);
  CHECK(!close(mcr_socket));

  auto mc_socket = connectToLocalPort(memcacheLocal->getPort());
  checkRequestReply(mc_socket, kGetRequest, kGetReply);
  CHECK(!close(mc_socket));

  mcr.terminate();
  mcr.wait();
}

// for backward compatibility with gflags
namespace gflags { }
namespace google { using namespace gflags; }

int main(int argc, char** argv) {
  testing::InitGoogleTest(&argc, argv);
  google::ParseCommandLineFlags(&argc, &argv, true);
  google::InitGoogleLogging(argv[0]);

  try {
    //blocks until local memcache server ready
    memcacheLocal = folly::make_unique<MemcacheLocal> ();
    configString = memcacheLocal->generateMcrouterConfigString();
  } catch (const SubprocessError& e) { // declared in Subprocess.h
    LOG(ERROR) << "SubprocessError:" << std::endl << e.what();
  }

  EXPECT_TRUE(memcacheLocal != nullptr); // gate to check if memcached is up

  return RUN_ALL_TESTS();
}
