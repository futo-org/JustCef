using System.Collections.Concurrent;
using System.Diagnostics;
using System.Security.Cryptography;
using System.Text.Json;
using JustCef.Tests.Infrastructure;
using Xunit;
using static JustCef.Tests.RealNative.RealNativeHarness;

namespace JustCef.Tests.RealNative;

[Trait("Category", "RealNative")]
public class RealNativeTests
{
    [RealNativeFact]
    public Task N0_StartCreateAndBridge() => TestUtil.RunScenario(async () =>
    {
        await using var harness = await StartAsync();
        var window = await harness.OpenPageAsync((_, method, json) => Task.FromResult<string?>(method == "ping" ? "\"pong\"" : json));

        var result = await EvaluateAsync(window, "bridge.rpc.call('ping', null)");
        Assert.Equal("pong", result.GetString());
        var size = await window.GetSizeAsync();
        Assert.True(size.Width > 0);
    }, TimeSpan.FromSeconds(120));

    [RealNativeFact]
    public Task N1_ConcurrentBridgeCallsWhoseHandlersCallNative() => TestUtil.RunScenario(async () =>
    {
        await using var harness = await StartAsync();
        const int count = 8;
        int arrived = 0;
        var allArrived = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var window = await harness.OpenPageAsync(async (w, method, json) =>
        {
            if (Interlocked.Increment(ref arrived) == count)
                allArrived.TrySetResult();
            await allArrived.Task.WaitAsync(TimeSpan.FromSeconds(20));

            var size = await w.GetSizeAsync();
            var devTools = await w.ExecuteDevToolsMethodAsync("Runtime.evaluate", "{\"expression\":\"1+1\",\"returnByValue\":true}");
            return JsonSerializer.Serialize(new { width = size.Width, ok = devTools.Success });
        });

        var result = await EvaluateAsync(window, $"Promise.all([...Array({count}).keys()].map(i => bridge.rpc.call('work', {{ i }}))).then(r => JSON.stringify(r))").WithTimeout(Wait);
        var items = JsonDocument.Parse(result.GetString()!).RootElement.EnumerateArray().ToList();
        Assert.Equal(count, items.Count);
        Assert.All(items, item =>
        {
            Assert.True(item.GetProperty("ok").GetBoolean());
            Assert.True(item.GetProperty("width").GetInt32() > 0);
        });
    }, TimeSpan.FromSeconds(180));

    [RealNativeFact]
    public Task N2_LargeBridgePayloadWhileCallsPending() => TestUtil.RunScenario(async () =>
    {
        await using var harness = await StartAsync();
        var release = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        int held = 0;
        var window = await harness.OpenPageAsync(async (_, method, json) =>
        {
            switch (method)
            {
                case "hold":
                    Interlocked.Increment(ref held);
                    await release.Task;
                    return "true";
                case "big":
                    return json;
                default:
                    return "null";
            }
        });

        await EvaluateAsync(window, "window.__pending = [0,1,2,3].map(() => bridge.rpc.call('hold', null)); true", awaitPromise: false);
        await TestUtil.WaitUntil(() => Volatile.Read(ref held) == 4, Wait, "four held bridge calls");

        const int size = 20 * 1024 * 1024;
        var length = await EvaluateAsync(window, $"bridge.rpc.call('big', 'x'.repeat({size})).then(r => r.length)").WithTimeout(Wait);
        Assert.Equal(size, length.GetInt32());

        await EvaluateAsync(window, "bridge.rpc.register('echo', p => p); true");
        string echoed = await window.CallBridgeRpcAsync("echo", JsonSerializer.Serialize(new string('y', size))).WithTimeout(Wait);
        Assert.Equal(size + 2, echoed.Length);

        release.TrySetResult();
        var completed = await EvaluateAsync(window, "Promise.all(window.__pending).then(r => r.length)").WithTimeout(Wait);
        Assert.Equal(4, completed.GetInt32());
    }, TimeSpan.FromSeconds(180));

    [RealNativeFact]
    public Task N3_LargePostAndSlowModifierDoNotBlockOtherRequests() => TestUtil.RunScenario(async () =>
    {
        await using var harness = await StartAsync();
        const int size = 20 * 1024 * 1024;
        harness.Route("/upload", request =>
        {
            using var hash = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
            long total = 0;
            foreach (var element in request.Elements)
            {
                if (element is IPCProxyBodyElementBytes bytes)
                {
                    hash.AppendData(bytes.Data);
                    total += bytes.Data.Length;
                }
                else if (element is IPCProxyBodyElementFile file)
                {
                    byte[] data = File.ReadAllBytes(file.FileName);
                    hash.AppendData(data);
                    total += data.Length;
                }
            }
            return Task.FromResult<IPCResponse?>(Text($"{total}:{Convert.ToHexString(hash.GetHashAndReset()).ToLowerInvariant()}"));
        });
        harness.Route("/slow", _ => Task.FromResult<IPCResponse?>(Text("slow")));
        harness.Route("/small", _ => Task.FromResult<IPCResponse?>(Text("small")));

        var window = await harness.OpenPageAsync(requestModifier: (_, request) =>
        {
            if (request.Url.Contains("/slow"))
                Thread.Sleep(TimeSpan.FromSeconds(5));
            request.Headers["X-Modified"] = new List<string> { "1" };
            return request;
        });

        string script = $$"""
            (async () => {
                const t0 = performance.now();
                const body = new Uint8Array({{size}});
                for (let i = 0; i < body.length; i++) body[i] = i % 251;
                const slow = fetch('/slow').then(r => r.text()).then(() => performance.now() - t0);
                const upload = fetch('/upload', { method: 'POST', body }).then(r => r.text());
                const small = Promise.all([...Array(20).keys()].map(i => fetch('/small?' + i).then(r => r.text()))).then(() => performance.now() - t0);
                const [slowMs, uploadResult, smallMs] = await Promise.all([slow, upload, small]);
                return JSON.stringify({ slowMs, uploadResult, smallMs });
            })()
            """;
        var result = await EvaluateAsync(window, script).WithTimeout(Wait);
        var json = JsonDocument.Parse(result.GetString()!).RootElement;

        byte[] expected = new byte[size];
        for (int i = 0; i < size; i++)
            expected[i] = (byte)(i % 251);
        string expectedHash = Convert.ToHexString(SHA256.HashData(expected)).ToLowerInvariant();

        Assert.Equal($"{size}:{expectedHash}", json.GetProperty("uploadResult").GetString());
        Assert.True(json.GetProperty("slowMs").GetDouble() >= 4500);
        Assert.True(json.GetProperty("smallMs").GetDouble() < json.GetProperty("slowMs").GetDouble());
    }, TimeSpan.FromSeconds(180));

    [RealNativeFact]
    public Task N5_NeverResolvingDevToolsCallThenDisposeExitsCleanly() => TestUtil.RunScenario(async () =>
    {
        var harness = await StartAsync();
        var window = await harness.OpenPageAsync();

        var pending = window.ExecuteDevToolsMethodAsync("Runtime.evaluate", "{\"expression\":\"new Promise(() => {})\",\"awaitPromise\":true}");
        await Task.Delay(500);
        Assert.False(pending.IsCompleted);

        var stopwatch = Stopwatch.StartNew();
        harness.Process.Dispose();
        await harness.WaitForChildExitAsync().WithTimeout(Wait);
        stopwatch.Stop();

        await Assert.ThrowsAnyAsync<OperationCanceledException>(() => pending.WithTimeout(Wait));
        Assert.True(harness.Process.HasExited);
        Assert.True(stopwatch.Elapsed < TimeSpan.FromSeconds(5), $"The native process needed {stopwatch.Elapsed} to exit, so it was killed.");
        Assert.Equal(0, harness.Process.ChildProcess!.ExitCode);
    }, TimeSpan.FromSeconds(120));

    [RealNativeFact]
    public Task N6_AbortedStreamedFetchesDisposeEverySource() => TestUtil.RunScenario(async () =>
    {
        await using var harness = await StartAsync();
        var sources = new ConcurrentBag<PatternDataSource>();
        harness.Route("/stream", _ =>
        {
            var source = new PatternDataSource(long.MaxValue / 2, TimeSpan.FromMilliseconds(1));
            sources.Add(source);
            return Task.FromResult<IPCResponse?>(Http.Stream(source));
        });
        var window = await harness.OpenPageAsync();

        const int count = 500;
        string script = $$"""
            (async () => {
                let aborted = 0;
                const run = async (i) => {
                    const controller = new AbortController();
                    try {
                        const response = await fetch('/stream?' + i, { signal: controller.signal });
                        const reader = response.body.getReader();
                        await reader.read();
                        controller.abort();
                        await reader.read().catch(() => {});
                        aborted++;
                    } catch (e) {
                        if (e && e.name === 'AbortError') aborted++;
                    }
                };
                const batch = 25;
                for (let i = 0; i < {{count}}; i += batch)
                    await Promise.all([...Array(batch).keys()].map(j => run(i + j)));
                return aborted;
            })()
            """;
        var aborted = await EvaluateAsync(window, script).WithTimeout(TimeSpan.FromSeconds(120));
        Assert.Equal(count, aborted.GetInt32());

        await TestUtil.WaitUntil(() => sources.Count == count && sources.All(s => s.DisposeCount >= 1), TimeSpan.FromSeconds(30), "every stream source disposed");
        Assert.All(sources, s => Assert.Equal(1, s.DisposeCount));
        await TestUtil.WaitUntil(() => harness.Process.OpenStreamCount == 0, TimeSpan.FromSeconds(30), "every stream released");
        Assert.True((await window.GetSizeAsync()).Width > 0);
    }, TimeSpan.FromSeconds(240));

    [RealNativeFact]
    public Task N7_WindowClosedArrivesBeforeExitAndEof() => TestUtil.RunScenario(async () =>
    {
        await using var harness = await StartAsync();
        var windows = new List<JustCefWindow>();
        for (int i = 0; i < 3; i++)
            windows.Add(await harness.OpenPageAsync());

        await harness.Process.NotifyExitAsync();
        await harness.Process.WaitForExitAsync().WithTimeout(Wait);
        await harness.WaitForChildExitAsync().WithTimeout(Wait);

        Assert.All(windows, w =>
        {
            Assert.True(w.WaitForExitAsync().IsCompleted);
            Assert.True(w.ClosedByNative, $"Window {w.Identifier} was not closed by a WindowClosed notification before Exit.");
        });
    }, TimeSpan.FromSeconds(180));

    [RealNativeFact]
    public Task N8_ExitWithLongBridgeCallsPending() => TestUtil.RunScenario(async () =>
    {
        await using var harness = await StartAsync();
        int started = 0;
        var release = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var window = await harness.OpenPageAsync(async (_, _, _) =>
        {
            Interlocked.Increment(ref started);
            await release.Task;
            return null;
        });

        await EvaluateAsync(window, "window.__long = [0,1,2,3].map(() => bridge.rpc.call('long', null).catch(e => String(e))); true", awaitPromise: false);
        await TestUtil.WaitUntil(() => Volatile.Read(ref started) == 4, Wait, "four long bridge calls");

        var stopwatch = Stopwatch.StartNew();
        await harness.Process.NotifyExitAsync();
        await harness.Process.WaitForExitAsync().WithTimeout(Wait);
        await harness.WaitForChildExitAsync().WithTimeout(Wait);
        stopwatch.Stop();

        Assert.True(stopwatch.Elapsed < TimeSpan.FromSeconds(10), $"Exit took {stopwatch.Elapsed}.");
        Assert.True(window.WaitForExitAsync().IsCompleted);
        release.TrySetResult();
    }, TimeSpan.FromSeconds(180));
}
