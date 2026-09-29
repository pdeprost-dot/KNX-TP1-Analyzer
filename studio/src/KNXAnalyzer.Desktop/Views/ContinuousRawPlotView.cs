using System;
using System.Globalization;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Media;
using KNXAnalyzer.Core;

namespace KNXAnalyzer.Desktop.Views;

public class ContinuousRawPlotView : Control
{
    public static readonly StyledProperty<ContinuousRawView?> ViewDataProperty =
        AvaloniaProperty.Register<ContinuousRawPlotView, ContinuousRawView?>(nameof(ViewData));
    public ContinuousRawView? ViewData { get => GetValue(ViewDataProperty); set => SetValue(ViewDataProperty, value); }

    public Action<ulong, ulong>? ViewRangeRequested { get; set; }
    public Action<ulong>? CursorRequested { get; set; }
    public Action<ulong, ulong>? SelectionRequested { get; set; }
    public ulong TotalSamples { get; private set; }
    public double SampleRateHz { get; private set; }
    public ulong VisibleStart => (ulong)Math.Floor(start);
    public ulong VisibleEnd => (ulong)Math.Ceiling(end);

    private double start, end;
    private bool panning, selecting;
    private double lastX;
    private ulong selectionAnchor, selectionEnd;
    private ulong? cursor;

    static ContinuousRawPlotView() => AffectsRender<ContinuousRawPlotView>(ViewDataProperty);

    public void Configure(ulong totalSamples, double sampleRateHz)
    { TotalSamples = totalSamples; SampleRateHz = sampleRateHz; FitAll(); }

    public void FitAll()
    { start = 0; end = TotalSamples; selectionAnchor = selectionEnd = 0; RequestView(); }

    protected override void OnPointerWheelChanged(PointerWheelEventArgs e)
    {
        if (TotalSamples == 0) return;
        var width = Math.Max(1, end - start); var anchor = Math.Clamp(XFraction(e.GetPosition(this).X), 0, 1);
        var next = Math.Clamp(width * Math.Pow(1.5, -e.Delta.Y), 16, (double)TotalSamples);
        var fixedSample = start + anchor * width;
        start = Math.Clamp(fixedSample - anchor * next, 0, Math.Max(0, TotalSamples - next)); end = start + next;
        RequestView(); e.Handled = true;
    }

    protected override void OnPointerPressed(PointerPressedEventArgs e)
    {
        if (e.ClickCount >= 2) { FitAll(); e.Handled = true; return; }
        if (!e.GetCurrentPoint(this).Properties.IsLeftButtonPressed || TotalSamples == 0) return;
        lastX = e.GetPosition(this).X;
        if (e.KeyModifiers.HasFlag(KeyModifiers.Shift)) { selecting = true; selectionAnchor = selectionEnd = SampleAt(lastX); }
        else panning = true;
        e.Pointer.Capture(this); InvalidateVisual(); e.Handled = true;
    }

    protected override void OnPointerMoved(PointerEventArgs e)
    {
        var x = e.GetPosition(this).X;
        if (panning && PlotWidth > 0) { var width = end - start; start = Math.Clamp(start + (lastX - x) * width / PlotWidth, 0, Math.Max(0, TotalSamples - width)); end = start + width; lastX = x; RequestView(); }
        else if (selecting) { selectionEnd = SampleAt(x); InvalidateVisual(); }
        else { cursor = SampleAt(x); CursorRequested?.Invoke(cursor.Value); InvalidateVisual(); }
        e.Handled = panning || selecting;
    }

    protected override void OnPointerReleased(PointerReleasedEventArgs e)
    {
        if (selecting) { selectionEnd = SampleAt(e.GetPosition(this).X); var a = Math.Min(selectionAnchor, selectionEnd); var b = Math.Max(selectionAnchor, selectionEnd) + 1; if (b > a) SelectionRequested?.Invoke(a, Math.Min(b, TotalSamples)); }
        panning = selecting = false; e.Pointer.Capture(null); InvalidateVisual();
    }

    private const double PlotLeft = 70;
    private double PlotWidth => Math.Max(1, Bounds.Width - PlotLeft);
    private double XFraction(double x) => (x - PlotLeft) / PlotWidth;
    private ulong SampleAt(double x) => TotalSamples == 0 ? 0 : (ulong)Math.Clamp(Math.Floor(start + Math.Clamp(XFraction(x), 0, 1) * Math.Max(1, end - start)), 0, TotalSamples - 1);
    private double X(ulong sample) => PlotLeft + (sample - start) * PlotWidth / Math.Max(1, end - start);
    private void RequestView() { InvalidateVisual(); if (end > start) ViewRangeRequested?.Invoke(VisibleStart, Math.Min(TotalSamples, Math.Max(VisibleStart + 1, VisibleEnd))); }

    public override void Render(DrawingContext context)
    {
        base.Render(context); var view = ViewData;
        if (view is null || Bounds.Width < 20 || Bounds.Height < 20) return;
        ushort min = ushort.MaxValue, max = 0;
        if (view.ExactSamples is { Length: > 0 } exact) foreach (var value in exact) { min = Math.Min(min, value); max = Math.Max(max, value); }
        else foreach (var bin in view.Bins) { min = Math.Min(min, bin.Minimum); max = Math.Max(max, bin.Maximum); }
        if (min == ushort.MaxValue) return;
        var margin = Math.Max(8, (max - min) * .08); var yMin = min - margin; var yRange = Math.Max(1, max - min + 2 * margin);
        double Y(double value) => Bounds.Height - 1 - (value - yMin) * (Bounds.Height - 2) / yRange;
        var grid = new Pen(Brushes.LightGray, 1); context.DrawLine(new Pen(Brushes.Gray, 1), new Point(PlotLeft, 0), new Point(PlotLeft, Bounds.Height));
        for (var tick = 0; tick <= 4; tick++) { var y = tick * (Bounds.Height - 1) / 4; var value = yMin + (1 - tick / 4.0) * yRange; context.DrawLine(grid, new Point(PlotLeft, y), new Point(Bounds.Width, y)); var label = new FormattedText($"{value:F0}", CultureInfo.InvariantCulture, FlowDirection.LeftToRight, new Typeface("Inter"), 11, Brushes.Gray); context.DrawText(label, new Point(Math.Max(0, PlotLeft - label.Width - 5), Math.Max(0, y - label.Height / 2))); }
        context.DrawText(new FormattedText("ADC RAW", CultureInfo.InvariantCulture, FlowDirection.LeftToRight, new Typeface("Inter"), 11, Brushes.Gray), new Point(3, 2));
        var wave = new Pen(Brushes.DodgerBlue, 1);
        if (view.ExactSamples is { Length: > 0 } samples) { Point? previous = null; for (var i = 0; i < samples.Length; i++) { var p = new Point(X(view.StartSample + (ulong)i), Y(samples[i])); if (previous is Point old) context.DrawLine(wave, old, p); previous = p; } }
        else foreach (var bin in view.Bins) { var x = X(bin.FirstSample + bin.SampleCount / 2); context.DrawLine(wave, new Point(x, Y(bin.Maximum)), new Point(x, Y(bin.Minimum))); }
        if (selectionAnchor != selectionEnd) { var left = X(Math.Min(selectionAnchor, selectionEnd)); var right = X(Math.Max(selectionAnchor, selectionEnd)); context.FillRectangle(new SolidColorBrush(Color.FromArgb(45, 255, 165, 0)), new Rect(Math.Min(left, right), 0, Math.Abs(right - left), Bounds.Height)); }
        if (cursor is ulong cursorSample && cursorSample >= start && cursorSample <= end) context.DrawLine(new Pen(Brushes.DarkOrange, 1), new Point(X(cursorSample), 0), new Point(X(cursorSample), Bounds.Height));
    }
}
