#include "keyed_mutex/d3d11_test_support.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <exception>
#include <iostream>
#include <mutex>
#include <numeric>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using keyed_mutex::test::AdapterName;
using keyed_mutex::test::ComPtr;
using keyed_mutex::test::CreateDevice;
using keyed_mutex::test::CreateSharedTextureOwner;
using keyed_mutex::test::DeviceBundle;
using keyed_mutex::test::HResultText;
using keyed_mutex::test::OpenSharedTexture;
using keyed_mutex::test::SelectHardwareAdapter;
using keyed_mutex::test::SharedTextureEndpoint;
using keyed_mutex::test::ThrowIfFailed;

namespace {

constexpr std::size_t kContenderCount = 3;
constexpr UINT kTextureWidth = 8;
constexpr UINT kTextureHeight = 8;
constexpr UINT kBytesPerPixel = 4;

struct Options {
  std::uint32_t rounds = 1'000;
  DWORD timeoutMs = 5'000;
  DWORD pollTimeoutMs = 50;
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
    if ((argument == "--rounds" || argument == "--timeout-ms" ||
         argument == "--poll-timeout-ms") &&
        index + 1 < arguments.size()) {
      const auto parsed = ParseUint(arguments[++index]);
      if (!parsed || *parsed == 0) {
        throw std::invalid_argument(std::string(argument) +
                                    " requires a positive integer");
      }
      if (argument == "--rounds") {
        options.rounds = *parsed;
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

struct FailureState {
  std::atomic<bool> failed = false;
  std::mutex messageMutex;
  std::string message;

  void Fail(std::string failure) {
    bool expected = false;
    if (failed.compare_exchange_strong(expected, true)) {
      std::scoped_lock lock(messageMutex);
      message = std::move(failure);
    }
  }
};

[[nodiscard]] std::vector<std::uint8_t>
MakeSignature(std::uint32_t round, std::uint8_t contender) {
  std::vector<std::uint8_t> pixels(
      static_cast<std::size_t>(kTextureWidth) * kTextureHeight *
      kBytesPerPixel);
  for (UINT y = 0; y < kTextureHeight; ++y) {
    for (UINT x = 0; x < kTextureWidth; ++x) {
      const std::size_t offset =
          (static_cast<std::size_t>(y) * kTextureWidth + x) * kBytesPerPixel;
      pixels[offset + 0] = static_cast<std::uint8_t>(round & 0xffu);
      pixels[offset + 1] = static_cast<std::uint8_t>((round >> 8u) & 0xffu);
      pixels[offset + 2] = contender;
      pixels[offset + 3] = static_cast<std::uint8_t>(
          0x80u ^ (round >> 16u) ^ (x * 23u) ^ (y * 11u));
    }
  }
  return pixels;
}

[[nodiscard]] ComPtr<ID3D11Texture2D>
CreateStagingTexture(const DeviceBundle& device,
                     const D3D11_TEXTURE2D_DESC& sharedDescription) {
  D3D11_TEXTURE2D_DESC stagingDescription = sharedDescription;
  stagingDescription.Usage = D3D11_USAGE_STAGING;
  stagingDescription.BindFlags = 0;
  stagingDescription.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  stagingDescription.MiscFlags = 0;

  ComPtr<ID3D11Texture2D> staging;
  ThrowIfFailed(device.device->CreateTexture2D(
                    &stagingDescription, nullptr, &staging),
                "ID3D11Device::CreateTexture2D(staging)");
  return staging;
}

void WriteSignature(const DeviceBundle& device,
                    ID3D11Texture2D* texture,
                    std::uint32_t round,
                    std::uint8_t contender) {
  const auto pixels = MakeSignature(round, contender);
  device.context->UpdateSubresource(texture,
                                    0,
                                    nullptr,
                                    pixels.data(),
                                    kTextureWidth * kBytesPerPixel,
                                    0);
  device.context->Flush();
}

struct ValidationResult {
  std::optional<std::string> error;
  std::uint8_t contender = 0;
};

[[nodiscard]] ValidationResult
ValidateSignature(const DeviceBundle& controller,
                  ID3D11Texture2D* sharedTexture,
                  ID3D11Texture2D* stagingTexture,
                  std::uint32_t round) {
  controller.context->CopyResource(stagingTexture, sharedTexture);

  D3D11_MAPPED_SUBRESOURCE mapped{};
  const HRESULT mapResult = controller.context->Map(
      stagingTexture, 0, D3D11_MAP_READ, 0, &mapped);
  if (FAILED(mapResult)) {
    return {"Map(staging) failed: " + HResultText(mapResult), 0};
  }

  const auto* actual = static_cast<const std::uint8_t*>(mapped.pData);
  const std::uint8_t contender = actual[2];
  std::optional<std::string> mismatch;
  if (contender >= kContenderCount) {
    mismatch = "round " + std::to_string(round) +
               " contains invalid contender id " +
               std::to_string(contender);
  } else {
    const auto expected = MakeSignature(round, contender);
    const UINT rowBytes = kTextureWidth * kBytesPerPixel;
    for (UINT y = 0; y < kTextureHeight && !mismatch; ++y) {
      const auto* actualRow =
          actual + static_cast<std::size_t>(y) * mapped.RowPitch;
      const auto* expectedRow =
          expected.data() + static_cast<std::size_t>(y) * rowBytes;
      for (UINT byte = 0; byte < rowBytes; ++byte) {
        if (actualRow[byte] != expectedRow[byte]) {
          std::ostringstream message;
          message << "round " << round << ", contender "
                  << static_cast<unsigned>(contender)
                  << " mismatch at row " << y << ", byte " << byte
                  << ": expected "
                  << static_cast<unsigned>(expectedRow[byte]) << ", got "
                  << static_cast<unsigned>(actualRow[byte]);
          mismatch = message.str();
          break;
        }
      }
    }
  }

  controller.context->Unmap(stagingTexture, 0);
  return {std::move(mismatch), contender};
}

void RunContender(
    std::size_t contenderIndex,
    const DeviceBundle& device,
    const SharedTextureEndpoint& endpoint,
    const Options& options,
    std::barrier<>& startGate,
    std::atomic<std::uint32_t>& announcedRound,
    std::atomic<bool>& stop,
    std::array<std::atomic<std::uint32_t>, kContenderCount>& wins,
    std::atomic<unsigned>& activeContenders,
    std::atomic<bool>& overlapDetected,
    FailureState& failure) {
  startGate.arrive_and_wait();

  bool ownsMutex = false;
  bool countedAsActive = false;
  try {
    while (!stop.load()) {
      const HRESULT acquire =
          endpoint.mutex->AcquireSync(1, options.pollTimeoutMs);
      if (acquire == static_cast<HRESULT>(WAIT_TIMEOUT)) {
        continue;
      }
      if (acquire == static_cast<HRESULT>(WAIT_ABANDONED)) {
        failure.Fail("contender " + std::to_string(contenderIndex) +
                     " received WAIT_ABANDONED");
        return;
      }
      if (acquire != S_OK) {
        failure.Fail("contender " + std::to_string(contenderIndex) +
                     " AcquireSync(1) failed: " + HResultText(acquire));
        return;
      }
      ownsMutex = true;

      if (activeContenders.fetch_add(1) != 0) {
        overlapDetected.store(true);
        failure.Fail("more than one contender entered the protected region");
      }
      countedAsActive = true;

      const std::uint32_t round = announcedRound.load();
      WriteSignature(device,
                     endpoint.texture.Get(),
                     round,
                     static_cast<std::uint8_t>(contenderIndex));
      wins[contenderIndex].fetch_add(1);

      activeContenders.fetch_sub(1);
      countedAsActive = false;

      const HRESULT release = endpoint.mutex->ReleaseSync(0);
      ownsMutex = false;
      if (release != S_OK) {
        failure.Fail("contender " + std::to_string(contenderIndex) +
                     " ReleaseSync(0) failed: " + HResultText(release));
        return;
      }
    }
  } catch (const std::exception& exception) {
    if (countedAsActive) {
      activeContenders.fetch_sub(1);
    }
    if (ownsMutex) {
      endpoint.mutex->ReleaseSync(0);
    }
    failure.Fail("contender " + std::to_string(contenderIndex) +
                 " exception: " + exception.what());
  }
}

[[nodiscard]] std::size_t
LongestWinningStreak(std::span<const std::uint8_t> winners) {
  std::size_t longest = 0;
  std::size_t current = 0;
  std::optional<std::uint8_t> previous;
  for (const std::uint8_t winner : winners) {
    if (previous && *previous == winner) {
      ++current;
    } else {
      previous = winner;
      current = 1;
    }
    longest = std::max(longest, current);
  }
  return longest;
}

void RunExperiment(const Options& options) {
  const auto adapter = SelectHardwareAdapter();
  const auto controller =
      CreateDevice(adapter.Get(), options.requestDebugLayer);
  std::array<DeviceBundle, kContenderCount> contenders = {
      CreateDevice(adapter.Get(), options.requestDebugLayer),
      CreateDevice(adapter.Get(), options.requestDebugLayer),
      CreateDevice(adapter.Get(), options.requestDebugLayer),
  };

  D3D11_TEXTURE2D_DESC description{};
  description.Width = kTextureWidth;
  description.Height = kTextureHeight;
  description.MipLevels = 1;
  description.ArraySize = 1;
  description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  description.SampleDesc.Count = 1;
  description.Usage = D3D11_USAGE_DEFAULT;
  description.BindFlags =
      D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
  description.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE |
                          D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;

  auto sharedOwner = CreateSharedTextureOwner(controller, description);
  std::array<SharedTextureEndpoint, kContenderCount> endpoints = {
      OpenSharedTexture(contenders[0], sharedOwner.handle.get()),
      OpenSharedTexture(contenders[1], sharedOwner.handle.get()),
      OpenSharedTexture(contenders[2], sharedOwner.handle.get()),
  };
  const auto staging = CreateStagingTexture(controller, description);

  const HRESULT initialAcquire =
      sharedOwner.endpoint.mutex->AcquireSync(0, options.timeoutMs);
  ASSERT_EQ(initialAcquire, S_OK)
      << "controller initial AcquireSync(0): "
      << HResultText(initialAcquire);

  std::barrier<> startGate(kContenderCount + 1);
  std::atomic<std::uint32_t> announcedRound = 0;
  std::atomic<bool> stop = false;
  std::array<std::atomic<std::uint32_t>, kContenderCount> wins{};
  std::atomic<unsigned> activeContenders = 0;
  std::atomic<bool> overlapDetected = false;
  FailureState failure;
  std::vector<std::thread> threads;
  threads.reserve(kContenderCount);
  for (std::size_t index = 0; index < kContenderCount; ++index) {
    threads.emplace_back([&, index] {
      RunContender(index,
                   contenders[index],
                   endpoints[index],
                   options,
                   startGate,
                   announcedRound,
                   stop,
                   wins,
                   activeContenders,
                   overlapDetected,
                   failure);
    });
  }

  bool controllerOwnsMutex = true;
  std::vector<std::uint8_t> winnerSequence;
  winnerSequence.reserve(options.rounds);
  startGate.arrive_and_wait();
  std::this_thread::sleep_for(std::chrono::milliseconds(10));

  for (std::uint32_t round = 0; round < options.rounds; ++round) {
    if (failure.failed.load()) {
      break;
    }

    announcedRound.store(round);
    const HRESULT release = sharedOwner.endpoint.mutex->ReleaseSync(1);
    if (release != S_OK) {
      failure.Fail("controller ReleaseSync(1) failed at round " +
                   std::to_string(round) + ": " + HResultText(release));
      break;
    }
    controllerOwnsMutex = false;

    const HRESULT acquire =
        sharedOwner.endpoint.mutex->AcquireSync(0, options.timeoutMs);
    if (acquire != S_OK) {
      failure.Fail("controller AcquireSync(0) failed at round " +
                   std::to_string(round) + ": " + HResultText(acquire));
      break;
    }
    controllerOwnsMutex = true;

    if (failure.failed.load()) {
      break;
    }
    auto validation = ValidateSignature(controller,
                                        sharedOwner.endpoint.texture.Get(),
                                        staging.Get(),
                                        round);
    if (validation.error) {
      failure.Fail(*validation.error);
      break;
    }
    winnerSequence.push_back(validation.contender);
  }

  stop.store(true);
  for (auto& thread : threads) {
    thread.join();
  }

  if (controllerOwnsMutex) {
    const HRESULT finalRelease = sharedOwner.endpoint.mutex->ReleaseSync(0);
    if (finalRelease != S_OK) {
      failure.Fail("controller final ReleaseSync(0) failed: " +
                   HResultText(finalRelease));
    }
  }

  if (failure.failed.load()) {
    std::scoped_lock lock(failure.messageMutex);
    FAIL() << failure.message;
  }

  ASSERT_EQ(winnerSequence.size(), options.rounds);
  EXPECT_FALSE(overlapDetected.load());
  EXPECT_EQ(activeContenders.load(), 0u);

  std::array<std::uint32_t, kContenderCount> observedWins{};
  for (const std::uint8_t winner : winnerSequence) {
    ++observedWins[winner];
  }

  std::uint32_t totalWins = 0;
  for (std::size_t index = 0; index < kContenderCount; ++index) {
    const std::uint32_t recorded = wins[index].load();
    EXPECT_EQ(recorded, observedWins[index])
        << "contender " << index << " counter differs from GPU signature";
    totalWins += recorded;
  }
  EXPECT_EQ(totalWins, options.rounds);

  std::cout << "PASS: completed " << options.rounds
            << " same-key contention rounds on " << AdapterName(adapter.Get())
            << '\n';
  for (std::size_t index = 0; index < kContenderCount; ++index) {
    std::cout << "      contender " << index << " wins: "
              << observedWins[index] << '\n';
  }
  std::cout << "      longest consecutive wins by one contender: "
            << LongestWinningStreak(winnerSequence) << '\n'
            << "      fairness is observational; no distribution is asserted\n";
}

TEST(D3D11KeyedMutex, SameKeyContention) { RunExperiment(gOptions); }

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

