#pragma once

#include <Windows.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <string>
#include <string_view>

namespace keyed_mutex::test {

using Microsoft::WRL::ComPtr;

class UniqueHandle {
public:
  UniqueHandle() = default;
  explicit UniqueHandle(HANDLE handle) noexcept;

  UniqueHandle(const UniqueHandle&) = delete;
  UniqueHandle& operator=(const UniqueHandle&) = delete;

  UniqueHandle(UniqueHandle&& other) noexcept;
  UniqueHandle& operator=(UniqueHandle&& other) noexcept;

  ~UniqueHandle();

  [[nodiscard]] HANDLE get() const noexcept;
  void reset(HANDLE replacement = nullptr) noexcept;

private:
  HANDLE handle_ = nullptr;
};

struct DeviceBundle {
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11Device1> device1;
  ComPtr<ID3D11DeviceContext> context;
  D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_9_1;
  bool debugLayerEnabled = false;
};

struct SharedTexturePair {
  ComPtr<ID3D11Texture2D> ownerTexture;
  ComPtr<ID3D11Texture2D> openedTexture;
  ComPtr<IDXGIKeyedMutex> ownerMutex;
  ComPtr<IDXGIKeyedMutex> openedMutex;
};

[[nodiscard]] std::string HResultText(HRESULT hr);
void ThrowIfFailed(HRESULT hr, std::string_view operation);

[[nodiscard]] DeviceBundle CreateDevice(IDXGIAdapter1* adapter,
                                        bool requestDebugLayer);
[[nodiscard]] ComPtr<IDXGIAdapter1> SelectHardwareAdapter();
[[nodiscard]] std::string AdapterName(IDXGIAdapter1* adapter);
[[nodiscard]] SharedTexturePair CreateSharedTexturePair(
    const DeviceBundle& owner,
    const DeviceBundle& opener,
    const D3D11_TEXTURE2D_DESC& description);

}  // namespace keyed_mutex::test
