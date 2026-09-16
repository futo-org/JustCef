using System.Text;
using JustCef.Tests.Infrastructure;
using Xunit;
using static JustCef.JustCefProcess;

namespace JustCef.Tests.Unit;

[Trait("Category", "Unit")]
public class ConnectionTests : IDisposable
{
    private static readonly TimeSpan Timeout = TimeSpan.FromSeconds(10);

    private readonly PipePeer _peer = new();

    public ConnectionTests()
    {
        TestUtil.QuietLogs();
    }

    public void Dispose() => _peer.Dispose();

    private JustCefProcess Process => _peer.Process;

    private void Reply(RawPacket request, byte[] body) => _peer.Send(request.RequestId, PacketType.Response, request.Opcode, body);

    private async Task<JustCefWindow> CreateWindowAsync(int identifier)
    {
        var create = Process.CreateWindowAsync("", 0, 0);
        var request = _peer.Take(PacketType.Request, Timeout);
        Assert.Equal((byte)OpcodeController.WindowCreate, request.Opcode);
        Reply(request, Wire.Ok(w => w.Write(identifier)));
        return await create.WithTimeout(Timeout);
    }

    [Fact]
    public async Task DuplicateInboundRequestIdIsRejected()
    {
        var window = await CreateWindowAsync(1);
        var held = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var entered = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        window.SetBridgeRpcHandler(async (_, _, _) =>
        {
            entered.TrySetResult();
            await held.Task;
            return "1";
        });

        byte[] body = Wire.Body(w => w.Write(1).WriteSizePrefixedString("m").WriteBytesField(System.Text.Encoding.UTF8.GetBytes("{}")));
        _peer.Send(9, PacketType.Request, (byte)OpcodeClient.WindowBridgeRpc, body);
        await entered.Task.WithTimeout(Timeout);
        _peer.Send(9, PacketType.Request, (byte)OpcodeClient.WindowBridgeRpc, body);

        var response = _peer.Take(PacketType.Response, Timeout);
        Assert.Equal(9u, response.RequestId);
        Assert.Equal((byte)JustCefStatus.InvalidRequest, response.Body[0]);

        held.TrySetResult();
        var second = _peer.Take(PacketType.Response, Timeout);
        Assert.Equal(9u, second.RequestId);
        Assert.Equal((byte)JustCefStatus.Ok, second.Body[0]);
    }

    [Fact]
    public async Task OutgoingPacketsKeepOrder()
    {
        var calls = new List<Task>();
        for (int i = 1; i <= 2000; i++)
            calls.Add(Process.PrintAsync($"line {i}"));

        uint? previous = null;
        for (int i = 1; i <= 2000; i++)
        {
            var packet = _peer.Take(Timeout);
            Assert.Equal(PacketType.Request, packet.Type);
            Assert.Equal($"line {i}", Encoding.UTF8.GetString(packet.Body));
            Assert.NotEqual(0u, packet.RequestId);
            if (previous != null)
                Assert.True(packet.RequestId > previous);
            previous = packet.RequestId;
            Reply(packet, Wire.Ok());
        }

        await Task.WhenAll(calls).WithTimeout(Timeout);
        Assert.Equal(0, Process.PendingCallCount);
    }

    [Fact]
    public async Task IncomingNotificationsKeepOrderAndEventsLeaveTheReader()
    {
        var window = await CreateWindowAsync(1);
        var observed = new List<(bool IsLoading, int Thread)>();
        var done = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        window.OnLoadingStateChanged += info =>
        {
            observed.Add((info.IsLoading, Environment.CurrentManagedThreadId));
            if (observed.Count == 2000)
                done.TrySetResult();
        };

        for (int i = 0; i < 2000; i++)
            _peer.Send(0, PacketType.Notification, (byte)OpcodeClientNotification.WindowLoadingStateChanged, Wire.Body(w => w.Write(1).Write(i % 2 == 0).Write(false).Write(false)));

        await done.Task.WithTimeout(Timeout);
        Assert.Equal(Enumerable.Range(0, 2000).Select(i => i % 2 == 0), observed.Select(e => e.IsLoading));
        Assert.NotEqual(0, Process.ReaderThreadId);
        Assert.DoesNotContain(observed, e => e.Thread == Process.ReaderThreadId || e.Thread == Process.WriterThreadId);
        await window.WaitUntilLoadedAsync().WithTimeout(Timeout);
    }

    [Fact]
    public async Task LargePayloadsRoundTrip()
    {
        byte[] big = new byte[5 * 1024 * 1024];
        Random.Shared.NextBytes(big);
        var call = Process.CallAsync(OpcodeController.Echo, new PacketWriter().WriteBytes(big));
        var request = _peer.Take(Timeout);
        Assert.Equal(big, request.Body);

        Reply(request, Wire.Ok(w => w.WriteBytes(big)));
        var reader = await call.WithTimeout(Timeout);
        Assert.Equal(big, reader.ReadBytes(reader.RemainingSize));
    }

    [Fact]
    public async Task NonOkStatusThrowsRemoteException()
    {
        var call = Process.WindowShowAsync(1);
        var request = _peer.Take(Timeout);
        Reply(request, Wire.Failure(JustCefStatus.NotFound, "gone"));

        var exception = await Assert.ThrowsAsync<JustCefRemoteException>(() => call.WithTimeout(Timeout));
        Assert.Equal(JustCefStatus.NotFound, exception.Status);
        Assert.Equal("gone", exception.Message);
    }

    [Fact]
    public async Task TimeoutSendsCancelAndDropsLateResponse()
    {
        Process.DefaultCallTimeout = TimeSpan.FromMilliseconds(200);
        var call = Process.WindowGetZoomAsync(1);
        var request = _peer.Take(Timeout);

        await Assert.ThrowsAsync<TimeoutException>(() => call.WithTimeout(Timeout));
        var cancel = _peer.Take(Timeout);
        Assert.Equal(PacketType.Cancel, cancel.Type);
        Assert.Equal(request.RequestId, cancel.RequestId);
        Assert.Equal(request.Opcode, cancel.Opcode);
        Assert.Empty(cancel.Body);

        Reply(request, Wire.Ok(w => w.Write(1.0)));
        Process.DefaultCallTimeout = TimeSpan.FromSeconds(30);
        var next = Process.PingAsync();
        var ping = _peer.Take(Timeout);
        Reply(ping, Wire.Ok());
        await next.WithTimeout(Timeout);
        Assert.Equal(0, Process.PendingCallCount);
    }

    [Fact]
    public async Task CancellationSendsCancel()
    {
        using var cts = new CancellationTokenSource();
        var call = Process.WindowGetZoomAsync(1, cts.Token);
        var request = _peer.Take(Timeout);
        cts.Cancel();

        await Assert.ThrowsAnyAsync<OperationCanceledException>(() => call.WithTimeout(Timeout));
        var cancel = _peer.Take(Timeout);
        Assert.Equal(PacketType.Cancel, cancel.Type);
        Assert.Equal(request.RequestId, cancel.RequestId);
    }

    [Fact]
    public async Task PickersWaitPastTheDefaultTimeout()
    {
        Process.DefaultCallTimeout = TimeSpan.FromMilliseconds(100);
        var call = Process.WindowPickDirectoryAsync(1);
        var request = _peer.Take(Timeout);
        await Task.Delay(400);
        Assert.False(call.IsCompleted);
        Reply(request, Wire.Ok(w => w.WriteSizePrefixedString("/tmp")));
        Assert.Equal("/tmp", await call.WithTimeout(Timeout));
    }

    [Fact]
    public async Task EofFailsPendingCallsAndClosesWindowsOnce()
    {
        var window = await CreateWindowAsync(1);
        int closeCount = 0;
        window.OnClose += () => Interlocked.Increment(ref closeCount);
        var calls = Enumerable.Range(0, 20).Select(_ => Process.PingAsync()).ToList();
        for (int i = 0; i < calls.Count; i++)
            _peer.Take(Timeout);

        _peer.CloseOutput();
        foreach (var call in calls)
            await Assert.ThrowsAnyAsync<OperationCanceledException>(() => call.WithTimeout(Timeout));

        await Process.WaitForExitAsync().WithTimeout(Timeout);
        await window.WaitForExitAsync().WithTimeout(Timeout);
        await TestUtil.WaitUntil(() => Volatile.Read(ref closeCount) == 1, Timeout, "OnClose fired");
        Process.Dispose();
        await Task.Delay(100);
        Assert.Equal(1, Volatile.Read(ref closeCount));
        await Assert.ThrowsAnyAsync<OperationCanceledException>(() => Process.PingAsync());
        await _peer.PeerEof.Task.WithTimeout(Timeout);
    }

    [Fact]
    public async Task OversizedHeaderClosesTheConnection()
    {
        var call = Process.PingAsync();
        _peer.Take(Timeout);
        byte[] header = new byte[Wire.HeaderSize];
        Wire.WriteHeader(header, Wire.MaxBodySize + 1, 1, PacketType.Request, 0);
        _peer.SendRaw(header);

        await Assert.ThrowsAnyAsync<OperationCanceledException>(() => call.WithTimeout(Timeout));
        await Process.WaitForExitAsync().WithTimeout(Timeout);
        await _peer.PeerEof.Task.WithTimeout(Timeout);
    }

    [Fact]
    public async Task DisposeSendsExitThenClosesThePipe()
    {
        var sw = System.Diagnostics.Stopwatch.StartNew();
        Process.Dispose();
        Process.Dispose();
        Assert.True(sw.Elapsed < TimeSpan.FromMilliseconds(200));

        var packet = _peer.Take(Timeout);
        Assert.Equal(PacketType.Notification, packet.Type);
        Assert.Equal((byte)OpcodeControllerNotification.Exit, packet.Opcode);
        Assert.Equal(0u, packet.RequestId);
        await _peer.PeerEof.Task.WithTimeout(Timeout);
        await Process.WaitForExitAsync().WithTimeout(Timeout);
    }

    [Fact]
    public async Task OversizedCallFailsLocally()
    {
        await Assert.ThrowsAnyAsync<InvalidOperationException>(() => Process.EchoAsync(new byte[Wire.MaxBodySize + 1]));
        Assert.Equal(0, Process.PendingCallCount);
    }

    [Fact]
    public async Task UnknownNativeRequestAnswersUnsupported()
    {
        _peer.Send(5, PacketType.Request, 200);
        var response = _peer.Take(Timeout);
        Assert.Equal(PacketType.Response, response.Type);
        Assert.Equal(5u, response.RequestId);
        Assert.Equal(200, response.Opcode);
        Assert.Equal((byte)JustCefStatus.Unsupported, response.Body[0]);
        await Task.CompletedTask;
    }
}
