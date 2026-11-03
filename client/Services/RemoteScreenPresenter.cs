using Avalonia;
using Avalonia.Controls;
using Avalonia.Media.Imaging;
using Avalonia.Platform;

namespace PartyUp.Client.Services;

/// <summary>
/// 把 <c>pu_frame</c>（BGRA8，引擎持有）拷进**复用的** <see cref="WriteableBitmap"/> 并让 Image 重绘。
///
/// 关键约束（都来自 Spike 1 实测，不要改）：
///   * 同一块 <see cref="WriteableBitmap"/> 反复用，只在远端分辨率变化时重建——每帧 new 一张
///     1440p 位图会直接把 GC 和显存打爆；
///   * 拷贝必须逐行按 <c>stride</c> 走：1440p 的实际 stride 常常大于 width*4（行尾 padding），
///     直接整块 memcpy 会把图像撕成斜的；
///   * 只在 UI 线程调用（Avalonia 位图不是线程安全的），由上层 Dispatcher 保证。
/// </summary>
public sealed class RemoteScreenPresenter
{
    private readonly Image _target;
    private WriteableBitmap? _bitmap;
    private int _width;
    private int _height;
    private long _presented;
    private DateTime _fpsWindowStart = DateTime.UtcNow;
    private DateTime _diagStart = DateTime.UtcNow;
    private int _fpsWindowFrames;
    private int _allocations;

    public RemoteScreenPresenter(Image target)
    {
        _target = target;
    }

    /// <summary>每秒更新一次，参数是过去一秒实际渲染的帧率。</summary>
    public event Action<double>? FpsChanged;

    /// <summary>累计渲染成功帧数（= UI 真正拿到并重绘的帧数）。</summary>
    public long PresentedFrames => Interlocked.Read(ref _presented);

    /// <summary>WriteableBitmap 分配次数（应为 1，分辨率变化才会涨；用来证明没有每帧 new）。</summary>
    public int Allocations => _allocations;

    public int Width => _width;

    public int Height => _height;

    public double Fps { get; private set; }

    public bool HasFrame => _bitmap is not null;

    /// <summary>把一帧 BGRA8 像素拷进复用位图并通知重绘。返回 false 表示参数不可用（未就绪/尺寸非法）。</summary>
    public bool Present(IntPtr pixels, int width, int height, int stride)
    {
        if (pixels == IntPtr.Zero || width <= 0 || height <= 0)
        {
            return false;
        }
        if (stride < width * 4)
        {
            AppLog.Warn($"remote screen: 非法 stride={stride}（{width}x{height} 至少需要 {width * 4}），本帧丢弃");
            return false;
        }

        if (_bitmap is null || _width != width || _height != height)
        {
            // 只在分辨率变化时重建；旧位图必须先释放，否则会累积。
            _bitmap?.Dispose();
            _bitmap = new WriteableBitmap(
                new PixelSize(width, height),
                new Vector(96, 96),
                PixelFormat.Bgra8888,
                AlphaFormat.Opaque);
            _target.Source = _bitmap;
            _width = width;
            _height = height;
            _allocations++;
            AppLog.Pass($"remote screen: WriteableBitmap {width}x{height} 已创建（allocs={_allocations}）");
        }

        using (var buffer = _bitmap.Lock())
        {
            // 目标行可能带 padding，源行也可能带 padding：逐行按两边的行宽分别走。
            var srcRowBytes = stride;
            var dstRowBytes = buffer.RowBytes;
            var copyBytes = Math.Min(srcRowBytes, dstRowBytes);
            unsafe
            {
                var src = (byte*)pixels;
                var dst = (byte*)buffer.Address;
                for (var y = 0; y < height; y++)
                {
                    Buffer.MemoryCopy(
                        src + (long)y * srcRowBytes,
                        dst + (long)y * dstRowBytes,
                        dstRowBytes,
                        copyBytes);
                }
            }
        }

        _target.InvalidateVisual();
        Interlocked.Increment(ref _presented);
        TrackFps();
        return true;
    }

    /// <summary>清空画面（共享结束/停止观看时调用）。位图保留，便于下次复用；比分发时重建便宜。</summary>
    public void Clear()
    {
        _target.Source = null;
        _presented = 0;
        _fpsWindowFrames = 0;
        _fpsWindowStart = DateTime.UtcNow;
        Fps = 0;
    }

    private void TrackFps()
    {
        _fpsWindowFrames++;
        var elapsed = (DateTime.UtcNow - _fpsWindowStart).TotalSeconds;
        if (elapsed < 1.0)
        {
            return;
        }
        Fps = _fpsWindowFrames / elapsed;
        _fpsWindowFrames = 0;
        _fpsWindowStart = DateTime.UtcNow;
        AppLog.Pass($"remote screen: {Fps:F1} fps ({_width}x{_height}, presented={_presented}, allocs={_allocations})");
        FpsChanged?.Invoke(Fps);
        DiagLog();
    }

    /// <summary>
    /// 每 5 秒打一条内存诊断：托管堆 / 工作集 / 私有字节分开看，便于判定「涨的是托管还是原生/GPU」。
    /// </summary>
    private void DiagLog()
    {
        if ((DateTime.UtcNow - _diagStart).TotalSeconds < 5)
        {
            return;
        }
        _diagStart = DateTime.UtcNow;
        using var proc = System.Diagnostics.Process.GetCurrentProcess();
        proc.Refresh();
        AppLog.Info($"remote screen diag: ws={proc.WorkingSet64 / 1048576}MB " +
                    $"priv={proc.PrivateMemorySize64 / 1048576}MB managed={GC.GetTotalMemory(false) / 1048576}MB " +
                    $"gc={GC.CollectionCount(0)}/{GC.CollectionCount(1)}/{GC.CollectionCount(2)}");
    }
}
