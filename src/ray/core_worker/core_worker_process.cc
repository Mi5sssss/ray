// Copyright 2017 The Ray Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//  http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "ray/core_worker/core_worker.h"
#include "ray/stats/stats.h"
#include "ray/util/event.h"
#include "ray/util/util.h"

namespace ray {
namespace core {
namespace {

std::unique_ptr<CoreWorkerProcessImpl> core_worker_process;

}  // namespace

void CoreWorkerProcess::Initialize(const CoreWorkerOptions &options) {
  RAY_CHECK(!core_worker_process)
      << "The process is already initialized for core worker.";
  core_worker_process.reset(new CoreWorkerProcessImpl(options));

#ifndef _WIN32
  // NOTE(kfstorm): std::atexit should be put at the end of `CoreWorkerProcess`
  // constructor. We assume that spdlog has been initialized before this line. When the
  // process is exiting, `HandleAtExit` will be invoked before destructing spdlog static
  // variables. We explicitly destruct `CoreWorkerProcess` instance in the callback to
  // ensure the static `CoreWorkerProcess` instance is destructed while spdlog is still
  // usable. This prevents crashing (or hanging) when using `RAY_LOG` in
  // `CoreWorkerProcess` destructor.
  RAY_CHECK(std::atexit(CoreWorkerProcess::HandleAtExit) == 0);
#endif
}

void CoreWorkerProcess::Shutdown() {
  RAY_LOG(DEBUG) << "Shutdown. Core worker process will be deleted";
  if (!core_worker_process) {
    return;
  }
  core_worker_process->ShutdownDriver();
  core_worker_process.reset();
}

bool CoreWorkerProcess::IsInitialized() { return core_worker_process != nullptr; }

void CoreWorkerProcess::HandleAtExit() { core_worker_process.reset(); }

CoreWorker &CoreWorkerProcess::GetCoreWorker() {
  EnsureInitialized(/*quick_exit*/ true);
  return core_worker_process->GetCoreWorkerForCurrentThread();
}

void CoreWorkerProcess::SetCurrentThreadWorkerId(const WorkerID &worker_id) {
  EnsureInitialized(/*quick_exit*/ false);
  core_worker_process->SetThreadLocalWorkerById(worker_id);
}

void CoreWorkerProcess::RunTaskExecutionLoop() {
  EnsureInitialized(/*quick_exit*/ false);
  core_worker_process->RunWorkerTaskExecutionLoop();
  core_worker_process.reset();
}

std::shared_ptr<CoreWorker> CoreWorkerProcess::TryGetWorker(const WorkerID &worker_id) {
  if (!core_worker_process) {
    return nullptr;
  }
  return core_worker_process->GetWorker(worker_id);
}

thread_local std::weak_ptr<CoreWorker> CoreWorkerProcessImpl::thread_local_core_worker_;

CoreWorkerProcessImpl::CoreWorkerProcessImpl(const CoreWorkerOptions &options)
    : options_(options),
      global_worker_id_(
          options.worker_type == WorkerType::DRIVER
              ? ComputeDriverIdFromJob(options_.job_id)
              : (options_.num_workers == 1 ? WorkerID::FromRandom() : WorkerID::Nil())) {
  if (options_.enable_logging) {
    std::stringstream app_name;
    app_name << LanguageString(options_.language) << "-core-"
             << WorkerTypeString(options_.worker_type);
    if (!global_worker_id_.IsNil()) {
      app_name << "-" << global_worker_id_;
    }
    RayLog::StartRayLog(app_name.str(), RayLogLevel::INFO, options_.log_dir);
    if (options_.install_failure_signal_handler) {
      // Core worker is loaded as a dynamic library from Python or other languages.
      // We are not sure if the default argv[0] would be suitable for loading symbols
      // so leaving it unspecified as nullptr. This could make symbolization of crash
      // traces fail in some circumstances.
      //
      // Also, call the previous crash handler, e.g. the one installed by the Python
      // worker.
      RayLog::InstallFailureSignalHandler(nullptr, /*call_previous_handler=*/true);
    }
  } else {
    RAY_CHECK(options_.log_dir.empty())
        << "log_dir must be empty because ray log is disabled.";
    RAY_CHECK(!options_.install_failure_signal_handler)
        << "install_failure_signal_handler must be false because ray log is disabled.";
  }

  RAY_CHECK(options_.num_workers > 0);
  if (options_.worker_type == WorkerType::DRIVER) {
    // Driver process can only contain one worker.
    RAY_CHECK(options_.num_workers == 1);
  }

  RAY_LOG(INFO) << "Constructing CoreWorkerProcess. pid: " << getpid();

  // NOTE(kfstorm): any initialization depending on RayConfig must happen after this line.
  InitializeSystemConfig();

  if (ShouldCreateGlobalWorkerOnConstruction()) {
    CreateWorker();
  }

  // Assume stats module will be initialized exactly once in once process.
  // So it must be called in CoreWorkerProcess constructor and will be reused
  // by all of core worker.
  RAY_LOG(DEBUG) << "Stats setup in core worker.";
  // Initialize stats in core worker global tags.
  const ray::stats::TagsType global_tags = {
      {ray::stats::ComponentKey, "core_worker"},
      {ray::stats::VersionKey, kRayVersion},
      {ray::stats::NodeAddressKey, options_.node_ip_address}};

  // NOTE(lingxuan.zlx): We assume RayConfig is initialized before it's used.
  // RayConfig is generated in Java_io_ray_runtime_RayNativeRuntime_nativeInitialize
  // for java worker or in constructor of CoreWorker for python worker.
  stats::Init(global_tags, options_.metrics_agent_port);

  // Initialize event framework.
  if (RayConfig::instance().event_log_reporter_enabled() && !options_.log_dir.empty()) {
    RayEventInit(ray::rpc::Event_SourceType::Event_SourceType_CORE_WORKER,
                 std::unordered_map<std::string, std::string>(), options_.log_dir,
                 RayConfig::instance().event_level());
  }
}

CoreWorkerProcessImpl::~CoreWorkerProcessImpl() {
  RAY_LOG(INFO) << "Destructing CoreWorkerProcessImpl. pid: " << getpid();
  RAY_LOG(DEBUG) << "Stats stop in core worker.";
  // Shutdown stats module if worker process exits.
  stats::Shutdown();
  if (options_.enable_logging) {
    RayLog::ShutDownRayLog();
  }
}

void CoreWorkerProcess::EnsureInitialized(bool quick_exit) {
  if (core_worker_process != nullptr) {
    return;
  }

  if (quick_exit) {
    RAY_LOG(WARNING) << "The core worker process is not initialized yet or already "
                     << "shutdown.";
    QuickExit();
  } else {
    RAY_CHECK(core_worker_process)
        << "The core worker process is not initialized yet or already "
        << "shutdown.";
  }
}

void CoreWorkerProcessImpl::InitializeSystemConfig() {
  // We have to create a short-time thread here because the RPC request to get the system
  // config from Raylet is asynchronous, and we need to synchronously initialize the
  // system config in the constructor of `CoreWorkerProcessImpl`.
  std::promise<std::string> promise;
  std::thread thread([&] {
    instrumented_io_context io_service;
    boost::asio::io_service::work work(io_service);
    rpc::ClientCallManager client_call_manager(io_service);
    auto grpc_client = rpc::NodeManagerWorkerClient::make(
        options_.raylet_ip_address, options_.node_manager_port, client_call_manager);
    raylet::RayletClient raylet_client(grpc_client);

    std::function<void(int64_t)> get_once = [this, &get_once, &raylet_client, &promise,
                                             &io_service](int64_t num_attempts) {
      raylet_client.GetSystemConfig(
          [this, num_attempts, &get_once, &promise, &io_service](
              const Status &status, const rpc::GetSystemConfigReply &reply) {
            RAY_LOG(DEBUG) << "Getting system config from raylet, remaining retries = "
                           << num_attempts;
            if (status.ok()) {
              promise.set_value(reply.system_config());
              io_service.stop();
              return;
            }

            if (num_attempts > 1) {
              std::this_thread::sleep_for(std::chrono::milliseconds(
                  RayConfig::instance().raylet_client_connect_timeout_milliseconds()));
              get_once(num_attempts - 1);
              return;
            }

            // If there's no more attempt to try.
            if (IsRayletFailed(RayConfig::instance().RAYLET_PID())) {
              std::ostringstream ss;
              ss << "Failed to get the system config from raylet because "
                 << "it is dead. Worker will terminate. Status: " << status;
              if (options_.worker_type == WorkerType::DRIVER) {
                // If it is the driver, surface the issue to the user.
                RAY_LOG(ERROR) << ss.str();
              } else {
                RAY_LOG(WARNING) << ss.str();
              }
              QuickExit();
            }

            // Unexpected.
            RAY_LOG(FATAL)
                << "Failed to get the system config from Raylet on time unexpectedly."
                << status;
          });
    };

    get_once(RayConfig::instance().raylet_client_num_connect_attempts());
    io_service.run();
  });
  thread.join();

  RayConfig::instance().initialize(promise.get_future().get());
}

bool CoreWorkerProcessImpl::ShouldCreateGlobalWorkerOnConstruction() const {
  // We need to create the worker instance here if:
  // 1. This is a driver process. In this case, the driver is ready to use right after
  // the CoreWorkerProcess::Initialize.
  // 2. This is a Python worker process. In this case, Python will invoke some core
  // worker APIs before `CoreWorkerProcess::RunTaskExecutionLoop` is called. So we need
  // to create the worker instance here. One example of invocations is
  // https://github.com/ray-project/ray/blob/45ce40e5d44801193220d2c546be8de0feeef988/python/ray/worker.py#L1281.
  return options_.num_workers == 1 && (options_.worker_type == WorkerType::DRIVER ||
                                       options_.language == Language::PYTHON);
}

std::shared_ptr<CoreWorker> CoreWorkerProcessImpl::GetWorker(
    const WorkerID &worker_id) const {
  absl::ReaderMutexLock lock(&mutex_);
  auto it = workers_.find(worker_id);
  if (it != workers_.end()) {
    return it->second;
  }
  return nullptr;
}

std::shared_ptr<CoreWorker> CoreWorkerProcessImpl::GetGlobalWorker() {
  absl::ReaderMutexLock lock(&mutex_);
  return global_worker_;
}

std::shared_ptr<CoreWorker> CoreWorkerProcessImpl::CreateWorker() {
  auto worker = std::make_shared<CoreWorker>(
      options_,
      global_worker_id_ != WorkerID::Nil() ? global_worker_id_ : WorkerID::FromRandom());
  RAY_LOG(DEBUG) << "Worker " << worker->GetWorkerID() << " is created.";
  absl::WriterMutexLock lock(&mutex_);
  if (options_.num_workers == 1) {
    global_worker_ = worker;
  }
  thread_local_core_worker_ = worker;

  workers_.emplace(worker->GetWorkerID(), worker);
  RAY_CHECK(workers_.size() <= static_cast<size_t>(options_.num_workers));
  return worker;
}

void CoreWorkerProcessImpl::RemoveWorker(std::shared_ptr<CoreWorker> worker) {
  worker->WaitForShutdown();
  absl::WriterMutexLock lock(&mutex_);
  if (global_worker_) {
    RAY_CHECK(global_worker_ == worker);
  } else {
    RAY_CHECK(thread_local_core_worker_.lock() == worker);
  }
  thread_local_core_worker_.reset();
  {
    workers_.erase(worker->GetWorkerID());
    RAY_LOG(INFO) << "Removed worker " << worker->GetWorkerID();
  }
  if (global_worker_ == worker) {
    global_worker_ = nullptr;
  }
}

void CoreWorkerProcessImpl::RunWorkerTaskExecutionLoop() {
  RAY_CHECK(options_.worker_type == WorkerType::WORKER);
  if (options_.num_workers == 1) {
    // Run the task loop in the current thread only if the number of workers is 1.
    auto worker = GetGlobalWorker();
    if (!worker) {
      worker = CreateWorker();
    }
    worker->RunTaskExecutionLoop();
    RAY_LOG(DEBUG) << "Task execution loop terminated. Removing the global worker.";
    RemoveWorker(worker);
  } else {
    std::vector<std::thread> worker_threads;
    for (int i = 0; i < options_.num_workers; i++) {
      worker_threads.emplace_back([this, i] {
        SetThreadName("worker.task" + std::to_string(i));
        auto worker = CreateWorker();
        worker->RunTaskExecutionLoop();
        RAY_LOG(INFO) << "Task execution loop terminated for a thread "
                      << std::to_string(i) << ". Removing a worker.";
        RemoveWorker(worker);
      });
    }
    for (auto &thread : worker_threads) {
      thread.join();
    }
  }
}

void CoreWorkerProcessImpl::ShutdownDriver() {
  RAY_CHECK(options_.worker_type == WorkerType::DRIVER)
      << "The `Shutdown` interface is for driver only.";
  auto global_worker = GetGlobalWorker();
  RAY_CHECK(global_worker);
  global_worker->Disconnect();
  global_worker->Shutdown();
  RemoveWorker(global_worker);
}

CoreWorker &CoreWorkerProcessImpl::GetCoreWorkerForCurrentThread() {
  if (options_.num_workers == 1) {
    auto global_worker = GetGlobalWorker();
    if (ShouldCreateGlobalWorkerOnConstruction() && !global_worker) {
      // This could only happen when the worker has already been shutdown.
      // In this case, we should exit without crashing.
      // TODO (scv119): A better solution could be returning error code
      // and handling it at language frontend.
      RAY_LOG(ERROR) << "The global worker has already been shutdown. This happens when "
                        "the language frontend accesses the Ray's worker after it is "
                        "shutdown. The process will exit";
      QuickExit();
    }
    RAY_CHECK(global_worker) << "global_worker_ must not be NULL";
    return *global_worker;
  }
  auto ptr = thread_local_core_worker_.lock();
  RAY_CHECK(ptr != nullptr)
      << "The current thread is not bound with a core worker instance.";
  return *ptr;
}

void CoreWorkerProcessImpl::SetThreadLocalWorkerById(const WorkerID &worker_id) {
  if (options_.num_workers == 1) {
    RAY_CHECK(GetGlobalWorker()->GetWorkerID() == worker_id);
    return;
  }
  auto worker = GetWorker(worker_id);
  RAY_CHECK(worker) << "Worker " << worker_id << " not found.";
  thread_local_core_worker_ = GetWorker(worker_id);
}

}  // namespace core
}  // namespace ray