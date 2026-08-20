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

## Test machine

The results recorded in this README were obtained on 2026-08-21 from the
following machine and toolchain:

| Item | Value |
| --- | --- |
| Operating system | Windows 10 Pro 25H2, build 26200.9168 |
| Processor | 12th Gen Intel(R) Core(TM) i9-12900KS (3.40 GHz) |
| Memory | 32.0 GB RAM (31.7 GB available) |
| System type | 64-bit operating system, x64-based processor |
| GPU | NVIDIA GeForce RTX 4070 Ti |
| NVIDIA driver | 591.86 |
| Dedicated video memory reported by `nvidia-smi` | 12282 MiB |
| Visual Studio / MSBuild | Visual Studio 2022, MSBuild 17.14.8 |
| CMake | 3.30.5 |
| Windows SDK selected by CMake | 10.0.26100.0 |
| CMake target | Windows 10.0.26200, x64, Debug |

These are single-machine observations. They do not establish that a behavior
is guaranteed across other GPU vendors, driver versions, Windows builds,
feature levels, resource descriptions, command workloads, or thread counts.
In particular, a passing unprotected or no-`Flush` observation must not be
read as a portable API guarantee.

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

## Experiment 07: same-interface cross-thread behavior

Target: `exp07_d3d11_same_interface_threads`

This experiment uses one shared texture and one `IDXGIKeyedMutex` interface on
one D3D11 device. It contains three tests:

- a normal `0 -> 1 -> 0` handoff where different threads use the same interface;
- an ownership-migration probe where one thread acquires and another thread
  attempts `ReleaseSync`;
- concurrent same-key waiters using the same interface, with exclusivity and
  wait-time measurements.

The ownership-migration test accepts either a successful or a failing
cross-thread `ReleaseSync`, records the exact `HRESULT`, and verifies that the
original owner can recover when migration is rejected. It therefore probes
runtime behavior without treating an implementation-specific result as a
portable fairness or thread-affinity guarantee.

Observed on 2026-08-21 with an NVIDIA GeForce RTX 4070 Ti, using the Debug
configuration generated by the checked-in `vs2022-x64` preset:

- `SameInterfaceCrossThreadHandoff`: thread A acquired and released the
  mutex; thread B's `AcquireSync(1)` on the same interface was rejected with
  `0x887A0001` (`DXGI_ERROR_INVALID_CALL`), after which thread A successfully
  reacquired and recovered the resource.
- `OwnershipThreadMigration`: thread A acquired the mutex and thread B's
  `ReleaseSync(1)` was accepted; the following acquisition and release also
  completed successfully.
- `SameInterfaceConcurrentWaiters`: concurrent `AcquireSync` calls on the
  same interface were rejected with `0x887A0001`; the probe completed without
  deadlock.

The complete preset test run passed all 9 tests:

```text
ctest --preset vs2022-debug
100% tests passed, 0 tests failed out of 9
```

```powershell
& .\out\build\vs2022-x64\experiments\07_d3d11_same_interface_threads\Debug\exp07_d3d11_same_interface_threads.exe --rounds 1000 --waiters 16
```

## Experiment 08: Flush and shared-texture visibility

Target: `exp08_d3d11_flush_visibility`

This experiment compares two otherwise identical two-device ping-pong paths:

```text
producer: AcquireSync(0) -> UpdateSubresource -> [Flush] -> ReleaseSync(1)
consumer: AcquireSync(1) -> CopyResource/Map/verify -> ReleaseSync(0)
```

The `with Flush` case is the correctness baseline and fails on any frame
mismatch. The `without Flush` case still fails on synchronization or API
errors, but records frame matches and mismatches as observations only; it does
not turn a visibility anomaly into a test failure.

```powershell
& .\out\build\vs2022-x64\experiments\08_d3d11_flush_visibility\Debug\exp08_d3d11_flush_visibility.exe --iterations 1000
```

Observed on 2026-08-21 with an NVIDIA GeForce RTX 4070 Ti, using 300 frames
and the Debug configuration generated by the checked-in `vs2022-x64` preset:

- with `Flush`: 300 matching frames, 0 mismatches;
- without `Flush`: 300 matching frames, 0 mismatches;
- both paths completed without timeout or API error.

This run did not expose a visibility difference. The no-`Flush` result is
observational only and does not establish that omitting `Flush` is a portable
correctness guarantee across drivers, resource types, or command workloads.

The complete preset test run passed all 11 tests:

```text
ctest --preset vs2022-debug
100% tests passed, 0 tests failed out of 11
```

## Experiment 09: shared immediate context and ID3D11Multithread protection

Target: `exp09_d3d11_multithread_context`

This experiment creates one D3D11 device and deliberately shares its single
immediate context between several CPU worker threads. Each worker repeatedly
records `UpdateSubresource` and `Flush` commands for its own texture, after
which the main thread validates the final per-pixel signature.

The experiment compares:

- `ID3D11Multithread::SetMultithreadProtected(TRUE)`: correctness baseline;
- `SetMultithreadProtected(FALSE)`: observation only, recording worker
  failures and final-texture mismatches without treating completion as proof
  that concurrent immediate-context use is supported.

```powershell
& .\out\build\vs2022-x64\experiments\09_d3d11_multithread_context\Debug\exp09_d3d11_multithread_context.exe --workers 4 --iterations 100
```

Observed on 2026-08-21 with an NVIDIA GeForce RTX 4070 Ti, using four worker
threads and 100 iterations per worker with the Debug configuration generated
by the checked-in `vs2022-x64` preset:

- `SetMultithreadProtected(TRUE)`: 400/400 `UpdateSubresource` + `Flush`
  operations completed and all 4 final textures matched;
- `SetMultithreadProtected(FALSE)`: 400/400 operations completed and all 4
  final textures matched;
- no worker failure or validation mismatch was observed in this run.

The unprotected result is an observation for this driver and workload only;
it does not establish that concurrent access to an immediate context is
portable or supported without `ID3D11Multithread` protection.

The complete preset test run passed all 13 tests:

```text
ctest --preset vs2022-debug
100% tests passed, 0 tests failed out of 13
```

## Experiment 10: two-resource lock ordering

Target: `exp10_d3d11_lock_order`

This experiment combines two independent shared textures and their keyed
mutexes:

- same order: both participants acquire A then B, and release B then A;
- reverse order: one participant acquires A then B while the other acquires B
  then A.

The reverse-order case deliberately forms a circular wait after each
participant holds its first mutex. Finite `AcquireSync` timeouts must fire on
both second acquisitions; the already-held first mutexes are then released,
and the main thread verifies recovery by acquiring both resources in canonical
A->B order. The experiment therefore demonstrates deadlock potential and
timeout-based recovery without allowing an actual test deadlock to persist.

```powershell
& .\out\build\vs2022-x64\experiments\10_d3d11_lock_order\Debug\exp10_d3d11_lock_order.exe --rounds 100 --timeout-ms 250
```

Observed on 2026-08-21 with an NVIDIA GeForce RTX 4070 Ti, using the Debug
configuration generated by the checked-in `vs2022-x64` preset:

- same-order A->B locking completed 100 rounds per participant without a
  timeout;
- reverse-order A->B / B->A locking caused both second acquisitions to return
  `WAIT_TIMEOUT` after 250 ms;
- after both participants released their first mutex, canonical A->B
  acquisition succeeded, confirming timeout-based recovery.

The complete preset test run passed all 15 tests:

```text
ctest --preset vs2022-debug
100% tests passed, 0 tests failed out of 15
```

## Experiment 11: multiple interfaces for one shared resource

Target: `exp11_d3d11_multiple_open_interfaces`

One device calls `OpenSharedResource1` twice with the same NT shared handle.
The experiment records whether the returned textures and keyed mutexes have
the same raw interface pointers and `IUnknown` identities, then probes:

- concurrent `AcquireSync(0)` calls through the two returned mutex references;
- `AcquireSync` through the first reference followed by `ReleaseSync` through
  the second reference;
- resource recovery and follow-up acquisition after either accepted or
  rejected cross-interface ownership transfer.

Pointer and COM-identity observations are reported before interpreting the
ownership results, because a runtime may return separate wrappers or reuse the
same underlying COM object for repeated opens on one device.

```powershell
& .\out\build\vs2022-x64\experiments\11_d3d11_multiple_open_interfaces\Debug\exp11_d3d11_multiple_open_interfaces.exe --timeout-ms 250
```

Observed on 2026-08-21 with an NVIDIA GeForce RTX 4070 Ti, using the Debug
configuration generated by the checked-in `vs2022-x64` preset:

- two `OpenSharedResource1` calls returned different texture pointers, mutex
  pointers, and `IUnknown` identities;
- concurrent acquisition through the two mutex interfaces produced one
  successful `AcquireSync(0)` and one `WAIT_TIMEOUT`, with no simultaneous
  owners;
- `ReleaseSync(1)` through the second interface after acquisition through the
  first returned `0x887A0001` (`DXGI_ERROR_INVALID_CALL`);
- releasing through the original interface restored the resource, and the
  second interface then completed `AcquireSync(1) -> ReleaseSync(0)`.

This result indicates that, on this runtime, repeated opens on one device
produce distinct interface identities while ownership remains shared at the
resource level and release is tied to the acquiring interface reference.
That interpretation is runtime-specific and should be rechecked on other
drivers and Windows versions.

The complete preset test run passed all 18 tests:

```text
ctest --preset vs2022-debug
100% tests passed, 0 tests failed out of 18
```

## Experiment 12: phased cross-process failure injection

Target: `exp12_d3d11_failure_injection`

The child process is terminated at five protocol points:

1. before `AcquireSync`;
2. after `AcquireSync(0)`;
3. after `UpdateSubresource` but before `Flush`;
4. after `Flush` but before `ReleaseSync`;
5. after `ReleaseSync(1)`.

The parent records the child exit result, its next `AcquireSync` result, and
whether a child-written signature is safely visible. For the abandoned
after-acquire, after-write, and after-Flush cases, the resource is not read
after `WAIT_ABANDONED`; it is discarded and a fresh shared resource is used to
verify recovery. The after-Release case remains usable and validates the
child's signature through the released key.

```powershell
& .\out\build\vs2022-x64\experiments\12_d3d11_failure_injection\Debug\exp12_d3d11_failure_injection.exe --timeout-ms 1000
```

Observed on 2026-08-21 with an NVIDIA GeForce RTX 4070 Ti, using the Debug
configuration generated by the checked-in `vs2022-x64` preset:

- before `AcquireSync`: parent `AcquireSync(0)` returned `S_OK`; the original
  resource remained usable and a normal handoff succeeded;
- after `AcquireSync`: parent `AcquireSync(0)` returned `WAIT_ABANDONED
  (0x80)`; the resource was discarded and recovery used a newly created
  shared resource;
- after GPU write, before `Flush`: parent returned `WAIT_ABANDONED (0x80)`;
  the abandoned resource was not read, and recovery used resource recreation;
- after `Flush`, before `ReleaseSync`: parent returned `WAIT_ABANDONED
  (0x80)`; the abandoned resource was not read, and recovery used resource
  recreation;
- after `ReleaseSync(1)`: parent acquired key 1 with `S_OK`, observed the
  child signature, and continued using the original resource.

The complete preset test run passed all 23 tests:

```text
ctest --preset vs2022-debug
100% tests passed, 0 tests failed out of 23
```

## Experiment 13: real GPU workload visibility

Target: `exp13_d3d11_gpu_visibility`

This experiment replaces the producer's simple `UpdateSubresource` write with
three GPU command workloads:

- `ClearRenderTargetView` directly clears the shared render target;
- `Draw` renders a full-screen triangle with a pixel shader color;
- `CopyResource` copies a GPU-cleared source texture into the shared texture.

Each workload has a `with Flush` correctness baseline and a `without Flush`
observation case. The consumer validates every frame by mapping a staging copy;
no-`Flush` mismatches are recorded but do not fail the observation test.

```powershell
& .\out\build\vs2022-x64\experiments\13_d3d11_gpu_visibility\Debug\exp13_d3d11_gpu_visibility.exe --iterations 30 --timeout-ms 5000
```

Observed on 2026-08-21 with an NVIDIA GeForce RTX 4070 Ti, using 30 frames
per workload and the Debug configuration generated by the checked-in
`vs2022-x64` preset:

- `ClearRenderTargetView` with and without `Flush`: 30/30 matching frames,
  0 mismatches in both paths;
- `Draw` with and without `Flush`: 30/30 matching frames, 0 mismatches in
  both paths;
- `CopyResource` with and without `Flush`: 30/30 matching frames, 0
  mismatches in both paths.

This run did not expose a visibility difference for these GPU workloads. The
no-`Flush` results remain observations for this driver, resource description,
shader, and command sequence; they do not establish a portable guarantee that
explicit flushing is unnecessary.

The complete preset test run passed all 29 tests:

```text
ctest --preset vs2022-debug
100% tests passed, 0 tests failed out of 29
```

## Experiment 14: shared handle and COM lifetime

Target: `exp14_d3d11_handle_com_lifetime`

This experiment separates NT-handle lifetime from the COM endpoint lifetime:

- close the creator's original NT handle after `OpenSharedResource1` has
  returned an endpoint, then perform a normal keyed-mutex handoff and verify a
  written signature;
- destroy the creator's texture, keyed mutex, shared handle, and device after
  the opener has opened the resource, then use only the opened endpoint for a
  handoff and data readback;
- release the final opened endpoint, attempt to reopen with the already closed
  handle, and create a fresh resource to verify that subsequent resources are
  unaffected.

The reopen check intentionally records the returned HRESULT and only requires
failure: the exact error code after a closed handle is not treated as a
cross-driver contract.

```powershell
& .\out\build\vs2022-x64\experiments\14_d3d11_handle_com_lifetime\Debug\exp14_d3d11_handle_com_lifetime.exe --timeout-ms 1000
```

Observed on 2026-08-21 with an NVIDIA GeForce RTX 4070 Ti, using the Debug
configuration generated by the checked-in `vs2022-x64` preset:

- closing the original NT handle after opening did not affect the opened
  endpoint; the signature remained visible and `AcquireSync`/`ReleaseSync`
  handoff succeeded;
- after the creator texture and device were destroyed, the opened endpoint
  still acquired/released the mutex and read the creator's signature;
- after the last opened COM reference was released, reopening with the closed
  handle failed with `0x80070057` (`E_INVALIDARG`), while a newly created
  shared resource completed a normal handoff.

These observations show that an already opened endpoint keeps the underlying
resource alive independently of the creator's handle and COM references on
this runtime. They do not define a portable lifetime guarantee for arbitrary
drivers, and they do not make a closed `HANDLE` valid for future opens.

The complete preset test run passed all 32 tests:

```text
ctest --preset vs2022-debug
100% tests passed, 0 tests failed out of 32
```
