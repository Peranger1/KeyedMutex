#include "keyed_mutex/d3d11_test_support.hpp"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <barrier>
#include <charconv>
#include <cstdint>
#include <exception>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

using keyed_mutex::test::AdapterName;
using keyed_mutex::test::CreateDevice;
using keyed_mutex::test::CreateSharedTextureOwner;
using keyed_mutex::test::DeviceBundle;
using keyed_mutex::test::HResultText;
using keyed_mutex::test::OpenSharedTexture;
using keyed_mutex::test::SelectHardwareAdapter;
using keyed_mutex::test::SharedTextureEndpoint;
using keyed_mutex::test::SharedTextureOwner;
using keyed_mutex::test::ThrowIfFailed;

namespace {

struct Options {
  std::uint32_t rounds = 100;
  DWORD timeoutMs = 250;
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
    if ((argument == "--rounds" || argument == "--timeout-ms") &&
        index + 1 < arguments.size()) {
      const auto parsed = ParseUint(arguments[++index]);
      if (!parsed || *parsed == 0) {
        throw std::invalid_argument(std::string(argument) +
                                    " requires a positive integer");
      }
      if (argument == "--rounds") {
        options.rounds = *parsed;
      } else {
        options.timeoutMs = *parsed;
      }
      continue;
    }
    throw std::invalid_argument("unknown or incomplete argument: " +
                                std::string(argument));
  }
  return options;
}

[[nodiscard]] D3D11_TEXTURE2D_DESC SharedDescription() {
  D3D11_TEXTURE2D_DESC description{};
  description.Width = 1;
  description.Height = 1;
  description.MipLevels = 1;
  description.ArraySize = 1;
  description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  description.SampleDesc.Count = 1;
  description.Usage = D3D11_USAGE_DEFAULT;
  description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  description.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE |
                          D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
  return description;
}

struct DualResources {
  SharedTextureOwner ownerA;
  SharedTextureOwner ownerB;
  SharedTextureEndpoint openerA;
  SharedTextureEndpoint openerB;
};

[[nodiscard]] DualResources CreateDualResources(const DeviceBundle& owner,
                                                const DeviceBundle& opener) {
  const auto description = SharedDescription();
  DualResources resources;
  resources.ownerA = CreateSharedTextureOwner(owner, description);
  resources.ownerB = CreateSharedTextureOwner(owner, description);
  resources.openerA = OpenSharedTexture(opener, resources.ownerA.handle.get());
  resources.openerB = OpenSharedTexture(opener, resources.ownerB.handle.get());
  return resources;
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
};

struct ParticipantResult {
  HRESULT firstAcquire = E_UNEXPECTED;
  HRESULT secondAcquire = E_UNEXPECTED;
  HRESULT secondRelease = E_UNEXPECTED;
  HRESULT firstRelease = E_UNEXPECTED;
  std::uint32_t completedRounds = 0;
};

void RunSameOrder(const Options& options) {
  const auto adapter = SelectHardwareAdapter();
  const auto owner = CreateDevice(adapter.Get(), options.requestDebugLayer);
  const auto opener = CreateDevice(adapter.Get(), options.requestDebugLayer);
  auto resources = CreateDualResources(owner, opener);

  std::barrier<> startGate(2);
  FailureState failure;
  std::array<ParticipantResult, 2> results;
  std::array<std::thread, 2> threads;

  for (std::size_t participant = 0; participant < threads.size();
       ++participant) {
    threads[participant] = std::thread([&, participant] {
      IDXGIKeyedMutex* mutexA = participant == 0
                                    ? resources.ownerA.endpoint.mutex.Get()
                                    : resources.openerA.mutex.Get();
      IDXGIKeyedMutex* mutexB = participant == 0
                                    ? resources.ownerB.endpoint.mutex.Get()
                                    : resources.openerB.mutex.Get();
      auto& result = results[participant];
      startGate.arrive_and_wait();
      for (std::uint32_t round = 0;
           round < options.rounds && !failure.failed.load(); ++round) {
        bool heldA = false;
        bool heldB = false;
        try {
          result.firstAcquire = mutexA->AcquireSync(0, options.timeoutMs);
          if (result.firstAcquire != S_OK) {
            failure.Fail("same-order participant " +
                         std::to_string(participant) +
                         " AcquireSync(A) failed: " +
                         HResultText(result.firstAcquire));
            break;
          }
          heldA = true;

          result.secondAcquire = mutexB->AcquireSync(0, options.timeoutMs);
          if (result.secondAcquire != S_OK) {
            failure.Fail("same-order participant " +
                         std::to_string(participant) +
                         " AcquireSync(B) failed: " +
                         HResultText(result.secondAcquire));
            break;
          }
          heldB = true;

          result.secondRelease = mutexB->ReleaseSync(0);
          heldB = false;
          if (result.secondRelease != S_OK) {
            failure.Fail("same-order participant " +
                         std::to_string(participant) +
                         " ReleaseSync(B) failed: " +
                         HResultText(result.secondRelease));
            break;
          }
          result.firstRelease = mutexA->ReleaseSync(0);
          heldA = false;
          if (result.firstRelease != S_OK) {
            failure.Fail("same-order participant " +
                         std::to_string(participant) +
                         " ReleaseSync(A) failed: " +
                         HResultText(result.firstRelease));
            break;
          }
          ++result.completedRounds;
        } catch (const std::exception& exception) {
          failure.Fail("same-order participant " +
                       std::to_string(participant) + " exception: " +
                       exception.what());
        }
        if (heldB) {
          mutexB->ReleaseSync(0);
        }
        if (heldA) {
          mutexA->ReleaseSync(0);
        }
      }
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }

  if (failure.failed.load()) {
    std::scoped_lock lock(failure.mutex);
    FAIL() << failure.message;
  }
  for (std::size_t participant = 0; participant < results.size();
       ++participant) {
    EXPECT_EQ(results[participant].completedRounds, options.rounds)
        << "participant " << participant;
  }
  std::cout << "PASS: same-order A->B locking completed " << options.rounds
            << " rounds per participant on " << AdapterName(adapter.Get())
            << '\n';
}

void RunReverseOrder(const Options& options) {
  const auto adapter = SelectHardwareAdapter();
  const auto owner = CreateDevice(adapter.Get(), options.requestDebugLayer);
  const auto opener = CreateDevice(adapter.Get(), options.requestDebugLayer);
  auto resources = CreateDualResources(owner, opener);

  std::barrier<> firstLocksHeld(2);
  FailureState failure;
  std::array<ParticipantResult, 2> results;
  std::array<std::thread, 2> threads;

  threads[0] = std::thread([&] {
    auto& result = results[0];
    bool heldA = false;
    bool heldB = false;
    result.firstAcquire = resources.ownerA.endpoint.mutex->AcquireSync(
        0, options.timeoutMs);
    heldA = result.firstAcquire == S_OK;
    firstLocksHeld.arrive_and_wait();
    result.secondAcquire = resources.ownerB.endpoint.mutex->AcquireSync(
        0, options.timeoutMs);
    heldB = result.secondAcquire == S_OK;
    if (heldB) {
      result.secondRelease = resources.ownerB.endpoint.mutex->ReleaseSync(0);
      heldB = false;
    }
    if (heldA) {
      result.firstRelease = resources.ownerA.endpoint.mutex->ReleaseSync(0);
      heldA = false;
    }
    if (heldB) {
      resources.ownerB.endpoint.mutex->ReleaseSync(0);
    }
    if (heldA) {
      resources.ownerA.endpoint.mutex->ReleaseSync(0);
    }
  });

  threads[1] = std::thread([&] {
    auto& result = results[1];
    bool heldA = false;
    bool heldB = false;
    result.firstAcquire = resources.openerB.mutex->AcquireSync(
        0, options.timeoutMs);
    heldB = result.firstAcquire == S_OK;
    firstLocksHeld.arrive_and_wait();
    result.secondAcquire = resources.openerA.mutex->AcquireSync(
        0, options.timeoutMs);
    heldA = result.secondAcquire == S_OK;
    if (heldA) {
      result.secondRelease = resources.openerA.mutex->ReleaseSync(0);
      heldA = false;
    }
    if (heldB) {
      result.firstRelease = resources.openerB.mutex->ReleaseSync(0);
      heldB = false;
    }
    if (heldA) {
      resources.openerA.mutex->ReleaseSync(0);
    }
    if (heldB) {
      resources.openerB.mutex->ReleaseSync(0);
    }
  });

  for (auto& thread : threads) {
    thread.join();
  }

  ASSERT_EQ(results[0].firstAcquire, S_OK)
      << "participant 0 first AcquireSync(A): "
      << HResultText(results[0].firstAcquire);
  ASSERT_EQ(results[1].firstAcquire, S_OK)
      << "participant 1 first AcquireSync(B): "
      << HResultText(results[1].firstAcquire);
  EXPECT_EQ(results[0].secondAcquire, static_cast<HRESULT>(WAIT_TIMEOUT))
      << "participant 0 reverse AcquireSync(B): "
      << HResultText(results[0].secondAcquire);
  EXPECT_EQ(results[1].secondAcquire, static_cast<HRESULT>(WAIT_TIMEOUT))
      << "participant 1 reverse AcquireSync(A): "
      << HResultText(results[1].secondAcquire);

  const HRESULT recoveryA =
      resources.ownerA.endpoint.mutex->AcquireSync(0, options.timeoutMs);
  ASSERT_EQ(recoveryA, S_OK)
      << "recovery AcquireSync(A): " << HResultText(recoveryA);
  const HRESULT recoveryB =
      resources.ownerB.endpoint.mutex->AcquireSync(0, options.timeoutMs);
  ASSERT_EQ(recoveryB, S_OK)
      << "recovery AcquireSync(B): " << HResultText(recoveryB);
  EXPECT_EQ(resources.ownerB.endpoint.mutex->ReleaseSync(0), S_OK);
  EXPECT_EQ(resources.ownerA.endpoint.mutex->ReleaseSync(0), S_OK);

  std::cout << "PASS: reverse-order A->B / B->A waiters both timed out after "
            << options.timeoutMs
            << " ms and recovered with canonical A->B acquisition on "
            << AdapterName(adapter.Get()) << '\n';
}

TEST(D3D11KeyedMutex, TwoResourceSameOrderNoDeadlock) {
  RunSameOrder(gOptions);
}

TEST(D3D11KeyedMutex, TwoResourceReverseOrderTimeoutRecovery) {
  RunReverseOrder(gOptions);
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
