#include "keyed_mutex/d3d11_test_support.hpp"

#include <algorithm>
#include <atomic>
#include <barrier>
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
using keyed_mutex::test::CreateSharedTextureOwner;
using keyed_mutex::test::DeviceBundle;
using keyed_mutex::test::HResultText;
using keyed_mutex::test::OpenSharedTexture;
using keyed_mutex::test::SelectHardwareAdapter;
using keyed_mutex::test::SharedTextureEndpoint;
using keyed_mutex::test::ThrowIfFailed;

namespace {

constexpr UINT kTextureWidth = 16;
constexpr UINT kTextureHeight = 16;
constexpr UINT kBytesPerPixel = 4;
constexpr std::uint8_t kStageA = 0xA1;
constexpr std::uint8_t kStageB = 0xB2;
constexpr std::uint8_t kStageC = 0xC3;

struct Options {
  std::uint32_t iterations = 1'000;
  DWORD timeoutMs = 5'000;
  bool requestDebugLayer = true;
};

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

struct WaitStats {
  std::uint64_t turns = 0;
  double totalMicroseconds = 0.0;
  double maximumMicroseconds = 0.0;

  void Record(std::chrono::steady_clock::duration duration) {
    const double microseconds =
        std::chrono::duration<double, std::micro>(duration).count();
    ++turns;
    totalMicroseconds += microseconds;
    maximumMicroseconds = std::max(maximumMicroseconds, microseconds);
  }
};

[[nodiscard]] std::vector<std::uint8_t>
MakeSignature(std::uint32_t frame, std::uint8_t stage) {
  std::vector<std::uint8_t> pixels(
      static_cast<std::size_t>(kTextureWidth) * kTextureHeight *
      kBytesPerPixel);
  for (UINT y = 0; y < kTextureHeight; ++y) {
    for (UINT x = 0; x < kTextureWidth; ++x) {
      const std::size_t offset =
          (static_cast<std::size_t>(y) * kTextureWidth + x) * kBytesPerPixel;
      pixels[offset + 0] = static_cast<std::uint8_t>(frame & 0xffu);
      pixels[offset + 1] = static_cast<std::uint8_t>((frame >> 8u) & 0xffu);
      pixels[offset + 2] = static_cast<std::uint8_t>(
          stage ^ ((frame >> 16u) + x * 29u + y * 13u));
      pixels[offset + 3] = stage;
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
                    std::uint32_t frame,
                    std::uint8_t stage) {
  const auto pixels = MakeSignature(frame, stage);
  device.context->UpdateSubresource(texture,
                                    0,
                                    nullptr,
                                    pixels.data(),
                                    kTextureWidth * kBytesPerPixel,
                                    0);
  device.context->Flush();
}

[[nodiscard]] std::optional<std::string>
ValidateSignature(const DeviceBundle& device,
                  ID3D11Texture2D* sharedTexture,
                  ID3D11Texture2D* stagingTexture,
                  std::uint32_t frame,
                  std::uint8_t stage) {
  device.context->CopyResource(stagingTexture, sharedTexture);

  D3D11_MAPPED_SUBRESOURCE mapped{};
  const HRESULT mapResult = device.context->Map(
      stagingTexture, 0, D3D11_MAP_READ, 0, &mapped);
  if (FAILED(mapResult)) {
    return "Map(staging) failed: " + HResultText(mapResult);
  }

  const auto expected = MakeSignature(frame, stage);
  const auto* actual = static_cast<const std::uint8_t*>(mapped.pData);
  const UINT rowBytes = kTextureWidth * kBytesPerPixel;
  std::optional<std::string> mismatch;
  for (UINT y = 0; y < kTextureHeight && !mismatch; ++y) {
    const auto* actualRow = actual + static_cast<std::size_t>(y) * mapped.RowPitch;
    const auto* expectedRow =
        expected.data() + static_cast<std::size_t>(y) * rowBytes;
    for (UINT byte = 0; byte < rowBytes; ++byte) {
      if (actualRow[byte] != expectedRow[byte]) {
        std::ostringstream message;
        message << "frame " << frame << ", stage 0x" << std::hex
                << static_cast<unsigned>(stage) << std::dec
                << " mismatch at row " << y << ", byte " << byte
                << ": expected " << static_cast<unsigned>(expectedRow[byte])
                << ", got " << static_cast<unsigned>(actualRow[byte]);
        mismatch = message.str();
        break;
      }
    }
  }
  device.context->Unmap(stagingTexture, 0);
  return mismatch;
}

[[nodiscard]] bool AcquireTurn(std::string_view name,
                               IDXGIKeyedMutex* mutex,
                               UINT64 key,
                               DWORD timeoutMs,
                               FailureState& failure,
                               WaitStats& stats) {
  const auto start = std::chrono::steady_clock::now();
  const HRESULT result = mutex->AcquireSync(key, timeoutMs);
  const auto elapsed = std::chrono::steady_clock::now() - start;
  if (result == S_OK) {
    stats.Record(elapsed);
    return true;
  }

  if (result == static_cast<HRESULT>(WAIT_TIMEOUT)) {
    failure.Fail(std::string(name) + ": AcquireSync(" +
                 std::to_string(key) + ") timed out");
  } else if (result == static_cast<HRESULT>(WAIT_ABANDONED)) {
    failure.Fail(std::string(name) + ": AcquireSync(" +
                 std::to_string(key) + ") returned WAIT_ABANDONED");
  } else {
    failure.Fail(std::string(name) + ": AcquireSync(" +
                 std::to_string(key) + ") failed: " + HResultText(result));
  }
  return false;
}

[[nodiscard]] bool ReleaseTurn(std::string_view name,
                               IDXGIKeyedMutex* mutex,
                               UINT64 key,
                               FailureState& failure) {
  const HRESULT result = mutex->ReleaseSync(key);
  if (result == S_OK) {
    return true;
  }
  failure.Fail(std::string(name) + ": ReleaseSync(" +
               std::to_string(key) + ") failed: " + HResultText(result));
  return false;
}

void RunStage(std::string_view name,
              const DeviceBundle& device,
              const SharedTextureEndpoint& endpoint,
              ID3D11Texture2D* staging,
              UINT64 acquireKey,
              UINT64 releaseKey,
              std::uint8_t expectedStage,
              std::uint8_t writtenStage,
              bool expectedFrameIsPrevious,
              const Options& options,
              std::barrier<>& startGate,
              FailureState& failure,
              WaitStats& stats) {
  startGate.arrive_and_wait();

  for (std::uint32_t frame = 0; frame < options.iterations; ++frame) {
    if (!AcquireTurn(
            name, endpoint.mutex.Get(), acquireKey, options.timeoutMs, failure, stats)) {
      return;
    }

    const bool hasExpectedInput = !expectedFrameIsPrevious || frame != 0;
    if (!failure.failed.load() && hasExpectedInput) {
      const std::uint32_t expectedFrame =
          expectedFrameIsPrevious ? frame - 1 : frame;
      if (auto mismatch = ValidateSignature(device,
                                            endpoint.texture.Get(),
                                            staging,
                                            expectedFrame,
                                            expectedStage)) {
        failure.Fail(std::string(name) + ": " + *mismatch);
      }
    }

    if (!failure.failed.load()) {
      WriteSignature(device, endpoint.texture.Get(), frame, writtenStage);
    }

    if (!ReleaseTurn(name, endpoint.mutex.Get(), releaseKey, failure)) {
      return;
    }
  }
}

void PrintStats(std::string_view name, const WaitStats& stats) {
  const double average =
      stats.turns == 0 ? 0.0 : stats.totalMicroseconds / stats.turns;
  std::cout << "      " << name << " wait: avg " << std::fixed
            << std::setprecision(2) << average << " us, max "
            << stats.maximumMicroseconds << " us, turns " << stats.turns
            << '\n';
}

int Run(const Options& options) {
  const auto adapter = SelectHardwareAdapter();
  const auto deviceA = CreateDevice(adapter.Get(), options.requestDebugLayer);
  const auto deviceB = CreateDevice(adapter.Get(), options.requestDebugLayer);
  const auto deviceC = CreateDevice(adapter.Get(), options.requestDebugLayer);

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

  auto sharedOwner = CreateSharedTextureOwner(deviceA, description);
  const auto endpointB = OpenSharedTexture(deviceB, sharedOwner.handle.get());
  const auto endpointC = OpenSharedTexture(deviceC, sharedOwner.handle.get());

  const auto stagingA = CreateStagingTexture(deviceA, description);
  const auto stagingB = CreateStagingTexture(deviceB, description);
  const auto stagingC = CreateStagingTexture(deviceC, description);

  FailureState failure;
  WaitStats statsA;
  WaitStats statsB;
  WaitStats statsC;
  std::barrier<> startGate(3);
  const auto start = std::chrono::steady_clock::now();

  std::thread threadA([&] {
    try {
      RunStage("device A",
               deviceA,
               sharedOwner.endpoint,
               stagingA.Get(),
               0,
               1,
               kStageC,
               kStageA,
               true,
               options,
               startGate,
               failure,
               statsA);

      if (AcquireTurn("device A final",
                      sharedOwner.endpoint.mutex.Get(),
                      0,
                      options.timeoutMs,
                      failure,
                      statsA)) {
        if (!failure.failed.load()) {
          if (auto mismatch = ValidateSignature(deviceA,
                                                sharedOwner.endpoint.texture.Get(),
                                                stagingA.Get(),
                                                options.iterations - 1,
                                                kStageC)) {
            failure.Fail("device A final: " + *mismatch);
          }
        }
        if (!ReleaseTurn("device A final",
                         sharedOwner.endpoint.mutex.Get(),
                         0,
                         failure)) {
          return;
        }
      }
    } catch (const std::exception& exception) {
      failure.Fail(std::string("device A exception: ") + exception.what());
    }
  });

  std::thread threadB([&] {
    try {
      RunStage("device B",
               deviceB,
               endpointB,
               stagingB.Get(),
               1,
               2,
               kStageA,
               kStageB,
               false,
               options,
               startGate,
               failure,
               statsB);
    } catch (const std::exception& exception) {
      failure.Fail(std::string("device B exception: ") + exception.what());
    }
  });

  std::thread threadC([&] {
    try {
      RunStage("device C",
               deviceC,
               endpointC,
               stagingC.Get(),
               2,
               0,
               kStageB,
               kStageC,
               false,
               options,
               startGate,
               failure,
               statsC);
    } catch (const std::exception& exception) {
      failure.Fail(std::string("device C exception: ") + exception.what());
    }
  });

  threadA.join();
  threadB.join();
  threadC.join();

  const auto elapsed = std::chrono::duration<double, std::milli>(
      std::chrono::steady_clock::now() - start);
  if (failure.failed.load()) {
    std::scoped_lock lock(failure.messageMutex);
    std::cerr << "FAIL: " << failure.message << '\n';
    return 1;
  }

  std::cout << "PASS: completed " << options.iterations
            << " three-device key rings on " << AdapterName(adapter.Get())
            << " in " << std::fixed << std::setprecision(2) << elapsed.count()
            << " ms\n";
  PrintStats("device A", statsA);
  PrintStats("device B", statsB);
  PrintStats("device C", statsC);
  std::cout << "      debug layers: "
            << (deviceA.debugLayerEnabled && deviceB.debugLayerEnabled &&
                        deviceC.debugLayerEnabled
                    ? "all enabled"
                    : "one or more disabled")
            << '\n';
  return 0;
}

}  // namespace

int main(int argc, char* argv[]) {
  try {
    return Run(ParseOptions(std::span(argv, static_cast<std::size_t>(argc))));
  } catch (const std::exception& exception) {
    std::cerr << "ERROR: " << exception.what() << '\n';
    return 2;
  }
}
