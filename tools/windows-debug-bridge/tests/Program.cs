namespace Wdb.Tests;

/// <summary>
/// 离线自运行入口（零 NuGet）：
///   dotnet run --project tools/windows-debug-bridge/tests/Wdb.Tests.csproj -- [--filter 子串] [--list] [--verbose]
/// 退出码 0 = 全绿，1 = 有失败。
/// </summary>
internal static class Program
{
    private static int Main(string[] args)
    {
        try
        {
            return TestRunner.Run(args);
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine($"[harness-fatal] {ex.GetType().Name}: {ex.Message}");
            Console.Error.WriteLine(ex.StackTrace);
            return 2;
        }
    }
}
