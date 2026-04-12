namespace JustCef;

internal enum IPCProxyBodyElementType
{
    Bytes = 1,
    File = 2,
    Stream = 3
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

public sealed class IPCProxyBodyElementStreamedBytes : IPCProxyBodyElement
{
    public IPCProxyBodyElementStreamedBytes(DataStream bodyStream, long? length = null)
    {
        ArgumentNullException.ThrowIfNull(bodyStream);
        BodyStream = bodyStream;
        Length = length;
    }

    public DataStream BodyStream { get; }
    public long? Length { get; }

    public async Task<byte[]> ReadAllBytesAsync(CancellationToken cancellationToken = default)
    {
        using MemoryStream memoryStream = new MemoryStream();
        byte[] buffer = new byte[64 * 1024];
        while (true)
        {
            int bytesRead = await BodyStream.ReadAsync(buffer, 0, buffer.Length, cancellationToken);
            if (bytesRead <= 0)
                break;

            memoryStream.Write(buffer, 0, bytesRead);
        }

        return memoryStream.ToArray();
    }
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
