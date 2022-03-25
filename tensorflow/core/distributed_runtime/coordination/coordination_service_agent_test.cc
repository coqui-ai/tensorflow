/* Copyright 2022 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "tensorflow/core/distributed_runtime/coordination/coordination_service_agent.h"

#include <memory>
#include <string>
#include <utility>

#include "absl/memory/memory.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "tensorflow/core/distributed_runtime/coordination/coordination_client.h"
#include "tensorflow/core/lib/core/status_test_util.h"
#include "tensorflow/core/platform/env.h"
#include "tensorflow/core/platform/errors.h"
#include "tensorflow/core/platform/status.h"
#include "tensorflow/core/platform/test.h"
#include "tensorflow/core/protobuf/coordination_config.pb.h"

namespace tensorflow {
namespace {
using ::testing::_;
using ::testing::DoAll;
using ::testing::InvokeArgument;
using ::testing::SetArgPointee;
using ::testing::WithArgs;

class TestCoordinationClient : public CoordinationClient {
 public:
  TestCoordinationClient() = default;
  // MOCK_METHOD does not work on Windows build, using deprecated MOCK_METHOD3
  // instead.
  MOCK_METHOD3(GetKeyValueAsync, void(const GetKeyValueRequest*,
                                      GetKeyValueResponse*, StatusCallback));
  MOCK_METHOD4(RegisterTaskAsync, void(CallOptions*, const RegisterTaskRequest*,
                                       RegisterTaskResponse*, StatusCallback));
  MOCK_METHOD3(ShutdownTaskAsync, void(const ShutdownTaskRequest*,
                                       ShutdownTaskResponse*, StatusCallback));
  MOCK_METHOD3(ResetTaskAsync, void(const ResetTaskRequest*, ResetTaskResponse*,
                                    StatusCallback));
  MOCK_METHOD3(ReportErrorToServiceAsync,
               void(const ReportErrorToServiceRequest*,
                    ReportErrorToServiceResponse*, StatusCallback));

#define UNIMPLEMENTED(method)                                         \
  void method##Async(const method##Request* request,                  \
                     method##Response* response, StatusCallback done) \
      override {                                                      \
    done(errors::Unimplemented(#method "Async"));                     \
  }

  UNIMPLEMENTED(Heartbeat);
  UNIMPLEMENTED(WaitForAllTasks);
  UNIMPLEMENTED(ReportErrorToTask);
  UNIMPLEMENTED(InsertKeyValue);
  UNIMPLEMENTED(DeleteKeyValue);
  UNIMPLEMENTED(Barrier);
  UNIMPLEMENTED(CancelBarrier);
#undef UNIMPLEMENTED
};

class CoordinationServiceAgentTest : public ::testing::Test {
 public:
  void SetUp() override {
    ON_CALL(*client_, RegisterTaskAsync(_, _, _, _))
        .WillByDefault(InvokeArgument<3>(Status::OK()));
    ON_CALL(*client_, ShutdownTaskAsync(_, _, _))
        .WillByDefault(InvokeArgument<2>(Status::OK()));
    ON_CALL(*client_, ReportErrorToServiceAsync(_, _, _))
        .WillByDefault(InvokeArgument<2>(Status::OK()));
    ON_CALL(*GetClient(), ResetTaskAsync(_, _, _))
        .WillByDefault(InvokeArgument<2>(Status::OK()));
  }

  // Should be called after mocking service responses, before testing the agent.
  void InitializeAgent() {
    CoordinationServiceConfig config;
    config.set_service_leader("test_leader");
    TF_EXPECT_OK(agent_->Initialize(
        Env::Default(), /*job_name=*/"test_job",
        /*task_id=*/0, config, std::move(client_),
        /*error_fn=*/[](Status s) {
          LOG(ERROR) << "Coordination agent is set to error: " << s;
        }));
  }

  TestCoordinationClient* GetClient() {
    // InitializeAgent() transfers ownership of the coordination client.
    CHECK(client_ != nullptr)
        << "GetClient() was called after InitializeAgent()";
    return client_.get();
  }

 protected:
  std::unique_ptr<CoordinationServiceAgent> agent_ =
      CreateCoordinationServiceAgent();
  std::unique_ptr<TestCoordinationClient> client_ =
      std::make_unique<TestCoordinationClient>();
};

TEST_F(CoordinationServiceAgentTest, GetKeyValue_Simple_Success) {
  const std::string& test_key = "test_key";
  const std::string& test_value = "test_value";
  // Mock server response: set key-value pair and invoke done callback.
  GetKeyValueResponse mocked_response;
  auto kv = mocked_response.mutable_kv();
  kv->set_key(test_key);
  kv->set_value(test_value);
  ON_CALL(*GetClient(), GetKeyValueAsync(_, _, _))
      .WillByDefault(DoAll(SetArgPointee<1>(mocked_response),
                           InvokeArgument<2>(Status::OK())));
  // Initialize coordination agent.
  InitializeAgent();

  auto result = agent_->GetKeyValue(test_key);

  TF_EXPECT_OK(result.status());
  EXPECT_EQ(result.ValueOrDie(), test_value);
}

TEST_F(CoordinationServiceAgentTest, GetKeyValue_WithTimeout_Success) {
  const std::string& test_key = "test_key";
  const std::string& test_value = "test_value";
  // Mock server response: set key-value pair and invoke done callback.
  GetKeyValueResponse mocked_response;
  auto kv = mocked_response.mutable_kv();
  kv->set_key(test_key);
  kv->set_value(test_value);
  ON_CALL(*GetClient(), GetKeyValueAsync(_, _, _))
      .WillByDefault(DoAll(SetArgPointee<1>(mocked_response),
                           InvokeArgument<2>(Status::OK())));
  // Initialize coordination agent.
  InitializeAgent();

  auto result = agent_->GetKeyValue(test_key, /*timeout=*/absl::Seconds(10));

  TF_EXPECT_OK(result.status());
  EXPECT_EQ(result.ValueOrDie(), test_value);
}

TEST_F(CoordinationServiceAgentTest, GetKeyValue_Timeout_ReturnError) {
  const std::string& test_key = "test_key";
  InitializeAgent();

  auto result = agent_->GetKeyValue(test_key, /*timeout=*/absl::Seconds(1));

  EXPECT_EQ(result.status().code(), error::DEADLINE_EXCEEDED);
}

TEST_F(CoordinationServiceAgentTest,
       GetKeyValue_DelayedResponse_TimeoutWithoutMemoryError) {
  const std::string& test_key = "test_key";
  const std::string& test_value = "test_value";
  auto client = std::make_unique<TestCoordinationClient>();
  GetKeyValueResponse* owned_response;
  StatusCallback owned_done;
  ON_CALL(*GetClient(), GetKeyValueAsync(_, _, _))
      .WillByDefault(WithArgs<1, 2>(
          [&](GetKeyValueResponse* response, StatusCallback done) {
            // Copy method arguments to prevent de-allocation before mocking the
            // server callback beyond timeout.
            owned_response = response;
            owned_done = done;
          }));
  // Initialize coordination service agent.
  InitializeAgent();

  auto result = agent_->GetKeyValue(test_key, /*timeout=*/absl::Seconds(3));
  EXPECT_EQ(result.status().code(), error::DEADLINE_EXCEEDED);

  // Delayed server response: set key-value response, and invoke done callback.
  auto kv = owned_response->mutable_kv();
  kv->set_key(test_key);
  kv->set_value(test_value);
  owned_done(Status::OK());
  // No explicit test, but used to verify there is no stack-use-after-return
  // or other memory-related errors.
}

TEST_F(CoordinationServiceAgentTest,
       GetKeyValue_DelayedResponseBeforeTimeout_Success) {
  const std::string& test_key = "test_key";
  const std::string& test_value = "test_value";
  // Mock delayed server response before timeout: set key-value pair and invoke
  // done callback.
  auto client = std::make_unique<TestCoordinationClient>();
  std::unique_ptr<Thread> async_thread;
  GetKeyValueResponse* owned_response;
  StatusCallback owned_done;
  ON_CALL(*GetClient(), GetKeyValueAsync(_, _, _))
      // Setup async callback to insert key-value after a brief delay (5s)
      // before timeout (10s).
      .WillByDefault(WithArgs<1, 2>(
          [&](GetKeyValueResponse* response, StatusCallback done) {
            // Copy method arguments to prevent de-allocation before
            //  triggering this async callback.
            owned_response = response;
            owned_done = done;
            async_thread = absl::WrapUnique(Env::Default()->StartThread(
                ThreadOptions(), "async_thread", [&]() {
                  // Set brief delay.
                  absl::SleepFor(absl::Seconds(5));
                  // Set key-value response, and invoke done callback.
                  auto kv = owned_response->mutable_kv();
                  kv->set_key(test_key);
                  kv->set_value(test_value);
                  owned_done(Status::OK());
                }));
          }));
  InitializeAgent();

  auto result = agent_->GetKeyValue(test_key, /*timeout=*/absl::Seconds(10));

  TF_EXPECT_OK(result.status());
  EXPECT_EQ(result.ValueOrDie(), test_value);
}

TEST_F(CoordinationServiceAgentTest, NotAllowedToConnectAfterShuttingDown) {
  InitializeAgent();
  TF_EXPECT_OK(agent_->Connect());

  TF_EXPECT_OK(agent_->Shutdown());
  Status status = agent_->Connect();

  // Not allowed to connect after shutting down.
  EXPECT_TRUE(errors::IsFailedPrecondition(status));
}

TEST_F(CoordinationServiceAgentTest, ShutdownInErrorShouldReturnError) {
  // Connect coordination agent and set it to error.
  InitializeAgent();
  TF_EXPECT_OK(agent_->Connect());
  TF_EXPECT_OK(agent_->ReportError(errors::Internal("Test Error.")));

  // Shutdown should return error.
  Status s = agent_->Shutdown();

  EXPECT_TRUE(errors::IsFailedPrecondition(s));
}

TEST_F(CoordinationServiceAgentTest, Reset_ConnectedButNotInError_Fail) {
  // Connect agent.
  InitializeAgent();
  TF_EXPECT_OK(agent_->Connect());

  auto status = agent_->Reset();

  // Fails because agent is not in ERROR state.
  EXPECT_TRUE(errors::IsFailedPrecondition(status));
}

TEST_F(CoordinationServiceAgentTest, ConnectAfterResetError) {
  // Connect coordination agent and set it to error.
  InitializeAgent();
  TF_EXPECT_OK(agent_->Connect());
  TF_EXPECT_OK(agent_->ReportError(errors::Internal("Test Error.")));

  // Reset error.
  TF_EXPECT_OK(agent_->Reset());
  // Agent should be able to reconnect to the service after resetting.
  TF_EXPECT_OK(agent_->Connect());
}

TEST_F(CoordinationServiceAgentTest, ResetCanBeRetried) {
  // Mock reset error failing for the first time.
  EXPECT_CALL(*GetClient(), ResetTaskAsync(_, _, _))
      .WillOnce(InvokeArgument<2>(errors::Internal("Reset error")))
      .WillOnce(InvokeArgument<2>(Status::OK()));
  // Connect coordination agent and set it to error.
  InitializeAgent();
  TF_EXPECT_OK(agent_->Connect());
  TF_EXPECT_OK(agent_->ReportError(errors::Internal("Test Error.")));

  // Reset error fails for the first time.
  Status reset_status = agent_->Reset();
  EXPECT_TRUE(errors::IsInternal(reset_status));

  // Agent should be able to attempt resetting again.
  TF_EXPECT_OK(agent_->Reset());
  // Agent should be able to reconnect to the service after resetting.
  TF_EXPECT_OK(agent_->Connect());
}
}  // namespace
}  // namespace tensorflow
