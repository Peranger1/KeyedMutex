#include "keyed_mutex/d3d11_test_support.hpp"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <barrier>
#include <charconv>
#include <cstdint>
#include <exception>
#include <iostream>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

using keyed_mutex::test::AdapterName;
using keyed_mutex::test::ComPtr;
using keyed_mutex::test::CreateDevice;
using keyed_mutex::test::CreateSharedTextureOwner;
using keyed_mutex::test::DeviceBundle;
using keyed_mutex::test::HResultText;
using keyed_mutex::test::OpenSharedTexture;
using keyed_mutex::test::SelectHardwareAdapter;
using keyed_mutex::test::SharedTextureEndpoint;
using keyed_mutex::test::SharedTextureOwner;
using keyed_mutex::test::ThrowIfFailed;

namespace {

struct Options {
  DWORD timeoutMs = 250;
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

[[nodiscard]] D3D11_TEXTURE2D_DESC SharedDescription() {
  D3D11_TEXTURE2D_DESC description{};
  description.Width = 1;
  description.Height = 1;
  description.MipLevels = 1;
  description.ArraySize = 1;
  description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  description.SampleDesc.Count = 1;
  description.Usage = D3D11_USAGE_DEFAULT;
  description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  description.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE |
                          D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
  return description;
}

struct MultipleOpenResource {
  SharedTextureOwner owner;
  SharedTextureEndpoint first;
  SharedTextureEndpoint second;
};

[[nodiscard]] MultipleOpenResource
CreateMultipleOpenResource(const DeviceBundle& creator,
                           const DeviceBundle& opener) {
  MultipleOpenResource resource;
  resource.owner = CreateSharedTextureOwner(creator, SharedDescription());
  resource.first = OpenSharedTexture(opener, resource.owner.handle.get());
  resource.second = OpenSharedTexture(opener, resource.owner.handle.get());
  return resource;
}

struct IdentityObservation {
  bool sameTexturePointer = false;
  bool sameMutexPointer = false;
  bool sameTextureIdentity = false;
  bool sameMutexIdentity = false;
};

[[nodiscard]] IdentityObservation
ObserveIdentity(const MultipleOpenResource& resource) {
  ComPtr<IUnknown> firstTextureIdentity;
  ComPtr<IUnknown> secondTextureIdentity;
  ComPtr<IUnknown> firstMutexIdentity;
  ComPtr<IUnknown> secondMutexIdentity;
  ThrowIfFailed(resource.first.texture.As(&firstTextureIdentity),
                "first texture QueryInterface(IUnknown)");
  ThrowIfFailed(resource.second.texture.As(&secondTextureIdentity),
                "second texture QueryInterface(IUnknown)");
  ThrowIfFailed(resource.first.mutex.As(&firstMutexIdentity),
                "first mutex QueryInterface(IUnknown)");
  ThrowIfFailed(resource.second.mutex.As(&secondMutexIdentity),
                "second mutex QueryInterface(IUnknown)");

  return {
      resource.first.texture.Get() == resource.second.texture.Get(),
      resource.first.mutex.Get() == resource.second.mutex.Get(),
      firstTextureIdentity.Get() == secondTextureIdentity.Get(),
      firstMutexIdentity.Get() == secondMutexIdentity.Get(),
  };
}

void ReportIdentity(const IdentityObservation& identity) {
  std::cout << "      same ID3D11Texture2D pointer: "
            << (identity.sameTexturePointer ? "yes" : "no") << '\n'
            << "      same IDXGIKeyedMutex pointer: "
            << (identity.sameMutexPointer ? "yes" : "no") << '\n'
            << "      same texture IUnknown identity: "
            << (identity.sameTextureIdentity ? "yes" : "no") << '\n'
            << "      same mutex IUnknown identity: "
            << (identity.sameMutexIdentity ? "yes" : "no") << '\n';
}

void RunMultipleOpenIdentity(const Options& options) {
  const auto adapter = SelectHardwareAdapter();
  const auto creator = CreateDevice(adapter.Get(), options.requestDebugLayer);
  const auto opener = CreateDevice(adapter.Get(), options.requestDebugLayer);
  const auto resource = CreateMultipleOpenResource(creator, opener);
  const auto identity = ObserveIdentity(resource);

  std::cout << "OBSERVED: two OpenSharedResource1 calls on one device on "
            << AdapterName(adapter.Get()) << '\n';
  ReportIdentity(identity);

  const HRESULT acquire =
      resource.first.mutex->AcquireSync(0, options.timeoutMs);
  ASSERT_EQ(acquire, S_OK) << HResultText(acquire);
  EXPECT_EQ(resource.first.mutex->ReleaseSync(0), S_OK);
}

struct AcquireAttempt {
  HRESULT acquire = E_UNEXPECTED;
  HRESULT release = E_UNEXPECTED;
};

void RunConcurrentAcquire(const Options& options) {
  const auto adapter = SelectHardwareAdapter();
  const auto creator = CreateDevice(adapter.Get(), options.requestDebugLayer);
  const auto opener = CreateDevice(adapter.Get(), options.requestDebugLayer);
  const auto resource = CreateMultipleOpenResource(creator, opener);
  const auto identity = ObserveIdentity(resource);

  std::barrier<> startGate(2);
  std::barrier<> attemptsFinished(2);
  std::atomic<unsigned> activeOwners = 0;
  std::atomic<bool> overlap = false;
  std::array<AcquireAttempt, 2> attempts;
  std::array<std::thread, 2> threads;
  std::array<IDXGIKeyedMutex*, 2> mutexes = {
      resource.first.mutex.Get(), resource.second.mutex.Get()};

  for (std::size_t index = 0; index < threads.size(); ++index) {
    threads[index] = std::thread([&, index] {
      startGate.arrive_and_wait();
      attempts[index].acquire =
          mutexes[index]->AcquireSync(0, options.timeoutMs);
      if (attempts[index].acquire == S_OK &&
          activeOwners.fetch_add(1) != 0) {
        overlap.store(true);
      }
      attemptsFinished.arrive_and_wait();
      if (attempts[index].acquire == S_OK) {
        attempts[index].release = mutexes[index]->ReleaseSync(0);
        activeOwners.fetch_sub(1);
      }
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }

  unsigned successes = 0;
  for (const auto& attempt : attempts) {
    if (attempt.acquire == S_OK) {
      ++successes;
      EXPECT_EQ(attempt.release, S_OK) << HResultText(attempt.release);
    }
  }
  EXPECT_LE(successes, 1u);
  EXPECT_FALSE(overlap.load());
  EXPECT_EQ(activeOwners.load(), 0u);

  const HRESULT recovery =
      resource.first.mutex->AcquireSync(0, options.timeoutMs);
  ASSERT_EQ(recovery, S_OK) << HResultText(recovery);
  EXPECT_EQ(resource.first.mutex->ReleaseSync(0), S_OK);

  std::cout << "OBSERVED: concurrent AcquireSync(0) through two opens on "
            << AdapterName(adapter.Get()) << '\n';
  ReportIdentity(identity);
  for (std::size_t index = 0; index < attempts.size(); ++index) {
    std::cout << "      interface " << index << " AcquireSync: "
              << HResultText(attempts[index].acquire) << '\n';
  }
  std::cout << "      simultaneous successful owners: "
            << (overlap.load() ? "yes" : "no") << '\n'
            << "      recovery AcquireSync(0): S_OK\n";
}

void RunCrossInterfaceRelease(const Options& options) {
  const auto adapter = SelectHardwareAdapter();
  const auto creator = CreateDevice(adapter.Get(), options.requestDebugLayer);
  const auto opener = CreateDevice(adapter.Get(), options.requestDebugLayer);
  const auto resource = CreateMultipleOpenResource(creator, opener);
  const auto identity = ObserveIdentity(resource);

  const HRESULT acquireFirst =
      resource.first.mutex->AcquireSync(0, options.timeoutMs);
  ASSERT_EQ(acquireFirst, S_OK) << HResultText(acquireFirst);

  const HRESULT crossRelease = resource.second.mutex->ReleaseSync(1);
  HRESULT ownerCleanup = E_UNEXPECTED;
  if (crossRelease != S_OK) {
    ownerCleanup = resource.first.mutex->ReleaseSync(1);
    ASSERT_EQ(ownerCleanup, S_OK) << HResultText(ownerCleanup);
  }

  const HRESULT followupAcquire =
      resource.second.mutex->AcquireSync(1, options.timeoutMs);
  ASSERT_EQ(followupAcquire, S_OK) << HResultText(followupAcquire);
  const HRESULT followupRelease = resource.second.mutex->ReleaseSync(0);
  EXPECT_EQ(followupRelease, S_OK) << HResultText(followupRelease);

  std::cout << "OBSERVED: first interface acquired key 0; second interface "
               "ReleaseSync(1) returned "
            << HResultText(crossRelease) << " on "
            << AdapterName(adapter.Get()) << '\n';
  ReportIdentity(identity);
  if (crossRelease == S_OK) {
    std::cout << "      ownership accepted across the two interface "
                 "references\n";
  } else {
    std::cout << "      ownership rejected across interfaces; original "
                 "interface cleanup succeeded\n";
  }
  std::cout << "      follow-up AcquireSync(1) and ReleaseSync(0): S_OK\n";
}

TEST(D3D11KeyedMutex, MultipleOpenInterfaceIdentity) {
  RunMultipleOpenIdentity(gOptions);
}

TEST(D3D11KeyedMutex, MultipleOpenConcurrentAcquire) {
  RunConcurrentAcquire(gOptions);
}

TEST(D3D11KeyedMutex, MultipleOpenCrossInterfaceRelease) {
  RunCrossInterfaceRelease(gOptions);
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
