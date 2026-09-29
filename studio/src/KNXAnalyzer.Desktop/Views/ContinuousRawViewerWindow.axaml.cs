using System;
using System.Diagnostics;
using System.Threading;
using System.Threading.Tasks;
using Avalonia.Controls;
using KNXAnalyzer.Core;

namespace KNXAnalyzer.Desktop.Views;

public partial class ContinuousRawViewerWindow : Window
{
    private RawCorpusEntry entry = null!;
    private RawCorpusContinuousReader reader = null!;
    private CancellationTokenSource? loadCancellation;
    private long loadVersion;
    private ulong lastCursor = ulong.MaxValue;

    public ContinuousRawViewerWindow() => InitializeComponent();

    public ContinuousRawViewerWindow(RawCorpusEntry entry) : this()
    {
        this.entry = entry; reader = new(entry);
        TitleText.Text = $"Continuous RAW · {entry.SessionId}";
        SummaryText.Text = $"{entry.SampleCount:N0} samples · {reader.SampleRateHz:F6} Hz · {entry.RawBytes:N0} bytes · Integrity {entry.Integrity}";
        Plot.ViewRangeRequested = RequestView;
        Plot.CursorRequested = CursorRequested;
        Plot.SelectionRequested = SelectionRequested;
        Opened += (_, _) => Plot.Configure(entry.SampleCount, reader.SampleRateHz);
        Closed += (_, _) => loadCancellation?.Cancel();
    }

    private void RequestView(ulong start, ulong end)
    {
        var version = Interlocked.Increment(ref loadVersion); loadCancellation?.Cancel(); loadCancellation = new(); var token = loadCancellation.Token;
        ViewportText.Text = $"Loading {FormatTime(start)} – {FormatTime(end)} · {end - start:N0} samples";
        _ = Task.Run(() => reader.ReadView(start, end, cancellationToken: token), token).ContinueWith(task => {
            if (task.IsCanceled || task.IsFaulted || token.IsCancellationRequested || version != loadVersion) return;
            Avalonia.Threading.Dispatcher.UIThread.Post(() => { if (version != loadVersion) return; Plot.ViewData = task.Result; ViewportText.Text = $"Visible {FormatTime(start)} – {FormatTime(end)} · {end - start:N0} samples · {(task.Result.ExactSamples is null ? "streaming min/max" : "exact samples")}"; });
        }, CancellationToken.None);
    }

    private void CursorRequested(ulong sample)
    {
        if (sample == lastCursor) return; lastCursor = sample;
        try { var adc = reader.ReadSample(sample); CursorText.Text = $"Cursor {FormatTime(sample)} · sample {sample:N0} · ADC {adc}"; }
        catch (Exception ex) { CursorText.Text = ex.Message; }
    }

    private void SelectionRequested(ulong start, ulong end)
    {
        SelectionText.Text = "Selection: calculating streaming statistics…";
        _ = Task.Run(() => reader.ReadSelectionStats(start, end)).ContinueWith(task => {
            if (task.IsFaulted) { Avalonia.Threading.Dispatcher.UIThread.Post(() => SelectionText.Text = task.Exception?.GetBaseException().Message); return; }
            var s = task.Result; Avalonia.Threading.Dispatcher.UIThread.Post(() => SelectionText.Text = $"Selection {FormatTime(s.StartSample)} – {FormatTime(s.EndSample)} · count {s.Count:N0} · min {s.Minimum} · max {s.Maximum} · mean {s.Mean:F3} · P-P {s.PeakToPeak}");
        });
    }

    private string FormatTime(ulong sample) => $"{sample / reader.SampleRateHz:F6} s";
    private void FitClicked(object? sender, Avalonia.Interactivity.RoutedEventArgs e) => Plot.FitAll();
    private void OpenFolderClicked(object? sender, Avalonia.Interactivity.RoutedEventArgs e) => Process.Start(new ProcessStartInfo("explorer.exe", $"\"{entry.DataDirectory}\"") { UseShellExecute = true });
}
