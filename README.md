# IDXGIKeyedMutex experiments

This repository contains small, independently buildable D3D11 experiments for
studying `IDXGIKeyedMutex`. Each experiment is one executable target and one
CTest test.

## Prerequisites

- Windows 8 or newer
- Visual Studio 2022 with the Desktop development with C++ workload
- CMake 3.25 or newer

The checked-in preset selects the Visual Studio installation at:

```text
D:\CodePrograms\Microsoft Visual Studio\2022\Community
```

## Configure, build, and validate

```powershell
cmake --preset vs2022-x64
cmake --build --preset vs2022-debug
ctest --preset vs2022-debug
```

To run the first experiment directly with a longer stress pass:

```powershell
& .\out\build\vs2022-x64\experiments\01_d3d11_two_device_ping_pong\Debug\exp01_d3d11_two_device_ping_pong.exe --iterations 10000
```

## Experiment 01: two-device ping-pong

The experiment creates two D3D11 devices on the same hardware adapter. A
shared 2D texture is opened by both devices using an NT shared handle. The
producer and consumer each own a separate immediate context and run on
separate CPU threads.

The keyed-mutex protocol is:

```text
producer: AcquireSync(0) -> UpdateSubresource -> Flush -> ReleaseSync(1)
consumer: AcquireSync(1) -> CopyResource/Map/verify -> ReleaseSync(0)
```

Every frame contains a deterministic per-pixel signature. The executable
returns success only when every transferred frame matches the expected data.

## Experiment 02: key state machine

Target: `exp02_d3d11_key_state_machine`

This experiment validates the documented key protocol without issuing render
commands. It checks that:

- the only valid initial key is `0`;
- acquiring a key while another device owns the resource times out;
- `ReleaseSync(K)` selects the key required by the next acquisition;
- a device that does not own the resource cannot release it;
- `WAIT_TIMEOUT` is non-negative, so `SUCCEEDED(result)` is not a sufficient
  success check for `AcquireSync`.

The non-owner release check requires a failing `HRESULT` but records the exact
value instead of fixing it to `E_FAIL`; the tested NVIDIA/DXGI runtime returned
`DXGI_ERROR_INVALID_CALL`, despite the reference documentation naming
`E_FAIL`.

All potentially blocking calls use a finite timeout. Run it directly with:

```powershell
& .\out\build\vs2022-x64\experiments\02_d3d11_key_state_machine\Debug\exp02_d3d11_key_state_machine.exe --timeout-ms 25
```
