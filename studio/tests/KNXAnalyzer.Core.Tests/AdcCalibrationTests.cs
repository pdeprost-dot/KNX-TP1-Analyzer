using System.Text.Json;
using KNXAnalyzer.Core;

namespace KNXAnalyzer.Core.Tests;

public class AdcCalibrationTests
{
    [Fact]
    public void RequiresExplicitCharacterizedPointsAndDoesNotExtrapolate()
    {
        Assert.True(AdcCalibration.TryCreate("synthetic bench calibration",
            [new(1000, 500), new(2000, 1400), new(3000, 2500)], out var calibration));
        Assert.NotNull(calibration);
        Assert.True(calibration.TryEstimateMillivolts(1500, out var millivolts));
        Assert.Equal(950, millivolts);
        Assert.False(calibration.TryEstimateMillivolts(999, out _));
        Assert.False(AdcCalibration.TryCreate("", [new(1000, 500), new(2000, 1400)], out _));
        Assert.False(AdcCalibration.TryCreate("invalid", [new(2000, 1400), new(1000, 500)], out _));
    }

    [Fact]
    public void FutureMetadataBuildsExplicitVoltageChainOnly()
    {
        using var future = JsonDocument.Parse("""
            {"adc_calibration":{"source":"synthetic bench calibration",
              "points":[{"raw":1000,"millivolts":500},{"raw":2000,"millivolts":1400}]},
             "analog_front_end":{"source":"synthetic divider","gpio5_per_bus_ratio":0.1}}
            """);
        var pipeline = AnalogVoltagePipeline.FromSessionMetadata(future.RootElement);
        Assert.True(pipeline.TryEstimateGpio5Millivolts(1500, out var millivolts));
        Assert.Equal(950, millivolts);
        Assert.True(pipeline.TryEstimateKnxBusVolts(1500, out var volts));
        Assert.InRange(volts, 9.499999, 9.500001);

        using var historical = JsonDocument.Parse("""{"firmware":"historical"}""");
        var noCalibration = AnalogVoltagePipeline.FromSessionMetadata(historical.RootElement);
        Assert.False(noCalibration.TryEstimateGpio5Millivolts(1500, out _));
        Assert.False(noCalibration.TryEstimateKnxBusVolts(1500, out _));
    }
}
