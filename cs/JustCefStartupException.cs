namespace JustCef;

public enum JustCefStartupFailure
{
    Unknown = 0,
    ProfileInUse = 1,
    ProcessNotified = 2,
    ProtocolMismatch = 3
}

public class JustCefStartupException : Exception
{
    public int ExitCode { get; }
    public JustCefStartupFailure Failure { get; }

    public JustCefStartupException(int exitCode, JustCefStartupFailure failure, string message) : base(message)
    {
        ExitCode = exitCode;
        Failure = failure;
    }
}
