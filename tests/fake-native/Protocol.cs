using System.Buffers.Binary;
using System.Text;

namespace FakeNative;

public enum PacketType : byte
{
    Request = 0,
    Response = 1,
    Notification = 2,
    Cancel = 3
}

public enum Status : byte
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

public enum ControllerOp : byte
{
    Ping = 0,
    Print = 1,
    Echo = 2,
    WindowCreate = 3,
    WindowSetDevelopmentToolsEnabled = 5,
    WindowLoadUrl = 6,
    WindowSetZoom = 9,
    WindowGetPosition = 14,
    WindowSetPosition = 15,
    WindowMaximize = 17,
    WindowMinimize = 18,
    WindowRestore = 19,
    WindowShow = 20,
    WindowHide = 21,
    WindowClose = 22,
    WindowRequestFocus = 25,
    WindowActivate = 28,
    WindowBringToTop = 29,
    WindowSetAlwaysOnTop = 30,
    WindowSetFullscreen = 31,
    WindowCenterSelf = 32,
    WindowSetProxyRequests = 33,
    WindowSetModifyRequests = 34,
    PickFile = 39,
    PickDirectory = 40,
    SaveFile = 41,
    WindowExecuteDevToolsMethod = 42,
    WindowSetDevelopmentToolsVisible = 43,
    WindowSetTitle = 44,
    WindowSetIcon = 45,
    WindowAddUrlToProxy = 46,
    WindowRemoveUrlToProxy = 47,
    WindowAddUrlToModify = 48,
    WindowRemoveUrlToModify = 49,
    WindowGetSize = 50,
    WindowSetSize = 51,
    WindowAddDevToolsEventMethod = 52,
    WindowRemoveDevToolsEventMethod = 53,
    WindowAddDomainToProxy = 54,
    WindowRemoveDomainToProxy = 55,
    WindowGetZoom = 56,
    WindowBridgeRpc = 57,
    GetWidevineStatus = 59,
    Debug = 250
}

public enum ControllerNotification : byte
{
    Exit = 0,
    StreamData = 1,
    StreamEnd = 2,
    StreamError = 3
}

public enum ClientOp : byte
{
    Ping = 0,
    Print = 1,
    Echo = 2,
    WindowProxyRequest = 3,
    WindowModifyRequest = 4,
    WindowBridgeRpc = 9,
    WindowViewCreated = 11
}

public enum ClientNotification : byte
{
    Ready = 0,
    Exit = 1,
    WindowOpened = 2,
    WindowClosed = 3,
    WindowFocused = 5,
    WindowUnfocused = 6,
    WindowFullscreenChanged = 12,
    WindowFrameLoadStart = 13,
    WindowFrameLoadEnd = 14,
    WindowFrameLoadError = 15,
    WindowDevToolsEvent = 16,
    WindowLoadingStateChanged = 17,
    StreamCredit = 18,
    StreamCancel = 19,
    Debug = 250
}

public static class Protocol
{
    public const int HeaderSize = 10;
    public const int MaxBody = 268435456;
    public const uint Version = 2;
    public const int MaxStreamChunk = 262144;
    public const long InitialCredit = 1048576;
    public const int CreditBatch = 65536;
}

public sealed class ProtocolException : Exception
{
    public ProtocolException(string message) : base(message)
    {
    }
}

public sealed record Packet(uint RequestId, byte Type, byte Opcode, byte[] Body);

public sealed class BodyReader
{
    private static readonly UTF8Encoding StrictUtf8 = new(false, true);

    private readonly byte[] _data;
    private readonly int _end;
    private int _pos;

    public BodyReader(byte[] data) : this(data, 0, data.Length)
    {
    }

    public BodyReader(byte[] data, int offset, int count)
    {
        _data = data;
        _pos = offset;
        _end = offset + count;
    }

    public int Remaining => _end - _pos;

    private ReadOnlySpan<byte> Take(int count, string what)
    {
        if (count < 0 || count > Remaining)
            throw new ProtocolException($"truncated {what}: need {count} bytes, have {Remaining}");
        var span = new ReadOnlySpan<byte>(_data, _pos, count);
        _pos += count;
        return span;
    }

    public byte U8() => Take(1, "u8")[0];

    public bool Bool()
    {
        var v = Take(1, "bool")[0];
        if (v > 1)
            throw new ProtocolException($"bool value {v} is not 0 or 1");
        return v == 1;
    }

    public int I32() => BinaryPrimitives.ReadInt32LittleEndian(Take(4, "i32"));
    public uint U32() => BinaryPrimitives.ReadUInt32LittleEndian(Take(4, "u32"));
    public long I64() => BinaryPrimitives.ReadInt64LittleEndian(Take(8, "i64"));
    public ulong U64() => BinaryPrimitives.ReadUInt64LittleEndian(Take(8, "u64"));
    public double F64() => BinaryPrimitives.ReadDoubleLittleEndian(Take(8, "f64"));

    public string? Str()
    {
        var length = I32();
        if (length == -1)
            return null;
        if (length < -1)
            throw new ProtocolException($"invalid str length {length}");
        var span = Take(length, "str");
        try
        {
            return StrictUtf8.GetString(span);
        }
        catch (DecoderFallbackException)
        {
            throw new ProtocolException("str is not valid UTF-8");
        }
    }

    public ReadOnlyMemory<byte> Bytes()
    {
        var length = U32();
        if (length > (uint)Remaining)
            throw new ProtocolException($"truncated bytes: need {length} bytes, have {Remaining}");
        var memory = new ReadOnlyMemory<byte>(_data, _pos, (int)length);
        _pos += (int)length;
        return memory;
    }

    public ReadOnlyMemory<byte> Rest()
    {
        var memory = new ReadOnlyMemory<byte>(_data, _pos, Remaining);
        _pos = _end;
        return memory;
    }

    public uint Count(int minElementSize, string what)
    {
        var count = U32();
        if (count > (uint)(Remaining / minElementSize))
            throw new ProtocolException($"{what} count {count} exceeds remaining body");
        return count;
    }

    public int SignedCount(int minElementSize, string what)
    {
        var count = I32();
        if (count < 0)
            throw new ProtocolException($"negative {what} count {count}");
        if (count > Remaining / minElementSize)
            throw new ProtocolException($"{what} count {count} exceeds remaining body");
        return count;
    }

    public void End()
    {
        if (Remaining != 0)
            throw new ProtocolException($"{Remaining} trailing bytes");
    }
}

public sealed class PacketBuilder
{
    private byte[] _buffer;
    private int _length;

    public PacketBuilder(PacketType type, byte opcode, uint requestId, int capacity = 64)
    {
        _buffer = new byte[Protocol.HeaderSize + Math.Max(capacity, 0)];
        BinaryPrimitives.WriteUInt32LittleEndian(_buffer.AsSpan(4), requestId);
        _buffer[8] = (byte)type;
        _buffer[9] = opcode;
        _length = Protocol.HeaderSize;
    }

    public int BodyLength => _length - Protocol.HeaderSize;

    private Span<byte> Grow(int count)
    {
        if (_length + count > _buffer.Length)
        {
            var size = Math.Max(_buffer.Length * 2L, (long)_length + count);
            if (size > Array.MaxLength)
                size = Array.MaxLength;
            Array.Resize(ref _buffer, (int)size);
        }
        var span = _buffer.AsSpan(_length, count);
        _length += count;
        return span;
    }

    public PacketBuilder U8(byte v)
    {
        Grow(1)[0] = v;
        return this;
    }

    public PacketBuilder Bool(bool v) => U8(v ? (byte)1 : (byte)0);

    public PacketBuilder I32(int v)
    {
        BinaryPrimitives.WriteInt32LittleEndian(Grow(4), v);
        return this;
    }

    public PacketBuilder U32(uint v)
    {
        BinaryPrimitives.WriteUInt32LittleEndian(Grow(4), v);
        return this;
    }

    public PacketBuilder I64(long v)
    {
        BinaryPrimitives.WriteInt64LittleEndian(Grow(8), v);
        return this;
    }

    public PacketBuilder U64(ulong v)
    {
        BinaryPrimitives.WriteUInt64LittleEndian(Grow(8), v);
        return this;
    }

    public PacketBuilder F64(double v)
    {
        BinaryPrimitives.WriteDoubleLittleEndian(Grow(8), v);
        return this;
    }

    public PacketBuilder Str(string? v)
    {
        if (v == null)
            return I32(-1);
        var count = Encoding.UTF8.GetByteCount(v);
        I32(count);
        Encoding.UTF8.GetBytes(v, Grow(count));
        return this;
    }

    public PacketBuilder Bytes(ReadOnlySpan<byte> v)
    {
        U32((uint)v.Length);
        return Raw(v);
    }

    public PacketBuilder Raw(ReadOnlySpan<byte> v)
    {
        v.CopyTo(Grow(v.Length));
        return this;
    }

    public ReadOnlyMemory<byte> Finish()
    {
        BinaryPrimitives.WriteUInt32LittleEndian(_buffer, (uint)(_length - 4));
        return new ReadOnlyMemory<byte>(_buffer, 0, _length);
    }
}
