// ---------------------------------------------------------------------------
// GuiApp —— Windows 调试桥的 Win32 窗口夹具（fixture）
//
// 只证明一件事：
//   由 `wdb build-run`（Windows 前台 Worker + Job Object）拉起的进程，真的在**用户交互桌面**
//   上创建并显示了一个**可见**的 Win32 窗口，且该窗口客户区被**真实绘制**过（像素级可核验）。
//
// 硬性约束：
//   * 纯 .NET 10 BCL、零 NuGet；只 P/Invoke user32 / gdi32 / kernel32 / dwmapi / ntdll；
//   * 不读取、不写入任何用户业务数据（画廊库 / SQLite / 配置 / Telegram 会话一律不碰）；
//     唯一落盘物是**自己窗口客户区**（PrintWindow 自绘输出）的 BMP 与证据 JSONL，均在自身构建输出目录内；
//   * 绝不是抓屏：screen 通道只 GetPixel 读 1 个像素，桌面/他人窗口内容一律不落盘、不外传；
//   * 不抢焦点：WS_EX_NOACTIVATE + ShowWindow(SW_SHOWNOACTIVATE)，全程不调用 SetForegroundWindow；
//   * 自动收尾：默认显示 12s 后自行退出（--duration-ms 可调）；**绝不弹 MessageBox 等用户点**；
//     也可由 `wdb stop <本任务 id>` 经 Job Object 强杀（此时不会有 exit 事件，属预期路径）。
//
// 像素取证（刻意保守，绝不擅自抓屏）：
//   1) printClient（唯一持久化的帧）—— PrintWindow(PW_CLIENTONLY) 让本窗口 WndProc 把**自己的客户区**
//      画进我们给的内存 DC：与遮挡、合成器状态无关，且只可能包含本窗口自己画出的像素；
//   2) screen（只读单点）—— GetDC(自己的 hwnd) + GetPixel 读 marker 处**一个像素**，佐证
//      「此刻桌面上这个窗口的客户区就是这些像素」。
//   **不做**整帧 BitBlt/GetDIBits、不落盘、不外传：窗口 DC 的整帧回读在窗口被覆盖时会连同
//   其它窗口（可能是用户敏感内容）的像素一起读进来。GetDIBits 亦遵守 Win32 规定：
//   目标位图必须先 SelectObject 回原对象（不得在选入 DC 的状态下取位）。
//
// 输出通道（双写，stdout 为主）：
//   stdout（Worker 重定向到 queue/jobs/<id>/stdout.log）：行前缀 `WDB_GUI `，后接单行 JSON；
//   同目录 wdb-gui-evidence.jsonl：同样内容的追加副本，供 stdout 通道不可用时兜底取证。
//
// 退出码：0=窗口可见且像素自证通过；2=参数错误；3=窗口创建失败；4=绘制/像素自证失败；5=异常。
// `--selftest`：纯逻辑自检（BGRA↔COLORREF 色序、BMP 自写头、颜色计数），**不碰任何 Win32**，
//   因此可在 Linux 上 `dotnet GuiApp.dll --selftest` 离线跑，作为像素判据本身的机器可读自证。
// ---------------------------------------------------------------------------

using System.Diagnostics;
using System.Globalization;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;

namespace GuiApp;

internal static class Program
{
    private const string GuiPrefix = "WDB_GUI ";
    private const string EvidenceFileName = "wdb-gui-evidence.jsonl";
    private const string WindowClassName = "WdbGuiFixtureWindow";
    private const string AppName = "GuiApp";
    private const int DefaultDurationMs = 12_000;
    private const int TimerId = 0x51D;
    private const int TimerPeriodMs = 250;

    // ── 绘制场景常量：每个值都必须可被外部像素核验 ────────────────────────────
    // COLORREF = 0x00BBGGRR；MarkerColor 取 RGB(255,0,200)（不常用色，降低与桌面其它像素撞色的概率）。
    private static readonly uint BgColor = Rgb(24, 28, 38);
    private static readonly uint MarkerColor = Rgb(255, 0, 200);
    private static readonly uint[] BandColors =
    {
        Rgb(230, 60, 60), Rgb(240, 150, 40), Rgb(230, 220, 60), Rgb(120, 220, 80),
        Rgb(60, 210, 160), Rgb(60, 170, 230), Rgb(90, 110, 240), Rgb(150, 90, 230),
        Rgb(230, 90, 190), Rgb(255, 255, 255), Rgb(160, 160, 160), Rgb(90, 90, 90),
        Rgb(40, 90, 40), Rgb(90, 40, 40), Rgb(40, 40, 90), Rgb(0, 0, 0),
    };

    private const int ClientWidth = 420;
    private const int ClientHeight = 260;
    private const int Margin = 8;
    private const int MarkerX = 8;
    private const int MarkerY = 8;
    private const int MarkerW = 96;
    private const int MarkerH = 48;
    private const int BandTop = 72;
    private const int BandHeight = 32;
    private const int ProbeOffsetX = 5;   // 取样点：marker 内部 (MarkerX+5, MarkerY+5)
    private const int ProbeOffsetY = 5;

    // ── 进程内状态（只在 UI 线程读写） ────────────────────────────────────────
    private static IntPtr _hwnd = IntPtr.Zero;
    private static WndProcDelegate? _wndProcKeepAlive;   // 必须保活，否则 GC 后回调地址悬空
    private static readonly List<Dictionary<string, object?>> Heartbeats = new();
    private static int _wmPaintCount;
    private static string? _firstPaintUtc;
    private static string? _lastPaintUtc;
    private static DateTimeOffset _startedUtc;
    private static long _startedTicks;
    private static string? _readyUtc;
    private static int _requestedDurationMs = DefaultDurationMs;
    private static bool _writeBmp = true;
    private static bool _bmpRefreshRequested;
    private static bool _oursEverForeground;
    private static IntPtr _foregroundAtReady;

    // printClient 通道（权威）
    private static bool _printCaptured;
    private static bool _printMarkerOk;
    private static int _printDistinct = -1;
    private static string? _printSha;
    private static int _printWidth, _printHeight;
    private static string? _printBmpPath;
    private static long _printBmpBytes = -1;
    private static string? _printBmpSha;
    private static string? _printError;

    // screen 通道：**只读单点**（不做整帧回读、不落盘），用于佐证「桌面此刻就是这个像素」
    private static bool _screenProbed;
    private static bool _screenPixelEverOk;
    private static bool _screenPixelFinalOk;
    private static uint? _screenPixelFinal;
    private static string? _screenError;

    private static (int W, int H) _windowSize;
    private static string? _lastCaptureUtc;
    private static string? _evidenceJsonlPath;
    private static string? _pidFilePath;
    private static bool _pidFileWritten;

    /// <summary>启动时（尚未创建自己的窗口）的前景窗口，用于证明「没抢焦点」。</summary>
    private static IntPtr _foregroundAtStart;

    private static int Main(string[] args)
    {
        // --selftest 必须在**任何 Win32 调用之前**分流：它只跑纯逻辑（像素解码/BMP 头/颜色序），
        // 因此可以在 Linux 上用 `dotnet GuiApp.dll --selftest` 离线执行，作为像素判据的机器可读自证。
        if (args.Length > 0 && args[0] == "--selftest")
            return SelfTest();

        _startedUtc = DateTimeOffset.UtcNow;
        _startedTicks = Stopwatch.GetTimestamp();
        _foregroundAtStart = GetForegroundWindow();

        try
        {
            if (!ParseArgs(args, out string? argError))
            {
                Emit("error", argError, 2);
                return 2;
            }

            if (_helpRequested)
            {
                Console.Out.WriteLine(Usage());
                Console.Out.Flush();
                return 0;
            }

            _evidenceJsonlPath = Path.Combine(AppContext.BaseDirectory, EvidenceFileName);
            TryResetEvidenceFile();

            NativeMethods.SetProcessDPIAwareSafe();

            IntPtr hInstance = GetModuleHandleW(null);
            _wndProcKeepAlive = WndProc;

            var wc = new WNDCLASSEXW
            {
                cbSize = (uint)Marshal.SizeOf<WNDCLASSEXW>(),
                style = 0,
                lpfnWndProc = Marshal.GetFunctionPointerForDelegate(_wndProcKeepAlive),
                cbClsExtra = 0,
                cbWndExtra = 0,
                hInstance = hInstance,
                hIcon = IntPtr.Zero,
                hCursor = LoadCursorW(IntPtr.Zero, (IntPtr)IdcArrow),
                hbrBackground = IntPtr.Zero,      // 背景完全由 WM_PAINT / WM_PRINTCLIENT 负责
                lpszMenuName = null,
                lpszClassName = WindowClassName,
                hIconSm = IntPtr.Zero,
            };
            ushort atom = RegisterClassExW(ref wc);
            if (atom == 0)
            {
                Emit("error", $"RegisterClassExW 失败（win32={Marshal.GetLastWin32Error()}）", 3);
                return 3;
            }

            // 不带 WS_VISIBLE：随后用 SW_SHOWNOACTIVATE 显示，避免激活/抢焦点。
            const uint style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN;
            const uint exStyle = WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_CONTROLPARENT;

            // 期望的是 **客户区** 420x260：CreateWindowExW 收的是整窗尺寸，
            // 必须先 AdjustWindowRectEx 把标题栏/边框算进去，否则 GetClientRect 会小于请求值。
            var want = new RECT { left = 0, top = 0, right = ClientWidth, bottom = ClientHeight };
            if (!AdjustWindowRectEx(ref want, style, false, exStyle))
                Emit("warn", $"AdjustWindowRectEx 失败（win32={Marshal.GetLastWin32Error()}），回退为整窗尺寸", null);
            int winW = Math.Max(1, want.right - want.left);
            int winH = Math.Max(1, want.bottom - want.top);

            (int x, int y) = ComputeSafePosition(winW, winH);
            _windowSize = (winW, winH);

            _hwnd = CreateWindowExW(exStyle, WindowClassName,
                $"WDB GuiApp Fixture pid={Environment.ProcessId}", style,
                x, y, winW, winH,
                IntPtr.Zero, IntPtr.Zero, hInstance, IntPtr.Zero);

            if (_hwnd == IntPtr.Zero)
            {
                Emit("error", $"CreateWindowExW 失败（win32={Marshal.GetLastWin32Error()}）", 3);
                return 3;
            }

            // SW_SHOWNOACTIVATE = 4：显示但绝不激活（不抢用户焦点）
            ShowWindow(_hwnd, SwShowNoActivate);
            UpdateWindow(_hwnd);   // 同步投递 WM_PAINT，保证 ready 事件之前至少已绘制一帧

            WritePidFile();        // 供 Windows 侧 ProcessProbe 事后核验「本进程真的没了」
            RefreshCapture();
            _readyUtc = DateTimeOffset.UtcNow.ToString("O");
            _foregroundAtReady = GetForegroundWindow();

            bool ok = PaintSelfCheckOk;
            Emit("ready", null, null);

            // 合成器/首次绘制偶尔滞后：给最多 3s 补齐机会，再落 ready_confirm
            for (int i = 0; i < 12 && !ok; i++)
            {
                Thread.Sleep(TimerPeriodMs);
                NativeMethods.PumpPending();
                RefreshCapture();
                ok = PaintSelfCheckOk;
            }
            if (!ok) Emit("ready_confirm", null, null);

            SetTimer(_hwnd, TimerId, TimerPeriodMs, IntPtr.Zero);
            MessageLoop();
            KillTimer(_hwnd, TimerId);

            _bmpRefreshRequested = true;   // 退出前刷新 BMP，保证落盘的是最终帧
            RefreshCapture();
            bool exitOk = PaintSelfCheckOk;
            Emit("exit", null, exitOk ? 0 : 4);

            if (_hwnd != IntPtr.Zero)
            {
                DestroyWindow(_hwnd);
                _hwnd = IntPtr.Zero;
            }

            return exitOk ? 0 : 4;
        }
        catch (Exception ex)
        {
            try { Emit("error", ex.GetType().Name + ": " + ex.Message, 5); }
            catch { /* 取证通道也失败时只能靠退出码 */ }
            return 5;
        }
    }

    /// <summary>自证通过判据：至少绘制过一帧，且 printClient 通道的 marker 像素与期望值逐位相等。</summary>
    private static bool PaintSelfCheckOk => _wmPaintCount >= 1 && _printMarkerOk;

    /// <summary>
    /// 把自己的 PID 写到构建输出目录下的 wdb-gui-&lt;pid&gt;.pid。
    /// 作用：WSL 侧**不得**用 os.kill / signal 判断 Windows PID，只能让 Windows 侧的 ProcessProbe
    /// 拿这个 pidfile 去 Process.GetProcessById 复核「本进程（含被 Job Object 强杀的情形）已消失」。
    /// </summary>
    private static void WritePidFile()
    {
        try
        {
            string path = Path.Combine(AppContext.BaseDirectory, $"wdb-gui-{Environment.ProcessId}.pid");
            File.WriteAllText(path, Environment.ProcessId.ToString(CultureInfo.InvariantCulture) + "\n", new UTF8Encoding(false));
            _pidFilePath = path;
            _pidFileWritten = true;
        }
        catch
        {
            _pidFileWritten = false;
        }
    }

    // ---------------------------------------------------------------- 消息循环

    private static void MessageLoop()
    {
        while (true)
        {
            int r = GetMessageW(out MSG msg, IntPtr.Zero, 0, 0);
            if (r == 0) break;          // WM_QUIT
            if (r == -1) break;         // 错误
            TranslateMessage(ref msg);
            DispatchMessageW(ref msg);
        }
    }

    private static IntPtr WndProc(IntPtr hWnd, uint msg, IntPtr wParam, IntPtr lParam)
    {
        switch (msg)
        {
            case WM_PAINT:
            {
                IntPtr hdc = BeginPaint(hWnd, out PAINTSTRUCT ps);
                try
                {
                    DrawScene(hdc);
                    _wmPaintCount++;
                    string now = DateTimeOffset.UtcNow.ToString("O");
                    _firstPaintUtc ??= now;
                    _lastPaintUtc = now;
                }
                finally
                {
                    EndPaint(hWnd, ref ps);
                }
                return IntPtr.Zero;
            }

            case WM_PRINTCLIENT:
                // PrintWindow(PW_CLIENTONLY) 走这里：把客户区画进调用者给的 DC（与遮挡无关）
                DrawScene(wParam);
                return IntPtr.Zero;

            case WM_ERASEBKGND:
                return (IntPtr)1;       // 全部交由 WM_PAINT 绘制，杜绝闪白

            case WM_TIMER:
                OnTick();
                return IntPtr.Zero;

            case WM_CLOSE:
            case WM_DESTROY:
                PostQuitMessage(0);
                return IntPtr.Zero;

            default:
                return DefWindowProcW(hWnd, msg, wParam, lParam);
        }
    }

    private static void OnTick()
    {
        if (_hwnd == IntPtr.Zero) return;

        long elapsedMs = (long)Stopwatch.GetElapsedTime(_startedTicks).TotalMilliseconds;
        IntPtr fg = GetForegroundWindow();
        bool oursFg = fg != IntPtr.Zero && fg == _hwnd;
        if (oursFg) _oursEverForeground = true;

        // 心跳采样（最多 8 条，避免 stdout 过长）：窗口可见性 + 前景窗口是否是自己
        if (Heartbeats.Count < 8 && (Heartbeats.Count == 0 || elapsedMs - LastHeartbeatMs() >= 1_400))
        {
            Heartbeats.Add(new Dictionary<string, object?>
            {
                ["tMs"] = elapsedMs,
                ["atUtc"] = DateTimeOffset.UtcNow.ToString("O"),
                ["isWindowVisible"] = IsWindowVisible(_hwnd),
                ["isIconic"] = IsIconic(_hwnd),
                ["foreground"] = Hex(fg),
                ["oursIsForeground"] = oursFg,
                ["wmPaintCount"] = _wmPaintCount,
            });
        }

        RefreshCapture();

        if (elapsedMs >= _requestedDurationMs)
        {
            KillTimer(_hwnd, TimerId);
            PostQuitMessage(0);
        }
    }

    private static long LastHeartbeatMs()
    {
        if (Heartbeats.Count == 0) return long.MinValue;
        return Convert.ToInt64(Heartbeats[^1]["tMs"], CultureInfo.InvariantCulture);
    }

    // ---------------------------------------------------------------- 绘制

    private static void DrawScene(IntPtr hdc)
    {
        if (hdc == IntPtr.Zero || _hwnd == IntPtr.Zero) return;
        if (!GetClientRect(_hwnd, out RECT rc)) return;
        int w = rc.right - rc.left;
        int h = rc.bottom - rc.top;
        if (w <= 0 || h <= 0) return;

        // 1) 纯色背景
        IntPtr bg = CreateSolidBrush(BgColor);
        var full = new RECT { left = 0, top = 0, right = w, bottom = h };
        FillRect(hdc, ref full, bg);
        DeleteObject(bg);

        // 2) marker 矩形（像素自证的唯一基准色）
        IntPtr mk = CreateSolidBrush(MarkerColor);
        var marker = new RECT { left = MarkerX, top = MarkerY, right = MarkerX + MarkerW, bottom = MarkerY + MarkerH };
        FillRect(hdc, ref marker, mk);
        DeleteObject(mk);

        // 3) 16 条彩色竖带（保证 DIB 里颜色数 > 1，杜绝「空白窗口也能过」）
        int bandW = Math.Max(1, (w - 2 * Margin) / BandColors.Length);
        for (int i = 0; i < BandColors.Length; i++)
        {
            int left = Margin + i * bandW;
            if (left >= w) break;
            var r = new RECT { left = left, top = BandTop, right = Math.Min(w, left + bandW), bottom = BandTop + BandHeight };
            IntPtr br = CreateSolidBrush(BandColors[i]);
            FillRect(hdc, ref r, br);
            DeleteObject(br);
        }

        // 4) 文本（证明这是本 fixture 自己的窗口）
        IntPtr font = GetStockObject(DefaultGuiFont);
        IntPtr oldFont = font != IntPtr.Zero ? SelectObject(hdc, font) : IntPtr.Zero;
        int oldBk = SetBkMode(hdc, Transparent);
        uint oldColor = SetTextColor(hdc, Rgb(235, 240, 250));

        long elapsedMs = (long)Stopwatch.GetElapsedTime(_startedTicks).TotalMilliseconds;
        var line1 = new RECT { left = Margin + 4, top = BandTop + BandHeight + 8, right = w - 4, bottom = BandTop + BandHeight + 30 };
        var line2 = new RECT { left = MarkerX, top = MarkerY + MarkerH - 20, right = MarkerX + MarkerW - 4, bottom = MarkerY + MarkerH - 2 };
        DrawTextW(hdc, $"WDB GUI FIXTURE pid={Environment.ProcessId} hwnd=0x{_hwnd.ToInt64():X}", -1, ref line1, DtLeft | DtSingleLine | DtNoPrefix | DtVCenter);
        DrawTextW(hdc, $"t={elapsedMs}ms paint={_wmPaintCount}", -1, ref line2, DtLeft | DtSingleLine | DtNoPrefix | DtVCenter);

        SetTextColor(hdc, oldColor);
        SetBkMode(hdc, oldBk);
        if (oldFont != IntPtr.Zero) SelectObject(hdc, oldFont);
    }

    // ---------------------------------------------------------------- 像素自证

    /// <summary>
    /// 一次取证：
    ///   * printClient（权威通道）—— PrintWindow(PW_CLIENTONLY) 让本窗口的 WndProc 把**自己的客户区**
    ///     画进我们给的内存 DIB；与遮挡无关，且**只可能包含本窗口自己画的像素**（绝无他人窗口内容）。
    ///     这是唯一被持久化的帧（BMP/SHA256）。
    ///   * screen（单点探针）—— GetDC(自己的 hwnd) + GetPixel 只读**一个点**，用来佐证「此刻桌面上这个
    ///     窗口的客户区确实是这些像素」。**刻意不做整帧 BitBlt/GetDIBits、不落盘、不外传**：窗口 DC 的
    ///     整帧回读在窗口被覆盖时会把别的窗口（可能是用户敏感内容）像素一起读进来，属于擅自抓屏。
    /// </summary>
    private static void RefreshCapture()
    {
        if (_hwnd == IntPtr.Zero) return;
        if (!GetClientRect(_hwnd, out RECT rc)) return;
        int w = rc.right - rc.left;
        int h = rc.bottom - rc.top;
        if (w <= 0 || h <= 0) return;

        int px = MarkerX + ProbeOffsetX;
        int py = MarkerY + ProbeOffsetY;

        IntPtr screenDc = GetDC(IntPtr.Zero);       // 仅用于创建兼容 DC/Bitmap（不做任何抓屏）
        IntPtr windowDc = GetDC(_hwnd);
        if (windowDc == IntPtr.Zero)
        {
            if (screenDc != IntPtr.Zero) ReleaseDC(IntPtr.Zero, screenDc);
            _printError = "GetDC(自己的 hwnd) 返回 NULL";
            return;
        }

        try
        {
            // ── 通道 1：printClient（自己的客户区，唯一被持久化的帧） ───────────
            IntPtr refDc = screenDc != IntPtr.Zero ? screenDc : windowDc;
            IntPtr mem1 = CreateCompatibleDC(refDc);
            IntPtr bmp1 = CreateCompatibleBitmap(refDc, w, h);
            if (mem1 != IntPtr.Zero && bmp1 != IntPtr.Zero)
            {
                IntPtr old1 = SelectObject(mem1, bmp1);
                bool printed = PrintWindow(_hwnd, mem1, PwClientOnly);
                // GetDIBits 的 Win32 硬性要求：目标位图**不得**处于被选入 DC 的状态，
                // 必须先把原对象选回去再取位（否则文档语义下行为未定义）。
                if (old1 != IntPtr.Zero) SelectObject(mem1, old1);
                string err1 = "";
                if (printed && TryReadDib(mem1, bmp1, w, h, out byte[] buf1, out err1))
                {
                    _printCaptured = true;
                    _printError = null;
                    _printWidth = w;
                    _printHeight = h;
                    _printMarkerOk = ReadMarker(buf1, w, px, py, out _);
                    _printDistinct = CountDistinct(buf1);
                    _printSha = Sha256Hex(buf1);
                    if (_writeBmp && (_printBmpPath is null || _bmpRefreshRequested))
                    {
                        WriteBmpSafe(buf1, w, h, $"wdb-gui-printclient-{Environment.ProcessId}.bmp",
                            out _printBmpPath, out _printBmpBytes, out _printBmpSha, ref _printError);
                    }
                }
                else
                {
                    _printError = printed ? err1 : $"PrintWindow 失败（win32={Marshal.GetLastWin32Error()}）";
                }
                DeleteObject(bmp1);
                DeleteDC(mem1);
            }
            else
            {
                _printError = $"CreateCompatibleDC/Bitmap(printClient) 失败（win32={Marshal.GetLastWin32Error()}）";
            }

            // ── 通道 2：screen 单点探针（只读一个像素；不做整帧回读、不落盘） ───
            uint got = GetPixel(windowDc, px, py);
            if (got != ClrInvalid)
            {
                _screenProbed = true;
                _screenError = null;
                _screenPixelFinal = got;
                bool pixelOk = (got & 0x00FFFFFF) == (MarkerColor & 0x00FFFFFF);
                _screenPixelFinalOk = pixelOk;
                if (pixelOk) _screenPixelEverOk = true;
            }
            else
            {
                _screenError = $"GetPixel 失败（win32={Marshal.GetLastWin32Error()}）";
            }

            _bmpRefreshRequested = false;
            _lastCaptureUtc = DateTimeOffset.UtcNow.ToString("O");
        }
        finally
        {
            ReleaseDC(_hwnd, windowDc);
            if (screenDc != IntPtr.Zero) ReleaseDC(IntPtr.Zero, screenDc);
        }
    }

    /// <summary>把内存 DC 里那张**未被选入**的位图读成 32bpp 顶朝下 BGRA 缓冲区（每像素 [B,G,R,A]）。</summary>
    private static bool TryReadDib(IntPtr hdcMem, IntPtr hbm, int w, int h, out byte[] buf, out string error)
    {
        buf = Array.Empty<byte>();
        error = "";
        var bmi = new BITMAPINFOHEADER
        {
            biSize = (uint)Marshal.SizeOf<BITMAPINFOHEADER>(),
            biWidth = w,
            biHeight = -h,          // 负数 = 顶朝下，便于按行索引
            biPlanes = 1,
            biBitCount = 32,
            biCompression = BiRgb,
            biSizeImage = (uint)(w * h * 4),
        };
        var tmp = new byte[w * h * 4];
        if (GetDIBits(hdcMem, hbm, 0, (uint)h, tmp, ref bmi, DibRgbColors) == 0)
        {
            error = $"GetDIBits 失败（win32={Marshal.GetLastWin32Error()}）";
            return false;
        }
        buf = tmp;
        return true;
    }

    /// <summary>
    /// 取自己客户区 DIB 中 (px,py) 的像素并与期望 marker 色比对（忽略 alpha 通道）。
    /// DIB 是 BGRA 字节序，而基准 MarkerColor 是 **COLORREF(0x00BBGGRR)**：
    /// 必须按 R | G&lt;&lt;8 | B&lt;&lt;16 组装；写成 R&lt;&lt;16|G&lt;&lt;8|B 会把 R/B 颠倒，导致「真有窗口也判红」。
    /// 该逻辑由 --selftest 的离线用例（含 R/B 交换的反证）看住，Linux 侧也能跑。
    /// </summary>
    private static bool ReadMarker(byte[] bgra, int w, int px, int py, out uint got)
    {
        int off = (py * w + px) * 4;
        got = (uint)(bgra[off + 2] | bgra[off + 1] << 8 | bgra[off + 0] << 16);   // R | G<<8 | B<<16
        return got == (MarkerColor & 0x00FFFFFF);
    }

    private static int CountDistinct(byte[] bgra)
    {
        var seen = new HashSet<int>();
        for (int i = 0; i < bgra.Length; i += 4)
        {
            if (seen.Count >= 4096) break;
            seen.Add(bgra[i] | bgra[i + 1] << 8 | bgra[i + 2] << 16);
        }
        return seen.Count;
    }

    private static string Sha256Hex(byte[] data) => Convert.ToHexString(SHA256.HashData(data)).ToLowerInvariant();

    private static void WriteBmpSafe(byte[] bgra, int w, int h, string fileName,
        out string? path, out long bytes, out string? sha, ref string? errorSlot)
    {
        path = null; bytes = -1; sha = null;
        try
        {
            string p = Path.Combine(AppContext.BaseDirectory, fileName);
            byte[] bmp = BuildBmp24(w, h, bgra);
            File.WriteAllBytes(p, bmp);
            path = p;
            bytes = bmp.LongLength;
            sha = Sha256Hex(bmp);
        }
        catch (Exception ex)
        {
            errorSlot = ex.GetType().Name + ": " + ex.Message;
        }
    }

    /// <summary>把 32bpp 顶朝下的 BGRA 缓冲区写成自写头的 24bpp BMP（零第三方库）。</summary>
    private static byte[] BuildBmp24(int w, int h, byte[] bgra)
    {
        int rowRaw = w * 3;
        int rowPad = (rowRaw + 3) & ~3;
        int pixelBytes = rowPad * h;
        const int headerBytes = 14 + 40;
        var outp = new byte[headerBytes + pixelBytes];

        // BITMAPFILEHEADER (14B)
        outp[0] = (byte)'B'; outp[1] = (byte)'M';
        WriteI32(outp, 2, headerBytes + pixelBytes);
        WriteI32(outp, 6, 0);
        WriteI32(outp, 10, headerBytes);
        // BITMAPINFOHEADER (40B)。biHeight 为正 → 底朝下，故行序倒着写。
        WriteI32(outp, 14, 40);
        WriteI32(outp, 18, w);
        WriteI32(outp, 22, h);
        WriteU16(outp, 26, 1);
        WriteU16(outp, 28, 24);
        WriteI32(outp, 30, 0);            // BI_RGB
        WriteI32(outp, 34, pixelBytes);
        WriteI32(outp, 38, 2835);
        WriteI32(outp, 42, 2835);
        WriteI32(outp, 46, 0);
        WriteI32(outp, 50, 0);

        for (int y = 0; y < h; y++)
        {
            int src = y * w * 4;
            int dst = headerBytes + (h - 1 - y) * rowPad;
            for (int x = 0; x < w; x++)
            {
                outp[dst + x * 3 + 0] = bgra[src + x * 4 + 0];  // B
                outp[dst + x * 3 + 1] = bgra[src + x * 4 + 1];  // G
                outp[dst + x * 3 + 2] = bgra[src + x * 4 + 2];  // R
            }
        }
        return outp;
    }

    private static void WriteI32(byte[] b, int at, int v)
    {
        b[at] = (byte)v; b[at + 1] = (byte)(v >> 8); b[at + 2] = (byte)(v >> 16); b[at + 3] = (byte)(v >> 24);
    }

    private static void WriteU16(byte[] b, int at, ushort v)
    {
        b[at] = (byte)v; b[at + 1] = (byte)(v >> 8);
    }

    // ---------------------------------------------------------------- 证据输出

    private static void Emit(string ev, string? error, int? exitCode)
    {
        Dictionary<string, object?> payload = Snapshot(ev, error, exitCode);
        string line;
        try
        {
            line = GuiPrefix + JsonSerializer.Serialize(payload, JsonOpts) + "\n";
        }
        catch (Exception ex)
        {
            line = GuiPrefix + "{\"event\":\"serialize_error\",\"error\":\"" + ex.GetType().Name + "\"}\n";
        }

        try
        {
            Console.Out.Write(line);
            Console.Out.Flush();
        }
        catch
        {
            // WinExe 且 stdout 未重定向时可能不可写；证据通道退化为 jsonl 文件。
        }

        try
        {
            string path = _evidenceJsonlPath ?? Path.Combine(AppContext.BaseDirectory, EvidenceFileName);
            File.AppendAllText(path, line, new UTF8Encoding(false));
        }
        catch
        {
            // 兜底通道失败也不影响主通道。
        }
    }

    private static readonly JsonSerializerOptions JsonOpts = new() { WriteIndented = false };

    private static Dictionary<string, object?> Snapshot(string ev, string? error, int? exitCode)
    {
        long elapsedMs = (long)Stopwatch.GetElapsedTime(_startedTicks).TotalMilliseconds;
        return new Dictionary<string, object?>
        {
            ["event"] = ev,
            ["app"] = AppName,
            ["fixture"] = "win32-window",
            ["guiVerified"] = null,   // 由 WSL 侧验收脚本裁决，夹具不自封
            ["selfCheckOk"] = PaintSelfCheckOk,
            ["error"] = error,
            ["exitCode"] = exitCode,
            ["pid"] = Environment.ProcessId,
            ["os"] = OsInfo(),
            ["session"] = SessionInfo(),
            ["window"] = WindowInfo(),
            ["paint"] = PaintInfo(),
            ["focus"] = new Dictionary<string, object?>
            {
                ["foregroundAtStart"] = Hex(_foregroundAtStart),
                ["foregroundAtReady"] = Hex(_foregroundAtReady),
                ["foregroundAtExit"] = Hex(GetForegroundWindow()),
                ["oursEverForeground"] = _oursEverForeground,
                ["oursIsForegroundNow"] = _hwnd != IntPtr.Zero && GetForegroundWindow() == _hwnd,
                ["activeWindowOfThread"] = Hex(GetActiveWindow()),
                ["note"] = "WS_EX_NOACTIVATE + SW_SHOWNOACTIVATE；从不调用 SetForegroundWindow",
            },
            ["env"] = EnvInfo(),
            ["timing"] = new Dictionary<string, object?>
            {
                ["startedUtc"] = _startedUtc.ToString("O"),
                ["readyUtc"] = _readyUtc,
                ["nowUtc"] = DateTimeOffset.UtcNow.ToString("O"),
                ["elapsedMs"] = elapsedMs,
                ["requestedDurationMs"] = _requestedDurationMs,
                ["autoExit"] = true,
            },
            ["heartbeats"] = Heartbeats,
            ["evidenceJsonl"] = _evidenceJsonlPath,
            ["pidFile"] = _pidFilePath,
            ["pidFileWritten"] = _pidFileWritten,
        };
    }

    private static Dictionary<string, object?> OsInfo()
    {
        var rtl = new RTL_OSVERSIONINFOEXW { dwOSVersionInfoSize = (uint)Marshal.SizeOf<RTL_OSVERSIONINFOEXW>() };
        int status = RtlGetVersion(ref rtl);
        return new Dictionary<string, object?>
        {
            ["platform"] = Environment.OSVersion.Platform.ToString(),
            ["osDescription"] = RuntimeInformation.OSDescription,
            ["environmentOsVersion"] = Environment.OSVersion.VersionString,
            ["version"] = Environment.OSVersion.Version.ToString(),
            ["rtlStatus"] = status,
            ["major"] = rtl.dwMajorVersion,
            ["minor"] = rtl.dwMinorVersion,
            ["build"] = rtl.dwBuildNumber,
            ["platformId"] = rtl.dwPlatformId,
            ["is64BitOs"] = Environment.Is64BitOperatingSystem,
            ["is64BitProcess"] = Environment.Is64BitProcess,
            ["framework"] = RuntimeInformation.FrameworkDescription,
            ["processArch"] = RuntimeInformation.ProcessArchitecture.ToString(),
        };
    }

    private static Dictionary<string, object?> SessionInfo()
    {
        uint session = 0;
        bool okSession = ProcessIdToSessionId((uint)Environment.ProcessId, out session);
        uint consoleSession = WTSGetActiveConsoleSessionId();

        string winSta = UserObjectName(GetProcessWindowStation());
        string desk = UserObjectName(GetThreadDesktop(GetCurrentThreadId()));

        return new Dictionary<string, object?>
        {
            ["processSessionId"] = session,
            ["processSessionIdOk"] = okSession,
            ["activeConsoleSessionId"] = consoleSession,
            ["isConsoleSession"] = okSession && session == consoleSession,
            ["windowStation"] = winSta,
            ["desktop"] = desk,
            ["isInteractiveDesktop"] = string.Equals(winSta, "WinSta0", StringComparison.OrdinalIgnoreCase)
                                       && string.Equals(desk, "Default", StringComparison.OrdinalIgnoreCase),
            ["desktopWindow"] = Hex(GetDesktopWindow()),
            ["dwmCompositionEnabled"] = NativeMethods.DwmCompositionEnabled(),
        };
    }

    private static Dictionary<string, object?> WindowInfo()
    {
        if (_hwnd == IntPtr.Zero)
            return new Dictionary<string, object?> { ["hwnd"] = null, ["created"] = false };

        bool visible = IsWindowVisible(_hwnd);
        bool hasRect = GetWindowRect(_hwnd, out RECT wr);
        bool hasClient = GetClientRect(_hwnd, out RECT cr);
        var monInfo = new MONITORINFO { cbSize = (uint)Marshal.SizeOf<MONITORINFO>() };
        IntPtr mon = MonitorFromWindow(_hwnd, MonitorDefaultToNull);
        bool hasMon = mon != IntPtr.Zero && GetMonitorInfoW(mon, ref monInfo);
        var (enumCount, oursEnumerated) = NativeMethods.CountTopLevelWindows(_hwnd);

        return new Dictionary<string, object?>
        {
            ["created"] = true,
            ["hwnd"] = Hex(_hwnd),
            ["hwndValue"] = _hwnd.ToInt64(),
            ["className"] = NativeMethods.GetClassNameOf(_hwnd),
            ["title"] = NativeMethods.GetWindowTextOf(_hwnd),
            ["isWindow"] = IsWindow(_hwnd),
            ["isWindowVisible"] = visible,
            ["isIconic"] = IsIconic(_hwnd),
            ["isZoomed"] = IsZoomed(_hwnd),
            ["style"] = "0x" + ((uint)GetWindowLongW(_hwnd, GwlStyle)).ToString("X8"),
            ["exStyle"] = "0x" + ((uint)GetWindowLongW(_hwnd, GwlExStyle)).ToString("X8"),
            ["rect"] = hasRect ? RectDict(wr) : null,
            ["clientRect"] = hasClient ? RectDict(cr) : null,
            ["requestedClientSize"] = new Dictionary<string, object?> { ["width"] = ClientWidth, ["height"] = ClientHeight },
            ["requestedWindowSize"] = new Dictionary<string, object?> { ["width"] = _windowSize.W, ["height"] = _windowSize.H },
            ["monitorFromWindow"] = Hex(mon),
            ["monitorCount"] = GetSystemMetrics(SmCMonitors),
            ["monitorWorkArea"] = hasMon ? RectDict(monInfo.rcWork) : null,
            ["monitorRect"] = hasMon ? RectDict(monInfo.rcMonitor) : null,
            ["onScreen"] = hasRect && mon != IntPtr.Zero,
            ["dwmCloaked"] = NativeMethods.DwmCloaked(_hwnd),
            ["dpi"] = NativeMethods.DpiForWindow(_hwnd),
            ["topLevelWindowCount"] = enumCount,
            ["oursEnumeratedAsTopLevel"] = oursEnumerated,
        };
    }

    private static Dictionary<string, object?> PaintInfo() => new()
    {
        ["wmPaintCount"] = _wmPaintCount,
        ["firstPaintUtc"] = _firstPaintUtc,
        ["lastPaintUtc"] = _lastPaintUtc,
        ["lastCaptureUtc"] = _lastCaptureUtc,
        ["markerRectClient"] = RectDict(MarkerX, MarkerY, MarkerX + MarkerW, MarkerY + MarkerH),
        ["probePointClient"] = new Dictionary<string, object?> { ["x"] = MarkerX + ProbeOffsetX, ["y"] = MarkerY + ProbeOffsetY },
        ["expectedMarkerColorRef"] = Hex(MarkerColor),
        ["scene"] = "纯色背景 RGB(24,28,38) + marker 矩形 RGB(255,0,200)@(8,8,96x48) + 16 彩色竖带 + 文本",
        ["printClient"] = new Dictionary<string, object?>
        {
            ["method"] = "PrintWindow(PW_CLIENTONLY) → 本窗口 WndProc 画进内存 DIB（与遮挡无关，权威通道）",
            ["captured"] = _printCaptured,
            ["markerMatches"] = _printMarkerOk,
            ["distinctColors"] = _printDistinct,
            ["sha256"] = _printSha,
            ["width"] = _printWidth,
            ["height"] = _printHeight,
            ["bmpPath"] = _printBmpPath,
            ["bmpBytes"] = _printBmpBytes,
            ["bmpSha256"] = _printBmpSha,
            ["error"] = _printError,
        },
        ["screen"] = new Dictionary<string, object?>
        {
            ["method"] = "GetDC(自己的 hwnd) + GetPixel 单点探针（只读 marker 处的 1 个像素；受遮挡影响）",
            ["probed"] = _screenProbed,
            ["pixelMatchesEver"] = _screenPixelEverOk,
            ["pixelMatchesFinal"] = _screenPixelFinalOk,
            ["pixelFromWindowDc"] = _screenPixelFinal is null ? null : Hex(_screenPixelFinal.Value),
            ["error"] = _screenError,
            ["framePersisted"] = false,
            ["note"] = "刻意不做整帧 BitBlt/GetDIBits、不落盘、不外传：窗口 DC 整帧在窗口被覆盖时会混入他人窗口像素（可能是用户敏感内容）",
        },
        ["captureScope"] = "唯一持久化的帧 = PrintWindow(PW_CLIENTONLY) 由本窗口 WM_PRINTCLIENT 画出的自己客户区（420x260）；screen 通道只读 1 个像素点",
    };

    private static Dictionary<string, object?> EnvInfo()
    {
        string exe = Environment.ProcessPath ?? "";
        return new Dictionary<string, object?>
        {
            ["exePath"] = exe,
            ["exeOnLocalDrive"] = NativeMethods.IsLocalDrivePath(exe),
            ["exeIsUnc"] = exe.StartsWith(@"\\", StringComparison.Ordinal),
            ["exeExists"] = exe.Length > 0 && File.Exists(exe),
            ["baseDirectory"] = AppContext.BaseDirectory,
            ["currentDirectory"] = Environment.CurrentDirectory,
            ["commandLine"] = Environment.CommandLine,
            ["entryAssemblyLocation"] = System.Reflection.Assembly.GetEntryAssembly()?.Location,
            ["runtimeVersion"] = Environment.Version.ToString(),
            ["outputType"] = "WinExe",
            ["stdoutRedirected"] = Console.IsOutputRedirected,
            ["packageRefs"] = "none (pure BCL, zero NuGet)",
        };
    }

    // ---------------------------------------------------------------- 离线自检（--selftest）

    /// <summary>
    /// 纯逻辑自检：**不调用任何 Win32 API**，故可在 Linux/CI 上跑，且独立于「窗口有没有起来」，
    /// 专门看住像素判据本身（尤其是 BGRA ↔ COLORREF 的 R/B 序）。
    /// </summary>
    private static int SelfTest()
    {
        var cases = new List<Dictionary<string, object?>>();

        void Check(string name, bool expect, bool actual) =>
            cases.Add(new Dictionary<string, object?> { ["name"] = name, ["expect"] = expect, ["actual"] = actual, ["pass"] = expect == actual });

        void CheckInt(string name, int expect, int actual) =>
            cases.Add(new Dictionary<string, object?> { ["name"] = name, ["expect"] = expect, ["actual"] = actual, ["pass"] = expect == actual });

        // marker 像素：COLORREF 0x00C800FF = RGB(255,0,200) → BGRA 字节 [B=200, G=0, R=255, A=0]
        // 合成缓冲区取真实客户区尺寸（420x260），这样探针点 (13,13) 必然在界内。
        const int w = ClientWidth, h = ClientHeight;
        int px = MarkerX + ProbeOffsetX;   // 13
        int py = MarkerY + ProbeOffsetY;   // 13

        byte[] MakeBuf(int b, int g, int r)
        {
            var buf = new byte[w * h * 4];
            for (int i = 0; i < buf.Length; i += 4) { buf[i] = (byte)b; buf[i + 1] = (byte)g; buf[i + 2] = (byte)r; buf[i + 3] = 0; }
            return buf;
        }

        byte[] correct = MakeBuf(200, 0, 255);          // 正确序：B=200,G=0,R=255
        byte[] swapped = MakeBuf(255, 0, 200);          // 反证：把 R/B 交换
        byte[] background = MakeBuf(38, 28, 24);        // BgColor RGB(24,28,38)

        CheckInt("colorref_marker_is_C800FF", unchecked((int)0x00C800FF), unchecked((int)(MarkerColor & 0x00FFFFFF)));
        Check("readmarker_accepts_correct_bgra", true, ReadMarker(correct, w, px, py, out _));
        Check("readmarker_rejects_swapped_rb", false, ReadMarker(swapped, w, px, py, out _));
        Check("readmarker_rejects_background", false, ReadMarker(background, w, px, py, out _));
        Check("readmarker_ignores_alpha", true, ReadMarker(SetAlpha(correct, 0x7F), w, px, py, out _));

        // 与真实场景同构的合成帧：背景 + 仅 marker 矩形为 marker 色
        byte[] scene = MakeBuf(38, 28, 24);
        for (int y = MarkerY; y < MarkerY + MarkerH && y < h; y++)
        {
            for (int x = MarkerX; x < MarkerX + MarkerW && x < w; x++)
            {
                int o = (y * w + x) * 4;
                scene[o] = 200; scene[o + 1] = 0; scene[o + 2] = 255;
            }
        }
        Check("readmarker_accepts_marker_inside_rect", true, ReadMarker(scene, w, px, py, out _));
        Check("readmarker_rejects_outside_rect", false, ReadMarker(scene, w, MarkerX + MarkerW + 4, py, out _));
        Check("readmarker_rejects_before_first_paint", false, ReadMarker(MakeBuf(38, 28, 24), w, px, py, out _));

        CheckInt("countdistinct_is_3", 3, CountDistinct(Make3Colors(w, h)));
        CheckInt("hex_uint_format", 0, Hex(0x00C800FFu) == "0x00C800FF" ? 0 : 1);

        // BMP 自写头 round-trip：24bpp、BI_RGB、底朝下、像素逐字节正确
        int bw = 4, bh = 3;
        var small = new byte[bw * bh * 4];
        for (int y = 0; y < bh; y++)
        {
            for (int x = 0; x < bw; x++)
            {
                int o = (y * bw + x) * 4;
                small[o] = (byte)(10 + x); small[o + 1] = (byte)(20 + y); small[o + 2] = (byte)(30 + x + y); small[o + 3] = 0;
            }
        }
        byte[] bmp = BuildBmp24(bw, bh, small);
        int rowPad = ((bw * 3) + 3) & ~3;
        Check("bmp24_magic", true, bmp[0] == (byte)'B' && bmp[1] == (byte)'M');
        CheckInt("bmp24_offbits", 54, BitConverter.ToInt32(bmp, 10));
        CheckInt("bmp24_bpp", 24, BitConverter.ToInt16(bmp, 28));
        CheckInt("bmp24_width", bw, BitConverter.ToInt32(bmp, 18));
        CheckInt("bmp24_height", bh, BitConverter.ToInt32(bmp, 22));
        CheckInt("bmp24_size", 54 + rowPad * bh, bmp.Length);
        // 顶朝下 (x=1,y=0) → 文件里最后一行；BGR 顺序
        int at = 54 + (bh - 1) * rowPad + 1 * 3;
        Check("bmp24_pixel_bgr_order", true,
            bmp[at] == 10 + 1 && bmp[at + 1] == 20 + 0 && bmp[at + 2] == 30 + 1 + 0);

        bool ok = cases.All(c => c["pass"] is true);
        var payload = new Dictionary<string, object?>
        {
            ["tool"] = "guiapp-selftest",
            ["schema"] = 1,
            ["ok"] = ok,
            ["cases"] = cases,
            ["os"] = RuntimeInformation.OSDescription,
            ["framework"] = RuntimeInformation.FrameworkDescription,
            ["note"] = "纯逻辑，不调用任何 Win32 API；用于在 Linux/CI 上看住像素判据（含 R/B 交换反证）",
        };
        string line = "WDB_GUI_SELFTEST " + JsonSerializer.Serialize(payload, JsonOpts);
        Console.Out.WriteLine(line);
        Console.Out.Flush();
        try { File.WriteAllText(Path.Combine(AppContext.BaseDirectory, "wdb-gui-selftest.json"), line, new UTF8Encoding(false)); }
        catch { /* 自检输出以 stdout 为准 */ }
        return ok ? 0 : 1;
    }

    private static byte[] SetAlpha(byte[] src, byte a)
    {
        var copy = (byte[])src.Clone();
        for (int i = 3; i < copy.Length; i += 4) copy[i] = a;
        return copy;
    }

    private static byte[] Make3Colors(int w, int h)
    {
        var buf = new byte[w * h * 4];
        for (int i = 0; i < buf.Length; i += 4)
        {
            int k = (i / 4) % 3;
            buf[i] = (byte)(k == 0 ? 200 : k == 1 ? 0 : 255);
            buf[i + 1] = 0;
            buf[i + 2] = (byte)(k == 0 ? 255 : k == 1 ? 200 : 0);
        }
        return buf;
    }

    // ---------------------------------------------------------------- 参数

    private static bool _helpRequested;

    private static bool ParseArgs(string[] args, out string? error)
    {
        error = null;
        for (int i = 0; i < args.Length; i++)
        {
            switch (args[i])
            {
                case "--duration-ms":
                    if (i + 1 >= args.Length
                        || !int.TryParse(args[++i], NumberStyles.Integer, CultureInfo.InvariantCulture, out int ms)
                        || ms < 500 || ms > 600_000)
                    {
                        error = "--duration-ms 需要 500..600000 的整数";
                        return false;
                    }
                    _requestedDurationMs = ms;
                    break;
                case "--no-bmp":
                    _writeBmp = false;
                    break;
                case "--help":
                case "-h":
                    _helpRequested = true;
                    break;
                default:
                    error = "未知参数：" + args[i];
                    return false;
            }
        }
        return true;
    }

    private static string Usage() =>
        "GuiApp —— Windows 调试桥 Win32 窗口夹具\n" +
        "  --duration-ms <n>  窗口显示时长后自动退出（默认 12000）\n" +
        "  --no-bmp           不写客户区 BMP 证据文件\n" +
        "  --selftest         纯逻辑自检（像素判据，零 Win32；Linux 也能跑）\n" +
        "  --help             本帮助\n" +
        "输出：stdout 与 wdb-gui-evidence.jsonl 双写，行前缀 `WDB_GUI `。\n";

    private static void TryResetEvidenceFile()
    {
        try
        {
            string path = _evidenceJsonlPath!;
            Directory.CreateDirectory(Path.GetDirectoryName(path)!);
            File.WriteAllText(path, "", new UTF8Encoding(false));
        }
        catch
        {
            _evidenceJsonlPath = null;
        }
    }

    // ---------------------------------------------------------------- 工具

    private static (int X, int Y) ComputeSafePosition(int winW, int winH)
    {
        // 放在工作区右下角内侧：小、短暂、不遮住用户主要工作区。
        var wa = new RECT();
        if (!SystemParametersInfoW(SpiGetWorkArea, 0, ref wa, 0))
        {
            wa.left = 0; wa.top = 0;
            wa.right = GetSystemMetrics(SmCxScreen);
            wa.bottom = GetSystemMetrics(SmCyScreen);
        }
        int x = Math.Max(wa.left, wa.right - winW - 24);
        int y = Math.Max(wa.top, wa.bottom - winH - 24);
        return (x, y);
    }

    private static uint Rgb(int r, int g, int b) => (uint)((r & 0xFF) | (g & 0xFF) << 8 | (b & 0xFF) << 16);

    private static string Hex(IntPtr v) => "0x" + v.ToInt64().ToString("X");

    private static string Hex(uint v) => "0x" + v.ToString("X8");

    private static Dictionary<string, object?> RectDict(RECT r) => new()
    {
        ["left"] = r.left, ["top"] = r.top, ["right"] = r.right, ["bottom"] = r.bottom,
        ["width"] = r.right - r.left, ["height"] = r.bottom - r.top,
    };

    private static Dictionary<string, object?> RectDict(int l, int t, int r, int b) =>
        RectDict(new RECT { left = l, top = t, right = r, bottom = b });

    private static string UserObjectName(IntPtr handle)
    {
        if (handle == IntPtr.Zero) return "";
        var sb = new StringBuilder(256);
        if (GetUserObjectInformationW(handle, UoiName, sb, sb.Capacity * 2, out int needed) && needed > 0)
            return sb.ToString();
        return "";
    }

    // ---------------------------------------------------------------- 常量

    private const uint WM_PAINT = 0x000F, WM_TIMER = 0x0113, WM_ERASEBKGND = 0x0014,
                       WM_CLOSE = 0x0010, WM_DESTROY = 0x0002, WM_PRINTCLIENT = 0x0318;
    private const uint WS_OVERLAPPED = 0x00000000, WS_CAPTION = 0x00C00000, WS_SYSMENU = 0x00080000, WS_CLIPCHILDREN = 0x02000000;
    private const uint WS_EX_NOACTIVATE = 0x08000000, WS_EX_TOOLWINDOW = 0x00000080, WS_EX_CONTROLPARENT = 0x00010000;
    private const int SwShowNoActivate = 4;
    private const int GwlStyle = -16, GwlExStyle = -20;
    private const int UoiName = 2;
    private const int IdcArrow = 32512;
    private const int DefaultGuiFont = 17;
    private const uint DtLeft = 0x00000000, DtVCenter = 0x00000004, DtSingleLine = 0x00000020, DtNoPrefix = 0x00000800;
    private const uint BiRgb = 0, DibRgbColors = 0, PwClientOnly = 0x00000001;
    private const uint ClrInvalid = 0xFFFFFFFF;
    private const int Transparent = 1;
    private const int SmCxScreen = 0, SmCyScreen = 1, SmCMonitors = 80;
    private const uint SpiGetWorkArea = 0x0030;
    private const uint MonitorDefaultToNull = 0;

    // ---------------------------------------------------------------- P/Invoke

    private delegate IntPtr WndProcDelegate(IntPtr hWnd, uint msg, IntPtr wParam, IntPtr lParam);
    private delegate bool EnumWindowsProc(IntPtr hWnd, IntPtr lParam);

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct WNDCLASSEXW
    {
        public uint cbSize;
        public uint style;
        public IntPtr lpfnWndProc;
        public int cbClsExtra;
        public int cbWndExtra;
        public IntPtr hInstance;
        public IntPtr hIcon;
        public IntPtr hCursor;
        public IntPtr hbrBackground;
        [MarshalAs(UnmanagedType.LPWStr)] public string? lpszMenuName;
        [MarshalAs(UnmanagedType.LPWStr)] public string lpszClassName;
        public IntPtr hIconSm;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct RECT { public int left, top, right, bottom; }

    [StructLayout(LayoutKind.Sequential)]
    private struct POINT { public int x, y; }

    [StructLayout(LayoutKind.Sequential)]
    private struct MSG
    {
        public IntPtr hwnd;
        public uint message;
        public IntPtr wParam;
        public IntPtr lParam;
        public uint time;
        public POINT pt;
        public uint lPrivate;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct PAINTSTRUCT
    {
        public IntPtr hdc;
        public int fErase;
        public RECT rcPaint;
        public int fRestore;
        public int fIncUpdate;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 32)] public byte[] rgbReserved;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct BITMAPINFOHEADER
    {
        public uint biSize;
        public int biWidth;
        public int biHeight;
        public ushort biPlanes;
        public ushort biBitCount;
        public uint biCompression;
        public uint biSizeImage;
        public int biXPelsPerMeter;
        public int biYPelsPerMeter;
        public uint biClrUsed;
        public uint biClrImportant;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct MONITORINFO
    {
        public uint cbSize;
        public RECT rcMonitor;
        public RECT rcWork;
        public uint dwFlags;
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct RTL_OSVERSIONINFOEXW
    {
        public uint dwOSVersionInfoSize;
        public uint dwMajorVersion;
        public uint dwMinorVersion;
        public uint dwBuildNumber;
        public uint dwPlatformId;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string szCSDVersion;
    }

    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern ushort RegisterClassExW(ref WNDCLASSEXW lpwcx);

    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern IntPtr CreateWindowExW(uint dwExStyle, string lpClassName, string lpWindowName,
        uint dwStyle, int x, int y, int nWidth, int nHeight, IntPtr hWndParent, IntPtr hMenu, IntPtr hInstance, IntPtr lpParam);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern bool DestroyWindow(IntPtr hWnd);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern IntPtr DefWindowProcW(IntPtr hWnd, uint msg, IntPtr wParam, IntPtr lParam);

    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern int GetMessageW(out MSG lpMsg, IntPtr hWnd, uint wMsgFilterMin, uint wMsgFilterMax);

    [DllImport("user32.dll")]
    private static extern bool TranslateMessage(ref MSG lpMsg);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern IntPtr DispatchMessageW(ref MSG lpMsg);

    [DllImport("user32.dll")]
    private static extern void PostQuitMessage(int nExitCode);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern IntPtr SetTimer(IntPtr hWnd, int nIDEvent, uint uElapse, IntPtr lpTimerFunc);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern bool KillTimer(IntPtr hWnd, int uIDEvent);

    [DllImport("user32.dll")]
    private static extern bool ShowWindow(IntPtr hWnd, int nCmdShow);

    [DllImport("user32.dll")]
    private static extern bool UpdateWindow(IntPtr hWnd);

    [DllImport("user32.dll")]
    private static extern bool PrintWindow(IntPtr hwnd, IntPtr hdcBlt, uint nFlags);

    [DllImport("user32.dll")]
    private static extern bool IsWindowVisible(IntPtr hWnd);

    [DllImport("user32.dll")]
    private static extern bool IsWindow(IntPtr hWnd);

    [DllImport("user32.dll")]
    private static extern bool IsIconic(IntPtr hWnd);

    [DllImport("user32.dll")]
    private static extern bool IsZoomed(IntPtr hWnd);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern bool GetWindowRect(IntPtr hWnd, out RECT lpRect);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern bool GetClientRect(IntPtr hWnd, out RECT lpRect);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern bool AdjustWindowRectEx(ref RECT lpRect, uint dwStyle, bool bMenu, uint dwExStyle);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern IntPtr BeginPaint(IntPtr hWnd, out PAINTSTRUCT lpPaint);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern bool EndPaint(IntPtr hWnd, ref PAINTSTRUCT lpPaint);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern IntPtr GetDC(IntPtr hWnd);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern int ReleaseDC(IntPtr hWnd, IntPtr hDC);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern int FillRect(IntPtr hDC, ref RECT lprc, IntPtr hbr);

    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern int DrawTextW(IntPtr hdc, string lpchText, int cchText, ref RECT lprc, uint format);

    [DllImport("user32.dll")]
    private static extern IntPtr GetForegroundWindow();

    [DllImport("user32.dll")]
    private static extern IntPtr GetActiveWindow();

    [DllImport("user32.dll")]
    private static extern IntPtr GetDesktopWindow();

    [DllImport("user32.dll")]
    private static extern int GetSystemMetrics(int nIndex);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern IntPtr MonitorFromWindow(IntPtr hwnd, uint dwFlags);

    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern bool GetMonitorInfoW(IntPtr hMonitor, ref MONITORINFO lpmi);

    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern bool SystemParametersInfoW(uint uiAction, uint uiParam, ref RECT pvParam, uint fWinIni);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern int GetWindowLongW(IntPtr hWnd, int nIndex);

    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern IntPtr LoadCursorW(IntPtr hInstance, IntPtr lpCursorName);

    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern bool EnumWindows(EnumWindowsProc lpEnumFunc, IntPtr lParam);

    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern int GetClassNameW(IntPtr hWnd, StringBuilder lpClassName, int nMaxCount);

    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern int GetWindowTextW(IntPtr hWnd, StringBuilder lpString, int nMaxCount);

    // 必须显式 CharSet.Unicode：否则 StringBuilder 按 ANSI 封送，把 UTF-16 的 "WinSta0" 读成 "W"（实测踩过）。
    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern bool GetUserObjectInformationW(IntPtr hObj, int nIndex, StringBuilder pvInfo, int nLength, out int lpnLengthNeeded);

    [DllImport("user32.dll")]
    private static extern IntPtr GetProcessWindowStation();

    [DllImport("user32.dll")]
    private static extern IntPtr GetThreadDesktop(uint dwThreadId);

    [DllImport("user32.dll")]
    private static extern bool SetProcessDPIAware();

    [DllImport("gdi32.dll", SetLastError = true)]
    private static extern IntPtr CreateSolidBrush(uint colorref);

    [DllImport("gdi32.dll", SetLastError = true)]
    private static extern bool DeleteObject(IntPtr hObject);

    [DllImport("gdi32.dll", SetLastError = true)]
    private static extern IntPtr SelectObject(IntPtr hdc, IntPtr hObject);

    [DllImport("gdi32.dll", SetLastError = true)]
    private static extern IntPtr CreateCompatibleDC(IntPtr hdc);

    [DllImport("gdi32.dll", SetLastError = true)]
    private static extern IntPtr CreateCompatibleBitmap(IntPtr hdc, int cx, int cy);

    [DllImport("gdi32.dll", SetLastError = true)]
    private static extern bool DeleteDC(IntPtr hdc);

    [DllImport("gdi32.dll", SetLastError = true)]
    private static extern int GetDIBits(IntPtr hdc, IntPtr hbm, uint start, uint cLines, byte[] lpvBits, ref BITMAPINFOHEADER lpbmi, uint usage);

    [DllImport("gdi32.dll", SetLastError = true)]
    private static extern uint GetPixel(IntPtr hdc, int x, int y);

    [DllImport("gdi32.dll", SetLastError = true)]
    private static extern IntPtr GetStockObject(int i);

    [DllImport("gdi32.dll")]
    private static extern int SetBkMode(IntPtr hdc, int mode);

    [DllImport("gdi32.dll")]
    private static extern uint SetTextColor(IntPtr hdc, uint colorref);

    [DllImport("kernel32.dll")]
    private static extern IntPtr GetModuleHandleW(string? lpModuleName);

    [DllImport("kernel32.dll")]
    private static extern uint GetCurrentThreadId();

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool ProcessIdToSessionId(uint dwProcessId, out uint pSessionId);

    [DllImport("kernel32.dll")]
    private static extern uint WTSGetActiveConsoleSessionId();

    [DllImport("ntdll.dll", SetLastError = true)]
    private static extern int RtlGetVersion(ref RTL_OSVERSIONINFOEXW lpVersionInformation);

    [DllImport("dwmapi.dll", PreserveSig = true)]
    private static extern int DwmGetWindowAttribute(IntPtr hwnd, int dwAttribute, out int pvAttribute, int cbAttribute);

    [DllImport("dwmapi.dll", PreserveSig = true)]
    private static extern int DwmIsCompositionEnabled(out bool pfEnabled);

    // ---------------------------------------------------------------- 辅助 P/Invoke 包装

    private static class NativeMethods
    {
        internal static void SetProcessDPIAwareSafe()
        {
            try { SetProcessDPIAware(); } catch { /* 老系统或已设置 */ }
        }

        internal static void PumpPending()
        {
            // 只排空当前线程消息队列（不阻塞），用于 ready 前的补帧窗口
            while (PeekMessageW(out MSG msg, IntPtr.Zero, 0, 0, PmRemove))
            {
                TranslateMessage(ref msg);
                DispatchMessageW(ref msg);
            }
        }

        internal static string GetClassNameOf(IntPtr hwnd)
        {
            var sb = new StringBuilder(256);
            int n = GetClassNameW(hwnd, sb, sb.Capacity);
            return n > 0 ? sb.ToString() : "";
        }

        internal static string GetWindowTextOf(IntPtr hwnd)
        {
            var sb = new StringBuilder(512);
            int n = GetWindowTextW(hwnd, sb, sb.Capacity);
            return n > 0 ? sb.ToString() : "";
        }

        internal static (int Count, bool OursEnumerated) CountTopLevelWindows(IntPtr ours)
        {
            int count = 0;
            bool found = false;
            try
            {
                EnumWindows((h, _) =>
                {
                    if (IsWindowVisible(h)) count++;
                    if (h == ours) found = true;
                    return true;
                }, IntPtr.Zero);
            }
            catch { /* 枚举失败不影响主判定 */ }
            return (count, found);
        }

        internal static bool? DwmCloaked(IntPtr hwnd)
        {
            try
            {
                return DwmGetWindowAttribute(hwnd, DwmwaCloaked, out int cloaked, sizeof(int)) == 0
                    ? cloaked != 0
                    : null;
            }
            catch { return null; }
        }

        internal static bool? DwmCompositionEnabled()
        {
            try { return DwmIsCompositionEnabled(out bool on) == 0 ? on : null; }
            catch { return null; }
        }

        internal static uint? DpiForWindow(IntPtr hwnd)
        {
            try
            {
                uint dpi = GetDpiForWindow(hwnd);
                return dpi == 0 ? null : dpi;
            }
            catch { return null; }
        }

        internal static bool IsLocalDrivePath(string path)
        {
            if (path.Length < 3) return false;
            if (path.StartsWith(@"\\", StringComparison.Ordinal)) return false;
            return char.IsLetter(path[0]) && path[1] == ':' && (path[2] == '\\' || path[2] == '/');
        }

        private const uint PmRemove = 0x0001;
        private const int DwmwaCloaked = 14;

        [DllImport("user32.dll", CharSet = CharSet.Unicode)]
        private static extern bool PeekMessageW(out MSG lpMsg, IntPtr hWnd, uint wMsgFilterMin, uint wMsgFilterMax, uint wRemoveMsg);

        [DllImport("user32.dll")]
        private static extern uint GetDpiForWindow(IntPtr hwnd);
    }
}
