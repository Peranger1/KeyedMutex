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
#include <utility>
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
  std::uint32_t iterations = 1'000;
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
          (frameIndex + x * 31u + y * 17u) & 0xffu);
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
    const auto* actualRow = actual + static_cast<std::size_t>(y) * mapped.RowPitch;
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

[[nodiscard]] std::optional<std::string>
Acquire(IDXGIKeyedMutex* mutex, UINT64 key, DWORD timeoutMs) {
  const HRESULT hr = mutex->AcquireSync(key, timeoutMs);
  if (hr == S_OK) {
    return std::nullopt;
  }
  if (hr == static_cast<HRESULT>(WAIT_TIMEOUT)) {
    return "AcquireSync(" + std::to_string(key) + ") timed out";
  }
  if (hr == static_cast<HRESULT>(WAIT_ABANDONED)) {
    return "AcquireSync(" + std::to_string(key) +
           ") returned WAIT_ABANDONED; the resource must be recreated";
  }
  return "AcquireSync(" + std::to_string(key) + ") failed: " +
         HResultText(hr);
}

struct SharedExperimentState {
  std::atomic<bool> failed = false;
  std::mutex failureMutex;
  std::string failureMessage;

  void Fail(std::string message) {
    bool expected = false;
    if (failed.compare_exchange_strong(expected, true)) {
      std::scoped_lock lock(failureMutex);
      failureMessage = std::move(message);
    }
  }
};

void RunExperiment(const Options& options) {
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

  SharedExperimentState state;
  const auto start = std::chrono::steady_clock::now();

  std::thread producerThread([&] {
    try {
      for (std::uint32_t frame = 0;
           frame < options.iterations && !state.failed.load();
           ++frame) {
        if (auto error =
                Acquire(shared.ownerMutex.Get(), 0, options.timeoutMs)) {
          state.Fail("producer: " + *error);
          break;
        }

        const auto pixels = MakeFrame(frame);
        producer.context->UpdateSubresource(shared.ownerTexture.Get(),
                                            0,
                                            nullptr,
                                            pixels.data(),
                                            kTextureWidth * kBytesPerPixel,
                                            0);
        producer.context->Flush();

        const HRESULT release = shared.ownerMutex->ReleaseSync(1);
        if (FAILED(release)) {
          state.Fail("producer: ReleaseSync(1) failed: " +
                     HResultText(release));
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
           frame < options.iterations && !state.failed.load();
           ++frame) {
        if (auto error =
                Acquire(shared.openedMutex.Get(), 1, options.timeoutMs)) {
          state.Fail("consumer: " + *error);
          break;
        }

        consumer.context->CopyResource(stagingTexture.Get(),
                                       shared.openedTexture.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        const HRESULT mapResult = consumer.context->Map(
            stagingTexture.Get(), 0, D3D11_MAP_READ, 0, &mapped);
        if (FAILED(mapResult)) {
          state.Fail("consumer: Map(staging) failed: " +
                     HResultText(mapResult));
        } else {
          if (auto mismatch = ValidateFrame(mapped, frame)) {
            state.Fail("consumer: " + *mismatch);
          }
          consumer.context->Unmap(stagingTexture.Get(), 0);
        }

        const HRESULT release = shared.openedMutex->ReleaseSync(0);
        if (FAILED(release)) {
          state.Fail("consumer: ReleaseSync(0) failed: " +
                     HResultText(release));
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

  std::cout << "PASS: transferred and verified " << options.iterations
            << " frames on " << AdapterName(adapter.Get()) << " in "
            << std::fixed << std::setprecision(2) << elapsed.count() << " ms\n"
            << "      producer debug layer: "
            << (producer.debugLayerEnabled ? "enabled" : "disabled") << '\n'
            << "      consumer debug layer: "
            << (consumer.debugLayerEnabled ? "enabled" : "disabled") << '\n';
}

TEST(D3D11KeyedMutex, TwoDevicePingPong) { RunExperiment(gOptions); }

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
