using System.Text.Json;
using KNXAnalyzer.Core;

namespace KNXAnalyzer.Core.Tests;

public class TrafficAnalyzerTests
{
    [Fact]
    public void SeparatesSyntheticAndAggregatesParticipantsGroupsAndRecurringPairs()
    {
        using var metadata = JsonDocument.Parse("""{"session_id":"s"}""");
        using var originalObserved = JsonDocument.Parse("""{"synthetic_test":false}""");
        using var originalSynthetic = JsonDocument.Parse("""{"synthetic_test":true}""");
        Session Make(string id) => new() { DirectoryPath = Path.GetTempPath(), Id = id, State = "CLOSED", Metadata = metadata.RootElement.Clone() };
        Tp1Candidate Candidate(int line, long us, string hex, bool synthetic) => new() {
            Line = line, MonotonicUs = us, Classification = "VALID_UNKNOWN", RawHex = hex,
            RawBytes = Convert.FromHexString(hex), SyntheticTest = synthetic,
            Original = (synthetic ? originalSynthetic : originalObserved).RootElement.Clone()
        };
        var a = Make("a");
        var first = Frame(0x1120, 0x1814);
        var second = Frame(0x11D9, 0x1914);
        var third = Frame(0x11D9, 0x1A16);
        a.Candidates.Add(Candidate(1, 0, first, false));
        a.Candidates.Add(Candidate(2, 27_000, second, false));
        a.Candidates.Add(Candidate(3, 48_000, third, false));
        a.Candidates.Add(Candidate(4, 100_000, first, true));
        var b = Make("b");
        b.Candidates.Add(Candidate(1, 0, first, false));
        b.Candidates.Add(Candidate(2, 25_000, second, false));

        var analysis = TrafficAnalyzer.Analyze([a, b]);
        Assert.Equal(5, analysis.ObservedTelegramCount);
        Assert.Equal(1, analysis.SyntheticTelegramCount);
        Assert.Equal(2, analysis.Participants.Single(x => x.Address == "1.1.32").TelegramCount);
        Assert.Equal(2, analysis.Participants.Single(x => x.Address == "1.1.32").SessionCount);
        Assert.Equal(2, analysis.Groups.Single(x => x.Address == "3/0/20").TelegramCount);
        var interaction = Assert.Single(analysis.Interactions, x => x.A.Source == "1.1.32" && x.B.Source == "1.1.217");
        Assert.Equal(2, interaction.Occurrences);
        Assert.Equal(2, interaction.SessionCount);
        Assert.Equal(26, interaction.MedianDelayMs);
    }

    private static string Frame(ushort source, ushort destination)
    {
        byte[] bytes = [0xBC, (byte)(source >> 8), (byte)source, (byte)(destination >> 8), (byte)destination, 0xE1, 0x00, 0x81, 0];
        byte checksum = 0xFF;
        for (var i = 0; i < bytes.Length - 1; i++) checksum ^= bytes[i];
        bytes[^1] = checksum;
        return Convert.ToHexString(bytes);
    }
}
