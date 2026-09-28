using System.IO.Compression;
using KNXAnalyzer.Core;
using Xunit.Abstractions;

namespace KNXAnalyzer.Core.Tests;

/// <summary>Blind, offline phase study. Parity and STOP never participate in phase selection.</summary>
public sealed class PhysicalDecoderCharacterTimingV4Tests(ITestOutputHelper output)
{
    private enum EvidenceClass { NoPulse, Ambiguous, Pulse }
    private sealed record EventData(RawCapture Capture, Tp1DecodeResult Decode);
    private sealed record Evidence(double Score, EvidenceClass Class, int MinimumSample, double TimingError,
        double RelativeDepth, double TemplateScore);
    private sealed record Fit(EventData Data, Tp1DecodedRecord Record, Tp1Character Character,
        double Phase, double BaselineCost, double BestCost, Evidence[] Baseline, Evidence[] Best,
        bool BaselineBlindParity, bool BestBlindParity, bool BaselineBlindStop, bool BestBlindStop,
        int UnexplainedBefore, int UnexplainedAfter);

    private const double NegativeDepth90 = .196;
    private const double PositiveDepth10 = .823;
    private const double NegativeTemplate90 = .637;
    private const double PositiveTemplate10 = .742;

    [Fact]
    public void StudiesFractionalCharacterPhaseWithoutChangingProductionDecoder()
    {
        WithFixture(folder => {
            var source = Load(folder); GuardBaseline(source);
            var validRecords = source.SelectMany(x => x.Decode.Records).Where(IsValid).ToArray();
            var positivePulses = validRecords.SelectMany(x => x.Characters).SelectMany(x => x.Slots)
                .Where(x => x.SourcePulse is not null).Select(x => x.SourcePulse!).ToArray();
            var template = MedianTemplate(source, positivePulses);
            var references = PulseDepthReferences(source, positivePulses);
            var fits = source.SelectMany(data => data.Decode.Records.SelectMany(record => record.Characters
                .Select(character => FitCharacter(data, record, character, template, references)))).ToArray();
            var valid = fits.Where(x => IsValid(x.Record)).ToArray();
            var invalid = fits.Where(x => x.Record.Classification == Tp1RecordClassification.INVALID_PARITY && !x.Character.ParityValid).ToArray();

            DumpFit("VALID", valid); DumpFit("INVALID_PARITY", invalid);
            output.WriteLine($"PHYSICAL_COST invalid improved={invalid.Count(IsImproved)} unchanged={invalid.Count(IsUnchanged)} degraded={invalid.Count(x => x.BestCost > x.BaselineCost + 1e-9)}");
            BlindValidation("VALID", valid); BlindValidation("INVALID", invalid);
            output.WriteLine($"UNEXPLAINED_STRONG invalid before={invalid.Sum(x => x.UnexplainedBefore)} after={invalid.Sum(x => x.UnexplainedAfter)}");

            var missing = invalid.Where(x => x.Character.Slots[9].SourcePulse is null && !x.Character.ExpectedParity).ToArray();
            var unexpected = invalid.Where(x => x.Character.Slots[9].SourcePulse is not null && x.Character.ExpectedParity).ToArray();
            var missingPulse = missing.Where(x => x.Baseline[9].Class == EvidenceClass.Pulse).ToArray();
            var missingNone = missing.Where(x => x.Baseline[9].Class == EvidenceClass.NoPulse).ToArray();
            var unexpectedPulse = unexpected.Where(x => x.Baseline[9].Class == EvidenceClass.Pulse).ToArray();
            Outcome("MISSING_PULSE_61", missingPulse);
            Outcome("MISSING_NO_PULSE_126", missingNone);
            Outcome("UNEXPECTED_PULSE_47", unexpectedPulse);

            var stopPulse = invalid.Where(x => !x.Character.ReceivedStop && x.Baseline[10].Class == EvidenceClass.Pulse).ToArray();
            var stopNextStart = 0; var stopGridShift = 0; var stopCurrent = 0; var stopIndeterminate = 0;
            foreach (var fit in stopPulse) {
                var next = NextCharacter(fit, fits);
                var distanceNext = next is null ? double.PositiveInfinity : Math.Abs(fit.Baseline[10].MinimumSample - next.Character.StartSample);
                if (distanceNext <= 3) stopNextStart++;
                else if (Math.Abs(fit.Phase) >= .75 && fit.Best[10].Class != EvidenceClass.Pulse) stopGridShift++;
                else if (fit.Best[10].Class == EvidenceClass.Pulse) stopCurrent++;
                else stopIndeterminate++;
            }
            output.WriteLine($"STOP_PULSE_LIKE n={stopPulse.Length} next-start={stopNextStart} grid-shift={stopGridShift} remains-current-stop={stopCurrent} indeterminate={stopIndeterminate}");

            var startSuspect = invalid.Count(x => x.Baseline[0].Class != EvidenceClass.Pulse ||
                (Math.Abs(x.Phase) >= 2 && x.BaselineCost - x.BestCost >= .25));
            var boundarySuspect = invalid.Count(x => {
                var index = x.Character.Index;
                return (index == 0 || index == x.Record.Characters.Count - 1) &&
                    (Math.Abs(x.Phase) >= .75 || !x.Character.ReceivedStop);
            });
            output.WriteLine($"SUSPECT start={startSuspect}/{invalid.Length} boundary={boundarySuspect}/{invalid.Length}");

            var diagnostic = invalid.GroupBy(ClassifyDiagnostic).OrderBy(x => x.Key)
                .Select(x => $"{x.Key}={x.Count()}");
            output.WriteLine("DIAGNOSTIC " + string.Join(' ', diagnostic));

            CompareEvents("VALID", valid); CompareEvents("INVALID", invalid);
            RecordContinuity(fits);
            SampleRateCounterfactual(source, template, references, 83315);
            SampleRateCounterfactual(source, template, references, 83330);
            SampleRateCounterfactual(source, template, references, 83333);
        });
    }

    private Fit FitCharacter(EventData data, Tp1DecodedRecord record, Tp1Character character,
        double[] template, IReadOnlyDictionary<uint, double> references)
    {
        // The initial ±3-sample probe was rejected by the valid-character control (P95 |phase|=1.375,
        // parity degradation and many new unexplained pulses). Keep the accepted study inside ±1.5.
        var candidates = Enumerable.Range(-12, 25).Select(i => i / 8.0).Select(phase => {
            var evidence = character.Slots.Select(slot => Measure(data.Capture, slot.ExpectedSample + phase, template, references[data.Capture.EventId])).ToArray();
            // Blind objective: preserve the independently observed START+D0..D7 physical assignments.
            // PARITY and STOP are deliberately excluded.
            double cost = 0;
            for (var slot = 0; slot <= 8; slot++) {
                var expectedPulse = character.Slots[slot].SourcePulse is not null;
                var target = expectedPulse ? 1.0 : 0.0;
                cost += (evidence[slot].Score - target) * (evidence[slot].Score - target);
            }
            cost += phase * phase * .002; // deterministic tie-breaker, not a timing fit target
            return (phase, cost, evidence);
        }).ToArray();
        var baseline = candidates.Single(x => x.phase == 0);
        var best = candidates.OrderBy(x => x.cost).ThenBy(x => Math.Abs(x.phase)).First();
        var before = CountUnexplained(data, character, 0);
        var after = CountUnexplained(data, character, best.phase);
        return new(data, record, character, best.phase, baseline.cost, best.cost, baseline.evidence, best.evidence,
            Parity(character, baseline.evidence), Parity(character, best.evidence), Stop(baseline.evidence), Stop(best.evidence), before, after);
    }

    private static bool Parity(Tp1Character character, Evidence[] e)
    {
        byte value = 0;
        for (var bit = 0; bit < 8; bit++) if (!PulseDecision(e[bit + 1], character.Slots[bit + 1])) value |= (byte)(1 << bit);
        var expectedParity = (System.Numerics.BitOperations.PopCount(value) & 1) != 0;
        var receivedParity = !PulseDecision(e[9], character.Slots[9]);
        return receivedParity == expectedParity;
    }
    private static bool Stop(Evidence[] e) => e[10].Class != EvidenceClass.Pulse;
    private static bool PulseDecision(Evidence e, Tp1BitSlot baseline) => e.Class switch {
        EvidenceClass.Pulse => true, EvidenceClass.NoPulse => false, _ => baseline.SourcePulse is not null
    };

    private static int CountUnexplained(EventData data, Tp1Character character, double phase)
    {
        var period = data.Capture.SampleRateHz / 9600.0;
        return data.Decode.Pulses.Count(p => p.StartSample >= character.StartSample - 4 && p.StartSample <= character.EndSample + 4 &&
            Enumerable.Range(0, 11).Min(slot => Math.Abs(p.StartSample - (character.Slots[0].ExpectedSample + phase + slot * period))) > 3);
    }

    private static Evidence Measure(RawCapture capture, double expected, double[] template, double referenceDepth)
    {
        var samples = capture.Samples; var center = (int)Math.Round(expected);
        var localFrom = Math.Max(0, center - 20); var localTo = Math.Min(samples.Length, center + 21);
        var baseline = Q(samples.Skip(localFrom).Take(localTo - localFrom).Select(x => (double)x), .75);
        var from = Math.Max(0, center - 3); var to = Math.Min(samples.Length - 1, center + 3);
        var minimumSample = Enumerable.Range(from, to - from + 1).MinBy(i => samples[i]);
        var depth = Math.Max(0, baseline - samples[minimumSample]) / Math.Max(1, referenceDepth);
        var templateScore = Enumerable.Range(-3, 7).Max(shift => Cosine(Vector(capture, center + shift), template));
        var d = Math.Clamp((depth - NegativeDepth90) / (PositiveDepth10 - NegativeDepth90), 0, 1);
        var s = Math.Clamp((templateScore - NegativeTemplate90) / (PositiveTemplate10 - NegativeTemplate90), 0, 1);
        var score = .55 * d + .45 * s;
        var cls = score >= .75 ? EvidenceClass.Pulse : score <= .25 ? EvidenceClass.NoPulse : EvidenceClass.Ambiguous;
        return new(score, cls, minimumSample, minimumSample - expected, depth, templateScore);
    }

    private void DumpFit(string name, Fit[] fits) => output.WriteLine($"PHASE_{name} n={fits.Length} correction={QD(fits.Select(x => x.Phase))} " +
        $"abs={QD(fits.Select(x => Math.Abs(x.Phase)))} p95abs={Q(fits.Select(x => Math.Abs(x.Phase)), .95):F3} " +
        $"cost-before={QD(fits.Select(x => x.BaselineCost))} cost-after={QD(fits.Select(x => x.BestCost))}");
    private void BlindValidation(string name, Fit[] fits) => output.WriteLine($"BLIND_{name} parity-before={fits.Count(x => x.BaselineBlindParity)}/{fits.Length} " +
        $"after={fits.Count(x => x.BestBlindParity)}/{fits.Length} improved={fits.Count(x => !x.BaselineBlindParity && x.BestBlindParity)} " +
        $"degraded={fits.Count(x => x.BaselineBlindParity && !x.BestBlindParity)} stop-before={fits.Count(x => x.BaselineBlindStop)}/{fits.Length} " +
        $"after={fits.Count(x => x.BestBlindStop)}/{fits.Length} improved={fits.Count(x => !x.BaselineBlindStop && x.BestBlindStop)} " +
        $"degraded={fits.Count(x => x.BaselineBlindStop && !x.BestBlindStop)}");
    private void Outcome(string name, Fit[] fits) => output.WriteLine($"{name} n={fits.Length} parity-before/after={fits.Count(x => x.BaselineBlindParity)}/{fits.Count(x => x.BestBlindParity)} " +
        $"parity-improved/degraded={fits.Count(x => !x.BaselineBlindParity && x.BestBlindParity)}/{fits.Count(x => x.BaselineBlindParity && !x.BestBlindParity)} " +
        $"phase-corrected={fits.Count(x => Math.Abs(x.Phase) >= .75)} centered-before/after={fits.Count(x => Math.Abs(x.Baseline[9].TimingError) <= 1.5)}/{fits.Count(x => Math.Abs(x.Best[9].TimingError) <= 1.5)} " +
        $"neighbor-slot={fits.Count(x => NeighborBetter(x, 9))} data-mismatch={fits.Count(DataEvidenceMismatch)} cost-improved={fits.Count(IsImproved)}");
    private void CompareEvents(string name, Fit[] fits)
    {
        var e89 = fits.Where(x => x.Data.Capture.EventId is 8 or 9).ToArray(); var other = fits.Where(x => x.Data.Capture.EventId is not 8 and not 9).ToArray();
        output.WriteLine($"EVENTS_{name} E89 n={e89.Length} abs-phase={QD(e89.Select(x => Math.Abs(x.Phase)))} improved={e89.Count(IsImproved)}; " +
            $"other n={other.Length} abs-phase={QD(other.Select(x => Math.Abs(x.Phase)))} improved={other.Count(IsImproved)}");
    }
    private void RecordContinuity(Fit[] fits)
    {
        var pairs = fits.GroupBy(x => (x.Data.Capture.EventId, x.Record.RecordIndex)).SelectMany(group => group.OrderBy(x => x.Character.Index)
            .Zip(group.OrderBy(x => x.Character.Index).Skip(1))).ToArray();
        output.WriteLine($"CONTINUITY pairs={pairs.Length} phase-delta={QD(pairs.Select(x => x.Second.Phase - x.First.Phase))} " +
            $"abs-delta={QD(pairs.Select(x => Math.Abs(x.Second.Phase - x.First.Phase)))} " +
            $"baseline-start-gap={QD(pairs.Select(x => (double)(x.Second.Character.StartSample - x.First.Character.EndSample)))}");
    }
    private void SampleRateCounterfactual(EventData[] source, double[] template, IReadOnlyDictionary<uint, double> references, uint rate)
    {
        var phase = new List<double>(); var cost = new List<double>();
        foreach (var data in source) foreach (var record in data.Decode.Records) foreach (var character in record.Characters) {
            var slots = character.Slots.Select(slot => slot with {
                ExpectedSample = character.StartSample + slot.Index * (rate / 9600.0)
            }).ToArray();
            var synthetic = character with { Slots = slots };
            var fit = FitCharacter(data, record, synthetic, template, references); phase.Add(fit.Phase); cost.Add(fit.BestCost);
        }
        output.WriteLine($"RATE {rate} chars={phase.Count} abs-phase={QD(phase.Select(Math.Abs))} cost={QD(cost)}");
    }

    private static string ClassifyDiagnostic(Fit x)
    {
        if (x.Baseline[9].Class == EvidenceClass.Pulse && x.Character.Slots[9].SourcePulse is null && Math.Abs(x.Phase) < .75) return "ANALOG_PULSE_MISSED";
        if (Math.Abs(x.Phase) >= .75 && x.BestBlindParity && !x.BaselineBlindParity && IsImproved(x)) return "PHASE_MISALIGNED";
        if (x.Baseline[0].Class != EvidenceClass.Pulse || Math.Abs(x.Phase) >= 2) return "START_SUSPECT";
        var index = x.Character.Index;
        if ((index == 0 || index == x.Record.Characters.Count - 1) && !x.Character.ReceivedStop) return "CHARACTER_BOUNDARY_SUSPECT";
        if (x.Baseline[9].Class == EvidenceClass.NoPulse && x.Best[9].Class == EvidenceClass.NoPulse && !x.BestBlindParity) return "NO_PHYSICAL_EVIDENCE";
        if (!x.BestBlindParity && (x.Best[9].Class == EvidenceClass.Pulse || !x.BestBlindStop)) return "PHYSICALLY_CORRUPTED";
        return "INDETERMINATE";
    }
    private static Fit? NextCharacter(Fit fit, Fit[] all) => all.FirstOrDefault(x => ReferenceEquals(x.Data, fit.Data) && ReferenceEquals(x.Record, fit.Record) && x.Character.Index == fit.Character.Index + 1);
    private static bool NeighborBetter(Fit x, int slot) => (slot > 0 && x.Best[slot - 1].Score > x.Best[slot].Score + .2) ||
        (slot < 10 && x.Best[slot + 1].Score > x.Best[slot].Score + .2);
    private static bool DataEvidenceMismatch(Fit x) => Enumerable.Range(1, 8).Any(slot =>
        (x.Character.Slots[slot].SourcePulse is not null && x.Best[slot].Class == EvidenceClass.NoPulse) ||
        (x.Character.Slots[slot].SourcePulse is null && x.Best[slot].Class == EvidenceClass.Pulse));
    private static bool IsImproved(Fit x) => x.BaselineCost - x.BestCost >= .05;
    private static bool IsUnchanged(Fit x) => Math.Abs(x.BaselineCost - x.BestCost) < .05;
    private static bool IsValid(Tp1DecodedRecord x) => x.Classification is Tp1RecordClassification.VALID_KNOWN or Tp1RecordClassification.VALID_UNKNOWN;

    private static IReadOnlyDictionary<uint, double> PulseDepthReferences(EventData[] source, Tp1Pulse[] pulses)
    {
        var all = pulses.Select(p => RawDepth(source.Single(x => x.Capture.EventId == p.EventId).Capture, p.StartSample)).ToArray(); var global = Q(all, .5);
        return source.ToDictionary(data => data.Capture.EventId, data => {
            var values = pulses.Where(p => p.EventId == data.Capture.EventId).Select(p => RawDepth(data.Capture, p.StartSample)).ToArray();
            return values.Length == 0 ? global : Q(values, .5);
        });
    }
    private static double RawDepth(RawCapture capture, int anchor)
    {
        var from = Math.Max(0, anchor - 20); var to = Math.Min(capture.Samples.Length, anchor + 21);
        var baseline = Q(capture.Samples.Skip(from).Take(to - from).Select(x => (double)x), .75);
        var minimum = Enumerable.Range(Math.Max(0, anchor - 2), Math.Min(capture.Samples.Length, anchor + 10) - Math.Max(0, anchor - 2)).Min(i => (double)capture.Samples[i]);
        return Math.Max(1, baseline - minimum);
    }
    private static double[] MedianTemplate(EventData[] source, Tp1Pulse[] pulses)
    {
        var vectors = pulses.Select(p => Vector(source.Single(x => x.Capture.EventId == p.EventId).Capture, p.StartSample)).ToArray();
        return Enumerable.Range(0, 16).Select(i => Q(vectors.Select(x => x[i]), .5)).ToArray();
    }
    private static double[] Vector(RawCapture capture, int anchor)
    {
        var from = Math.Max(0, anchor - 20); var to = Math.Min(capture.Samples.Length, anchor + 21);
        var baseline = Q(capture.Samples.Skip(from).Take(to - from).Select(x => (double)x), .75); var depth = RawDepth(capture, anchor);
        return Enumerable.Range(-3, 16).Select(offset => (baseline - capture.Samples[Math.Clamp(anchor + offset, 0, capture.Samples.Length - 1)]) / depth).ToArray();
    }
    private static double Cosine(double[] a, double[] b)
    {
        double dot = 0, aa = 0, bb = 0; for (var i = 0; i < a.Length; i++) { dot += a[i] * b[i]; aa += a[i] * a[i]; bb += b[i] * b[i]; }
        return aa == 0 || bb == 0 ? 0 : dot / Math.Sqrt(aa * bb);
    }
    private static string QD(IEnumerable<double> source) { var x = source.ToArray(); return x.Length == 0 ? "—" : $"{Q(x, .10):F3}/{Q(x, .50):F3}/{Q(x, .90):F3}"; }
    private static double Q(IEnumerable<double> source, double p)
    {
        var x = source.Order().ToArray(); if (x.Length == 0) return 0; var at = p * (x.Length - 1); var lo = (int)Math.Floor(at); var hi = (int)Math.Ceiling(at);
        return x[lo] + (x[hi] - x[lo]) * (at - lo);
    }
    private static EventData[] Load(string folder)
    {
        var session = EventRawV2Reader.OpenSession(folder);
        return session.AnalogEvents.OrderBy(x => x.EventId).Select(x => { var capture = EventRawV2Reader.ReadCapture(folder, x.EventId);
            return new EventData(capture, Tp1DeterministicDecoder.Decode(capture, Tp1AnalogDecodeProfile.FieldCandidate)); }).ToArray();
    }
    private static void GuardBaseline(EventData[] source)
    {
        Assert.Equal(2577, source.Sum(x => x.Decode.Pulses.Count)); Assert.Equal(622, source.Sum(x => x.Decode.CharacterCount)); Assert.Equal(197, source.Sum(x => x.Decode.Records.Count));
        var e8 = source.Single(x => x.Capture.EventId == 8).Decode; Assert.Equal(990, e8.Pulses.Count); Assert.Equal(220, e8.CharacterCount);
        Assert.Equal(54, e8.Records.Count); Assert.Equal(19, e8.AckCount); Assert.Equal(7, e8.Records.Count(x => x.Telegram is not null));
        Assert.Contains(e8.Records, x => x.RawHex == "BCFF160001E10080CA"); Assert.Contains(e8.Records, x => x.RawHex == "9CFF160001E10080EA");
    }
    private static void WithFixture(Action<string> action)
    {
        var archive = Path.Combine(AppContext.BaseDirectory, "Fixtures", "KNX-F35F14A4.zip"); var folder = Path.Combine(Path.GetTempPath(), "knx-timing-v4-" + Guid.NewGuid().ToString("N"));
        try { ZipFile.ExtractToDirectory(archive, folder); action(folder); } finally { if (Directory.Exists(folder)) Directory.Delete(folder, true); }
    }
}
