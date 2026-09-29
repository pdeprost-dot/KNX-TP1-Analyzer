using System.Buffers.Binary;
using System.IO.Compression;
using KNXAnalyzer.Core;
using KNXAnalyzer.Desktop.ViewModels;

namespace KNXAnalyzer.Desktop.Tests;

public class AnalogCaptureViewModelTests
{
    [Fact]
    public void RealEvent8FieldCandidatePopulatesBoundTrafficCollections()
    {
        var archive = Path.Combine(AppContext.BaseDirectory, "Fixtures", "KNX-F35F14A4.zip");
        var folder = Path.Combine(Path.GetTempPath(), "knxstudio-traffic-replay-" + Guid.NewGuid().ToString("N"));
        try {
            ZipFile.ExtractToDirectory(archive, folder);
            var vm = new MainViewModel();
            vm.OpenFolder(folder);
            vm.AnalogEvents.CollectionChanged += (_, change) => {
                if (change.Action == System.Collections.Specialized.NotifyCollectionChangedAction.Replace)
                    vm.SelectedAnalogEvent = null;
            };
            vm.SelectedAnalogEvent = vm.AnalogEvents.Single(item => item.EventId == 8);
            Assert.Empty(vm.Participants);

            vm.SelectedOfflineProfile = Tp1AnalogDecodeProfile.FieldCandidate;

            Assert.Equal(54, vm.OfflineRecordResults.Count);
            Assert.Equal((uint)8, vm.SelectedAnalogEvent?.EventId);
            Assert.Equal(7, vm.ObservedTrafficCount);
            Assert.NotEmpty(vm.Participants);
            Assert.NotEmpty(vm.Groups);
            Assert.NotEmpty(vm.Interactions);
            Assert.Equal(vm.ParticipantCount, vm.Participants.Count);
            Assert.Equal(vm.GroupCount, vm.Groups.Count);
            Assert.Equal(vm.InteractionCount, vm.Interactions.Count);
        } finally {
            if (Directory.Exists(folder)) Directory.Delete(folder, true);
        }
    }

    [Fact]
    public void FiltersAndNavigatesTp1RecordsWithinTheVisibleList()
    {
        var vm = new MainViewModel();
        var valid = Record("AABB", Tp1RecordClassification.VALID_UNKNOWN);
        var ack = Record("CC", Tp1RecordClassification.VALID_KNOWN, Tp1KnownControl.ACK);
        var error = Record("C8", Tp1RecordClassification.INVALID_PARITY);
        vm.OfflineRecordResults.Add(valid); vm.OfflineRecordResults.Add(ack); vm.OfflineRecordResults.Add(error);

        vm.SelectedOfflineRecordFilter = "ACK";
        Assert.Equal(ack, Assert.Single(vm.OfflineCandidates));
        vm.SelectedOfflineRecordFilter = "Errors";
        Assert.Equal(error, Assert.Single(vm.OfflineCandidates));
        vm.SelectedOfflineRecordFilter = "All";
        Assert.Equal(3, vm.OfflineCandidates.Count);
        vm.SelectedOfflineCandidate = valid;
        Assert.Equal(valid, vm.SelectedOfflineCandidate);
        vm.SelectAdjacentOfflineRecord(1);
        Assert.Equal(ack, vm.SelectedOfflineCandidate);
        vm.SelectAdjacentOfflineRecord(1);
        Assert.Equal(error, vm.SelectedOfflineCandidate);
        vm.SelectAdjacentOfflineRecord(-1);
        Assert.Equal(ack, vm.SelectedOfflineCandidate);
    }

    [Fact]
    public void PresentsTelegramBusControlAndInvalidRecordWithoutInventingSemantics()
    {
        var vm = new MainViewModel();
        var telegram = ParsedRecord("BCFF160001E10080CA", Tp1RecordClassification.VALID_UNKNOWN);
        var ack = ParsedRecord("CC", Tp1RecordClassification.VALID_KNOWN, Tp1KnownControl.ACK);
        var invalid = ParsedRecord("FFFFFFFFBDFFAF", Tp1RecordClassification.INVALID_PARITY);

        vm.OfflineRecordResults.Add(telegram);
        vm.OfflineRecordResults.Add(ack);
        vm.OfflineRecordResults.Add(invalid);
        vm.SelectedOfflineRecordFilter = "All";

        vm.SelectedOfflineCandidate = telegram;
        Assert.Equal("KNX TELEGRAM", vm.OfflineFrameTitle);
        Assert.Contains("0xBC", vm.OfflineFrameControl);
        Assert.Equal("15.15.22", vm.OfflineFrameSource);
        Assert.Contains("0/0/1", vm.OfflineFrameDestination);
        Assert.Equal("GroupValueWrite", vm.OfflineFrameService);
        Assert.Equal("0080", vm.OfflineFrameApdu);
        Assert.Contains("DPT Unknown", vm.OfflineFramePayload);

        vm.SelectedOfflineCandidate = ack;
        Assert.Contains("BUS CONTROL", vm.OfflineFrameTitle);
        Assert.Equal("ACK", vm.OfflineFrameService);
        Assert.Equal("Non applicable", vm.OfflineFrameSource);

        vm.SelectedOfflineCandidate = invalid;
        Assert.Contains("TP1 DIAGNOSTIC", vm.OfflineFrameTitle);
        Assert.Equal("Non applicable", vm.OfflineFrameSource);
        Assert.Equal("Non applicable", vm.OfflineFrameService);
    }

    [Fact]
    public void DistinguishesPendingRawAnalysisFromUnavailableData()
    {
        var item = new KNXAnalyzer.Core.AnalogEvent { EventId = 1, RawPersisted = true };
        Assert.Equal("Pending", item.D44MaximumText);
        Assert.Equal("Pending", item.CrcText);
        item.RawIntegrity = KNXAnalyzer.Core.RawIntegrityStatus.MissingChunk;
        Assert.Equal("Missing", item.CrcText);
    }

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
            Assert.Equal("Invalid", vm.SelectedAnalogEvent.CrcText);
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
            Assert.NotNull(vm.SelectedTp1Decode);
            Assert.True(vm.ShowTp1Overlays);
            Assert.Equal("historical", vm.SelectedOfflineProfile.Id);
            vm.SelectedOfflineProfile = KNXAnalyzer.Core.Tp1AnalogDecodeProfile.FieldCandidate;
            Assert.Contains("Experimental / field candidate", vm.OfflineAnalysisSummary);
            vm.SelectedAnalogSort = "P-P ascending";
            Assert.Equal([3u, 2u], vm.AnalogEvents.Select(x => x.EventId));
            vm.SelectedAnalogEvent = vm.AnalogEvents[0];
            vm.SelectAdjacentEvent(1);
            Assert.Equal((uint)2, vm.SelectedAnalogEvent?.EventId);
            vm.SelectAdjacentEvent(-1);
            Assert.Equal((uint)3, vm.SelectedAnalogEvent?.EventId);
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

    private static OfflineTp1Candidate Record(string raw, Tp1RecordClassification classification,
        Tp1KnownControl control = Tp1KnownControl.None) => new(0, 100, 0, "negative", raw,
            classification is Tp1RecordClassification.VALID_KNOWN or Tp1RecordClassification.VALID_UNKNOWN
                ? OfflineAnalogClassification.TP1_VALID_FRAME : OfflineAnalogClassification.TP1_INVALID_PARITY,
            classification == Tp1RecordClassification.INVALID_PARITY ? 1 : 0, 0, true, 0, 0, null, [], classification, control);

    private static OfflineTp1Candidate ParsedRecord(string raw, Tp1RecordClassification classification,
        Tp1KnownControl control = Tp1KnownControl.None)
    {
        var bytes = Convert.FromHexString(raw);
        var parsed = classification is Tp1RecordClassification.VALID_KNOWN or Tp1RecordClassification.VALID_UNKNOWN
            ? KnxTelegramDecoder.Parse(bytes)
            : new KnxTelegramParseResult(KnxTelegramParseStatus.Invalid, KnxRecordKind.InvalidTp1Record,
                bytes, Reason: $"TP1 classification {classification}");
        return new OfflineTp1Candidate(0, 100, 0, "negative", raw,
            classification is Tp1RecordClassification.VALID_KNOWN or Tp1RecordClassification.VALID_UNKNOWN
                ? OfflineAnalogClassification.TP1_VALID_FRAME : OfflineAnalogClassification.TP1_INVALID_PARITY,
            classification == Tp1RecordClassification.INVALID_PARITY ? 1 : 0, 0, true, 0, 0,
            parsed.Telegram, [], classification, control, parsed);
    }
}
