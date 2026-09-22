using System.Text.Json;
using KNXAnalyzer.Core;

namespace KNXAnalyzer.Core.Tests;

public class SessionReaderTests
{
    [Fact]
    public void ReadsRealFirmwareFieldNamesAndSurvivesFutureAndCorruptLines()
    {
        var root = Path.Combine(Path.GetTempPath(), "knxstudio-test-" + Guid.NewGuid().ToString("N"));
        var sessionDir = Path.Combine(root, "knx-analyzer", "sessions", "s-test");
        Directory.CreateDirectory(sessionDir);
        try {
            File.WriteAllText(Path.Combine(sessionDir, "session.json"), """{"session_id":"s-test","state":"CLOSED","date_time":null,"duration_ms":1200,"event_count":0,"capture_format":1,"future_field":42}""");
            File.WriteAllText(Path.Combine(sessionDir, "tp1-candidates.jsonl"),
                """{"type":"TP1_CANDIDATE","monotonic_us":123456,"date_time":null,"classification":"VALID_UNKNOWN","raw_hex":"BC110A0A0100E100","bytes":8,"future_field":{"x":1}}""" + "\n" +
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
            Assert.Equal("ACK", session.Candidates[2].Ack);
        } finally { Directory.Delete(root, true); }
    }

    [Fact]
    public void ReadsRawHeaderSamplesAndCrc()
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
            Assert.True(capture.CrcValid);
        } finally { File.Delete(file); }
    }
}
