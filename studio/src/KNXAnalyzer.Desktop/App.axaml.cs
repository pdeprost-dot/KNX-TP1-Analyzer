using System.Linq;
using Avalonia;
using Avalonia.Controls.ApplicationLifetimes;
using Avalonia.Markup.Xaml;
using KNXAnalyzer.Desktop.ViewModels;
using KNXAnalyzer.Desktop.Views;

namespace KNXAnalyzer.Desktop;

public partial class App : Application
{
    public override void Initialize()
    {
        AvaloniaXamlLoader.Load(this);
    }

    public override void OnFrameworkInitializationCompleted()
    {
        if (ApplicationLifetime is IClassicDesktopStyleApplicationLifetime desktop)
        {
            var vm = new MainViewModel();
            if (desktop.Args is { Length: > 0 }) vm.OpenFolder(desktop.Args[0]);
            if (desktop.Args?.Contains("--top-capture") == true) vm.SelectLargestCapture();
            if (desktop.Args?.Contains("--interactions") == true) vm.SelectedTabIndex = 4;
            desktop.MainWindow = new MainWindow
            {
                DataContext = vm,
            };
        }

        base.OnFrameworkInitializationCompleted();
    }
}
