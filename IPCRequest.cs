namespace DotCef;

public enum IPCProxyBodyElementType
{
    Empty = 0,
    Bytes = 1,
    File = 2
}

public class IPCProxyBodyElement
{
    public required IPCProxyBodyElementType Type { get; init; }
}

public class IPCProxyBodyElementBytes : IPCProxyBodyElement
{
    public required byte[] Data { get; init; }
}

public class IPCProxyBodyElementFile : IPCProxyBodyElement
{
    public required string FileName { get; init; }
}

public class IPCRequest
{
    public required string Method { get; set; }
    public required string Url { get; set; }
    public required Dictionary<string, List<string>> Headers { get; set; }
    public required List<IPCProxyBodyElement> Elements { get; set; }
}