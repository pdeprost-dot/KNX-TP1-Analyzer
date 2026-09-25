using System.Text.Json;
using KNXAnalyzer.Core;

namespace KNXAnalyzer.Core.Tests;

public class DatasetAnalysisTests
{
    [Fact]
    public void AggregatesSessionsWithProvenanceAndWritesOnlyAnalyzableSessions()
    {
        using var meta1 = JsonDocument.Parse("""{"session_id":"s-one","adc_overruns":2,"dma_errors":1,"sd_write_errors":0}""");
        using var meta2 = JsonDocument.Parse("""{"session_id":"s-two"}""");
        using var empty = JsonDocument.Parse("""{"session_id":"s-empty"}""");
        using var original = JsonDocument.Parse("""{"classification":"VALID_UNKNOWN","raw_hex":"BC12342110E1008035"}""");
        var one = new Session { DirectoryPath = Path.GetTempPath(), Id = "s-one", State = "CLOSED", DurationMs = 1000, Metadata = meta1.RootElement.Clone() };
        one.Candidates.Add(new Tp1Candidate { Line = 4, Classification = "VALID_UNKNOWN", RawHex = "BC12342110E1008035", RawBytes = Convert.FromHexString("BC12342110E1008035"), Original = original.RootElement.Clone() });
        one.Candidates.Add(new Tp1Candidate { Line = 5, Classification = "INVALID_PARITY", RawHex = "BC", RawBytes = [0xBC], Original = original.RootElement.Clone() });
        var two = new Session { DirectoryPath = Path.GetTempPath(), Id = "s-two", State = "INTERRUPTED", DurationMs = 2000, Metadata = meta2.RootElement.Clone() };
        two.Candidates.Add(new Tp1Candidate { Line = 1, Classification = "VALID_KNOWN", RawHex = "CC", RawBytes = [0xCC], Original = original.RootElement.Clone() });
        var none = new Session { DirectoryPath = Path.GetTempPath(), Id = "s-empty", State = "CLOSED", Metadata = empty.RootElement.Clone() };

        var json = DatasetAnalysis.ToJson([one, two, none]);
        using var doc = JsonDocument.Parse(json);
        var root = doc.RootElement;
        Assert.Equal("dataset", root.GetProperty("analysis_scope").GetString());
        Assert.Equal(3, root.GetProperty("dataset").GetProperty("session_count").GetInt32());
        Assert.Equal(2, root.GetProperty("dataset").GetProperty("analyzable_session_count").GetInt32());
        Assert.Equal(3, root.GetProperty("global_statistics").GetProperty("total_candidates").GetInt32());
        Assert.Equal(1, root.GetProperty("acquisition_health").GetProperty("dma_errors").GetInt32());
        Assert.Equal("s-one", root.GetProperty("errors")[0].GetProperty("session_id").GetString());
        Assert.Equal(900, root.GetProperty("timeline").GetProperty("bucket_duration_seconds").GetInt32());
        Assert.Equal("s-one", root.GetProperty("participants")[0].GetProperty("sessions")[0].GetString());

        var target = Path.Combine(Path.GetTempPath(), "knx-analysis-test-" + Guid.NewGuid().ToString("N"));
        try {
            DatasetAnalysis.WriteBatch(target, [one, two, none]);
            Assert.True(File.Exists(Path.Combine(target, "rapport-global.txt")));
            Assert.True(File.Exists(Path.Combine(target, "analysis-global.json")));
            Assert.True(File.Exists(Path.Combine(target, "sessions", "s-one", "analysis.json")));
            Assert.True(File.Exists(Path.Combine(target, "sessions", "s-two", "rapport.txt")));
            Assert.False(Directory.Exists(Path.Combine(target, "sessions", "s-empty")));
        } finally { Directory.Delete(target, true); }
    }
}
