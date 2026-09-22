using KNXAnalyzer.Core;

namespace KNXAnalyzer.Desktop.ViewModels;

public sealed record ParticipantDisplay(ParticipantTraffic Model, double TrafficPercent)
{
    public string Address => Model.Address;
    public int TelegramCount => Model.TelegramCount;
    public int SessionCount => Model.SessionCount;
    public int GroupCount => Model.GroupDestinations.Count;
}

public sealed record ParticipantDestinationDisplay(
    string Destination, int TelegramCount, string Services, string ValuesAndApdu);

public sealed record GroupDisplay(GroupTraffic Model)
{
    public string Address => Model.Address;
    public int TelegramCount => Model.TelegramCount;
    public int EmitterCount => Model.Sources.Count;
    public int SessionCount => Model.SessionCount;
}

public sealed record InteractionDisplay(TemporalInteraction Model)
{
    public string Sequence => $"{Model.A.Source} → {Model.A.Destination}  →  {Model.B.Source} → {Model.B.Destination}";
    public int Occurrences => Model.Occurrences;
    public int SessionCount => Model.SessionCount;
    public double MedianDelayMs => Model.MedianDelayMs;
    public string ASource => Model.A.Source;
    public string ADestination => Model.A.Destination;
    public string AService => Model.A.Service;
    public string AValue => FormatValue(Model.A);
    public string BSource => Model.B.Source;
    public string BDestination => Model.B.Destination;
    public string BService => Model.B.Service;
    public string BValue => FormatValue(Model.B);
    public double MinimumDelayMs => Model.MinimumDelayMs;
    public double MeanDelayMs => Model.MeanDelayMs;
    public double MaximumDelayMs => Model.MaximumDelayMs;

    private static string FormatValue(TrafficEndpoint endpoint) =>
        string.IsNullOrEmpty(endpoint.PayloadHex) ? $"APDU {endpoint.ApduHex}" : $"APDU {endpoint.ApduHex} · payload {endpoint.PayloadHex}";
}
