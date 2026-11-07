using Avalonia.Controls;

namespace PartyUp.Client.Views;

/// <summary>
/// 分步注册视图。只有 XAML 装配，没有业务逻辑：
/// DataContext 由宿主注入 <see cref="ViewModels.RegistrationViewModel"/>，可见性由宿主控制。
/// </summary>
public partial class RegistrationView : UserControl
{
    /// <summary>Avalonia XAML 加载器要求的无参构造（设计器/热重载也用它）。</summary>
    public RegistrationView() => InitializeComponent();
}
