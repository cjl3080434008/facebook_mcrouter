/**
 *  Copyright (c) 2014, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 */
#include <semaphore.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <string>
#include <vector>

#include <boost/filesystem.hpp>

#include <gtest/gtest.h>

#include <folly/experimental/TestUtil.h>
#include <folly/experimental/Singleton.h>
#include <folly/FileUtil.h>
#include <folly/io/async/EventBase.h>

#include "mcrouter/config.h"
#include "mcrouter/options.h"
#include "mcrouter/router.h"

using namespace boost::filesystem;
using namespace facebook::memcache::mcrouter;
using namespace std;

using facebook::memcache::createMcMsgRef;
using facebook::memcache::McMsgRef;
using facebook::memcache::McReply;
using facebook::memcache::McrouterOptions;

const std::string kAlreadyRepliedConfig =
  "mcrouter/test/cpp_unit_tests/files/already_replied.json";

#define MEMCACHE_CONFIG "mcrouter/test/test_ascii.json"
#define MEMCACHE_ROUTE "/././"

McMsgRef new_get_req(const char *key) {
  auto msg = createMcMsgRef(key);
  msg->op = mc_op_get;
  return std::move(msg);
}

McMsgRef new_del_req(const char *key) {
  auto msg = createMcMsgRef(key);
  msg->op = mc_op_delete;
  return std::move(msg);
}

namespace {
std::atomic<size_t> gReplies;
std::mutex gMutex;
std::condition_variable gCv;
}

void on_reply(mcrouter_client_t *client,
              mcrouter_msg_t *router_req,
              void* context) {
  mcrouter_msg_t *r_msg = static_cast<mcrouter_msg_t*>(router_req->context);
  r_msg->reply = std::move(router_req->reply);
  ++gReplies;
  gCv.notify_one();
}

void mcrouter_send_helper(mcrouter_client_t *client,
                          const vector<McMsgRef>& reqs,
                          vector<McReply> &replies) {
  vector<mc_msg_t*> ret;
  int n = reqs.size();
  replies.clear();

  mcrouter_msg_t *r_msgs = new mcrouter_msg_t[n];
  for (int i = 0; i < n; i++) {
    r_msgs[i].req = const_cast<mc_msg_t*>(reqs[i].get());
    r_msgs[i].reply = McReply(mc_res_unknown);
    r_msgs[i].context = &r_msgs[i];
  }

  gReplies = 0;
  mcrouter_send(client, r_msgs, n);
  std::unique_lock<std::mutex> lk(gMutex);
  gCv.wait(lk, [n] () { return gReplies == n; });

  for (size_t i = 0; i < n; ++i) {
    CHECK(r_msgs[i].reply.result() != mc_res_unknown);
    replies.push_back(std::move(r_msgs[i].reply));
  }
  delete [] r_msgs;
}

TEST(mcrouter, start_and_stop) {
  for (int i = 0; i < 2; i++) {
    auto opts = defaultTestOptions();
    opts.config_file = MEMCACHE_CONFIG;
    opts.default_route = MEMCACHE_ROUTE;
    mcrouter_t *router = mcrouter_new(opts);
    EXPECT_FALSE(router == nullptr);

    mcrouter_client_t *client = mcrouter_client_new(
      router,
      (mcrouter_client_callbacks_t){nullptr, nullptr, nullptr},
      nullptr,
      0);
    EXPECT_FALSE(client == nullptr);

    mcrouter_client_disconnect(client);
    mcrouter_free(router);
  }
}

void on_disconnect(void* context) {
  sem_post((sem_t*)context);
}

TEST(mcrouter, test_zeroreqs_mcroutersend) {
  sem_t sem_disconnect;
  sem_init(&sem_disconnect, 0, 0);
  auto opts = defaultTestOptions();
  opts.config_file = MEMCACHE_CONFIG;
  opts.default_route = MEMCACHE_ROUTE;
  mcrouter_t *router = mcrouter_new(opts);
  mcrouter_client_t *client = mcrouter_client_new(
    router,
    (mcrouter_client_callbacks_t){nullptr, nullptr, on_disconnect},
    &sem_disconnect,
    0);

  vector<mcrouter_msg_t> reqs(0);

  mcrouter_send(client, reqs.data(), reqs.size());
  mcrouter_send(client, nullptr, 0);

  mcrouter_client_disconnect(client);

  mcrouter_free(router);
}

TEST(mcrouter, disconnect_callback) {
  sem_t sem_disconnect;
  sem_init(&sem_disconnect, 0, 0);

  auto opts = defaultTestOptions();
  opts.config_file = MEMCACHE_CONFIG;
  opts.default_route = MEMCACHE_ROUTE;
  mcrouter_t *router = mcrouter_new(opts);
  EXPECT_FALSE(router == nullptr);

  mcrouter_client_t *client = mcrouter_client_new(
    router,
    (mcrouter_client_callbacks_t){nullptr, nullptr, on_disconnect},
    &sem_disconnect,
    0);
  EXPECT_FALSE(client == nullptr);

  const char test_key[] = "test_key_disconnect";
  vector<mcrouter_msg_t> reqs(1);
  auto msg = new_get_req(test_key);
  reqs.back().req = const_cast<mc_msg_t*>(msg.get());
  reqs.back().reply = McReply(mc_res_unknown);

  mcrouter_send(client, reqs.data(), reqs.size());

  EXPECT_NE(0, sem_trywait(&sem_disconnect));
  mcrouter_client_disconnect(client);
  EXPECT_EQ(0, sem_wait(&sem_disconnect));

  mcrouter_free(router);
}

TEST(mcrouter, fork) {
  const char persistence_id[] = "fork";
  auto opts = defaultTestOptions();
  opts.config_file = MEMCACHE_CONFIG;
  opts.default_route = MEMCACHE_ROUTE;
  mcrouter_t *router = mcrouter_init(persistence_id, opts);
  EXPECT_NE(static_cast<mcrouter_t*>(nullptr), router);

  mcrouter_client_t *client = mcrouter_client_new(
    router,
    (mcrouter_client_callbacks_t){on_reply, nullptr, nullptr},
    nullptr,
    0);
  EXPECT_NE(static_cast<mcrouter_client_t*>(nullptr), client);

  const char parent_key[] = "libmcrouter_test:fork:parent";
  const char child_key[] = "libmcrouter_test:fork:child";

  vector<McMsgRef> preqs;
  vector<McReply> preplies;
  preqs.push_back(new_get_req(parent_key));
  mcrouter_send_helper(client, preqs, preplies);

  mcrouter_client_disconnect(client);

  int fds[2];
  pipe(fds);
  folly::SingletonVault::singleton()->destroyInstances();
  pid_t pid = fork();
  folly::SingletonVault::singleton()->reenableInstances();

  router = mcrouter_init(persistence_id, opts);
  EXPECT_NE(static_cast<mcrouter_t*>(nullptr), router);

  client = mcrouter_client_new(
    router,
    (mcrouter_client_callbacks_t){on_reply, nullptr, nullptr},
    nullptr,
    0);
  EXPECT_NE(static_cast<mcrouter_client_t*>(nullptr), client);

  vector<McMsgRef> reqs;
  vector<McReply> replies;
  if (pid) { // parent
    close(fds[1]);
    reqs.push_back(new_get_req(parent_key));
    mcrouter_send_helper(client, reqs, replies);
  } else {
    router = mcrouter_get(persistence_id);
    reqs.push_back(new_get_req(child_key));
    mcrouter_send_helper(client, reqs, replies);
  }
  mcrouter_client_disconnect(client);

  if (pid) { // parent
    char buf[100];
    read(fds[0], buf, sizeof(buf));
    close(fds[0]);
    waitpid(pid, nullptr, 0);
  } else {
    write(fds[1], "done", 4);
    close(fds[1]);
    exit(0);
  }
}

TEST(mcrouter, already_replied_failed_delete) {
  folly::test::TemporaryDirectory tmpdir("already_replied_failed_delete");
  mcrouter_client_callbacks_t cb = {
    .on_reply = &on_reply,
    .on_cancel = nullptr,
    .on_disconnect = nullptr
  };
  auto opts = defaultTestOptions();
  std::string configStr;
  EXPECT_TRUE(folly::readFile(kAlreadyRepliedConfig.data(), configStr));
  opts.config_str = configStr;
  opts.async_spool = tmpdir.path().string();
  mcrouter_t *router = mcrouter_new(opts);
  EXPECT_TRUE(router != nullptr);

  mcrouter_client_t *client = mcrouter_client_new(router, cb,
      nullptr, 0);
  EXPECT_TRUE(client != nullptr);

  vector<McMsgRef> reqs;
  vector<McReply> replies;
  reqs.push_back(new_del_req("abc"));
  mcrouter_send_helper(client, reqs, replies);

  // Give mcrouter a chance to write the log -- we don't really know when
  // the write completes because we receive the response right away.
  sleep(1);

  mcrouter_client_disconnect(client);
  mcrouter_free(router);

  // Verify the existance and contents of the file.
  vector<string> files;
  string contents;
  string prefix(R"!(["AS1.0",)!");
  string suffix(R"!(,"C",["127.0.0.1",4242,"delete abc\r\n"]])!" "\n");

  for (recursive_directory_iterator it(tmpdir.path());
       it != recursive_directory_iterator();
       ++it) {
    if (!is_directory(it->symlink_status())) {
      files.push_back(it->path().string());
    }
  }
  EXPECT_EQ(files.size(), 1);

  folly::readFile(files[0].data(), contents);
  EXPECT_GT(contents.size(), prefix.size());
  EXPECT_GT(contents.size(), suffix.size());

  auto p = mismatch(prefix.begin(), prefix.end(), contents.begin());
  EXPECT_TRUE(p.first == prefix.end());

  auto s = mismatch(suffix.rbegin(), suffix.rend(), contents.rbegin());
  EXPECT_TRUE(s.first == suffix.rend());
}
