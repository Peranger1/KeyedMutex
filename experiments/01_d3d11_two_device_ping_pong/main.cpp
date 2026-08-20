#include <Windows.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <exception>
#include <iomanip>
#include <iostream>
#include <limits>
#include <mutex>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

constexpr UINT kTextureWidth = 64;
constexpr UINT kTextureHeight = 64;
constexpr UINT kBytesPerPixel = 4;

class UniqueHandle {
public:
  UniqueHandle() = default;
  explicit UniqueHandle(HANDLE handle) noexcept : handle_(handle) {}

  UniqueHandle(const UniqueHandle&) = delete;
  UniqueHandle& operator=(const UniqueHandle&) = delete;

  UniqueHandle(UniqueHandle&& other) noexcept
      : handle_(std::exchange(other.handle_, nullptr)) {}

  UniqueHandle& operator=(UniqueHandle&& other) noexcept {
    if (this != &other) {
      reset();
      handle_ = std::exchange(other.handle_, nullptr);
    }
    return *this;
  }

  ~UniqueHandle() { reset(); }

  [[nodiscard]] HANDLE get() const noexcept { return handle_; }

  void reset(HANDLE replacement = nullptr) noexcept {
    if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
      CloseHandle(handle_);
    }
    handle_ = replacement;
  }

private:
  HANDLE handle_ = nullptr;
};

[[nodiscard]] std::string HResultText(HRESULT hr) {
  std::ostringstream stream;
  stream << "0x" << std::hex << std::uppercase
         << static_cast<std::uint32_t>(hr);

  LPSTR message = nullptr;
  const DWORD length = FormatMessageA(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
          FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr,
      static_cast<DWORD>(hr),
      MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
      reinterpret_cast<LPSTR>(&message),
      0,
      nullptr);
  if (length != 0 && message != nullptr) {
    std::string text(message, length);
    while (!text.empty() && (text.back() == '\r' || text.back() == '\n')) {
      text.pop_back();
    }
    stream << " (" << text << ')';
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

struct Options {
  std::uint32_t iterations = 1'000;
  DWORD timeoutMs = 5'000;
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

struct DeviceBundle {
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11Device1> device1;
  ComPtr<ID3D11DeviceContext> context;
  D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_9_1;
  bool debugLayerEnabled = false;
};

[[nodiscard]] DeviceBundle CreateDevice(IDXGIAdapter1* adapter,
                                        bool requestDebugLayer) {
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

[[nodiscard]] ComPtr<IDXGIAdapter1> SelectHardwareAdapter() {
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

[[nodiscard]] std::string AdapterName(IDXGIAdapter1* adapter) {
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

[[nodiscard]] std::vector<std::uint8_t> MakeFrame(std::uint32_t frameIndex) {
  std::vector<std::uint8_t> pixels(
      static_cast<std::size_t>(kTextureWidth) * kTextureHeight *
      kBytesPerPixel);
  for (UINT y = 0; y < kTextureHeight; ++y) {
    for (UINT x = 0; x < kTextureWidth; ++x) {
      const std::size_t offset =
          (static_cast<std::size_t>(y) * kTextureWidth + x) * kBytesPerPixel;
      pixels[offset + 0] = static_cast<std::uint8_t>(frameIndex & 0xffu);
      pixels[offset + 1] =
          static_cast<std::uint8_t>((frameIndex >> 8u) & 0xffu);
      pixels[offset + 2] = static_cast<std::uint8_t>(
          (frameIndex + x * 31u + y * 17u) & 0xffu);
      pixels[offset + 3] = 0xffu;
    }
  }
  return pixels;
}

[[nodiscard]] std::optional<std::string>
ValidateFrame(const D3D11_MAPPED_SUBRESOURCE& mapped,
              std::uint32_t frameIndex) {
  const auto expected = MakeFrame(frameIndex);
  const auto* actual = static_cast<const std::uint8_t*>(mapped.pData);
  const UINT rowBytes = kTextureWidth * kBytesPerPixel;

  for (UINT y = 0; y < kTextureHeight; ++y) {
    const auto* actualRow = actual + static_cast<std::size_t>(y) * mapped.RowPitch;
    const auto* expectedRow =
        expected.data() + static_cast<std::size_t>(y) * rowBytes;
    for (UINT byte = 0; byte < rowBytes; ++byte) {
      if (actualRow[byte] != expectedRow[byte]) {
        std::ostringstream message;
        message << "frame " << frameIndex << " mismatch at row " << y
                << ", byte " << byte << ": expected "
                << static_cast<unsigned>(expectedRow[byte]) << ", got "
                << static_cast<unsigned>(actualRow[byte]);
        return message.str();
      }
    }
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<std::string>
Acquire(IDXGIKeyedMutex* mutex, UINT64 key, DWORD timeoutMs) {
  const HRESULT hr = mutex->AcquireSync(key, timeoutMs);
  if (hr == S_OK) {
    return std::nullopt;
  }
  if (hr == static_cast<HRESULT>(WAIT_TIMEOUT)) {
    return "AcquireSync(" + std::to_string(key) + ") timed out";
  }
  if (hr == static_cast<HRESULT>(WAIT_ABANDONED)) {
    return "AcquireSync(" + std::to_string(key) +
           ") returned WAIT_ABANDONED; the resource must be recreated";
  }
  return "AcquireSync(" + std::to_string(key) + ") failed: " +
         HResultText(hr);
}

struct SharedExperimentState {
  std::atomic<bool> failed = false;
  std::mutex failureMutex;
  std::string failureMessage;

  void Fail(std::string message) {
    bool expected = false;
    if (failed.compare_exchange_strong(expected, true)) {
      std::scoped_lock lock(failureMutex);
      failureMessage = std::move(message);
    }
  }
};

int Run(const Options& options) {
  const auto adapter = SelectHardwareAdapter();
  auto producer = CreateDevice(adapter.Get(), options.requestDebugLayer);
  auto consumer = CreateDevice(adapter.Get(), options.requestDebugLayer);

  D3D11_TEXTURE2D_DESC sharedDescription{};
  sharedDescription.Width = kTextureWidth;
  sharedDescription.Height = kTextureHeight;
  sharedDescription.MipLevels = 1;
  sharedDescription.ArraySize = 1;
  sharedDescription.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  sharedDescription.SampleDesc.Count = 1;
  sharedDescription.Usage = D3D11_USAGE_DEFAULT;
  sharedDescription.BindFlags =
      D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
  sharedDescription.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE |
                                D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;

  ComPtr<ID3D11Texture2D> producerTexture;
  ThrowIfFailed(producer.device->CreateTexture2D(
                    &sharedDescription, nullptr, &producerTexture),
                "producer ID3D11Device::CreateTexture2D");

  ComPtr<IDXGIResource1> dxgiResource;
  ThrowIfFailed(producerTexture.As(&dxgiResource),
                "QueryInterface(IDXGIResource1)");

  HANDLE rawSharedHandle = nullptr;
  ThrowIfFailed(dxgiResource->CreateSharedHandle(
                    nullptr,
                    DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                    nullptr,
                    &rawSharedHandle),
                "IDXGIResource1::CreateSharedHandle");
  UniqueHandle sharedHandle(rawSharedHandle);

  ComPtr<ID3D11Texture2D> consumerTexture;
  ThrowIfFailed(consumer.device1->OpenSharedResource1(
                    sharedHandle.get(),
                    IID_PPV_ARGS(&consumerTexture)),
                "ID3D11Device1::OpenSharedResource1");

  ComPtr<IDXGIKeyedMutex> producerMutex;
  ComPtr<IDXGIKeyedMutex> consumerMutex;
  ThrowIfFailed(producerTexture.As(&producerMutex),
                "producer QueryInterface(IDXGIKeyedMutex)");
  ThrowIfFailed(consumerTexture.As(&consumerMutex),
                "consumer QueryInterface(IDXGIKeyedMutex)");

  D3D11_TEXTURE2D_DESC stagingDescription = sharedDescription;
  stagingDescription.Usage = D3D11_USAGE_STAGING;
  stagingDescription.BindFlags = 0;
  stagingDescription.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  stagingDescription.MiscFlags = 0;

  ComPtr<ID3D11Texture2D> stagingTexture;
  ThrowIfFailed(consumer.device->CreateTexture2D(
                    &stagingDescription, nullptr, &stagingTexture),
                "consumer ID3D11Device::CreateTexture2D(staging)");

  SharedExperimentState state;
  const auto start = std::chrono::steady_clock::now();

  std::thread producerThread([&] {
    try {
      for (std::uint32_t frame = 0;
           frame < options.iterations && !state.failed.load();
           ++frame) {
        if (auto error = Acquire(producerMutex.Get(), 0, options.timeoutMs)) {
          state.Fail("producer: " + *error);
          break;
        }

        const auto pixels = MakeFrame(frame);
        producer.context->UpdateSubresource(producerTexture.Get(),
                                            0,
                                            nullptr,
                                            pixels.data(),
                                            kTextureWidth * kBytesPerPixel,
                                            0);
        producer.context->Flush();

        const HRESULT release = producerMutex->ReleaseSync(1);
        if (FAILED(release)) {
          state.Fail("producer: ReleaseSync(1) failed: " +
                     HResultText(release));
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
           frame < options.iterations && !state.failed.load();
           ++frame) {
        if (auto error = Acquire(consumerMutex.Get(), 1, options.timeoutMs)) {
          state.Fail("consumer: " + *error);
          break;
        }

        consumer.context->CopyResource(stagingTexture.Get(),
                                       consumerTexture.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        const HRESULT mapResult = consumer.context->Map(
            stagingTexture.Get(), 0, D3D11_MAP_READ, 0, &mapped);
        if (FAILED(mapResult)) {
          state.Fail("consumer: Map(staging) failed: " +
                     HResultText(mapResult));
        } else {
          if (auto mismatch = ValidateFrame(mapped, frame)) {
            state.Fail("consumer: " + *mismatch);
          }
          consumer.context->Unmap(stagingTexture.Get(), 0);
        }

        const HRESULT release = consumerMutex->ReleaseSync(0);
        if (FAILED(release)) {
          state.Fail("consumer: ReleaseSync(0) failed: " +
                     HResultText(release));
          break;
        }
      }
    } catch (const std::exception& exception) {
      state.Fail(std::string("consumer exception: ") + exception.what());
    }
  });

  producerThread.join();
  consumerThread.join();

  const auto elapsed = std::chrono::duration<double, std::milli>(
      std::chrono::steady_clock::now() - start);

  if (state.failed.load()) {
    std::scoped_lock lock(state.failureMutex);
    std::cerr << "FAIL: " << state.failureMessage << '\n';
    return 1;
  }

  std::cout << "PASS: transferred and verified " << options.iterations
            << " frames on " << AdapterName(adapter.Get()) << " in "
            << std::fixed << std::setprecision(2) << elapsed.count() << " ms\n"
            << "      producer debug layer: "
            << (producer.debugLayerEnabled ? "enabled" : "disabled") << '\n'
            << "      consumer debug layer: "
            << (consumer.debugLayerEnabled ? "enabled" : "disabled") << '\n';
  return 0;
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
