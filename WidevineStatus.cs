namespace JustCef;

public enum WidevineComponentState
{
    New = 0,
    Checking = 1,
    CanUpdate = 2,
    Downloading = 3,
    Decompressing = 4,
    Patching = 5,
    Updating = 6,
    Updated = 7,
    UpToDate = 8,
    UpdateError = 9,
    Run = 10
}

public class WidevineStatus
{
    public required bool Registered { get; init; }
    public required bool Installed { get; init; }
    public required bool RequiresRestart { get; init; }
    public required WidevineComponentState State { get; init; }
    public required string? Version { get; init; }
}
