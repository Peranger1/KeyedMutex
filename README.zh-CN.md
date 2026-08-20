# IDXGIKeyedMutex 实验

[English README](README.md)

本仓库包含一组围绕 `IDXGIKeyedMutex` 的小型 D3D11 实验。每个实验都对应
一个独立的可执行目标和一个 CTest 测试，用于观察共享纹理、KeyedMutex、NT
共享句柄以及 D3D11/COM 生命周期之间的行为关系。

## 环境要求

- Windows 8 或更高版本
- 安装“使用 C++ 的桌面开发”工作负载的 Visual Studio 2022
- CMake 3.25 或更高版本
- 初始化 Git 子模块（`third_party/googletest`，固定为 v1.18.0）

项目内置 preset 使用以下 Visual Studio 安装路径：

```text
D:\CodePrograms\Microsoft Visual Studio\2022\Community
```

## 测试机信息

本 README 中记录的结果于 2026-08-21 在以下测试机和工具链上取得：

| 项目 | 信息 |
| --- | --- |
| 操作系统 | Windows 10 Pro 25H2，Build 26200.9168 |
| 处理器 | 12th Gen Intel(R) Core(TM) i9-12900KS (3.40 GHz) |
| 内存 | 32.0 GB RAM（31.7 GB 可用） |
| 系统类型 | 64 位操作系统，基于 x64 的处理器 |
| GPU | NVIDIA GeForce RTX 4070 Ti |
| NVIDIA 驱动 | 591.86 |
| 独立显存 | `nvidia-smi` 报告 12282 MiB |
| Visual Studio / MSBuild | Visual Studio 2022，MSBuild 17.14.8 |
| CMake | 3.30.5 |
| Windows SDK | 10.0.26100.0 |
| CMake 目标 | Windows 10.0.26200，x64，Debug |

这些结果是单机观察结果，不能证明行为在其他 GPU 厂商、驱动版本、Windows
版本、功能级别、资源描述、命令负载或线程数量下仍然成立。尤其是，未开启
保护或未调用 `Flush` 时测试通过，只能说明当前运行环境下观察到了该现象，
不能视为可移植的 API 保证。

## 配置、编译和验证

```powershell
git submodule update --init --recursive
cmake --preset vs2022-x64
cmake --build --preset vs2022-debug
ctest --preset vs2022-debug
```

GoogleTest 使用仓库中固定版本的子模块，因此配置和编译项目不会下载测试
依赖。每个实验都是独立的可执行目标，研究性断言由 GoogleTest 提供，
CMake 的 `gtest_discover_tests` 会将各个测试用例暴露给 CTest。

如需直接运行第一个实验并进行更长时间的压力测试：

```powershell
& .\out\build\vs2022-x64\experiments\01_d3d11_two_device_ping_pong\Debug\exp01_d3d11_two_device_ping_pong.exe --iterations 10000
```

## 实验 01：双设备 Ping-Pong

实验在同一个硬件适配器上创建两个 D3D11 device。两个 device 通过 NT 共享
句柄打开同一个共享二维纹理，生产者和消费者分别使用自己的 immediate
context，并运行在不同 CPU 线程上。

KeyedMutex 协议如下：

```text
生产者：AcquireSync(0) -> UpdateSubresource -> Flush -> ReleaseSync(1)
消费者：AcquireSync(1) -> CopyResource/Map/校验 -> ReleaseSync(0)
```

每一帧都包含确定性的逐像素签名。只有所有传输帧都与预期数据一致时，程序
才返回成功。

## 实验 02：Key 状态机

目标：`exp02_d3d11_key_state_machine`

该实验不发出渲染命令，只验证文档定义的 key 协议：

- 初始状态唯一有效的 key 是 `0`；
- 一个 device 持有资源时，其他 device 获取相同 key 会超时；
- `ReleaseSync(K)` 会选择下一次获取所需的 key；
- 不拥有资源的 device 不能释放资源；
- `WAIT_TIMEOUT` 是非负值，因此不能只用 `SUCCEEDED(result)` 判断
  `AcquireSync` 是否成功。

非所有者释放测试只要求返回失败的 `HRESULT`，并记录实际值，而不固定为
`E_FAIL`。当前 NVIDIA/DXGI 运行时返回了 `DXGI_ERROR_INVALID_CALL`，虽然
参考文档使用了 `E_FAIL`。

所有可能阻塞的调用都使用有限超时。直接运行：

```powershell
& .\out\build\vs2022-x64\experiments\02_d3d11_key_state_machine\Debug\exp02_d3d11_key_state_machine.exe --timeout-ms 25
```

## 实验 03：三设备 Key 环

目标：`exp03_d3d11_three_device_ring`

三个 D3D11 device 打开同一个 NT 句柄共享纹理，每个 device 使用独立的
immediate context 和 CPU 线程。所有权按确定性环路传递：

```text
设备 A：AcquireSync(0) -> 校验 C -> 写入 A -> Flush -> ReleaseSync(1)
设备 B：AcquireSync(1) -> 校验 A -> 写入 B -> Flush -> ReleaseSync(2)
设备 C：AcquireSync(2) -> 校验 B -> 写入 C -> Flush -> ReleaseSync(0)
```

每次交接都会校验阶段对应的逐像素帧签名，并报告每个参与者的平均和最大
`AcquireSync` 等待时间。所有线程都使用有限超时；任何超时、非预期
`HRESULT` 或签名不匹配都会使测试失败。

```powershell
& .\out\build\vs2022-x64\experiments\03_d3d11_three_device_ring\Debug\exp03_d3d11_three_device_ring.exe --iterations 1000
```

## 实验 04：同 Key 竞争

目标：`exp04_d3d11_same_key_contention`

一个控制器 device 持有 key `0`，三个竞争者 device 等待同一个 key `1`。
每轮由控制器释放 key `1`；最先唤醒的竞争者写入自己的 device ID 和轮次，
执行 Flush，然后释放 key `0` 给控制器。

测试验证互斥进入、完成交接总数和每个获胜者写入的 GPU 签名，并报告每个
竞争者的获胜次数及最长连续获胜次数。由于相同 key 的等待者唤醒顺序未定义，
实验不对公平性作断言。

```powershell
& .\out\build\vs2022-x64\experiments\04_d3d11_same_key_contention\Debug\exp04_d3d11_same_key_contention.exe --rounds 1000
```

## 实验 05：所有者放弃与恢复

目标：`exp05_d3d11_owner_abandonment`

父进程创建可继承的 NT 共享句柄，并启动同一可执行文件的子进程。子进程
打开 D3D11 纹理、获取 key `0`，随后不释放 mutex、也不运行 COM 析构逻辑
就终止。父进程验证下一次 `AcquireSync(0)` 返回 `WAIT_ABANDONED`。

由于 abandoned 的 KeyedMutex 及其表面内容处于不一致状态，测试会丢弃旧
资源，创建新的共享纹理，并验证两个健康 device 可以完成 `0 -> 9 -> 0`
交接。

```powershell
& .\out\build\vs2022-x64\experiments\05_d3d11_owner_abandonment\Debug\exp05_d3d11_owner_abandonment.exe --iterations 10
```

## 实验 06：跨进程 Ping-Pong

目标：`exp06_d3d11_cross_process_ping_pong`

父进程创建可继承的 NT 共享句柄，并在完全相同的硬件适配器上启动子进程。
两个进程各自拥有 D3D11 device 和 immediate context，只通过共享纹理的
KeyedMutex 进行协调：

```text
父进程：AcquireSync(0) -> 校验子进程 -> 写入父进程 -> Flush -> ReleaseSync(1)
子进程：AcquireSync(1) -> 校验父进程 -> 写入子进程 -> Flush -> ReleaseSync(0)
```

双方都将共享纹理复制到本地 staging 纹理，并校验确定性的逐像素签名。父
进程还会检查子进程是否正常退出，因此任一进程的超时、交接失败或数据校验
失败都会使 GoogleTest 用例失败。

```powershell
& .\out\build\vs2022-x64\experiments\06_d3d11_cross_process_ping_pong\Debug\exp06_d3d11_cross_process_ping_pong.exe --iterations 1000
```

## 实验 07：同一接口跨线程行为

目标：`exp07_d3d11_same_interface_threads`

实验在一个 D3D11 device 上使用一个共享纹理和一个
`IDXGIKeyedMutex` 接口，包含三个测试：

- 不同线程使用同一接口完成正常的 `0 -> 1 -> 0` 交接；
- 一个线程获取后，由另一个线程尝试执行 `ReleaseSync` 的所有权迁移；
- 多个线程通过同一接口并发等待同一个 key，并测量互斥和等待时间。

所有权迁移测试接受成功或失败的跨线程 `ReleaseSync`，记录实际 `HRESULT`，
并在迁移被拒绝时验证原所有者可以恢复。因此该实验用于探测运行时行为，
不将实现相关结果解释为可移植的线程亲和性或公平性保证。

2026-08-21 在 NVIDIA GeForce RTX 4070 Ti、preset 生成的 Debug 配置上观察到：

- `SameInterfaceCrossThreadHandoff`：线程 A 获取并释放 mutex；线程 B 在同一
  接口上的 `AcquireSync(1)` 被 `0x887A0001`（`DXGI_ERROR_INVALID_CALL`）拒绝，
  随后线程 A 成功重新获取并恢复资源；
- `OwnershipThreadMigration`：线程 A 获取 mutex，线程 B 的 `ReleaseSync(1)`
  被接受，后续获取和释放也成功；
- `SameInterfaceConcurrentWaiters`：同一接口上的并发 `AcquireSync` 被
  `0x887A0001` 拒绝，测试未发生死锁。

完整 preset 测试当时通过了 9 个测试：

```text
ctest --preset vs2022-debug
100% tests passed, 0 tests failed out of 9
```

```powershell
& .\out\build\vs2022-x64\experiments\07_d3d11_same_interface_threads\Debug\exp07_d3d11_same_interface_threads.exe --rounds 1000 --waiters 16
```

## 实验 08：Flush 与共享纹理可见性

目标：`exp08_d3d11_flush_visibility`

实验比较两条除 Flush 外完全相同的双设备 Ping-Pong 路径：

```text
生产者：AcquireSync(0) -> UpdateSubresource -> [Flush] -> ReleaseSync(1)
消费者：AcquireSync(1) -> CopyResource/Map/校验 -> ReleaseSync(0)
```

带 `Flush` 路径作为正确性基准，任何帧不匹配都会失败。不带 `Flush` 路径
仍会对同步或 API 错误失败，但帧匹配和不匹配只作为观察结果，不会把可见性
异常转化为测试失败。

```powershell
& .\out\build\vs2022-x64\experiments\08_d3d11_flush_visibility\Debug\exp08_d3d11_flush_visibility.exe --iterations 1000
```

2026-08-21 在 NVIDIA GeForce RTX 4070 Ti、300 帧和 preset 生成的 Debug
配置上观察到：

- 带 `Flush`：300 帧匹配，0 帧不匹配；
- 不带 `Flush`：300 帧匹配，0 帧不匹配；
- 两条路径都没有超时或 API 错误。

该次运行没有暴露可见性差异。不带 `Flush` 的结果仅是观察结果，不能证明
省略 `Flush` 在其他驱动、资源类型或命令负载下仍然正确。

完整 preset 测试通过了 11 个测试：

```text
ctest --preset vs2022-debug
100% tests passed, 0 tests failed out of 11
```

## 实验 09：共享 immediate context 与 ID3D11Multithread 保护

目标：`exp09_d3d11_multithread_context`

实验创建一个 D3D11 device，并故意让多个 CPU worker 线程共享同一个
immediate context。每个 worker 反复为自己的纹理记录 `UpdateSubresource`
和 `Flush` 命令，主线程随后验证最终逐像素签名。

实验比较：

- `ID3D11Multithread::SetMultithreadProtected(TRUE)`：正确性基准；
- `SetMultithreadProtected(FALSE)`：仅作观察，记录 worker 失败和最终纹理
  不匹配，不能把操作完成视为并发使用受到支持的证明。

```powershell
& .\out\build\vs2022-x64\experiments\09_d3d11_multithread_context\Debug\exp09_d3d11_multithread_context.exe --workers 4 --iterations 100
```

2026-08-21 在 NVIDIA GeForce RTX 4070 Ti、4 个 worker、每个 worker 100 轮的
Debug 配置上观察到：

- `SetMultithreadProtected(TRUE)`：400/400 次 `UpdateSubresource` + `Flush`
  完成，4 个最终纹理全部匹配；
- `SetMultithreadProtected(FALSE)`：400/400 次操作完成，4 个最终纹理全部匹配；
- 没有观察到 worker 失败或校验不匹配。

未保护路径的结果只适用于该驱动和工作负载，不能证明关闭
`ID3D11Multithread` 保护后并发访问 immediate context 是可移植或受支持的。

完整 preset 测试通过了 13 个测试：

```text
ctest --preset vs2022-debug
100% tests passed, 0 tests failed out of 13
```

## 实验 10：双资源锁顺序

目标：`exp10_d3d11_lock_order`

实验组合两个相互独立的共享纹理及其 KeyedMutex：

- 同序：两个参与者都按 A 后 B 获取，并按 B 后 A 释放；
- 反序：一个参与者按 A 后 B，另一个按 B 后 A。

反序场景在双方持有第一把锁后构造循环等待。有限的 `AcquireSync` 超时必须
使双方第二次获取返回超时；随后释放已经持有的第一把锁，主线程再按统一的
A->B 顺序获取两项资源验证恢复。实验展示了组合资源的死锁风险和超时恢复，
不会让实际死锁无限期存在。

```powershell
& .\out\build\vs2022-x64\experiments\10_d3d11_lock_order\Debug\exp10_d3d11_lock_order.exe --rounds 100 --timeout-ms 250
```

2026-08-21 在 NVIDIA GeForce RTX 4070 Ti、preset 生成的 Debug 配置上观察到：

- 同序 A->B 锁定完成了双方各 100 轮，没有超时；
- 反序 A->B / B->A 锁定使双方第二次获取在 250 ms 后返回 `WAIT_TIMEOUT`；
- 双方释放第一把锁后，按统一 A->B 顺序获取成功，确认可以恢复。

完整 preset 测试通过了 15 个测试：

```text
ctest --preset vs2022-debug
100% tests passed, 0 tests failed out of 15
```

## 实验 11：同一共享资源的多接口

目标：`exp11_d3d11_multiple_open_interfaces`

同一个 device 使用相同 NT 共享句柄两次调用 `OpenSharedResource1`。实验记录
返回的纹理和 KeyedMutex 是否具有相同的原始接口指针和 `IUnknown` identity，
并进一步验证：

- 两个 mutex 引用并发执行 `AcquireSync(0)`；
- 第一个引用获取后，由第二个引用执行 `ReleaseSync`；
- 无论跨接口所有权转移被接受还是拒绝，资源都能恢复并继续获取。

实验先记录指针和 COM identity，再解释所有权结果，因为运行时可能返回独立
包装对象，也可能复用同一个底层 COM 对象。

```powershell
& .\out\build\vs2022-x64\experiments\11_d3d11_multiple_open_interfaces\Debug\exp11_d3d11_multiple_open_interfaces.exe --timeout-ms 250
```

2026-08-21 在 NVIDIA GeForce RTX 4070 Ti、preset 生成的 Debug 配置上观察到：

- 两次 `OpenSharedResource1` 返回了不同的纹理指针、mutex 指针和
  `IUnknown` identity；
- 两个接口并发获取时，一个 `AcquireSync(0)` 成功，另一个返回
  `WAIT_TIMEOUT`，没有同时持有者；
- 第一个接口获取后由第二个接口执行 `ReleaseSync(1)`，返回
  `0x887A0001`（`DXGI_ERROR_INVALID_CALL`）；
- 原接口释放后，第二个接口可以完成 `AcquireSync(1) -> ReleaseSync(0)`。

这说明在当前运行时，同一 device 的重复打开会产生不同的接口 identity，
但所有权仍在资源层面共享，释放操作还与获取它的接口引用有关。该解释
依赖具体运行时，应在其他驱动和 Windows 版本上重新验证。

完整 preset 测试通过了 18 个测试：

```text
ctest --preset vs2022-debug
100% tests passed, 0 tests failed out of 18
```

## 实验 12：跨进程分阶段故障注入

目标：`exp12_d3d11_failure_injection`

子进程分别在以下五个协议阶段退出：

1. `AcquireSync` 之前；
2. `AcquireSync(0)` 之后；
3. `UpdateSubresource` 之后、`Flush` 之前；
4. `Flush` 之后、`ReleaseSync(1)` 之前；
5. `ReleaseSync(1)` 之后。

父进程记录子进程退出结果、下一次 `AcquireSync` 结果，以及子进程签名是否
安全可见。对于获取后、GPU 写入后和 Flush 后异常退出的情况，父进程在收到
`WAIT_ABANDONED` 后不读取旧资源，而是丢弃并重建共享资源。Release 完成后
退出的情况则继续使用原资源并校验子进程签名。

```powershell
& .\out\build\vs2022-x64\experiments\12_d3d11_failure_injection\Debug\exp12_d3d11_failure_injection.exe --timeout-ms 1000
```

2026-08-21 在 NVIDIA GeForce RTX 4070 Ti、preset 生成的 Debug 配置上观察到：

- `AcquireSync` 之前退出：父进程 `AcquireSync(0)` 返回 `S_OK`，原资源仍可
  使用并完成正常交接；
- `AcquireSync` 之后退出：父进程返回 `WAIT_ABANDONED (0x80)`，丢弃旧资源
  并通过新资源恢复；
- GPU 写入后、Flush 前退出：父进程返回 `WAIT_ABANDONED (0x80)`，不读取
  abandoned 资源，改为重建；
- Flush 后、Release 前退出：父进程返回 `WAIT_ABANDONED (0x80)`，不读取
  abandoned 资源，改为重建；
- Release 后退出：父进程以 `S_OK` 获取 key 1，观察到子进程签名，原资源
  继续可用。

完整 preset 测试通过了 23 个测试：

```text
ctest --preset vs2022-debug
100% tests passed, 0 tests failed out of 23
```

## 实验 13：真实 GPU 工作负载可见性

目标：`exp13_d3d11_gpu_visibility`

实验将生产者原本简单的 `UpdateSubresource` 写入替换为三类 GPU 命令负载：

- `ClearRenderTargetView` 直接清除共享 render target；
- `Draw` 使用像素着色器绘制覆盖整个纹理的全屏三角形；
- `CopyResource` 将 GPU 清除的源纹理复制到共享纹理。

每种负载都有带 `Flush` 的正确性基准和不带 `Flush` 的现象观察用例。消费者
通过 staging 纹理映射逐帧校验；不带 `Flush` 的不匹配只记录，不导致观察测试
失败。

```powershell
& .\out\build\vs2022-x64\experiments\13_d3d11_gpu_visibility\Debug\exp13_d3d11_gpu_visibility.exe --iterations 30 --timeout-ms 5000
```

2026-08-21 在 NVIDIA GeForce RTX 4070 Ti、每种负载 30 帧、preset 生成的
Debug 配置上观察到：

- `ClearRenderTargetView` 带/不带 `Flush`：均为 30/30 帧匹配，0 不匹配；
- `Draw` 带/不带 `Flush`：均为 30/30 帧匹配，0 不匹配；
- `CopyResource` 带/不带 `Flush`：均为 30/30 帧匹配，0 不匹配。

该次运行没有暴露 GPU 工作负载的可见性差异。不带 `Flush` 的结果仍然只适用
于当前驱动、资源描述、着色器和命令序列，不能证明显式 Flush 在所有情况下
都可以省略。

完整 preset 测试通过了 29 个测试：

```text
ctest --preset vs2022-debug
100% tests passed, 0 tests failed out of 29
```

## 实验 14：Shared Handle 与 COM 生命周期

目标：`exp14_d3d11_handle_com_lifetime`

该实验将 NT handle 生命周期与 COM endpoint 生命周期分开验证：

- `OpenSharedResource1` 返回 endpoint 后关闭创建者手中的原始 NT handle，
  再执行正常 KeyedMutex 交接并验证写入签名；
- opener 打开资源后销毁创建者 texture、KeyedMutex、共享 handle 和 device，
  只使用已打开 endpoint 完成交接和数据读取；
- 释放最后一个已打开 endpoint 后，使用已经关闭的 handle 尝试重新打开，
  并创建新资源验证后续资源不受旧资源释放影响。

重新打开测试只要求调用失败并记录 HRESULT，不将关闭 handle 后的具体错误码
视为跨驱动契约。

```powershell
& .\out\build\vs2022-x64\experiments\14_d3d11_handle_com_lifetime\Debug\exp14_d3d11_handle_com_lifetime.exe --timeout-ms 1000
```

2026-08-21 在 NVIDIA GeForce RTX 4070 Ti、preset 生成的 Debug 配置上观察到：

- 打开资源后关闭原始 NT handle 不影响已打开 endpoint，签名仍可见，
  `AcquireSync`/`ReleaseSync` 交接成功；
- 创建者 texture 和 device 销毁后，已打开 endpoint 仍能获取/释放 mutex，
  并读取创建者写入的签名；
- 最后一个已打开 COM 引用释放后，使用已关闭 handle 重开失败，返回
  `0x80070057`（`E_INVALIDARG`）；新建共享资源仍能正常交接。

这些结果表明，在当前运行时，已经打开的 endpoint 可以独立于创建者的 handle
和 COM 引用保持底层资源可用。但这不是任意驱动上的可移植生命周期保证，
也不意味着关闭后的 `HANDLE` 可以继续用于打开资源。

完整 preset 测试通过了 32 个测试：

```text
ctest --preset vs2022-debug
100% tests passed, 0 tests failed out of 32
```
