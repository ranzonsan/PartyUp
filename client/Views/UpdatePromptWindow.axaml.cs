using Avalonia.Controls;
using Avalonia.Interactivity;

namespace PartyUp.Client.Views;

/// <summary>
/// 发现新版本时的询问框：只有用户点「立即更新」才会下载（用户要求：确认后才下载）。
///
/// 下载在 <see cref="ViewModels.ShellViewModel.DownloadUpdateCommand"/> 里继续跑，关掉本窗口
/// 不影响进度；状态文字同时写在设置页的「更新」区块，便于之后查看。
/// </summary>
public partial class UpdatePromptWindow : Window
{
    public UpdatePromptWindow()
    {
        InitializeComponent();
    }

    private void OnLaterClick(object? sender, RoutedEventArgs e) => Close();

    private void OnNowClick(object? sender, RoutedEventArgs e)
    {
        if (DataContext is ViewModels.ShellViewModel vm)
        {
            vm.DownloadUpdateCommand.Execute(null);
        }
        Close();
    }
}
