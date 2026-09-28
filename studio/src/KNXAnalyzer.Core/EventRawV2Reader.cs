using System.Text.Json;

namespace KNXAnalyzer.Core;

public static class EventRawV2Reader
{
    public const uint DefaultSampleRateHz = 83333;

    public static bool IsDataset(string directory) =>
        (File.Exists(Path.Combine(directory, "raw.bin")) || File.Exists(Path.Combine(directory, "segments.jsonl"))) &&
        File.Exists(Path.Combine(directory, "chunks.jsonl")) &&
        File.Exists(Path.Combine(directory, "events.jsonl"));

    public static Session OpenSession(string directory)
    {
        var full = Path.GetFullPath(directory);
        var metadataPath = Path.Combine(full, "test-result.json");
        var startPath = Path.Combine(full, "session-start.json");
        using var emptyMetadata = JsonDocument.Parse("{}");
        JsonElement metadata = emptyMetadata.RootElement.Clone();
        JsonElement startMetadata = emptyMetadata.RootElement.Clone();
        var state = "UNKNOWN";
        var id = Path.GetFileName(full);
        var analyzer = "LOCAL / SD";
        string? dateTime = null;
        if (File.Exists(startPath)) {
            using var startDoc = JsonDocument.Parse(File.ReadAllText(startPath));
            var start = startDoc.RootElement;
            startMetadata = start.Clone();
            id = String(start, "session_id") ?? String(start, "session_uuid") ?? id;
            analyzer = String(start, "analyzer_id") is { } analyzerId ? $"LOCAL / SD · {analyzerId}" : analyzer;
            dateTime = String(start, "start_utc") ?? String(start, "date_time");
        }
        long? durationMs = null;
        if (File.Exists(metadataPath)) {
            using var doc = JsonDocument.Parse(File.ReadAllText(metadataPath));
            metadata = doc.RootElement.Clone();
            state = String(metadata, "lifecycle") ??
                (metadata.TryGetProperty("closed", out var closed) && closed.ValueKind == JsonValueKind.True ? "CLOSED" : "INTERRUPTED");
            if (metadata.TryGetProperty("duration_us", out var duration)) {
                if (duration.ValueKind == JsonValueKind.Number && duration.TryGetInt64(out var us)) durationMs = us / 1000;
                else if (duration.ValueKind == JsonValueKind.String && long.TryParse(duration.GetString(), out us)) durationMs = us / 1000;
            }
        }
        var session = new Session {
            DirectoryPath = full, Id = id, State = state, DateTime = dateTime,
            DurationMs = durationMs, Metadata = metadata, StartMetadata = startMetadata, Analyzer = analyzer,
            RawAvailableBytes = ReadRawByteCount(full)
        };
        session.SampleRateHz = checked((uint)(U64Optional(startMetadata, "sample_rate_hz") ?? DefaultSampleRateHz));
        session.DetectionThreshold = checked((uint)(U64Optional(startMetadata, "d44_threshold") ?? 0));
        session.PreTriggerSamples = checked((uint)(U64Optional(startMetadata, "pre_samples") ?? 0));
        session.PostTriggerSamples = checked((uint)(U64Optional(startMetadata, "post_samples") ?? 0));
        session.RawChunks.AddRange(ReadChunks(full));
        var line = 0;
        foreach (var text in File.ReadLines(Path.Combine(full, "events.jsonl"))) {
            line++;
            if (string.IsNullOrWhiteSpace(text)) continue;
            using var doc = JsonDocument.Parse(text);
            var item = doc.RootElement;
            var idValue = item.GetProperty("event_id");
            var eventId = idValue.ValueKind == JsonValueKind.String ? uint.Parse(idValue.GetString()!) : idValue.GetUInt32();
            var analog = new AnalogEvent {
                EventId = eventId, RawPersisted = true, EventRawV2 = true, Original = item.Clone()
            };
            analog.RawDescriptor = Descriptor(session, analog);
            if (File.Exists(Path.Combine(full, "raw.bin"))) try { analog.CaptureSummary = ReadCapture(full, eventId).Summary; }
            catch (Exception e) when (e is IOException or InvalidDataException or JsonException) { session.Diagnostics.Add(new Diagnostic(Path.Combine(full, "raw.bin"), line, e.Message)); }
            session.AnalogEvents.Add(analog);
        }
        session.EventCount = session.AnalogEvents.Count;
        return session;
    }

    private static long? ReadRawByteCount(string directory)
    {
        var chunksPath = Path.Combine(directory, "chunks.jsonl");
        if (!File.Exists(chunksPath)) return null;
        long total = 0;
        foreach (var line in File.ReadLines(chunksPath)) {
            if (string.IsNullOrWhiteSpace(line)) continue;
            using var doc = JsonDocument.Parse(line);
            if (doc.RootElement.TryGetProperty("raw_bytes", out var value) && value.TryGetInt64(out var bytes)) total = checked(total + bytes);
        }
        return total;
    }

    private static string? String(JsonElement element, string name) =>
        element.TryGetProperty(name, out var value) && value.ValueKind == JsonValueKind.String ? value.GetString() : null;

    public static RawCapture ReadCapture(string directory, uint eventId)
    {
        var session = OpenSession(directory);
        var analog = session.AnalogEvents.SingleOrDefault(x => x.EventId == eventId)
            ?? throw new InvalidDataException($"Event {eventId} was not found.");
        var descriptor = analog.RawDescriptor ?? throw new InvalidDataException($"Event {eventId} has no RAW descriptor.");
        var segmented = !File.Exists(Path.Combine(directory, "raw.bin"));
        return EventRawAssembler.Assemble(descriptor, chunk => {
            var rawPath = segmented ? Path.Combine(directory, $"raw-{chunk.SegmentIndex:D4}.bin") : Path.Combine(directory, "raw.bin");
            using var raw = File.OpenRead(rawPath);
            raw.Position = chunk.SegmentOffset;
            var bytes = new byte[chunk.RawBytes];
            raw.ReadExactly(bytes);
            return bytes;
        });
    }

    public static EventRawDescriptor Descriptor(Session session, AnalogEvent analog)
    {
        var start = analog.SampleStart ?? throw new InvalidDataException($"Event {analog.EventId} has no sample_start.");
        var end = analog.SampleEnd ?? throw new InvalidDataException($"Event {analog.EventId} has no sample_end.");
        var trigger = analog.SampleTrigger ?? throw new InvalidDataException($"Event {analog.EventId} has no trigger sample.");
        var chunks = session.RawChunks.Where(x => x.SampleStart < end && x.SampleEnd > start).OrderBy(x => x.SampleStart).ToArray();
        EventRawChunk? history = chunks.Length > 0 && chunks[0].SampleStart < start ? null : session.RawChunks
            .Where(x => x.SampleEnd == start).OrderByDescending(x => x.SampleStart).FirstOrDefault();
        return new(analog.EventId, start, trigger, end, session.SampleRateHz, session.DetectionThreshold,
            session.PreTriggerSamples, session.PostTriggerSamples, chunks, history);
    }

    public static IReadOnlyList<EventRawChunk> ReadChunks(string directory)
    {
        var segmented = !File.Exists(Path.Combine(directory, "raw.bin"));
        return File.ReadLines(Path.Combine(directory, "chunks.jsonl"))
            .Where(line => !string.IsNullOrWhiteSpace(line)).Select(line => {
                using var doc = JsonDocument.Parse(line); var root = doc.RootElement;
                var start = U64(root, "sample_start");
                var end = root.TryGetProperty("sample_end", out _) ? U64(root, "sample_end") :
                    start + U64(root, "sample_count");
                var offset = segmented ? checked((long)U64(root, "segment_offset")) : checked((long)U64(root, "raw_offset"));
                var segment = root.TryGetProperty("segment_index", out _) ? checked((int)U64(root, "segment_index")) : 0;
                var crc = root.GetProperty("crc32");
                return new EventRawChunk(start, end, segment, offset, root.GetProperty("raw_bytes").GetInt32(),
                    crc.ValueKind == JsonValueKind.String ? Convert.ToUInt32(crc.GetString(), 16) : crc.GetUInt32());
            }).OrderBy(x => x.SampleStart).ToArray();
    }

    private static ulong U64(JsonElement element, string name)
    {
        var value = element.GetProperty(name);
        return value.ValueKind == JsonValueKind.String ? ulong.Parse(value.GetString()!) : value.GetUInt64();
    }

    private static ulong U64Either(JsonElement element, string first, string second) =>
        element.TryGetProperty(first, out _) ? U64(element, first) : U64(element, second);
    private static ulong? U64Optional(JsonElement element, string name) =>
        element.ValueKind == JsonValueKind.Object && element.TryGetProperty(name, out _) ? U64(element, name) : null;
}
