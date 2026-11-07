using System;
using Avalonia;
using Avalonia.Controls;
using PartyUp.Client.Localization;
using PartyUp.Client.Services;

namespace PartyUp.Client.Views;

/// <summary>
/// 设置页代码后置：只负责「关闭窗口时」下拉（绑定到 <see cref="AppSettings.CloseAction"/>）。
///
/// 为什么不放进 ShellViewModel：这一项是纯本机设置、没有运行时副作用，代码后置能少碰
/// 其它任务正在改的 ViewModel；所有文案仍走 <see cref="L"/>（resx），不硬编码。
/// </summary>
public partial class SettingsView : UserControl
{
    /// <summary>与 ItemsSource 一一对应的存储键。</summary>
    private static readonly string[] CloseActionKeys =
    [
        AppSettings.CloseActionAsk,
        AppSettings.CloseActionTray,
        AppSettings.CloseActionExit,
    ];

    private bool _syncing;

    public SettingsView()
    {
        InitializeComponent();
        CloseActionChoice.ItemsSource = new[]
        {
            L.Get("CloseAction_Ask"),
            L.Get("CloseAction_Tray"),
            L.Get("CloseAction_Exit"),
        };
        CloseActionChoice.SelectionChanged += (_, _) => OnCloseActionSelected();
        // 询问框里勾了「记住我的选择」后设置页已打开：重新可见时刷新成最新值。
        PropertyChanged += (_, e) =>
        {
            if (e.Property == IsVisibleProperty && IsVisible)
            {
                SyncFromSettings();
            }
        };
        SyncFromSettings();
    }

    private void SyncFromSettings()
    {
        var index = Array.IndexOf(CloseActionKeys, AppSettings.Instance.CloseAction);
        if (index < 0)
        {
            index = 0;
        }
        _syncing = true;
        try
        {
            CloseActionChoice.SelectedIndex = index;
        }
        finally
        {
            _syncing = false;
        }
    }

    private void OnCloseActionSelected()
    {
        if (_syncing)
        {
            return;
        }
        var index = CloseActionChoice.SelectedIndex;
        if (index < 0 || index >= CloseActionKeys.Length)
        {
            return;
        }
        var settings = AppSettings.Instance;
        settings.CloseAction = CloseActionKeys[index];
        settings.Save();
    }
}
