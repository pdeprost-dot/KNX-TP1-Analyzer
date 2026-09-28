using System.Buffers.Binary;
using KNXAnalyzer.Core;

namespace KNXAnalyzer.Core.Tests;

public class EventRawAnalysisTests
{
    [Fact]
    public void CalculatesFirmwareD44AndStrictThresholdExceedances()
    {
        ushort[] samples = [10, 10, 10, 10, 10, 10, 10, 10, 20, 30];
        var d44 = D44Analyzer.Calculate(samples);
        Assert.Equal([0, 0, 0, 0, 0, 0, 0, 0, 10, 30], d44);

        var descriptor = new EventRawDescriptor(7, 100, 104, 110, 1000, 10, 4, 6,
            [Chunk(100, samples)]);
        var capture = EventRawAssembler.Assemble(descriptor, _ => Bytes(samples));

        Assert.Equal((ushort)30, capture.D44Maximum);
        Assert.Equal([9], capture.ThresholdExceedanceIndices);
        Assert.Equal(10, capture.Samples.Length);
        Assert.Equal(10, capture.DurationMilliseconds);
        Assert.Equal((uint)4, capture.TriggerIndex);
        Assert.Equal(RawIntegrityStatus.Valid, capture.Integrity);
    }

    [Fact]
    public void UsesStoredPreEventSamplesForD44AtTheWindowBoundary()
    {
        ushort[] full = [10, 10, 10, 10, 10, 10, 10, 20, 30];
        var chunk = Chunk(0, full);
        var descriptor = new EventRawDescriptor(2, 7, 7, 9, 1000, 9, 0, 2, [chunk]);

        var capture = EventRawAssembler.Assemble(descriptor, _ => Bytes(full));

        Assert.Equal<ushort>([20, 30], capture.Samples);
        Assert.Equal<ushort>([10, 30], capture.D44);
        Assert.Equal([0, 1], capture.ThresholdExceedanceIndices);
    }

    [Fact]
    public void ReconstructsOrderedEventFromOutOfOrderChunksWithoutDuplicatingOverlap()
    {
        ushort[] first = [0, 1, 2, 3, 4, 5];
        ushort[] second = [6, 7, 8, 9, 10, 11];
        var a = Chunk(0, first);
        var b = Chunk(6, second);
        var descriptor = new EventRawDescriptor(1, 2, 5, 10, 2000, 0, 3, 5, [b, a]);
        var data = new Dictionary<ulong, byte[]> { [0] = Bytes(first), [6] = Bytes(second) };

        var capture = EventRawAssembler.Assemble(descriptor, chunk => data[chunk.SampleStart]);

        Assert.Equal<ushort>([2, 3, 4, 5, 6, 7, 8, 9], capture.Samples);
        Assert.Equal(2, capture.ChunkCount);
        Assert.Equal(4, capture.DurationMilliseconds);
        Assert.Equal((ushort)2, capture.Minimum);
        Assert.Equal((ushort)9, capture.Maximum);
        Assert.Equal(5.5, capture.Mean);
    }

    [Fact]
    public void DistinguishesMissingChunkGapAndInvalidCrc()
    {
        var missing = new EventRawDescriptor(1, 0, 1, 4, 1000, 1, 1, 3, []);
        Assert.Equal(RawIntegrityStatus.MissingChunk,
            Assert.Throws<RawCaptureLoadException>(() => EventRawAssembler.Assemble(missing, _ => [])).Status);

        ushort[] left = [1, 2]; ushort[] right = [5, 6];
        var leftChunk = Chunk(0, left); var rightChunk = Chunk(4, right);
        var gap = missing with { SampleEnd = 6, Chunks = [leftChunk, rightChunk] };
        var gapData = new Dictionary<ulong, byte[]> { [0] = Bytes(left), [4] = Bytes(right) };
        Assert.Equal(RawIntegrityStatus.Gap,
            Assert.Throws<RawCaptureLoadException>(() => EventRawAssembler.Assemble(gap, x => gapData[x.SampleStart])).Status);

        var corruptBytes = Bytes(left); corruptBytes[0] ^= 1;
        var corrupt = missing with { SampleEnd = 2, Chunks = [leftChunk] };
        Assert.Equal(RawIntegrityStatus.CrcInvalid,
            Assert.Throws<RawCaptureLoadException>(() => EventRawAssembler.Assemble(corrupt, _ => corruptBytes)).Status);
    }

    private static EventRawChunk Chunk(ulong start, ushort[] samples)
    {
        var bytes = Bytes(samples);
        return new(start, start + (ulong)samples.Length, 0, checked((long)start * 2), bytes.Length,
            EventRawAssembler.Crc32(bytes));
    }

    private static byte[] Bytes(ushort[] samples)
    {
        var bytes = new byte[samples.Length * 2];
        for (var i = 0; i < samples.Length; ++i)
            BinaryPrimitives.WriteUInt16LittleEndian(bytes.AsSpan(i * 2, 2), samples[i]);
        return bytes;
    }
}
