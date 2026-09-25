using System.Buffers.Binary;
using System.Net;
using System.Net.Http.Headers;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace KNXAnalyzer.Core;

public sealed record AnalyzerInfo(
    [property: JsonPropertyName("analyzer_id")] string AnalyzerId,
    [property: JsonPropertyName("hostname")] string Hostname,
    [property: JsonPropertyName("firmware_version")] string FirmwareVersion,
    [property: JsonPropertyName("state")] string State,
    [property: JsonPropertyName("ip")] string Ip,
    [property: JsonPropertyName("rssi")] int Rssi,
    [property: JsonPropertyName("sd_ready")] bool SdReady,
    [property: JsonPropertyName("ota_available")] bool OtaAvailable);

public sealed record NetworkSessionInfo(
    [property: JsonPropertyName("folder")] string Folder,
    [property: JsonPropertyName("session_id")] string SessionId,
    [property: JsonPropertyName("state")] string State,
    [property: JsonPropertyName("schema_version")] string SchemaVersion,
    [property: JsonPropertyName("events")] int? Events,
    [property: JsonPropertyName("valid_raw_bytes")] string RawBytes,
    [property: JsonPropertyName("duration_us")] string DurationUs);

public sealed record NetworkSessionContext(Uri BaseUri, string AnalyzerId, string SessionUuid, string Folder, string CacheDirectory);
public sealed record NetworkImportProgress(string Stage, long Downloaded = 0, long? Total = null, bool FromCache = false);

public sealed class NetworkImportService : IDisposable
{
    public static string RememberedAnalyzer {
        get { var path=Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),"KNXAnalyzerStudio","Cache","known-analyzer.txt"); return File.Exists(path)?File.ReadAllText(path).Trim():"knx-analyzer-ebac.local"; }
    }
    private readonly HttpClient _http;
    private readonly string _cacheRoot;
    public AnalyzerInfo? Analyzer { get; private set; }
    public Uri BaseUri { get; }

    public NetworkImportService(string hostOrUrl, string? cacheRoot = null, HttpMessageHandler? handler = null)
    {
        var value = hostOrUrl.Trim();
        if (!value.Contains("://", StringComparison.Ordinal)) value = "http://" + value;
        BaseUri = new Uri(value.TrimEnd('/') + "/", UriKind.Absolute);
        _http = handler is null ? new HttpClient() : new HttpClient(handler);
        _http.BaseAddress = BaseUri;
        _http.Timeout = TimeSpan.FromSeconds(20);
        _cacheRoot = cacheRoot ?? Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "KNXAnalyzerStudio", "Cache");
    }

    public async Task<AnalyzerInfo> ConnectAsync(CancellationToken ct = default)
    {
        Analyzer = await GetJsonAsync<AnalyzerInfo>("api/analyzer", ct);
        RememberAnalyzer(Analyzer);
        return Analyzer;
    }

    public async Task<IReadOnlyList<NetworkSessionInfo>> ListSessionsAsync(CancellationToken ct = default)
    {
        using var doc = await GetDocumentAsync("api/sessions", ct);
        return doc.RootElement.GetProperty("sessions").Deserialize<NetworkSessionInfo[]>() ?? [];
    }

    public async Task<Session> ImportMetadataAsync(NetworkSessionInfo info, IProgress<NetworkImportProgress>? progress = null, CancellationToken ct = default)
    {
        if (Analyzer is null) await ConnectAsync(ct);
        progress?.Report(new("metadata"));
        using var manifest = await GetDocumentAsync($"api/sessions/{Uri.EscapeDataString(info.Folder)}/manifest", ct);
        var sessionUuid = string.IsNullOrWhiteSpace(info.SessionId) ? info.Folder : info.SessionId;
        var cache = Path.Combine(_cacheRoot, SafePart(Analyzer!.AnalyzerId), SafePart(sessionUuid));
        Directory.CreateDirectory(cache);
        await WriteJsonAsync(Path.Combine(cache, "session-start.json"), manifest.RootElement.GetProperty("session_start"), ct);
        if (manifest.RootElement.TryGetProperty("test_result", out var result) && result.ValueKind == JsonValueKind.Object)
            await WriteJsonAsync(Path.Combine(cache, "test-result.json"), result, ct);
        var files = manifest.RootElement.GetProperty("files");
        foreach (var name in new[] { "segments.jsonl", "chunks.jsonl", "chunk-index.jsonl", "events.jsonl" }) {
            var descriptor = files.GetProperty(name);
            if (!descriptor.GetProperty("present").GetBoolean()) continue;
            var expected = long.Parse(descriptor.GetProperty("bytes").GetString()!);
            var target = Path.Combine(cache, name);
            if (File.Exists(target) && new FileInfo(target).Length == expected) { progress?.Report(new("cache", expected, expected, true)); continue; }
            await DownloadSmallAsync(descriptor.GetProperty("url").GetString()!, target, expected, ct);
            progress?.Report(new("downloaded", expected, expected));
        }
        var session = EventRawV2Reader.OpenSession(cache);
        session.Network = new(BaseUri, Analyzer.AnalyzerId, sessionUuid, info.Folder, cache);
        session.Analyzer = $"{Analyzer.Hostname} · {Analyzer.AnalyzerId}";
        if (long.TryParse(info.RawBytes, out var rawBytes)) session.RawAvailableBytes = rawBytes;
        return session;
    }

    public async Task<RawCapture> FetchEventAsync(Session session, uint eventId, IProgress<NetworkImportProgress>? progress = null, CancellationToken ct = default)
    {
        var context = session.Network ?? throw new InvalidOperationException("Session is not network-backed.");
        var eventJson = File.ReadLines(Path.Combine(context.CacheDirectory, "events.jsonl")).Select(x => JsonDocument.Parse(x))
            .FirstOrDefault(x => U64(x.RootElement, "event_id") == eventId)
            ?? throw new InvalidDataException($"Event {eventId} was not found.");
        using (eventJson) {
            var item = eventJson.RootElement; ulong eventStart = U64(item, "sample_start"); ulong eventEnd = U64(item, "sample_end"); ulong trigger = U64(item, "sample_trigger");
            var chunks = File.ReadLines(Path.Combine(context.CacheDirectory, "chunks.jsonl")).Where(x => !string.IsNullOrWhiteSpace(x)).Select(ParseChunk)
                .Where(x => x.SampleStart < eventEnd && x.SampleEnd > eventStart).OrderBy(x => x.SampleStart).ToArray();
            var samples = new List<ushort>(checked((int)(eventEnd - eventStart))); ulong expected = eventStart; bool crcValid = true;
            foreach (var chunk in chunks) {
                var bytes = await GetChunkAsync(context, chunk, progress, ct); crcValid &= Crc32(bytes) == chunk.Crc32;
                var from = Math.Max(eventStart, chunk.SampleStart); var to = Math.Min(eventEnd, chunk.SampleEnd);
                if (from > expected) throw new InvalidDataException($"RAW gap before sample {from}."); from = Math.Max(from, expected);
                for (var sample = from; sample < to; ++sample) samples.Add(BinaryPrimitives.ReadUInt16LittleEndian(bytes.AsSpan(checked((int)((sample - chunk.SampleStart) * 2)), 2)));
                expected = Math.Max(expected, to);
            }
            if (expected < eventEnd) throw new InvalidDataException($"RAW ends at sample {expected}, expected {eventEnd}.");
            var array = samples.ToArray(); progress?.Report(new("verification", array.Length * 2, array.Length * 2));
            return new RawCapture { EventId = eventId, SampleRateHz = EventRawV2Reader.SampleRateHz, TriggerIndex = checked((uint)(trigger - eventStart)), Samples = array,
                Minimum = array.Length == 0 ? (ushort)0 : array.Min(), Maximum = array.Length == 0 ? (ushort)0 : array.Max(), Mean = array.Length == 0 ? 0 : array.Average(x => (double)x), CrcValid = crcValid };
        }
    }

    private async Task<byte[]> GetChunkAsync(NetworkSessionContext context, Chunk chunk, IProgress<NetworkImportProgress>? progress, CancellationToken ct)
    {
        var ranges = Path.Combine(context.CacheDirectory, "ranges"); Directory.CreateDirectory(ranges);
        var final = Path.Combine(ranges, $"raw-{chunk.Segment:D4}.{chunk.Offset}.{chunk.RawBytes}.{chunk.Crc32:X8}.bin");
        if (File.Exists(final)) { var cached = await File.ReadAllBytesAsync(final, ct); if (cached.Length == chunk.RawBytes && Crc32(cached) == chunk.Crc32) { progress?.Report(new("cache", cached.Length, cached.Length, true)); return cached; } File.Delete(final); }
        var part = final + ".part"; long have = File.Exists(part) ? new FileInfo(part).Length : 0; if (have > chunk.RawBytes) { File.Delete(part); have = 0; }
        while (have < chunk.RawBytes) {
            var start = chunk.Offset + have; var end = chunk.Offset + chunk.RawBytes - 1;
            using var request = new HttpRequestMessage(HttpMethod.Get, $"api/sessions/{Uri.EscapeDataString(context.Folder)}/files/raw-{chunk.Segment:D4}.bin");
            request.Headers.Range = new RangeHeaderValue(start, end); using var response = await _http.SendAsync(request, HttpCompletionOption.ResponseHeadersRead, ct);
            if (response.StatusCode != HttpStatusCode.PartialContent) throw new InvalidDataException($"Expected HTTP 206, got {(int)response.StatusCode}.");
            var range = response.Content.Headers.ContentRange; var length = response.Content.Headers.ContentLength;
            if (range?.From != start || range.To != end || length != end - start + 1) throw new InvalidDataException("Invalid Content-Range or Content-Length.");
            await using var input = await response.Content.ReadAsStreamAsync(ct); await using var output = new FileStream(part, FileMode.Append, FileAccess.Write, FileShare.None, 8192, true);
            var buffer = new byte[8192]; int n; while ((n = await input.ReadAsync(buffer, ct)) > 0) { await output.WriteAsync(buffer.AsMemory(0, n), ct); have += n; progress?.Report(new("RAW requested", have, chunk.RawBytes)); }
        }
        var data = await File.ReadAllBytesAsync(part, ct); if (data.Length != chunk.RawBytes || Crc32(data) != chunk.Crc32) throw new InvalidDataException("Downloaded chunk CRC is invalid; retry is available.");
        File.Move(part, final, true); return data;
    }

    private static Chunk ParseChunk(string line) { using var doc = JsonDocument.Parse(line); var r = doc.RootElement; return new(U64(r,"sample_start"),U64(r,"sample_end"),r.GetProperty("segment_index").GetInt32(),long.Parse(r.GetProperty("segment_offset").GetString()!),r.GetProperty("raw_bytes").GetInt32(),Convert.ToUInt32(r.GetProperty("crc32").GetString(),16)); }
    private sealed record Chunk(ulong SampleStart, ulong SampleEnd, int Segment, long Offset, int RawBytes, uint Crc32);
    private static ulong U64(JsonElement e, string name) { var v=e.GetProperty(name); return v.ValueKind==JsonValueKind.String?ulong.Parse(v.GetString()!):v.GetUInt64(); }
    private async Task<T> GetJsonAsync<T>(string path, CancellationToken ct) => (await GetDocumentAsync(path, ct)).RootElement.Deserialize<T>() ?? throw new InvalidDataException(path);
    private async Task<JsonDocument> GetDocumentAsync(string path, CancellationToken ct) { using var r = await _http.GetAsync(path, ct); r.EnsureSuccessStatusCode(); return JsonDocument.Parse(await r.Content.ReadAsStreamAsync(ct)); }
    private async Task DownloadSmallAsync(string url, string target, long expected, CancellationToken ct) { using var r=await _http.GetAsync(url.TrimStart('/'),ct);r.EnsureSuccessStatusCode();var tmp=target+".tmp";await using(var i=await r.Content.ReadAsStreamAsync(ct))await using(var o=new FileStream(tmp,FileMode.Create,FileAccess.Write,FileShare.None,8192,true))await i.CopyToAsync(o,ct);if(new FileInfo(tmp).Length!=expected)throw new InvalidDataException("Metadata length mismatch.");File.Move(tmp,target,true); }
    private static async Task WriteJsonAsync(string path, JsonElement value, CancellationToken ct) { var tmp=path+".tmp";await File.WriteAllTextAsync(tmp,value.GetRawText(),ct);File.Move(tmp,path,true); }
    private static string SafePart(string value) => string.Concat(value.Select(c => char.IsLetterOrDigit(c) || c is '-' or '_' ? c : '_'));
    private void RememberAnalyzer(AnalyzerInfo a) { Directory.CreateDirectory(_cacheRoot); File.WriteAllText(Path.Combine(_cacheRoot,"known-analyzer.txt"),BaseUri.ToString()); var standard=Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),"KNXAnalyzerStudio","Cache");Directory.CreateDirectory(standard);File.WriteAllText(Path.Combine(standard,"known-analyzer.txt"),BaseUri.ToString()); }
    public static uint Crc32(ReadOnlySpan<byte> data) { uint crc=0xFFFFFFFF;foreach(var b in data){crc^=b;for(var i=0;i<8;i++)crc=(crc>>1)^((crc&1)!=0?0xEDB88320u:0u);}return crc^0xFFFFFFFF; }
    public void Dispose() => _http.Dispose();
}
