using System.Text.Json;
using KNXAnalyzer.Core;

namespace KNXAnalyzer.Core.Tests;

public class SessionReaderTests
{
    [Fact]
    public void OpensSegmentedFieldSessionFromMetadataAndReadsOnlySelectedEventChunks()
    {
        var folder = Path.Combine(Path.GetTempPath(), "knxstudio-segmented-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(folder);
        try {
            File.WriteAllText(Path.Combine(folder, "session-start.json"), """{"schema_version":"knx-long-session-1.0","analyzer_id":"analyzer-test","session_id":"session-test"}""");
            File.WriteAllText(Path.Combine(folder, "test-result.json"), """{"closed":true,"duration_us":"7200000000","raw_bytes":"8"}""");
            File.WriteAllText(Path.Combine(folder, "events.jsonl"), """{"record_type":"EVENT","event_id":"7","sample_start":"101","sample_trigger":"102","sample_end":"103","trigger_timestamp_us":"3600123456","trigger_source":1}""" + Environment.NewLine);
            var bytes = new byte[8];
            System.Buffers.Binary.BinaryPrimitives.WriteUInt16LittleEndian(bytes.AsSpan(0), 999);
            System.Buffers.Binary.BinaryPrimitives.WriteUInt16LittleEndian(bytes.AsSpan(2), 1000);
            System.Buffers.Binary.BinaryPrimitives.WriteUInt16LittleEndian(bytes.AsSpan(4), 1100);
            System.Buffers.Binary.BinaryPrimitives.WriteUInt16LittleEndian(bytes.AsSpan(6), 1200);
            var crc = NetworkImportService.Crc32(bytes);
            File.WriteAllText(Path.Combine(folder, "chunks.jsonl"), $"{{\"sample_start\":\"100\",\"sample_end\":\"104\",\"sample_count\":4,\"segment_index\":0,\"segment_offset\":\"0\",\"raw_bytes\":8,\"crc32\":\"{crc:X8}\"}}" + Environment.NewLine);

            var session = EventRawV2Reader.OpenSession(folder);
            Assert.Equal("session-test", session.Id);
            Assert.Equal("LOCAL / SD · analyzer-test", session.Analyzer);
            Assert.Equal("CLOSED", session.State);
            Assert.Equal(7_200_000, session.DurationMs);
            Assert.Equal(8, session.RawAvailableBytes);
            var item = Assert.Single(session.AnalogEvents);
            Assert.Equal("01:00:00.123", item.PositionText);
            Assert.Equal("D44", item.Kind);
            Assert.Equal("2 samples", item.WindowText);
            Assert.Null(item.CaptureSummary);

            File.WriteAllText(Path.Combine(folder, "test-result.json"), """{"closed":true,"duration_us":7200000000,"raw_bytes":"8"}""");
            Assert.Equal(7_200_000, EventRawV2Reader.OpenSession(folder).DurationMs);

            File.WriteAllBytes(Path.Combine(folder, "raw-0000.bin"), bytes);
            var capture = EventRawV2Reader.ReadCapture(folder, 7);
            Assert.Equal(new ushort[] { 1000, 1100 }, capture.Samples);
            Assert.Equal((uint)1, capture.TriggerIndex);
            Assert.True(capture.CrcValid);
        } finally { Directory.Delete(folder, true); }
    }

    [Fact]
    public void ReadsRealFirmwareFieldNamesAndSurvivesFutureAndCorruptLines()
    {
        var root = Path.Combine(Path.GetTempPath(), "knxstudio-test-" + Guid.NewGuid().ToString("N"));
        var sessionDir = Path.Combine(root, "knx-analyzer", "sessions", "s-test");
        Directory.CreateDirectory(sessionDir);
        try {
            File.WriteAllText(Path.Combine(sessionDir, "session.json"), """{"session_id":"s-test","state":"CLOSED","date_time":null,"duration_ms":1200,"event_count":0,"capture_format":1,"future_field":42}""");
            File.WriteAllText(Path.Combine(sessionDir, "tp1-candidates.jsonl"),
                """{"type":"TP1_CANDIDATE","monotonic_us":123456,"date_time":null,"classification":"VALID_UNKNOWN","raw_hex":"BC110A0A0100E100","bytes":8,"synthetic_test":true,"future_field":{"x":1}}""" + "\n" +
                """{"type":"TP1_CANDIDATE","classification":"FUTURE_CLASS","raw_hex":"CC"}""" + "\n" +
                "{invalid\n" +
                """{"type":"TP1_CANDIDATE","classification":"VALID_KNOWN","raw_hex":"CC"}""" + "\n");
            var session = Assert.Single(SessionReader.OpenFolder(root));
            Assert.Equal("CLOSED", session.State);
            Assert.Equal(1200, session.DurationMs);
            Assert.Equal(3, session.Candidates.Count);
            Assert.Single(session.Diagnostics);
            Assert.Equal(3, session.Diagnostics[0].Line);
            Assert.Equal("FUTURE_CLASS", session.Candidates[1].Classification);
            Assert.Equal([0xCC], session.Candidates[1].RawBytes);
            Assert.Null(session.Candidates[1].MonotonicUs);
            Assert.Equal(42, session.Metadata.GetProperty("future_field").GetInt32());
            Assert.True(session.Candidates[0].Original.TryGetProperty("future_field", out _));
            Assert.True(session.Candidates[0].IsValid);
            Assert.True(session.Candidates[0].SyntheticTest);
            Assert.Equal("ACK", session.Candidates[2].Ack);
        } finally { Directory.Delete(root, true); }
    }

    [Fact]
    public void DerivesMissingGenericFieldsFromSyntheticValidStandardFrame()
    {
        var folder = Path.Combine(Path.GetTempPath(), "knxstudio-missing-fields-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(folder);
        try {
            File.WriteAllText(Path.Combine(folder, "session.json"), """{"session_id":"s-synthetic","state":"CLOSED"}""");
            File.WriteAllText(Path.Combine(folder, "tp1-candidates.jsonl"),
                """{"classification":"VALID_UNKNOWN","raw_hex":"BC12342110E1008035","bytes":9}""" + "\n" +
                """{"classification":"VALID_UNKNOWN","raw_hex":"BC12342110E1008034","bytes":9}""" + "\n");
            var session = SessionReader.OpenSession(folder);
            var frame = session.Candidates[0];
            Assert.True(frame.GenericFieldsFromRaw);
            Assert.Equal("1.2.52", frame.Source);
            Assert.Equal("4/1/16", frame.Destination);
            Assert.Equal("group", frame.DestinationType);
            Assert.Equal(6, frame.HopCount);
            Assert.Equal(1, frame.TpLength);
            Assert.False(session.Candidates[1].GenericFieldsFromRaw);
            Assert.Null(session.Candidates[1].Source);
        } finally { Directory.Delete(folder, true); }
    }

    [Fact]    public void ReadsRawHeaderSamplesAndCrc()
    {
        var file = Path.GetTempFileName();
        try {
            var data = new byte[52];
            "KNXADC1\0"u8.CopyTo(data);
            System.Buffers.Binary.BinaryPrimitives.WriteUInt16LittleEndian(data.AsSpan(8), 1);
            System.Buffers.Binary.BinaryPrimitives.WriteUInt16LittleEndian(data.AsSpan(10), 48);
            System.Buffers.Binary.BinaryPrimitives.WriteUInt32LittleEndian(data.AsSpan(12), 7);
            System.Buffers.Binary.BinaryPrimitives.WriteUInt32LittleEndian(data.AsSpan(16), 83333);
            System.Buffers.Binary.BinaryPrimitives.WriteUInt32LittleEndian(data.AsSpan(20), 2);
            System.Buffers.Binary.BinaryPrimitives.WriteUInt32LittleEndian(data.AsSpan(24), 1);
            data[48] = 1; data[50] = 2;
            uint crc = 0xFFFFFFFF;
            foreach (var b in data.AsSpan(48)) { crc ^= b; for (var i = 0; i < 8; i++) crc = (crc >> 1) ^ ((crc & 1) != 0 ? 0xEDB88320u : 0u); }
            System.Buffers.Binary.BinaryPrimitives.WriteUInt32LittleEndian(data.AsSpan(40), crc ^ 0xFFFFFFFF);
            File.WriteAllBytes(file, data);
            var capture = RawCapture.Read(file);
            Assert.Equal((uint)7, capture.EventId);
            Assert.Equal([1, 2], capture.Samples);
            Assert.Equal((ushort)1, capture.Minimum);
            Assert.Equal((ushort)2, capture.Maximum);
            Assert.Equal(1, capture.PeakToPeak);
            Assert.Equal(1.5, capture.Mean);
            Assert.True(capture.CrcValid);
            Assert.Equal(1, capture.Summary.PeakToPeak);
        } finally { File.Delete(file); }
    }
}
