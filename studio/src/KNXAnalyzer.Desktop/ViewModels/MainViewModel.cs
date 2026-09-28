using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.IO;
using System.Linq;
using System.Text.Json;
using System.Threading.Tasks;
using CommunityToolkit.Mvvm.ComponentModel;
using KNXAnalyzer.Core;

namespace KNXAnalyzer.Desktop.ViewModels;

public partial class MainViewModel : ViewModelBase
{
    private NetworkImportService? networkImport;
    public ObservableCollection<Session> Sessions { get; } = [];
    public ObservableCollection<NetworkSessionInfo> NetworkSessions { get; } = [];
    public ObservableCollection<Tp1Candidate> VisibleCandidates { get; } = [];
    public ObservableCollection<AnalogEvent> AnalogEvents { get; } = [];
    public ObservableCollection<ParticipantDisplay> Participants { get; } = [];
    public ObservableCollection<GroupDisplay> Groups { get; } = [];
    public ObservableCollection<InteractionDisplay> Interactions { get; } = [];
    public ObservableCollection<ParticipantDestinationDisplay> ParticipantDestinations { get; } = [];
    public ObservableCollection<OfflineTp1Candidate> OfflineCandidates { get; } = [];
    public ObservableCollection<OfflineTp1Candidate> OfflineRecordResults { get; } = [];
    public ObservableCollection<Tp1Character> OfflineCharacters { get; } = [];
    public ObservableCollection<Tp1BitSlot> OfflineCharacterSlots { get; } = [];
    public string[] Filters { get; } = ["All", "Valid", "Errors", "ACK", "Unknown"];
    public string[] AnalogSortOptions { get; } = ["P-P descending", "P-P ascending", "Event ID"];
    public IReadOnlyList<Tp1AnalogDecodeProfile> OfflineProfiles { get; } = Tp1AnalogDecodeProfile.Available;
    public string[] OfflineRecordFilters { get; } = ["All", "Valid", "ACK", "Errors"];
    public string[] InteractionSortOptions { get; } = ["Occurrences", "Sessions", "Délai médian"];
    [ObservableProperty] private string folderPath = "";
    [ObservableProperty] private string networkHost = NetworkImportService.RememberedAnalyzer;
    [ObservableProperty] private string networkAnalyzer = "No network Analyzer connected.";
    [ObservableProperty] private string networkProgress = "";
    [ObservableProperty] private NetworkSessionInfo? selectedNetworkSession;
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
    [ObservableProperty] private string analogCursor = "Move the pointer over either graph to inspect a sample.";
    [ObservableProperty] private string eventViewerSummary = "Select an event to open Event RAW Viewer V1.";
    [ObservableProperty] private int observedTrafficCount;
    [ObservableProperty] private int syntheticExcludedCount;
    [ObservableProperty] private int participantCount;
    [ObservableProperty] private int groupCount;
    [ObservableProperty] private int interactionCount;
    [ObservableProperty] private ParticipantDisplay? selectedParticipant;
    [ObservableProperty] private GroupDisplay? selectedGroup;
    [ObservableProperty] private InteractionDisplay? selectedInteraction;
    [ObservableProperty] private string participantPriorities = "—";
    [ObservableProperty] private string participantMotifs = "Aucun motif récurrent affichable.";
    [ObservableProperty] private string groupServices = "—";
    [ObservableProperty] private string groupValues = "—";
    [ObservableProperty] private string groupInteractions = "Aucun motif récurrent affichable.";
    [ObservableProperty] private string selectedInteractionSort = "Occurrences";
    [ObservableProperty] private Tp1AnalogDecodeProfile selectedOfflineProfile = Tp1AnalogDecodeProfile.Historical;
    [ObservableProperty] private string offlineProfileComparison = "—";
    [ObservableProperty] private string offlineAnalysisSummary = "Sélectionnez une capture analogique.";
    [ObservableProperty] private int offlineSampleCount;
    [ObservableProperty] private string offlineDuration = "—";
    [ObservableProperty] private string offlineBaseline = "—";
    [ObservableProperty] private string offlineNoiseRms = "—";
    [ObservableProperty] private string offlineActivity = "—";
    [ObservableProperty] private int offlinePulseCount;
    [ObservableProperty] private int offlineCandidateCount;
    [ObservableProperty] private int offlineValidFrameCount;
    [ObservableProperty] private OfflineTp1Candidate? selectedOfflineCandidate;
    [ObservableProperty] private Tp1Character? selectedOfflineCharacter;
    [ObservableProperty] private string offlineCharacterDiagnostic = "Sélectionnez un caractère.";
    [ObservableProperty] private string selectedOfflineRecordFilter = "All";
    [ObservableProperty] private Tp1DecodeResult? selectedTp1Decode;
    [ObservableProperty] private bool showTp1Overlays = true;
    [ObservableProperty] private string offlineFrameTitle = "";
    [ObservableProperty] private string offlineFrameSource = "—";
    [ObservableProperty] private string offlineFrameDestination = "—";
    [ObservableProperty] private string offlineFrameService = "—";
    [ObservableProperty] private string offlineFrameChecksum = "—";
    [ObservableProperty] private string offlineFrameParity = "—";
    [ObservableProperty] private string offlineFrameTiming = "—";
    [ObservableProperty] private string offlineFrameRaw = "—";

    public void OpenFolder(string path)
    {
        try {
            Sessions.Clear();
            foreach (var session in SessionReader.OpenFolder(path)) Sessions.Add(session);
            FolderPath = path;
            RefreshTrafficAnalysis();
            SelectedSession = Sessions.FirstOrDefault();
            Status = $"{Sessions.Count} session(s) found. Original files are read only.";
        } catch (Exception e) { Status = e.Message; }
    }
    public async Task OpenNetworkAsync()
    {
        try {
            Status = "Network: discovery / Analyzer API..."; NetworkProgress = "metadata";
            networkImport?.Dispose(); networkImport = new NetworkImportService(NetworkHost);
            var analyzer = await networkImport.ConnectAsync();
            NetworkAnalyzer = $"{analyzer.Hostname} · {analyzer.AnalyzerId} · {analyzer.Ip} · RSSI {analyzer.Rssi} dBm · {analyzer.FirmwareVersion} · {analyzer.State} · SD {(analyzer.SdReady ? "READY" : "ERROR")}";
            var remote = await networkImport.ListSessionsAsync(); NetworkSessions.Clear();
            foreach (var descriptor in remote.OrderByDescending(x => x.StartUtc).ThenByDescending(x => x.SessionId)) NetworkSessions.Add(descriptor);
            SelectedNetworkSession = NetworkSessions.FirstOrDefault();
            FolderPath = $"NETWORK {networkImport.BaseUri}";
            NetworkProgress = "session list ready"; Status = $"{NetworkSessions.Count} network session(s). Select one, then import metadata.";
        } catch (Exception e) { Status = $"Network import: {e.Message}"; NetworkProgress = "error"; }
    }
    public async Task ImportSelectedNetworkSessionAsync()
    {
        if (networkImport is null || SelectedNetworkSession is null) return;
        try {
            var progress = new Progress<NetworkImportProgress>(x => NetworkProgress = x.FromCache ? $"cache · {x.Stage}" : x.Total is long total ? $"{x.Stage} · {x.Downloaded}/{total}" : x.Stage);
            var session = await networkImport.ImportMetadataAsync(SelectedNetworkSession, progress);
            Sessions.Clear(); Sessions.Add(session); RefreshTrafficAnalysis(); SelectedSession = session;
            NetworkProgress = "metadata validated"; Status = "Session imported. Event RAW will be fetched and CRC-checked on demand.";
        } catch (Exception e) { Status = $"Network import: {e.Message}"; NetworkProgress = "error"; }
    }
    private void RefreshTrafficAnalysis()
    {
        Participants.Clear(); Groups.Clear(); Interactions.Clear();
        var analysis = TrafficAnalyzer.Analyze(Sessions);
        foreach (var item in analysis.Participants)
            Participants.Add(new ParticipantDisplay(item, analysis.ObservedTelegramCount == 0 ? 0 : 100.0 * item.TelegramCount / analysis.ObservedTelegramCount));
        foreach (var item in analysis.Groups) Groups.Add(new GroupDisplay(item));
        foreach (var item in analysis.Interactions) Interactions.Add(new InteractionDisplay(item));
        ObservedTrafficCount = analysis.ObservedTelegramCount;
        SyntheticExcludedCount = analysis.SyntheticTelegramCount;
        ParticipantCount = analysis.Participants.Count;
        GroupCount = analysis.Groups.Count;
        InteractionCount = analysis.Interactions.Count;
        SelectedParticipant = Participants.FirstOrDefault();
        SelectedGroup = Groups.FirstOrDefault();
        SelectedInteraction = Interactions.FirstOrDefault();
    }

    partial void OnSelectedParticipantChanged(ParticipantDisplay? value)
    {
        ParticipantDestinations.Clear();
        if (value is null) return;
        var address = value.Address;
        var telegrams = Sessions.SelectMany(session => session.Candidates
            .Where(candidate => !candidate.SyntheticTest)
            .Select(candidate => KnxTelegramDecoder.Decode(candidate))
            .Where(telegram => telegram is not null && telegram.Source == address)
            .Cast<KnxTelegram>()).ToArray();
        foreach (var group in telegrams.Where(x => x.DestinationType == "group").GroupBy(x => x.Destination)
            .OrderByDescending(x => x.Count()).ThenBy(x => x.Key))
            ParticipantDestinations.Add(new ParticipantDestinationDisplay(group.Key, group.Count(),
                string.Join(", ", group.Select(x => x.Service).Distinct().Order()),
                string.Join(", ", group.Select(x => string.IsNullOrEmpty(x.PayloadHex) ? x.ApduHex : $"{x.ApduHex} / {x.PayloadHex}").Distinct().Order())));
        ParticipantPriorities = FormatCounts(value.Model.Priorities) +
            $"\nRepeat: {value.Model.Repeated} · Checksum valide: {value.Model.ChecksumValid}/{value.Model.TelegramCount}";
        ParticipantMotifs = FormatInteractions(Interactions.Where(x => x.Model.A.Source == address || x.Model.B.Source == address));
    }

    partial void OnSelectedGroupChanged(GroupDisplay? value)
    {
        if (value is null) return;
        GroupServices = FormatCounts(value.Model.Services);
        GroupValues = $"APDU\n{FormatCounts(value.Model.Apdus)}\n\nValeurs / payloads\n{FormatCounts(value.Model.Payloads)}";
        GroupInteractions = FormatInteractions(Interactions.Where(x => x.Model.A.Destination == value.Address || x.Model.B.Destination == value.Address));
    }

    partial void OnSelectedInteractionSortChanged(string value) => RefreshInteractionSort();
    private void RefreshInteractionSort()
    {
        var selected = SelectedInteraction;
        var ordered = SelectedInteractionSort switch {
            "Sessions" => Interactions.OrderByDescending(x => x.SessionCount).ThenByDescending(x => x.Occurrences).ToArray(),
            "Délai médian" => Interactions.OrderBy(x => x.MedianDelayMs).ThenByDescending(x => x.Occurrences).ToArray(),
            _ => Interactions.OrderByDescending(x => x.Occurrences).ThenByDescending(x => x.SessionCount).ToArray()
        };
        Interactions.Clear(); foreach (var item in ordered) Interactions.Add(item);
        if (selected is not null && Interactions.Contains(selected)) SelectedInteraction = selected;
    }

    private static string FormatCounts(System.Collections.Generic.IEnumerable<TrafficCount> values) =>
        string.Join("\n", values.Select(x => $"{x.Value}  ·  {x.Count}"));
    private static string FormatInteractions(System.Collections.Generic.IEnumerable<InteractionDisplay> values)
    {
        var rows = values.Take(10).Select(x => $"{x.Sequence}  ·  {x.Occurrences} occurrences");
        var text = string.Join("\n", rows);
        return string.IsNullOrEmpty(text) ? "Aucun motif récurrent affichable." : text;
    }

    partial void OnSelectedSessionChanged(Session? value)
    {
        VisibleCandidates.Clear(); AnalogEvents.Clear(); SelectedCandidate = null; SelectedAnalogEvent = null;
        if (value is null) { Summary = "No session selected"; return; }
        RefreshAnalogSort();
        Summary = $"Analyzer: {value.Analyzer}   Session: {value.Id}   Mode: {value.AcquisitionMode}   State: {value.State}   Duration: {(value.DurationMs is long ms ? $"{TimeSpan.FromMilliseconds(ms):hh\\:mm\\:ss}" : "unknown")}   Events: {value.DisplayEventCount}   RAW available: {value.RawAvailableText}\n" +
            $"Date: {value.DateTime ?? "unavailable (device clock unset)"}   Format: {value.Generation}   Candidates: {value.Candidates.Count}   Valid: {value.Count("VALID_KNOWN") + value.Count("VALID_UNKNOWN")}   ACK: {value.Candidates.Count(x => x.Ack == "ACK")}   Parity: {value.Count("INVALID_PARITY")}   Checksum: {value.Count("INVALID_CHECKSUM")}   Timing: {value.Count("INVALID_TIMING")}   Incomplete: {value.Count("INCOMPLETE")}   Warnings: {value.Diagnostics.Count}";
        var decoded = value.Candidates.Select(KnxTelegramDecoder.Decode).Where(x => x is not null).ToArray();
        Summary += $"   Decoded standard: {decoded.Length}   Group: {decoded.Count(x => x!.DestinationType == "group")}   Individual: {decoded.Count(x => x!.DestinationType == "individual")}   Repeat: {decoded.Count(x => x!.Repeat)}";
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
            "P-P ascending" => SelectedSession.AnalogEvents.OrderBy(x => x.CaptureSummary?.PeakToPeak ?? x.RecordedPeakToPeak ?? int.MaxValue).ThenBy(x => x.EventId),
            "Event ID" => SelectedSession.AnalogEvents.OrderBy(x => x.EventId),
            _ => SelectedSession.AnalogEvents.OrderByDescending(x => x.CaptureSummary?.PeakToPeak ?? x.RecordedPeakToPeak ?? -1).ThenBy(x => x.EventId)
        };
        foreach (var item in ordered) AnalogEvents.Add(item);
        if (selected is not null && AnalogEvents.Contains(selected)) SelectedAnalogEvent = selected;
    }
    public void SelectLargestCapture()
    {
        var winner = Sessions.SelectMany(s => s.AnalogEvents.Where(e => e.RawPersisted)
            .Select(e => (Session: s, Event: e)))
            .OrderByDescending(x => x.Event.CaptureSummary?.PeakToPeak ?? x.Event.RecordedPeakToPeak ?? -1).FirstOrDefault();
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
    partial void OnSelectedCandidateChanged(Tp1Candidate? value)
    {
        if (value is null) { Details = "Select a TP1 candidate."; return; }
        var telegram = KnxTelegramDecoder.Decode(value);
        var decoded = telegram is null ? "Generic protocol fields: unavailable or not verified" :
            $"DERIVED from RAW: {telegram.Format} | control 0x{telegram.Control:X2} | repeat {telegram.Repeat} | priority {telegram.Priority}\\n" +
            $"TPCI {telegram.Tpci} | APCI {telegram.Apci?.ToString() ?? "unavailable"} | service {telegram.Service} | APDU {telegram.ApduHex} | payload {telegram.PayloadHex}";
        Details = $"Line {value.Line}   {value.Classification}\\nTime: {value.Time}\\nSource: {value.Source ?? "unavailable"}   Destination: {value.Destination ?? "unavailable"} ({value.DestinationType ?? "unavailable"})\\n" +
            $"Generic fields: {(value.GenericFieldsFromRaw ? "derived from RAW" : "recorded or unavailable")}   Hop count: {value.HopCount?.ToString() ?? "unavailable"}   TP length: {value.TpLength?.ToString() ?? "unavailable"}   Checksum: {value.Checksum}   ACK: {value.Ack}\\n" +
            $"Parity errors: {value.ParityErrors}   Timing errors: {value.TimingErrors}   Overflow: {value.Overflow}\\n{decoded}\\nRaw bytes: {string.Join(" ", value.RawBytes.Select(x => x.ToString("X2")))}\\n\\nOriginal JSON:\\n{JsonSerializer.Serialize(value.Original, new JsonSerializerOptions { WriteIndented = true })}";
    }
    partial void OnSelectedAnalogEventChanged(AnalogEvent? value)
    {
        SelectedCapture = null; AnalogAxis = "Time axis unavailable"; AnalogVariationNotice = "";
        OfflineCandidates.Clear(); OfflineRecordResults.Clear(); SelectedOfflineCandidate = null; SelectedTp1Decode = null; OfflineAnalysisSummary = "Analyse offline indisponible.";
        OfflineSampleCount = 0; OfflineDuration = OfflineBaseline = OfflineNoiseRms = OfflineActivity = "—";
        OfflinePulseCount = OfflineCandidateCount = OfflineValidFrameCount = 0; ClearOfflineFrame();
        if (value is null || SelectedSession is null) { AnalogDetails = "Select an analog event."; EventViewerSummary = "Select an event to open Event RAW Viewer V1."; return; }
        AnalogDetails = JsonSerializer.Serialize(value.Original, new JsonSerializerOptions { WriteIndented = true });
        if (!value.RawPersisted) return;
        if (SelectedSession.Network is not null && networkImport is not null) { _ = LoadNetworkCaptureAsync(SelectedSession, value); return; }
        var path = System.IO.Path.Combine(SelectedSession.DirectoryPath, "captures", $"event-{value.EventId:D6}.bin");
        try {
            var raw = value.EventRawV2 ? EventRawV2Reader.ReadCapture(SelectedSession.DirectoryPath, value.EventId) : RawCapture.Read(path);
            PresentCapture(raw);
        } catch (RawCaptureLoadException e) { SetRawIntegrity(value, e.Status); AnalogDetails = $"RAW {e.Status}: {e.Message}\n\n" + AnalogDetails; EventViewerSummary = $"Event {value.EventId} · RAW {e.Status}"; }
        catch (InvalidDataException e) { SetRawIntegrity(value, RawIntegrityStatus.CrcInvalid); AnalogDetails = $"RAW CRC invalid: {e.Message}\n\n" + AnalogDetails; EventViewerSummary = $"Event {value.EventId} · RAW CRC invalid"; }
        catch (Exception e) { AnalogDetails = $"RAW unavailable: {e.Message}\n\n" + AnalogDetails; }
    }
    private async Task LoadNetworkCaptureAsync(Session session, AnalogEvent value)
    {
        try {
            NetworkProgress = "RAW requested"; Status = $"Downloading RAW for event {value.EventId}...";
            var progress = new Progress<NetworkImportProgress>(x => NetworkProgress = x.FromCache ? $"cache · {x.Downloaded} bytes" : x.Total is long total ? $"{x.Stage} · {x.Downloaded}/{total}" : x.Stage);
            var raw = await networkImport!.FetchEventAsync(session, value.EventId, progress);
            if (!ReferenceEquals(SelectedSession, session) || SelectedAnalogEvent?.EventId != value.EventId) return;
            value.CaptureSummary = raw.Summary; PresentCapture(raw);
            NetworkProgress = "verification · CRC OK · decode"; Status = $"Event {value.EventId}: network RAW verified and decoded.";
        } catch (RawCaptureLoadException e) { SetRawIntegrity(value, e.Status); NetworkProgress = $"error · {e.Status}"; Status = $"Network RAW {e.Status}: {e.Message}"; AnalogDetails = $"RAW {e.Status}: {e.Message}\n\n" + AnalogDetails; EventViewerSummary = $"Event {value.EventId} · RAW {e.Status}"; }
        catch (Exception e) { NetworkProgress = "error · retry available"; Status = $"Network RAW: {e.Message}"; AnalogDetails = $"RAW unavailable: {e.Message}\n\n" + AnalogDetails; }
    }
    public void SelectAdjacentEvent(int delta)
    {
        if (AnalogEvents.Count == 0) return;
        var index = SelectedAnalogEvent is null ? 0 : AnalogEvents.IndexOf(SelectedAnalogEvent);
        SelectedAnalogEvent = AnalogEvents[Math.Clamp(index + delta, 0, AnalogEvents.Count - 1)];
    }
    private void PresentCapture(RawCapture raw)
    {
        if (!raw.CrcValid) throw new InvalidDataException("RAW CRC is invalid; decode refused.");
        if (SelectedAnalogEvent is { } selected) {
            selected.CaptureSummary = raw.Summary;
            selected.RawIntegrity = raw.Integrity;
            selected.D44Maximum = raw.D44Maximum;
            selected.ThresholdExceedanceCount = raw.ThresholdExceedanceIndices.Length;
            var index = AnalogEvents.IndexOf(selected);
            if (index >= 0) AnalogEvents[index] = selected;
        }
        SelectedCapture = raw; ApplyOfflineAnalysis(raw);
        EventViewerSummary = $"Event {raw.EventId} · {raw.DurationMilliseconds:F3} ms · {raw.Samples.Length:N0} samples · {raw.SampleRateHz:N0} Hz · ADC {raw.Minimum}…{raw.Maximum} · D44 max {raw.D44Maximum} · threshold {raw.Threshold} · {raw.Integrity}";
        AnalogVariationNotice = raw.PeakToPeak <= 2 ? "No analog variation in this capture" : "";
        AnalogAxis = raw.SampleRateHz == 0 ? "Time axis unavailable (sample rate 0)" : $"Time: {-1000.0 * raw.TriggerIndex / raw.SampleRateHz:F1} ms     trigger t=0     +{1000.0 * (raw.Samples.Length - raw.TriggerIndex) / raw.SampleRateHz:F1} ms";
        var pipeline = AnalogVoltagePipeline.FromSessionMetadata(SelectedSession!.Metadata);
        var s3Gpio1 = SelectedSession.StartMetadata.ValueKind == JsonValueKind.Object &&
            SelectedSession.StartMetadata.TryGetProperty("adc_gpio", out var adcGpio) && adcGpio.TryGetInt32(out var gpio) && gpio == 1;
        string voltage;
        if (s3Gpio1) {
            voltage = "ADC RAW is authoritative. No voltage conversion is recorded for the XIAO GPIO1 front-end.";
        } else if (pipeline.Calibration is null) {
            voltage = $"Estimated ADC voltage — experimental calibration (GPIO5): {ExperimentalEstimatedCalibration.EstimateMillivolts(raw.Minimum):F0}–{ExperimentalEstimatedCalibration.EstimateMillivolts(raw.Maximum):F0} mV. One approximate point; proportional display assumption; accuracy unknown. RAW remains authoritative.";
        } else if (pipeline.TryEstimateGpio5Millivolts(raw.Minimum, out var lowMv) && pipeline.TryEstimateGpio5Millivolts(raw.Maximum, out var highMv)) {
            voltage = $"Estimated ADC at GPIO5: {lowMv:F1}–{highMv:F1} mV (calibration: {pipeline.Calibration.Source}).";
            if (pipeline.TryEstimateKnxBusVolts(raw.Minimum, out var lowBus) && pipeline.TryEstimateKnxBusVolts(raw.Maximum, out var highBus))
                voltage += $" Estimated KNX bus: {lowBus:F2}–{highBus:F2} V (divider: {pipeline.Divider!.Source}).";
        } else voltage = "ADC calibration does not cover this RAW range. GPIO5 voltage is unavailable.";
        AnalogDetails = $"EVENT RAW VIEWER V1\nEvent {raw.EventId} | {raw.ChunkCount} chunks | {raw.Samples.Length:N0} samples | {raw.DurationMilliseconds:F3} ms | {raw.SampleRateHz:N0} Hz\n" +
            $"Samples [{raw.SampleStart:N0}, {raw.SampleEnd:N0}) | trigger {raw.TriggerSample:N0} (index {raw.TriggerIndex:N0}) | actual pre {raw.TriggerIndex:N0} | actual post {raw.Samples.Length - raw.TriggerIndex:N0} | configured pre/post {raw.ConfiguredPreSamples:N0}/{raw.ConfiguredPostSamples:N0}\n" +
            $"ADC min {raw.Minimum} | max {raw.Maximum} | P-P {raw.PeakToPeak} | mean {raw.Mean:F3}\n" +
            $"D44 max {raw.D44Maximum} | threshold {raw.Threshold} | D44 > threshold: {raw.ThresholdExceedanceIndices.Length:N0}\n" +
            $"Integrity {raw.Integrity} | CRC OK\n{voltage}\n\n" + AnalogDetails;
    }
    private void SetRawIntegrity(AnalogEvent item, RawIntegrityStatus status)
    {
        item.RawIntegrity = status;
        var index = AnalogEvents.IndexOf(item);
        if (index >= 0) AnalogEvents[index] = item;
    }
    partial void OnSelectedOfflineProfileChanged(Tp1AnalogDecodeProfile value)
    {
        if (SelectedCapture is not null) ApplyOfflineAnalysis(SelectedCapture);
    }

    private void ApplyOfflineAnalysis(RawCapture raw)
    {
        var offline = OfflineRawAnalyzer.Analyze(raw, SelectedOfflineProfile);
        SelectedTp1Decode = offline.DecodeResult;
        OfflineRecordResults.Clear();
        foreach (var candidate in offline.Tp1Candidates) OfflineRecordResults.Add(candidate);
        RefreshOfflineRecordFilter(preferValid: true);
        var experimental = offline.Profile.Experimental ? " · Experimental / field candidate" : "";
        OfflineAnalysisSummary = $"Profil actif : {offline.Profile.DisplayName} · seuils {offline.Profile.LowThreshold}/{offline.Profile.HighThreshold} RAW{experimental} · classification {offline.Classification}";
        OfflineSampleCount = offline.SampleCount;
        OfflineDuration = $"{offline.DurationMilliseconds:F3} ms";
        OfflineBaseline = $"{offline.Baseline:F2} RAW";
        OfflineNoiseRms = $"{offline.NoiseRms:F2} RAW";
        OfflineActivity = offline.Classification == OfflineAnalogClassification.NO_SIGNAL ? "Aucune" : "Détectée";
        OfflinePulseCount = offline.DecodeResult?.Pulses.Count ?? 0;
        OfflineCandidateCount = offline.Tp1Candidates.Count;
        OfflineValidFrameCount = offline.ValidFrameCount;
        var comparison = OfflineRawAnalyzer.CompareProfiles(raw);
        OfflineProfileComparison = string.Join(Environment.NewLine,
            $"Historical : {comparison.Historical.Tp1Candidates.Count} records · valides {comparison.Historical.ValidFrameCount} · ACK {comparison.Historical.DecodeResult?.AckCount ?? 0} · parité {comparison.Historical.ParityErrorCount} · checksum {comparison.Historical.ChecksumErrorCount} · timing {comparison.Historical.TimingErrorCount}",
            $"Field candidate : {comparison.FieldCandidate.Tp1Candidates.Count} records · valides {comparison.FieldCandidate.ValidFrameCount} · ACK {comparison.FieldCandidate.DecodeResult?.AckCount ?? 0} · parité {comparison.FieldCandidate.ParityErrorCount} · checksum {comparison.FieldCandidate.ChecksumErrorCount} · timing {comparison.FieldCandidate.TimingErrorCount}",
            $"Octets identiques : {(comparison.ByteStreamsIdentical ? "oui" : "non — différences détectées")}");
    }
    partial void OnSelectedOfflineRecordFilterChanged(string value) => RefreshOfflineRecordFilter();
    private void RefreshOfflineRecordFilter(bool preferValid = false)
    {
        var selected = SelectedOfflineCandidate;
        OfflineCandidates.Clear();
        foreach (var record in OfflineRecordResults.Where(x => MatchesOfflineRecordFilter(x, SelectedOfflineRecordFilter)))
            OfflineCandidates.Add(record);
        if (selected is not null && OfflineCandidates.Contains(selected)) SelectedOfflineCandidate = selected;
        else SelectedOfflineCandidate = preferValid
            ? OfflineCandidates.FirstOrDefault(x => x.RecordClassification is Tp1RecordClassification.VALID_KNOWN or Tp1RecordClassification.VALID_UNKNOWN)
                ?? OfflineCandidates.FirstOrDefault()
            : OfflineCandidates.FirstOrDefault();
    }
    public static bool MatchesOfflineRecordFilter(OfflineTp1Candidate record, string filter) => filter switch {
        "Valid" => record.RecordClassification is Tp1RecordClassification.VALID_KNOWN or Tp1RecordClassification.VALID_UNKNOWN,
        "ACK" => record.KnownControl == Tp1KnownControl.ACK,
        "Errors" => record.RecordClassification is Tp1RecordClassification.INVALID_PARITY or Tp1RecordClassification.INVALID_TIMING or Tp1RecordClassification.INVALID_CHECKSUM,
        _ => true
    };
    public void SelectAdjacentOfflineRecord(int delta)
    {
        if (OfflineCandidates.Count == 0) return;
        var index = SelectedOfflineCandidate is null ? 0 : OfflineCandidates.IndexOf(SelectedOfflineCandidate);
        SelectedOfflineCandidate = OfflineCandidates[Math.Clamp(index + delta, 0, OfflineCandidates.Count - 1)];
    }
    partial void OnSelectedOfflineCandidateChanged(OfflineTp1Candidate? value)
    {
        OfflineCharacters.Clear(); OfflineCharacterSlots.Clear(); SelectedOfflineCharacter = null;
        if (value is null) { ClearOfflineFrame(); return; }
        var decodedRecord = SelectedTp1Decode?.Records.FirstOrDefault(x =>
            x.StartSample == value.StartSample && x.EndSample == value.EndSample && x.RawHex == value.RawHex);
        if (decodedRecord is not null) {
            foreach (var character in decodedRecord.Characters) OfflineCharacters.Add(character);
            SelectedOfflineCharacter = OfflineCharacters.FirstOrDefault(x => !x.ParityValid || !x.TimingValid)
                ?? OfflineCharacters.FirstOrDefault();
        }
        OfflineFrameTitle = $"{value.RecordClassification} · {(value.KnownControl == Tp1KnownControl.None ? "record TP1" : value.KnownControl)}";
        var notApplicable = value.KnownControl != Tp1KnownControl.None || value.RawHex.Length < 16;
        OfflineFrameSource = value.Telegram?.Source ?? (notApplicable ? "Non applicable" : "Indisponible");
        OfflineFrameDestination = value.Telegram?.Destination ?? (notApplicable ? "Non applicable" : "Indisponible");
        OfflineFrameService = value.Telegram?.Service ?? (notApplicable ? "Non applicable" : "Indisponible");
        OfflineFrameChecksum = value.RawHex.Length < 16 ? "Non applicable" : value.ChecksumValid ? "Valide" : "Invalide";
        OfflineFrameParity = value.ParityErrors == 0 ? "Valide" : $"Invalide · {value.ParityErrors} erreur(s)";
        OfflineFrameTiming = $"{value.TimingRmsMicroseconds:F2} µs RMS · max {value.TimingMaxErrorMicroseconds:F2} µs";
        OfflineFrameRaw = value.RawHex;
    }
    partial void OnSelectedOfflineCharacterChanged(Tp1Character? value)
    {
        OfflineCharacterSlots.Clear();
        if (value is null) { OfflineCharacterDiagnostic = "Sélectionnez un caractère."; return; }
        foreach (var slot in value.Slots) OfflineCharacterSlots.Add(slot);
        OfflineCharacterDiagnostic =
            $"Event {value.EventId} · Record {value.RecordIndex} · Character {value.Index} · local sample {value.StartSample:N0} · absolute {value.AbsoluteStartSample:N0} · t={value.RelativeStartMicroseconds / 1000.0:+0.000;-0.000;0.000} ms\n" +
            $"Byte {value.Value:X2} · bits LSB {value.DataBitsLsb} · ones {value.DataOneCount} · parity expected {(value.ExpectedParity ? 1 : 0)} / observed {(value.ReceivedParity ? 1 : 0)} · {(value.ParityValid ? "VALID" : "INVALID")} · STOP {(value.ReceivedStop ? 1 : 0)}\n" +
            $"Timing signed mean {value.TimingSignedMeanSamples:+0.000;-0.000;0.000} sample · abs mean {value.TimingAbsoluteMeanSamples:F3} · RMS {value.TimingRmsSamples:F3} · max {value.TimingMaximumSamples:F3} · unassigned {value.UnassignedPulses.Count} · duplicate-slot {value.DuplicateSlotPulses.Count}";
    }
    private void ClearOfflineFrame()
    {
        OfflineFrameTitle = "";
        OfflineCharacterDiagnostic = "Sélectionnez un caractère.";
        OfflineFrameSource = OfflineFrameDestination = OfflineFrameService = OfflineFrameChecksum = OfflineFrameParity = OfflineFrameTiming = OfflineFrameRaw = "—";
    }
}
