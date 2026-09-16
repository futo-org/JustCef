using Xunit;

namespace JustCef.Tests.Unit;

[Trait("Category", "Unit")]
public class CodecTests
{
    [Fact]
    public void WriterRejectsBodiesOverLimit()
    {
        using var writer = new PacketWriter(maxSize: 16);
        writer.WriteBytes(new byte[16]);
        Assert.Throws<InvalidOperationException>(() => writer.Write((byte)1));
    }

    [Fact]
    public void WriterGrowsUpToTheLimit()
    {
        const int limit = 3 * 1024 * 1024;
        using var writer = new PacketWriter(maxSize: limit);
        byte[] data = new byte[limit - 8];
        Random.Shared.NextBytes(data);
        writer.Write(7).Write((uint)data.Length).WriteBytes(data);
        Assert.Equal(limit, writer.Size);

        var reader = new PacketReader(writer.Data, writer.Size);
        Assert.Equal(7, reader.Read<int>());
        Assert.Equal(data, reader.ReadBytes((int)reader.Read<uint>()));
    }

    [Fact]
    public void WriterDefaultLimitIsTheProtocolLimit()
    {
        using var writer = new PacketWriter();
        var chunk = new byte[16 * 1024 * 1024];
        for (int written = 0; written < 256 * 1024 * 1024; written += chunk.Length)
            writer.WriteBytes(chunk);

        Assert.Equal(256 * 1024 * 1024, writer.Size);
        Assert.Throws<InvalidOperationException>(() => writer.WriteBytes(new byte[1]));
    }
}
