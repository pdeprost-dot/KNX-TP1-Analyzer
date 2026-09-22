using System;
using System.IO;
using KNXAnalyzer.Core;
using Avalonia.Controls.Primitives;
using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Platform.Storage;
using KNXAnalyzer.Desktop.ViewModels;

namespace KNXAnalyzer.Desktop.Views;

public partial class MainWindow : Window
{
    private bool _updatingSliders;
    public MainWindow()
    {
        InitializeComponent();
        DataContextChanged += (_, _) => AttachViewModel();
        Waveform.ViewportChanged += (_, range) => {
            if (DataContext is MainViewModel vm) vm.AnalogAxis = range;
            _updatingSliders = true;
            ZoomSlider.Value = Waveform.ZoomSliderValue;
            PositionSlider.Value = Waveform.PositionSliderValue;
            _updatingSliders = false;
        };
    }

    private void AttachViewModel()
    {
        if (DataContext is not MainViewModel vm) return;
        vm.PropertyChanged += (_, e) => {
            if (e.PropertyName is nameof(MainViewModel.SelectedCapture) or nameof(MainViewModel.SelectedSession))
            {
                var calibration = vm.SelectedSession is null ? null :
                    AnalogVoltagePipeline.FromSessionMetadata(vm.SelectedSession.Metadata).Calibration;
                var capture = vm.SelectedCapture;
                if (calibration is not null && capture is not null &&
                    (!calibration.TryEstimateMillivolts(capture.Minimum, out double _unused) ||
                     !calibration.TryEstimateMillivolts(capture.Maximum, out double _unused2))) calibration = null;
                Waveform.SetCalibration(calibration);
            }
        };
    }
    private void ZoomSliderChanged(object? sender, RangeBaseValueChangedEventArgs e)
    {
        if (!_updatingSliders && Waveform is not null) Waveform.SetZoomSlider(e.NewValue);
    }
    private void PositionSliderChanged(object? sender, RangeBaseValueChangedEventArgs e)
    {
        if (!_updatingSliders && Waveform is not null) Waveform.SetPositionSlider(e.NewValue);
    }
    private void AnalogUnitChanged(object? sender, SelectionChangedEventArgs e)
    {
        if (Waveform is not null && AnalogUnit is not null) Waveform.SetRawDisplay(AnalogUnit.SelectedIndex == 1);
    }
    private async void ReportClicked(object? sender, Avalonia.Interactivity.RoutedEventArgs e)
    {
        if (DataContext is not MainViewModel vm || vm.SelectedSession is null) return;
        var file = await StorageProvider.SaveFilePickerAsync(new FilePickerSaveOptions {
            Title = "Générer rapport", SuggestedFileName = $"rapport-{vm.SelectedSession.Id}.txt",
            FileTypeChoices = [new FilePickerFileType("Texte UTF-8") { Patterns = ["*.txt"] }]
        });
        if (file?.TryGetLocalPath() is string path) {
            try { EnsureOutsideSource(path, vm.SelectedSession); await File.WriteAllTextAsync(path, SessionAnalysis.ToFrenchReport(vm.SelectedSession)); vm.Status = $"Rapport créé : {path}"; }
            catch (Exception ex) { vm.Status = ex.Message; }
        }
    }
    private async void ExportJsonClicked(object? sender, Avalonia.Interactivity.RoutedEventArgs e)
    {
        if (DataContext is not MainViewModel vm || vm.SelectedSession is null) return;
        var file = await StorageProvider.SaveFilePickerAsync(new FilePickerSaveOptions {
            Title = "Exporter analysis.json", SuggestedFileName = "analysis.json",
            FileTypeChoices = [new FilePickerFileType("JSON") { Patterns = ["*.json"] }]
        });
        if (file?.TryGetLocalPath() is string path) {
            try { EnsureOutsideSource(path, vm.SelectedSession); await File.WriteAllTextAsync(path, SessionAnalysis.ToJson(vm.SelectedSession)); vm.Status = $"JSON créé : {path}"; }
            catch (Exception ex) { vm.Status = ex.Message; }
        }
    }
    private static void EnsureOutsideSource(string outputPath, Session session)
    {
        var source = Path.GetFullPath(session.DirectoryPath).TrimEnd(Path.DirectorySeparatorChar) + Path.DirectorySeparatorChar;
        var output = Path.GetFullPath(outputPath);
        if (output.StartsWith(source, StringComparison.OrdinalIgnoreCase)) throw new IOException("Choose an output path outside the SD session folder.");
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
