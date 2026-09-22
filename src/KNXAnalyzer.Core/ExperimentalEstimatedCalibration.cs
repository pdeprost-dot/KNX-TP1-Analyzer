namespace KNXAnalyzer.Core;

/// <summary>One approximate prototype observation. The proportional display model is an assumption, not ADC characterization.</summary>
public static class ExperimentalEstimatedCalibration
{
    public const ushort ReferenceRaw = 1888;
    public const double ReferenceMillivolts = 1930;
    public const string Description = "Estimated ADC voltage — experimental calibration (GPIO5; one approximate point, proportional display assumption; accuracy unknown)";

    public static double EstimateMillivolts(ushort raw) => raw * ReferenceMillivolts / ReferenceRaw;
}
