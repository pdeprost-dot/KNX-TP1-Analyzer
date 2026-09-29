using KNXAnalyzer.Core;
using System.IO.Compression;
using Xunit.Abstractions;

namespace KNXAnalyzer.Core.Tests;

public sealed class KnxTelegramRealReplayTests(ITestOutputHelper output)
{
    [Fact]
    public void ReplaysRealFieldCandidateCorpusOfflineWithoutChangingTp1Baseline()
    {
        var archive = Path.Combine(AppContext.BaseDirectory, "Fixtures", "KNX-F35F14A4.zip");
        Assert.True(File.Exists(archive), $"Offline regression fixture is missing: {archive}");
        var folder = Path.Combine(Path.GetTempPath(), "knx-telegram-replay-" + Guid.NewGuid().ToString("N"));
        try {
            ZipFile.ExtractToDirectory(archive, folder);
            var session = EventRawV2Reader.OpenSession(folder);
            var results = session.AnalogEvents.OrderBy(x => x.EventId)
                .Select(item => (item.EventId, Decode: Tp1DeterministicDecoder.Decode(
                    EventRawV2Reader.ReadCapture(folder, item.EventId), Tp1AnalogDecodeProfile.FieldCandidate)))
                .ToArray();
            var records = results.SelectMany(x => x.Decode.Records).ToArray();

            Assert.Equal(2577, results.Sum(x => x.Decode.Pulses.Count));
            Assert.Equal(622, results.Sum(x => x.Decode.CharacterCount));
            Assert.Equal(197, records.Length);
            Assert.Equal(14, records.Count(x => x.Classification == Tp1RecordClassification.VALID_UNKNOWN));
            Assert.Equal(63, records.Count(x => x.Classification == Tp1RecordClassification.VALID_KNOWN));
            Assert.Equal(62, records.Count(x => x.KnownControl == Tp1KnownControl.ACK));
            Assert.Equal(1, records.Count(x => x.KnownControl == Tp1KnownControl.BUSY));
            Assert.Equal(98, records.Count(x => x.Classification == Tp1RecordClassification.INVALID_PARITY));
            Assert.Equal(2, records.Count(x => x.Classification == Tp1RecordClassification.INVALID_TIMING));
            Assert.Equal(0, records.Count(x => x.Classification == Tp1RecordClassification.INVALID_CHECKSUM));
            Assert.Equal(8, records.Count(x => x.Classification == Tp1RecordClassification.INCOMPLETE));
            Assert.Equal(12, records.Count(x => x.Classification == Tp1RecordClassification.ANALOG_UNDECODED));

            var event8 = results.Single(x => x.EventId == 8).Decode;
            Assert.Equal(990, event8.Pulses.Count);
            Assert.Equal(220, event8.CharacterCount);
            Assert.Equal(54, event8.Records.Count);
            Assert.Equal(19, event8.Records.Count(x => x.KnownControl == Tp1KnownControl.ACK));
            Assert.Contains(event8.Records, x => x.Classification == Tp1RecordClassification.INVALID_PARITY);
            var event8Telegrams = event8.Records.Select(KnxTelegramDecoder.Parse)
                .Where(x => x.Telegram is not null).Select(x => x.Telegram!).ToArray();
            Assert.Equal(7, event8Telegrams.Length);
            var original = Assert.Single(event8Telegrams, x => Convert.ToHexString(x.RawBytes) == "BCFF160001E10080CA");
            Assert.False(original.Repeat);
            Assert.Contains(event8Telegrams, x => Convert.ToHexString(x.RawBytes) == "9CFF160001E10080EA");
            var repeated = event8Telegrams.First(x => Convert.ToHexString(x.RawBytes) == "9CFF160001E10080EA");
            Assert.True(repeated.Repeat);
            Assert.All(new[] { original, repeated }, telegram => {
                Assert.Equal("15.15.22", telegram.Source);
                Assert.Equal("0/0/1", telegram.Destination);
                Assert.Equal("GroupValueWrite", telegram.Service);
                Assert.True(telegram.ChecksumValid);
            });

            var parsed = records.Select(KnxTelegramDecoder.Parse).ToArray();
            var telegrams = parsed.Where(x => x.Telegram is not null).Select(x => x.Telegram!).ToArray();
            output.WriteLine($"parse-status: {string.Join(", ", parsed.GroupBy(x => x.Status).Select(x => $"{x.Key}={x.Count()}"))}");
            output.WriteLine($"destinations: {string.Join(", ", telegrams.GroupBy(x => x.DestinationKind).Select(x => $"{x.Key}={x.Count()}"))}");
            output.WriteLine($"services: {string.Join(", ", telegrams.GroupBy(x => x.Service).Select(x => $"{x.Key}={x.Count()}"))}");
            output.WriteLine($"sources: {string.Join(", ", telegrams.Select(x => x.Source).Distinct().Order())}");
            output.WriteLine($"destination-addresses: {string.Join(", ", telegrams.Select(x => x.Destination).Distinct().Order())}");
            output.WriteLine($"telegrams={telegrams.Length}");
            output.WriteLine($"event8: pulses={event8.Pulses.Count}, characters={event8.CharacterCount}, records={event8.Records.Count}, ACK={event8.Records.Count(x => x.KnownControl == Tp1KnownControl.ACK)}, telegrams={event8Telegrams.Length}");
        }
        finally {
            if (Directory.Exists(folder)) Directory.Delete(folder, true);
        }
    }
}
