namespace PartyUp.Client.Services;

/// <summary>
/// 控制台日志。每行带 "HH:mm:ss.fff LEVEL " 前缀，wdb 把 stdout 原样落进日志文件，
/// 一键脚本用 grep 断言 PASS/FAIL 行即可，不需要读日志格式之外的任何东西。
/// </summary>
internal static class AppLog
{
    private static readonly object Gate = new();

    /// <summary>
    /// 强制 stdout 走 UTF-8：Windows 默认控制台代码页是 cp936，
    /// wdb 把 stdout 原样落盘后，WSL 侧按 UTF-8 读会把中文日志读成乱码（验收排查踩过）。
    /// 重定向场景下这里只影响 StreamWriter 的编码，不改控制台窗口代码页。
    /// </summary>
    static AppLog()
    {
        try
        {
            Console.OutputEncoding = new System.Text.UTF8Encoding(encoderShouldEmitUTF8Identifier: false);
        }
        catch (Exception)
        {
            // 没有控制台（比如被宿主重定向到管道）时不要因为设置编码把进程带崩。
        }
    }

    public static void Info(string message) => Write("INFO", message);

    public static void Warn(string message) => Write("WARN", message);

    public static void Pass(string message) => Write("PASS", message);

    public static void Fail(string message) => Write("FAIL", message);

    private static void Write(string level, string message)
    {
        lock (Gate)
        {
            Console.WriteLine($"{DateTime.Now:HH:mm:ss.fff} {level} {message}");
            Console.Out.Flush();
        }
    }
}
