using System;
using System.Collections.ObjectModel;
using System.Linq;
using System.Text.Json;
using CommunityToolkit.Mvvm.ComponentModel;
using KNXAnalyzer.Core;

namespace KNXAnalyzer.Desktop.ViewModels;

public partial class MainViewModel : ViewModelBase
{
    public ObservableCollection<Session> Sessions { get; } = [];
    public ObservableCollection<Tp1Candidate> VisibleCandidates { get; } = [];
    public ObservableCollection<AnalogEvent> AnalogEvents { get; } = [];
    public string[] Filters { get; } = ["All", "Valid", "Errors", "ACK", "Unknown"];
    [ObservableProperty] private string folderPath = "";
    [ObservableProperty] private string status = "Open an SD root, sessions folder, or one session folder.";
    [ObservableProperty] private Session? selectedSession;
    [ObservableProperty] private Tp1Candidate? selectedCandidate;
    [ObservableProperty] private AnalogEvent? selectedAnalogEvent;
    [ObservableProperty] private string selectedFilter = "All";
    [ObservableProperty] private string summary = "No session selected";
    [ObservableProperty] private string details = "Select a TP1 candidate.";
    [ObservableProperty] private string analogDetails = "Select an analog event.";
    [ObservableProperty] private RawCapture? selectedCapture;

    public void OpenFolder(string path)
    {
        try {
            Sessions.Clear();
            foreach (var session in SessionReader.OpenFolder(path)) Sessions.Add(session);
            FolderPath = path; SelectedSession = Sessions.FirstOrDefault();
            Status = $"{Sessions.Count} session(s) found. Original files are read only.";
        } catch (Exception e) { Status = e.Message; }
    }
    partial void OnSelectedSessionChanged(Session? value)
    {
        VisibleCandidates.Clear(); AnalogEvents.Clear(); SelectedCandidate = null; SelectedAnalogEvent = null;
        if (value is null) { Summary = "No session selected"; return; }
        foreach (var item in value.AnalogEvents) AnalogEvents.Add(item);
        Summary = $"{value.Id}   {value.State}   Date: {value.DateTime ?? "unavailable (device clock unset)"}   Duration: {(value.DurationMs is long ms ? $"{ms / 1000.0:F1} s" : "unknown")}\n" +
            $"Candidates: {value.Candidates.Count}   Valid: {value.Count("VALID_KNOWN") + value.Count("VALID_UNKNOWN")}   ACK: {value.Candidates.Count(x => x.Ack == "ACK")}   Parity: {value.Count("INVALID_PARITY")}   Checksum: {value.Count("INVALID_CHECKSUM")}   Timing: {value.Count("INVALID_TIMING")}   Incomplete: {value.Count("INCOMPLETE")}   Analog: {value.AnalogEvents.Count}   Warnings: {value.Diagnostics.Count}";
        RefreshFilter();
        Status = value.Diagnostics.Count == 0 ? "Session loaded." : string.Join(" | ", value.Diagnostics.Take(3).Select(x => $"{System.IO.Path.GetFileName(x.File)}:{x.Line} {x.Message}"));
    }
    partial void OnSelectedFilterChanged(string value) => RefreshFilter();
    private void RefreshFilter()
    {
        VisibleCandidates.Clear();
        if (SelectedSession is null) return;
        foreach (var item in SelectedSession.Candidates.Where(x => SelectedFilter switch {
            "Valid" => x.IsValid, "Errors" => x.IsError, "ACK" => x.IsAck,
            "Unknown" => x.Classification is "VALID_UNKNOWN" or "ANALOG_UNDECODED" || !Known(x.Classification), _ => true
        })) VisibleCandidates.Add(item);
    }
    private static bool Known(string value) => value is "VALID_KNOWN" or "VALID_UNKNOWN" or "INVALID_PARITY" or "INVALID_TIMING" or "INVALID_CHECKSUM" or "INCOMPLETE" or "ANALOG_UNDECODED";
    partial void OnSelectedCandidateChanged(Tp1Candidate? value) => Details = value is null ? "Select a TP1 candidate." :
        $"Line {value.Line}   {value.Classification}\nTime: {value.Time}\nSource: {value.Source ?? "—"}   Destination: {value.Destination ?? "—"} ({value.DestinationType ?? "—"})\n" +
        $"Hop count: {value.HopCount?.ToString() ?? "—"}   TP length: {value.TpLength?.ToString() ?? "—"}   Checksum: {value.Checksum}   ACK: {value.Ack}\n" +
        $"Parity errors: {value.ParityErrors}   Timing errors: {value.TimingErrors}   Overflow: {value.Overflow}\nRaw bytes: {string.Join(" ", value.RawBytes.Select(x => x.ToString("X2")))}\n\nOriginal JSON:\n{JsonSerializer.Serialize(value.Original, new JsonSerializerOptions { WriteIndented = true })}";
    partial void OnSelectedAnalogEventChanged(AnalogEvent? value)
    {
        SelectedCapture = null;
        if (value is null || SelectedSession is null) { AnalogDetails = "Select an analog event."; return; }
        AnalogDetails = JsonSerializer.Serialize(value.Original, new JsonSerializerOptions { WriteIndented = true });
        if (!value.RawPersisted) return;
        var path = System.IO.Path.Combine(SelectedSession.DirectoryPath, "captures", $"event-{value.EventId:D6}.bin");
        try {
            var raw = RawCapture.Read(path);
            SelectedCapture = raw;
            AnalogDetails = $"RAW: {raw.Samples.Length} samples, {raw.SampleRateHz} Hz, min/max {raw.Minimum}/{raw.Maximum}, trigger index {raw.TriggerIndex}, CRC {(raw.CrcValid ? "OK" : "INVALID")}\n\n" + AnalogDetails;
        } catch (Exception e) { AnalogDetails = $"RAW unavailable: {e.Message}\n\n" + AnalogDetails; }
    }
}
