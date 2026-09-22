using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Platform.Storage;
using KNXAnalyzer.Desktop.ViewModels;

namespace KNXAnalyzer.Desktop.Views;

public partial class MainWindow : Window
{
    public MainWindow()
    {
        InitializeComponent();
        Waveform.ViewportChanged += (_, range) => {
            if (DataContext is MainViewModel vm) vm.AnalogAxis = range;
        };
    }

    private void ResetWaveformClicked(object? sender, Avalonia.Interactivity.RoutedEventArgs e) => Waveform.ResetFit();

    private async void OpenClicked(object? sender, Avalonia.Interactivity.RoutedEventArgs e)
    {
        var folders = await StorageProvider.OpenFolderPickerAsync(new Avalonia.Platform.Storage.FolderPickerOpenOptions { Title = "Open SD / Sessions Folder", AllowMultiple = false });
        if (folders.Count > 0 && DataContext is MainViewModel vm && folders[0].TryGetLocalPath() is string path) vm.OpenFolder(path);
    }

    private void PathKeyDown(object? sender, KeyEventArgs e)
    {
        if (e.Key == Key.Enter && DataContext is MainViewModel vm) vm.OpenFolder(vm.FolderPath);
    }
}
