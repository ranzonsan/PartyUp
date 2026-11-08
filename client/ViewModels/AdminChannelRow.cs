using PartyUp.Client.Models;
using System;

namespace PartyUp.Client.ViewModels;

/// <summary>
/// 服务器管理页「频道」列表的一行：把 DTO 的原始字段转成可直接绑定的显示文案。
/// 原始描述单独保留（<see cref="Description"/>），改名/描述编辑框要用它，
/// 不能用显示文案（空描述会显示「（无描述）」）。
/// </summary>
public sealed class AdminChannelRow
{
    public AdminChannelRow(AdminChannelDto dto)
    {
        Id = dto.Id;
        Name = dto.Name;
        Description = dto.Description ?? string.Empty;
        DescriptionCaption = Description.Length == 0 ? "（无描述）" : Description;
        CreatedCaption = DateTimeOffset.FromUnixTimeMilliseconds(dto.CreatedAt)
            .ToLocalTime().ToString("yyyy-MM-dd HH:mm");
    }

    public string Id { get; }

    public string Name { get; }

    public string Description { get; }

    public string DescriptionCaption { get; }

    public string CreatedCaption { get; }
}
