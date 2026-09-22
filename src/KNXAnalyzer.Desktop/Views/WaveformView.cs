using System;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Media;
using KNXAnalyzer.Core;

namespace KNXAnalyzer.Desktop.Views;

public class WaveformView : Control
{
    public static readonly StyledProperty<RawCapture?> CaptureProperty = AvaloniaProperty.Register<WaveformView, RawCapture?>(nameof(Capture));
    public RawCapture? Capture { get => GetValue(CaptureProperty); set => SetValue(CaptureProperty, value); }
    static WaveformView() => AffectsRender<WaveformView>(CaptureProperty);

    public override void Render(DrawingContext context)
    {
        base.Render(context);
        var capture = Capture;
        if (capture is null || capture.Samples.Length == 0 || Bounds.Width < 10 || Bounds.Height < 10) return;
        var width = Bounds.Width; var height = Bounds.Height;
        var range = Math.Max(1, capture.Maximum - capture.Minimum);
        var columns = Math.Min((int)width, capture.Samples.Length);
        var wave = new Pen(Brushes.DodgerBlue, 1);
        for (var x = 0; x < columns; x++) {
            var start = (int)((long)x * capture.Samples.Length / columns);
            var end = Math.Max(start + 1, (int)((long)(x + 1) * capture.Samples.Length / columns));
            ushort min = ushort.MaxValue, max = 0;
            for (var i = start; i < end; i++) { min = Math.Min(min, capture.Samples[i]); max = Math.Max(max, capture.Samples[i]); }
            var top = height - 1 - (max - capture.Minimum) * (height - 2) / range;
            var bottom = height - 1 - (min - capture.Minimum) * (height - 2) / range;
            context.DrawLine(wave, new Point(x, top), new Point(x, bottom));
        }
        var triggerX = width * capture.TriggerIndex / capture.Samples.Length;
        context.DrawLine(new Pen(Brushes.OrangeRed, 1), new Point(triggerX, 0), new Point(triggerX, height));
    }
}
