using System;
using System.Globalization;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Media;
using KNXAnalyzer.Core;

namespace KNXAnalyzer.Desktop.Views;

public class WaveformView : Control
{
    public static readonly StyledProperty<RawCapture?> CaptureProperty =
        AvaloniaProperty.Register<WaveformView, RawCapture?>(nameof(Capture));
    public RawCapture? Capture { get => GetValue(CaptureProperty); set => SetValue(CaptureProperty, value); }

    public event EventHandler<string>? ViewportChanged;
    public double VisibleStartSample => _start;
    public double VisibleEndSample => _end;

    private double _start;
    private double _end;
    private double _verticalZoom = 1;
    private bool _dragging;
    private double _lastDragX;

    static WaveformView() => AffectsRender<WaveformView>(CaptureProperty);

    protected override void OnPropertyChanged(AvaloniaPropertyChangedEventArgs change)
    {
        base.OnPropertyChanged(change);
        if (change.Property == CaptureProperty) ResetFit();
    }

    public void ResetFit()
    {
        _start = 0;
        _end = Capture?.Samples.Length ?? 0;
        _verticalZoom = 1;
        UpdateVisibleRange();
        InvalidateVisual();
    }

    public void ZoomAt(double factor, double relativeAnchor)
    {
        var count = Capture?.Samples.Length ?? 0;
        if (count == 0 || !double.IsFinite(factor) || factor <= 0) return;
        var width = _end - _start;
        if (width <= 0) width = count;
        var newWidth = Math.Clamp(width * factor, Math.Min(32, count), count);
        var anchor = Math.Clamp(relativeAnchor, 0, 1);
        var fixedSample = _start + anchor * width;
        _start = Math.Clamp(fixedSample - anchor * newWidth, 0, count - newWidth);
        _end = _start + newWidth;
        UpdateVisibleRange();
        InvalidateVisual();
    }

    public void PanBySamples(double delta)
    {
        var count = Capture?.Samples.Length ?? 0;
        var width = _end - _start;
        if (count == 0 || width <= 0 || !double.IsFinite(delta)) return;
        _start = Math.Clamp(_start + delta, 0, count - width);
        _end = _start + width;
        UpdateVisibleRange();
        InvalidateVisual();
    }

    protected override void OnPointerWheelChanged(PointerWheelEventArgs e)
    {
        base.OnPointerWheelChanged(e);
        if (Capture is null) return;
        if (e.KeyModifiers.HasFlag(KeyModifiers.Control)) {
            _verticalZoom = Math.Clamp(_verticalZoom * Math.Pow(1.4, e.Delta.Y), 1, 50);
            InvalidateVisual();
        } else {
            ZoomAt(Math.Pow(1.5, -e.Delta.Y), Bounds.Width <= 0 ? 0.5 : e.GetPosition(this).X / Bounds.Width);
        }
        e.Handled = true;
    }

    protected override void OnPointerPressed(PointerPressedEventArgs e)
    {
        base.OnPointerPressed(e);
        if (e.ClickCount >= 2) { ResetFit(); e.Handled = true; return; }
        if (Capture is null || !e.GetCurrentPoint(this).Properties.IsLeftButtonPressed) return;
        _dragging = true;
        _lastDragX = e.GetPosition(this).X;
        e.Pointer.Capture(this);
        e.Handled = true;
    }

    protected override void OnPointerMoved(PointerEventArgs e)
    {
        base.OnPointerMoved(e);
        if (!_dragging || Bounds.Width <= 0) return;
        var x = e.GetPosition(this).X;
        PanBySamples((_lastDragX - x) * (_end - _start) / Bounds.Width);
        _lastDragX = x;
        e.Handled = true;
    }

    protected override void OnPointerReleased(PointerReleasedEventArgs e)
    {
        base.OnPointerReleased(e);
        _dragging = false;
        e.Pointer.Capture(null);
    }

    private void UpdateVisibleRange()
    {
        var capture = Capture;
        if (capture is null || capture.SampleRateHz == 0 || capture.Samples.Length == 0) {
            ViewportChanged?.Invoke(this, "Visible time unavailable | Y: ADC RAW at GPIO5");
            return;
        }
        var startMs = 1000.0 * (_start - capture.TriggerIndex) / capture.SampleRateHz;
        var endMs = 1000.0 * (_end - capture.TriggerIndex) / capture.SampleRateHz;
        string Format(double value) => value.ToString("+0.000;-0.000;0.000", CultureInfo.InvariantCulture);
        var trigger = _start <= capture.TriggerIndex && capture.TriggerIndex <= _end
            ? "trigger t=0 visible" : "trigger t=0 outside view";
        ViewportChanged?.Invoke(this, $"Visible: {Format(startMs)} to {Format(endMs)} ms | {trigger} | {_end - _start:F0} samples | Y: ADC RAW at GPIO5");
    }

    public override void Render(DrawingContext context)
    {
        base.Render(context);
        var capture = Capture;
        if (capture is null || capture.Samples.Length == 0 || Bounds.Width < 10 || Bounds.Height < 10) return;
        var width = Bounds.Width;
        var height = Bounds.Height;
        var count = capture.Samples.Length;
        var start = Math.Clamp(_start, 0, count - 1);
        var end = Math.Clamp(_end, start + 1, count);
        var span = end - start;
        var first = Math.Clamp((int)Math.Floor(start), 0, count - 1);
        var last = Math.Clamp((int)Math.Ceiling(end), first + 1, count);

        ushort visibleMin = ushort.MaxValue, visibleMax = 0;
        for (var i = first; i < last; i++) {
            visibleMin = Math.Min(visibleMin, capture.Samples[i]);
            visibleMax = Math.Max(visibleMax, capture.Samples[i]);
        }
        var margin = Math.Max(10.0, (visibleMax - visibleMin) * 0.08);
        var center = (visibleMin + visibleMax) / 2.0;
        var halfRange = (visibleMax - visibleMin + 2 * margin) / (2 * _verticalZoom);
        var yMin = center - halfRange;
        var yRange = 2 * halfRange;
        double Y(ushort sample) => height - 1 - (sample - yMin) * (height - 2) / yRange;

        var wave = new Pen(Brushes.DodgerBlue, 1);
        if (span <= width * 2) {
            Point? previous = null;
            for (var i = first; i < last; i++) {
                var point = new Point((i - start) * width / span, Y(capture.Samples[i]));
                if (previous is Point p) context.DrawLine(wave, p, point);
                previous = point;
            }
        } else {
            var columns = Math.Max(1, (int)width);
            for (var x = 0; x < columns; x++) {
                var a = Math.Clamp((int)Math.Floor(start + x * span / columns), first, last - 1);
                var b = Math.Clamp((int)Math.Ceiling(start + (x + 1) * span / columns), a + 1, last);
                ushort low = ushort.MaxValue, high = 0;
                for (var i = a; i < b; i++) {
                    low = Math.Min(low, capture.Samples[i]);
                    high = Math.Max(high, capture.Samples[i]);
                }
                context.DrawLine(wave, new Point(x, Y(high)), new Point(x, Y(low)));
            }
        }
        if (capture.TriggerIndex >= start && capture.TriggerIndex <= end) {
            var triggerX = (capture.TriggerIndex - start) * width / span;
            context.DrawLine(new Pen(Brushes.OrangeRed, 1), new Point(triggerX, 0), new Point(triggerX, height));
        }
    }
}
