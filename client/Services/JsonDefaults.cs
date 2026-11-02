using System.Text.Json;
using System.Text.Json.Serialization;

namespace PartyUp.Client.Services;

/// <summary>
/// 全局 JSON 选项。协议字段是 snake_case（docs/PROTOCOL.md 示例：
/// <c>{"channel_id": "...", "client_msg_id": "..."}</c>），C# 侧用 PascalCase，
/// 由 <see cref="JsonNamingPolicy.SnakeCaseLower"/> 自动转换，不手写字段名映射。
/// </summary>
public static class JsonDefaults
{
    public static readonly JsonSerializerOptions Options = Create();

    private static JsonSerializerOptions Create()
    {
        var options = new JsonSerializerOptions
        {
            PropertyNamingPolicy = JsonNamingPolicy.SnakeCaseLower,
            PropertyNameCaseInsensitive = true,
            DefaultIgnoreCondition = JsonIgnoreCondition.WhenWritingNull,
            NumberHandling = JsonNumberHandling.AllowReadingFromString,
        };
        return options;
    }
}
