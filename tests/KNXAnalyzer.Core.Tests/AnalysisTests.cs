using System.Text.Json;
using KNXAnalyzer.Core;

namespace KNXAnalyzer.Core.Tests;

public class AnalysisTests
{
    [Fact]
    public void StandardTelegramFieldsAndChecksumAreDerivedOnlyFromValidBytes()
    {
        var bytes = Convert.FromHexString("BC12342110E1008035");
        var frame = Assert.IsType<KnxTelegram>(KnxTelegramDecoder.Decode(bytes));
        Assert.Equal("Standard", frame.Format);
        Assert.Equal("1.2.52", frame.Source);
        Assert.Equal("4/1/16", frame.Destination);
        Assert.Equal("group", frame.DestinationType);
        Assert.Equal(6, frame.HopCount);
        Assert.Equal(1, frame.Length);
        Assert.Equal("Unnumbered data", frame.Tpci);
        Assert.Equal(2, frame.Apci);
        Assert.Equal("GroupValueWrite", frame.Service);
        Assert.Equal("0080", frame.ApduHex);
        Assert.Equal("", frame.PayloadHex);
        Assert.Equal("Low", frame.Priority);
        Assert.False(frame.Repeat);
        bytes[^1] ^= 1;
        Assert.Null(KnxTelegramDecoder.Decode(bytes));
    }

    [Fact]
    public void ExperimentalEstimateHasExplicitSinglePointAndExportsKeepOriginal()
    {
        Assert.Equal(1930, ExperimentalEstimatedCalibration.EstimateMillivolts(1888));
        using var doc = JsonDocument.Parse("""{"session_id":"s-synthetic","state":"CLOSED"}""");
        var session = new Session { DirectoryPath = Path.GetTempPath(), Id = "s-synthetic", State = "CLOSED", Metadata = doc.RootElement.Clone() };
        var json = SessionAnalysis.ToJson(session);
        using var output = JsonDocument.Parse(json);
        Assert.Equal("1.0", output.RootElement.GetProperty("schema_version").GetString());
        Assert.Equal(0, output.RootElement.GetProperty("statistics").GetProperty("total_candidates").GetInt32());
        Assert.Equal("s-synthetic", output.RootElement.GetProperty("session").GetProperty("recorded").GetProperty("session_id").GetString());
        var report = SessionAnalysis.ToFrenchReport(session);
        Assert.Contains("NON DÉTERMINABLE", report);
        Assert.Contains("calibration expérimentale", report);
    }
}
