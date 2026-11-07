using Avalonia.Controls;

namespace PartyUp.Client.Views;

/// <summary>
/// 服务器管理视图。只有 XAML 装配，没有业务逻辑：
/// DataContext 由宿主注入 <see cref="ViewModels.AdminViewModel"/>，可见性由宿主控制。
/// </summary>
public partial class AdminView : UserControl
{
    public AdminView() => InitializeComponent();
}
