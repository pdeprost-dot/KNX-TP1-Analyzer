namespace KNXAnalyzer.Core;

public sealed record Tp1AnalogDecodeProfile(
    string Id, string DisplayName, ushort LowThreshold, ushort HighThreshold, bool Experimental)
{
    public static Tp1AnalogDecodeProfile Historical { get; } =
        new("historical", "Historical 1600/1700", 1600, 1700, false);
    public static Tp1AnalogDecodeProfile FieldCandidate { get; } =
        new("field-candidate", "Field candidate 1715/1815", 1715, 1815, true);
    public static IReadOnlyList<Tp1AnalogDecodeProfile> Available { get; } = [Historical, FieldCandidate];
}

public sealed record Tp1ProfileComparison(
    AnalogAnalysis Historical,
    AnalogAnalysis FieldCandidate,
    bool ByteStreamsIdentical);
