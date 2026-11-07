using Avalonia.Controls;
using Avalonia.Interactivity;

namespace PartyUp.Client.Views;

/// <summary>
/// 点窗口关闭时的询问框：最小化到托盘（默认） / 退出 PartyUp / 记住我的选择。
///
/// 结果用字符串回传（<c>tray</c> / <c>exit</c>），关闭窗口或按 Esc 得到 null = 取消本次关闭。
/// 用字符串而不是 enum：<see cref="Window.ShowDialog{TResult}"/> 在窗口被直接关闭时返回 default，
/// enum 的 default 会变成"用户选了托盘"，与取消无法区分。
/// </summary>
public partial class CloseToTrayDialog : Window
{
    public const string ResultTray = "tray";
    public const string ResultExit = "exit";

    public CloseToTrayDialog()
    {
        InitializeComponent();
    }

    /// <summary>「记住我的选择，以后不再询问」是否勾选。</summary>
    public bool RememberChoice => RememberCheck.IsChecked == true;

    private void OnTrayClick(object? sender, RoutedEventArgs e) => Close(ResultTray);

    private void OnExitClick(object? sender, RoutedEventArgs e) => Close(ResultExit);
}
