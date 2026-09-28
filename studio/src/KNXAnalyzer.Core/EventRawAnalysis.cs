using System.Buffers.Binary;

namespace KNXAnalyzer.Core;

public enum RawIntegrityStatus
{
    Valid,
    Incomplete,
    MissingChunk,
    Gap,
    CrcInvalid
}

public sealed class RawCaptureLoadException : IOException
{
    public RawCaptureLoadException(RawIntegrityStatus status, string message) : base(message) => Status = status;
    public RawIntegrityStatus Status { get; }
}

public sealed record EventRawChunk(
    ulong SampleStart,
    ulong SampleEnd,
    int SegmentIndex,
    long SegmentOffset,
    int RawBytes,
    uint Crc32)
{
    public uint SampleCount => checked((uint)(SampleEnd - SampleStart));
}

public sealed record EventRawDescriptor(
    uint EventId,
    ulong SampleStart,
    ulong TriggerSample,
    ulong SampleEnd,
    uint SampleRateHz,
    uint Threshold,
    uint ConfiguredPreSamples,
    uint ConfiguredPostSamples,
    IReadOnlyList<EventRawChunk> Chunks,
    EventRawChunk? D44HistoryChunk = null)
{
    public ulong SampleCount => SampleEnd - SampleStart;
    public ulong ActualPreSamples => TriggerSample - SampleStart;
    public ulong ActualPostSamples => SampleEnd - TriggerSample;
    public double DurationMilliseconds => SampleRateHz == 0 ? 0 : 1000.0 * SampleCount / SampleRateHz;
}

public static class D44Analyzer
{
    public static ushort[] Calculate(ReadOnlySpan<ushort> samples, ReadOnlySpan<ushort> precedingSamples = default)
    {
        var d44 = new ushort[samples.Length];
        var first = precedingSamples.Length >= 7 ? 0 : 8;
        for (var i = first; i < samples.Length; ++i) {
            var recent = (uint)At(samples, precedingSamples, i) + At(samples, precedingSamples, i - 1) +
                At(samples, precedingSamples, i - 2) + At(samples, precedingSamples, i - 3);
            var previous = (uint)At(samples, precedingSamples, i - 4) + At(samples, precedingSamples, i - 5) +
                At(samples, precedingSamples, i - 6) + At(samples, precedingSamples, i - 7);
            d44[i] = checked((ushort)(recent >= previous ? recent - previous : previous - recent));
        }
        return d44;
    }

    private static ushort At(ReadOnlySpan<ushort> samples, ReadOnlySpan<ushort> precedingSamples, int index) =>
        index >= 0 ? samples[index] : precedingSamples[precedingSamples.Length + index];
}

public static class EventRawAssembler
{
    public static RawCapture Assemble(EventRawDescriptor descriptor, Func<EventRawChunk, byte[]> readChunk) =>
        AssembleAsync(descriptor, (chunk, _) => ValueTask.FromResult(readChunk(chunk))).AsTask().GetAwaiter().GetResult();

    public static async ValueTask<RawCapture> AssembleAsync(
        EventRawDescriptor descriptor,
        Func<EventRawChunk, CancellationToken, ValueTask<byte[]>> readChunk,
        CancellationToken cancellationToken = default)
    {
        if (descriptor.SampleEnd < descriptor.SampleStart || descriptor.TriggerSample < descriptor.SampleStart ||
            descriptor.TriggerSample > descriptor.SampleEnd)
            throw new RawCaptureLoadException(RawIntegrityStatus.Incomplete, "Invalid event sample bounds.");
        if (descriptor.SampleCount > int.MaxValue)
            throw new RawCaptureLoadException(RawIntegrityStatus.Incomplete, "Event is too large for the in-memory viewer.");

        var samples = new ushort[checked((int)descriptor.SampleCount)];
        var preceding = new ushort[7];
        var precedingCount = 0;
        if (descriptor.D44HistoryChunk is { } historyChunk) {
            var historyBytes = await ReadAndValidate(historyChunk, readChunk, cancellationToken);
            precedingCount = CopyPreceding(historyChunk, historyBytes, descriptor.SampleStart, preceding);
        }
        var expected = descriptor.SampleStart;
        foreach (var chunk in descriptor.Chunks.OrderBy(x => x.SampleStart)) {
            if (chunk.SampleEnd <= expected || chunk.SampleStart >= descriptor.SampleEnd) continue;
            if (chunk.SampleStart > expected)
                throw new RawCaptureLoadException(RawIntegrityStatus.Gap, $"RAW gap [{expected}, {chunk.SampleStart}).");

            var bytes = await ReadAndValidate(chunk, readChunk, cancellationToken);
            if (precedingCount < 7 && chunk.SampleStart < descriptor.SampleStart && chunk.SampleEnd >= descriptor.SampleStart)
                precedingCount = CopyPreceding(chunk, bytes, descriptor.SampleStart, preceding);

            var from = Math.Max(expected, Math.Max(descriptor.SampleStart, chunk.SampleStart));
            var to = Math.Min(descriptor.SampleEnd, chunk.SampleEnd);
            for (var sample = from; sample < to; ++sample) {
                var source = checked((int)((sample - chunk.SampleStart) * 2));
                samples[checked((int)(sample - descriptor.SampleStart))] = BinaryPrimitives.ReadUInt16LittleEndian(bytes.AsSpan(source, 2));
            }
            expected = Math.Max(expected, to);
        }
        if (expected < descriptor.SampleEnd)
            throw new RawCaptureLoadException(descriptor.Chunks.Count == 0 ? RawIntegrityStatus.MissingChunk : RawIntegrityStatus.Gap,
                $"RAW ends at sample {expected}, expected {descriptor.SampleEnd}.");

        ushort minimum = ushort.MaxValue, maximum = 0;
        ulong sum = 0;
        foreach (var sample in samples) { minimum = Math.Min(minimum, sample); maximum = Math.Max(maximum, sample); sum += sample; }
        if (samples.Length == 0) minimum = 0;
        var d44 = D44Analyzer.Calculate(samples, preceding.AsSpan(7 - precedingCount, precedingCount));
        var exceedances = descriptor.Threshold == 0 ? [] : d44.Select((value, index) => (value, index))
            .Where(x => x.value > descriptor.Threshold).Select(x => x.index).ToArray();
        return new RawCapture {
            EventId = descriptor.EventId,
            SampleRateHz = descriptor.SampleRateHz,
            SampleStart = descriptor.SampleStart,
            SampleEnd = descriptor.SampleEnd,
            TriggerSample = descriptor.TriggerSample,
            TriggerIndex = checked((uint)(descriptor.TriggerSample - descriptor.SampleStart)),
            Threshold = descriptor.Threshold,
            ConfiguredPreSamples = descriptor.ConfiguredPreSamples,
            ConfiguredPostSamples = descriptor.ConfiguredPostSamples,
            ChunkCount = descriptor.Chunks.Count,
            Samples = samples,
            D44 = d44,
            ThresholdExceedanceIndices = exceedances,
            Minimum = minimum,
            Maximum = maximum,
            Mean = samples.Length == 0 ? 0 : (double)sum / samples.Length,
            CrcValid = true,
            Integrity = RawIntegrityStatus.Valid
        };
    }

    private static async ValueTask<byte[]> ReadAndValidate(EventRawChunk chunk,
        Func<EventRawChunk, CancellationToken, ValueTask<byte[]>> readChunk, CancellationToken cancellationToken)
    {
        byte[] bytes;
        try { bytes = await readChunk(chunk, cancellationToken); }
        catch (FileNotFoundException e) { throw new RawCaptureLoadException(RawIntegrityStatus.MissingChunk, e.Message); }
        catch (DirectoryNotFoundException e) { throw new RawCaptureLoadException(RawIntegrityStatus.MissingChunk, e.Message); }
        if (bytes.Length != chunk.RawBytes || bytes.Length != checked((int)chunk.SampleCount * 2))
            throw new RawCaptureLoadException(RawIntegrityStatus.Incomplete, $"Chunk at {chunk.SampleStart} has an invalid byte length.");
        if (Crc32(bytes) != chunk.Crc32)
            throw new RawCaptureLoadException(RawIntegrityStatus.CrcInvalid, $"Chunk CRC is invalid at sample {chunk.SampleStart}.");
        return bytes;
    }

    private static int CopyPreceding(EventRawChunk chunk, ReadOnlySpan<byte> bytes, ulong eventStart, Span<ushort> target)
    {
        if (chunk.SampleStart >= eventStart || chunk.SampleEnd < eventStart) return 0;
        var count = checked((int)Math.Min(7UL, eventStart - chunk.SampleStart));
        var first = eventStart - (ulong)count;
        for (var i = 0; i < count; ++i) {
            var source = checked((int)((first + (ulong)i - chunk.SampleStart) * 2));
            target[target.Length - count + i] = BinaryPrimitives.ReadUInt16LittleEndian(bytes.Slice(source, 2));
        }
        return count;
    }

    public static uint Crc32(ReadOnlySpan<byte> data)
    {
        uint crc = 0xFFFFFFFF;
        foreach (var b in data) {
            crc ^= b;
            for (var i = 0; i < 8; ++i) crc = (crc >> 1) ^ ((crc & 1) != 0 ? 0xEDB88320u : 0u);
        }
        return crc ^ 0xFFFFFFFF;
    }
}
