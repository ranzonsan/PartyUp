using System;
using System.Collections.Generic;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;

namespace PartyUp.Client.Services;

/// <summary>
/// 登录凭据保险箱：「记住我」存下来的 token 用 **Windows DPAPI** 按当前用户加密，
/// 落盘的是密文，明文 token 不写进任何 JSON。
///
/// 为什么不用「加密库 + 自管密钥」：自管密钥最终还是要明文存放或依赖某个 keystore，
/// 在单机 Windows 客户端上这不比 DPAPI 更安全，只是更复杂。DPAPI 的密钥由系统按
/// 用户 SID 派生，换用户/换机器解不开——这正是「本机记住我」想要的边界。
///
/// 非 Windows 平台明确降级：不保存、返回失败原因，由 UI 如实提示（不假装记住）。
/// </summary>
public static class TokenVault
{
    private const string FolderName = "PartyUp";
    private const string FileName = "tokens.json";

    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        WriteIndented = true,
        PropertyNameCaseInsensitive = true,
    };

    /// <summary>键 = 规范化服务器 URL + "|" + 用户名；值 = DPAPI 密文的 base64。</summary>
    private sealed class VaultData
    {
        public Dictionary<string, string> Entries { get; set; } = new();
    }

    public static string FolderPath => AppPaths.Root;

    public static string FilePath => Path.Combine(FolderPath, FileName);

    public static bool IsSupported => OperatingSystem.IsWindows();

    public static bool TrySave(Uri baseUri, string username, string token, out string? error)
    {
        error = null;
        if (!IsSupported)
        {
            error = "当前平台不支持「记住我」（需要 Windows DPAPI）。";
            return false;
        }
        if (string.IsNullOrWhiteSpace(token) || string.IsNullOrWhiteSpace(username))
        {
            error = "token 或用户名为空。";
            return false;
        }

        try
        {
            var encrypted = Protect(Encoding.UTF8.GetBytes(token));
            var data = LoadData();
            data.Entries[KeyOf(baseUri, username)] = Convert.ToBase64String(encrypted);
            SaveData(data);
            AppLog.Info($"vault: token saved for {username} @ {baseUri.Host}");
            return true;
        }
        catch (Exception ex)
        {
            error = ex.Message;
            AppLog.Warn("vault: save failed: " + ex.Message);
            return false;
        }
    }

    public static string? TryLoad(Uri baseUri, string username, out string? error)
    {
        error = null;
        if (!IsSupported)
        {
            error = "当前平台不支持「记住我」（需要 Windows DPAPI）。";
            return null;
        }

        try
        {
            var data = LoadData();
            if (!data.Entries.TryGetValue(KeyOf(baseUri, username), out var blob))
            {
                return null;
            }
            var plain = Unprotect(Convert.FromBase64String(blob));
            return Encoding.UTF8.GetString(plain);
        }
        catch (Exception ex)
        {
            // 解不开（换机器/换用户/文件被改）就当成没有：并清掉这条坏记录，避免每次启动都失败。
            error = ex.Message;
            AppLog.Warn("vault: load failed: " + ex.Message);
            Forget(baseUri, username);
            return null;
        }
    }

    public static void Forget(Uri baseUri, string username)
    {
        var data = LoadData();
        if (data.Entries.Remove(KeyOf(baseUri, username)))
        {
            SaveData(data);
        }
    }

    private static string KeyOf(Uri baseUri, string username) =>
        ServerEndpoint.NormalizeBase(baseUri).ToString().TrimEnd('/') + "|" + username;

    private static VaultData LoadData()
    {
        try
        {
            if (!File.Exists(FilePath))
            {
                return new VaultData();
            }
            return JsonSerializer.Deserialize<VaultData>(File.ReadAllText(FilePath), JsonOptions)
                   ?? new VaultData();
        }
        catch (Exception ex)
        {
            AppLog.Warn("vault: read failed, starting empty: " + ex.Message);
            return new VaultData();
        }
    }

    private static void SaveData(VaultData data)
    {
        Directory.CreateDirectory(FolderPath);
        var temp = FilePath + ".tmp";
        File.WriteAllText(temp, JsonSerializer.Serialize(data, JsonOptions));
        File.Move(temp, FilePath, overwrite: true);
    }

    // ---------------- DPAPI ----------------

    private static byte[] Protect(byte[] plain)
    {
        var inBlob = new DataBlob(plain);
        var outBlob = new DataBlob();
        try
        {
            // CRYPTPROTECT_UI_FORBIDDEN(0x1)：绝不弹系统对话框（后台静默失败即可）。
            if (!CryptProtectData(ref inBlob, "PartyUp token", IntPtr.Zero, IntPtr.Zero, IntPtr.Zero, 0x1, ref outBlob))
            {
                throw new InvalidOperationException("CryptProtectData 失败，错误码 " + Marshal.GetLastWin32Error());
            }
            return outBlob.ToArray();
        }
        finally
        {
            inBlob.Free();
            outBlob.Free();
        }
    }

    private static byte[] Unprotect(byte[] cipher)
    {
        var inBlob = new DataBlob(cipher);
        var outBlob = new DataBlob();
        try
        {
            if (!CryptUnprotectData(ref inBlob, IntPtr.Zero, IntPtr.Zero, IntPtr.Zero, IntPtr.Zero, 0x1, ref outBlob))
            {
                throw new InvalidOperationException("CryptUnprotectData 失败，错误码 " + Marshal.GetLastWin32Error());
            }
            return outBlob.ToArray();
        }
        finally
        {
            inBlob.Free();
            outBlob.Free();
        }
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct DataBlob
    {
        public int Size;
        public IntPtr Data;

        public DataBlob(byte[] bytes)
        {
            Size = bytes.Length;
            Data = Marshal.AllocHGlobal(bytes.Length);
            Marshal.Copy(bytes, 0, Data, bytes.Length);
        }

        public byte[] ToArray()
        {
            if (Size <= 0 || Data == IntPtr.Zero)
            {
                return Array.Empty<byte>();
            }
            var result = new byte[Size];
            Marshal.Copy(Data, result, 0, Size);
            return result;
        }

        public void Free()
        {
            if (Data != IntPtr.Zero)
            {
                Marshal.FreeHGlobal(Data);
                Data = IntPtr.Zero;
                Size = 0;
            }
        }
    }

    [DllImport("crypt32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    private static extern bool CryptProtectData(
        ref DataBlob dataIn, string? description, IntPtr entropy, IntPtr reserved,
        IntPtr prompt, int flags, ref DataBlob dataOut);

    [DllImport("crypt32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    private static extern bool CryptUnprotectData(
        ref DataBlob dataIn, IntPtr description, IntPtr entropy, IntPtr reserved,
        IntPtr prompt, int flags, ref DataBlob dataOut);
}
