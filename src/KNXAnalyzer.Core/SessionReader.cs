using System.Buffers.Binary;
using System.Text.Json;

namespace KNXAnalyzer.Core;

public sealed record Diagnostic(string File, int Line, string Message);

public sealed class Session
{
    public required string DirectoryPath { get; init; }
    public required string Id { get; init; }
    public required string State { get; init; }
    public string? DateTime { get; init; }
    public long? DurationMs { get; init; }
    public int EventCount { get; init; }
    public JsonElement Metadata { get; init; }
    public List<Tp1Candidate> Candidates { get; } = [];
    public List<AnalogEvent> AnalogEvents { get; } = [];
    public List<Diagnostic> Diagnostics { get; } = [];
    public string Generation => Candidates.Count > 0 ? "TP1 journal (exact build unrecorded)"
        : File.Exists(Path.Combine(DirectoryPath, "tp1-candidates.jsonl")) ? "TP1 journal empty"
        : AnalogEvents.Count > 0 ? "Sessions / Scope"
        : "Session metadata only";
    public int Count(string classification) => Candidates.Count(x => x.Classification == classification);
}

public sealed class Tp1Candidate
{
    public required int Line { get; init; }
    public required string Classification { get; init; }
    public long? MonotonicUs { get; init; }
    public string? DateTime { get; init; }
    public required string RawHex { get; init; }
    public byte[] RawBytes { get; init; } = [];
    public string? Source { get; init; }
    public string? Destination { get; init; }
    public string? DestinationType { get; init; }
    public bool GenericFieldsFromRaw { get; init; }
    public int? HopCount { get; init; }
    public int? TpLength { get; init; }
    public int ParityErrors { get; init; }
    public int TimingErrors { get; init; }
    public bool Overflow { get; init; }
    public JsonElement Original { get; init; }
    public bool IsAck => RawBytes.Length == 1 && RawBytes[0] is 0xCC or 0x0C or 0xC0;
    public string Ack => RawBytes.Length == 1 ? RawBytes[0] switch { 0xCC => "ACK", 0x0C => "NAK", 0xC0 => "BUSY", _ => "" } : "";
    public string Checksum => RawBytes.Length >= 8 ? RawBytes.Aggregate((byte)0, (a, b) => (byte)(a ^ b)) == 0xFF ? "OK" : "Invalid" : "—";
    public bool IsValid => Classification is "VALID_KNOWN" or "VALID_UNKNOWN";
    public bool IsError => Classification.StartsWith("INVALID_", StringComparison.Ordinal) || Classification == "INCOMPLETE";
    public string Time => DateTime ?? (MonotonicUs is long us ? $"{us / 1_000_000.0:F6} s uptime" : "—");
    public string Service => IsAck ? Ack : "—";
}

public sealed class AnalogEvent
{
    public uint EventId { get; init; }
    public bool RawPersisted { get; init; }
    public JsonElement Original { get; init; }
    public RawCaptureSummary? CaptureSummary { get; set; }
    public string MinimumText => CaptureSummary?.Minimum.ToString() ?? "—";
    public string MaximumText => CaptureSummary?.Maximum.ToString() ?? "—";
    public string PeakToPeakText => CaptureSummary?.PeakToPeak.ToString() ?? "—";
    public string CrcText => CaptureSummary is null ? "Unavailable" : CaptureSummary.CrcValid ? "OK" : "INVALID";
}

public sealed record RawCaptureSummary(int SampleCount, uint SampleRateHz, ushort Minimum, ushort Maximum, int PeakToPeak, double Mean, bool CrcValid);

public static class SessionReader
{
    public static IReadOnlyList<Session> OpenFolder(string folder)
    {
        var root = Path.GetFullPath(folder);
        var candidates = new[] { Path.Combine(root, "knx-analyzer", "sessions"), Path.Combine(root, "sessions"), root };
        var sessionsRoot = candidates.FirstOrDefault(Directory.Exists) ?? throw new DirectoryNotFoundException(root);
        if (File.Exists(Path.Combine(sessionsRoot, "session.json"))) return [OpenSession(sessionsRoot)];
        return Directory.EnumerateDirectories(sessionsRoot)
            .Where(d => File.Exists(Path.Combine(d, "session.json")))
            .Select(OpenSession).OrderBy(s => s.Id).ToArray();
    }

    public static Session OpenSession(string directory)
    {
        var path = Path.Combine(directory, "session.json");
        using var doc = JsonDocument.Parse(File.ReadAllText(path));
        var root = doc.RootElement;
        var session = new Session {
            DirectoryPath = Path.GetFullPath(directory), Id = String(root, "session_id") ?? Path.GetFileName(directory),
            State = String(root, "state") ?? "UNKNOWN", DateTime = String(root, "date_time"),
            DurationMs = Long(root, "duration_ms"), EventCount = (int)(Long(root, "event_count") ?? 0), Metadata = root.Clone()
        };
        if (Long(root, "capture_format") is > 1) session.Diagnostics.Add(new Diagnostic(path, 0, "Future capture format; RAW will be inspected conservatively."));
        ReadJsonl(Path.Combine(directory, "tp1-candidates.jsonl"), session, (line, item) => {
            var hex = String(item, "raw_hex") ?? "";
            byte[] bytes;
            try { bytes = Convert.FromHexString(hex); }
            catch (FormatException) { bytes = []; session.Diagnostics.Add(new Diagnostic(Path.Combine(directory, "tp1-candidates.jsonl"), line, "Invalid raw_hex; original text retained")); }
            var classification = String(item, "classification") ?? "UNKNOWN";
            var recordedSource = String(item, "source");
            var recordedDestination = String(item, "destination");
            var decoded = classification == "VALID_UNKNOWN" ? DecodeStandard(bytes) : null;
            session.Candidates.Add(new Tp1Candidate {
                Line = line, Classification = classification, MonotonicUs = Long(item, "monotonic_us"),
                DateTime = String(item, "date_time"), RawHex = hex, RawBytes = bytes,
                Source = recordedSource ?? decoded?.Source, Destination = recordedDestination ?? decoded?.Destination, DestinationType = String(item, "destination_type") ?? decoded?.DestinationType,
                GenericFieldsFromRaw = decoded is not null && (recordedSource is null || recordedDestination is null),
                HopCount = (int?)Long(item, "hop_count") ?? decoded?.HopCount, TpLength = (int?)Long(item, "tp_length") ?? decoded?.TpLength,
                ParityErrors = (int)(Long(item, "parity_errors") ?? 0), TimingErrors = (int)(Long(item, "timing_errors") ?? 0),
                Overflow = Bool(item, "overflow"), Original = item.Clone()
            });
        });
        ReadJsonl(Path.Combine(directory, "events.jsonl"), session, (line, item) => {
            var analog = new AnalogEvent {
                EventId = (uint)(Long(item, "event_id") ?? 0), RawPersisted = Bool(item, "raw_persisted"), Original = item.Clone()
            };
            if (analog.RawPersisted) {
                var rawPath = Path.Combine(directory, "captures", $"event-{analog.EventId:D6}.bin");
                try {
                    analog.CaptureSummary = RawCapture.Read(rawPath).Summary;
                } catch (Exception e) when (e is IOException or UnauthorizedAccessException or NotSupportedException) {
                    session.Diagnostics.Add(new Diagnostic(rawPath, line, e.Message));
                }
            }
            session.AnalogEvents.Add(analog);
        });
        return session;
    }

    private sealed record GenericFields(string Source, string Destination, string DestinationType, int HopCount, int TpLength);
    private static GenericFields? DecodeStandard(byte[] bytes)
    {
        if (bytes.Length < 8 || (bytes[0] & 0x80) == 0 || bytes.Length != 8 + (bytes[5] & 0x0F)) return null;
        byte xor = 0;
        foreach (var b in bytes) xor ^= b;
        if (xor != 0xFF) return null;
        var src = (bytes[1] << 8) | bytes[2];
        var dst = (bytes[3] << 8) | bytes[4];
        var source = $"{(src >> 12) & 15}.{(src >> 8) & 15}.{src & 255}";
        var group = (bytes[5] & 0x80) != 0;
        var destination = group
            ? $"{(dst >> 11) & 31}/{(dst >> 8) & 7}/{dst & 255}"
            : $"{(dst >> 12) & 15}.{(dst >> 8) & 15}.{dst & 255}";
        return new GenericFields(source, destination, group ? "group" : "individual", (bytes[5] >> 4) & 7, bytes[5] & 15);
    }
    private static void ReadJsonl(string path, Session session, Action<int, JsonElement> add)
    {
        if (!File.Exists(path)) return;
        var line = 0;
        foreach (var text in File.ReadLines(path)) {
            line++;
            if (string.IsNullOrWhiteSpace(text)) continue;
            try { using var doc = JsonDocument.Parse(text); add(line, doc.RootElement); }
            catch (Exception e) when (e is JsonException or FormatException or InvalidOperationException or OverflowException) {
                session.Diagnostics.Add(new Diagnostic(path, line, e.Message));
            }
        }
    }
    private static string? String(JsonElement e, string name) => e.TryGetProperty(name, out var v) && v.ValueKind == JsonValueKind.String ? v.GetString() : null;
    private static long? Long(JsonElement e, string name) => e.TryGetProperty(name, out var v) && v.ValueKind == JsonValueKind.Number && v.TryGetInt64(out var n) ? n : null;
    private static bool Bool(JsonElement e, string name) => e.TryGetProperty(name, out var v) && v.ValueKind == JsonValueKind.True;
}

public sealed class RawCapture
{
    public uint EventId { get; init; }
    public uint SampleRateHz { get; init; }
    public uint TriggerIndex { get; init; }
    public ushort Minimum { get; init; }
    public ushort Maximum { get; init; }
    public ushort[] Samples { get; init; } = [];
    public bool CrcValid { get; init; }
    public double Mean { get; init; }
    public int PeakToPeak => Maximum - Minimum;
    public RawCaptureSummary Summary => new(Samples.Length, SampleRateHz, Minimum, Maximum, PeakToPeak, Mean, CrcValid);

    public static RawCapture Read(string path)
    {
        var bytes = File.ReadAllBytes(path);
        if (bytes.Length < 48 || !bytes.AsSpan(0, 8).SequenceEqual("KNXADC1\0"u8)) throw new InvalidDataException("Invalid RAW magic or header");
        ushort u16(int offset) => BinaryPrimitives.ReadUInt16LittleEndian(bytes.AsSpan(offset));
        uint u32(int offset) => BinaryPrimitives.ReadUInt32LittleEndian(bytes.AsSpan(offset));
        if (u16(8) != 1 || u16(10) != 48) throw new NotSupportedException("Unsupported RAW capture version");
        var count = u32(20);
        if (count > 10_000_000 || bytes.Length != 48L + count * 2) throw new InvalidDataException("RAW sample count or file length mismatch");
        var samples = new ushort[count];
        ushort minimum = ushort.MaxValue, maximum = 0;
        ulong sum = 0;
        for (var i = 0; i < samples.Length; i++) {
            var sample = u16(48 + i * 2); samples[i] = sample;
            minimum = Math.Min(minimum, sample); maximum = Math.Max(maximum, sample); sum += sample;
        }
        if (samples.Length == 0) { minimum = 0; maximum = 0; }
        return new RawCapture { EventId = u32(12), SampleRateHz = u32(16), TriggerIndex = u32(24), Minimum = minimum, Maximum = maximum, Mean = samples.Length == 0 ? 0 : (double)sum / samples.Length, Samples = samples,
            CrcValid = Crc32(bytes.AsSpan(48)) == u32(40) };
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
