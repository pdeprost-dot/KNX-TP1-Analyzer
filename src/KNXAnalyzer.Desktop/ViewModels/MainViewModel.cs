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
    public string[] AnalogSortOptions { get; } = ["P-P descending", "P-P ascending", "Event ID"];
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
    [ObservableProperty] private string selectedAnalogSort = "P-P descending";
    [ObservableProperty] private int selectedTabIndex;
    [ObservableProperty] private string analogVariationNotice = "";
    [ObservableProperty] private string analogAxis = "Time axis unavailable";

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
        RefreshAnalogSort();
        Summary = $"{value.Id}   {value.State}   {value.Generation}   Date: {value.DateTime ?? "unavailable (device clock unset)"}   Duration: {(value.DurationMs is long ms ? $"{ms / 1000.0:F1} s" : "unknown")}\n" +
            $"Candidates: {value.Candidates.Count}   Valid: {value.Count("VALID_KNOWN") + value.Count("VALID_UNKNOWN")}   ACK: {value.Candidates.Count(x => x.Ack == "ACK")}   Parity: {value.Count("INVALID_PARITY")}   Checksum: {value.Count("INVALID_CHECKSUM")}   Timing: {value.Count("INVALID_TIMING")}   Incomplete: {value.Count("INCOMPLETE")}   Analog: {value.AnalogEvents.Count}   Warnings: {value.Diagnostics.Count}";
        RefreshFilter();
        Status = value.Diagnostics.Count == 0 ? "Session loaded." : string.Join(" | ", value.Diagnostics.Take(3).Select(x => $"{System.IO.Path.GetFileName(x.File)}:{x.Line} {x.Message}"));
    }
    partial void OnSelectedFilterChanged(string value) => RefreshFilter();
    partial void OnSelectedAnalogSortChanged(string value) => RefreshAnalogSort();
    private void RefreshAnalogSort()
    {
        var selected = SelectedAnalogEvent;
        AnalogEvents.Clear();
        if (SelectedSession is null) return;
        var ordered = SelectedAnalogSort switch {
            "P-P ascending" => SelectedSession.AnalogEvents.OrderBy(x => x.CaptureSummary?.PeakToPeak ?? int.MaxValue).ThenBy(x => x.EventId),
            "Event ID" => SelectedSession.AnalogEvents.OrderBy(x => x.EventId),
            _ => SelectedSession.AnalogEvents.OrderByDescending(x => x.CaptureSummary?.PeakToPeak ?? -1).ThenBy(x => x.EventId)
        };
        foreach (var item in ordered) AnalogEvents.Add(item);
        if (selected is not null && AnalogEvents.Contains(selected)) SelectedAnalogEvent = selected;
    }
    public void SelectLargestCapture()
    {
        var winner = Sessions.SelectMany(s => s.AnalogEvents.Where(e => e.CaptureSummary is { CrcValid: true })
            .Select(e => (Session: s, Event: e)))
            .OrderByDescending(x => x.Event.CaptureSummary!.PeakToPeak).FirstOrDefault();
        if (winner.Event is null) return;
        SelectedSession = winner.Session;
        SelectedAnalogSort = "P-P descending";
        SelectedAnalogEvent = winner.Event;
        SelectedTabIndex = 1;
    }
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
        $"Generic fields: {(value.GenericFieldsFromRaw ? "derived from RAW" : "recorded or unavailable")}   Hop count: {value.HopCount?.ToString() ?? "—"}   TP length: {value.TpLength?.ToString() ?? "—"}   Checksum: {value.Checksum}   ACK: {value.Ack}\n" +
        $"Parity errors: {value.ParityErrors}   Timing errors: {value.TimingErrors}   Overflow: {value.Overflow}\nRaw bytes: {string.Join(" ", value.RawBytes.Select(x => x.ToString("X2")))}\n\nOriginal JSON:\n{JsonSerializer.Serialize(value.Original, new JsonSerializerOptions { WriteIndented = true })}";
    partial void OnSelectedAnalogEventChanged(AnalogEvent? value)
    {
        SelectedCapture = null; AnalogAxis = "Time axis unavailable"; AnalogVariationNotice = "";
        if (value is null || SelectedSession is null) { AnalogDetails = "Select an analog event."; return; }
        AnalogDetails = JsonSerializer.Serialize(value.Original, new JsonSerializerOptions { WriteIndented = true });
        if (!value.RawPersisted) return;
        var path = System.IO.Path.Combine(SelectedSession.DirectoryPath, "captures", $"event-{value.EventId:D6}.bin");
        try {
            var raw = RawCapture.Read(path);
            SelectedCapture = raw;
            AnalogVariationNotice = raw.PeakToPeak <= 2 ? "No analog variation in this capture" : "";
            AnalogAxis = raw.SampleRateHz == 0 ? "Time axis unavailable (sample rate 0)" : $"Time: {-1000.0 * raw.TriggerIndex / raw.SampleRateHz:F1} ms     trigger t=0     +{1000.0 * (raw.Samples.Length - raw.TriggerIndex) / raw.SampleRateHz:F1} ms";
            var pipeline = AnalogVoltagePipeline.FromSessionMetadata(SelectedSession.Metadata);
            string voltage;
            if (pipeline.Calibration is null) {
                voltage = "ADC calibration unavailable. GPIO5 voltage is not estimated.";
            } else if (pipeline.TryEstimateGpio5Millivolts(raw.Minimum, out var lowMv) &&
                       pipeline.TryEstimateGpio5Millivolts(raw.Maximum, out var highMv)) {
                voltage = $"Estimated ADC at GPIO5: {lowMv:F1}–{highMv:F1} mV (calibration: {pipeline.Calibration.Source}).";
                if (pipeline.TryEstimateKnxBusVolts(raw.Minimum, out var lowBus) &&
                    pipeline.TryEstimateKnxBusVolts(raw.Maximum, out var highBus))
                    voltage += $" Estimated KNX bus: {lowBus:F2}–{highBus:F2} V (divider: {pipeline.Divider!.Source}).";
            } else {
                voltage = "ADC calibration does not cover this RAW range. GPIO5 voltage is unavailable.";
            }
            AnalogDetails = $"RAW at GPIO5: {raw.Samples.Length} samples | {raw.SampleRateHz} Hz | min {raw.Minimum} | max {raw.Maximum} | P-P {raw.PeakToPeak} | mean {raw.Mean:F2} | CRC {(raw.CrcValid ? "OK" : "INVALID")} | trigger index {raw.TriggerIndex}\n{voltage}\n\n" + AnalogDetails;
        } catch (Exception e) { AnalogDetails = $"RAW unavailable: {e.Message}\n\n" + AnalogDetails; }
    }
}
