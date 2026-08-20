#include "keyed_mutex/d3d11_test_support.hpp"

#include <gtest/gtest.h>

#include <array>
#include <charconv>
#include <cstdint>
#include <iostream>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

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

constexpr UINT kTextureWidth = 4;
constexpr UINT kTextureHeight = 4;
constexpr std::uint32_t kSignature = 0x1455'0001u;

struct Options {
  DWORD timeoutMs = 1'000;
  bool requestDebugLayer = true;
};

Options gOptions;

[[nodiscard]] std::optional<std::uint32_t> ParseUint(std::string_view value) {
  std::uint32_t result = 0;
  const auto parsed =
      std::from_chars(value.data(), value.data() + value.size(), result);
  if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) {
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
        throw std::invalid_argument("--timeout-ms requires a positive integer");
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
  description.Width = kTextureWidth;
  description.Height = kTextureHeight;
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

[[nodiscard]] ComPtr<ID3D11Texture2D>
CreateStagingTexture(const DeviceBundle& device) {
  auto description = SharedDescription();
  description.Usage = D3D11_USAGE_STAGING;
  description.BindFlags = 0;
  description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  description.MiscFlags = 0;
  ComPtr<ID3D11Texture2D> staging;
  ThrowIfFailed(device.device->CreateTexture2D(&description, nullptr, &staging),
                "CreateTexture2D(staging)");
  return staging;
}

void WriteSignature(const DeviceBundle& device, ID3D11Texture2D* texture,
                    std::uint32_t signature) {
  std::array<std::uint32_t, kTextureWidth * kTextureHeight> pixels{};
  pixels.fill(signature);
  const D3D11_BOX box = {0, 0, 0, kTextureWidth, kTextureHeight, 1};
  device.context->UpdateSubresource(texture, 0, &box, pixels.data(),
                                    kTextureWidth * sizeof(std::uint32_t), 0);
  device.context->Flush();
}

[[nodiscard]] std::uint32_t ReadSignature(const DeviceBundle& device,
                                           ID3D11Texture2D* texture,
                                           ID3D11Texture2D* staging) {
  device.context->CopyResource(staging, texture);
  device.context->Flush();
  D3D11_MAPPED_SUBRESOURCE mapped{};
  ThrowIfFailed(device.context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped),
                "Map(staging)");
  const auto value = *static_cast<const std::uint32_t*>(mapped.pData);
  device.context->Unmap(staging, 0);
  return value;
}

void Report(const char* title, IDXGIAdapter1* adapter, HRESULT reopenResult,
            bool endpointUsable, bool dataVisible) {
  std::cout << "OBSERVED: " << title << " on " << AdapterName(adapter) << '\n'
            << "      opened endpoint usable: " << (endpointUsable ? "yes" : "no")
            << '\n'
            << "      signature visible: " << (dataVisible ? "yes" : "no")
            << '\n';
  if (reopenResult != E_UNEXPECTED) {
    std::cout << "      reopen with closed handle: "
              << HResultText(reopenResult) << '\n';
  }
}

TEST(D3D11KeyedMutex, OriginalHandleClosedAfterOpen) {
  const auto adapter = SelectHardwareAdapter();
  const auto creator = CreateDevice(adapter.Get(), gOptions.requestDebugLayer);
  const auto opener = CreateDevice(adapter.Get(), gOptions.requestDebugLayer);
  auto owner = CreateSharedTextureOwner(creator, SharedDescription());
  auto opened = OpenSharedTexture(opener, owner.handle.get());
  owner.handle.reset();

  ASSERT_EQ(owner.handle.get(), nullptr);
  ASSERT_EQ(owner.endpoint.mutex->AcquireSync(0, gOptions.timeoutMs), S_OK);
  WriteSignature(creator, owner.endpoint.texture.Get(), kSignature);
  ASSERT_EQ(owner.endpoint.mutex->ReleaseSync(1), S_OK);

  ASSERT_EQ(opened.mutex->AcquireSync(1, gOptions.timeoutMs), S_OK);
  auto staging = CreateStagingTexture(opener);
  const auto observed = ReadSignature(opener, opened.texture.Get(), staging.Get());
  ASSERT_EQ(opened.mutex->ReleaseSync(0), S_OK);
  ASSERT_EQ(owner.endpoint.mutex->AcquireSync(0, gOptions.timeoutMs), S_OK);
  ASSERT_EQ(owner.endpoint.mutex->ReleaseSync(1), S_OK);

  EXPECT_EQ(observed, kSignature);
  Report("original NT handle closed after OpenSharedResource1", adapter.Get(),
         E_UNEXPECTED, true, observed == kSignature);
}

TEST(D3D11KeyedMutex, CreatorTextureAndDeviceDestroyedAfterOpen) {
  const auto adapter = SelectHardwareAdapter();
  const auto opener = CreateDevice(adapter.Get(), gOptions.requestDebugLayer);
  SharedTextureEndpoint opened;
  {
    const auto creator = CreateDevice(adapter.Get(), gOptions.requestDebugLayer);
    auto owner = CreateSharedTextureOwner(creator, SharedDescription());
    opened = OpenSharedTexture(opener, owner.handle.get());
    ASSERT_EQ(owner.endpoint.mutex->AcquireSync(0, gOptions.timeoutMs), S_OK);
    WriteSignature(creator, owner.endpoint.texture.Get(), kSignature);
    ASSERT_EQ(owner.endpoint.mutex->ReleaseSync(1), S_OK);
  }

  auto staging = CreateStagingTexture(opener);
  ASSERT_EQ(opened.mutex->AcquireSync(1, gOptions.timeoutMs), S_OK);
  const auto observed = ReadSignature(opener, opened.texture.Get(), staging.Get());
  ASSERT_EQ(opened.mutex->ReleaseSync(0), S_OK);
  EXPECT_EQ(observed, kSignature);
  Report("creator texture/device destroyed after open", adapter.Get(),
         E_UNEXPECTED, true, observed == kSignature);
}

TEST(D3D11KeyedMutex, LastReferenceReleased) {
  const auto adapter = SelectHardwareAdapter();
  HANDLE staleHandle = nullptr;
  SharedTextureEndpoint opened;
  {
    const auto creator = CreateDevice(adapter.Get(), gOptions.requestDebugLayer);
    const auto opener = CreateDevice(adapter.Get(), gOptions.requestDebugLayer);
    auto owner = CreateSharedTextureOwner(creator, SharedDescription());
    staleHandle = owner.handle.get();
    opened = OpenSharedTexture(opener, staleHandle);
    owner.handle.reset();
    ASSERT_EQ(opened.mutex->AcquireSync(0, gOptions.timeoutMs), S_OK);
    ASSERT_EQ(opened.mutex->ReleaseSync(0), S_OK);
  }

  // The opener endpoint is the only remaining COM reference here.
  const auto opener = CreateDevice(adapter.Get(), gOptions.requestDebugLayer);
  ASSERT_EQ(opened.mutex->AcquireSync(0, gOptions.timeoutMs), S_OK);
  ASSERT_EQ(opened.mutex->ReleaseSync(0), S_OK);
  opened = {};

  ComPtr<ID3D11Texture2D> reopened;
  const HRESULT reopenResult = opener.device1->OpenSharedResource1(
      staleHandle, IID_PPV_ARGS(&reopened));
  EXPECT_TRUE(FAILED(reopenResult)) << HResultText(reopenResult);

  auto freshOwner = CreateSharedTextureOwner(opener, SharedDescription());
  auto freshOpened = OpenSharedTexture(opener, freshOwner.handle.get());
  ASSERT_EQ(freshOpened.mutex->AcquireSync(0, gOptions.timeoutMs), S_OK);
  EXPECT_EQ(freshOpened.mutex->ReleaseSync(0), S_OK);

  Report("last COM reference released", adapter.Get(), reopenResult, true,
         true);
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
