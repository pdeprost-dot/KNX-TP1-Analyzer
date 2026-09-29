using System.Buffers.Binary;
using System.Text.Json;

namespace KNXAnalyzer.Core;

public readonly record struct ContinuousRawBin(ulong FirstSample, ulong SampleCount, ushort Minimum, ushort Maximum);
public sealed record ContinuousRawView(ulong StartSample, ulong EndSample, ContinuousRawBin[] Bins, ushort[]? ExactSamples)
{
    public ulong SampleCount => EndSample - StartSample;
}
public readonly record struct ContinuousRawSelectionStats(ulong StartSample, ulong EndSample, ulong Count, ushort Minimum, ushort Maximum, double Mean)
{
    public ushort PeakToPeak => (ushort)(Maximum - Minimum);
}

public sealed class RawCorpusContinuousReader
{
    private sealed record Chunk(ulong Start, ulong Count, string Path, long Offset, int Bytes)
    { public ulong End => Start + Count; }

    private readonly Chunk[] chunks;
    public RawCorpusEntry Entry { get; }
    public ulong SampleCount => Entry.SampleCount;
    public double SampleRateHz => Entry.MeasuredSampleRateHz ?? throw new InvalidDataException("Measured sample rate is missing.");

    public RawCorpusContinuousReader(RawCorpusEntry entry)
    {
        Entry = entry;
        if (entry.AcquisitionMode != "CONTINUOUS_RAW") throw new NotSupportedException("The entry is not CONTINUOUS_RAW.");
        if (entry.Integrity != RawCorpusIntegrity.Valid) throw new InvalidDataException($"Continuous RAW viewer requires VALID integrity (found {entry.Integrity}).");
        if (entry.SampleFormat != "uint16_le") throw new NotSupportedException($"Unsupported sample format: {entry.SampleFormat}.");
        chunks = ReadChunks(entry.DataDirectory);
        if (chunks.Length == 0 || chunks[0].Start != 0 || chunks[^1].End != entry.SampleCount)
            throw new InvalidDataException("Chunk map does not cover the declared sample range.");
    }

    public ContinuousRawView ReadView(ulong startSample, ulong endSample, int targetBins = 1600, int exactSampleLimit = 20_000, CancellationToken cancellationToken = default)
    {
        (startSample, endSample) = ClampRange(startSample, endSample);
        var count = endSample - startSample;
        if (count <= (ulong)exactSampleLimit) {
            var exact = new ushort[checked((int)count)]; var index = 0;
            Stream(startSample, endSample, sample => exact[index++] = sample, cancellationToken);
            return new(startSample, endSample, [], exact);
        }
        var binCount = checked((int)Math.Min((ulong)Math.Max(1, targetBins), count));
        var minimum = Enumerable.Repeat(ushort.MaxValue, binCount).ToArray(); var maximum = new ushort[binCount]; var seen = new ulong[binCount];
        ulong position = 0;
        Stream(startSample, endSample, sample => {
            var bin = Math.Min(binCount - 1, checked((int)(position * (ulong)binCount / count)));
            minimum[bin] = Math.Min(minimum[bin], sample); maximum[bin] = Math.Max(maximum[bin], sample); seen[bin]++; position++;
        }, cancellationToken);
        var bins = new ContinuousRawBin[binCount]; ulong first = startSample;
        for (var i = 0; i < binCount; i++) { bins[i] = new(first, seen[i], minimum[i] == ushort.MaxValue ? (ushort)0 : minimum[i], maximum[i]); first += seen[i]; }
        return new(startSample, endSample, bins, null);
    }

    public ushort ReadSample(ulong sampleIndex)
    {
        if (sampleIndex >= SampleCount) throw new ArgumentOutOfRangeException(nameof(sampleIndex));
        ushort value = 0; Stream(sampleIndex, sampleIndex + 1, sample => value = sample, CancellationToken.None); return value;
    }

    public ContinuousRawSelectionStats ReadSelectionStats(ulong startSample, ulong endSample)
    {
        (startSample, endSample) = ClampRange(startSample, endSample);
        ushort minimum = ushort.MaxValue, maximum = 0; ulong count = 0; double mean = 0;
        Stream(startSample, endSample, sample => { count++; minimum = Math.Min(minimum, sample); maximum = Math.Max(maximum, sample); mean += (sample - mean) / count; }, CancellationToken.None);
        return new(startSample, endSample, count, minimum, maximum, mean);
    }

    private (ulong Start, ulong End) ClampRange(ulong start, ulong end)
    {
        start = Math.Min(start, SampleCount); end = Math.Min(end, SampleCount);
        if (end <= start) throw new ArgumentOutOfRangeException(nameof(end), "The sample range is empty.");
        return (start, end);
    }

    private void Stream(ulong start, ulong end, Action<ushort> accept, CancellationToken cancellationToken)
    {
        var buffer = new byte[64 * 1024];
        foreach (var chunk in chunks.Where(x => x.Start < end && x.End > start)) {
            cancellationToken.ThrowIfCancellationRequested();
            var from = Math.Max(start, chunk.Start); var to = Math.Min(end, chunk.End); var samples = to - from;
            using var stream = new FileStream(chunk.Path, FileMode.Open, FileAccess.Read, FileShare.Read, buffer.Length, FileOptions.SequentialScan);
            stream.Position = checked(chunk.Offset + (long)((from - chunk.Start) * 2)); var bytesRemaining = checked((long)samples * 2);
            while (bytesRemaining > 0) {
                cancellationToken.ThrowIfCancellationRequested();
                var wanted = checked((int)Math.Min(buffer.Length, bytesRemaining)); var read = stream.Read(buffer, 0, wanted);
                if (read == 0 || (read & 1) != 0) throw new EndOfStreamException($"RAW ends before sample {to}.");
                for (var offset = 0; offset < read; offset += 2) accept(BinaryPrimitives.ReadUInt16LittleEndian(buffer.AsSpan(offset, 2)));
                bytesRemaining -= read;
            }
        }
    }

    private static Chunk[] ReadChunks(string directory)
    {
        var path = Path.Combine(directory, "chunks.jsonl"); if (!File.Exists(path)) throw new InvalidDataException("chunks.jsonl is missing.");
        var result = new List<Chunk>();
        foreach (var line in File.ReadLines(path).Where(x => !string.IsNullOrWhiteSpace(x))) {
            using var document = JsonDocument.Parse(line); var item = document.RootElement;
            var start = U64(item, "sample_start"); var count = item.TryGetProperty("sample_count", out _) ? U64(item, "sample_count") : U64(item, "sample_end") - start;
            var segment = item.TryGetProperty("segment_index", out _) ? checked((int)U64(item, "segment_index")) : 0;
            var offset = item.TryGetProperty("segment_offset", out _) ? checked((long)U64(item, "segment_offset")) : checked((long)U64(item, "raw_offset"));
            var bytes = checked((int)U64(item, "raw_bytes")); var file = File.Exists(Path.Combine(directory, "raw.bin")) ? "raw.bin" : $"raw-{segment:D4}.bin";
            var full = Path.Combine(directory, file); if (!File.Exists(full)) throw new InvalidDataException($"Missing RAW file: {file}.");
            result.Add(new(start, count, full, offset, bytes));
        }
        var ordered = result.OrderBy(x => x.Start).ToArray(); ulong expected = 0;
        foreach (var chunk in ordered) { if (chunk.Start != expected || chunk.Bytes != checked((int)chunk.Count * 2)) throw new InvalidDataException("Chunk map is discontinuous or inconsistent."); expected = chunk.End; }
        return ordered;
    }

    private static ulong U64(JsonElement element, string name)
    {
        var value = element.GetProperty(name);
        if (value.ValueKind == JsonValueKind.Number && value.TryGetUInt64(out var number)) return number;
        if (value.ValueKind == JsonValueKind.String && ulong.TryParse(value.GetString(), out number)) return number;
        throw new InvalidDataException($"Invalid {name}.");
    }
}
