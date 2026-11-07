using System;
using System.IO;
using System.Security.Cryptography;
using System.Text;
using System.Threading;
using Avalonia.Threading;

namespace PartyUp.Client.Services;

/// <summary>
/// 单实例守卫（命名 Mutex + 命名事件）。
///
/// 为什么需要：服务端是单端登录（同一账号新登录会把旧会话踢下线），
/// 同时开两个客户端会互相踢来踢去；桌面端只允许一个实例。
///
/// 通信方式：
///   * <c>Local\PartyUp.Client</c> 命名 Mutex——持有者 = 正在运行的客户端；
///   * <c>Local\PartyUp.Client.Activate</c> 命名 AutoReset 事件——第二个进程启动时 Set 一下，
///     第一个进程的后台线程收到后把主窗口 Show/Activate（真正的窗口操作经
///     <see cref="Dispatcher.UIThread"/> 调度）。
/// 用 <c>Local\</c> 前缀而不是 <c>Global\</c>：只要求同一登录会话内互斥，
/// 也避免跨会话（如另一用户/服务会话）打开全局对象时的权限问题。
/// </summary>
public static class SingleInstance
{
    /// <summary>
    /// 单实例作用域。默认空后缀＝整个用户会话只能有一个客户端（普通用户的正确语义）。
    /// 指定了独立配置根（<c>--appdata</c>）时追加一个由该路径派生的短哈希后缀，
    /// 让同一台机器上可以同时跑多个"不同身份"的客户端（手动双账号测试、自动化验收）。
    /// 这不是给普通用户开多开——不传 <c>--appdata</c> 时行为与以前完全一致。
    /// </summary>
    private static readonly string Scope = ComputeScope();
    private static readonly string MutexName = @"Local\PartyUp.Client" + Scope;
    private static readonly string ActivationEventName = @"Local\PartyUp.Client.Activate" + Scope;

    private static string ComputeScope()
    {
        var root = AppPaths.OverrideRoot;
        if (string.IsNullOrWhiteSpace(root))
        {
            return string.Empty;
        }
        var normalized = Path.GetFullPath(root).TrimEnd(Path.DirectorySeparatorChar).ToLowerInvariant();
        var hash = SHA256.HashData(Encoding.UTF8.GetBytes(normalized));
        return "." + Convert.ToHexString(hash, 0, 6);
    }

    private static Mutex? _mutex;
    private static EventWaitHandle? _activationEvent;
    private static Action? _activationHandler;
    private static volatile bool _pendingActivation;
    private static volatile bool _stopped;
    private static int _released;

    /// <summary>
    /// 尝试成为唯一实例。返回 false 表示已有实例在运行，调用方应
    /// <see cref="SignalExisting"/> 后以退出码 0 结束。
    /// 守卫本身不可用（异常）时返回 true 并记 FAIL：宁可失去互斥，也不能把用户锁在门外。
    /// </summary>
    public static bool TryAcquire(out Mutex? mutex)
    {
        mutex = null;
        try
        {
            var candidate = new Mutex(initiallyOwned: true, MutexName, out var createdNew);
            if (!createdNew)
            {
                candidate.Dispose();
                AppLog.Info("single-instance: another client already owns " + MutexName);
                return false;
            }

            _mutex = candidate;
            mutex = candidate;

            if (OperatingSystem.IsWindows())
            {
                _activationEvent = new EventWaitHandle(
                    initialState: false, EventResetMode.AutoReset, ActivationEventName);
                StartActivationListener();
            }
            else
            {
                // 命名 EventWaitHandle 在 .NET 只有 Windows 实现；非 Windows 上仍保留 Mutex 互斥，
                // 但没有「唤醒已有实例」通道（Windows 首发，macOS/Linux 的激活机制后续再做）。
                AppLog.Warn("single-instance: activation event is Windows-only; " +
                            "a second launch will exit without raising the main window");
            }
            AppLog.Info("single-instance: acquired " + MutexName);
            return true;
        }
        catch (Exception ex)
        {
            AppLog.Fail("single-instance: guard unavailable, continuing without it: " + ex.Message);
            mutex = null;
            return true;
        }
    }

    /// <summary>
    /// 通知已在运行的实例把主窗口提到前台。找不到事件（第一个实例还没建好/已退出）时只记日志。
    /// </summary>
    public static void SignalExisting()
    {
        if (!OperatingSystem.IsWindows())
        {
            AppLog.Info("single-instance: activation signal is Windows-only; nothing to signal");
            return;
        }
        try
        {
            using var activation = EventWaitHandle.OpenExisting(ActivationEventName);
            activation.Set();
            AppLog.Info("single-instance: activation signal sent to the running client");
        }
        catch (WaitHandleCannotBeOpenedException)
        {
            AppLog.Warn("single-instance: no activation event (existing client still starting?)");
        }
        catch (Exception ex)
        {
            AppLog.Warn("single-instance: failed to signal the running client: " + ex.Message);
        }
    }

    /// <summary>
    /// 由托盘宿主注册「激活=显示主窗口」回调。窗口建好之前收到的信号会先挂起，
    /// 注册时立刻补发。
    /// </summary>
    public static void RegisterActivationHandler(Action handler)
    {
        Volatile.Write(ref _activationHandler, handler);
        if (_pendingActivation)
        {
            _pendingActivation = false;
            Dispatcher.UIThread.Post(handler);
        }
    }

    /// <summary>窗口销毁时解除回调，避免退出阶段事件再触发到已关闭的窗口。</summary>
    public static void ClearActivationHandler() => Volatile.Write(ref _activationHandler, null);

    /// <summary>进程退出前释放 Mutex/事件（只对成功 <see cref="TryAcquire"/> 的实例有意义）。</summary>
    public static void Release()
    {
        if (Interlocked.Exchange(ref _released, 1) != 0)
        {
            return;
        }

        // 先标记停止：下面 Set() 会唤醒监听线程，它在退出阶段不应再上报「激活」。
        _stopped = true;
        Volatile.Write(ref _activationHandler, null);

        var activation = Interlocked.Exchange(ref _activationEvent, null);
        if (activation is not null)
        {
            try
            {
                activation.Set(); // 唤醒监听线程，让它从 WaitOne 返回后自行退出。
            }
            catch (ObjectDisposedException)
            {
                // 已释放，忽略。
            }
            activation.Dispose();
        }

        var mutex = Interlocked.Exchange(ref _mutex, null);
        if (mutex is not null)
        {
            try
            {
                mutex.ReleaseMutex();
            }
            catch (ApplicationException)
            {
                // 当前线程不是持有者（理论上不会发生）；忽略即可。
            }
            mutex.Dispose();
        }

        AppLog.Info("single-instance: released");
    }

    private static void StartActivationListener()
    {
        var handle = _activationEvent;
        if (handle is null)
        {
            return;
        }

        var thread = new Thread(() => ListenLoop(handle))
        {
            IsBackground = true,
            Name = "PartyUp.SingleInstance.Listener",
        };
        thread.Start();
    }

    private static void ListenLoop(EventWaitHandle handle)
    {
        while (true)
        {
            try
            {
                handle.WaitOne();
            }
            catch (ObjectDisposedException)
            {
                return;
            }
            catch (Exception ex)
            {
                AppLog.Warn("single-instance: listener stopped: " + ex.Message);
                return;
            }

            if (_stopped)
            {
                return;
            }
            DispatchActivation();
        }
    }

    private static void DispatchActivation()
    {
        if (_stopped)
        {
            return;
        }
        var handler = Volatile.Read(ref _activationHandler);
        if (handler is null)
        {
            // 窗口还没建好（第二个实例来得比 Avalonia 初始化早）：挂起，等注册回调时补发。
            _pendingActivation = true;
            AppLog.Info("single-instance: activation requested before the main window was ready");
            return;
        }

        AppLog.Info("single-instance: activation requested; bringing the main window to front");
        Dispatcher.UIThread.Post(handler);
    }
}
