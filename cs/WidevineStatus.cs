namespace JustCef;

public enum WidevineState
{
    Ready = 0,
    RestartRequired = 1,
    Unavailable = 2
}

public enum WidevineUnavailableReason
{
    None = 0,
    NotSupported = 1,
    NoCachePath = 2,
    UpdaterUnavailable = 3,
    UpdateFailed = 4,
    NoUsableCdm = 5
}

public sealed record WidevineStatus(WidevineState State, WidevineUnavailableReason Reason, string? Detail, string? Version);
