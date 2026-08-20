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

using keyed_mutex::test::AdapterName;
using keyed_mutex::test::CreateDevice;
using keyed_mutex::test::CreateSharedTexturePair;
using keyed_mutex::test::HResultText;
using keyed_mutex::test::SelectHardwareAdapter;

namespace {

struct Options {
  DWORD timeoutMs = 25;
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
    if (argument == "--timeout-ms" && index + 1 < arguments.size()) {
      const auto parsed = ParseUint(arguments[++index]);
      if (!parsed || *parsed == 0) {
        throw std::invalid_argument(
            "--timeout-ms requires a positive integer");
      }
      options.timeoutMs = *parsed;
      continue;
    }
    throw std::invalid_argument("unknown or incomplete argument: " +
                                std::string(argument));
  }
  return options;
}

void RunExperiment(const Options& options) {
  const auto adapter = SelectHardwareAdapter();
  const auto owner = CreateDevice(adapter.Get(), options.requestDebugLayer);
  const auto opener = CreateDevice(adapter.Get(), options.requestDebugLayer);

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

  const auto shared = CreateSharedTexturePair(owner, opener, description);
  const HRESULT timeout = static_cast<HRESULT>(WAIT_TIMEOUT);

  std::cout << "D3D11 keyed-mutex key state machine on "
            << AdapterName(adapter.Get()) << '\n';

  const HRESULT initialWrongKey =
      shared.openedMutex->AcquireSync(7, options.timeoutMs);
  EXPECT_EQ(initialWrongKey, timeout)
      << "initial AcquireSync(7): " << HResultText(initialWrongKey);
  EXPECT_TRUE(SUCCEEDED(initialWrongKey))
      << "WAIT_TIMEOUT must be checked explicitly instead of with SUCCEEDED";

  const HRESULT initialAcquire =
      shared.ownerMutex->AcquireSync(0, options.timeoutMs);
  ASSERT_EQ(initialAcquire, S_OK)
      << "initial AcquireSync(0): " << HResultText(initialAcquire);

  const HRESULT whileOwned =
      shared.openedMutex->AcquireSync(1, options.timeoutMs);
  EXPECT_EQ(whileOwned, timeout)
      << "AcquireSync(1) while owned: " << HResultText(whileOwned);

  const HRESULT release42 = shared.ownerMutex->ReleaseSync(42);
  ASSERT_EQ(release42, S_OK)
      << "owner ReleaseSync(42): " << HResultText(release42);

  const HRESULT wrongReleasedKey =
      shared.openedMutex->AcquireSync(1, options.timeoutMs);
  EXPECT_EQ(wrongReleasedKey, timeout)
      << "post-release AcquireSync(1): " << HResultText(wrongReleasedKey);

  const HRESULT acquire42 =
      shared.openedMutex->AcquireSync(42, options.timeoutMs);
  ASSERT_EQ(acquire42, S_OK)
      << "post-release AcquireSync(42): " << HResultText(acquire42);

  const HRESULT nonOwnerRelease = shared.ownerMutex->ReleaseSync(0);
  EXPECT_TRUE(FAILED(nonOwnerRelease))
      << "non-owner ReleaseSync(0) unexpectedly succeeded";
  if (nonOwnerRelease != E_FAIL) {
    std::cout << "NOTE: documentation names " << HResultText(E_FAIL)
              << ", but this runtime returned "
              << HResultText(nonOwnerRelease) << '\n';
  }

  const HRESULT currentOwnerRelease = shared.openedMutex->ReleaseSync(0);
  ASSERT_EQ(currentOwnerRelease, S_OK)
      << "current owner ReleaseSync(0): "
      << HResultText(currentOwnerRelease);

  const HRESULT reacquire0 =
      shared.ownerMutex->AcquireSync(0, options.timeoutMs);
  ASSERT_EQ(reacquire0, S_OK)
      << "original device AcquireSync(0): " << HResultText(reacquire0);

  const HRESULT finalRelease = shared.ownerMutex->ReleaseSync(0);
  EXPECT_EQ(finalRelease, S_OK)
      << "final ReleaseSync(0): " << HResultText(finalRelease);
}

TEST(D3D11KeyedMutex, KeyStateMachine) { RunExperiment(gOptions); }

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
