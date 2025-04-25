namespace DotCef;

public class IPCResponse
{
    public required int StatusCode { get; init; }
    public required string StatusText { get; init;}
    public required Dictionary<string, List<string>> Headers { get; init; }
    public required Stream? BodyStream { get; init; }
}