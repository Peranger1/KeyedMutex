#include "keyed_mutex/d3d11_test_support.hpp"

#include <d3d11_4.h>

#include <gtest/gtest.h>

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
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using keyed_mutex::test::AdapterName;
using keyed_mutex::test::ComPtr;
using keyed_mutex::test::CreateDevice;
using keyed_mutex::test::DeviceBundle;
using keyed_mutex::test::HResultText;
using keyed_mutex::test::SelectHardwareAdapter;
using keyed_mutex::test::ThrowIfFailed;

namespace {

constexpr UINT kTextureWidth = 32;
constexpr UINT kTextureHeight = 32;
constexpr UINT kBytesPerPixel = 4;

struct Options {
  std::uint32_t workers = 4;
  std::uint32_t iterations = 100;
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
    if ((argument == "--workers" || argument == "--iterations") &&
        index + 1 < arguments.size()) {
      const auto parsed = ParseUint(arguments[++index]);
      if (!parsed || *parsed == 0) {
        throw std::invalid_argument(std::string(argument) +
                                    " requires a positive integer");
      }
      if (argument == "--workers") {
        options.workers = *parsed;
      } else {
        options.iterations = *parsed;
      }
      continue;
    }
    throw std::invalid_argument("unknown or incomplete argument: " +
                                std::string(argument));
  }
  return options;
}

[[nodiscard]] D3D11_TEXTURE2D_DESC TextureDescription() {
  D3D11_TEXTURE2D_DESC description{};
  description.Width = kTextureWidth;
  description.Height = kTextureHeight;
  description.MipLevels = 1;
  description.ArraySize = 1;
  description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  description.SampleDesc.Count = 1;
  description.Usage = D3D11_USAGE_DEFAULT;
  description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  return description;
}

[[nodiscard]] D3D11_TEXTURE2D_DESC StagingDescription() {
  auto description = TextureDescription();
  description.Usage = D3D11_USAGE_STAGING;
  description.BindFlags = 0;
  description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  return description;
}

[[nodiscard]] std::vector<std::uint8_t> MakeFrame(std::uint32_t worker,
                                                   std::uint32_t iteration) {
  std::vector<std::uint8_t> pixels(
      static_cast<std::size_t>(kTextureWidth) * kTextureHeight *
      kBytesPerPixel);
  for (UINT y = 0; y < kTextureHeight; ++y) {
    for (UINT x = 0; x < kTextureWidth; ++x) {
      const std::size_t offset =
          (static_cast<std::size_t>(y) * kTextureWidth + x) * kBytesPerPixel;
      pixels[offset + 0] = static_cast<std::uint8_t>(worker);
      pixels[offset + 1] = static_cast<std::uint8_t>(iteration & 0xffu);
      pixels[offset + 2] = static_cast<std::uint8_t>(
          (iteration * 17u + worker * 53u + x * 11u + y * 7u) & 0xffu);
      pixels[offset + 3] = 0xffu;
    }
  }
  return pixels;
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

struct Observation {
  std::uint32_t completed = 0;
  std::uint32_t mismatchedTextures = 0;
  std::string firstMismatch;
  std::optional<std::string> failure;
};

[[nodiscard]] std::optional<std::string>
ValidateTexture(const DeviceBundle& device,
                ID3D11Texture2D* texture,
                ID3D11Texture2D* staging,
                std::uint32_t worker,
                std::uint32_t iteration) {
  device.context->CopyResource(staging, texture);
  D3D11_MAPPED_SUBRESOURCE mapped{};
  const HRESULT mapResult =
      device.context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped);
  if (FAILED(mapResult)) {
    return "Map(staging) failed: " + HResultText(mapResult);
  }

  const auto expected = MakeFrame(worker, iteration);
  const auto* actual = static_cast<const std::uint8_t*>(mapped.pData);
  const UINT rowBytes = kTextureWidth * kBytesPerPixel;
  std::optional<std::string> mismatch;
  for (UINT y = 0; y < kTextureHeight && !mismatch; ++y) {
    const auto* actualRow =
        actual + static_cast<std::size_t>(y) * mapped.RowPitch;
    const auto* expectedRow =
        expected.data() + static_cast<std::size_t>(y) * rowBytes;
    for (UINT byte = 0; byte < rowBytes; ++byte) {
      if (actualRow[byte] != expectedRow[byte]) {
        std::ostringstream message;
        message << "worker " << worker << ", iteration " << iteration
                << " mismatch at row " << y << ", byte " << byte
                << ": expected " << static_cast<unsigned>(expectedRow[byte])
                << ", got " << static_cast<unsigned>(actualRow[byte]);
        mismatch = message.str();
        break;
      }
    }
  }
  device.context->Unmap(staging, 0);
  return mismatch;
}

Observation RunExperiment(const Options& options, bool protectedContext) {
  const auto adapter = SelectHardwareAdapter();
  const auto device = CreateDevice(adapter.Get(), options.requestDebugLayer);

  ComPtr<ID3D11Multithread> multithread;
  ThrowIfFailed(device.context.As(&multithread),
                "QueryInterface(ID3D11Multithread)");
  multithread->SetMultithreadProtected(protectedContext ? TRUE : FALSE);

  std::vector<ComPtr<ID3D11Texture2D>> textures(options.workers);
  const auto description = TextureDescription();
  for (auto& texture : textures) {
    ThrowIfFailed(device.device->CreateTexture2D(&description, nullptr,
                                                  &texture),
                  "ID3D11Device::CreateTexture2D(worker texture)");
  }

  std::barrier<> startGate(static_cast<std::ptrdiff_t>(options.workers));
  FailureState failure;
  std::atomic<std::uint32_t> completed = 0;
  std::vector<std::thread> threads;
  threads.reserve(options.workers);
  for (std::uint32_t worker = 0; worker < options.workers; ++worker) {
    threads.emplace_back([&, worker] {
      try {
        startGate.arrive_and_wait();
        for (std::uint32_t iteration = 0;
             iteration < options.iterations && !failure.failed.load();
             ++iteration) {
          const auto pixels = MakeFrame(worker, iteration);
          device.context->UpdateSubresource(textures[worker].Get(),
                                             0,
                                             nullptr,
                                             pixels.data(),
                                             kTextureWidth * kBytesPerPixel,
                                             0);
          device.context->Flush();
          completed.fetch_add(1);
        }
      } catch (const std::exception& exception) {
        failure.Fail(std::string("worker ") + std::to_string(worker) +
                     " exception: " + exception.what());
      }
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }

  Observation observation;
  observation.completed = completed.load();
  if (failure.failed.load()) {
    std::scoped_lock lock(failure.mutex);
    observation.failure = failure.message;
    return observation;
  }

  ComPtr<ID3D11Texture2D> staging;
  const auto stagingDescription = StagingDescription();
  ThrowIfFailed(device.device->CreateTexture2D(&stagingDescription, nullptr,
                                                &staging),
                "ID3D11Device::CreateTexture2D(staging)");
  for (std::uint32_t worker = 0; worker < options.workers; ++worker) {
    if (auto mismatch = ValidateTexture(device,
                                        textures[worker].Get(),
                                        staging.Get(),
                                        worker,
                                        options.iterations - 1)) {
      ++observation.mismatchedTextures;
      if (observation.firstMismatch.empty()) {
        observation.firstMismatch = *mismatch;
      }
    }
  }
  return observation;
}

void ReportObservation(std::string_view mode,
                       const Options& options,
                       const Observation& observation,
                       const std::string& adapterName) {
  std::cout << "RESULT: ID3D11Multithread " << mode << " on " << adapterName
            << '\n'
            << "      completed UpdateSubresource/Flush calls: "
            << observation.completed << " / "
            << options.workers * options.iterations << '\n'
            << "      mismatched final textures: "
            << observation.mismatchedTextures << " / " << options.workers
            << '\n';
  if (observation.failure) {
    std::cout << "      worker failure: " << *observation.failure << '\n';
  }
  if (!observation.firstMismatch.empty()) {
    std::cout << "      first mismatch: " << observation.firstMismatch << '\n';
  }
}

void RunProtectedBaseline(const Options& options) {
  const auto adapter = SelectHardwareAdapter();
  const auto observation = RunExperiment(options, true);
  ReportObservation("protected=TRUE", options, observation,
                    AdapterName(adapter.Get()));
  ASSERT_FALSE(observation.failure)
      << (observation.failure ? *observation.failure : "");
  EXPECT_EQ(observation.completed, options.workers * options.iterations);
  EXPECT_EQ(observation.mismatchedTextures, 0u);
}

void RunUnprotectedObservation(const Options& options) {
  const auto adapter = SelectHardwareAdapter();
  const auto observation = RunExperiment(options, false);
  ReportObservation("protected=FALSE", options, observation,
                    AdapterName(adapter.Get()));
  if (observation.failure) {
    std::cout << "      observation only: unprotected execution reported a "
                 "worker failure\n";
  } else {
    std::cout << "      observation only: unprotected execution completed; "
                 "this is not a thread-safety guarantee\n";
  }
}

TEST(D3D11Multithread, ProtectedImmediateContextBaseline) {
  RunProtectedBaseline(gOptions);
}

TEST(D3D11Multithread, UnprotectedImmediateContextObservation) {
  RunUnprotectedObservation(gOptions);
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
