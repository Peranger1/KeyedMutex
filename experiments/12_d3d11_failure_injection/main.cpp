#include "keyed_mutex/d3d11_test_support.hpp"

#include <gtest/gtest.h>

#include <charconv>
#include <cstdint>
#include <exception>
#include <iomanip>
#include <iostream>
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
using keyed_mutex::test::SelectHardwareAdapterByLuid;
using keyed_mutex::test::SharedTextureEndpoint;
using keyed_mutex::test::SharedTextureOwner;
using keyed_mutex::test::ThrowIfFailed;
using keyed_mutex::test::UniqueHandle;

namespace {

constexpr UINT kTextureWidth = 32;
constexpr UINT kTextureHeight = 32;
constexpr UINT kBytesPerPixel = 4;
constexpr std::uint8_t kParentStage = 0x5a;
constexpr std::uint8_t kChildStage = 0xc3;

constexpr DWORD kChildCompletedExitCode = 90;
constexpr DWORD kChildSetupFailureExitCode = 91;
constexpr DWORD kChildAcquireFailureExitCode = 92;
constexpr DWORD kChildReleaseFailureExitCode = 93;
constexpr DWORD kChildUnexpectedExitCode = 94;

enum class FailurePhase : std::uint32_t {
  BeforeAcquire = 0,
  AfterAcquire = 1,
  AfterGpuWrite = 2,
  AfterFlush = 3,
  AfterRelease = 4,
};

struct Options {
  DWORD timeoutMs = 1'000;
  bool requestDebugLayer = true;
};

struct ChildOptions {
  HANDLE sharedHandle = nullptr;
  LUID adapterLuid{};
  FailurePhase phase = FailurePhase::BeforeAcquire;
  DWORD timeoutMs = 1'000;
  bool requestDebugLayer = true;
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

[[nodiscard]] std::string PhaseName(FailurePhase phase) {
  switch (phase) {
    case FailurePhase::BeforeAcquire:
      return "before AcquireSync";
    case FailurePhase::AfterAcquire:
      return "after AcquireSync";
    case FailurePhase::AfterGpuWrite:
      return "after GPU write";
    case FailurePhase::AfterFlush:
      return "after Flush";
    case FailurePhase::AfterRelease:
      return "after ReleaseSync";
  }
  return "unknown phase";
}

[[nodiscard]] std::optional<FailurePhase>
ParsePhase(std::string_view value) {
  const auto parsed = ParseUint64(value);
  if (!parsed || *parsed > static_cast<std::uint64_t>(FailurePhase::AfterRelease)) {
    return std::nullopt;
  }
  return static_cast<FailurePhase>(*parsed);
}

[[nodiscard]] Options ParseOptions(std::span<char*> arguments) {
  Options options;
  for (std::size_t index = 1; index < arguments.size(); ++index) {
    const std::string_view argument(arguments[index]);
    if (argument == "--no-debug-layer") {
      options.requestDebugLayer = false;
      continue;
    }
    if (argument == "--timeout-ms" && index + 1 < arguments.size()) {
      const auto parsed = ParseUint64(arguments[++index]);
      if (!parsed || *parsed == 0 || *parsed > UINT32_MAX) {
        throw std::invalid_argument(
            "--timeout-ms requires a positive 32-bit integer");
      }
      options.timeoutMs = static_cast<DWORD>(*parsed);
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
  bool foundPhase = false;
  for (std::size_t index = 1; index < arguments.size(); ++index) {
    const std::string_view argument(arguments[index]);
    if (argument == "--child") {
      continue;
    }
    if (argument == "--no-debug-layer") {
      options.requestDebugLayer = false;
      continue;
    }
    if ((argument == "--shared-handle" || argument == "--adapter-luid" ||
         argument == "--phase" || argument == "--timeout-ms") &&
        index + 1 < arguments.size()) {
      const auto parsed = ParseUint64(arguments[++index]);
      if (!parsed ||
          ((argument == "--shared-handle" || argument == "--phase") &&
           *parsed == 0)) {
        if (argument == "--phase" && parsed && *parsed == 0) {
          options.phase = FailurePhase::BeforeAcquire;
          foundPhase = true;
          continue;
        }
        throw std::invalid_argument(std::string(argument) +
                                    " requires an unsigned integer");
      }
      if (argument == "--shared-handle") {
        options.sharedHandle = reinterpret_cast<HANDLE>(
            static_cast<std::uintptr_t>(*parsed));
        foundHandle = true;
      } else if (argument == "--adapter-luid") {
        options.adapterLuid.LowPart = static_cast<DWORD>(*parsed);
        options.adapterLuid.HighPart =
            static_cast<LONG>(static_cast<DWORD>(*parsed >> 32u));
        foundAdapter = true;
      } else if (argument == "--phase") {
        const auto phase = ParsePhase(std::to_string(*parsed));
        if (!phase) {
          throw std::invalid_argument("--phase must be in range 0..4");
        }
        options.phase = *phase;
        foundPhase = true;
      } else {
        if (*parsed > UINT32_MAX) {
          throw std::invalid_argument("--timeout-ms exceeds DWORD range");
        }
        options.timeoutMs = static_cast<DWORD>(*parsed);
      }
      continue;
    }
    throw std::invalid_argument("unknown or incomplete child argument: " +
                                std::string(argument));
  }
  if (!foundHandle || !foundAdapter || !foundPhase) {
    throw std::invalid_argument(
        "--shared-handle, --adapter-luid and --phase are required in child mode");
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
  auto stagingDescription = sharedDescription;
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
                    std::uint8_t stage,
                    bool flush) {
  const auto pixels = MakeSignature(frame, stage);
  device.context->UpdateSubresource(texture,
                                    0,
                                    nullptr,
                                    pixels.data(),
                                    kTextureWidth * kBytesPerPixel,
                                    0);
  if (flush) {
    device.context->Flush();
  }
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
                << " mismatch at row " << y << ", byte " << byte;
        mismatch = message.str();
        break;
      }
    }
  }
  device.context->Unmap(stagingTexture, 0);
  return mismatch;
}

[[nodiscard]] int RunChild(const ChildOptions& options) {
  try {
    const auto adapter = SelectHardwareAdapterByLuid(options.adapterLuid);
    const auto device = CreateDevice(adapter.Get(), options.requestDebugLayer);
    const auto endpoint = OpenSharedTexture(device, options.sharedHandle);

    if (options.phase == FailurePhase::BeforeAcquire) {
      TerminateProcess(GetCurrentProcess(), kChildCompletedExitCode);
      return static_cast<int>(kChildUnexpectedExitCode);
    }

    const HRESULT acquire = endpoint.mutex->AcquireSync(0, options.timeoutMs);
    if (acquire != S_OK) {
      return static_cast<int>(kChildAcquireFailureExitCode);
    }
    if (options.phase == FailurePhase::AfterAcquire) {
      TerminateProcess(GetCurrentProcess(), kChildCompletedExitCode);
      return static_cast<int>(kChildUnexpectedExitCode);
    }

    WriteSignature(device,
                   endpoint.texture.Get(),
                   0,
                   kChildStage,
                   options.phase == FailurePhase::AfterFlush ||
                       options.phase == FailurePhase::AfterRelease);
    if (options.phase == FailurePhase::AfterGpuWrite) {
      TerminateProcess(GetCurrentProcess(), kChildCompletedExitCode);
      return static_cast<int>(kChildUnexpectedExitCode);
    }
    if (options.phase == FailurePhase::AfterFlush) {
      TerminateProcess(GetCurrentProcess(), kChildCompletedExitCode);
      return static_cast<int>(kChildUnexpectedExitCode);
    }

    const HRESULT release = endpoint.mutex->ReleaseSync(1);
    if (release != S_OK) {
      return static_cast<int>(kChildReleaseFailureExitCode);
    }
    return static_cast<int>(kChildCompletedExitCode);
  } catch (...) {
    return static_cast<int>(kChildSetupFailureExitCode);
  }
}

[[nodiscard]] DWORD SpawnChild(HANDLE sharedHandle,
                               LUID adapterLuid,
                               FailurePhase phase,
                               const Options& options) {
  const std::wstring executable = CurrentExecutablePath();
  std::wstring commandLine =
      L"\"" + executable + L"\" --child --shared-handle " +
      std::to_wstring(reinterpret_cast<std::uintptr_t>(sharedHandle)) +
      L" --adapter-luid " + std::to_wstring(PackLuid(adapterLuid)) +
      L" --phase " + std::to_wstring(static_cast<std::uint32_t>(phase)) +
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
  UniqueHandle process(processInfo.hProcess);
  UniqueHandle thread(processInfo.hThread);
  const DWORD wait = WaitForSingleObject(process.get(), options.timeoutMs + 5'000);
  if (wait == WAIT_TIMEOUT) {
    TerminateProcess(process.get(), kChildUnexpectedExitCode);
    WaitForSingleObject(process.get(), options.timeoutMs);
    throw std::runtime_error("child process did not exit within timeout");
  }
  if (wait != WAIT_OBJECT_0) {
    ThrowIfFailed(HRESULT_FROM_WIN32(GetLastError()),
                  "WaitForSingleObject(child)");
  }
  DWORD exitCode = 0;
  if (!GetExitCodeProcess(process.get(), &exitCode)) {
    ThrowIfFailed(HRESULT_FROM_WIN32(GetLastError()), "GetExitCodeProcess");
  }
  return exitCode;
}

void VerifyHealthyHandoff(const DeviceBundle& owner,
                          const DeviceBundle& opener,
                          const D3D11_TEXTURE2D_DESC& description,
                          DWORD timeoutMs) {
  auto fresh = CreateSharedTextureOwner(owner, description);
  auto opened = OpenSharedTexture(opener, fresh.handle.get());
  auto staging = CreateStagingTexture(opener, description);

  ASSERT_EQ(fresh.endpoint.mutex->AcquireSync(0, timeoutMs), S_OK);
  WriteSignature(owner, fresh.endpoint.texture.Get(), 1, kParentStage, true);
  ASSERT_EQ(fresh.endpoint.mutex->ReleaseSync(1), S_OK);
  ASSERT_EQ(opened.mutex->AcquireSync(1, timeoutMs), S_OK);
  ASSERT_FALSE(ValidateSignature(opener,
                                 opened.texture.Get(),
                                 staging.Get(),
                                 1,
                                 kParentStage));
  ASSERT_EQ(opened.mutex->ReleaseSync(0), S_OK);
}

void RunPhase(FailurePhase phase, const Options& options) {
  const auto adapter = SelectHardwareAdapter();
  const auto owner = CreateDevice(adapter.Get(), options.requestDebugLayer);
  const auto opener = CreateDevice(adapter.Get(), options.requestDebugLayer);
  const auto description = SharedTextureDescription();
  SECURITY_ATTRIBUTES security{};
  security.nLength = sizeof(security);
  security.bInheritHandle = TRUE;

  auto shared = CreateSharedTextureOwner(owner, description, &security);
  const DWORD childExit =
      SpawnChild(shared.handle.get(), GetAdapterLuid(adapter.Get()), phase,
                 options);
  ASSERT_EQ(childExit, kChildCompletedExitCode)
      << "phase " << PhaseName(phase) << " child exit code " << childExit;

  std::cout << "PHASE: " << PhaseName(phase) << " on "
            << AdapterName(adapter.Get()) << '\n';
  if (phase == FailurePhase::BeforeAcquire) {
    const HRESULT acquire =
        shared.endpoint.mutex->AcquireSync(0, options.timeoutMs);
    ASSERT_EQ(acquire, S_OK) << HResultText(acquire);
    ASSERT_EQ(shared.endpoint.mutex->ReleaseSync(0), S_OK);
    std::cout << "      parent AcquireSync(0): S_OK; resource remained healthy\n"
              << "      data visibility: no child GPU write\n"
              << "      recovery: continue using the original resource\n";
    VerifyHealthyHandoff(owner, opener, description, options.timeoutMs);
    return;
  }

  if (phase == FailurePhase::AfterRelease) {
    auto staging = CreateStagingTexture(owner, description);
    const HRESULT acquire =
        shared.endpoint.mutex->AcquireSync(1, options.timeoutMs);
    ASSERT_EQ(acquire, S_OK) << HResultText(acquire);
    ASSERT_FALSE(ValidateSignature(owner,
                                   shared.endpoint.texture.Get(),
                                   staging.Get(),
                                   0,
                                   kChildStage));
    ASSERT_EQ(shared.endpoint.mutex->ReleaseSync(0), S_OK);
    std::cout << "      parent AcquireSync(1): S_OK\n"
              << "      data visibility: child signature was visible after "
                 "Flush and ReleaseSync(1)\n"
              << "      recovery: continue using the original resource\n";
    return;
  }

  const HRESULT acquire =
      shared.endpoint.mutex->AcquireSync(0, options.timeoutMs);
  EXPECT_EQ(acquire, static_cast<HRESULT>(WAIT_ABANDONED))
      << "parent AcquireSync(0): " << HResultText(acquire);
  const std::string acquireText =
      acquire == static_cast<HRESULT>(WAIT_ABANDONED)
          ? "WAIT_ABANDONED (0x80)"
          : HResultText(acquire);
  std::cout << "      parent AcquireSync(0): " << acquireText << '\n'
            << "      data visibility: not inspected because abandoned "
               "resource is discarded\n"
            << "      recovery: discard shared resource and recreate it\n";
  VerifyHealthyHandoff(owner, opener, description, options.timeoutMs);
}

TEST(D3D11KeyedMutex, FailureBeforeAcquire) {
  RunPhase(FailurePhase::BeforeAcquire, gOptions);
}

TEST(D3D11KeyedMutex, FailureAfterAcquire) {
  RunPhase(FailurePhase::AfterAcquire, gOptions);
}

TEST(D3D11KeyedMutex, FailureAfterGpuWrite) {
  RunPhase(FailurePhase::AfterGpuWrite, gOptions);
}

TEST(D3D11KeyedMutex, FailureAfterFlush) {
  RunPhase(FailurePhase::AfterFlush, gOptions);
}

TEST(D3D11KeyedMutex, FailureAfterRelease) {
  RunPhase(FailurePhase::AfterRelease, gOptions);
}

}  // namespace

int main(int argc, char* argv[]) {
  const auto arguments = std::span(argv, static_cast<std::size_t>(argc));
  if (HasArgument(arguments, "--child")) {
    try {
      return RunChild(ParseChildOptions(arguments));
    } catch (...) {
      return static_cast<int>(kChildSetupFailureExitCode);
    }
  }

  ::testing::InitGoogleTest(&argc, argv);
  try {
    gOptions = ParseOptions(
        std::span(argv, static_cast<std::size_t>(argc)));
  } catch (const std::exception& exception) {
    std::cerr << "ERROR: " << exception.what() << '\n';
    return 2;
  }
  return RUN_ALL_TESTS();
}
