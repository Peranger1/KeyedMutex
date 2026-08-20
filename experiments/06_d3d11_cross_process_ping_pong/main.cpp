#include "keyed_mutex/d3d11_test_support.hpp"

#include <gtest/gtest.h>

#include <charconv>
#include <chrono>
#include <cstdint>
#include <exception>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using keyed_mutex::test::AdapterName;
using keyed_mutex::test::ComPtr;
using keyed_mutex::test::CreateDevice;
using keyed_mutex::test::CreateSharedTextureOwner;
using keyed_mutex::test::DeviceBundle;
using keyed_mutex::test::HResultText;
using keyed_mutex::test::OpenSharedTexture;
using keyed_mutex::test::SelectHardwareAdapter;
using keyed_mutex::test::SelectHardwareAdapterByLuid;
using keyed_mutex::test::SharedTextureEndpoint;
using keyed_mutex::test::ThrowIfFailed;
using keyed_mutex::test::UniqueHandle;

namespace {

constexpr UINT kTextureWidth = 32;
constexpr UINT kTextureHeight = 32;
constexpr UINT kBytesPerPixel = 4;
constexpr std::uint8_t kParentStage = 0x5a;
constexpr std::uint8_t kChildStage = 0xc3;

constexpr DWORD kChildSuccessExitCode = 0;
constexpr DWORD kChildSetupFailureExitCode = 70;
constexpr DWORD kChildAcquireFailureExitCode = 71;
constexpr DWORD kChildValidationFailureExitCode = 72;
constexpr DWORD kChildReleaseFailureExitCode = 73;
constexpr DWORD kForcedTerminationExitCode = 74;

struct Options {
  std::uint32_t iterations = 100;
  DWORD timeoutMs = 5'000;
  bool requestDebugLayer = true;
};

struct ChildOptions : Options {
  HANDLE sharedHandle = nullptr;
  LUID adapterLuid{};
};

Options gOptions;

[[nodiscard]] std::optional<std::uint64_t>
ParseUint64(std::string_view value) {
  std::uint64_t result = 0;
  const auto parse =
      std::from_chars(value.data(), value.data() + value.size(), result);
  if (parse.ec != std::errc{} || parse.ptr != value.data() + value.size()) {
    return std::nullopt;
  }
  return result;
}

[[nodiscard]] bool HasArgument(std::span<char*> arguments,
                               std::string_view expected) {
  for (std::size_t index = 1; index < arguments.size(); ++index) {
    if (std::string_view(arguments[index]) == expected) {
      return true;
    }
  }
  return false;
}

void ParseCommonOption(std::string_view argument,
                       std::string_view value,
                       Options& options) {
  const auto parsed = ParseUint64(value);
  if (!parsed || *parsed == 0 || *parsed > UINT32_MAX) {
    throw std::invalid_argument(std::string(argument) +
                                " requires a positive 32-bit integer");
  }
  if (argument == "--iterations") {
    options.iterations = static_cast<std::uint32_t>(*parsed);
  } else {
    options.timeoutMs = static_cast<DWORD>(*parsed);
  }
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
      ParseCommonOption(argument, arguments[++index], options);
      continue;
    }
    throw std::invalid_argument("unknown or incomplete argument: " +
                                std::string(argument));
  }
  return options;
}

[[nodiscard]] ChildOptions ParseChildOptions(std::span<char*> arguments) {
  ChildOptions options;
  bool foundHandle = false;
  bool foundAdapter = false;
  for (std::size_t index = 1; index < arguments.size(); ++index) {
    const std::string_view argument(arguments[index]);
    if (argument == "--child") {
      continue;
    }
    if (argument == "--no-debug-layer") {
      options.requestDebugLayer = false;
      continue;
    }
    if ((argument == "--iterations" || argument == "--timeout-ms") &&
        index + 1 < arguments.size()) {
      ParseCommonOption(argument, arguments[++index], options);
      continue;
    }
    if ((argument == "--shared-handle" || argument == "--adapter-luid") &&
        index + 1 < arguments.size()) {
      const auto parsed = ParseUint64(arguments[++index]);
      if (!parsed || (argument == "--shared-handle" && *parsed == 0)) {
        throw std::invalid_argument(std::string(argument) +
                                    " requires an unsigned integer");
      }
      if (argument == "--shared-handle") {
        options.sharedHandle = reinterpret_cast<HANDLE>(
            static_cast<std::uintptr_t>(*parsed));
        foundHandle = true;
      } else {
        options.adapterLuid.LowPart = static_cast<DWORD>(*parsed);
        options.adapterLuid.HighPart =
            static_cast<LONG>(static_cast<DWORD>(*parsed >> 32u));
        foundAdapter = true;
      }
      continue;
    }
    throw std::invalid_argument("unknown or incomplete child argument: " +
                                std::string(argument));
  }
  if (!foundHandle || !foundAdapter) {
    throw std::invalid_argument(
        "--shared-handle and --adapter-luid are required in child mode");
  }
  return options;
}

[[nodiscard]] std::uint64_t PackLuid(LUID luid) {
  return (static_cast<std::uint64_t>(static_cast<DWORD>(luid.HighPart))
          << 32u) |
         luid.LowPart;
}

[[nodiscard]] LUID GetAdapterLuid(IDXGIAdapter1* adapter) {
  DXGI_ADAPTER_DESC1 description{};
  ThrowIfFailed(adapter->GetDesc1(&description),
                "IDXGIAdapter1::GetDesc1");
  return description.AdapterLuid;
}

[[nodiscard]] std::wstring CurrentExecutablePath() {
  std::vector<wchar_t> buffer(32'768);
  const DWORD length = GetModuleFileNameW(
      nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
  if (length == 0 || length == buffer.size()) {
    ThrowIfFailed(HRESULT_FROM_WIN32(GetLastError()), "GetModuleFileNameW");
  }
  return std::wstring(buffer.data(), length);
}

[[nodiscard]] D3D11_TEXTURE2D_DESC SharedTextureDescription() {
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
          stage ^ ((frame >> 16u) + x * 17u + y * 31u));
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
    const auto* actualRow =
        actual + static_cast<std::size_t>(y) * mapped.RowPitch;
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

[[nodiscard]] std::optional<std::string>
Acquire(IDXGIKeyedMutex* mutex,
        UINT64 key,
        DWORD timeoutMs,
        std::string_view participant) {
  const HRESULT result = mutex->AcquireSync(key, timeoutMs);
  if (result == S_OK) {
    return std::nullopt;
  }
  return std::string(participant) + " AcquireSync(" +
         std::to_string(key) + ") returned " + HResultText(result);
}

[[nodiscard]] std::optional<std::string>
Release(IDXGIKeyedMutex* mutex,
        UINT64 key,
        std::string_view participant) {
  const HRESULT result = mutex->ReleaseSync(key);
  if (result == S_OK) {
    return std::nullopt;
  }
  return std::string(participant) + " ReleaseSync(" +
         std::to_string(key) + ") returned " + HResultText(result);
}

[[nodiscard]] int RunChild(const ChildOptions& options) {
  try {
    const auto adapter = SelectHardwareAdapterByLuid(options.adapterLuid);
    const auto device = CreateDevice(adapter.Get(), options.requestDebugLayer);
    const auto endpoint = OpenSharedTexture(device, options.sharedHandle);
    const auto staging =
        CreateStagingTexture(device, SharedTextureDescription());
    bool validationFailed = false;

    for (std::uint32_t frame = 0; frame < options.iterations; ++frame) {
      if (auto error =
              Acquire(endpoint.mutex.Get(), 1, options.timeoutMs, "child")) {
        std::cerr << "ERROR: " << *error << '\n';
        return static_cast<int>(kChildAcquireFailureExitCode);
      }

      if (auto mismatch = ValidateSignature(device,
                                            endpoint.texture.Get(),
                                            staging.Get(),
                                            frame,
                                            kParentStage)) {
        std::cerr << "ERROR: child validation: " << *mismatch << '\n';
        validationFailed = true;
      }
      WriteSignature(
          device, endpoint.texture.Get(), frame, kChildStage);

      if (auto error = Release(endpoint.mutex.Get(), 0, "child")) {
        std::cerr << "ERROR: " << *error << '\n';
        return static_cast<int>(kChildReleaseFailureExitCode);
      }
    }
    return validationFailed
               ? static_cast<int>(kChildValidationFailureExitCode)
               : static_cast<int>(kChildSuccessExitCode);
  } catch (const std::exception& exception) {
    std::cerr << "ERROR: child setup: " << exception.what() << '\n';
    return static_cast<int>(kChildSetupFailureExitCode);
  }
}

class ChildProcess {
public:
  ChildProcess(UniqueHandle process, UniqueHandle thread)
      : process_(std::move(process)), thread_(std::move(thread)) {}

  ChildProcess(const ChildProcess&) = delete;
  ChildProcess& operator=(const ChildProcess&) = delete;
  ChildProcess(ChildProcess&&) = default;
  ChildProcess& operator=(ChildProcess&&) = default;

  ~ChildProcess() {
    DWORD exitCode = 0;
    if (process_.get() != nullptr &&
        GetExitCodeProcess(process_.get(), &exitCode) &&
        exitCode == STILL_ACTIVE) {
      TerminateProcess(process_.get(), kForcedTerminationExitCode);
      WaitForSingleObject(process_.get(), 5'000);
    }
  }

  [[nodiscard]] DWORD Wait(DWORD timeoutMs) {
    const DWORD wait = WaitForSingleObject(process_.get(), timeoutMs);
    if (wait == WAIT_TIMEOUT) {
      throw std::runtime_error("child process did not exit within timeout");
    }
    if (wait != WAIT_OBJECT_0) {
      ThrowIfFailed(HRESULT_FROM_WIN32(GetLastError()),
                    "WaitForSingleObject(child)");
    }

    DWORD exitCode = 0;
    if (!GetExitCodeProcess(process_.get(), &exitCode)) {
      ThrowIfFailed(HRESULT_FROM_WIN32(GetLastError()),
                    "GetExitCodeProcess");
    }
    return exitCode;
  }

private:
  UniqueHandle process_;
  UniqueHandle thread_;
};

[[nodiscard]] ChildProcess SpawnChild(HANDLE sharedHandle,
                                      LUID adapterLuid,
                                      const Options& options) {
  const std::wstring executable = CurrentExecutablePath();
  std::wstring commandLine =
      L"\"" + executable + L"\" --child --shared-handle " +
      std::to_wstring(reinterpret_cast<std::uintptr_t>(sharedHandle)) +
      L" --adapter-luid " + std::to_wstring(PackLuid(adapterLuid)) +
      L" --iterations " + std::to_wstring(options.iterations) +
      L" --timeout-ms " + std::to_wstring(options.timeoutMs);
  if (!options.requestDebugLayer) {
    commandLine += L" --no-debug-layer";
  }
  std::vector<wchar_t> mutableCommand(commandLine.begin(), commandLine.end());
  mutableCommand.push_back(L'\0');

  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION processInfo{};
  const BOOL created = CreateProcessW(executable.c_str(),
                                      mutableCommand.data(),
                                      nullptr,
                                      nullptr,
                                      TRUE,
                                      CREATE_NO_WINDOW,
                                      nullptr,
                                      nullptr,
                                      &startup,
                                      &processInfo);
  if (!created) {
    ThrowIfFailed(HRESULT_FROM_WIN32(GetLastError()), "CreateProcessW");
  }
  return ChildProcess(UniqueHandle(processInfo.hProcess),
                      UniqueHandle(processInfo.hThread));
}

void RecordFirstFailure(std::optional<std::string>& failure,
                        std::string message) {
  if (!failure) {
    failure = std::move(message);
  }
}

void RunExperiment(const Options& options) {
  const auto adapter = SelectHardwareAdapter();
  const auto device = CreateDevice(adapter.Get(), options.requestDebugLayer);
  const auto description = SharedTextureDescription();
  const auto staging = CreateStagingTexture(device, description);

  SECURITY_ATTRIBUTES security{};
  security.nLength = sizeof(security);
  security.bInheritHandle = TRUE;
  auto shared = CreateSharedTextureOwner(device, description, &security);
  auto child =
      SpawnChild(shared.handle.get(), GetAdapterLuid(adapter.Get()), options);

  std::optional<std::string> failure;
  const auto start = std::chrono::steady_clock::now();
  bool protocolActive = true;
  for (std::uint32_t frame = 0; frame < options.iterations; ++frame) {
    if (auto error = Acquire(shared.endpoint.mutex.Get(),
                             0,
                             options.timeoutMs,
                             "parent")) {
      RecordFirstFailure(failure, *error);
      protocolActive = false;
      break;
    }

    if (frame != 0) {
      if (auto mismatch = ValidateSignature(device,
                                            shared.endpoint.texture.Get(),
                                            staging.Get(),
                                            frame - 1,
                                            kChildStage)) {
        RecordFirstFailure(failure, "parent validation: " + *mismatch);
      }
    }
    WriteSignature(
        device, shared.endpoint.texture.Get(), frame, kParentStage);

    if (auto error = Release(shared.endpoint.mutex.Get(), 1, "parent")) {
      RecordFirstFailure(failure, *error);
      protocolActive = false;
      break;
    }
  }

  if (protocolActive) {
    if (auto error = Acquire(shared.endpoint.mutex.Get(),
                             0,
                             options.timeoutMs,
                             "parent final")) {
      RecordFirstFailure(failure, *error);
    } else {
      if (auto mismatch = ValidateSignature(device,
                                            shared.endpoint.texture.Get(),
                                            staging.Get(),
                                            options.iterations - 1,
                                            kChildStage)) {
        RecordFirstFailure(failure, "parent final validation: " + *mismatch);
      }
      if (auto releaseError =
              Release(shared.endpoint.mutex.Get(), 0, "parent final")) {
        RecordFirstFailure(failure, *releaseError);
      }
    }
  }

  const DWORD childExit = child.Wait(options.timeoutMs + 5'000);
  if (childExit != kChildSuccessExitCode) {
    RecordFirstFailure(failure,
                       "child process exited with code " +
                           std::to_string(childExit));
  }
  if (failure) {
    FAIL() << *failure;
  }

  const auto elapsed = std::chrono::duration<double, std::milli>(
      std::chrono::steady_clock::now() - start);
  std::cout << "PASS: completed " << options.iterations
            << " cross-process GPU signature round trips on "
            << AdapterName(adapter.Get()) << " in " << std::fixed
            << std::setprecision(2) << elapsed.count() << " ms\n"
            << "      parent debug layer: "
            << (device.debugLayerEnabled ? "enabled" : "disabled") << '\n';
}

TEST(D3D11KeyedMutex, CrossProcessPingPong) { RunExperiment(gOptions); }

}  // namespace

int main(int argc, char* argv[]) {
  const auto arguments = std::span(argv, static_cast<std::size_t>(argc));
  if (HasArgument(arguments, "--child")) {
    try {
      return RunChild(ParseChildOptions(arguments));
    } catch (const std::exception& exception) {
      std::cerr << "ERROR: child arguments: " << exception.what() << '\n';
      return static_cast<int>(kChildSetupFailureExitCode);
    }
  }

  ::testing::InitGoogleTest(&argc, argv);
  try {
    gOptions = ParseOptions(std::span(argv, static_cast<std::size_t>(argc)));
  } catch (const std::exception& exception) {
    std::cerr << "ERROR: " << exception.what() << '\n';
    return 2;
  }
  return RUN_ALL_TESTS();
}
