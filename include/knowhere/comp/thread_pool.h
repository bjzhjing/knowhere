// Copyright (C) 2019-2023 Zilliz. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except in compliance
// with the License. You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied. See the License for the specific language governing permissions and limitations under the License

#pragma once

#include <omp.h>

#ifdef __linux__
#include <sys/resource.h>
#if __GLIBC__ == 2 && __GLIBC_MINOR__ < 30
#include <sys/syscall.h>
#define gettid() syscall(SYS_gettid)
#endif
#endif

#include <cerrno>
#include <cstring>
#include <memory>
#include <thread>
#include <utility>

#include "folly/executors/CPUThreadPoolExecutor.h"
#include "folly/futures/Future.h"
#include "knowhere/expected.h"
#include "knowhere/log.h"

namespace knowhere {

class ThreadPool {
#ifdef __linux__
 private:
    class LowPriorityThreadFactory : public folly::NamedThreadFactory {
     public:
        using folly::NamedThreadFactory::NamedThreadFactory;
        std::thread
        newThread(folly::Func&& func) override {
            return folly::NamedThreadFactory::newThread([&, func = std::move(func)]() mutable {
                if (setpriority(PRIO_PROCESS, gettid(), 19) != 0) {
                    LOG_KNOWHERE_ERROR_ << "Failed to set priority of knowhere thread. Error is: "
                                        << std::strerror(errno);
                } else {
                    LOG_KNOWHERE_INFO_ << "Successfully set priority of knowhere thread.";
                }
                func();
            });
        }
    };

 public:
    explicit ThreadPool(uint32_t num_threads, const std::string& thread_name_prefix)
        : pool_(folly::CPUThreadPoolExecutor(
              num_threads,
              std::make_unique<
                  folly::LifoSemMPMCQueue<folly::CPUThreadPoolExecutor::CPUTask, folly::QueueBehaviorIfFull::BLOCK>>(
                  num_threads * kTaskQueueFactor),
              std::make_shared<LowPriorityThreadFactory>(thread_name_prefix))) {
    }
#else
 public:
    explicit ThreadPool(uint32_t num_threads, const std::string& thread_name_prefix)
        : pool_(folly::CPUThreadPoolExecutor(
              num_threads,
              std::make_unique<
                  folly::LifoSemMPMCQueue<folly::CPUThreadPoolExecutor::CPUTask, folly::QueueBehaviorIfFull::BLOCK>>(
                  num_threads * kTaskQueueFactor),
              std::make_shared<folly::NamedThreadFactory>(thread_name_prefix))) {
    }
#endif

    ThreadPool(const ThreadPool&) = delete;

    ThreadPool&
    operator=(const ThreadPool&) = delete;

    ThreadPool(ThreadPool&&) noexcept = delete;

    ThreadPool&
    operator=(ThreadPool&&) noexcept = delete;

    template <typename Func, typename... Args>
    auto
    push(Func&& func, Args&&... args) {
        return folly::makeSemiFuture().via(&pool_).then(
            [func = std::forward<Func>(func), &args...](auto&&) mutable { return func(std::forward<Args>(args)...); });
    }

    [[nodiscard]] int32_t
    size() const noexcept {
        return pool_.numThreads();
    }

    void
    SetNumThreads(uint32_t num_threads) {
        if (num_threads == 0) {
            LOG_KNOWHERE_ERROR_ << "set number of threads can not be 0";
            return;
        } else {
            // setNumThreads() adjust the relevant variables instead of changing the number of threads directly;
            // If numThreads < active threads, reduce number of running threads.
            pool_.setNumThreads(num_threads);
            return;
        }
    }

    static void
    InitGlobalBuildThreadPool(uint32_t num_threads) {
        if (num_threads <= 0) {
            LOG_KNOWHERE_ERROR_ << "num_threads should be bigger than 0";
            return;
        }

        if (build_pool_ == nullptr) {
            std::lock_guard<std::mutex> lock(build_pool_mutex_);
            if (build_pool_ == nullptr) {
                build_pool_ = std::make_shared<ThreadPool>(num_threads, "knowhere_build");
                LOG_KNOWHERE_INFO_ << "Init global build thread pool with size " << num_threads;
                return;
            }
        } else {
            LOG_KNOWHERE_INFO_ << "Global build thread pool size has already been initialized to "
                               << build_pool_->size();
        }
    }

    static void
    InitGlobalSearchThreadPool(uint32_t num_threads) {
        if (num_threads <= 0) {
            LOG_KNOWHERE_ERROR_ << "num_threads should be bigger than 0";
            return;
        }

        if (search_pool_ == nullptr) {
            std::lock_guard<std::mutex> lock(search_pool_mutex_);
            if (search_pool_ == nullptr) {
                search_pool_ = std::make_shared<ThreadPool>(num_threads, "knowhere_search");
                LOG_KNOWHERE_INFO_ << "Init global search thread pool with size " << num_threads;
                return;
            }
        } else {
            LOG_KNOWHERE_INFO_ << "Global search thread pool size has already been initialized to "
                               << search_pool_->size();
        }
    }

    static void
    SetGlobalBuildThreadPoolSize(uint32_t num_threads) {
        if (build_pool_ == nullptr) {
            InitGlobalBuildThreadPool(num_threads);
            return;
        } else {
            build_pool_->SetNumThreads(num_threads);
            LOG_KNOWHERE_INFO_ << "Global build thread pool size has already been set to " << build_pool_->size();
            return;
        }
    }

    static void
    SetGlobalSearchThreadPoolSize(uint32_t num_threads) {
        if (search_pool_ == nullptr) {
            InitGlobalSearchThreadPool(num_threads);
            return;
        } else {
            search_pool_->SetNumThreads(num_threads);
            LOG_KNOWHERE_INFO_ << "Global search thread pool size has already been set to " << search_pool_->size();
            return;
        }
    }

    static std::shared_ptr<ThreadPool>
    GetGlobalBuildThreadPool() {
        if (build_pool_ == nullptr) {
            InitGlobalBuildThreadPool(std::thread::hardware_concurrency());
        }
        return build_pool_;
    }

    static std::shared_ptr<ThreadPool>
    GetGlobalSearchThreadPool() {
        if (search_pool_ == nullptr) {
            InitGlobalSearchThreadPool(std::thread::hardware_concurrency());
        }
        return search_pool_;
    }

    class ScopedOmpSetter {
        int omp_before;

     public:
        explicit ScopedOmpSetter(int num_threads = 0) {
            if (build_pool_ == nullptr) {  // this should not happen in prod
                omp_before = omp_get_max_threads();
            } else {
                omp_before = build_pool_->size();
            }

            omp_set_num_threads(num_threads <= 0 ? omp_before : num_threads);
        }
        ~ScopedOmpSetter() {
            omp_set_num_threads(omp_before);
        }
    };

 private:
    folly::CPUThreadPoolExecutor pool_;

    inline static std::mutex build_pool_mutex_;
    inline static std::shared_ptr<ThreadPool> build_pool_ = nullptr;

    inline static std::mutex search_pool_mutex_;
    inline static std::shared_ptr<ThreadPool> search_pool_ = nullptr;

    constexpr static size_t kTaskQueueFactor = 16;
};

// T is either folly::Unit or Status
template <typename T>
inline Status
WaitAllSuccess(std::vector<folly::Future<T>>& futures) {
    static_assert(std::is_same<T, folly::Unit>::value || std::is_same<T, Status>::value,
                  "WaitAllSuccess can only be used with folly::Unit or knowhere::Status");
    auto allFuts = folly::collectAll(futures.begin(), futures.end()).get();
    for (const auto& result : allFuts) {
        result.throwUnlessValue();
        if constexpr (!std::is_same_v<T, folly::Unit>) {
            if (result.value() != Status::success) {
                return result.value();
            }
        }
    }
    return Status::success;
}

}  // namespace knowhere
