namespace Wdb.Tests;

/// <summary>
/// Harness 自证：一个永远"全绿"的测试框架等于没有测试。
/// 这里证明断言真的会失败、发现真的能找到测试、现场真的会隔离与清理。
/// </summary>
public static class HarnessSelfTests
{
    [WdbFact("harness.assert-failure-is-real")]
    public static void AssertFailureIsReal()
    {
        // 阴性对照：不相等必须抛 WdbAssertException，而不是被吞掉。
        var ex = WdbAssert.Throws<WdbAssertException>(
            () => WdbAssert.Equal("a", "b"),
            "不相等却未失败 = 假绿");
        WdbAssert.Contains("值不相等", ex.Message);

        // 阳性对照：相等不得抛。
        WdbAssert.Equal("a", "a");
    }

    [WdbFact("harness.invisible-difference-is-caught")]
    public static void InvisibleDifferenceIsCaught()
    {
        // 阴性对照：视觉上完全一样的字符串（尾随 NBSP / CRLF）必须被判不等。
        WdbAssert.Throws<WdbAssertException>(() => WdbAssert.Equal("abc", "abc\u00A0"));
        WdbAssert.Throws<WdbAssertException>(() => WdbAssert.Equal("a\n", "a\r\n"));
    }

    [WdbFact("harness.discovery-finds-tests")]
    public static void DiscoveryFindsTests()
    {
        var ids = TestRunner.Discover().Select(TestRunner.IdOf).ToList();
        WdbAssert.True(ids.Count > 0, "发现结果为 0 = 反射发现坏了");
        WdbAssert.Contains("harness.assert-failure-is-real", string.Join('\n', ids));
    }

    [WdbFact("harness.temp-space-is-isolated-and-cleaned")]
    public static void TempSpaceIsIsolatedAndCleaned()
    {
        string path;
        using (var space = TestSpace.Create("self-check"))
        {
            path = space.Root;
            WdbAssert.True(Directory.Exists(path), "现场目录未创建");
            space.WriteFile("queue/jobs/x/request.json", "{}");
            WdbAssert.True(File.Exists(space.PathOf("queue", "jobs", "x", "request.json")), "写入未落盘");
            WdbAssert.StartsWith(TestSpace.TmpRoot, path, "现场必须落在测试临时根内（workspace 可写区）");
        }

        if (!TestSpace.Keep)
        {
            WdbAssert.False(Directory.Exists(path), "Dispose 后现场未清理");
        }
    }
}
