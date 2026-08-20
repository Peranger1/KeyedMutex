# IDXGIKeyedMutex experiments

This repository contains small, independently buildable D3D11 experiments for
studying `IDXGIKeyedMutex`. Each experiment is one executable target and one
CTest test.

## Prerequisites

- Windows 8 or newer
- Visual Studio 2022 with the Desktop development with C++ workload
- CMake 3.25 or newer
- Git submodules initialized (`third_party/googletest`, pinned to v1.18.0)

The checked-in preset selects the Visual Studio installation at:

```text
D:\CodePrograms\Microsoft Visual Studio\2022\Community
```

## Configure, build, and validate

```powershell
git submodule update --init --recursive
cmake --preset vs2022-x64
cmake --build --preset vs2022-debug
ctest --preset vs2022-debug
```

GoogleTest is built from the checked-in submodule, so configuring and building
the project does not download test dependencies. Each experiment remains a
separate executable target. Its research assertions are GoogleTest assertions,
and CMake's `gtest_discover_tests` exposes the individual cases to CTest.

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

## Experiment 03: three-device key ring

Target: `exp03_d3d11_three_device_ring`

Three D3D11 devices open the same NT-handle shared texture. Each device owns a
separate immediate context and CPU thread. Ownership follows a deterministic
ring:

```text
device A: AcquireSync(0) -> verify C -> write A -> Flush -> ReleaseSync(1)
device B: AcquireSync(1) -> verify A -> write B -> Flush -> ReleaseSync(2)
device C: AcquireSync(2) -> verify B -> write C -> Flush -> ReleaseSync(0)
```

Every handoff verifies a stage-specific, per-pixel frame signature. The
experiment also reports average and maximum `AcquireSync` wait time for each
participant. All three threads use finite timeouts and the executable fails on
any timeout, unexpected HRESULT, or signature mismatch.

```powershell
& .\out\build\vs2022-x64\experiments\03_d3d11_three_device_ring\Debug\exp03_d3d11_three_device_ring.exe --iterations 1000
```

## Experiment 04: same-key contention

Target: `exp04_d3d11_same_key_contention`

One controller device owns key `0`, while three contender devices wait for the
same key `1`. On every round, the controller releases key `1`; whichever
contender wakes first writes its device id and round number into the shared
texture, flushes, and releases key `0` back to the controller.

The test asserts exclusive entry, the exact number of completed handoffs, and
the GPU signature written by every winner. It reports each contender's win
count and the longest same-winner streak, but deliberately makes no fairness
assertion because wake order for equal-key waiters is undefined.

```powershell
& .\out\build\vs2022-x64\experiments\04_d3d11_same_key_contention\Debug\exp04_d3d11_same_key_contention.exe --rounds 1000
```

## Experiment 05: owner abandonment and recovery

Target: `exp05_d3d11_owner_abandonment`

The parent creates an inheritable NT shared handle and starts a child copy of
the same executable. The child opens the D3D11 texture, acquires key `0`, then
terminates itself without releasing the mutex or running COM destructors. The
parent checks that its next `AcquireSync(0)` returns `WAIT_ABANDONED`.

Because an abandoned keyed mutex and its surface are inconsistent, the test
then discards them, creates a fresh shared texture, and verifies a complete
`0 -> 9 -> 0` handoff between two healthy devices.

```powershell
& .\out\build\vs2022-x64\experiments\05_d3d11_owner_abandonment\Debug\exp05_d3d11_owner_abandonment.exe --iterations 10
```

## Experiment 06: cross-process ping-pong

Target: `exp06_d3d11_cross_process_ping_pong`

The parent creates an inheritable NT shared handle and launches a child copy of
the executable on the exact same hardware adapter. Each process owns its own
D3D11 device and immediate context. They coordinate exclusively through the
shared texture's keyed mutex:

```text
parent: AcquireSync(0) -> verify child -> write parent -> Flush -> ReleaseSync(1)
child:  AcquireSync(1) -> verify parent -> write child  -> Flush -> ReleaseSync(0)
```

Both sides copy the shared texture to a local staging texture and verify a
deterministic per-pixel signature. The parent also checks that the child exits
normally, so a timeout, failed handoff, or validation error in either process
fails the GoogleTest case.

```powershell
& .\out\build\vs2022-x64\experiments\06_d3d11_cross_process_ping_pong\Debug\exp06_d3d11_cross_process_ping_pong.exe --iterations 1000
```
