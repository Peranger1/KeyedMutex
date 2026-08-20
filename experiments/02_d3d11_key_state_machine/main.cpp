#include "keyed_mutex/d3d11_test_support.hpp"

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

class Checks {
public:
  void ExpectHr(std::string_view name, HRESULT actual, HRESULT expected) {
    ++total_;
    if (actual == expected) {
      std::cout << "  PASS  " << name << " -> " << HResultText(actual)
                << '\n';
      return;
    }

    ++failed_;
    std::cerr << "  FAIL  " << name << " -> expected "
              << HResultText(expected) << ", got " << HResultText(actual)
              << '\n';
  }

  void ExpectTrue(std::string_view name, bool actual) {
    ++total_;
    if (actual) {
      std::cout << "  PASS  " << name << '\n';
      return;
    }

    ++failed_;
    std::cerr << "  FAIL  " << name << '\n';
  }

  void ExpectFailure(std::string_view name,
                     HRESULT actual,
                     HRESULT documentedCode) {
    ++total_;
    if (FAILED(actual)) {
      std::cout << "  PASS  " << name << " -> " << HResultText(actual)
                << '\n';
      if (actual != documentedCode) {
        std::cout << "        NOTE: documentation names "
                  << HResultText(documentedCode)
                  << ", but this runtime returned a different failure code\n";
      }
      return;
    }

    ++failed_;
    std::cerr << "  FAIL  " << name << " -> expected a failing HRESULT, got "
              << HResultText(actual) << '\n';
  }

  [[nodiscard]] int Finish() const {
    if (failed_ != 0) {
      std::cerr << "FAIL: " << failed_ << " of " << total_
                << " checks failed\n";
      return 1;
    }
    std::cout << "PASS: all " << total_ << " checks passed\n";
    return 0;
  }

private:
  unsigned total_ = 0;
  unsigned failed_ = 0;
};

int Run(const Options& options) {
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

  Checks checks;

  const HRESULT initialWrongKey =
      shared.openedMutex->AcquireSync(7, options.timeoutMs);
  checks.ExpectHr("initial AcquireSync(7) times out",
                  initialWrongKey,
                  timeout);
  checks.ExpectTrue("SUCCEEDED(WAIT_TIMEOUT) is true; explicit comparison is required",
                    SUCCEEDED(initialWrongKey));

  checks.ExpectHr("initial AcquireSync(0) succeeds",
                  shared.ownerMutex->AcquireSync(0, options.timeoutMs),
                  S_OK);
  checks.ExpectHr("AcquireSync(1) while another device owns the resource times out",
                  shared.openedMutex->AcquireSync(1, options.timeoutMs),
                  timeout);
  checks.ExpectHr("owner ReleaseSync(42) succeeds",
                  shared.ownerMutex->ReleaseSync(42),
                  S_OK);

  checks.ExpectHr("post-release AcquireSync(1) uses the wrong key and times out",
                  shared.openedMutex->AcquireSync(1, options.timeoutMs),
                  timeout);
  checks.ExpectHr("post-release AcquireSync(42) succeeds",
                  shared.openedMutex->AcquireSync(42, options.timeoutMs),
                  S_OK);
  checks.ExpectFailure("non-owner ReleaseSync(0) fails",
                       shared.ownerMutex->ReleaseSync(0),
                       E_FAIL);
  checks.ExpectHr("current owner ReleaseSync(0) succeeds",
                  shared.openedMutex->ReleaseSync(0),
                  S_OK);

  checks.ExpectHr("original device can reacquire key 0",
                  shared.ownerMutex->AcquireSync(0, options.timeoutMs),
                  S_OK);
  checks.ExpectHr("final ReleaseSync(0) succeeds",
                  shared.ownerMutex->ReleaseSync(0),
                  S_OK);

  return checks.Finish();
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
