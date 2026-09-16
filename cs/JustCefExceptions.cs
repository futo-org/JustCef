namespace JustCef;

public enum JustCefStatus : byte
{
    Ok = 0,
    Error = 1,
    Canceled = 2,
    NotFound = 3,
    NotHandled = 4,
    Unsupported = 5,
    ShuttingDown = 6,
    TooLarge = 7,
    InvalidRequest = 8
}

public class JustCefRemoteException : InvalidOperationException
{
    public JustCefStatus Status { get; }

    public JustCefRemoteException(JustCefStatus status, string? message)
        : base(message ?? $"The JustCef call failed with status {status}.")
    {
        Status = status;
    }
}
