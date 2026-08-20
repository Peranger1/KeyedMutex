#include "keyed_mutex/d3d11_test_support.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <barrier>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <optional>
#include <semaphore>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using keyed_mutex::test::AdapterName;
using keyed_mutex::test::CreateDevice;
using keyed_mutex::test::CreateSharedTextureOwner;
using keyed_mutex::test::DeviceBundle;
using keyed_mutex::test::HResultText;
using keyed_mutex::test::SelectHardwareAdapter;
using keyed_mutex::test::SharedTextureOwner;

namespace {

struct Options {
  std::uint32_t rounds = 100;
  std::uint32_t waiters = 8;
  DWORD timeoutMs = 1'000;
  DWORD pollTimeoutMs = 25;
  bool requestDebugLayer = true;
};

Options gOptions;

[[nodiscard]] std::optional<std::uint32_t> ParseUint(std::string_view value) {
  std::uint32_t result = 0;
  const auto parse =
      std::from_chars(value.data(), value.data() + value.size(), result);
  if (parse.ec != std::errc{} || parse.ptr != value.data() + value.size()) {
    return std::nullopt;
  }
  return result;
}

[[nodiscard]] Options ParseOptions(std::span<char*> arguments) {
  Options options;
  for (std::size_t index = 1; index < arguments.size(); ++index) {
    const std::string_view argument(arguments[index]);
    if (argument == "--no-debug-layer") {
      options.requestDebugLayer = false;
      continue;
    }
    if ((argument == "--rounds" || argument == "--waiters" ||
         argument == "--timeout-ms" || argument == "--poll-timeout-ms") &&
        index + 1 < arguments.size()) {
      const auto parsed = ParseUint(arguments[++index]);
      if (!parsed || *parsed == 0) {
        throw std::invalid_argument(std::string(argument) +
                                    " requires a positive integer");
      }
      if (argument == "--rounds") {
        options.rounds = *parsed;
      } else if (argument == "--waiters") {
        options.waiters = *parsed;
      } else if (argument == "--timeout-ms") {
        options.timeoutMs = *parsed;
      } else {
        options.pollTimeoutMs = *parsed;
      }
      continue;
    }
    throw std::invalid_argument("unknown or incomplete argument: " +
                                std::string(argument));
  }
  return options;
}

[[nodiscard]] D3D11_TEXTURE2D_DESC SharedTextureDescription() {
  D3D11_TEXTURE2D_DESC description{};
  description.Width = 1;
  description.Height = 1;
  description.MipLevels = 1;
  description.ArraySize = 1;
  description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  description.SampleDesc.Count = 1;
  description.Usage = D3D11_USAGE_DEFAULT;
  description.BindFlags =
      D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
  description.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE |
                          D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
  return description;
}

[[nodiscard]] SharedTextureOwner CreateTestResource(
    const DeviceBundle& device) {
  return CreateSharedTextureOwner(device, SharedTextureDescription());
}

void UpdateMax(std::atomic<std::uint64_t>& target, std::uint64_t value) {
  auto current = target.load();
  while (current < value &&
         !target.compare_exchange_weak(current, value)) {
  }
}

void RunCrossThreadHandoff(const Options& options) {
  const auto adapter = SelectHardwareAdapter();
  const auto device = CreateDevice(adapter.Get(), options.requestDebugLayer);
  auto resource = CreateTestResource(device);
  auto* mutex = resource.endpoint.mutex.Get();

  HRESULT acquireA = E_UNEXPECTED;
  HRESULT releaseA = E_UNEXPECTED;
  HRESULT acquireB = E_UNEXPECTED;
  HRESULT releaseB = E_UNEXPECTED;
  HRESULT recoveryAcquireA = E_UNEXPECTED;
  HRESULT recoveryReleaseA = E_UNEXPECTED;
  std::binary_semaphore acquired(0);
  std::binary_semaphore readyForAcquire(0);
  std::binary_semaphore allowRelease(0);
  std::binary_semaphore attemptFinished(0);

  std::thread threadA([&] {
    acquireA = mutex->AcquireSync(0, options.timeoutMs);
    acquired.release();
    if (acquireA == S_OK) {
      readyForAcquire.acquire();
      allowRelease.acquire();
      releaseA = mutex->ReleaseSync(1);
      attemptFinished.acquire();
      if (acquireB != S_OK) {
        recoveryAcquireA = mutex->AcquireSync(1, options.timeoutMs);
        if (recoveryAcquireA == S_OK) {
          recoveryReleaseA = mutex->ReleaseSync(0);
        }
      }
    }
  });
  std::thread threadB([&] {
    acquired.acquire();
    if (acquireA == S_OK) {
      readyForAcquire.release();
      allowRelease.release();
      acquireB = mutex->AcquireSync(1, options.timeoutMs);
      if (acquireB == S_OK) {
        releaseB = mutex->ReleaseSync(0);
      }
      attemptFinished.release();
    }
  });
  threadA.join();
  threadB.join();

  EXPECT_EQ(acquireA, S_OK) << HResultText(acquireA);
  EXPECT_EQ(releaseA, S_OK) << HResultText(releaseA);
  ASSERT_TRUE(acquireB == S_OK || FAILED(acquireB))
      << "unexpected cross-thread AcquireSync result: "
      << HResultText(acquireB);
  if (acquireB == S_OK) {
    EXPECT_EQ(releaseB, S_OK) << HResultText(releaseB);
    std::cout << "OBSERVED: same interface cross-thread handoff accepted\n";
  } else {
    std::cout << "OBSERVED: same interface cross-thread AcquireSync rejected: "
              << HResultText(acquireB) << '\n';
    EXPECT_EQ(recoveryAcquireA, S_OK) << HResultText(recoveryAcquireA);
    EXPECT_EQ(recoveryReleaseA, S_OK) << HResultText(recoveryReleaseA);
  }
  std::cout << "PASS: same-interface cross-thread probe on "
            << AdapterName(adapter.Get()) << '\n';
}

struct MigrationResult {
  HRESULT acquireA = E_UNEXPECTED;
  HRESULT releaseB = E_UNEXPECTED;
  HRESULT cleanupA = E_UNEXPECTED;
  HRESULT acquireAfter = E_UNEXPECTED;
  HRESULT releaseAfter = E_UNEXPECTED;
};

MigrationResult RunMigrationCase(const DeviceBundle& device,
                                 const Options& options) {
  auto resource = CreateTestResource(device);
  auto* mutex = resource.endpoint.mutex.Get();
  MigrationResult result;
  std::mutex stateMutex;
  std::condition_variable stateChanged;
  bool acquired = false;
  bool migrationAttempted = false;

  std::thread ownerThread([&] {
    result.acquireA = mutex->AcquireSync(0, options.timeoutMs);
    {
      std::scoped_lock lock(stateMutex);
      acquired = true;
    }
    stateChanged.notify_all();
    std::unique_lock lock(stateMutex);
    stateChanged.wait(lock, [&] { return migrationAttempted; });
    lock.unlock();
    if (result.acquireA == S_OK && result.releaseB != S_OK) {
      result.cleanupA = mutex->ReleaseSync(1);
    }
  });
  std::thread migratingThread([&] {
    std::unique_lock lock(stateMutex);
    stateChanged.wait(lock, [&] { return acquired; });
    lock.unlock();
    if (result.acquireA == S_OK) {
      result.releaseB = mutex->ReleaseSync(1);
    }
    {
      std::scoped_lock doneLock(stateMutex);
      migrationAttempted = true;
    }
    stateChanged.notify_all();
  });
  ownerThread.join();
  migratingThread.join();
  result.acquireAfter = mutex->AcquireSync(1, options.timeoutMs);
  if (result.acquireAfter == S_OK) {
    result.releaseAfter = mutex->ReleaseSync(0);
  }
  return result;
}

void RunOwnershipMigration(const Options& options) {
  const auto adapter = SelectHardwareAdapter();
  const auto device = CreateDevice(adapter.Get(), options.requestDebugLayer);
  const auto result = RunMigrationCase(device, options);

  ASSERT_EQ(result.acquireA, S_OK) << HResultText(result.acquireA);
  ASSERT_TRUE(result.releaseB == S_OK || FAILED(result.releaseB))
      << HResultText(result.releaseB);
  if (result.releaseB == S_OK) {
    std::cout << "OBSERVED: cross-thread ownership migration accepted\n";
    EXPECT_EQ(result.cleanupA, E_UNEXPECTED);
  } else {
    std::cout << "OBSERVED: cross-thread ownership migration rejected: "
              << HResultText(result.releaseB) << '\n';
    EXPECT_EQ(result.cleanupA, S_OK) << HResultText(result.cleanupA);
  }
  EXPECT_EQ(result.acquireAfter, S_OK) << HResultText(result.acquireAfter);
  EXPECT_EQ(result.releaseAfter, S_OK) << HResultText(result.releaseAfter);
}

struct FailureState {
  std::atomic<bool> failed = false;
  std::mutex mutex;
  std::string message;

  void Fail(std::string text) {
    bool expected = false;
    if (failed.compare_exchange_strong(expected, true)) {
      std::scoped_lock lock(mutex);
      message = std::move(text);
    }
  }

  void Unsupported(std::string text) {
    bool expected = false;
    if (failed.compare_exchange_strong(expected, true)) {
      std::scoped_lock lock(mutex);
      message = std::move(text);
    }
  }
};

void RunConcurrentWaiters(const Options& options) {
  const auto adapter = SelectHardwareAdapter();
  const auto device = CreateDevice(adapter.Get(), options.requestDebugLayer);
  auto resource = CreateTestResource(device);
  auto* mutex = resource.endpoint.mutex.Get();
  ASSERT_EQ(mutex->AcquireSync(0, options.timeoutMs), S_OK);

  std::barrier<> startGate(static_cast<std::ptrdiff_t>(options.waiters) + 1);
  std::atomic<bool> stop = false;
  FailureState failure;
  std::atomic<unsigned> active = 0;
  std::atomic<bool> overlap = false;
  std::atomic<std::uint32_t> completed = 0;
  std::atomic<std::uint64_t> totalWaitUs = 0;
  std::atomic<std::uint64_t> maxWaitUs = 0;
  std::vector<std::atomic<std::uint32_t>> wins(options.waiters);
  for (auto& count : wins) count.store(0);

  std::vector<std::thread> threads;
  for (std::uint32_t index = 0; index < options.waiters; ++index) {
    threads.emplace_back([&, index] {
      startGate.arrive_and_wait();
      bool owns = false;
      bool counted = false;
      try {
        while (!stop.load() && !failure.failed.load()) {
          const auto start = std::chrono::steady_clock::now();
          const HRESULT acquire =
              mutex->AcquireSync(1, options.pollTimeoutMs);
          const auto elapsed = std::chrono::duration_cast<
              std::chrono::microseconds>(std::chrono::steady_clock::now() -
                                         start)
                                    .count();
          if (acquire == static_cast<HRESULT>(WAIT_TIMEOUT)) continue;
          if (acquire == static_cast<HRESULT>(WAIT_ABANDONED)) {
            failure.Fail("waiter received WAIT_ABANDONED");
            return;
          }
          if (acquire == DXGI_ERROR_INVALID_CALL) {
            failure.Unsupported(
                "same-interface concurrent AcquireSync was rejected: " +
                HResultText(acquire));
            return;
          }
          if (acquire != S_OK) {
            failure.Fail("waiter AcquireSync(1) failed: " +
                         HResultText(acquire));
            return;
          }
          owns = true;
          totalWaitUs.fetch_add(static_cast<std::uint64_t>(elapsed));
          UpdateMax(maxWaitUs, static_cast<std::uint64_t>(elapsed));
          if (active.fetch_add(1) != 0) {
            overlap.store(true);
            failure.Fail("more than one waiter entered at once");
          }
          counted = true;
          wins[index].fetch_add(1);
          completed.fetch_add(1);
          const HRESULT release = mutex->ReleaseSync(0);
          owns = false;
          active.fetch_sub(1);
          counted = false;
          if (release != S_OK) {
            failure.Fail("waiter ReleaseSync(0) failed: " +
                         HResultText(release));
            return;
          }
        }
      } catch (const std::exception& exception) {
        if (counted) active.fetch_sub(1);
        if (owns) mutex->ReleaseSync(0);
        failure.Fail(std::string("waiter exception: ") + exception.what());
      }
    });
  }

  startGate.arrive_and_wait();
  bool controllerOwns = true;
  for (std::uint32_t round = 0;
       round < options.rounds && !failure.failed.load(); ++round) {
    const HRESULT release = mutex->ReleaseSync(1);
    if (release != S_OK) {
      failure.Fail("controller ReleaseSync(1) failed: " +
                   HResultText(release));
      controllerOwns = false;
      break;
    }
    controllerOwns = false;
    const HRESULT acquire = mutex->AcquireSync(0, options.timeoutMs);
    if (acquire != S_OK) {
      failure.Fail("controller AcquireSync(0) failed: " +
                   HResultText(acquire));
      break;
    }
    controllerOwns = true;
  }
  stop.store(true);
  for (auto& thread : threads) thread.join();
  if (controllerOwns) mutex->ReleaseSync(0);

  if (failure.failed.load()) {
    std::scoped_lock lock(failure.mutex);
    if (failure.message.find("same-interface concurrent") !=
        std::string::npos) {
      std::cout << "OBSERVED: " << failure.message << '\n'
                << "PASS: concurrent-waiter support probe completed on "
                << AdapterName(adapter.Get()) << '\n';
      return;
    }
    FAIL() << failure.message;
  }
  EXPECT_EQ(completed.load(), options.rounds);
  EXPECT_FALSE(overlap.load());
  EXPECT_EQ(active.load(), 0u);
  const double average = completed.load() == 0
                             ? 0.0
                             : static_cast<double>(totalWaitUs.load()) /
                                   static_cast<double>(completed.load());
  std::cout << "PASS: " << options.waiters << " same-interface waiters, "
            << options.rounds << " rounds on " << AdapterName(adapter.Get())
            << "\n      average wait: " << std::fixed << std::setprecision(2)
            << average << " us\n      maximum wait: " << maxWaitUs.load()
            << " us\n";
  for (std::uint32_t index = 0; index < options.waiters; ++index) {
    std::cout << "      waiter " << index << " wins: " << wins[index].load()
              << '\n';
  }
}

TEST(D3D11KeyedMutex, SameInterfaceCrossThreadHandoff) {
  RunCrossThreadHandoff(gOptions);
}

TEST(D3D11KeyedMutex, OwnershipThreadMigration) {
  RunOwnershipMigration(gOptions);
}

TEST(D3D11KeyedMutex, SameInterfaceConcurrentWaiters) {
  RunConcurrentWaiters(gOptions);
}

}  // namespace

int main(int argc, char* argv[]) {
  ::testing::InitGoogleTest(&argc, argv);
  try {
    gOptions = ParseOptions(std::span(argv, static_cast<std::size_t>(argc)));
  } catch (const std::exception& exception) {
    std::cerr << "ERROR: " << exception.what() << '\n';
    return 2;
  }
  return RUN_ALL_TESTS();
}
