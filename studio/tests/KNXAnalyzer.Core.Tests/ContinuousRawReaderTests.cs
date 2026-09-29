using System.Buffers.Binary;
using KNXAnalyzer.Core;

namespace KNXAnalyzer.Core.Tests;

public sealed class RawCorpusContinuousReaderTests
{
    [Fact]
    public void StreamsOverviewReadsExactZoomAndComputesSelectionStatistics()
    {
        var root = Path.Combine(Path.GetTempPath(), "knx-continuous-view-" + Guid.NewGuid().ToString("N")); Directory.CreateDirectory(root);
        try {
            const int chunks = 32, samplesPerChunk = 4096; var rawPath = Path.Combine(root, "raw-0000.bin");
            using (var raw = File.Create(rawPath)) using (var map = new StreamWriter(Path.Combine(root, "chunks.jsonl"))) {
                for (var chunk = 0; chunk < chunks; chunk++) {
                    var bytes = new byte[samplesPerChunk * 2]; for (var i = 0; i < samplesPerChunk; i++) BinaryPrimitives.WriteUInt16LittleEndian(bytes.AsSpan(i * 2), (ushort)((chunk * samplesPerChunk + i) % 4096));
                    raw.Write(bytes); var start = chunk * samplesPerChunk;
                    map.WriteLine($"{{\"sample_start\":\"{start}\",\"sample_count\":{samplesPerChunk},\"segment_index\":0,\"segment_offset\":\"{chunk * bytes.Length}\",\"raw_bytes\":{bytes.Length}}}");
                }
            }
            var entry = Entry(root, chunks * samplesPerChunk, new FileInfo(rawPath).Length);
            var reader = new RawCorpusContinuousReader(entry);
            var overview = reader.ReadView(0, entry.SampleCount, 128, 1000);
            Assert.Null(overview.ExactSamples); Assert.Equal(128, overview.Bins.Length); Assert.Contains(overview.Bins, x => x.Minimum == 0 && x.Maximum > 0);
            var zoom = reader.ReadView(4090, 4110, 128, 1000);
            Assert.NotNull(zoom.ExactSamples); Assert.Equal(20, zoom.ExactSamples!.Length); Assert.Equal((ushort)4090, zoom.ExactSamples[0]); Assert.Equal((ushort)0, zoom.ExactSamples[6]);
            Assert.Equal((ushort)1234, reader.ReadSample(1234));
            var stats = reader.ReadSelectionStats(0, 4096);
            Assert.Equal((ulong)4096, stats.Count); Assert.Equal((ushort)0, stats.Minimum); Assert.Equal((ushort)4095, stats.Maximum); Assert.Equal(2047.5, stats.Mean, 6); Assert.Equal((ushort)4095, stats.PeakToPeak);
        } finally { Directory.Delete(root, true); }
    }

    [Fact]
    public void RefusesCorpusThatIsNotValid()
    {
        var entry = Entry(Path.GetTempPath(), 1, 2) with { Integrity = RawCorpusIntegrity.Incomplete };
        Assert.Throws<InvalidDataException>(() => new RawCorpusContinuousReader(entry));
    }

    private static RawCorpusEntry Entry(string directory, long samples, long bytes) => new() {
        CorpusId = "raw-test", ContentSha256 = new('A', 64), LogicalRawSha256 = new('B', 64), SessionId = "session-test",
        ImportedUtc = DateTimeOffset.UtcNow, SourceDirectory = directory, DataDirectory = directory, StorageMode = RawCorpusStorageMode.Reference,
        AcquisitionMode = "CONTINUOUS_RAW", State = "CLOSED", CompletionStatus = "COMPLETE", MeasuredSampleRateHz = 83333,
        SampleCount = checked((ulong)samples), SampleFormat = "uint16_le", RawBytes = bytes, ChunkCount = checked((int)((samples + 4095) / 4096)),
        AcquisitionMetadata = new(null, null, null, null), Integrity = RawCorpusIntegrity.Valid, IntegrityMessages = [], Files = []
    };
}
