using System;
using System.Collections.ObjectModel;
using System.IO;
using System.Linq;
using System.Threading.Tasks;
using CommunityToolkit.Mvvm.ComponentModel;
using KNXAnalyzer.Core;

namespace KNXAnalyzer.Desktop.ViewModels;

public sealed class RawCorpusEntryDisplay
{
    public RawCorpusEntryDisplay(RawCorpusEntry entry) => Entry = entry;
    public RawCorpusEntry Entry { get; }
    public string Session => Entry.SessionId;
    public string Date => Entry.AcquisitionUtc ?? "UNSYNCED";
    public string Site => Entry.AcquisitionMetadata.Site ?? "—";
    public string Bus => Entry.AcquisitionMetadata.Bus ?? "—";
    public string Point => Entry.AcquisitionMetadata.Point ?? "—";
    public string Duration => Entry.AdcDurationMicroseconds is ulong us ? TimeSpan.FromTicks((long)Math.Min(us, (ulong)(long.MaxValue / 10)) * 10).ToString("hh\\:mm\\:ss") : "—";
    public string Rate => Entry.MeasuredSampleRateHz is double hz ? $"{hz:N2} Hz" : "—";
    public string Samples => $"{Entry.SampleCount:N0}";
    public string RawSize => Entry.RawBytes < 1024 * 1024 ? $"{Entry.RawBytes / 1024.0:F1} KiB" : $"{Entry.RawBytes / (1024.0 * 1024):F1} MiB";
    public string Integrity => Entry.Integrity.ToString();
}

public partial class RawCorpusViewModel : ViewModelBase
{
    private readonly RawCorpusCatalog catalog;
    public ObservableCollection<RawCorpusEntryDisplay> Entries { get; } = [];
    public string[] IntegrityFilters { get; } = ["All", .. Enum.GetNames<RawCorpusIntegrity>()];
    [ObservableProperty] private RawCorpusEntryDisplay? selectedEntry;
    [ObservableProperty] private string textFilter = "";
    [ObservableProperty] private string siteFilter = "";
    [ObservableProperty] private string busFilter = "";
    [ObservableProperty] private string pointFilter = "";
    [ObservableProperty] private string integrityFilter = "All";
    [ObservableProperty] private bool managedCopy = true;
    [ObservableProperty] private string status = "Import a completed CONTINUOUS_RAW session folder.";
    [ObservableProperty] private string details = "Select a corpus entry.";
    [ObservableProperty] private string localAlias = "";
    [ObservableProperty] private string localComment = "";
    [ObservableProperty] private string localTags = "";

    public RawCorpusViewModel(string? catalogPath = null)
    {
        catalog = RawCorpusCatalog.Open(catalogPath ?? RawCorpusCatalog.DefaultPath); Refresh();
    }

    public async Task ImportAsync(string path)
    {
        try {
            Status = "Validating manifest and streaming RAW chunks…";
            var mode = ManagedCopy ? RawCorpusStorageMode.ManagedCopy : RawCorpusStorageMode.Reference;
            var result = await Task.Run(() => catalog.Import(path, mode)); Refresh(result.Entry.CorpusId);
            Status = result.AlreadyImported ? $"Already imported: {result.Entry.CorpusId}" :
                $"Imported {result.Entry.SessionId} · {result.Entry.Integrity} · {result.Metrics.Duration.TotalSeconds:F2} s · streaming buffer ≤ {result.Metrics.MaximumBufferBytes / 1024} KiB";
        } catch (Exception e) { Status = $"Import failed: {e.Message}"; }
    }

    public void SaveLocalMetadata()
    {
        if (SelectedEntry is null) return;
        var tags = LocalTags.Split(',', StringSplitOptions.TrimEntries | StringSplitOptions.RemoveEmptyEntries);
        catalog.UpdateLocalMetadata(SelectedEntry.Entry.CorpusId, new(Empty(LocalAlias), Empty(LocalComment), tags));
        Refresh(SelectedEntry.Entry.CorpusId); Status = "Local corpus metadata saved; acquisition manifest unchanged.";
    }

    partial void OnTextFilterChanged(string value) => Refresh();
    partial void OnSiteFilterChanged(string value) => Refresh();
    partial void OnBusFilterChanged(string value) => Refresh();
    partial void OnPointFilterChanged(string value) => Refresh();
    partial void OnIntegrityFilterChanged(string value) => Refresh();
    partial void OnSelectedEntryChanged(RawCorpusEntryDisplay? value)
    {
        if (value is null) { Details = "Select a corpus entry."; return; }
        var e = value.Entry; LocalAlias = e.LocalMetadata.Alias ?? ""; LocalComment = e.LocalMetadata.Comment ?? ""; LocalTags = string.Join(", ", e.LocalMetadata.Tags ?? []);
        Details = $"CORPUS {e.CorpusId}\nCorpus SHA-256 {e.ContentSha256}\nLogical RAW SHA-256 {e.LogicalRawSha256}\nSession {e.SessionId} · {e.State}/{e.CompletionStatus ?? "—"}\n" +
            $"Mode {e.AcquisitionMode} · {e.SampleFormat} · {e.SampleCount:N0} samples · {e.RawBytes:N0} bytes · {e.ChunkCount:N0} chunks\n" +
            $"ADC duration {(e.AdcDurationMicroseconds is ulong us ? us / 1_000_000.0 + " s" : "—")} · rate {e.MeasuredSampleRateHz?.ToString("F6") ?? "—"} Hz\n" +
            $"Integrity {e.Integrity} · gaps {e.GapCount} · dropped {e.DroppedSamples} · ADC errors {e.AdcErrors} · SD errors {e.SdErrors}\n" +
            $"Analyzer {e.AnalyzerId ?? "—"} · firmware {e.FirmwareVersion ?? "—"} · board {e.Board ?? "—"} · frontend {e.Frontend ?? "—"}\n" +
            $"Acquisition metadata: site={e.AcquisitionMetadata.Site ?? "—"}; bus={e.AcquisitionMetadata.Bus ?? "—"}; point={e.AcquisitionMetadata.Point ?? "—"}; note={e.AcquisitionMetadata.Note ?? "—"}\n" +
            $"Storage {e.StorageMode} · {e.DataDirectory}\nFiles:\n{string.Join("\n", e.Files.Select(x => $"  {x.RelativePath} · {x.Bytes:N0} bytes · {x.Role}"))}\n" +
            (e.IntegrityMessages.Length == 0 ? "All required integrity checks passed." : string.Join("\n", e.IntegrityMessages));
    }

    private void Refresh(string? selectId = null)
    {
        var selected = selectId ?? SelectedEntry?.Entry.CorpusId; Entries.Clear();
        var query = catalog.Entries.Where(Matches).OrderByDescending(x => x.AcquisitionUtc).ThenBy(x => x.SessionId);
        foreach (var entry in query) Entries.Add(new(entry)); SelectedEntry = Entries.FirstOrDefault(x => x.Entry.CorpusId == selected) ?? Entries.FirstOrDefault();
    }
    private bool Matches(RawCorpusEntry e)
    {
        bool Contains(string? value, string filter) => string.IsNullOrWhiteSpace(filter) || (value?.Contains(filter, StringComparison.OrdinalIgnoreCase) ?? false);
        var haystack = string.Join(' ', e.SessionId, e.CorpusId, e.AcquisitionMetadata.Site, e.AcquisitionMetadata.Bus, e.AcquisitionMetadata.Point, e.LocalMetadata.Alias, e.LocalMetadata.Comment, string.Join(' ', e.LocalMetadata.Tags ?? []));
        return Contains(haystack, TextFilter) && Contains(e.AcquisitionMetadata.Site, SiteFilter) && Contains(e.AcquisitionMetadata.Bus, BusFilter) &&
            Contains(e.AcquisitionMetadata.Point, PointFilter) && (IntegrityFilter == "All" || e.Integrity.ToString() == IntegrityFilter);
    }
    private static string? Empty(string value) => string.IsNullOrWhiteSpace(value) ? null : value.Trim();
}
