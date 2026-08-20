#include "keyed_mutex/d3d11_test_support.hpp"

#include <d3dcompiler.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cmath>
#include <cstring>
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
using keyed_mutex::test::DeviceBundle;
using keyed_mutex::test::HResultText;
using keyed_mutex::test::SelectHardwareAdapter;
using keyed_mutex::test::ThrowIfFailed;

namespace {

constexpr UINT kTextureWidth = 64;
constexpr UINT kTextureHeight = 64;
constexpr UINT kBytesPerPixel = 4;

enum class WorkloadKind : std::uint8_t {
  Clear,
  Draw,
  Copy,
};

struct Options {
  std::uint32_t iterations = 30;
  DWORD timeoutMs = 5'000;
  bool requestDebugLayer = true;
};

Options gOptions;

[[nodiscard]] std::string WorkloadName(WorkloadKind workload) {
  switch (workload) {
    case WorkloadKind::Clear:
      return "ClearRenderTargetView";
    case WorkloadKind::Draw:
      return "Draw";
    case WorkloadKind::Copy:
      return "CopyResource";
  }
  return "unknown";
}

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

[[nodiscard]] D3D11_TEXTURE2D_DESC SharedDescription() {
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
  return description;
}

[[nodiscard]] ComPtr<ID3D11Texture2D>
CreateStagingTexture(const DeviceBundle& device,
                     const D3D11_TEXTURE2D_DESC& sharedDescription) {
  auto description = sharedDescription;
  description.Usage = D3D11_USAGE_STAGING;
  description.BindFlags = 0;
  description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  description.MiscFlags = 0;
  ComPtr<ID3D11Texture2D> staging;
  ThrowIfFailed(device.device->CreateTexture2D(&description, nullptr, &staging),
                "ID3D11Device::CreateTexture2D(staging)");
  return staging;
}

struct ColorBytes {
  std::uint8_t r;
  std::uint8_t g;
  std::uint8_t b;
  std::uint8_t a;
};

[[nodiscard]] ColorBytes MakeColor(WorkloadKind workload,
                                   std::uint32_t frame) {
  const auto kind = static_cast<std::uint8_t>(workload);
  return {
      static_cast<std::uint8_t>((frame * 37u + kind * 41u + 17u) & 0xffu),
      static_cast<std::uint8_t>((frame * 19u + kind * 73u + 29u) & 0xffu),
      static_cast<std::uint8_t>((frame * 53u + kind * 11u + 43u) & 0xffu),
      0xffu,
  };
}

[[nodiscard]] float ToFloat(std::uint8_t value) {
  return static_cast<float>(value) / 255.0f;
}

struct FailureState {
  std::atomic<bool> failed = false;
  std::mutex failureMutex;
  std::string failureMessage;
  std::atomic<std::uint32_t> matches = 0;
  std::atomic<std::uint32_t> mismatches = 0;
  std::mutex mismatchMutex;
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
    std::scoped_lock lock(mismatchMutex);
    if (firstMismatch.empty()) {
      firstMismatch = std::move(message);
    }
  }
};

[[nodiscard]] std::optional<std::string>
ValidateColor(const D3D11_MAPPED_SUBRESOURCE& mapped,
              WorkloadKind workload,
              std::uint32_t frame) {
  const auto expected = MakeColor(workload, frame);
  const auto* actual = static_cast<const std::uint8_t*>(mapped.pData);
  constexpr unsigned kTolerance = 2;
  for (UINT y = 0; y < kTextureHeight; ++y) {
    const auto* row = actual + static_cast<std::size_t>(y) * mapped.RowPitch;
    for (UINT x = 0; x < kTextureWidth; ++x) {
      const auto* pixel = row + static_cast<std::size_t>(x) * kBytesPerPixel;
      const std::array<std::uint8_t, 4> expectedBytes = {
          expected.r, expected.g, expected.b, expected.a};
      for (UINT channel = 0; channel < expectedBytes.size(); ++channel) {
        const auto difference = std::abs(
            static_cast<int>(pixel[channel]) -
            static_cast<int>(expectedBytes[channel]));
        if (difference > static_cast<int>(kTolerance)) {
          std::ostringstream message;
          message << WorkloadName(workload) << " frame " << frame
                  << " mismatch at pixel (" << x << "," << y
                  << "), channel " << channel << ": expected "
                  << static_cast<unsigned>(expectedBytes[channel]) << ", got "
                  << static_cast<unsigned>(pixel[channel]);
          return message.str();
        }
      }
    }
  }
  return std::nullopt;
}

[[nodiscard]] ComPtr<ID3DBlob> CompileShader(const char* source,
                                             const char* entry,
                                             const char* target) {
  ComPtr<ID3DBlob> bytecode;
  ComPtr<ID3DBlob> errors;
  const HRESULT result = D3DCompile(source,
                                    std::strlen(source),
                                    nullptr,
                                    nullptr,
                                    nullptr,
                                    entry,
                                    target,
                                    D3DCOMPILE_ENABLE_STRICTNESS,
                                    0,
                                    &bytecode,
                                    &errors);
  if (FAILED(result)) {
    std::string message = "D3DCompile failed: " + HResultText(result);
    if (errors != nullptr) {
      message += " (" +
                 std::string(static_cast<const char*>(errors->GetBufferPointer()),
                             errors->GetBufferSize()) +
                 ")";
    }
    throw std::runtime_error(message);
  }
  return bytecode;
}

struct DrawResources {
  ComPtr<ID3D11RenderTargetView> renderTarget;
  ComPtr<ID3D11VertexShader> vertexShader;
  ComPtr<ID3D11PixelShader> pixelShader;
  ComPtr<ID3D11Buffer> colorBuffer;
};

[[nodiscard]] DrawResources CreateDrawResources(
    const DeviceBundle& device,
    ID3D11Texture2D* texture) {
  DrawResources resources;
  ThrowIfFailed(device.device->CreateRenderTargetView(
                    texture, nullptr, &resources.renderTarget),
                "ID3D11Device::CreateRenderTargetView(draw)");

  constexpr char kVertexShader[] = R"(
struct VSOut { float4 position : SV_POSITION; };
VSOut VSMain(uint id : SV_VertexID) {
  float2 positions[3] = { float2(-1.0, -1.0), float2(-1.0, 3.0), float2(3.0, -1.0) };
  VSOut output;
  output.position = float4(positions[id], 0.0, 1.0);
  return output;
}
)";
  constexpr char kPixelShader[] = R"(
cbuffer ColorBuffer : register(b0) { float4 color; };
float4 PSMain() : SV_TARGET { return color; }
)";
  const auto vertexBytecode =
      CompileShader(kVertexShader, "VSMain", "vs_5_0");
  const auto pixelBytecode =
      CompileShader(kPixelShader, "PSMain", "ps_5_0");
  ThrowIfFailed(device.device->CreateVertexShader(vertexBytecode->GetBufferPointer(),
                                                  vertexBytecode->GetBufferSize(),
                                                  nullptr,
                                                  &resources.vertexShader),
                "ID3D11Device::CreateVertexShader");
  ThrowIfFailed(device.device->CreatePixelShader(pixelBytecode->GetBufferPointer(),
                                                 pixelBytecode->GetBufferSize(),
                                                 nullptr,
                                                 &resources.pixelShader),
                "ID3D11Device::CreatePixelShader");

  D3D11_BUFFER_DESC bufferDescription{};
  bufferDescription.ByteWidth = 16;
  bufferDescription.Usage = D3D11_USAGE_DYNAMIC;
  bufferDescription.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  bufferDescription.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  ThrowIfFailed(device.device->CreateBuffer(&bufferDescription,
                                             nullptr,
                                             &resources.colorBuffer),
                "ID3D11Device::CreateBuffer(color)");
  return resources;
}

void DrawColor(const DeviceBundle& device,
               const DrawResources& resources,
               ColorBytes color) {
  D3D11_MAPPED_SUBRESOURCE mapped{};
  ThrowIfFailed(device.context->Map(resources.colorBuffer.Get(),
                                    0,
                                    D3D11_MAP_WRITE_DISCARD,
                                    0,
                                    &mapped),
                "Map(draw color buffer)");
  auto* values = static_cast<float*>(mapped.pData);
  values[0] = ToFloat(color.r);
  values[1] = ToFloat(color.g);
  values[2] = ToFloat(color.b);
  values[3] = ToFloat(color.a);
  device.context->Unmap(resources.colorBuffer.Get(), 0);

  D3D11_VIEWPORT viewport{};
  viewport.Width = static_cast<float>(kTextureWidth);
  viewport.Height = static_cast<float>(kTextureHeight);
  viewport.MaxDepth = 1.0f;
  ID3D11RenderTargetView* renderTarget = resources.renderTarget.Get();
  ID3D11Buffer* colorBuffer = resources.colorBuffer.Get();
  device.context->OMSetRenderTargets(1, &renderTarget, nullptr);
  device.context->RSSetViewports(1, &viewport);
  device.context->IASetInputLayout(nullptr);
  device.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  device.context->VSSetShader(resources.vertexShader.Get(), nullptr, 0);
  device.context->PSSetShader(resources.pixelShader.Get(), nullptr, 0);
  device.context->PSSetConstantBuffers(0, 1, &colorBuffer);
  device.context->Draw(3, 0);
}

void ExecuteWorkload(const DeviceBundle& device,
                     ID3D11Texture2D* sharedTexture,
                     ID3D11RenderTargetView* sharedRenderTarget,
                     ID3D11Texture2D* copySource,
                     ID3D11RenderTargetView* copyRenderTarget,
                     const DrawResources& drawResources,
                     WorkloadKind workload,
                     std::uint32_t frame) {
  const auto color = MakeColor(workload, frame);
  const float clearColor[4] = {
      ToFloat(color.r), ToFloat(color.g), ToFloat(color.b), ToFloat(color.a)};
  if (workload == WorkloadKind::Clear) {
    device.context->ClearRenderTargetView(sharedRenderTarget, clearColor);
  } else if (workload == WorkloadKind::Draw) {
    DrawColor(device, drawResources, color);
  } else {
    device.context->ClearRenderTargetView(copyRenderTarget, clearColor);
    device.context->CopyResource(sharedTexture, copySource);
  }
}

void RunExperiment(const Options& options,
                   WorkloadKind workload,
                   bool flushAfterWorkload) {
  const auto adapter = SelectHardwareAdapter();
  const auto producer = CreateDevice(adapter.Get(), options.requestDebugLayer);
  const auto consumer = CreateDevice(adapter.Get(), options.requestDebugLayer);
  const auto description = SharedDescription();
  auto shared = CreateSharedTexturePair(producer, consumer, description);
  auto staging = CreateStagingTexture(consumer, description);

  ComPtr<ID3D11RenderTargetView> sharedRenderTarget;
  ThrowIfFailed(producer.device->CreateRenderTargetView(
                    shared.ownerTexture.Get(), nullptr, &sharedRenderTarget),
                "CreateRenderTargetView(shared)");
  ComPtr<ID3D11Texture2D> copySource;
  ComPtr<ID3D11RenderTargetView> copyRenderTarget;
  if (workload == WorkloadKind::Copy) {
    auto sourceDescription = description;
    sourceDescription.MiscFlags = 0;
    ThrowIfFailed(producer.device->CreateTexture2D(
                      &sourceDescription, nullptr, &copySource),
                  "CreateTexture2D(copy source)");
    ThrowIfFailed(producer.device->CreateRenderTargetView(
                      copySource.Get(), nullptr, &copyRenderTarget),
                  "CreateRenderTargetView(copy source)");
  }
  DrawResources drawResources;
  if (workload == WorkloadKind::Draw) {
    drawResources = CreateDrawResources(producer, shared.ownerTexture.Get());
  }

  FailureState state;
  const auto start = std::chrono::steady_clock::now();
  std::thread producerThread([&] {
    try {
      for (std::uint32_t frame = 0;
           frame < options.iterations && !state.failed.load(); ++frame) {
        const HRESULT acquire =
            shared.ownerMutex->AcquireSync(0, options.timeoutMs);
        if (acquire != S_OK) {
          state.Fail("producer AcquireSync(0) failed: " + HResultText(acquire));
          break;
        }
        ExecuteWorkload(producer,
                        shared.ownerTexture.Get(),
                        sharedRenderTarget.Get(),
                        copySource.Get(),
                        copyRenderTarget.Get(),
                        drawResources,
                        workload,
                        frame);
        if (flushAfterWorkload) {
          producer.context->Flush();
        }
        const HRESULT release = shared.ownerMutex->ReleaseSync(1);
        if (release != S_OK) {
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
        const HRESULT acquire =
            shared.openedMutex->AcquireSync(1, options.timeoutMs);
        if (acquire != S_OK) {
          state.Fail("consumer AcquireSync(1) failed: " + HResultText(acquire));
          break;
        }
        consumer.context->CopyResource(staging.Get(),
                                       shared.openedTexture.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        const HRESULT mapResult =
            consumer.context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
        if (FAILED(mapResult)) {
          state.Fail("consumer Map(staging) failed: " + HResultText(mapResult));
        } else {
          if (auto mismatch = ValidateColor(mapped, workload, frame)) {
            state.RecordMismatch(*mismatch);
          } else {
            state.matches.fetch_add(1);
          }
          consumer.context->Unmap(staging.Get(), 0);
        }
        const HRESULT release = shared.openedMutex->ReleaseSync(0);
        if (release != S_OK) {
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

  if (state.failed.load()) {
    std::scoped_lock lock(state.failureMutex);
    FAIL() << state.failureMessage;
  }
  if (flushAfterWorkload) {
    EXPECT_EQ(state.mismatches.load(), 0u);
  }
  const auto elapsed = std::chrono::duration<double, std::milli>(
      std::chrono::steady_clock::now() - start);
  std::cout << "RESULT: " << WorkloadName(workload) << " "
            << (flushAfterWorkload ? "with Flush" : "without Flush")
            << " on " << AdapterName(adapter.Get()) << "\n"
            << "      matching frames: " << state.matches.load() << " / "
            << options.iterations << "\n"
            << "      mismatching frames: " << state.mismatches.load() << "\n"
            << "      elapsed: " << std::fixed << std::setprecision(2)
            << elapsed.count() << " ms\n"
            << "      observation only: "
            << (flushAfterWorkload ? "no; mismatches fail the baseline"
                                   : "yes; mismatches are recorded only")
            << '\n';
  if (state.mismatches.load() != 0) {
    std::scoped_lock lock(state.mismatchMutex);
    std::cout << "      first mismatch: " << state.firstMismatch << '\n';
  }
}

#define DEFINE_VISIBILITY_TEST(Name, Workload, Flush) \
  TEST(D3D11KeyedMutex, Name) { RunExperiment(gOptions, Workload, Flush); }

DEFINE_VISIBILITY_TEST(ClearVisibilityWithFlush, WorkloadKind::Clear, true)
DEFINE_VISIBILITY_TEST(ClearVisibilityWithoutFlushObservation,
                       WorkloadKind::Clear,
                       false)
DEFINE_VISIBILITY_TEST(DrawVisibilityWithFlush, WorkloadKind::Draw, true)
DEFINE_VISIBILITY_TEST(DrawVisibilityWithoutFlushObservation,
                       WorkloadKind::Draw,
                       false)
DEFINE_VISIBILITY_TEST(CopyVisibilityWithFlush, WorkloadKind::Copy, true)
DEFINE_VISIBILITY_TEST(CopyVisibilityWithoutFlushObservation,
                       WorkloadKind::Copy,
                       false)

#undef DEFINE_VISIBILITY_TEST

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
