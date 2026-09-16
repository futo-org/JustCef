using System.Collections.Concurrent;
using System.Diagnostics;
using System.Text.Json.Nodes;
using JustCef.Tests.Infrastructure;
using Xunit;
using static JustCef.JustCefProcess;
using static JustCef.Tests.Infrastructure.FakeNativeHarness;

namespace JustCef.Tests.Fake;

[Trait("Category", "Fake")]
public class HazardTests
{
    private static readonly TimeSpan Wait = TimeSpan.FromSeconds(15);

    [Fact]
    public Task C1_LoadingStateKeepsWireOrderAcrossWindows() => TestUtil.RunScenario(async () =>
    {
        await using var harness = await StartAsync();
        var windows = new List<JustCefWindow>();
        var observed = new Dictionary<int, List<bool>>();
        for (int i = 0; i < 20; i++)
        {
            var window = await harness.Process.CreateWindowAsync("", 0, 0);
            var list = new List<bool>();
            observed[window.Identifier] = list;
            window.OnLoadingStateChanged += info =>
            {
                lock (list)
                    list.Add(info.IsLoading);
            };
            windows.Add(window);
        }

        const int count = 10000;
        var ids = new JsonArray(windows.Select(w => (JsonNode)w.Identifier).ToArray());
        await harness.CommandAsync(new JsonObject { ["cmd"] = "loadingBurst", ["browserIds"] = ids, ["count"] = count }, "loadingBurstDone", TimeSpan.FromSeconds(60));

        int perBrowser = count / windows.Count;
        await TestUtil.WaitUntil(() => observed.Values.All(list =>
        {
            lock (list)
                return list.Count == perBrowser;
        }), TimeSpan.FromSeconds(30), "every window observed its loading events");

        var expected = Enumerable.Range(0, perBrowser).Select(i => (perBrowser - 1 - i) % 2 == 1).ToList();
        foreach (var window in windows)
        {
            lock (observed[window.Identifier])
                Assert.Equal(expected, observed[window.Identifier]);
            await window.WaitUntilLoadedAsync().WithTimeout(Wait);
        }
    }, TimeSpan.FromSeconds(120));

    [Fact]
    public Task C2_TrafficRightAfterCreateReplyIsRouted() => TestUtil.RunScenario(async () =>
    {
        await using var harness = await StartAsync();
        await harness.ConfigureAsync("WindowCreate", "ok", c => c["afterReply"] = new JsonArray(
            new JsonObject { ["action"] = "proxyRequest", ["browserId"] = "$new", ["url"] = "https://app/early" },
            new JsonObject { ["action"] = "notify", ["opcode"] = 17, ["browserId"] = "$new", ["isLoading"] = true },
            new JsonObject { ["action"] = "notify", ["opcode"] = 3, ["browserId"] = "$new" }));

        var proxied = new TaskCompletionSource<string>(TaskCreationOptions.RunContinuationsAsynchronously);
        var window = await harness.Process.CreateWindowAsync("", 0, 0, proxyRequests: true, requestProxy: (_, request) =>
        {
            proxied.TrySetResult(request.Url);
            return Task.FromResult<IPCResponse?>(Http.Text("early"));
        });

        var loading = new ConcurrentQueue<bool>();
        int closeCount = 0;
        window.OnLoadingStateChanged += info => loading.Enqueue(info.IsLoading);
        window.OnClose += () => Interlocked.Increment(ref closeCount);

        Assert.Equal("https://app/early", await proxied.Task.WithTimeout(Wait));
        var response = await harness.WaitForEventAsync(e => Name(e) == "proxyResponse");
        Assert.Equal(0, (int)response["status"]!);
        Assert.Equal(200, (int)response["statusCode"]!);
        Assert.Equal(1, (int)response["bodyType"]!);
        Assert.Equal(Http.Sha256Hex("early"u8.ToArray()), (string?)response["sha256"]);

        await window.WaitForExitAsync().WithTimeout(Wait);
        await TestUtil.WaitUntil(() => Volatile.Read(ref closeCount) == 1, Wait, "OnClose fired");
        await Task.Delay(200);
        Assert.Equal(1, Volatile.Read(ref closeCount));
        Assert.Equal(new[] { true }, loading.ToArray());
        Assert.Null(harness.Process.GetBrowser(window.Identifier));
        Assert.Equal(0, await harness.ViolationsAsync());
    }, TimeSpan.FromSeconds(60));

    [Fact]
    public Task C3_ResponseBeforeExitAndEofStillCompletes() => TestUtil.RunScenario(async () =>
    {
        await using var harness = await StartAsync();
        var window = await harness.Process.CreateWindowAsync("", 0, 0);
        int closeCount = 0;
        window.OnClose += () => Interlocked.Increment(ref closeCount);
        await harness.ConfigureAsync("WindowGetSize", "ok", c => c["afterReply"] = new JsonArray(new JsonObject { ["action"] = "exit" }));

        var size = await window.GetSizeAsync().WithTimeout(Wait);
        Assert.Equal((800, 600), size);
        await harness.Process.WaitForExitAsync().WithTimeout(Wait);
        await TestUtil.WaitUntil(() => Volatile.Read(ref closeCount) == 1, Wait, "OnClose fired");
        await Assert.ThrowsAnyAsync<OperationCanceledException>(() => window.GetSizeAsync());
    }, TimeSpan.FromSeconds(60));

    [Fact]
    public Task C4_UnknownWindowAnswersNotFoundQuickly() => TestUtil.RunScenario(async () =>
    {
        await using var harness = await StartAsync();
        var stopwatch = Stopwatch.StartNew();
        var proxy = await harness.CommandAsync(new JsonObject { ["cmd"] = "proxyRequest", ["browserId"] = 999 }, "proxyResponse");
        var modify = await harness.CommandAsync(new JsonObject { ["cmd"] = "modifyRequest", ["browserId"] = 999 }, "modifyResponse");
        var bridge = await harness.CommandAsync(new JsonObject { ["cmd"] = "bridgeRpc", ["browserId"] = 999 }, "bridgeRpcResponse");
        var view = await harness.CommandAsync(new JsonObject { ["cmd"] = "viewCreated", ["parentId"] = 999, ["viewId"] = 1000 }, "viewCreatedResponse");
        stopwatch.Stop();

        Assert.Equal(3, (int)proxy["status"]!);
        Assert.Equal(3, (int)modify["status"]!);
        Assert.Equal(3, (int)bridge["status"]!);
        Assert.Equal(3, (int)view["status"]!);
        Assert.True(stopwatch.Elapsed < TimeSpan.FromSeconds(2), $"NotFound took {stopwatch.Elapsed}.");
        Assert.Equal(0, await harness.ViolationsAsync());
    }, TimeSpan.FromSeconds(60));

    [Fact]
    public async Task C5_BlockingResultInsideSingleThreadedContextDoesNotDeadlock()
    {
        using var context = new SingleThreadSynchronizationContext();
        FakeNativeHarness? harness = null;
        try
        {
            harness = await StartAsync().WithTimeout(Wait);
            var process = harness.Process;
            int eventThread = 0;

            var result = await context.RunAsync(() =>
            {
                var window = process.CreateWindowAsync("https://app/", 0, 0).Result;
                window.OnLoadingStateChanged += _ => eventThread = Environment.CurrentManagedThreadId;
                var size = window.GetSizeAsync().Result;
                window.WaitUntilLoadedAsync().Wait();
                window.NavigateAsync("https://app/second").Wait();
                process.EchoAsync(new byte[] { 4, 5, 6 }).Wait();
                var echo = process.CallAsync(OpcodeController.Echo, new PacketWriter().WriteBytes(new byte[] { 4, 5, 6 })).Result;
                window.SetTitleAsync("title").GetAwaiter().GetResult();
                return (size, echo.RemainingSize);
            }).WithTimeout(TimeSpan.FromSeconds(20));

            Assert.Equal((800, 600), result.size);
            Assert.Equal(3, result.RemainingSize);
            await TestUtil.WaitUntil(() => Volatile.Read(ref eventThread) != 0, Wait, "an event ran");
            Assert.NotEqual(context.ThreadId, eventThread);
            Assert.NotEqual(process.ReaderThreadId, eventThread);
        }
        finally
        {
            if (harness != null)
                await harness.DisposeAsync();
        }
    }

    [Fact]
    public Task C6_HandlerExceptionsAnswerErrorAndKeepWindowOpen() => TestUtil.RunScenario(async () =>
    {
        await using var harness = await StartAsync();
        var window = await harness.Process.CreateWindowAsync("", 0, 0,
            proxyRequests: true,
            requestProxy: (_, _) => throw new InvalidOperationException("proxy boom"),
            modifyRequests: true,
            requestModifier: (_, _) => throw new InvalidOperationException("modify boom"),
            bridgeEnabled: true,
            bridgeRpcHandler: (_, _, _) => throw new InvalidOperationException("bridge boom"));

        var proxy = await harness.CommandAsync(new JsonObject { ["cmd"] = "proxyRequest", ["browserId"] = window.Identifier }, "proxyResponse");
        Assert.Equal(1, (int)proxy["status"]!);
        Assert.Equal("proxy boom", (string?)proxy["message"]);

        var modify = await harness.CommandAsync(new JsonObject { ["cmd"] = "modifyRequest", ["browserId"] = window.Identifier }, "modifyResponse");
        Assert.Equal(1, (int)modify["status"]!);
        Assert.Equal("modify boom", (string?)modify["message"]);

        var bridge = await harness.CommandAsync(new JsonObject { ["cmd"] = "bridgeRpc", ["browserId"] = window.Identifier }, "bridgeRpcResponse");
        Assert.Equal(1, (int)bridge["status"]!);
        Assert.Equal("bridge boom", (string?)bridge["message"]);

        Assert.Equal((800, 600), await window.GetSizeAsync());
        Assert.False(window.WaitForExitAsync().IsCompleted);
        var state = await harness.StateAsync();
        var entry = state["windows"]!.AsArray().Single(w => (int)w!["id"]! == window.Identifier)!;
        Assert.False((bool)entry["closed"]!);
        Assert.Equal(0, (int)state["violations"]!);
    }, TimeSpan.FromSeconds(60));

    [Fact]
    public Task C7_HeldCallTimesOutAndSendsCancel() => TestUtil.RunScenario(async () =>
    {
        await using var harness = await StartAsync(process => process.DefaultCallTimeout = TimeSpan.FromMilliseconds(500));
        var window = await harness.Process.CreateWindowAsync("", 0, 0);
        await harness.ConfigureAsync("WindowGetZoom", "hold");

        var stopwatch = Stopwatch.StartNew();
        await Assert.ThrowsAsync<TimeoutException>(() => window.GetZoomAsync().WithTimeout(Wait));
        stopwatch.Stop();
        Assert.True(stopwatch.Elapsed < TimeSpan.FromSeconds(5), $"Timeout took {stopwatch.Elapsed}.");

        var cancel = await harness.WaitForEventAsync(e => Name(e) == "cancelReceived" && (int)e["opcode"]! == 56);
        Assert.True((bool)cancel["matched"]!);
        Assert.True((bool)cancel["held"]!);

        Assert.Equal((800, 600), await window.GetSizeAsync());
        Assert.Equal(0, harness.Process.PendingCallCount);
        Assert.Equal(0, await harness.ViolationsAsync());
    }, TimeSpan.FromSeconds(60));

    [Fact]
    public Task BlockingAfterCreateDoesNotStallEventsOrOtherCreates() => TestUtil.RunScenario(async () =>
    {
        await using var harness = await StartAsync();
        var inContinuation = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        using var release = new ManualResetEventSlim(false);

        var blocked = Task.Run(async () =>
        {
            var window = await harness.Process.CreateWindowAsync("", 0, 0);
            var focused = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
            window.OnFocused += () => focused.TrySetResult();
            inContinuation.TrySetResult();
            release.Wait(Wait);
            return (window, focused);
        });

        await inContinuation.Task.WithTimeout(Wait);
        var second = await harness.Process.CreateWindowAsync("", 0, 0).WithTimeout(Wait);
        Assert.NotEqual(0, second.Identifier);

        release.Set();
        var (first, firstFocused) = await blocked.WithTimeout(Wait);
        await harness.NotifyAsync(5, first.Identifier);
        await firstFocused.Task.WithTimeout(Wait);
    }, TimeSpan.FromSeconds(60));

    [Fact]
    public Task AFailedProxyResponseDisposesTheDataSource() => TestUtil.RunScenario(async () =>
    {
        await using var harness = await StartAsync();
        var source = new PatternDataSource(1024);
        var window = await harness.Process.CreateWindowAsync("", 0, 0, proxyRequests: true, requestProxy: (_, _) => Task.FromResult<IPCResponse?>(new IPCResponse
        {
            StatusCode = 200,
            StatusText = "OK",
            Headers = new Dictionary<string, List<string>> { ["content-length"] = null! },
            DataSource = source,
        }));

        var response = await harness.CommandAsync(new JsonObject { ["cmd"] = "proxyRequest", ["browserId"] = window.Identifier, ["url"] = "https://app/broken" }, "proxyResponse");
        Assert.NotEqual(0, (int)response["status"]!);
        await TestUtil.WaitUntil(() => source.DisposeCount == 1, Wait, "the data source was disposed");
    }, TimeSpan.FromSeconds(60));

    [Fact]
    public Task C8_GrandchildHoldingPipesStillFailsPendingCalls() => TestUtil.RunScenario(async () =>
    {
        await using var harness = await StartAsync();
        var window = await harness.Process.CreateWindowAsync("", 0, 0);
        int closeCount = 0;
        window.OnClose += () => Interlocked.Increment(ref closeCount);
        await harness.ConfigureAsync("WindowGetZoom", "hold");
        var pending = Enumerable.Range(0, 5).Select(_ => window.GetZoomAsync()).ToList();
        await TestUtil.WaitUntil(() => harness.EventsNamed("held").Count == 5, Wait, "five held calls");

        var spawned = await harness.CommandAsync(new JsonObject { ["cmd"] = "spawnGrandchildAndExit", ["seconds"] = 20 }, "grandchildSpawned");
        int grandchildPid = (int)spawned["pid"]!;
        try
        {
            await TestUtil.WaitUntil(() => harness.Process.HasExited, Wait, "the child exited");
            var exited = Stopwatch.StartNew();
            foreach (var call in pending)
                await Assert.ThrowsAnyAsync<OperationCanceledException>(() => call.WithTimeout(Wait));
            exited.Stop();
            Assert.True(exited.Elapsed < TimeSpan.FromMilliseconds(1500), $"Pending calls failed {exited.Elapsed} after the child exited.");
            await TestUtil.WaitUntil(() => Volatile.Read(ref closeCount) == 1, Wait, "OnClose fired");
            await harness.Process.WaitForExitAsync().WithTimeout(Wait);
        }
        finally
        {
            try
            {
                System.Diagnostics.Process.GetProcessById(grandchildPid).Kill();
            }
            catch
            {
            }
        }
    }, TimeSpan.FromSeconds(60));

    [Fact]
    public Task C9_ConcurrentDisposeWithCallsInFlight() => TestUtil.RunScenario(async () =>
    {
        await using var harness = await StartAsync();
        var windows = new List<JustCefWindow>();
        var closeCounts = new ConcurrentDictionary<int, int>();
        for (int i = 0; i < 5; i++)
        {
            var window = await harness.Process.CreateWindowAsync("", 0, 0);
            closeCounts[window.Identifier] = 0;
            window.OnClose += () => closeCounts.AddOrUpdate(window.Identifier, 1, (_, v) => v + 1);
            windows.Add(window);
        }

        await harness.ConfigureAsync("WindowGetZoom", "hold");
        var calls = windows.SelectMany(w => Enumerable.Range(0, 10).Select(_ => (Task)w.GetZoomAsync())).ToList();
        calls.AddRange(windows.Select(w => (Task)w.GetSizeAsync()));
        await TestUtil.WaitUntil(() => harness.EventsNamed("held").Count == 50, Wait, "held calls");

        using var gate = new ManualResetEventSlim(false);
        var disposers = Enumerable.Range(0, 100).Select(_ => Task.Run(() =>
        {
            gate.Wait();
            harness.Process.Dispose();
        })).ToList();
        gate.Set();
        await Task.WhenAll(disposers).WithTimeout(Wait);

        foreach (var call in calls)
        {
            try
            {
                await call.WithTimeout(Wait);
            }
            catch (OperationCanceledException)
            {
            }
        }

        await TestUtil.WaitUntil(() => closeCounts.Values.All(v => v >= 1), Wait, "every window closed");
        await Task.Delay(300);
        Assert.All(closeCounts.Values, v => Assert.Equal(1, v));
        await harness.WaitForChildExitAsync().WithTimeout(Wait);
        Assert.True(harness.Process.HasExited);
    }, TimeSpan.FromSeconds(60));

    [Fact]
    public Task C15_UserCodeNeverRunsOnTransportThreads() => TestUtil.RunScenario(async () =>
    {
        await using var harness = await StartAsync();
        var threads = new ConcurrentBag<(string Where, int Thread)>();
        void Record(string where) => threads.Add((where, Environment.CurrentManagedThreadId));

        var window = await harness.Process.CreateWindowAsync("https://app/", 0, 0,
            proxyRequests: true,
            requestProxy: (_, _) =>
            {
                Record("proxy");
                return Task.FromResult<IPCResponse?>(Http.Text("x"));
            },
            modifyRequests: true,
            requestModifier: (_, request) =>
            {
                Record("modify");
                return request;
            },
            bridgeEnabled: true,
            bridgeRpcHandler: (_, _, _) =>
            {
                Record("bridge");
                return Task.FromResult<string?>("1");
            },
            viewsEnabled: true,
            viewCreatedHandler: _ =>
            {
                Record("view");
                return Task.CompletedTask;
            });
        Record("createContinuation");
        window.OnLoadingStateChanged += _ => Record("loading");
        window.OnFocused += () => Record("focused");
        window.OnFrameLoadEnd += _ => Record("frameLoadEnd");
        window.OnDevToolsEvent += (_, _) => Record("devtools");
        window.OnClose += () => Record("close");

        await harness.CommandAsync(new JsonObject { ["cmd"] = "proxyRequest", ["browserId"] = window.Identifier }, "proxyResponse");
        await harness.CommandAsync(new JsonObject { ["cmd"] = "modifyRequest", ["browserId"] = window.Identifier }, "modifyResponse");
        await harness.CommandAsync(new JsonObject { ["cmd"] = "bridgeRpc", ["browserId"] = window.Identifier }, "bridgeRpcResponse");
        await harness.CommandAsync(new JsonObject { ["cmd"] = "viewCreated", ["parentId"] = window.Identifier, ["viewId"] = 50 }, "viewCreatedResponse");
        await harness.NotifyAsync(5, window.Identifier);
        await harness.NotifyAsync(16, window.Identifier);
        await window.NavigateAsync("https://app/next");
        await window.CloseAsync();
        await TestUtil.WaitUntil(() => threads.Any(t => t.Where == "close"), Wait, "close event");

        var names = threads.Select(t => t.Where).ToHashSet();
        foreach (var expected in new[] { "proxy", "modify", "bridge", "view", "loading", "focused", "frameLoadEnd", "devtools", "close", "createContinuation" })
            Assert.Contains(expected, names);

        int reader = harness.Process.ReaderThreadId;
        int writer = harness.Process.WriterThreadId;
        Assert.NotEqual(0, reader);
        Assert.All(threads, t =>
        {
            Assert.NotEqual(reader, t.Thread);
            Assert.NotEqual(writer, t.Thread);
        });
    }, TimeSpan.FromSeconds(60));

    [Fact]
    public Task C16_LargeEchoBothDirectionsAtOnce() => TestUtil.RunScenario(async () =>
    {
        await using var harness = await StartAsync(process => process.DefaultCallTimeout = TimeSpan.FromSeconds(120));
        const int size = 200 * 1024 * 1024;
        byte[] data = new byte[size];
        Random.Shared.NextBytes(data);

        var nativeEcho = harness.CommandAsync(new JsonObject { ["cmd"] = "echo", ["size"] = size }, "echoResponse", TimeSpan.FromSeconds(150));
        var controllerEcho = harness.Process.CallAsync(OpcodeController.Echo, new PacketWriter().WriteBytes(data));

        var echoed = await controllerEcho.WithTimeout(TimeSpan.FromSeconds(150));
        Assert.True(data.AsSpan().SequenceEqual(echoed.ReadBytes(echoed.RemainingSize)));

        var result = await nativeEcho;
        Assert.Equal(0, (int)result["status"]!);
        Assert.Equal(size, (int)result["receivedSize"]!);
        Assert.True((bool)result["match"]!);
        Assert.Equal(0, await harness.ViolationsAsync());
    }, TimeSpan.FromSeconds(180));

    [Fact]
    public Task ReadyWithWrongProtocolVersionFailsStartup() => TestUtil.RunScenario(async () =>
    {
        await using var harness = await StartAsync(extraArguments: "--fake-ready-version 1", waitForReady: false);
        var exception = await Assert.ThrowsAsync<JustCefStartupException>(() => harness.Process.WaitForReadyAsync().WithTimeout(Wait));
        Assert.Equal(JustCefStartupFailure.ProtocolMismatch, exception.Failure);
    }, TimeSpan.FromSeconds(60));

    [Fact]
    public Task NavigateFailsOnMainFrameErrorButIgnoresAborted() => TestUtil.RunScenario(async () =>
    {
        await using var harness = await StartAsync();
        var window = await harness.Process.CreateWindowAsync("", 0, 0);

        await harness.ConfigureAsync("WindowLoadUrl", "ok", c => c["afterReply"] = new JsonArray(
            new JsonObject { ["action"] = "notify", ["opcode"] = 17, ["browserId"] = window.Identifier, ["isLoading"] = true },
            new JsonObject { ["action"] = "notify", ["opcode"] = 15, ["browserId"] = window.Identifier, ["errorCode"] = -3, ["errorText"] = "net::ERR_ABORTED" }));
        await window.NavigateAsync("https://app/aborted").WithTimeout(Wait);

        await harness.ConfigureAsync("WindowLoadUrl", "ok", c => c["afterReply"] = new JsonArray(
            new JsonObject { ["action"] = "notify", ["opcode"] = 17, ["browserId"] = window.Identifier, ["isLoading"] = true },
            new JsonObject { ["action"] = "notify", ["opcode"] = 15, ["browserId"] = window.Identifier, ["errorCode"] = -105, ["errorText"] = "net::ERR_NAME_NOT_RESOLVED" }));
        var exception = await Assert.ThrowsAsync<InvalidOperationException>(() => window.NavigateAsync("https://app/missing").WithTimeout(Wait));
        Assert.Contains("ERR_NAME_NOT_RESOLVED", exception.Message);

        await harness.ConfigureAsync("WindowLoadUrl", "ok", c => c["afterReply"] = new JsonArray(
            new JsonObject { ["action"] = "notify", ["opcode"] = 3, ["browserId"] = window.Identifier }));
        await Assert.ThrowsAsync<InvalidOperationException>(() => window.NavigateAsync("https://app/closed").WithTimeout(Wait));
        Assert.True(window.WaitForExitAsync().IsCompleted);
    }, TimeSpan.FromSeconds(60));

    [Fact]
    public Task DisposeExitsChildCleanly() => TestUtil.RunScenario(async () =>
    {
        var harness = await StartAsync();
        var window = await harness.Process.CreateWindowAsync("", 0, 0);
        var closed = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        window.OnClose += () => closed.TrySetResult();
        harness.Process.Dispose();
        await harness.WaitForChildExitAsync().WithTimeout(Wait);
        Assert.True(harness.Process.HasExited);
        Assert.Equal(0, harness.Process.ChildProcess!.ExitCode);
        await closed.Task.WithTimeout(Wait);
        await harness.WaitForEventAsync(e => Name(e) == "exiting");
        await Assert.ThrowsAnyAsync<OperationCanceledException>(() => window.GetSizeAsync());
        await harness.DisposeAsync();
    }, TimeSpan.FromSeconds(60));

    [Fact]
    public Task DisposeKillsUnresponsiveChild() => TestUtil.RunScenario(async () =>
    {
        await using var harness = await StartAsync();
        await harness.AckAsync(new JsonObject { ["cmd"] = "stopReading" });
        var stopwatch = Stopwatch.StartNew();
        harness.Process.Dispose();
        Assert.True(stopwatch.Elapsed < TimeSpan.FromMilliseconds(200), $"Dispose blocked for {stopwatch.Elapsed}.");
        await TestUtil.WaitUntil(() => harness.Process.HasExited, TimeSpan.FromSeconds(10), "the child was killed");
    }, TimeSpan.FromSeconds(60));
}
