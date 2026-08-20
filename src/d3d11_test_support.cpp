#include "keyed_mutex/d3d11_test_support.hpp"

#include <cstdint>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace keyed_mutex::test {

UniqueHandle::UniqueHandle(HANDLE handle) noexcept : handle_(handle) {}

UniqueHandle::UniqueHandle(UniqueHandle&& other) noexcept
    : handle_(std::exchange(other.handle_, nullptr)) {}

UniqueHandle& UniqueHandle::operator=(UniqueHandle&& other) noexcept {
  if (this != &other) {
    reset();
    handle_ = std::exchange(other.handle_, nullptr);
  }
  return *this;
}

UniqueHandle::~UniqueHandle() { reset(); }

HANDLE UniqueHandle::get() const noexcept { return handle_; }

void UniqueHandle::reset(HANDLE replacement) noexcept {
  if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
    CloseHandle(handle_);
  }
  handle_ = replacement;
}

std::string HResultText(HRESULT hr) {
  std::ostringstream stream;
  stream << "0x" << std::hex << std::uppercase
         << static_cast<std::uint32_t>(hr);

  LPWSTR message = nullptr;
  const DWORD length = FormatMessageW(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
          FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr,
      static_cast<DWORD>(hr),
      MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
      reinterpret_cast<LPWSTR>(&message),
      0,
      nullptr);
  if (length != 0 && message != nullptr) {
    std::wstring wideText(message, length);
    while (!wideText.empty() &&
           (wideText.back() == L'\r' || wideText.back() == L'\n')) {
      wideText.pop_back();
    }
    const int utf8Length = WideCharToMultiByte(
        CP_UTF8,
        0,
        wideText.data(),
        static_cast<int>(wideText.size()),
        nullptr,
        0,
        nullptr,
        nullptr);
    if (utf8Length > 0) {
      std::string text(static_cast<std::size_t>(utf8Length), '\0');
      WideCharToMultiByte(CP_UTF8,
                          0,
                          wideText.data(),
                          static_cast<int>(wideText.size()),
                          text.data(),
                          utf8Length,
                          nullptr,
                          nullptr);
      stream << " (" << text << ')';
    }
  }
  if (message != nullptr) {
    LocalFree(message);
  }
  return stream.str();
}

void ThrowIfFailed(HRESULT hr, std::string_view operation) {
  if (FAILED(hr)) {
    throw std::runtime_error(std::string(operation) + " failed: " +
                             HResultText(hr));
  }
}

DeviceBundle CreateDevice(IDXGIAdapter1* adapter, bool requestDebugLayer) {
  constexpr D3D_FEATURE_LEVEL levels[] = {
      D3D_FEATURE_LEVEL_11_1,
      D3D_FEATURE_LEVEL_11_0,
  };

  auto create = [&](UINT flags, DeviceBundle& bundle) {
    return D3D11CreateDevice(adapter,
                             D3D_DRIVER_TYPE_UNKNOWN,
                             nullptr,
                             flags,
                             levels,
                             static_cast<UINT>(std::size(levels)),
                             D3D11_SDK_VERSION,
                             &bundle.device,
                             &bundle.featureLevel,
                             &bundle.context);
  };

  DeviceBundle bundle;
  const UINT baseFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
  HRESULT hr = E_FAIL;
  if (requestDebugLayer) {
    hr = create(baseFlags | D3D11_CREATE_DEVICE_DEBUG, bundle);
    bundle.debugLayerEnabled = SUCCEEDED(hr);
  }
  if (!requestDebugLayer || hr == DXGI_ERROR_SDK_COMPONENT_MISSING) {
    bundle.device.Reset();
    bundle.context.Reset();
    hr = create(baseFlags, bundle);
    bundle.debugLayerEnabled = false;
  }
  ThrowIfFailed(hr, "D3D11CreateDevice");
  ThrowIfFailed(bundle.device.As(&bundle.device1),
                "QueryInterface(ID3D11Device1)");
  return bundle;
}

ComPtr<IDXGIAdapter1> SelectHardwareAdapter() {
  ComPtr<IDXGIFactory1> factory;
  ThrowIfFailed(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),
                "CreateDXGIFactory1");

  for (UINT index = 0;; ++index) {
    ComPtr<IDXGIAdapter1> adapter;
    const HRESULT hr = factory->EnumAdapters1(index, &adapter);
    if (hr == DXGI_ERROR_NOT_FOUND) {
      break;
    }
    ThrowIfFailed(hr, "IDXGIFactory1::EnumAdapters1");

    DXGI_ADAPTER_DESC1 description{};
    ThrowIfFailed(adapter->GetDesc1(&description),
                  "IDXGIAdapter1::GetDesc1");
    if ((description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0) {
      return adapter;
    }
  }
  throw std::runtime_error("no hardware DXGI adapter was found");
}

std::string AdapterName(IDXGIAdapter1* adapter) {
  DXGI_ADAPTER_DESC1 description{};
  ThrowIfFailed(adapter->GetDesc1(&description), "IDXGIAdapter1::GetDesc1");

  const int length = WideCharToMultiByte(CP_UTF8,
                                         0,
                                         description.Description,
                                         -1,
                                         nullptr,
                                         0,
                                         nullptr,
                                         nullptr);
  if (length <= 1) {
    return "<unknown adapter>";
  }
  std::string result(static_cast<std::size_t>(length), '\0');
  WideCharToMultiByte(CP_UTF8,
                      0,
                      description.Description,
                      -1,
                      result.data(),
                      length,
                      nullptr,
                      nullptr);
  result.pop_back();
  return result;
}

SharedTexturePair CreateSharedTexturePair(
    const DeviceBundle& owner,
    const DeviceBundle& opener,
    const D3D11_TEXTURE2D_DESC& description) {
  SharedTexturePair pair;
  ThrowIfFailed(owner.device->CreateTexture2D(
                    &description, nullptr, &pair.ownerTexture),
                "owner ID3D11Device::CreateTexture2D(shared)");

  ComPtr<IDXGIResource1> dxgiResource;
  ThrowIfFailed(pair.ownerTexture.As(&dxgiResource),
                "QueryInterface(IDXGIResource1)");

  HANDLE rawSharedHandle = nullptr;
  ThrowIfFailed(dxgiResource->CreateSharedHandle(
                    nullptr,
                    DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                    nullptr,
                    &rawSharedHandle),
                "IDXGIResource1::CreateSharedHandle");
  UniqueHandle sharedHandle(rawSharedHandle);

  ThrowIfFailed(opener.device1->OpenSharedResource1(
                    sharedHandle.get(), IID_PPV_ARGS(&pair.openedTexture)),
                "opener ID3D11Device1::OpenSharedResource1");
  ThrowIfFailed(pair.ownerTexture.As(&pair.ownerMutex),
                "owner QueryInterface(IDXGIKeyedMutex)");
  ThrowIfFailed(pair.openedTexture.As(&pair.openedMutex),
                "opener QueryInterface(IDXGIKeyedMutex)");
  return pair;
}

}  // namespace keyed_mutex::test
