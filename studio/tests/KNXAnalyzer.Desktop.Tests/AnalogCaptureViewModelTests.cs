using System.Buffers.Binary;
using KNXAnalyzer.Desktop.ViewModels;

namespace KNXAnalyzer.Desktop.Tests;

public class AnalogCaptureViewModelTests
{
    [Fact]
    public void RefusesToDecodeLocalRawWhenCrcIsInvalid()
    {
        var root = Path.Combine(Path.GetTempPath(), "knxstudio-invalid-crc-" + Guid.NewGuid().ToString("N"));
        try {
            MakeSession(root, "s-invalid", (9u, new ushort[] { 100, 200, 300 }));
            var rawPath = Path.Combine(root, "sessions", "s-invalid", "captures", "event-000009.bin");
            var bytes = File.ReadAllBytes(rawPath);
            bytes[^1] ^= 1;
            File.WriteAllBytes(rawPath, bytes);
            var vm = new MainViewModel();
            vm.OpenFolder(root);
            vm.SelectedAnalogEvent = Assert.Single(vm.AnalogEvents);
            Assert.Null(vm.SelectedCapture);
            Assert.Contains("CRC is invalid; decode refused", vm.AnalogDetails);
            Assert.Empty(vm.OfflineCandidates);
        } finally { Directory.Delete(root, true); }
    }

    [Fact]
    public void SortsSyntheticCapturesAndSelectsLargestAcrossSessions()
    {
        var root = Path.Combine(Path.GetTempPath(), "knxstudio-analog-" + Guid.NewGuid().ToString("N"));
        try {
            MakeSession(root, "s-a", (1u, new ushort[] { 10, 10, 11, 10 }));
            MakeSession(root, "s-b", (2u, new ushort[] { 100, 300, 120, 250 }),
                (3u, new ushort[] { 100, 130, 115, 120 }));
            var vm = new MainViewModel();
            vm.OpenFolder(root);
            Assert.Equal(2, vm.Sessions.Count);
            vm.SelectLargestCapture();
            Assert.Equal("s-b", vm.SelectedSession?.Id);
            Assert.Equal((uint)2, vm.SelectedAnalogEvent?.EventId);
            Assert.Equal(200, vm.SelectedCapture?.PeakToPeak);
            Assert.Equal(1, vm.SelectedTabIndex);
            Assert.Equal([2u, 3u], vm.AnalogEvents.Select(x => x.EventId));
            Assert.Contains("P-P 200", vm.AnalogDetails);
            Assert.Contains("CRC OK", vm.AnalogDetails);
            Assert.Empty(vm.AnalogVariationNotice);
            Assert.Equal(4, vm.OfflineSampleCount);
            Assert.StartsWith("4", vm.OfflineDuration);
            Assert.Equal(0, vm.OfflineValidFrameCount);
            Assert.Equal("historical", vm.SelectedOfflineProfile.Id);
            vm.SelectedOfflineProfile = KNXAnalyzer.Core.Tp1AnalogDecodeProfile.FieldCandidate;
            Assert.Contains("Experimental / field candidate", vm.OfflineAnalysisSummary);
            vm.SelectedAnalogSort = "P-P ascending";
            Assert.Equal([3u, 2u], vm.AnalogEvents.Select(x => x.EventId));
            vm.SelectedSession = vm.Sessions.Single(x => x.Id == "s-a");
            vm.SelectedAnalogEvent = Assert.Single(vm.AnalogEvents);
            Assert.Equal("No analog variation in this capture", vm.AnalogVariationNotice);
            Assert.Equal("1", vm.SelectedAnalogEvent.PeakToPeakText);
            Assert.Equal("OK", vm.SelectedAnalogEvent.CrcText);
        } finally { Directory.Delete(root, true); }
    }

    private static void MakeSession(string root, string id, params (uint EventId, ushort[] Samples)[] captures)
    {
        var folder = Path.Combine(root, "sessions", id);
        var captureFolder = Path.Combine(folder, "captures");
        Directory.CreateDirectory(captureFolder);
        File.WriteAllText(Path.Combine(folder, "session.json"),
            $"{{\"session_id\":\"{id}\",\"state\":\"CLOSED\",\"event_count\":{captures.Length}}}");
        var lines = new List<string>();
        foreach (var (eventId, samples) in captures) {
            lines.Add($"{{\"event_id\":{eventId},\"session_id\":\"{id}\",\"raw_persisted\":true,\"sample_count\":{samples.Length}}}");
            var data = new byte[48 + samples.Length * 2];
            "KNXADC1\0"u8.CopyTo(data);
            BinaryPrimitives.WriteUInt16LittleEndian(data.AsSpan(8), 1);
            BinaryPrimitives.WriteUInt16LittleEndian(data.AsSpan(10), 48);
            BinaryPrimitives.WriteUInt32LittleEndian(data.AsSpan(12), eventId);
            BinaryPrimitives.WriteUInt32LittleEndian(data.AsSpan(16), 1000);
            BinaryPrimitives.WriteUInt32LittleEndian(data.AsSpan(20), (uint)samples.Length);
            BinaryPrimitives.WriteUInt32LittleEndian(data.AsSpan(24), 2);
            BinaryPrimitives.WriteUInt16LittleEndian(data.AsSpan(36), samples.Min());
            BinaryPrimitives.WriteUInt16LittleEndian(data.AsSpan(38), samples.Max());
            for (var i = 0; i < samples.Length; i++)
                BinaryPrimitives.WriteUInt16LittleEndian(data.AsSpan(48 + i * 2), samples[i]);
            uint crc = 0xFFFFFFFF;
            foreach (var b in data.AsSpan(48)) {
                crc ^= b;
                for (var i = 0; i < 8; i++)
                    crc = (crc >> 1) ^ ((crc & 1) != 0 ? 0xEDB88320u : 0u);
            }
            BinaryPrimitives.WriteUInt32LittleEndian(data.AsSpan(40), crc ^ 0xFFFFFFFF);
            File.WriteAllBytes(Path.Combine(captureFolder, $"event-{eventId:D6}.bin"), data);
        }
        File.WriteAllLines(Path.Combine(folder, "events.jsonl"), lines);
    }
}
