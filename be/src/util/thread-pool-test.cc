// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include <boost/thread.hpp>
#include <boost/thread/mutex.hpp>
#include <glog/logging.h>
#include <unistd.h>

#include "common/logging.h"
#include "testutil/gtest-util.h"
#include "util/thread-pool.h"

#include "common/names.h"

namespace impala {

const int NUM_THREADS = 5;
int thread_counters[NUM_THREADS];

// Per-thread mutex to ensure visibility of counters after thread pool terminates
mutex thread_mutexes[NUM_THREADS];

void Count(int thread_id, const int& i) {
  lock_guard<mutex> l(thread_mutexes[thread_id]);
  thread_counters[thread_id] += i;
}

TEST(ThreadPoolTest, BasicTest) {
  const int OFFERED_RANGE = 10000;
  for (int i = 0; i < NUM_THREADS; ++i) {
    thread_counters[i] = 0;
  }

  ThreadPool<int> thread_pool("thread-pool", "worker", 5, 250, Count);
  ASSERT_OK(thread_pool.Init());
  for (int i = 0; i <= OFFERED_RANGE; ++i) {
    ASSERT_TRUE(thread_pool.Offer(i));
  }

  thread_pool.DrainAndShutdown();

  // Check that Offer() after Shutdown() will return false
  ASSERT_FALSE(thread_pool.Offer(-1));
  EXPECT_EQ(0, thread_pool.GetQueueSize());

  int expected_count = (OFFERED_RANGE * (OFFERED_RANGE + 1)) / 2;
  int count = 0;
  for (int i = 0; i < NUM_THREADS; ++i) {
    lock_guard<mutex> l(thread_mutexes[i]);
    LOG(INFO) << "Counter " << i << ": " << thread_counters[i];
    count += thread_counters[i];
  }

  EXPECT_EQ(expected_count, count);
}

class SleepWorkItem : public SynchronousWorkItem {
public:
  SleepWorkItem(int64_t timeout_ms, bool* destructor_called)
    : timeout_ms_(timeout_ms), destructor_called_ptr_(destructor_called) {
    *destructor_called_ptr_ = false;
  }

  ~SleepWorkItem() {
    *destructor_called_ptr_ = true;
  }

  virtual Status Execute() override {
    if (timeout_ms_ > 0) SleepForMs(timeout_ms_);
    message_ = "Done";
    return Status::OK();
  }
  virtual std::string GetDescription() override {
    return Substitute("Simple task with $0 millisecond timeout", timeout_ms_);
  }

  std::string GetMessage() { return message_; }
private:
  std::string message_ = "Not done";
  int64_t timeout_ms_;
  bool* destructor_called_ptr_;
};

TEST(ThreadPoolTest, SynchronousThreadPoolTest) {
  // Create a synchronous pool with one thread and a queue size of one.
  SynchronousThreadPool pool("sync-thread-pool", "worker", 1, 1);
  ASSERT_OK(pool.Init());

  // Base case: work item takes no time, run it with a timeout of 5 milliseconds
  unique_ptr<bool> no_sleep_destroyed(new bool);
  std::shared_ptr<SleepWorkItem> no_sleep(new SleepWorkItem(0, no_sleep_destroyed.get()));
  ASSERT_OK(pool.SynchronousOffer(no_sleep, 5));
  ASSERT_EQ(no_sleep->GetMessage(), "Done");
  // If the SynchronousOffer() completed successfully, the thread pool does not have any
  // shared_ptr to the work item. The caller is the only holder, so when it calls
  // reset, the destructor must be called.
  no_sleep.reset();
  ASSERT_TRUE(*no_sleep_destroyed);

  // Timeout case #1: Submit one task that takes 100 milliseconds. Offer it with a timeout
  // of 1 millisecond so that the caller immediately times out.
  unique_ptr<bool> long_sleep_destroyed(new bool);
  std::shared_ptr<SleepWorkItem> long_sleep(
      new SleepWorkItem(100, long_sleep_destroyed.get()));
  Status timeout_status = pool.SynchronousOffer(long_sleep, 1);
  ASSERT_EQ(timeout_status.code(), TErrorCode::THREAD_POOL_TASK_TIMED_OUT);
  // The work item is still running, and even if the caller releases its shared_ptr,
  // the work item is not destroyed.
  long_sleep.reset();
  ASSERT_FALSE(*long_sleep_destroyed);

  // The single thread in the thread pool is still running. Submit another task
  // that will queue. The task doesn't matter.
  unique_ptr<bool> queued_task_destroyed(new bool);
  std::shared_ptr<SleepWorkItem> queued_task(
      new SleepWorkItem(0, queued_task_destroyed.get()));
  Status queued_task_status = pool.SynchronousOffer(queued_task, 1);
  ASSERT_EQ(queued_task_status.code(), TErrorCode::THREAD_POOL_TASK_TIMED_OUT);
  // The work item is queued, and even if the caller releases its shared_ptr, the work
  // item is not destroyed.
  queued_task.reset();
  ASSERT_FALSE(*queued_task_destroyed);

  // Now, the queue is full. Any new task will fail to submit.
  unique_ptr<bool> fail_to_submit_destroyed(new bool);
  std::shared_ptr<SleepWorkItem> fail_to_submit(
      new SleepWorkItem(0, fail_to_submit_destroyed.get()));
  Status fail_to_submit_status = pool.SynchronousOffer(fail_to_submit, 1);
  ASSERT_EQ(fail_to_submit_status.code(), TErrorCode::THREAD_POOL_SUBMIT_FAILED);
  // When the submit fails, the thread pool does not keep any shared_ptr to the work
  // item. When the caller releases its shared_ptr, the work item is immediately
  // destroyed.
  fail_to_submit.reset();
  ASSERT_TRUE(*fail_to_submit_destroyed);

  // The tasks will still complete
  pool.DrainAndShutdown();
  // The work items that the thread pool had running and in the queue are destroyed
  // when they complete (even though the caller long since released its shared_ptr).
  ASSERT_TRUE(*long_sleep_destroyed);
  ASSERT_TRUE(*queued_task_destroyed);
}

}

IMPALA_TEST_MAIN();
