// Windows 调试桥 · 单一入口
//
//   wdb worker --queue <Q> --runs-root <R>    → Windows 前台 Worker（由 start-worker.ps1 启动）
//   wdb <其它子命令>                           → WSL 侧 CLI
//
// argv 全程不经过 shell：所有进程启动都使用 ProcessStartInfo.ArgumentList。
using System.Text;
using GalleriesManager.Wdb;

try
{
    // Windows 控制台默认代码页会破坏中文日志；能设就设，失败不影响功能
    Console.OutputEncoding = new UTF8Encoding(encoderShouldEmitUTF8Identifier: false);
}
catch (IOException) { }
catch (PlatformNotSupportedException) { }

if (args.Length > 0 && string.Equals(args[0], "worker", StringComparison.OrdinalIgnoreCase))
{
    return await Worker.RunAsync(args.Skip(1).ToArray());
}

return await Cli.RunAsync(args);
