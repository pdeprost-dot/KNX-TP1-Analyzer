using KNXAnalyzer.Core;

namespace KNXAnalyzer.Core.Tests;

public class NetworkImportServiceTests
{
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
