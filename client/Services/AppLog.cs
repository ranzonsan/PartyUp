namespace PartyUp.Client.Services;

/// <summary>
/// 控制台日志。每行带 "HH:mm:ss.fff LEVEL " 前缀，wdb 把 stdout 原样落进日志文件，
/// 一键脚本用 grep 断言 PASS/FAIL 行即可，不需要读日志格式之外的任何东西。
/// </summary>
internal static class AppLog
{
    private static readonly object Gate = new();

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
