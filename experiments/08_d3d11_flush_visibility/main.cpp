#include "keyed_mutex/d3d11_test_support.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <exception>
#include <iomanip>
#include <iostream>
#include <mutex>
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
using keyed_mutex::test::CreateSharedTexturePair;
using keyed_mutex::test::HResultText;
using keyed_mutex::test::SelectHardwareAdapter;
using keyed_mutex::test::ThrowIfFailed;

namespace {

constexpr UINT kTextureWidth = 64;
constexpr UINT kTextureHeight = 64;
constexpr UINT kBytesPerPixel = 4;

struct Options {
  std::uint32_t iterations = 300;
  DWORD timeoutMs = 5'000;
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
    if ((argument == "--iterations" || argument == "--timeout-ms") &&
        index + 1 < arguments.size()) {
      const auto parsed = ParseUint(arguments[++index]);
      if (!parsed || *parsed == 0) {
        throw std::invalid_argument(std::string(argument) +
                                    " requires a positive integer");
      }
      if (argument == "--iterations") {
        options.iterations = *parsed;
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

[[nodiscard]] std::vector<std::uint8_t> MakeFrame(std::uint32_t frameIndex) {
  std::vector<std::uint8_t> pixels(
      static_cast<std::size_t>(kTextureWidth) * kTextureHeight *
      kBytesPerPixel);
  for (UINT y = 0; y < kTextureHeight; ++y) {
    for (UINT x = 0; x < kTextureWidth; ++x) {
      const std::size_t offset =
          (static_cast<std::size_t>(y) * kTextureWidth + x) * kBytesPerPixel;
      pixels[offset + 0] = static_cast<std::uint8_t>(frameIndex & 0xffu);
      pixels[offset + 1] =
          static_cast<std::uint8_t>((frameIndex >> 8u) & 0xffu);
      pixels[offset + 2] = static_cast<std::uint8_t>(
          (frameIndex * 13u + x * 31u + y * 17u) & 0xffu);
      pixels[offset + 3] = 0xffu;
    }
  }
  return pixels;
}

[[nodiscard]] std::optional<std::string>
ValidateFrame(const D3D11_MAPPED_SUBRESOURCE& mapped,
              std::uint32_t frameIndex) {
  const auto expected = MakeFrame(frameIndex);
  const auto* actual = static_cast<const std::uint8_t*>(mapped.pData);
  const UINT rowBytes = kTextureWidth * kBytesPerPixel;
  for (UINT y = 0; y < kTextureHeight; ++y) {
    const auto* actualRow =
        actual + static_cast<std::size_t>(y) * mapped.RowPitch;
    const auto* expectedRow =
        expected.data() + static_cast<std::size_t>(y) * rowBytes;
    for (UINT byte = 0; byte < rowBytes; ++byte) {
      if (actualRow[byte] != expectedRow[byte]) {
        std::ostringstream message;
        message << "frame " << frameIndex << " mismatch at row " << y
                << ", byte " << byte << ": expected "
                << static_cast<unsigned>(expectedRow[byte]) << ", got "
                << static_cast<unsigned>(actualRow[byte]);
        return message.str();
      }
    }
  }
  return std::nullopt;
}

struct SharedState {
  std::atomic<bool> failed = false;
  std::mutex failureMutex;
  std::string failureMessage;
  std::atomic<std::uint32_t> matches = 0;
  std::atomic<std::uint32_t> mismatches = 0;
  std::mutex firstMismatchMutex;
  std::string firstMismatch;

  void Fail(std::string message) {
    bool expected = false;
    if (failed.compare_exchange_strong(expected, true)) {
      std::scoped_lock lock(failureMutex);
      failureMessage = std::move(message);
    }
  }

  void RecordMismatch(std::string message) {
    mismatches.fetch_add(1);
    std::scoped_lock lock(firstMismatchMutex);
    if (firstMismatch.empty()) {
      firstMismatch = std::move(message);
    }
  }
};

[[nodiscard]] std::optional<std::string>
Acquire(IDXGIKeyedMutex* mutex, UINT64 key, DWORD timeoutMs,
         std::string_view participant) {
  const HRESULT result = mutex->AcquireSync(key, timeoutMs);
  if (result == S_OK) {
    return std::nullopt;
  }
  if (result == static_cast<HRESULT>(WAIT_TIMEOUT)) {
    return std::string(participant) + " AcquireSync(" + std::to_string(key) +
           ") timed out";
  }
  if (result == static_cast<HRESULT>(WAIT_ABANDONED)) {
    return std::string(participant) + " AcquireSync(" + std::to_string(key) +
           ") returned WAIT_ABANDONED";
  }
  return std::string(participant) + " AcquireSync(" + std::to_string(key) +
         ") failed: " + HResultText(result);
}

void RunExperiment(const Options& options, bool flushAfterUpdate) {
  const auto adapter = SelectHardwareAdapter();
  auto producer = CreateDevice(adapter.Get(), options.requestDebugLayer);
  auto consumer = CreateDevice(adapter.Get(), options.requestDebugLayer);

  D3D11_TEXTURE2D_DESC sharedDescription{};
  sharedDescription.Width = kTextureWidth;
  sharedDescription.Height = kTextureHeight;
  sharedDescription.MipLevels = 1;
  sharedDescription.ArraySize = 1;
  sharedDescription.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  sharedDescription.SampleDesc.Count = 1;
  sharedDescription.Usage = D3D11_USAGE_DEFAULT;
  sharedDescription.BindFlags =
      D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
  sharedDescription.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE |
                                D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;

  auto shared =
      CreateSharedTexturePair(producer, consumer, sharedDescription);

  D3D11_TEXTURE2D_DESC stagingDescription = sharedDescription;
  stagingDescription.Usage = D3D11_USAGE_STAGING;
  stagingDescription.BindFlags = 0;
  stagingDescription.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  stagingDescription.MiscFlags = 0;

  ComPtr<ID3D11Texture2D> stagingTexture;
  ThrowIfFailed(consumer.device->CreateTexture2D(
                    &stagingDescription, nullptr, &stagingTexture),
                "consumer ID3D11Device::CreateTexture2D(staging)");

  SharedState state;
  const auto start = std::chrono::steady_clock::now();

  std::thread producerThread([&] {
    try {
      for (std::uint32_t frame = 0;
           frame < options.iterations && !state.failed.load(); ++frame) {
        if (auto error =
                Acquire(shared.ownerMutex.Get(), 0, options.timeoutMs,
                        "producer")) {
          state.Fail(*error);
          break;
        }

        const auto pixels = MakeFrame(frame);
        producer.context->UpdateSubresource(shared.ownerTexture.Get(),
                                            0,
                                            nullptr,
                                            pixels.data(),
                                            kTextureWidth * kBytesPerPixel,
                                            0);
        if (flushAfterUpdate) {
          producer.context->Flush();
        }

        const HRESULT release = shared.ownerMutex->ReleaseSync(1);
        if (FAILED(release)) {
          state.Fail("producer ReleaseSync(1) failed: " + HResultText(release));
          break;
        }
      }
    } catch (const std::exception& exception) {
      state.Fail(std::string("producer exception: ") + exception.what());
    }
  });

  std::thread consumerThread([&] {
    try {
      for (std::uint32_t frame = 0;
           frame < options.iterations && !state.failed.load(); ++frame) {
        if (auto error =
                Acquire(shared.openedMutex.Get(), 1, options.timeoutMs,
                        "consumer")) {
          state.Fail(*error);
          break;
        }

        consumer.context->CopyResource(stagingTexture.Get(),
                                       shared.openedTexture.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        const HRESULT mapResult = consumer.context->Map(
            stagingTexture.Get(), 0, D3D11_MAP_READ, 0, &mapped);
        if (FAILED(mapResult)) {
          state.Fail("consumer Map(staging) failed: " + HResultText(mapResult));
        } else {
          if (auto mismatch = ValidateFrame(mapped, frame)) {
            state.RecordMismatch(*mismatch);
          } else {
            state.matches.fetch_add(1);
          }
          consumer.context->Unmap(stagingTexture.Get(), 0);
        }

        const HRESULT release = shared.openedMutex->ReleaseSync(0);
        if (FAILED(release)) {
          state.Fail("consumer ReleaseSync(0) failed: " + HResultText(release));
          break;
        }
      }
    } catch (const std::exception& exception) {
      state.Fail(std::string("consumer exception: ") + exception.what());
    }
  });

  producerThread.join();
  consumerThread.join();

  const auto elapsed = std::chrono::duration<double, std::milli>(
      std::chrono::steady_clock::now() - start);

  if (state.failed.load()) {
    std::scoped_lock lock(state.failureMutex);
    FAIL() << state.failureMessage;
  }

  if (flushAfterUpdate) {
    EXPECT_EQ(state.mismatches.load(), 0u);
  }
  std::cout << "PASS: " << (flushAfterUpdate ? "with Flush" : "without Flush")
            << " transferred " << options.iterations << " frames on "
            << AdapterName(adapter.Get()) << " in " << std::fixed
            << std::setprecision(2) << elapsed.count() << " ms\n"
            << "      matching frames: " << state.matches.load() << '\n'
            << "      mismatching frames: " << state.mismatches.load() << '\n';
  if (state.mismatches.load() != 0) {
    std::scoped_lock lock(state.firstMismatchMutex);
    std::cout << "      first mismatch: " << state.firstMismatch << '\n';
  }
  std::cout << "      observation only: "
            << (flushAfterUpdate ? "no; mismatches fail the baseline"
                                  : "yes; mismatches are recorded only")
            << '\n';
}

TEST(D3D11KeyedMutex, SharedTextureVisibilityWithFlush) {
  RunExperiment(gOptions, true);
}

TEST(D3D11KeyedMutex, SharedTextureVisibilityWithoutFlushObservation) {
  RunExperiment(gOptions, false);
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
