using KNXAnalyzer.Core;

namespace KNXAnalyzer.Core.Tests;

public class NetworkImportServiceTests
{
    [Fact]
    public async Task ImportsLiveContinuousRawMetadataAndReadsBoundedRangesWhenConfigured()
    {
        var address = Environment.GetEnvironmentVariable("KNX_ANALYZER_LIVE_URL");
        var sessionId = Environment.GetEnvironmentVariable("KNX_ANALYZER_LIVE_SESSION");
        var expectedSamplesText = Environment.GetEnvironmentVariable("KNX_ANALYZER_LIVE_SAMPLES");
        var expectedBytesText = Environment.GetEnvironmentVariable("KNX_ANALYZER_LIVE_BYTES");
        var expectedChunksText = Environment.GetEnvironmentVariable("KNX_ANALYZER_LIVE_CHUNKS");
        if (string.IsNullOrWhiteSpace(address) || string.IsNullOrWhiteSpace(sessionId) ||
            !ulong.TryParse(expectedSamplesText, out var expectedSamples) ||
            !long.TryParse(expectedBytesText, out var expectedBytes) ||
            !int.TryParse(expectedChunksText, out var expectedChunks)) return;

        var cache = Path.Combine(Path.GetTempPath(), "knxstudio-live-continuous-" + Guid.NewGuid().ToString("N"));
        try
        {
            using var service = new NetworkImportService(address, cache, new HttpClientHandler { UseProxy = false });
            await service.ConnectAsync();
            var descriptor = (await service.ListSessionsAsync()).Single(x => x.SessionId == sessionId);
            var session = await service.ImportMetadataAsync(descriptor);
            Assert.True(session.IsContinuousRaw);
            Assert.Equal("CONTINUOUS_RAW", session.AcquisitionMode);
            Assert.Equal((ulong)0, session.RawSampleStart);
            Assert.Equal(expectedSamples, session.RawSampleEnd);
            Assert.Equal(expectedBytes, session.RawAvailableBytes);
            Assert.Equal(expectedChunks, session.RawChunks.Count);
            if (Environment.GetEnvironmentVariable("KNX_ANALYZER_LIVE_FIELD_SITE") is { } expectedSite) {
                Assert.NotNull(session.Campaign);
                Assert.Equal(expectedSite, session.Campaign.Site);
                Assert.Equal("Test Segment", session.Campaign.Bus);
                Assert.Equal("Bench", session.Campaign.Point);
                Assert.Equal("Field Campaign V1 validation", session.Campaign.Note);
                Assert.Equal((uint)10, session.Campaign.RequestedDurationSeconds);
            }
            ulong nextSample = 0;
            long mappedBytes = 0;
            foreach (var chunk in session.RawChunks.OrderBy(x => x.SampleStart))
            {
                Assert.Equal(nextSample, chunk.SampleStart);
                nextSample = chunk.SampleEnd;
                mappedBytes += chunk.RawBytes;
            }
            Assert.Equal(expectedSamples, nextSample);
            Assert.Equal(expectedBytes, mappedBytes);
            Assert.Empty(session.AnalogEvents);
            Assert.False(File.Exists(Path.Combine(session.DirectoryPath, "raw-0000.bin")));

            foreach (var start in new ulong[] { 0, expectedSamples / 4, expectedSamples / 2,
                         expectedSamples * 3 / 4, expectedSamples - 64 })
            {
                var range = await service.FetchContinuousRangeAsync(session, start, 64);
                Assert.Equal(start, range.SampleStart);
                Assert.Equal(64, range.Samples.Length);
                Assert.Equal(RawIntegrityStatus.Valid, range.Integrity);
            }
        }
        finally
        {
            if (Directory.Exists(cache)) Directory.Delete(cache, true);
        }
    }

    [Fact]
    public async Task ImportsRealS3SessionAndFetchesOnlySelectedEventRawWhenConfigured()
    {
        var address = Environment.GetEnvironmentVariable("KNX_ANALYZER_LIVE_URL");
        if (string.IsNullOrWhiteSpace(address)) return;

        var cache = Path.Combine(Path.GetTempPath(), "knxstudio-live-" + Guid.NewGuid().ToString("N"));
        try {
            using var service = new NetworkImportService(address, cache, new HttpClientHandler { UseProxy = false });
            var analyzer = await service.ConnectAsync();
            Assert.True(analyzer.SdReady);
            var descriptor = (await service.ListSessionsAsync()).Single(x => x.Folder == "KNX-F35F14A4");
            Assert.Equal(18, descriptor.Events);
            Assert.Equal("983040", descriptor.RawBytes);

            var session = await service.ImportMetadataAsync(descriptor);
            Assert.Equal("CLOSED", session.State);
            Assert.Equal(18, session.AnalogEvents.Count);
            Assert.Equal(983040, session.RawAvailableBytes);
            Assert.Contains("2026-09-27_20-03-36_KNX-F35F14A4", session.DirectoryPath);
            Assert.False(File.Exists(Path.Combine(session.DirectoryPath, "raw-0000.bin")));

            var capture = await service.FetchEventAsync(session, 1);
            Assert.True(capture.CrcValid);
            Assert.Equal((uint)83333, capture.SampleRateHz);
            Assert.Equal(21727, capture.Samples.Length);
            Assert.True(Directory.EnumerateFiles(Path.Combine(session.DirectoryPath, "ranges"), "*.bin").Any());
        }
        finally
        {
            if (Directory.Exists(cache)) Directory.Delete(cache, true);
        }
    }
}
