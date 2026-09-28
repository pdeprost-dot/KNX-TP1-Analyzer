using System.Buffers.Binary;

namespace KNXAnalyzer.Core;

public sealed record RawSampleRange(
    ulong SampleStart,
    ushort[] Samples,
    int ValidatedChunkCount,
    RawIntegrityStatus Integrity)
{
    public ulong SampleEnd => SampleStart + (ulong)Samples.Length;
}

/// <summary>Chunk-backed, bounded access to continuous RAW. Only overlapping chunks are read.</summary>
public static class ContinuousRawReader
{
    public static RawSampleRange Read(string directory, ulong sampleStart, int sampleCount)
    {
        var session = EventRawV2Reader.OpenSession(directory);
        if (!session.IsContinuousRaw) throw new InvalidOperationException("Session is not CONTINUOUS_RAW.");
        var segmented = !File.Exists(Path.Combine(directory, "raw.bin"));
        return ReadAsync(session, sampleStart, sampleCount, (chunk, _) => {
            var path = segmented ? Path.Combine(directory, $"raw-{chunk.SegmentIndex:D4}.bin") : Path.Combine(directory, "raw.bin");
            using var raw = File.OpenRead(path);
            raw.Position = chunk.SegmentOffset;
            var bytes = new byte[chunk.RawBytes];
            raw.ReadExactly(bytes);
            return ValueTask.FromResult(bytes);
        }).AsTask().GetAwaiter().GetResult();
    }

    public static async ValueTask<RawSampleRange> ReadAsync(Session session, ulong sampleStart, int sampleCount,
        Func<EventRawChunk, CancellationToken, ValueTask<byte[]>> readChunk, CancellationToken cancellationToken = default)
    {
        if (!session.IsContinuousRaw) throw new InvalidOperationException("Session is not CONTINUOUS_RAW.");
        if (sampleCount < 0) throw new ArgumentOutOfRangeException(nameof(sampleCount));
        var end = checked(sampleStart + (ulong)sampleCount);
        var samples = new ushort[sampleCount];
        var expected = sampleStart;
        var validated = 0;
        foreach (var chunk in session.RawChunks.Where(x => x.SampleEnd > sampleStart && x.SampleStart < end).OrderBy(x => x.SampleStart)) {
            if (chunk.SampleStart > expected)
                throw new RawCaptureLoadException(RawIntegrityStatus.Gap, $"RAW gap [{expected}, {chunk.SampleStart}).");
            var bytes = await readChunk(chunk, cancellationToken);
            if (bytes.Length != chunk.RawBytes || bytes.Length != checked((int)chunk.SampleCount * 2))
                throw new RawCaptureLoadException(RawIntegrityStatus.Incomplete, $"Chunk at {chunk.SampleStart} has an invalid byte length.");
            if (EventRawAssembler.Crc32(bytes) != chunk.Crc32)
                throw new RawCaptureLoadException(RawIntegrityStatus.CrcInvalid, $"Chunk CRC is invalid at sample {chunk.SampleStart}.");
            ++validated;
            var from = Math.Max(expected, Math.Max(sampleStart, chunk.SampleStart));
            var to = Math.Min(end, chunk.SampleEnd);
            for (var sample = from; sample < to; ++sample) {
                var source = checked((int)((sample - chunk.SampleStart) * 2));
                samples[checked((int)(sample - sampleStart))] = BinaryPrimitives.ReadUInt16LittleEndian(bytes.AsSpan(source, 2));
            }
            expected = Math.Max(expected, to);
        }
        if (expected < end)
            throw new RawCaptureLoadException(validated == 0 ? RawIntegrityStatus.MissingChunk : RawIntegrityStatus.Gap,
                $"RAW ends at sample {expected}, expected {end}.");
        return new(sampleStart, samples, validated, RawIntegrityStatus.Valid);
    }
}
