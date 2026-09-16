using System.Buffers.Binary;
using static JustCef.JustCefProcess;

namespace JustCef.Tests.Infrastructure;

internal readonly record struct WireHeader(uint Size, uint RequestId, PacketType Type, byte Opcode)
{
    public int BodyLength => (int)Size + 4 - Wire.HeaderSize;
}

internal static class Wire
{
    public const int HeaderSize = 10;
    public const int MaxBodySize = 256 * 1024 * 1024;

    public static void WriteHeader(Span<byte> destination, int bodyLength, uint requestId, PacketType type, byte opcode)
    {
        BinaryPrimitives.WriteUInt32LittleEndian(destination, (uint)(bodyLength + HeaderSize - 4));
        BinaryPrimitives.WriteUInt32LittleEndian(destination.Slice(4), requestId);
        destination[8] = (byte)type;
        destination[9] = opcode;
    }

    public static WireHeader ReadHeader(ReadOnlySpan<byte> source)
        => new(BinaryPrimitives.ReadUInt32LittleEndian(source), BinaryPrimitives.ReadUInt32LittleEndian(source.Slice(4)), (PacketType)source[8], source[9]);

    public static byte[] Packet(uint requestId, PacketType type, byte opcode, ReadOnlySpan<byte> body)
    {
        byte[] packet = new byte[HeaderSize + body.Length];
        WriteHeader(packet, body.Length, requestId, type, opcode);
        body.CopyTo(packet.AsSpan(HeaderSize));
        return packet;
    }

    public static byte[] Body(Action<PacketWriter> write)
    {
        using var writer = new PacketWriter();
        write(writer);
        return writer.Data.AsSpan(0, writer.Size).ToArray();
    }

    public static byte[] Ok(Action<PacketWriter>? write = null)
        => Body(w =>
        {
            w.Write((byte)JustCefStatus.Ok);
            write?.Invoke(w);
        });

    public static byte[] Failure(JustCefStatus status, string? message)
        => Body(w => w.Write((byte)status).WriteSizePrefixedString(message));

    public static PacketWriter WriteBytesField(this PacketWriter writer, byte[] data)
        => writer.Write((uint)data.Length).WriteBytes(data);
}
