using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Media.Imaging;
using Avalonia.Platform;
using Avalonia.Threading;

namespace SpikeFrame;

/// <summary>
/// Spike 1：在本机验证「1080p60 帧以受控成本进入 Avalonia 表面」。
/// 阶段 A（本文件当前实现）：帧在托管侧生成，经 WriteableBitmap 直接写入并上传。
/// 阶段 B：改由 C++ 媒体核心生成，帧通过 C ABI 取回后写入同一张 WriteableBitmap。
/// 运行固定 30 秒后把统计写入 exe 同目录的 frame-report.txt 并退出，便于从 WSL 侧无头取证。
/// </summary>
public partial class MainWindow : Window
{
    private const int FrameWidth = 1920;
    private const int FrameHeight = 1080;
    private const int BytesPerFrame = FrameWidth * FrameHeight * 4;
    private const int DurationMs = 30_000;
    private const int StatsIntervalMs = 500;

    private readonly WriteableBitmap _bitmap;
    private readonly byte[] _pattern = new byte[BytesPerFrame];
    private readonly List<double> _renderTimes = new(4096);
    private readonly DispatcherTimer _timer;
    private readonly Stopwatch _clock = Stopwatch.StartNew();
    private readonly Process _process = Process.GetCurrentProcess();

    private TimeSpan _cpuAtStart;
    private double _lastStatsAtMs;
    private int _framesAtLastStats;
    private int _frames;
    private bool _finished;

    public MainWindow()
    {
        InitializeComponent();

        BuildTestPattern();

        _bitmap = new WriteableBitmap(
            new PixelSize(FrameWidth, FrameHeight),
            new Vector(96, 96),
            PixelFormat.Bgra8888,
            AlphaFormat.Premul);

        Surface.Source = _bitmap;
        _cpuAtStart = _process.TotalProcessorTime;

        // 第一个命令行参数可覆盖打帧间隔（毫秒），便于同一份产物对比不同节奏。
        var intervalMs = 16;
        var argv = Environment.GetCommandLineArgs();
        if (argv.Length > 1 && int.TryParse(argv[1], out var requested) && requested > 0)
        {
            intervalMs = requested;
        }

        _timer = new DispatcherTimer(DispatcherPriority.Render)
        {
            Interval = TimeSpan.FromMilliseconds(intervalMs)
        };
        _timer.Tick += OnTick;
        _timer.Start();
    }

    private void BuildTestPattern()
    {
        for (var y = 0; y < FrameHeight; y++)
        {
            var row = y * FrameWidth * 4;
            for (var x = 0; x < FrameWidth; x++)
            {
                var i = row + x * 4;
                _pattern[i] = (byte)(x * 255 / FrameWidth);
                _pattern[i + 1] = (byte)(y * 255 / FrameHeight);
                _pattern[i + 2] = (byte)((x + y) * 255 / (FrameWidth + FrameHeight));
                _pattern[i + 3] = 255;
            }
        }
    }

    private void OnTick(object? sender, EventArgs e)
    {
        if (_finished)
        {
            return;
        }

        var t0 = Stopwatch.GetTimestamp();

        using (var fb = _bitmap.Lock())
        {
            Marshal.Copy(_pattern, 0, fb.Address, BytesPerFrame);
        }

        var t1 = Stopwatch.GetTimestamp();
        _renderTimes.Add((t1 - t0) * 1000.0 / Stopwatch.Frequency);
        _frames++;

        Surface.InvalidateVisual();
        UpdateStats();

        if (_clock.ElapsedMilliseconds >= DurationMs)
        {
            Finish();
        }
    }

    private void UpdateStats()
    {
        var elapsedMs = _clock.Elapsed.TotalMilliseconds;
        if (elapsedMs - _lastStatsAtMs < StatsIntervalMs)
        {
            return;
        }

        var fps = (_frames - _framesAtLastStats) * 1000.0 / (elapsedMs - _lastStatsAtMs);
        _lastStatsAtMs = elapsedMs;
        _framesAtLastStats = _frames;

        var cpu = (_process.TotalProcessorTime - _cpuAtStart).TotalSeconds;
        var cpuPercent = cpu / _clock.Elapsed.TotalSeconds / Environment.ProcessorCount * 100.0;

        Stats.Text = string.Format(
            CultureInfo.InvariantCulture,
            "frames {0}   fps {1:F1}   render {2:F2} ms   cpu {3:F1}%   ws {4:F0} MB",
            _frames,
            fps,
            _renderTimes.Count > 0 ? _renderTimes[^1] : 0,
            cpuPercent,
            _process.WorkingSet64 / 1024.0 / 1024.0);
    }

    private void Finish()
    {
        _finished = true;
        _timer.Stop();

        var elapsed = _clock.Elapsed.TotalSeconds;
        var cpu = (_process.TotalProcessorTime - _cpuAtStart).TotalSeconds;

        _renderTimes.Sort();

        var report = new StringBuilder();
        report.AppendLine("spike=frame-presentation");
        report.AppendLine("mode=managed-writeablebitmap");
        report.AppendLine(string.Format(CultureInfo.InvariantCulture, "resolution={0}x{1}", FrameWidth, FrameHeight));
        report.AppendLine(string.Format(CultureInfo.InvariantCulture, "duration_s={0:F2}", elapsed));
        report.AppendLine(string.Format(CultureInfo.InvariantCulture, "frames={0}", _frames));
        report.AppendLine(string.Format(CultureInfo.InvariantCulture, "fps={0:F1}", _frames / elapsed));
        report.AppendLine(string.Format(CultureInfo.InvariantCulture, "render_ms_p50={0:F3}", Percentile(50)));
        report.AppendLine(string.Format(CultureInfo.InvariantCulture, "render_ms_p95={0:F3}", Percentile(95)));
        report.AppendLine(string.Format(CultureInfo.InvariantCulture, "render_ms_max={0:F3}", _renderTimes.Count > 0 ? _renderTimes[^1] : 0));
        report.AppendLine(string.Format(CultureInfo.InvariantCulture, "cpu_avg_percent={0:F1}", cpu / elapsed / Environment.ProcessorCount * 100.0));
        report.AppendLine(string.Format(CultureInfo.InvariantCulture, "gen0={0} gen1={1} gen2={2}", GC.CollectionCount(0), GC.CollectionCount(1), GC.CollectionCount(2)));
        report.AppendLine(string.Format(CultureInfo.InvariantCulture, "working_set_mb={0:F1}", _process.WorkingSet64 / 1024.0 / 1024.0));
        // 2026-11-01 内存差异排查：同一份代码在 UNC 与本地磁盘运行时工作集差约 30 MB，这里补充
        // 私有/分页/GC 堆与模块、句柄计数，用于判定是「记账口径差异」还是「真实分配增长」。
        report.AppendLine(string.Format(CultureInfo.InvariantCulture, "private_mb={0:F1}", _process.PrivateMemorySize64 / 1024.0 / 1024.0));
        report.AppendLine(string.Format(CultureInfo.InvariantCulture, "paged_mb={0:F1}", _process.PagedMemorySize64 / 1024.0 / 1024.0));
        report.AppendLine(string.Format(CultureInfo.InvariantCulture, "gc_heap_mb={0:F1}", GC.GetTotalMemory(false) / 1024.0 / 1024.0));
        report.AppendLine(string.Format(CultureInfo.InvariantCulture, "modules={0}", _process.Modules.Count));
        report.AppendLine(string.Format(CultureInfo.InvariantCulture, "handles={0}", _process.HandleCount));
        // TPA = 运行时实际解析到的托管程序集清单；条目数与被解析目录数能暴露「同一程序集被两份探测路径各加载一次」。
        var tpa = ((AppContext.GetData("TRUSTED_PLATFORM_ASSEMBLIES") as string) ?? string.Empty)
            .Split(Path.PathSeparator, StringSplitOptions.RemoveEmptyEntries);
        report.AppendLine(string.Format(CultureInfo.InvariantCulture, "tpa_entries={0}", tpa.Length));
        report.AppendLine(string.Format(CultureInfo.InvariantCulture, "tpa_distinct_dirs={0}", tpa.Select(p => Path.GetDirectoryName(p) ?? string.Empty).Distinct(StringComparer.OrdinalIgnoreCase).Count()));
        // 模块名清单：用于定位本地/UNC 两次运行相差的那 30 个模块到底是谁（图形栈？诊断？重复副本？）。
        var moduleNames = new List<string>();
        foreach (ProcessModule module in _process.Modules)
        {
            moduleNames.Add(Path.GetFileName(module.FileName ?? module.ModuleName) ?? module.ModuleName);
        }

        moduleNames.Sort(StringComparer.OrdinalIgnoreCase);
        report.AppendLine("modules_list=" + string.Join("|", moduleNames));
        report.AppendLine(string.Format(CultureInfo.InvariantCulture, "base_dir={0}", AppContext.BaseDirectory));
        report.AppendLine(string.Format(CultureInfo.InvariantCulture, "cores={0}", Environment.ProcessorCount));
        report.AppendLine(string.Format(CultureInfo.InvariantCulture, "interval_ms={0:F0}", _timer.Interval.TotalMilliseconds));
        // 实际渲染后端：ANGLE 裸名加载失败时 Avalonia 会静默回落软件渲染，
        // 不显式记录的话，降级后的数字会伪装成正常结果。
        var angleLoaded = moduleNames.Exists(n => string.Equals(n, "av_libGLESv2.dll", StringComparison.OrdinalIgnoreCase));
        report.AppendLine("renderer=" + (angleLoaded ? "angle-d3d11" : "skia-cpu"));

        var path = Path.Combine(AppContext.BaseDirectory, "frame-report.txt");
        File.WriteAllText(path, report.ToString());

        Close();
    }

    private double Percentile(int percent)
    {
        if (_renderTimes.Count == 0)
        {
            return 0;
        }

        var index = (int)Math.Clamp(Math.Round(percent / 100.0 * (_renderTimes.Count - 1)), 0, _renderTimes.Count - 1);
        return _renderTimes[index];
    }
}
