namespace KNXAnalyzer.Core;

public sealed record TrafficEndpoint(string Source, string Destination, string DestinationType, string Service, string ApduHex, string PayloadHex);

public sealed record ParticipantTraffic(
    string Address, int TelegramCount, int SessionCount,
    IReadOnlyList<TrafficCount> GroupDestinations, IReadOnlyList<TrafficCount> Services,
    IReadOnlyList<TrafficCount> Apdus, IReadOnlyList<TrafficCount> Payloads,
    IReadOnlyList<TrafficCount> Priorities, int Repeated, int ChecksumValid);

public sealed record GroupTraffic(
    string Address, int TelegramCount, int SessionCount,
    IReadOnlyList<TrafficCount> Sources, IReadOnlyList<TrafficCount> Services,
    IReadOnlyList<TrafficCount> Apdus, IReadOnlyList<TrafficCount> Payloads);

public sealed record TrafficCount(string Value, int Count);

public sealed record TemporalInteraction(
    TrafficEndpoint A, TrafficEndpoint B, int Occurrences, int SessionCount,
    double MinimumDelayMs, double MedianDelayMs, double MeanDelayMs, double MaximumDelayMs);

public sealed class TrafficAnalysis
{
    public int ObservedTelegramCount { get; init; }
    public int SyntheticTelegramCount { get; init; }
    public IReadOnlyList<ParticipantTraffic> Participants { get; init; } = [];
    public IReadOnlyList<GroupTraffic> Groups { get; init; } = [];
    public IReadOnlyList<TemporalInteraction> Interactions { get; init; } = [];
}

public static class TrafficAnalyzer
{
    private sealed record Item(string SessionId, Tp1Candidate Candidate, KnxTelegram Telegram)
    {
        public TrafficEndpoint Endpoint => new(Telegram.Source, Telegram.Destination, Telegram.DestinationType,
            Telegram.Service, Telegram.ApduHex, Telegram.PayloadHex);
    }

    public static TrafficAnalysis Analyze(IEnumerable<Session> source, bool includeSynthetic = false,
        TimeSpan? interactionWindow = null, int minimumOccurrences = 2)
    {
        var sessions = source.ToArray();
        var all = sessions.SelectMany(s => s.Candidates.Select(c => new { Session = s, Candidate = c, Telegram = KnxTelegramDecoder.Decode(c) }))
            .Where(x => x.Telegram is not null).Select(x => new Item(x.Session.Id, x.Candidate, x.Telegram!)).ToArray();
        var selected = all.Where(x => includeSynthetic || !x.Candidate.SyntheticTest).ToArray();
        IReadOnlyList<TrafficCount> Counts(IEnumerable<string> values) => values.GroupBy(x => x)
            .Select(g => new TrafficCount(g.Key, g.Count())).OrderByDescending(x => x.Count).ThenBy(x => x.Value).ToArray();

        var participants = selected.GroupBy(x => x.Telegram.Source).Select(g => new ParticipantTraffic(
            g.Key, g.Count(), g.Select(x => x.SessionId).Distinct().Count(),
            Counts(g.Where(x => x.Telegram.DestinationType == "group").Select(x => x.Telegram.Destination)),
            Counts(g.Select(x => x.Telegram.Service)), Counts(g.Select(x => x.Telegram.ApduHex)),
            Counts(g.Select(x => string.IsNullOrEmpty(x.Telegram.PayloadHex) ? "(inline/no payload bytes)" : x.Telegram.PayloadHex)),
            Counts(g.Select(x => x.Telegram.Priority)), g.Count(x => x.Telegram.Repeat),
            g.Count(x => x.Telegram.ChecksumValid))).OrderByDescending(x => x.TelegramCount).ThenBy(x => x.Address).ToArray();

        var groups = selected.Where(x => x.Telegram.DestinationType == "group").GroupBy(x => x.Telegram.Destination)
            .Select(g => new GroupTraffic(g.Key, g.Count(), g.Select(x => x.SessionId).Distinct().Count(),
                Counts(g.Select(x => x.Telegram.Source)), Counts(g.Select(x => x.Telegram.Service)),
                Counts(g.Select(x => x.Telegram.ApduHex)),
                Counts(g.Select(x => string.IsNullOrEmpty(x.Telegram.PayloadHex) ? "(inline/no payload bytes)" : x.Telegram.PayloadHex))))
            .OrderByDescending(x => x.TelegramCount).ThenBy(x => x.Address).ToArray();

        var windowUs = (long)(interactionWindow ?? TimeSpan.FromMilliseconds(100)).TotalMilliseconds * 1000;
        var pairs = new List<(TrafficEndpoint A, TrafficEndpoint B, string Session, double DelayMs)>();
        foreach (var session in selected.GroupBy(x => x.SessionId)) {
            var ordered = session.Where(x => x.Candidate.MonotonicUs is not null).OrderBy(x => x.Candidate.MonotonicUs).ToArray();
            for (var i = 0; i + 1 < ordered.Length; i++) {
                var delay = ordered[i + 1].Candidate.MonotonicUs!.Value - ordered[i].Candidate.MonotonicUs!.Value;
                if (delay >= 0 && delay <= windowUs)
                    pairs.Add((ordered[i].Endpoint, ordered[i + 1].Endpoint, session.Key, delay / 1000.0));
            }
        }
        var interactions = pairs.GroupBy(x => (x.A, x.B)).Where(g => g.Count() >= minimumOccurrences)
            .Select(g => {
                var delays = g.Select(x => x.DelayMs).Order().ToArray();
                var middle = delays.Length / 2;
                var median = delays.Length % 2 == 0 ? (delays[middle - 1] + delays[middle]) / 2 : delays[middle];
                return new TemporalInteraction(g.Key.A, g.Key.B, delays.Length, g.Select(x => x.Session).Distinct().Count(),
                    delays[0], median, delays.Average(), delays[^1]);
            }).OrderByDescending(x => x.Occurrences).ThenByDescending(x => x.SessionCount).ThenBy(x => x.MedianDelayMs).ToArray();

        return new TrafficAnalysis {
            ObservedTelegramCount = all.Count(x => !x.Candidate.SyntheticTest),
            SyntheticTelegramCount = all.Count(x => x.Candidate.SyntheticTest),
            Participants = participants, Groups = groups, Interactions = interactions
        };
    }
}
