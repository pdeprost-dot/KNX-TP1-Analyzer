using KNXAnalyzer.Core;
using KNXAnalyzer.Desktop.Views;

namespace KNXAnalyzer.Desktop.Tests;

public class WaveformNavigationTests
{
    [Fact]
    public void ZoomsAndPansAcrossSeparateSyntheticPerturbationsWithoutChangingSamples()
    {
        var samples = Enumerable.Repeat((ushort)1900, 50_000).ToArray();
        samples[5_500] = 1200;
        samples[5_501] = 2400;
        samples[45_500] = 1250;
        samples[45_501] = 2350;
        var original = samples.ToArray();
        var capture = new RawCapture {
            SampleRateHz = 83_333, TriggerIndex = 35_000,
            Minimum = 1200, Maximum = 2400, Samples = samples
        };
        var view = new WaveformView { Capture = capture };
        Assert.Equal(0, view.VisibleStartSample);
        Assert.Equal(50_000, view.VisibleEndSample);

        view.ZoomAt(0.02, 0.11);
        Assert.True(view.VisibleEndSample - view.VisibleStartSample <= 1_000);
        Assert.InRange(5_500.0, view.VisibleStartSample, view.VisibleEndSample);
        var durationMs = 1000 * (view.VisibleEndSample - view.VisibleStartSample) / capture.SampleRateHz;
        Assert.True(durationMs < 20);

        view.PanBySamples(40_000);
        Assert.InRange(45_500.0, view.VisibleStartSample, view.VisibleEndSample);
        view.ZoomAt(0.0001, 0.5);
        Assert.Equal(32, view.VisibleEndSample - view.VisibleStartSample);
        view.ResetFit();
        Assert.Equal(0, view.VisibleStartSample);
        Assert.Equal(50_000, view.VisibleEndSample);
        view.SetZoomSlider(0.75);
        Assert.True(view.VisibleEndSample - view.VisibleStartSample < 1_000);
        view.SetPositionSlider(0);
        Assert.InRange(view.VisibleStartSample, 0, 1);
        view.SetPositionSlider(1);
        Assert.InRange(view.VisibleEndSample, 49_999, 50_001);
        var startBeforeUnitChange = view.VisibleStartSample;
        var endBeforeUnitChange = view.VisibleEndSample;
        view.SetRawDisplay(true);
        view.SetRawDisplay(false);
        Assert.Equal(startBeforeUnitChange, view.VisibleStartSample);
        Assert.Equal(endBeforeUnitChange, view.VisibleEndSample);
        Assert.Equal(original, capture.Samples);
        view.ShowSampleRange(5_500, 5_700);
        Assert.InRange(5_500.0, view.VisibleStartSample, view.VisibleEndSample);
        Assert.InRange(5_700.0, view.VisibleStartSample, view.VisibleEndSample);
        Assert.Equal(original, capture.Samples);
        view.ResetFit();
        Assert.Equal(0, view.ZoomSliderValue);
        Assert.Equal(0, view.PositionSliderValue);
        Assert.Equal(original, capture.Samples);
    }
}
