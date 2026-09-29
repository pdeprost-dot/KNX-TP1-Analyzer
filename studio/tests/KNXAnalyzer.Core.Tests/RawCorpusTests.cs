using System.Buffers.Binary;

namespace KNXAnalyzer.Core.Tests;

public sealed class RawCorpusTests
{
    [Fact]
    public void ImportsValidContinuousRawAndPersistsAcquisitionMetadata()
    {
        WithFixture((session, catalogPath) => {
            var catalog = RawCorpusCatalog.Open(catalogPath);
            var result = catalog.Import(session, RawCorpusStorageMode.ManagedCopy);
            var entry = result.Entry;
            Assert.False(result.AlreadyImported); Assert.Equal(RawCorpusIntegrity.Valid, entry.Integrity);
            Assert.Equal("corpus-test", entry.SessionId); Assert.Equal((ulong)8192, entry.SampleCount);
            Assert.Equal(16384, entry.RawBytes); Assert.Equal(2, entry.ChunkCount); Assert.Equal("uint16_le", entry.SampleFormat);
            Assert.Equal("Maison", entry.AcquisitionMetadata.Site); Assert.Equal("Ligne A", entry.AcquisitionMetadata.Bus);
            Assert.Equal("Tableau", entry.AcquisitionMetadata.Point); Assert.Equal("soir", entry.AcquisitionMetadata.Note);
            Assert.Equal("S3-D190A994", entry.AnalyzerId); Assert.Equal("field-v-test", entry.FirmwareVersion);
            Assert.Equal("xiao-esp32s3-sense", entry.Board); Assert.Equal("knx-analog-v1", entry.Frontend);
            Assert.Equal(64 * 1024, result.Metrics.MaximumBufferBytes); Assert.True(Directory.Exists(entry.DataDirectory));
            Assert.NotEqual(Path.GetFullPath(session), entry.DataDirectory); Assert.True(File.Exists(Path.Combine(entry.DataDirectory, "raw-0000.bin")));
            Assert.Equal(64, entry.ContentSha256.Length);
            Assert.Equal(64, entry.LogicalRawSha256.Length);
        });
    }

    [Fact]
    public void DetectsDuplicateByContentHashAndReloadsIdenticalCatalog()
    {
        WithFixture((session, catalogPath) => {
            var catalog = RawCorpusCatalog.Open(catalogPath); var first = catalog.Import(session, RawCorpusStorageMode.Reference);
            var second = catalog.Import(session, RawCorpusStorageMode.ManagedCopy);
            Assert.True(second.AlreadyImported); Assert.Equal(first.Entry.CorpusId, second.Entry.CorpusId); Assert.Single(catalog.Entries);
            var reloaded = RawCorpusCatalog.Open(catalogPath); var entry = Assert.Single(reloaded.Entries);
            Assert.Equal(first.Entry.ContentSha256, entry.ContentSha256); Assert.Equal(RawCorpusStorageMode.Reference, entry.StorageMode);
            Assert.Equal("field-v-test", entry.FirmwareVersion); Assert.Equal("xiao-esp32s3-sense", entry.Board);
            Assert.Equal("knx-analog-v1", entry.Frontend);
        });
    }

    [Fact]
    public void DuplicateImportRefreshesV08ProvenanceWithoutCopyingRawAgain()
    {
        WithFixture((session, catalogPath) => {
            var startPath = Path.Combine(session, "session-start.json");
            var current = File.ReadAllText(startPath);
            File.WriteAllText(startPath, current.Replace("\"firmware\":\"field-v-test\",\"board_profile\":\"xiao-esp32s3-sense\",\"frontend_profile\":\"knx-analog-v1\",", ""));
            var catalog = RawCorpusCatalog.Open(catalogPath);
            var first = catalog.Import(session, RawCorpusStorageMode.Reference);
            Assert.Null(first.Entry.FirmwareVersion); Assert.Null(first.Entry.Board); Assert.Null(first.Entry.Frontend);

            File.WriteAllText(startPath, current);
            var duplicate = catalog.Import(session, RawCorpusStorageMode.ManagedCopy);
            Assert.True(duplicate.AlreadyImported); Assert.Equal(RawCorpusStorageMode.Reference, duplicate.Entry.StorageMode);
            Assert.Equal("field-v-test", duplicate.Entry.FirmwareVersion); Assert.Equal("xiao-esp32s3-sense", duplicate.Entry.Board);
            Assert.Equal("knx-analog-v1", duplicate.Entry.Frontend);
            var persisted = Assert.Single(RawCorpusCatalog.Open(catalogPath).Entries);
            Assert.Equal(duplicate.Entry.FirmwareVersion, persisted.FirmwareVersion);
            Assert.Equal(duplicate.Entry.Board, persisted.Board); Assert.Equal(duplicate.Entry.Frontend, persisted.Frontend);
        });
    }

    [Fact]
    public void ReportsMissingRawAsIncomplete()
    {
        WithFixture((session, catalogPath) => {
            File.Delete(Path.Combine(session, "raw-0000.bin"));
            var entry = RawCorpusCatalog.Open(catalogPath).Import(session, RawCorpusStorageMode.Reference).Entry;
            Assert.Equal(RawCorpusIntegrity.Incomplete, entry.Integrity); Assert.Contains(entry.IntegrityMessages, x => x.Contains("Missing RAW"));
        });
    }

    [Fact]
    public void DetectsControlledCrcCorruption()
    {
        WithFixture((session, catalogPath) => {
            var path = Path.Combine(session, "raw-0000.bin"); using (var stream = new FileStream(path, FileMode.Open, FileAccess.Write)) { stream.Position = 9000; stream.WriteByte(0xFF); }
            var entry = RawCorpusCatalog.Open(catalogPath).Import(session, RawCorpusStorageMode.Reference).Entry;
            Assert.Equal(RawCorpusIntegrity.Corrupted, entry.Integrity); Assert.Contains(entry.IntegrityMessages, x => x.Contains("CRC mismatch"));
        });
    }

    [Fact]
    public void DetectsDeclaredTotalsMismatch()
    {
        WithFixture((session, catalogPath) => {
            File.WriteAllText(Path.Combine(session, "manifest.json"), """
                {"schema_version":"knx-continuous-raw-manifest-1.0","session_id":"corpus-test","acquisition_mode":"CONTINUOUS_RAW","sample_format":"uint16_le","lifecycle":"CLOSED","completion_status":"COMPLETE","chunk_count":2,"total_samples":42,"total_raw_bytes":16384,"continuity_complete":true}
                """);
            var entry = RawCorpusCatalog.Open(catalogPath).Import(session, RawCorpusStorageMode.Reference).Entry;
            Assert.Equal(RawCorpusIntegrity.Corrupted, entry.Integrity);
            Assert.Contains(entry.IntegrityMessages, x => x.Contains("sample count"));
        });
    }

    [Fact]
    public void RejectsInvalidManifestCleanly()
    {
        WithFixture((session, catalogPath) => {
            File.WriteAllText(Path.Combine(session, "manifest.json"), "{invalid");
            var error = Assert.Throws<InvalidDataException>(() => RawCorpusCatalog.Open(catalogPath).Import(session));
            Assert.Contains("Invalid manifest.json", error.Message);
        });
    }

    [Fact]
    public void LargeImportUsesBoundedStreamingBuffer()
    {
        WithFixture((session, catalogPath) => {
            CreateFixture(session, 256);
            var result = RawCorpusCatalog.Open(catalogPath).Import(session, RawCorpusStorageMode.Reference);
            Assert.Equal(RawCorpusIntegrity.Valid, result.Entry.Integrity); Assert.Equal(256, result.Entry.ChunkCount);
            Assert.Equal(2_097_152, result.Entry.RawBytes); Assert.Equal(64 * 1024, result.Metrics.MaximumBufferBytes);
        });
    }

    [Fact]
    public void LocalMetadataIsSeparateFromOriginalManifest()
    {
        WithFixture((session, catalogPath) => {
            var before = File.ReadAllBytes(Path.Combine(session, "manifest.json")); var catalog = RawCorpusCatalog.Open(catalogPath);
            var entry = catalog.Import(session, RawCorpusStorageMode.Reference).Entry;
            catalog.UpdateLocalMetadata(entry.CorpusId, new("Reference salon", "Analyse ultérieure", ["site-a", "quiet"]));
            Assert.Equal(before, File.ReadAllBytes(Path.Combine(session, "manifest.json")));
            var local = Assert.Single(RawCorpusCatalog.Open(catalogPath).Entries).LocalMetadata;
            Assert.Equal("Reference salon", local.Alias); Assert.NotNull(local.Tags); Assert.Equal(["site-a", "quiet"], local.Tags);
        });
    }

    private static void WithFixture(Action<string, string> action)
    {
        var root = Path.Combine(Path.GetTempPath(), "knx-corpus-test-" + Guid.NewGuid().ToString("N")); var session = Path.Combine(root, "source");
        Directory.CreateDirectory(session); CreateFixture(session, 2);
        try { action(session, Path.Combine(root, "catalog", "catalog.json")); }
        finally { if (Directory.Exists(root)) Directory.Delete(root, true); }
    }

    private static void CreateFixture(string directory, int chunks)
    {
        Directory.CreateDirectory(directory);
        File.WriteAllText(Path.Combine(directory, "session-start.json"), """
            {"schema_version":"knx-long-session-1.1","session_id":"corpus-test","analyzer_id":"S3-D190A994","firmware":"field-v-test","board_profile":"xiao-esp32s3-sense","frontend_profile":"knx-analog-v1","acquisition_mode":"CONTINUOUS_RAW","sample_format":"uint16_le","sample_rate_hz":83333,"start_utc":"2026-09-29T10:00:00Z","field_campaign":{"schema":"knx-field-campaign-1.0","site":"Maison","bus":"Ligne A","point":"Tableau","note":"soir","requested_duration_s":60}}
            """);
        File.WriteAllText(Path.Combine(directory, "test-result.json"), $"{{\"lifecycle\":\"CLOSED\",\"completion_status\":\"COMPLETE\",\"adc_capture_duration_us\":\"{chunks * 49152L}\",\"sample_rate_measured_hz\":83333.0,\"gap_count\":0,\"lost_samples\":0,\"adc_read_errors\":0,\"sd_errors\":0}}");
        File.WriteAllText(Path.Combine(directory, "manifest.json"), $"{{\"schema_version\":\"knx-continuous-raw-manifest-1.0\",\"session_id\":\"corpus-test\",\"acquisition_mode\":\"CONTINUOUS_RAW\",\"sample_format\":\"uint16_le\",\"lifecycle\":\"CLOSED\",\"completion_status\":\"COMPLETE\",\"chunk_count\":{chunks},\"total_samples\":{chunks * 4096L},\"total_raw_bytes\":{chunks * 8192L},\"continuity_complete\":true}}");
        using var raw = new FileStream(Path.Combine(directory, "raw-0000.bin"), FileMode.Create, FileAccess.Write); using var map = new StreamWriter(Path.Combine(directory, "chunks.jsonl"));
        for (var chunk = 0; chunk < chunks; chunk++) {
            var bytes = new byte[8192]; for (var i = 0; i < 4096; i++) BinaryPrimitives.WriteUInt16LittleEndian(bytes.AsSpan(i * 2), (ushort)((chunk * 4096 + i) & 0x0FFF));
            var crc = Crc(bytes); raw.Write(bytes); var start = (ulong)chunk * 4096;
            map.WriteLine($"{{\"sample_start\":\"{start}\",\"sample_end\":\"{start + 4096}\",\"sample_count\":4096,\"segment_index\":0,\"segment_offset\":\"{chunk * 8192L}\",\"raw_bytes\":8192,\"crc32\":\"{crc:X8}\"}}");
        }
        File.WriteAllText(Path.Combine(directory, "events.jsonl"), ""); File.WriteAllText(Path.Combine(directory, "gaps.jsonl"), "");
    }

    private static uint Crc(ReadOnlySpan<byte> bytes) { uint crc = 0xFFFFFFFF; foreach (var b in bytes) { crc ^= b; for (var i = 0; i < 8; i++) crc = (crc >> 1) ^ ((crc & 1) != 0 ? 0xEDB88320u : 0u); } return crc ^ 0xFFFFFFFF; }
}
