using System.Diagnostics;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace KNXAnalyzer.Core;

public enum RawCorpusIntegrity { Valid, ValidWithWarnings, Incomplete, Corrupted, Unsupported }
public enum RawCorpusStorageMode { Reference, ManagedCopy }

public sealed record RawCorpusFieldMetadata(string? Site, string? Bus, string? Point, string? Note);
public sealed record RawCorpusLocalMetadata(string? Alias = null, string? Comment = null, string[]? Tags = null);
public sealed record RawCorpusFile(string RelativePath, long Bytes, string Role);

public sealed record RawCorpusEntry
{
    public required string CorpusId { get; init; }
    public required string ContentSha256 { get; init; }
    public required string LogicalRawSha256 { get; init; }
    public required string SessionId { get; init; }
    public string? AcquisitionUtc { get; init; }
    public required DateTimeOffset ImportedUtc { get; init; }
    public required string SourceDirectory { get; init; }
    public required string DataDirectory { get; init; }
    public required RawCorpusStorageMode StorageMode { get; init; }
    public string AcquisitionMode { get; init; } = "CONTINUOUS_RAW";
    public string State { get; init; } = "UNKNOWN";
    public string? CompletionStatus { get; init; }
    public ulong? RequestedDurationSeconds { get; init; }
    public ulong? AdcDurationMicroseconds { get; init; }
    public double? MeasuredSampleRateHz { get; init; }
    public ulong SampleCount { get; init; }
    public string SampleFormat { get; init; } = "uint16_le";
    public long RawBytes { get; init; }
    public int ChunkCount { get; init; }
    public ulong GapCount { get; init; }
    public ulong DroppedSamples { get; init; }
    public ulong AdcErrors { get; init; }
    public ulong SdErrors { get; init; }
    public string? FirmwareVersion { get; init; }
    public string? AnalyzerId { get; init; }
    public string? Board { get; init; }
    public string? Frontend { get; init; }
    public required RawCorpusFieldMetadata AcquisitionMetadata { get; init; }
    public RawCorpusLocalMetadata LocalMetadata { get; init; } = new();
    public required RawCorpusIntegrity Integrity { get; init; }
    public required string[] IntegrityMessages { get; init; }
    public required RawCorpusFile[] Files { get; init; }
}

public sealed record RawCorpusImportMetrics(TimeSpan Duration, long PeakManagedBytesDelta, int MaximumBufferBytes);
public sealed record RawCorpusImportResult(RawCorpusEntry Entry, bool AlreadyImported, RawCorpusImportMetrics Metrics);

public sealed class RawCorpusCatalog
{
    private readonly List<RawCorpusEntry> entries;
    public string CatalogPath { get; }
    public string RootDirectory => Path.GetDirectoryName(CatalogPath)!;
    public IReadOnlyList<RawCorpusEntry> Entries => entries;

    private RawCorpusCatalog(string path, IEnumerable<RawCorpusEntry>? source = null)
    { CatalogPath = Path.GetFullPath(path); entries = source?.ToList() ?? []; }

    public static RawCorpusCatalog Open(string path)
    {
        var full = Path.GetFullPath(path);
        if (!File.Exists(full)) return new(full);
        using var stream = File.OpenRead(full);
        var items = JsonSerializer.Deserialize<List<RawCorpusEntry>>(stream, JsonOptions()) ?? [];
        return new(full, items);
    }

    public static string DefaultPath => Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
        "KNXAnalyzerStudio", "raw-corpus", "catalog.json");

    public RawCorpusImportResult Import(string sessionOrManifest, RawCorpusStorageMode storageMode = RawCorpusStorageMode.ManagedCopy)
    {
        var source = File.Exists(sessionOrManifest) ? Path.GetDirectoryName(Path.GetFullPath(sessionOrManifest))! : Path.GetFullPath(sessionOrManifest);
        var measured = RawCorpusImporter.Inspect(source);
        var existing = entries.FirstOrDefault(x => string.Equals(x.ContentSha256, measured.Entry.ContentSha256, StringComparison.OrdinalIgnoreCase));
        if (existing is not null) {
            var refreshed = existing with {
                FirmwareVersion = measured.Entry.FirmwareVersion ?? existing.FirmwareVersion,
                Board = measured.Entry.Board ?? existing.Board,
                Frontend = measured.Entry.Frontend ?? existing.Frontend
            };
            if (refreshed != existing) {
                entries[entries.IndexOf(existing)] = refreshed;
                Save();
            }
            return new(refreshed, true, measured.Metrics);
        }

        var entry = measured.Entry;
        if (storageMode == RawCorpusStorageMode.ManagedCopy) {
            var target = Path.Combine(RootDirectory, "entries", entry.CorpusId);
            CopyDirectoryExplicit(source, target);
            entry = entry with { DataDirectory = target, StorageMode = storageMode };
        } else entry = entry with { DataDirectory = source, StorageMode = storageMode };
        entries.Add(entry); Save();
        return new(entry, false, measured.Metrics);
    }

    public void UpdateLocalMetadata(string corpusId, RawCorpusLocalMetadata metadata)
    {
        var index = entries.FindIndex(x => x.CorpusId == corpusId);
        if (index < 0) throw new KeyNotFoundException(corpusId);
        entries[index] = entries[index] with { LocalMetadata = metadata }; Save();
    }

    public void Save()
    {
        Directory.CreateDirectory(RootDirectory);
        var temporary = CatalogPath + ".tmp";
        using (var stream = new FileStream(temporary, FileMode.Create, FileAccess.Write, FileShare.None))
            JsonSerializer.Serialize(stream, entries, JsonOptions());
        File.Move(temporary, CatalogPath, true);
    }

    private static JsonSerializerOptions JsonOptions() => new() { WriteIndented = true, PropertyNamingPolicy = JsonNamingPolicy.SnakeCaseLower,
        Converters = { new JsonStringEnumConverter(JsonNamingPolicy.SnakeCaseLower) } };

    private static void CopyDirectoryExplicit(string source, string target)
    {
        if (Directory.Exists(target)) throw new IOException($"Managed corpus target already exists: {target}");
        Directory.CreateDirectory(target);
        try {
            foreach (var directory in Directory.EnumerateDirectories(source, "*", SearchOption.AllDirectories))
                Directory.CreateDirectory(Path.Combine(target, Path.GetRelativePath(source, directory)));
            foreach (var file in Directory.EnumerateFiles(source, "*", SearchOption.AllDirectories)) {
                var destination = Path.Combine(target, Path.GetRelativePath(source, file));
                Directory.CreateDirectory(Path.GetDirectoryName(destination)!); File.Copy(file, destination, false);
            }
        } catch { if (Directory.Exists(target)) Directory.Delete(target, true); throw; }
    }
}

public static class RawCorpusImporter
{
    private sealed record Chunk(ulong Start, ulong Count, int Segment, long Offset, int Bytes, uint Crc)
    { public ulong End => Start + Count; }

    public static RawCorpusImportResult Inspect(string sourceDirectory)
    {
        var stopwatch = Stopwatch.StartNew(); var memory = GC.GetTotalMemory(false); var peakMemory = memory; var source = Path.GetFullPath(sourceDirectory);
        if (!Directory.Exists(source)) throw new DirectoryNotFoundException(source);
        var startPath = Path.Combine(source, "session-start.json"); var resultPath = Path.Combine(source, "test-result.json");
        var chunksPath = Path.Combine(source, "chunks.jsonl"); var manifestPath = Path.Combine(source, "manifest.json");
        if (!File.Exists(startPath)) throw new InvalidDataException("session-start.json is missing.");
        if (!File.Exists(chunksPath)) throw new InvalidDataException("chunks.jsonl is missing.");
        using var startDoc = ParseObject(startPath); using var resultDoc = File.Exists(resultPath) ? ParseObject(resultPath) : JsonDocument.Parse("{}");
        using var manifestDoc = File.Exists(manifestPath) ? ParseObject(manifestPath) : null;
        var start = startDoc.RootElement; var result = resultDoc.RootElement; var manifest = manifestDoc?.RootElement;
        var mode = Text(start, "acquisition_mode") ?? Text(start, "capture_mode") ?? Text(manifest, "acquisition_mode") ?? "EVENT";
        if (mode != "CONTINUOUS_RAW") throw new NotSupportedException($"Only CONTINUOUS_RAW is supported by the corpus importer (found {mode}).");
        var chunks = ReadChunks(chunksPath); var messages = new List<string>(); var integrity = RawCorpusIntegrity.Valid;
        if (!File.Exists(manifestPath)) { integrity = RawCorpusIntegrity.Incomplete; messages.Add("Authoritative manifest.json is missing."); }
        if (chunks.Count == 0) { integrity = RawCorpusIntegrity.Incomplete; messages.Add("No RAW chunks are described."); }
        var state = Text(result, "lifecycle") ?? Text(manifest, "lifecycle") ?? (Bool(result, "closed") ? "CLOSED" : "UNKNOWN");
        var completion = Text(result, "completion_status") ?? Text(manifest, "completion_status");
        if (state != "CLOSED" || (completion is not null && !completion.StartsWith("COMPLETE", StringComparison.Ordinal))) {
            integrity = Max(integrity, RawCorpusIntegrity.Incomplete); messages.Add($"Session lifecycle is {state}/{completion ?? "unspecified"}.");
        }
        var gapCount = U64(result, "gap_count") ?? U64(result, "gaps") ?? U64(manifest, "gap_count") ?? 0;
        var loss = U64(result, "lost_samples") ?? U64(result, "data_loss_samples") ?? U64(result, "loss") ?? 0;
        var adcErrors = U64(result, "adc_read_errors") ?? U64(result, "adc_errors") ?? 0;
        var sdErrors = U64(result, "sd_errors") ?? 0;
        if (gapCount > 0 || loss > 0 || adcErrors > 0 || sdErrors > 0) {
            integrity = Max(integrity, RawCorpusIntegrity.ValidWithWarnings);
            messages.Add($"Recorded counters: gaps={gapCount}, loss={loss}, ADC errors={adcErrors}, SD errors={sdErrors}.");
        }

        using var sha = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
        using var logicalRawSha = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
        Add(sha, "KNX-RAW-CORPUS-V1\0"u8);
        foreach (var metadata in (File.Exists(manifestPath) ? new[] { manifestPath } : new[] { startPath, resultPath, chunksPath }).Where(File.Exists)) {
            Add(sha, Encoding.UTF8.GetBytes(Path.GetFileName(metadata))); HashFile(sha, metadata);
        }
        ulong expected = chunks.Count == 0 ? 0 : chunks[0].Start; long rawBytes = 0; var rawUnavailable = false; var files = new Dictionary<string, RawCorpusFile>(StringComparer.OrdinalIgnoreCase);
        var descriptor = new byte[28]; var streamingBuffer = new byte[64 * 1024];
        foreach (var chunk in chunks) {
            if (chunk.Start != expected) { integrity = Max(integrity, RawCorpusIntegrity.Incomplete); messages.Add($"Chunk discontinuity [{expected}, {chunk.Start})."); }
            if (chunk.Bytes != checked((int)chunk.Count * 2)) { integrity = Max(integrity, RawCorpusIntegrity.Corrupted); messages.Add($"Chunk {chunk.Start}: sample/byte count mismatch."); }
            var relative = ResolveRawFile(source, chunk.Segment); var path = Path.Combine(source, relative);
            files[relative] = new(relative, File.Exists(path) ? new FileInfo(path).Length : 0, "raw");
            if (!File.Exists(path)) { integrity = Max(integrity, RawCorpusIntegrity.Incomplete); messages.Add($"Missing RAW file: {relative}."); rawUnavailable = true; expected = chunk.End; continue; }
            try {
                using var stream = File.OpenRead(path);
                if (chunk.Offset < 0 || chunk.Offset + chunk.Bytes > stream.Length) throw new EndOfStreamException();
                stream.Position = chunk.Offset;
                BitConverter.TryWriteBytes(descriptor[..8], chunk.Start); BitConverter.TryWriteBytes(descriptor[8..16], chunk.Count);
                BitConverter.TryWriteBytes(descriptor[16..20], chunk.Segment); BitConverter.TryWriteBytes(descriptor[20..24], chunk.Bytes); BitConverter.TryWriteBytes(descriptor[24..28], chunk.Crc); Add(sha, descriptor);
                var crc = new StreamingCrc32(); var remaining = chunk.Bytes;
                while (remaining > 0) { var read = stream.Read(streamingBuffer, 0, Math.Min(streamingBuffer.Length, remaining)); if (read == 0) throw new EndOfStreamException(); crc.Append(streamingBuffer.AsSpan(0, read)); Add(sha, streamingBuffer.AsSpan(0, read)); Add(logicalRawSha, streamingBuffer.AsSpan(0, read)); remaining -= read; }
                if (crc.Value != chunk.Crc) { integrity = Max(integrity, RawCorpusIntegrity.Corrupted); messages.Add($"Chunk {chunk.Start}: CRC mismatch."); }
            } catch (EndOfStreamException) { integrity = Max(integrity, RawCorpusIntegrity.Incomplete); messages.Add($"Chunk {chunk.Start}: RAW range is incomplete."); rawUnavailable = true; }
            peakMemory = Math.Max(peakMemory, GC.GetTotalMemory(false));
            expected = chunk.End; rawBytes = checked(rawBytes + chunk.Bytes);
        }
        foreach (var metadata in new[] { startPath, resultPath, chunksPath, manifestPath }.Where(File.Exists)) {
            var relative = Path.GetFileName(metadata); files[relative] = new(relative, new FileInfo(metadata).Length, "metadata");
        }
        var digest = Convert.ToHexString(sha.GetHashAndReset());
        var logicalRawDigest = Convert.ToHexString(logicalRawSha.GetHashAndReset());
        var declaredRawDigest = Text(manifest, "logical_raw_sha256") ?? Text(result, "raw_sha256");
        if (!rawUnavailable && declaredRawDigest is not null && !string.Equals(declaredRawDigest, logicalRawDigest, StringComparison.OrdinalIgnoreCase)) {
            integrity = Max(integrity, RawCorpusIntegrity.Corrupted); messages.Add("Logical RAW SHA-256 mismatch.");
        }
        var campaign = Object(start, "field_campaign") ?? Object(manifest, "field_campaign");
        var sampleCount = chunks.Aggregate<Chunk, ulong>(0, (sum, x) => sum + x.Count);
        CompareDeclared(U64(manifest, "total_samples") ?? U64(result, "stored_samples"), sampleCount, "sample count", ref integrity, messages);
        if (!rawUnavailable) CompareDeclared(U64(manifest, "total_raw_bytes") ?? U64(result, "raw_bytes"), checked((ulong)rawBytes), "RAW byte count", ref integrity, messages);
        CompareDeclared(U64(manifest, "chunk_count") ?? U64(result, "chunks"), checked((ulong)chunks.Count), "chunk count", ref integrity, messages);
        if (NullableBool(manifest, "continuity_complete") is false) {
            integrity = Max(integrity, RawCorpusIntegrity.Incomplete); messages.Add("Manifest reports incomplete RAW continuity.");
        }
        var entry = new RawCorpusEntry {
            CorpusId = "raw-" + digest[..24].ToLowerInvariant(), ContentSha256 = digest, LogicalRawSha256 = logicalRawDigest,
            SessionId = Text(start, "session_id") ?? Text(start, "session_uuid") ?? Path.GetFileName(source),
            AcquisitionUtc = Text(start, "start_utc") ?? Text(start, "date_time"), ImportedUtc = DateTimeOffset.UtcNow,
            SourceDirectory = source, DataDirectory = source, StorageMode = RawCorpusStorageMode.Reference,
            AcquisitionMode = mode, State = state, CompletionStatus = completion,
            RequestedDurationSeconds = U64(campaign, "requested_duration_s"),
            AdcDurationMicroseconds = U64(result, "adc_capture_duration_us") ?? U64(result, "duration_us"),
            MeasuredSampleRateHz = Double(result, "sample_rate_measured_hz") ?? Double(start, "sample_rate_hz"),
            SampleCount = sampleCount, SampleFormat = Text(start, "sample_format") ?? Text(manifest, "sample_format") ?? "uint16_le",
            RawBytes = rawBytes, ChunkCount = chunks.Count, GapCount = gapCount, DroppedSamples = loss, AdcErrors = adcErrors, SdErrors = sdErrors,
            FirmwareVersion = Text(start, "firmware") ?? Text(start, "firmware_version") ?? Text(manifest, "firmware") ?? Text(manifest, "firmware_version"),
            AnalyzerId = Text(start, "analyzer_id"),
            Board = Text(start, "board_profile") ?? Text(start, "board") ?? Text(start, "hardware") ?? Text(manifest, "board_profile"),
            Frontend = Text(start, "frontend_profile") ?? Text(start, "frontend") ?? Text(start, "analog_frontend") ?? Text(manifest, "frontend_profile"),
            AcquisitionMetadata = new(Text(campaign, "site") ?? Text(start, "site_label"), Text(campaign, "bus") ?? Text(start, "bus_label"),
                Text(campaign, "point") ?? Text(start, "measurement_point"), Text(campaign, "note") ?? Text(start, "operator_note")),
            Integrity = integrity, IntegrityMessages = messages.ToArray(), Files = files.Values.OrderBy(x => x.RelativePath).ToArray()
        };
        stopwatch.Stop(); peakMemory = Math.Max(peakMemory, GC.GetTotalMemory(false)); var metrics = new RawCorpusImportMetrics(stopwatch.Elapsed, Math.Max(0, peakMemory - memory), streamingBuffer.Length);
        return new(entry, false, metrics);
    }

    private static List<Chunk> ReadChunks(string path)
    {
        var result = new List<Chunk>(); var lineNumber = 0;
        foreach (var line in File.ReadLines(path)) { lineNumber++; if (string.IsNullOrWhiteSpace(line)) continue;
            try { using var doc = JsonDocument.Parse(line); var x = doc.RootElement; var start = RequiredU64(x, "sample_start"); var count = U64(x, "sample_count") ?? checked(RequiredU64(x, "sample_end") - start); var segmented = x.TryGetProperty("segment_index", out _); result.Add(new(start, count, checked((int)(U64(x, "segment_index") ?? 0)), checked((long)(U64(x, segmented ? "segment_offset" : "raw_offset") ?? 0)), checked((int)RequiredU64(x, "raw_bytes")), RequiredCrc(x, "crc32"))); }
            catch (Exception e) when (e is JsonException or FormatException or OverflowException or InvalidOperationException) { throw new InvalidDataException($"Invalid chunks.jsonl line {lineNumber}: {e.Message}", e); }
        }
        return result.OrderBy(x => x.Start).ToList();
    }

    private static string ResolveRawFile(string source, int segment) => File.Exists(Path.Combine(source, "raw.bin")) ? "raw.bin" : $"raw-{segment:D4}.bin";
    private static JsonDocument ParseObject(string path) { try { var doc = JsonDocument.Parse(File.ReadAllText(path)); if (doc.RootElement.ValueKind != JsonValueKind.Object) { doc.Dispose(); throw new InvalidDataException($"{Path.GetFileName(path)} is not a JSON object."); } return doc; } catch (JsonException e) { throw new InvalidDataException($"Invalid {Path.GetFileName(path)}: {e.Message}", e); } }
    private static JsonElement? Object(JsonElement? x, string name) => x is { ValueKind: JsonValueKind.Object } value && value.TryGetProperty(name, out var child) && child.ValueKind == JsonValueKind.Object ? child : null;
    private static string? Text(JsonElement? x, string name) => x is { ValueKind: JsonValueKind.Object } value && value.TryGetProperty(name, out var child) && child.ValueKind == JsonValueKind.String ? child.GetString() : null;
    private static bool Bool(JsonElement? x, string name) => x is { ValueKind: JsonValueKind.Object } value && value.TryGetProperty(name, out var child) && child.ValueKind == JsonValueKind.True;
    private static bool? NullableBool(JsonElement? x, string name) { if (x is not { ValueKind: JsonValueKind.Object } value || !value.TryGetProperty(name, out var child)) return null; return child.ValueKind switch { JsonValueKind.True => true, JsonValueKind.False => false, _ => null }; }
    private static ulong? U64(JsonElement? x, string name) { if (x is not { ValueKind: JsonValueKind.Object } value || !value.TryGetProperty(name, out var child)) return null; if (child.ValueKind == JsonValueKind.Number && child.TryGetUInt64(out var n)) return n; return child.ValueKind == JsonValueKind.String && ulong.TryParse(child.GetString(), out n) ? n : null; }
    private static double? Double(JsonElement? x, string name) { if (x is not { ValueKind: JsonValueKind.Object } value || !value.TryGetProperty(name, out var child)) return null; if (child.ValueKind == JsonValueKind.Number && child.TryGetDouble(out var n)) return n; return child.ValueKind == JsonValueKind.String && double.TryParse(child.GetString(), System.Globalization.CultureInfo.InvariantCulture, out n) ? n : null; }
    private static ulong RequiredU64(JsonElement x, string name) => U64(x, name) ?? throw new InvalidDataException($"Missing/invalid {name}.");
    private static uint RequiredCrc(JsonElement x, string name) { if (!x.TryGetProperty(name, out var child)) throw new InvalidDataException($"Missing {name}."); if (child.ValueKind == JsonValueKind.Number && child.TryGetUInt32(out var n)) return n; if (child.ValueKind == JsonValueKind.String && uint.TryParse(child.GetString(), System.Globalization.NumberStyles.HexNumber, null, out n)) return n; throw new InvalidDataException($"Invalid {name}."); }
    private static RawCorpusIntegrity Max(RawCorpusIntegrity a, RawCorpusIntegrity b) => (RawCorpusIntegrity)Math.Max((int)a, (int)b);
    private static void CompareDeclared(ulong? declared, ulong actual, string name, ref RawCorpusIntegrity integrity, List<string> messages)
    {
        if (declared is null || declared.Value == actual) return;
        integrity = Max(integrity, RawCorpusIntegrity.Corrupted); messages.Add($"Declared {name} {declared.Value} differs from computed {actual}.");
    }
    private static void Add(IncrementalHash hash, ReadOnlySpan<byte> bytes) => hash.AppendData(bytes);
    private static void HashFile(IncrementalHash hash, string path) { using var stream = File.OpenRead(path); var buffer = new byte[64 * 1024]; int read; while ((read = stream.Read(buffer)) > 0) Add(hash, buffer.AsSpan(0, read)); }

    private sealed class StreamingCrc32
    {
        private uint crc = 0xFFFFFFFF;
        public void Append(ReadOnlySpan<byte> bytes) { foreach (var b in bytes) { crc ^= b; for (var i = 0; i < 8; i++) crc = (crc >> 1) ^ ((crc & 1) != 0 ? 0xEDB88320u : 0u); } }
        public uint Value => crc ^ 0xFFFFFFFF;
    }
}
