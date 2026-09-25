using KNXAnalyzer.Desktop.ViewModels;

namespace KNXAnalyzer.Desktop.Tests;

public class TrafficViewModelTests
{
    [Fact]
    public void BuildsFrenchSummaryAndReadableMasterDetailRowsWithoutSyntheticTraffic()
    {
        var root = Path.Combine(Path.GetTempPath(), "knxstudio-traffic-ui-" + Guid.NewGuid().ToString("N"));
        try {
            var first = Frame(0x1120, 0x1814);
            var second = Frame(0x11D9, 0x1914);
            MakeSession(root, "s-a", (0L, first, false), (27_000L, second, false), (90_000L, first, true));
            MakeSession(root, "s-b", (0L, first, false), (25_000L, second, false));
            var vm = new MainViewModel();
            vm.OpenFolder(root);

            Assert.Equal(4, vm.ObservedTrafficCount);
            Assert.Equal(1, vm.SyntheticExcludedCount);
            Assert.Equal(2, vm.ParticipantCount);
            Assert.Equal(2, vm.GroupCount);
            Assert.Equal(1, vm.InteractionCount);
            var participant = vm.Participants.Single(x => x.Address == "1.1.32");
            Assert.Equal(50, participant.TrafficPercent);
            Assert.Contains("1.1.32 → 3/0/20", vm.Interactions[0].Sequence);
            Assert.Contains("1.1.217 → 3/1/20", vm.Interactions[0].Sequence);
            Assert.NotEmpty(vm.ParticipantDestinations);
        } finally { Directory.Delete(root, true); }
    }

    private static void MakeSession(string root, string id, params (long Time, string Hex, bool Synthetic)[] candidates)
    {
        var folder = Path.Combine(root, "sessions", id);
        Directory.CreateDirectory(folder);
        File.WriteAllText(Path.Combine(folder, "session.json"), $"{{\"session_id\":\"{id}\",\"state\":\"CLOSED\"}}");
        File.WriteAllLines(Path.Combine(folder, "tp1-candidates.jsonl"), candidates.Select(x =>
            $"{{\"monotonic_us\":{x.Time},\"classification\":\"VALID_UNKNOWN\",\"raw_hex\":\"{x.Hex}\",\"synthetic_test\":{x.Synthetic.ToString().ToLowerInvariant()}}}"));
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
