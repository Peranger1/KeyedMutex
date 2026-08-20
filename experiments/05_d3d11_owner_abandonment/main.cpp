#include "keyed_mutex/d3d11_test_support.hpp"

#include <gtest/gtest.h>

#include <charconv>
#include <cstdint>
#include <exception>
#include <iostream>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using keyed_mutex::test::AdapterName;
using keyed_mutex::test::CreateDevice;
using keyed_mutex::test::CreateSharedTextureOwner;
using keyed_mutex::test::CreateSharedTexturePair;
using keyed_mutex::test::DeviceBundle;
using keyed_mutex::test::HResultText;
using keyed_mutex::test::OpenSharedTexture;
using keyed_mutex::test::SelectHardwareAdapter;
using keyed_mutex::test::ThrowIfFailed;
using keyed_mutex::test::UniqueHandle;

namespace {

constexpr DWORD kChildAcquiredExitCode = 77;
constexpr DWORD kChildSetupFailureExitCode = 78;
constexpr DWORD kChildAcquireFailureExitCode = 79;
constexpr DWORD kChildTerminationFailureExitCode = 80;

struct Options {
  std::uint32_t iterations = 3;
  DWORD timeoutMs = 5'000;
  bool requestDebugLayer = true;
};

struct ChildOptions {
  HANDLE sharedHandle = nullptr;
  DWORD timeoutMs = 5'000;
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
      const auto parsed = ParseUint64(arguments[++index]);
      if (!parsed || *parsed == 0 || *parsed > UINT32_MAX) {
        throw std::invalid_argument(std::string(argument) +
                                    " requires a positive 32-bit integer");
      }
      if (argument == "--iterations") {
        options.iterations = static_cast<std::uint32_t>(*parsed);
      } else {
        options.timeoutMs = static_cast<DWORD>(*parsed);
      }
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
  for (std::size_t index = 1; index < arguments.size(); ++index) {
    const std::string_view argument(arguments[index]);
    if (argument == "--child") {
      continue;
    }
    if (argument == "--no-debug-layer") {
      options.requestDebugLayer = false;
      continue;
    }
    if ((argument == "--shared-handle" || argument == "--timeout-ms") &&
        index + 1 < arguments.size()) {
      const auto parsed = ParseUint64(arguments[++index]);
      if (!parsed || *parsed == 0) {
        throw std::invalid_argument(std::string(argument) +
                                    " requires a positive integer");
      }
      if (argument == "--shared-handle") {
        options.sharedHandle = reinterpret_cast<HANDLE>(
            static_cast<std::uintptr_t>(*parsed));
        foundHandle = true;
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
  if (!foundHandle) {
    throw std::invalid_argument("--shared-handle is required in child mode");
  }
  return options;
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

[[nodiscard]] int RunChild(const ChildOptions& options) {
  try {
    const auto adapter = SelectHardwareAdapter();
    const auto device = CreateDevice(adapter.Get(), options.requestDebugLayer);
    const auto endpoint = OpenSharedTexture(device, options.sharedHandle);
    const HRESULT acquire = endpoint.mutex->AcquireSync(0, options.timeoutMs);
    if (acquire != S_OK) {
      return static_cast<int>(kChildAcquireFailureExitCode);
    }

    if (!TerminateProcess(GetCurrentProcess(), kChildAcquiredExitCode)) {
      return static_cast<int>(kChildTerminationFailureExitCode);
    }
    return static_cast<int>(kChildTerminationFailureExitCode);
  } catch (...) {
    return static_cast<int>(kChildSetupFailureExitCode);
  }
}

[[nodiscard]] DWORD SpawnAbruptOwner(HANDLE sharedHandle,
                                     const Options& options) {
  const std::wstring executable = CurrentExecutablePath();
  std::wstring commandLine =
      L"\"" + executable + L"\" --child --shared-handle " +
      std::to_wstring(reinterpret_cast<std::uintptr_t>(sharedHandle)) +
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
  const DWORD waitLimit = options.timeoutMs + 5'000;
  const DWORD wait = WaitForSingleObject(process.get(), waitLimit);
  if (wait == WAIT_TIMEOUT) {
    TerminateProcess(process.get(), kChildTerminationFailureExitCode);
    WaitForSingleObject(process.get(), options.timeoutMs);
    throw std::runtime_error("child process did not terminate within timeout");
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

[[nodiscard]] std::optional<std::string>
VerifyFreshResource(const DeviceBundle& owner,
                    const DeviceBundle& opener,
                    const D3D11_TEXTURE2D_DESC& description,
                    DWORD timeoutMs) {
  const auto fresh = CreateSharedTexturePair(owner, opener, description);

  const HRESULT acquire0 = fresh.ownerMutex->AcquireSync(0, timeoutMs);
  if (acquire0 != S_OK) {
    return "fresh owner AcquireSync(0): " + HResultText(acquire0);
  }

  const HRESULT release9 = fresh.ownerMutex->ReleaseSync(9);
  if (release9 != S_OK) {
    return "fresh owner ReleaseSync(9): " + HResultText(release9);
  }

  const HRESULT acquire9 = fresh.openedMutex->AcquireSync(9, timeoutMs);
  if (acquire9 != S_OK) {
    return "fresh opener AcquireSync(9): " + HResultText(acquire9);
  }

  const HRESULT release0 = fresh.openedMutex->ReleaseSync(0);
  if (release0 != S_OK) {
    return "fresh opener ReleaseSync(0): " + HResultText(release0);
  }
  return std::nullopt;
}

void RunExperiment(const Options& options) {
  const auto adapter = SelectHardwareAdapter();
  const auto owner = CreateDevice(adapter.Get(), options.requestDebugLayer);
  const auto opener = CreateDevice(adapter.Get(), options.requestDebugLayer);
  const auto description = SharedTextureDescription();

  SECURITY_ATTRIBUTES security{};
  security.nLength = sizeof(security);
  security.bInheritHandle = TRUE;

  for (std::uint32_t iteration = 0; iteration < options.iterations;
       ++iteration) {
    auto abandoned =
        CreateSharedTextureOwner(owner, description, &security);
    const DWORD childExit =
        SpawnAbruptOwner(abandoned.handle.get(), options);
    ASSERT_EQ(childExit, kChildAcquiredExitCode)
        << "child failed before abandoning iteration " << iteration;

    const HRESULT acquire =
        abandoned.endpoint.mutex->AcquireSync(0, options.timeoutMs);
    if (acquire == S_OK) {
      abandoned.endpoint.mutex->ReleaseSync(0);
    }
    ASSERT_EQ(acquire, static_cast<HRESULT>(WAIT_ABANDONED))
        << "iteration " << iteration << " returned "
        << HResultText(acquire);
    EXPECT_TRUE(SUCCEEDED(acquire))
        << "WAIT_ABANDONED must be compared explicitly";
  }

  if (auto recoveryError =
          VerifyFreshResource(owner, opener, description, options.timeoutMs)) {
    FAIL() << *recoveryError;
  }
  std::cout << "PASS: observed " << options.iterations
            << " abandoned owners and rebuilt a healthy shared resource on "
            << AdapterName(adapter.Get()) << '\n';
}

TEST(D3D11KeyedMutex, OwnerAbandonmentAndRecovery) {
  RunExperiment(gOptions);
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
    gOptions = ParseOptions(std::span(argv, static_cast<std::size_t>(argc)));
  } catch (const std::exception& exception) {
    std::cerr << "ERROR: " << exception.what() << '\n';
    return 2;
  }
  return RUN_ALL_TESTS();
}
