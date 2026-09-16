using System.Collections.Concurrent;
using System.IO.Pipes;
using static JustCef.JustCefProcess;

namespace JustCef.Tests.Infrastructure;

internal sealed record RawPacket(uint RequestId, PacketType Type, byte Opcode, byte[] Body);

internal sealed class PipePeer : IDisposable
{
    private readonly AnonymousPipeClientStream _peerIn;
    private readonly AnonymousPipeClientStream _peerOut;
    private readonly object _writeLock = new();

    public JustCefProcess Process { get; }
    public BlockingCollection<RawPacket> Received { get; } = new();
    public TaskCompletionSource PeerEof { get; } = new(TaskCreationOptions.RunContinuationsAsynchronously);

    public PipePeer()
    {
        Process = new JustCefProcess();
        _peerIn = new AnonymousPipeClientStream(PipeDirection.In, Process.WriterClientHandle);
        _peerOut = new AnonymousPipeClientStream(PipeDirection.Out, Process.ReaderClientHandle);
        Process.StartConnection();

        new Thread(ReadLoop) { IsBackground = true, Name = "pipe peer reader" }.Start();
    }

    public void Send(uint requestId, PacketType type, byte opcode, byte[]? body = null)
        => SendRaw(Wire.Packet(requestId, type, opcode, body ?? Array.Empty<byte>()));

    public void SendRaw(byte[] bytes)
    {
        lock (_writeLock)
        {
            _peerOut.Write(bytes);
            _peerOut.Flush();
        }
    }

    public void SendReady() => Send(0, PacketType.Notification, (byte)OpcodeClientNotification.Ready, BitConverter.GetBytes(2u));

    public void CloseOutput() => _peerOut.Dispose();

    public RawPacket Take(TimeSpan timeout)
    {
        if (!Received.TryTake(out var packet, timeout))
            throw new TimeoutException("The peer did not receive a packet in time.");
        return packet;
    }

    public RawPacket Take(PacketType type, TimeSpan timeout)
    {
        var deadline = DateTime.UtcNow + timeout;
        while (true)
        {
            var left = deadline - DateTime.UtcNow;
            var packet = Take(left > TimeSpan.Zero ? left : TimeSpan.Zero);
            if (packet.Type == type)
                return packet;
        }
    }

    private void ReadLoop()
    {
        byte[] header = new byte[Wire.HeaderSize];
        try
        {
            while (true)
            {
                if (!ReadExactly(header))
                    break;

                var parsed = Wire.ReadHeader(header);
                byte[] body = new byte[parsed.BodyLength];
                if (body.Length > 0 && !ReadExactly(body))
                    break;

                Received.Add(new RawPacket(parsed.RequestId, parsed.Type, parsed.Opcode, body));
            }
        }
        catch
        {
        }

        PeerEof.TrySetResult();
    }

    private bool ReadExactly(byte[] buffer)
    {
        int total = 0;
        while (total < buffer.Length)
        {
            int read = _peerIn.Read(buffer, total, buffer.Length - total);
            if (read <= 0)
                return false;
            total += read;
        }
        return true;
    }

    public void Dispose()
    {
        Process.Dispose();
        foreach (var stream in new Stream[] { _peerOut, _peerIn })
        {
            try
            {
                stream.Dispose();
            }
            catch
            {
            }
        }
    }
}
