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
    [property: JsonPropertyName("duration_us")] string DurationUs,
    [property: JsonPropertyName("start_utc")] string? StartUtc = null,
    [property: JsonPropertyName("completion_status")] string? CompletionStatus = null,
    [property: JsonPropertyName("calibration_crc")] string? CalibrationCrc = null,
    [property: JsonPropertyName("threshold")] int? Threshold = null,
    [property: JsonPropertyName("confidence")] string? Confidence = null,
    [property: JsonPropertyName("acquisition_mode")] string? AcquisitionMode = null,
    [property: JsonPropertyName("has_session_start")] bool? HasSessionStart = null)
{
    public string DisplayName => $"{(string.IsNullOrWhiteSpace(StartUtc) ? "UNSYNCED" : StartUtc)}  ·  {SessionId}  ·  {State}/{CompletionStatus}";
}

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
        _http = handler is null ? new HttpClient(new HttpClientHandler { UseProxy = false }) : new HttpClient(handler);
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
        using var doc = await GetDocumentAsync("api/v1/sessions", ct);
        var sessions = doc.RootElement.GetProperty("sessions").Deserialize<NetworkSessionInfo[]>() ?? [];
        return sessions.Where(x => x.HasSessionStart is not false).ToArray();
    }

    public async Task<Session> ImportMetadataAsync(NetworkSessionInfo info, IProgress<NetworkImportProgress>? progress = null, CancellationToken ct = default)
    {
        if (Analyzer is null) await ConnectAsync(ct);
        progress?.Report(new("metadata"));
        using var manifest = await GetDocumentAsync($"api/v1/sessions/{Uri.EscapeDataString(info.Folder)}", ct);
        var physicalContinuousManifest = manifest.RootElement.TryGetProperty("schema_version", out var manifestSchema) &&
            manifestSchema.GetString() == "knx-continuous-raw-manifest-1.0";
        var sessionUuid = string.IsNullOrWhiteSpace(info.SessionId) ? info.Folder : info.SessionId;
        var localName = LocalSessionName(info, sessionUuid);
        var analyzerCache = Path.Combine(_cacheRoot, SafePart(Analyzer!.AnalyzerId));
        var cache = Path.Combine(analyzerCache, localName);
        var complete = Path.Combine(cache, ".metadata-complete");
        if (!File.Exists(complete)) {
            Directory.CreateDirectory(analyzerCache);
            var staging = cache + ".importing-" + Guid.NewGuid().ToString("N");
            Directory.CreateDirectory(staging);
            try {
                if (physicalContinuousManifest) {
                    var prefix = $"api/v1/sessions/{Uri.EscapeDataString(info.Folder)}/files/";
                    foreach (var name in new[] { "session-start.json", "test-result.json", "segments.jsonl", "chunks.jsonl",
                                 "chunk-index.jsonl", "events.jsonl", "session-end.json", "manifest.json" }) {
                        var received = await DownloadSmallAsync(prefix + name, Path.Combine(staging, name), null, ct);
                        progress?.Report(new("downloaded", received, received));
                    }
                } else {
                    if (!manifest.RootElement.TryGetProperty("session_start", out var sessionStart) || sessionStart.ValueKind != JsonValueKind.Object)
                        throw new InvalidDataException($"Session {info.Folder} has no usable session_start metadata.");
                    await WriteJsonAsync(Path.Combine(staging, "session-start.json"), sessionStart, ct);
                    if (manifest.RootElement.TryGetProperty("test_result", out var result) && result.ValueKind == JsonValueKind.Object)
                        await WriteJsonAsync(Path.Combine(staging, "test-result.json"), result, ct);
                    var files = manifest.RootElement.GetProperty("files");
                    foreach (var name in new[] { "segments.jsonl", "chunks.jsonl", "chunk-index.jsonl", "events.jsonl" }) {
                        var descriptor = files.GetProperty(name);
                        if (!descriptor.GetProperty("present").GetBoolean()) continue;
                        var expected = ReadInt64(descriptor.GetProperty("bytes"));
                        await DownloadSmallAsync(descriptor.GetProperty("url").GetString()!, Path.Combine(staging, name), expected, ct);
                        progress?.Report(new("downloaded", expected, expected));
                    }
                    if (manifest.RootElement.TryGetProperty("file_list", out var fileList)) {
                        foreach (var item in fileList.EnumerateArray()) {
                            var name = item.GetProperty("name").GetString();
                            if (name is not ("manifest.json" or "session-end.json")) continue;
                            var expected = ReadInt64(item.GetProperty("bytes"));
                            await DownloadSmallAsync(item.GetProperty("url").GetString()!, Path.Combine(staging, name), expected, ct);
                        }
                    }
                }
                _ = EventRawV2Reader.OpenSession(staging);
                await File.WriteAllTextAsync(Path.Combine(staging, ".metadata-complete"), sessionUuid, ct);
                if (Directory.Exists(cache)) Directory.Delete(cache, true);
                Directory.Move(staging, cache);
            } catch { if (Directory.Exists(staging)) Directory.Delete(staging, true); throw; }
        } else {
            progress?.Report(new("metadata cache", FromCache: true));
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
        var analog = session.AnalogEvents.SingleOrDefault(x => x.EventId == eventId)
            ?? throw new InvalidDataException($"Event {eventId} was not found.");
        var descriptor = analog.RawDescriptor ?? throw new InvalidDataException($"Event {eventId} has no RAW descriptor.");
        var capture = await EventRawAssembler.AssembleAsync(descriptor,
            async (chunk, token) => await GetChunkAsync(context, chunk, progress, token), ct);
        progress?.Report(new("verification", capture.Samples.Length * 2, capture.Samples.Length * 2));
        return capture;
    }

    public async Task<RawSampleRange> FetchContinuousRangeAsync(Session session, ulong sampleStart, int sampleCount,
        IProgress<NetworkImportProgress>? progress = null, CancellationToken ct = default)
    {
        if (!session.IsContinuousRaw) throw new InvalidOperationException("Session is not CONTINUOUS_RAW.");
        var context = session.Network ?? throw new InvalidOperationException("Session is not network-backed.");
        return await ContinuousRawReader.ReadAsync(session, sampleStart, sampleCount,
            async (chunk, token) => await GetChunkAsync(context, chunk, progress, token), ct);
    }

    private async Task<byte[]> GetChunkAsync(NetworkSessionContext context, EventRawChunk chunk, IProgress<NetworkImportProgress>? progress, CancellationToken ct)
    {
        var ranges = Path.Combine(context.CacheDirectory, "ranges"); Directory.CreateDirectory(ranges);
        var final = Path.Combine(ranges, $"raw-{chunk.SegmentIndex:D4}.{chunk.SegmentOffset}.{chunk.RawBytes}.{chunk.Crc32:X8}.bin");
        if (File.Exists(final)) { var cached = await File.ReadAllBytesAsync(final, ct); if (cached.Length == chunk.RawBytes && Crc32(cached) == chunk.Crc32) { progress?.Report(new("cache", cached.Length, cached.Length, true)); return cached; } File.Delete(final); }
        var part = final + ".part"; long have = File.Exists(part) ? new FileInfo(part).Length : 0; if (have > chunk.RawBytes) { File.Delete(part); have = 0; }
        while (have < chunk.RawBytes) {
            var start = chunk.SegmentOffset + have; var end = chunk.SegmentOffset + chunk.RawBytes - 1;
            using var request = new HttpRequestMessage(HttpMethod.Get, $"api/v1/sessions/{Uri.EscapeDataString(context.Folder)}/files/raw-{chunk.SegmentIndex:D4}.bin");
            request.Headers.Range = new RangeHeaderValue(start, end); using var response = await _http.SendAsync(request, HttpCompletionOption.ResponseHeadersRead, ct);
            if (response.StatusCode == HttpStatusCode.NotFound)
                throw new RawCaptureLoadException(RawIntegrityStatus.MissingChunk, $"RAW segment {chunk.SegmentIndex} is missing.");
            if (response.StatusCode != HttpStatusCode.PartialContent) throw new InvalidDataException($"Expected HTTP 206, got {(int)response.StatusCode}.");
            var range = response.Content.Headers.ContentRange; var length = response.Content.Headers.ContentLength;
            if (range?.From != start || range.To != end || length != end - start + 1) throw new InvalidDataException("Invalid Content-Range or Content-Length.");
            await using var input = await response.Content.ReadAsStreamAsync(ct); await using var output = new FileStream(part, FileMode.Append, FileAccess.Write, FileShare.None, 8192, true);
            var buffer = new byte[8192]; int n; while ((n = await input.ReadAsync(buffer, ct)) > 0) { await output.WriteAsync(buffer.AsMemory(0, n), ct); have += n; progress?.Report(new("RAW requested", have, chunk.RawBytes)); }
        }
        var data = await File.ReadAllBytesAsync(part, ct);
        if (data.Length != chunk.RawBytes) throw new RawCaptureLoadException(RawIntegrityStatus.Incomplete, "Downloaded chunk length is invalid; retry is available.");
        if (Crc32(data) != chunk.Crc32) throw new RawCaptureLoadException(RawIntegrityStatus.CrcInvalid, "Downloaded chunk CRC is invalid; retry is available.");
        File.Move(part, final, true); return data;
    }

    private static ulong U64(JsonElement e, string name) { var v=e.GetProperty(name); return v.ValueKind==JsonValueKind.String?ulong.Parse(v.GetString()!):v.GetUInt64(); }
    private static ulong U64Either(JsonElement e, string first, string second) => e.TryGetProperty(first, out _) ? U64(e, first) : U64(e, second);
    private static long ReadInt64(JsonElement value) => value.ValueKind == JsonValueKind.String ? long.Parse(value.GetString()!) : value.GetInt64();
    private async Task<T> GetJsonAsync<T>(string path, CancellationToken ct) => (await GetDocumentAsync(path, ct)).RootElement.Deserialize<T>() ?? throw new InvalidDataException(path);
    private async Task<JsonDocument> GetDocumentAsync(string path, CancellationToken ct) { using var r = await _http.GetAsync(path, ct); r.EnsureSuccessStatusCode(); return JsonDocument.Parse(await r.Content.ReadAsStreamAsync(ct)); }
    private async Task<long> DownloadSmallAsync(string url, string target, long? expected, CancellationToken ct) { using var r=await _http.GetAsync(url.TrimStart('/'),HttpCompletionOption.ResponseHeadersRead,ct);r.EnsureSuccessStatusCode();var declared=r.Content.Headers.ContentLength;if(expected.HasValue&&declared.HasValue&&declared.Value!=expected.Value)throw new InvalidDataException("Metadata Content-Length mismatch.");var tmp=target+".tmp";await using(var i=await r.Content.ReadAsStreamAsync(ct))await using(var o=new FileStream(tmp,FileMode.Create,FileAccess.Write,FileShare.None,8192,true))await i.CopyToAsync(o,ct);var received=new FileInfo(tmp).Length;var required=expected??declared??throw new InvalidDataException("Metadata Content-Length is missing.");if(received!=required)throw new InvalidDataException("Metadata length mismatch.");File.Move(tmp,target,true);return received; }
    private static async Task WriteJsonAsync(string path, JsonElement value, CancellationToken ct) { var tmp=path+".tmp";await File.WriteAllTextAsync(tmp,value.GetRawText(),ct);File.Move(tmp,path,true); }
    private static string SafePart(string value) => string.Concat(value.Select(c => char.IsLetterOrDigit(c) || c is '-' or '_' ? c : '_'));
    private static string LocalSessionName(NetworkSessionInfo info, string sessionUuid) {
        if (DateTimeOffset.TryParse(info.StartUtc, out var start)) return $"{start.ToLocalTime():yyyy-MM-dd_HH-mm-ss}_{SafePart(sessionUuid)}";
        return SafePart(sessionUuid);
    }
    private void RememberAnalyzer(AnalyzerInfo a) { Directory.CreateDirectory(_cacheRoot); File.WriteAllText(Path.Combine(_cacheRoot,"known-analyzer.txt"),BaseUri.ToString()); }
    public static uint Crc32(ReadOnlySpan<byte> data) => EventRawAssembler.Crc32(data);
    public void Dispose() => _http.Dispose();
}
