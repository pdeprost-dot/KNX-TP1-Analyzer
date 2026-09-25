using System.Buffers.Binary;
using System.Text.Json;

namespace KNXAnalyzer.Core;

public static class EventRawV2Reader
{
    public const uint SampleRateHz = 83333;

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
        var state = "UNKNOWN";
        var id = Path.GetFileName(full);
        var analyzer = "LOCAL / SD";
        string? dateTime = null;
        if (File.Exists(startPath)) {
            using var startDoc = JsonDocument.Parse(File.ReadAllText(startPath));
            var start = startDoc.RootElement;
            id = String(start, "session_id") ?? String(start, "session_uuid") ?? id;
            analyzer = String(start, "analyzer_id") is { } analyzerId ? $"LOCAL / SD · {analyzerId}" : analyzer;
            dateTime = String(start, "date_time");
        }
        long? durationMs = null;
        if (File.Exists(metadataPath)) {
            using var doc = JsonDocument.Parse(File.ReadAllText(metadataPath));
            metadata = doc.RootElement.Clone();
            state = metadata.TryGetProperty("closed", out var closed) && closed.ValueKind == JsonValueKind.True ? "CLOSED" : "INTERRUPTED";
            if (metadata.TryGetProperty("duration_us", out var duration)) {
                if (duration.ValueKind == JsonValueKind.Number && duration.TryGetInt64(out var us)) durationMs = us / 1000;
                else if (duration.ValueKind == JsonValueKind.String && long.TryParse(duration.GetString(), out us)) durationMs = us / 1000;
            }
        }
        var session = new Session {
            DirectoryPath = full, Id = id, State = state, DateTime = dateTime,
            DurationMs = durationMs, Metadata = metadata, Analyzer = analyzer,
            RawAvailableBytes = ReadRawByteCount(full)
        };
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
        JsonElement item = default;
        foreach (var line in File.ReadLines(Path.Combine(directory, "events.jsonl"))) {
            using var candidate = JsonDocument.Parse(line);
            if (U64(candidate.RootElement, "event_id") == eventId) {
                item = candidate.RootElement.Clone();
                break;
            }
        }
        if (item.ValueKind == JsonValueKind.Undefined) throw new InvalidDataException($"Event ${eventId} was not found.");
        var eventStart = U64(item, "sample_start");
        var eventEnd = U64(item, "sample_end");
        var trigger = U64(item, "sample_trigger");
        var segmented = !File.Exists(Path.Combine(directory, "raw.bin"));
        var chunks = File.ReadLines(Path.Combine(directory, "chunks.jsonl"))
            .Where(line => !string.IsNullOrWhiteSpace(line))
            .Select(line => {
                using var doc = JsonDocument.Parse(line);
                var root = doc.RootElement;
                var start = U64(root, "sample_start");
                var count = root.TryGetProperty("sample_count", out var countValue) ? checked((uint)U64(root, "sample_count")) : checked((uint)(U64(root, "sample_end") - start));
                var offset = segmented ? checked((long)U64(root, "segment_offset")) : root.GetProperty("raw_offset").GetInt64();
                var segment = root.TryGetProperty("segment_index", out _) ? checked((int)U64(root, "segment_index")) : 0;
                return new Chunk(start, count, segment, offset, root.GetProperty("raw_bytes").GetInt32(),
                    Convert.ToUInt32(root.GetProperty("crc32").GetString(), 16));
            })
            .Where(x => x.SampleStart < eventEnd && x.SampleStart + x.SampleCount > eventStart)
            .OrderBy(x => x.SampleStart).ToArray();
        var samples = new List<ushort>(checked((int)(eventEnd - eventStart)));
        var expected = eventStart;
        var crcValid = true;
        foreach (var chunk in chunks) {
            var rawPath = segmented ? Path.Combine(directory, $"raw-{chunk.Segment:D4}.bin") : Path.Combine(directory, "raw.bin");
            using var raw = File.OpenRead(rawPath);
            raw.Position = chunk.RawOffset;
            var bytes = new byte[chunk.RawBytes];
            raw.ReadExactly(bytes);
            crcValid &= Crc32(bytes) == chunk.Crc32;
            var from = Math.Max(eventStart, chunk.SampleStart);
            var to = Math.Min(eventEnd, chunk.SampleStart + chunk.SampleCount);
            if (from > expected) throw new InvalidDataException($"RAW gap before sample {from}.");
            from = Math.Max(from, expected);
            for (var sample = from; sample < to; sample++) {
                var offset = checked((int)((sample - chunk.SampleStart) * 2));
                samples.Add(BinaryPrimitives.ReadUInt16LittleEndian(bytes.AsSpan(offset, 2)));
            }
            expected = Math.Max(expected, to);
        }
        if (expected < eventEnd) throw new InvalidDataException($"RAW ends at sample {expected}, expected {eventEnd}.");
        var array = samples.ToArray();
        return new RawCapture {
            EventId = eventId, SampleRateHz = SampleRateHz,
            TriggerIndex = checked((uint)(trigger - eventStart)), Samples = array,
            Minimum = array.Length == 0 ? (ushort)0 : array.Min(),
            Maximum = array.Length == 0 ? (ushort)0 : array.Max(),
            Mean = array.Length == 0 ? 0 : array.Average(x => (double)x),
            CrcValid = crcValid
        };
    }

    private sealed record Chunk(ulong SampleStart, uint SampleCount, int Segment, long RawOffset, int RawBytes, uint Crc32);

    private static ulong U64(JsonElement element, string name)
    {
        var value = element.GetProperty(name);
        return value.ValueKind == JsonValueKind.String ? ulong.Parse(value.GetString()!) : value.GetUInt64();
    }

    private static uint Crc32(ReadOnlySpan<byte> data)
    {
        uint crc = 0xFFFFFFFF;
        foreach (var b in data) {
            crc ^= b;
            for (var i = 0; i < 8; i++) crc = (crc >> 1) ^ ((crc & 1) != 0 ? 0xEDB88320u : 0u);
        }
        return crc ^ 0xFFFFFFFF;
    }
}
