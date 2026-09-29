using System;
using System.Diagnostics;
using Avalonia.Controls;
using Avalonia.Platform.Storage;
using KNXAnalyzer.Desktop.ViewModels;

namespace KNXAnalyzer.Desktop.Views;

public partial class RawCorpusWindow : Window
{
    public RawCorpusWindow() { InitializeComponent(); DataContext = new RawCorpusViewModel(); }

    private async void ImportClicked(object? sender, Avalonia.Interactivity.RoutedEventArgs e)
    {
        var folders = await StorageProvider.OpenFolderPickerAsync(new FolderPickerOpenOptions { Title = "Select completed CONTINUOUS_RAW session folder", AllowMultiple = false });
        if (folders.Count > 0 && folders[0].TryGetLocalPath() is string path && DataContext is RawCorpusViewModel vm) await vm.ImportAsync(path);
    }
    private void SaveMetadataClicked(object? sender, Avalonia.Interactivity.RoutedEventArgs e) => (DataContext as RawCorpusViewModel)?.SaveLocalMetadata();
    private void OpenSelectedClicked(object? sender, Avalonia.Interactivity.RoutedEventArgs e)
    {
        if (DataContext is not RawCorpusViewModel { SelectedEntry: { } selected } vm) return;
        try { new ContinuousRawViewerWindow(selected.Entry).Show(); vm.Status = "Continuous RAW Viewer opened in read-only, bounded-memory mode."; }
        catch (Exception ex) { vm.Status = ex.Message; }
    }
    private void OpenFolderClicked(object? sender, Avalonia.Interactivity.RoutedEventArgs e)
    {
        if (DataContext is not RawCorpusViewModel { SelectedEntry: { } selected } vm) return;
        try { Process.Start(new ProcessStartInfo("explorer.exe", $"\"{selected.Entry.DataDirectory}\"") { UseShellExecute = true }); vm.Status = "Opened managed corpus folder."; }
        catch (Exception ex) { vm.Status = ex.Message; }
    }
}
