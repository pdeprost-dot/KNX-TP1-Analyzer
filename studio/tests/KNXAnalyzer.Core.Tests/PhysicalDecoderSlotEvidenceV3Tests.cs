using System.IO.Compression;
using KNXAnalyzer.Core;
using Xunit.Abstractions;

namespace KNXAnalyzer.Core.Tests;

/// <summary>Offline-only V3 laboratory. None of its evidence is fed back to the production decoder.</summary>
public sealed class PhysicalDecoderSlotEvidenceV3Tests(ITestOutputHelper output)
{
    private enum EvidenceClass { NO_PULSE, AMBIGUOUS, PULSE }
    private sealed record EventData(RawCapture Capture, Tp1DecodeResult Decode);
    private sealed record Provenance(uint Event, int Record, int Character, int Slot, string SlotName);
    private sealed record Evidence(Provenance At, double Expected, int MinimumSample, double Baseline,
        double Minimum, double Depth, double RelativeDepth, double Width, double Fall, double Rise,
        double Area, double Noise, double TimingError, double TemplateScore, bool FieldPulse,
        bool BaselineValue, EvidenceClass Class, double Combined);
    private sealed record Thresholds(double NegativeDepth90, double PositiveDepth10,
        double NegativeTemplate90, double PositiveTemplate10);

    [Fact]
    public void CharacterizesSlotEvidenceWithoutChangingFieldCandidate()
    {
        WithFixture(folder => {
            var source = Load(folder);
            GuardBaseline(source);

            var positivePulses = ValidCharacters(source).SelectMany(x => x.Character.Slots)
                .Where(x => x.SourcePulse is not null).Select(x => x.SourcePulse!).ToArray();
            var globalTemplate = MedianTemplate(source, positivePulses);
            var references = PulseDepthReferences(source, positivePulses);
            var provisional = new Thresholds(0, 0, 0, 0);

            var positives = ValidCharacters(source).SelectMany(x => x.Character.Slots.Select(slot =>
                    (x.Data, x.Record, x.Character, Slot: slot)))
                .Where(x => x.Slot.SourcePulse is not null)
                .Select(x => Measure(x.Data, x.Record, x.Character, x.Slot, globalTemplate, references, provisional)).ToArray();
            var negatives = ValidCharacters(source).SelectMany(x => x.Character.Slots.Skip(1).Take(9).Select(slot =>
                    (x.Data, x.Record, x.Character, Slot: slot)))
                .Where(x => x.Slot.SourcePulse is null)
                .Select(x => Measure(x.Data, x.Record, x.Character, x.Slot, globalTemplate, references, provisional)).ToArray();

            var thresholds = new Thresholds(
                Q(negatives.Select(x => x.RelativeDepth), .90), Q(positives.Select(x => x.RelativeDepth), .10),
                Q(negatives.Select(x => x.TemplateScore), .90), Q(positives.Select(x => x.TemplateScore), .10));
            positives = Reclassify(positives, thresholds);
            negatives = Reclassify(negatives, thresholds);

            var parityInvalid = InvalidParityCharacters(source).ToArray();
            var missingParity = parityInvalid
                .Where(x => x.Character.Slots[9].SourcePulse is null && !x.Character.ExpectedParity)
                .Select(x => Measure(x.Data, x.Record, x.Character, x.Character.Slots[9], globalTemplate, references, thresholds)).ToArray();
            var unexpectedParity = parityInvalid
                .Where(x => x.Character.Slots[9].SourcePulse is not null && x.Character.ExpectedParity)
                .Select(x => Measure(x.Data, x.Record, x.Character, x.Character.Slots[9], globalTemplate, references, thresholds)).ToArray();
            var invalidStops = parityInvalid.Where(x => !x.Character.ReceivedStop)
                .Select(x => Measure(x.Data, x.Record, x.Character, x.Character.Slots[10], globalTemplate, references, thresholds)).ToArray();
            var validStops = ValidCharacters(source).Where(x => x.Character.ReceivedStop)
                .Select(x => Measure(x.Data, x.Record, x.Character, x.Character.Slots[10], globalTemplate, references, thresholds)).ToArray();

            Assert.Equal(191, missingParity.Length);
            Assert.Equal(50, unexpectedParity.Length);
            Assert.Equal(24, invalidStops.Length);

            output.WriteLine($"REFERENCE positive={positives.Length} negative={negatives.Length}");
            Dump("POSITIVE", positives); Dump("NEGATIVE", negatives);
            output.WriteLine($"THRESHOLDS depth negQ90={thresholds.NegativeDepth90:F3} posQ10={thresholds.PositiveDepth10:F3} " +
                $"template negQ90={thresholds.NegativeTemplate90:F3} posQ10={thresholds.PositiveTemplate10:F3}");
            output.WriteLine($"AUC depth={Auc(positives.Select(x => x.RelativeDepth), negatives.Select(x => x.RelativeDepth)):F4} " +
                $"template={Auc(positives.Select(x => x.TemplateScore), negatives.Select(x => x.TemplateScore)):F4} " +
                $"combined={Auc(positives.Select(x => x.Combined), negatives.Select(x => x.Combined)):F4}");
            Confusion("REFERENCE", positives, negatives);
            DumpClasses("MISSING_PARITY", missingParity);
            DumpClasses("UNEXPECTED_PARITY", unexpectedParity);
            DumpClasses("INVALID_STOP", invalidStops);
            Dump("VALID_STOP", validStops);

            var eventTemplates = source.Select(data => {
                var pulses = ValidCharacters([data]).SelectMany(x => x.Character.Slots)
                    .Where(x => x.SourcePulse is not null).Select(x => x.SourcePulse!).ToArray();
                return (data.Capture.EventId, Count: pulses.Length,
                    Template: pulses.Length < 5 ? null : MedianTemplate([data], pulses));
            }).Where(x => x.Template is not null).ToArray();
            output.WriteLine("EVENT_TEMPLATE " + string.Join(' ', eventTemplates.Select(x =>
                $"E{x.EventId}:n{x.Count}/cos{Cosine(x.Template!, globalTemplate):F3}")));
            var e89 = positives.Where(x => x.At.Event is 8 or 9).ToArray();
            var others = positives.Where(x => x.At.Event is not 8 and not 9).ToArray();
            output.WriteLine($"EVENT_89 positive={e89.Length} depth={QD(e89.Select(x => x.RelativeDepth))} template={QD(e89.Select(x => x.TemplateScore))}");
            output.WriteLine($"EVENT_OTHER positive={others.Length} depth={QD(others.Select(x => x.RelativeDepth))} template={QD(others.Select(x => x.TemplateScore))}");

            var allSlots = positives.Concat(negatives).Concat(missingParity).Concat(unexpectedParity).Concat(invalidStops).ToArray();
            Robustness("OFFSET-100", source, allSlots, globalTemplate, thresholds, -100, 1, 0);
            Robustness("OFFSET+100", source, allSlots, globalTemplate, thresholds, 100, 1, 0);
            Robustness("GAIN0.9", source, allSlots, globalTemplate, thresholds, 0, .9, 0);
            Robustness("GAIN1.1", source, allSlots, globalTemplate, thresholds, 0, 1.1, 0);
            Robustness("NOISE5", source, allSlots, globalTemplate, thresholds, 0, 1, 5);

            Example("A_VALID_DEEP", positives.MaxBy(x => x.RelativeDepth)!);
            Example("B_VALID_RECESSIVE", negatives.MinBy(x => x.Combined)!);
            Example("C_MISSING_STRONG", missingParity.Where(x => x.Class == EvidenceClass.PULSE).MaxBy(x => x.Combined));
            Example("D_MISSING_AMBIGUOUS", missingParity.Where(x => x.Class == EvidenceClass.AMBIGUOUS).OrderBy(x => Math.Abs(x.Combined - .5)).FirstOrDefault());
            Example("E_MISSING_NONE", missingParity.Where(x => x.Class == EvidenceClass.NO_PULSE).MinBy(x => x.Combined));
            Example("F_UNEXPECTED_PARITY", unexpectedParity.MaxBy(x => x.Combined));
            Example("G_INVALID_STOP", invalidStops.MaxBy(x => x.Combined));
        });
    }

    private void Robustness(string name, EventData[] original, Evidence[] slots, double[] template,
        Thresholds thresholds, double offset, double gain, double sigma)
    {
        var random = new Random(0x51A7);
        var transformed = original.Select(data => {
            var baseline = Q(data.Capture.Samples.Select(x => (double)x), .75);
            var samples = data.Capture.Samples.Select(x => {
                var noise = sigma == 0 ? 0 : sigma * Math.Sqrt(-2 * Math.Log(Math.Max(1e-12, random.NextDouble()))) * Math.Cos(2 * Math.PI * random.NextDouble());
                return (ushort)Math.Clamp(Math.Round(baseline + gain * (x - baseline) + offset + noise), 0, ushort.MaxValue);
            }).ToArray();
            var capture = new RawCapture {
                EventId = data.Capture.EventId, SampleRateHz = data.Capture.SampleRateHz,
                TriggerIndex = data.Capture.TriggerIndex, SampleStart = data.Capture.SampleStart,
                SampleEnd = data.Capture.SampleEnd, TriggerSample = data.Capture.TriggerSample,
                Threshold = data.Capture.Threshold, ConfiguredPreSamples = data.Capture.ConfiguredPreSamples,
                ConfiguredPostSamples = data.Capture.ConfiguredPostSamples, ChunkCount = data.Capture.ChunkCount,
                Minimum = samples.Min(), Maximum = samples.Max(), Samples = samples,
                CrcValid = data.Capture.CrcValid, Integrity = data.Capture.Integrity,
                Mean = samples.Average(x => (double)x)
            };
            return new EventData(capture, data.Decode);
        }).ToArray();
        var pulseRefs = PulseDepthReferences(transformed, ValidCharacters(transformed).SelectMany(x => x.Character.Slots)
            .Where(x => x.SourcePulse is not null).Select(x => x.SourcePulse!).ToArray());
        var changed = 0; var absoluteChanged = 0; var rankPairs = new List<(double Before, double After)>();
        foreach (var old in slots) {
            var data = transformed.Single(x => x.Capture.EventId == old.At.Event);
            var record = data.Decode.Records.Single(x => x.RecordIndex == old.At.Record);
            var character = record.Characters.Single(x => x.Index == old.At.Character);
            var slot = character.Slots[old.At.Slot];
            var now = Measure(data, record, character, slot, template, pulseRefs, thresholds);
            if (now.Class != old.Class) changed++;
            if ((old.Minimum < Tp1AnalogDecodeProfile.FieldCandidate.LowThreshold) !=
                (now.Minimum < Tp1AnalogDecodeProfile.FieldCandidate.LowThreshold)) absoluteChanged++;
            rankPairs.Add((old.Combined, now.Combined));
        }
        output.WriteLine($"ROBUST {name} class-agreement={slots.Length - changed}/{slots.Length} ({(slots.Length - changed) * 100.0 / slots.Length:F2}%) " +
            $"rank-correlation={Spearman(rankPairs):F4} absolute-LOW-agreement={slots.Length - absoluteChanged}/{slots.Length} " +
            $"({(slots.Length - absoluteChanged) * 100.0 / slots.Length:F2}%)");
    }

    private static Evidence Measure(EventData data, Tp1DecodedRecord record, Tp1Character character,
        Tp1BitSlot slot, double[] template, IReadOnlyDictionary<uint, double> pulseRefs, Thresholds thresholds)
    {
        var samples = data.Capture.Samples; var expected = slot.ExpectedSample; var center = (int)Math.Round(expected);
        var localFrom = Math.Max(0, center - 20); var localTo = Math.Min(samples.Length, center + 21);
        var local = samples.Skip(localFrom).Take(localTo - localFrom).Select(x => (double)x).ToArray();
        var baseline = Q(local, .75);
        var searchFrom = Math.Max(0, center - 3); var searchTo = Math.Min(samples.Length - 1, center + 3);
        var minimumSample = Enumerable.Range(searchFrom, searchTo - searchFrom + 1).MinBy(i => samples[i]);
        var minimum = (double)samples[minimumSample]; var depth = Math.Max(0, baseline - minimum);
        var relative = depth / Math.Max(1, pulseRefs[data.Capture.EventId]);
        var noise = Mad(local.Where(x => x >= Q(local, .50)));
        double fall = 0, rise = 0, area = 0;
        for (var i = Math.Max(1, minimumSample - 6); i <= Math.Min(samples.Length - 2, minimumSample + 8); i++) {
            fall = Math.Max(fall, samples[i - 1] - (double)samples[i]);
            rise = Math.Max(rise, samples[i + 1] - (double)samples[i]);
            area += Math.Max(0, baseline - samples[i]);
        }
        var width = Enumerable.Range(Math.Max(0, minimumSample - 5), Math.Min(samples.Length, minimumSample + 7) - Math.Max(0, minimumSample - 5))
            .Count(i => samples[i] <= baseline - depth * .5);
        var score = Enumerable.Range(-3, 7).Max(shift => Cosine(Vector(data.Capture, center + shift), template));
        var combined = Combined(relative, score, thresholds);
        var cls = Classify(combined);
        return new(new(data.Capture.EventId, record.RecordIndex, character.Index, slot.Index, slot.Name), expected,
            minimumSample, baseline, minimum, depth, relative, width, fall, rise, area, noise,
            minimumSample - expected, score, slot.SourcePulse is not null, slot.Value, cls, combined);
    }

    private static double Combined(double depth, double template, Thresholds t)
    {
        if (t.PositiveDepth10 <= t.NegativeDepth90 || t.PositiveTemplate10 <= t.NegativeTemplate90) return 0;
        var d = Math.Clamp((depth - t.NegativeDepth90) / (t.PositiveDepth10 - t.NegativeDepth90), 0, 1);
        var s = Math.Clamp((template - t.NegativeTemplate90) / (t.PositiveTemplate10 - t.NegativeTemplate90), 0, 1);
        return .55 * d + .45 * s;
    }
    private static EvidenceClass Classify(double score) => score >= .75 ? EvidenceClass.PULSE : score <= .25 ? EvidenceClass.NO_PULSE : EvidenceClass.AMBIGUOUS;
    private static Evidence[] Reclassify(Evidence[] source, Thresholds t) => source.Select(x => {
        var score = Combined(x.RelativeDepth, x.TemplateScore, t); return x with { Combined = score, Class = Classify(score) };
    }).ToArray();

    private void Dump(string name, Evidence[] x) => output.WriteLine($"{name} n={x.Length} depth={QD(x.Select(v => v.RelativeDepth))} " +
        $"template={QD(x.Select(v => v.TemplateScore))} combined={QD(x.Select(v => v.Combined))} width={QD(x.Select(v => v.Width))} " +
        $"timing={QD(x.Select(v => Math.Abs(v.TimingError)))} noise={QD(x.Select(v => v.Noise))}");
    private void DumpClasses(string name, Evidence[] x) => output.WriteLine($"{name} n={x.Length} pulse={x.Count(v => v.Class == EvidenceClass.PULSE)} " +
        $"ambiguous={x.Count(v => v.Class == EvidenceClass.AMBIGUOUS)} no-pulse={x.Count(v => v.Class == EvidenceClass.NO_PULSE)} " +
        $"depth={QD(x.Select(v => v.RelativeDepth))} template={QD(x.Select(v => v.TemplateScore))} timing={QD(x.Select(v => Math.Abs(v.TimingError)))}");
    private void Confusion(string name, Evidence[] p, Evidence[] n) => output.WriteLine($"{name}_CLASS positive P/A/N={p.Count(x => x.Class == EvidenceClass.PULSE)}/" +
        $"{p.Count(x => x.Class == EvidenceClass.AMBIGUOUS)}/{p.Count(x => x.Class == EvidenceClass.NO_PULSE)} negative P/A/N=" +
        $"{n.Count(x => x.Class == EvidenceClass.PULSE)}/{n.Count(x => x.Class == EvidenceClass.AMBIGUOUS)}/{n.Count(x => x.Class == EvidenceClass.NO_PULSE)}");
    private void Example(string name, Evidence? x)
    {
        if (x is null) { output.WriteLine($"EXAMPLE {name} none"); return; }
        output.WriteLine($"EXAMPLE {name} E{x.At.Event}/R{x.At.Record}/C{x.At.Character}/{x.At.SlotName} expected={x.Expected:F3} min@{x.MinimumSample} " +
            $"depth={x.Depth:F1} rel={x.RelativeDepth:F3} template={x.TemplateScore:F3} combined={x.Combined:F3} class={x.Class} " +
            $"width={x.Width:F0} fall={x.Fall:F1} rise={x.Rise:F1} area={x.Area:F1} noise={x.Noise:F1} timing={x.TimingError:F3} field={x.FieldPulse}");
    }

    private static IEnumerable<(EventData Data, Tp1DecodedRecord Record, Tp1Character Character)> ValidCharacters(IEnumerable<EventData> source) =>
        source.SelectMany(data => data.Decode.Records.Where(r => r.Classification is Tp1RecordClassification.VALID_KNOWN or Tp1RecordClassification.VALID_UNKNOWN)
            .SelectMany(r => r.Characters.Select(c => (data, r, c))));
    private static IEnumerable<(EventData Data, Tp1DecodedRecord Record, Tp1Character Character)> InvalidParityCharacters(IEnumerable<EventData> source) =>
        source.SelectMany(data => data.Decode.Records.Where(r => r.Classification == Tp1RecordClassification.INVALID_PARITY)
            .SelectMany(r => r.Characters.Where(c => !c.ParityValid).Select(c => (data, r, c))));

    private static IReadOnlyDictionary<uint, double> PulseDepthReferences(EventData[] source, Tp1Pulse[] pulses)
    {
        var allDepths = pulses.Select(p => RawDepth(source.Single(x => x.Capture.EventId == p.EventId).Capture, p.StartSample)).ToArray();
        var global = allDepths.Length == 0 ? 1 : Q(allDepths, .50);
        return source.ToDictionary(data => data.Capture.EventId, data => {
            var depths = pulses.Where(p => p.EventId == data.Capture.EventId).Select(p => RawDepth(data.Capture, p.StartSample)).ToArray();
            return depths.Length == 0 ? global : Q(depths, .50);
        });
    }
    private static double RawDepth(RawCapture capture, int anchor)
    {
        var from = Math.Max(0, anchor - 20); var to = Math.Min(capture.Samples.Length, anchor + 21);
        var baseline = Q(capture.Samples.Skip(from).Take(to - from).Select(x => (double)x), .75);
        var min = Enumerable.Range(Math.Max(0, anchor - 2), Math.Min(capture.Samples.Length, anchor + 10) - Math.Max(0, anchor - 2))
            .Min(i => (double)capture.Samples[i]);
        return Math.Max(1, baseline - min);
    }
    private static double[] MedianTemplate(EventData[] source, Tp1Pulse[] pulses)
    {
        var vectors = pulses.Select(p => Vector(source.Single(x => x.Capture.EventId == p.EventId).Capture, p.StartSample)).ToArray();
        return Enumerable.Range(0, 16).Select(i => Q(vectors.Select(v => v[i]), .50)).ToArray();
    }
    private static double[] Vector(RawCapture capture, int anchor)
    {
        var samples = capture.Samples; var from = Math.Max(0, anchor - 20); var to = Math.Min(samples.Length, anchor + 21);
        var baseline = Q(samples.Skip(from).Take(to - from).Select(x => (double)x), .75);
        var depth = RawDepth(capture, anchor);
        return Enumerable.Range(-3, 16).Select(o => (baseline - samples[Math.Clamp(anchor + o, 0, samples.Length - 1)]) / depth).ToArray();
    }
    private static double Cosine(double[] a, double[] b)
    {
        double dot = 0, aa = 0, bb = 0;
        for (var i = 0; i < a.Length; i++) { dot += a[i] * b[i]; aa += a[i] * a[i]; bb += b[i] * b[i]; }
        return aa == 0 || bb == 0 ? 0 : dot / Math.Sqrt(aa * bb);
    }
    private static double Mad(IEnumerable<double> source)
    {
        var x = source.ToArray(); if (x.Length == 0) return 0; var median = Q(x, .5); return Q(x.Select(v => Math.Abs(v - median)), .5);
    }
    private static double Auc(IEnumerable<double> positive, IEnumerable<double> negative)
    {
        var p = positive.ToArray(); var n = negative.ToArray(); double wins = 0;
        foreach (var a in p) foreach (var b in n) wins += a > b ? 1 : a == b ? .5 : 0;
        return wins / (p.Length * (double)n.Length);
    }
    private static double Spearman(List<(double Before, double After)> x)
    {
        var rb = Ranks(x.Select(v => v.Before).ToArray()); var ra = Ranks(x.Select(v => v.After).ToArray());
        var mb = rb.Average(); var ma = ra.Average();
        var numerator = rb.Zip(ra).Sum(v => (v.First - mb) * (v.Second - ma));
        var denominator = Math.Sqrt(rb.Sum(v => (v - mb) * (v - mb)) * ra.Sum(v => (v - ma) * (v - ma)));
        return denominator == 0 ? 1 : numerator / denominator;
    }
    private static double[] Ranks(double[] x)
    {
        var result = new double[x.Length];
        var ordered = x.Select((value, index) => (value, index)).OrderBy(v => v.value).ToArray();
        for (var first = 0; first < ordered.Length;) {
            var last = first;
            while (last + 1 < ordered.Length && ordered[last + 1].value == ordered[first].value) last++;
            var rank = (first + last) / 2.0 + 1;
            for (var i = first; i <= last; i++) result[ordered[i].index] = rank;
            first = last + 1;
        }
        return result;
    }
    private static string QD(IEnumerable<double> source)
    {
        var x = source.Order().ToArray(); return x.Length == 0 ? "—" : $"{Q(x, .10):F3}/{Q(x, .50):F3}/{Q(x, .90):F3}";
    }
    private static double Q(IEnumerable<double> source, double p)
    {
        var x = source.Order().ToArray(); if (x.Length == 0) return 0; var at = p * (x.Length - 1); var lo = (int)Math.Floor(at); var hi = (int)Math.Ceiling(at);
        return x[lo] + (x[hi] - x[lo]) * (at - lo);
    }

    private static EventData[] Load(string folder)
    {
        var session = EventRawV2Reader.OpenSession(folder);
        return session.AnalogEvents.OrderBy(x => x.EventId).Select(x => {
            var capture = EventRawV2Reader.ReadCapture(folder, x.EventId);
            return new EventData(capture, Tp1DeterministicDecoder.Decode(capture, Tp1AnalogDecodeProfile.FieldCandidate));
        }).ToArray();
    }
    private static void GuardBaseline(EventData[] source)
    {
        Assert.Equal(2577, source.Sum(x => x.Decode.Pulses.Count)); Assert.Equal(622, source.Sum(x => x.Decode.CharacterCount));
        Assert.Equal(197, source.Sum(x => x.Decode.Records.Count));
        var event8 = source.Single(x => x.Capture.EventId == 8).Decode;
        Assert.Equal(990, event8.Pulses.Count); Assert.Equal(220, event8.CharacterCount); Assert.Equal(54, event8.Records.Count);
        Assert.Equal(19, event8.AckCount); Assert.Equal(7, event8.Records.Count(x => x.Telegram is not null));
        Assert.Contains(event8.Records, x => x.RawHex == "BCFF160001E10080CA");
        Assert.Contains(event8.Records, x => x.RawHex == "9CFF160001E10080EA");
    }
    private static void WithFixture(Action<string> action)
    {
        var archive = Path.Combine(AppContext.BaseDirectory, "Fixtures", "KNX-F35F14A4.zip");
        var folder = Path.Combine(Path.GetTempPath(), "knx-slot-v3-" + Guid.NewGuid().ToString("N"));
        try { ZipFile.ExtractToDirectory(archive, folder); action(folder); }
        finally { if (Directory.Exists(folder)) Directory.Delete(folder, true); }
    }
}
