using System.Buffers.Binary;
using System.Text.Json;

namespace KNXAnalyzer.Core;

public static class EventRawV2Reader
{
    public const uint SampleRateHz = 83333;

    public static bool IsDataset(string directory) =>
        File.Exists(Path.Combine(directory, "raw.bin")) &&
        File.Exists(Path.Combine(directory, "chunks.jsonl")) &&
        File.Exists(Path.Combine(directory, "events.jsonl"));

    public static Session OpenSession(string directory)
    {
        var full = Path.GetFullPath(directory);
        var metadataPath = Path.Combine(full, "test-result.json");
        using var emptyMetadata = JsonDocument.Parse("{}");
        JsonElement metadata = emptyMetadata.RootElement.Clone();
        var state = "UNKNOWN";
        long? durationMs = null;
        if (File.Exists(metadataPath)) {
            using var doc = JsonDocument.Parse(File.ReadAllText(metadataPath));
            metadata = doc.RootElement.Clone();
            state = metadata.TryGetProperty("closed", out var closed) && closed.ValueKind == JsonValueKind.True ? "CLOSED" : "INTERRUPTED";
            if (metadata.TryGetProperty("duration_us", out var duration) && duration.TryGetInt64(out var us)) durationMs = us / 1000;
        }
        var session = new Session {
            DirectoryPath = full, Id = Path.GetFileName(full), State = state,
            DurationMs = durationMs, Metadata = metadata
        };
        var line = 0;
        foreach (var text in File.ReadLines(Path.Combine(full, "events.jsonl"))) {
            line++;
            if (string.IsNullOrWhiteSpace(text)) continue;
            using var doc = JsonDocument.Parse(text);
            var item = doc.RootElement;
            var eventId = item.GetProperty("event_id").GetUInt32();
            var analog = new AnalogEvent {
                EventId = eventId, RawPersisted = true, EventRawV2 = true, Original = item.Clone()
            };
            try { analog.CaptureSummary = ReadCapture(full, eventId).Summary; }
            catch (Exception e) when (e is IOException or InvalidDataException or JsonException) {
                session.Diagnostics.Add(new Diagnostic(Path.Combine(full, "raw.bin"), line, e.Message));
            }
            session.AnalogEvents.Add(analog);
        }
        return session;
    }

    public static RawCapture ReadCapture(string directory, uint eventId)
    {
        JsonElement item = default;
        foreach (var line in File.ReadLines(Path.Combine(directory, "events.jsonl"))) {
            using var candidate = JsonDocument.Parse(line);
            if (candidate.RootElement.GetProperty("event_id").GetUInt32() == eventId) {
                item = candidate.RootElement.Clone();
                break;
            }
        }
        if (item.ValueKind == JsonValueKind.Undefined) throw new InvalidDataException($"Event ${eventId} was not found.");
        var eventStart = item.GetProperty("sample_start").GetUInt64();
        var eventEnd = item.GetProperty("sample_end").GetUInt64();
        var trigger = item.GetProperty("sample_trigger").GetUInt64();
        var chunks = File.ReadLines(Path.Combine(directory, "chunks.jsonl"))
            .Where(line => !string.IsNullOrWhiteSpace(line))
            .Select(line => {
                using var doc = JsonDocument.Parse(line);
                var root = doc.RootElement;
                return new Chunk(root.GetProperty("sample_start").GetUInt64(), root.GetProperty("sample_count").GetUInt32(),
                    root.GetProperty("raw_offset").GetInt64(), root.GetProperty("raw_bytes").GetInt32(),
                    Convert.ToUInt32(root.GetProperty("crc32").GetString(), 16));
            })
            .Where(x => x.SampleStart < eventEnd && x.SampleStart + x.SampleCount > eventStart)
            .OrderBy(x => x.SampleStart).ToArray();
        var samples = new List<ushort>(checked((int)(eventEnd - eventStart)));
        var expected = eventStart;
        var crcValid = true;
        using var raw = File.OpenRead(Path.Combine(directory, "raw.bin"));
        foreach (var chunk in chunks) {
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

    private sealed record Chunk(ulong SampleStart, uint SampleCount, long RawOffset, int RawBytes, uint Crc32);

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
