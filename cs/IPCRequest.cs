namespace JustCef;

internal enum IPCProxyBodyElementType
{
    Bytes = 1,
    File = 2
}

public abstract class IPCProxyBodyElement
{
}

public sealed class IPCProxyBodyElementBytes : IPCProxyBodyElement
{
    public IPCProxyBodyElementBytes(byte[] data)
    {
        ArgumentNullException.ThrowIfNull(data);
        Data = data;
    }

    public byte[] Data { get; }
}

public sealed class IPCProxyBodyElementFile : IPCProxyBodyElement
{
    public IPCProxyBodyElementFile(string fileName)
    {
        ArgumentNullException.ThrowIfNull(fileName);
        FileName = fileName;
    }

    public string FileName { get; }
}

public class IPCRequest
{
    public required string Method { get; set; }
    public required string Url { get; set; }
    public required Dictionary<string, List<string>> Headers { get; set; }
    public required List<IPCProxyBodyElement> Elements { get; set; }
}
