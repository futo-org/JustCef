using System.Text.Json.Nodes;
using JustCef.Tests.Infrastructure;
using Xunit;
using static JustCef.Tests.Infrastructure.FakeNativeHarness;

namespace JustCef.Tests.Fake;

[Trait("Category", "Fake")]
public class StreamTests
{
    private static readonly TimeSpan Wait = TimeSpan.FromSeconds(30);

    private static async Task<JustCefWindow> CreateProxyWindow(FakeNativeHarness harness, Func<IPCRequest, IPCResponse?> respond)
    {
        return await harness.Process.CreateWindowAsync("", 0, 0, proxyRequests: true, requestProxy: (_, request) => Task.FromResult(respond(request)));
    }

    [Fact]
    public Task FiftyMegabyteStreamArrivesIntact() => TestUtil.RunScenario(async () =>
    {
        await using var harness = await StartAsync();
        const long size = 50L * 1024 * 1024;
        var source = new PatternDataSource(size);
        var window = await CreateProxyWindow(harness, _ => Http.Stream(source, size));

        var response = await harness.CommandAsync(new JsonObject { ["cmd"] = "proxyRequest", ["browserId"] = window.Identifier, ["url"] = "https://app/big" }, "proxyResponse");
        Assert.Equal(0, (int)response["status"]!);
        Assert.Equal(2, (int)response["bodyType"]!);
        Assert.Equal(size, (long)response["length"]!);
        uint streamId = (uint)response["streamId"]!;
        Assert.NotEqual(0u, streamId);

        var done = await harness.WaitForEventAsync(e => Name(e) == "streamDone" && (uint)e["streamId"]! == streamId, Wait);
        Assert.Equal("end", (string?)done["result"]);
        Assert.Equal(size, (long)done["bytes"]!);
        Assert.Equal(size, (long)done["totalReported"]!);
        Assert.Equal(PatternDataSource.Sha256Hex(size), (string?)done["sha256"]);
        Assert.Empty(harness.EventsNamed("creditViolation"));

        await source.Disposed.Task.WithTimeout(Wait);
        await Task.Delay(100);
        Assert.Equal(1, source.DisposeCount);
        await TestUtil.WaitUntil(() => harness.Process.OpenStreamCount == 0, Wait, "the stream was released");
        Assert.Equal(0, await harness.ViolationsAsync());
    }, TimeSpan.FromSeconds(120));

    [Fact]
    public Task UnknownLengthStreamAndStreamErrors() => TestUtil.RunScenario(async () =>
    {
        await using var harness = await StartAsync();
        var failing = new FailingDataSource(3 * 1024 * 1024);
        var pattern = new PatternDataSource(777_777);
        var window = await CreateProxyWindow(harness, request => request.Url.EndsWith("fail") ? Http.Stream(failing) : Http.Stream(pattern));

        var ok = await harness.CommandAsync(new JsonObject { ["cmd"] = "proxyRequest", ["browserId"] = window.Identifier, ["url"] = "https://app/ok" }, "proxyResponse");
        Assert.Equal(-1, (long)ok["length"]!);
        var okDone = await harness.WaitForEventAsync(e => Name(e) == "streamDone" && (uint)e["streamId"]! == (uint)ok["streamId"]!, Wait);
        Assert.Equal("end", (string?)okDone["result"]);
        Assert.Equal(PatternDataSource.Sha256Hex(777_777), (string?)okDone["sha256"]);

        var bad = await harness.CommandAsync(new JsonObject { ["cmd"] = "proxyRequest", ["browserId"] = window.Identifier, ["url"] = "https://app/fail" }, "proxyResponse");
        var badDone = await harness.WaitForEventAsync(e => Name(e) == "streamDone" && (uint)e["streamId"]! == (uint)bad["streamId"]!, Wait);
        Assert.Equal("error", (string?)badDone["result"]);
        Assert.Equal("source failed", (string?)badDone["message"]);
        await TestUtil.WaitUntil(() => Volatile.Read(ref failing.DisposeCount) == 1 && pattern.DisposeCount == 1, Wait, "both sources disposed");
        Assert.Equal(0, await harness.ViolationsAsync());
    }, TimeSpan.FromSeconds(60));

    [Fact]
    public Task SlowConsumerKeepsMemoryBounded() => TestUtil.RunScenario(async () =>
    {
        await using var harness = await StartAsync();
        const long size = 16L * 1024 * 1024;
        var source = new PatternDataSource(size);
        var window = await CreateProxyWindow(harness, _ => Http.Stream(source, size));

        var response = await harness.CommandAsync(new JsonObject { ["cmd"] = "proxyRequest", ["browserId"] = window.Identifier, ["readDelayMs"] = 5 }, "proxyResponse");
        uint streamId = (uint)response["streamId"]!;

        long maxAhead = 0;
        int samples = 0;
        var done = harness.WaitForEventAsync(e => Name(e) == "streamDone" && (uint)e["streamId"]! == streamId, TimeSpan.FromSeconds(60));
        while (!done.IsCompleted)
        {
            long read = source.Position;
            var state = await harness.StateAsync();
            var stream = state["streams"]!.AsArray().FirstOrDefault(s => (uint)s!["streamId"]! == streamId);
            if (stream != null)
            {
                long granted = (long)stream["granted"]!;
                maxAhead = Math.Max(maxAhead, read - granted);
                ++samples;
            }
            await Task.Delay(20);
        }

        var result = await done;
        Assert.Equal("end", (string?)result["result"]);
        Assert.Equal(PatternDataSource.Sha256Hex(size), (string?)result["sha256"]);
        Assert.Empty(harness.EventsNamed("creditViolation"));
        Assert.True(samples > 0, "The stream state was never sampled while the transfer was running.");
        Assert.True(maxAhead > 0, "The pump never ran ahead of the consumer, so the bound proves nothing.");
        Assert.True(maxAhead <= 1024 * 1024 + 256 * 1024, $"The pump ran {maxAhead} bytes ahead of the granted credit.");
        Assert.Equal(0, await harness.ViolationsAsync());
    }, TimeSpan.FromSeconds(120));

    [Fact]
    public Task CanceledStreamDisposesSourceOnceAndStops() => TestUtil.RunScenario(async () =>
    {
        await using var harness = await StartAsync();
        var source = new PatternDataSource(long.MaxValue / 2);
        var window = await CreateProxyWindow(harness, _ => Http.Stream(source));

        var response = await harness.CommandAsync(new JsonObject { ["cmd"] = "proxyRequest", ["browserId"] = window.Identifier, ["cancelAfterBytes"] = 600000 }, "proxyResponse");
        uint streamId = (uint)response["streamId"]!;
        var done = await harness.WaitForEventAsync(e => Name(e) == "streamDone" && (uint)e["streamId"]! == streamId, Wait);
        Assert.Equal("canceled", (string?)done["result"]);

        await source.Disposed.Task.WithTimeout(Wait);
        long position = source.Position;
        await Task.Delay(300);
        Assert.Equal(position, source.Position);
        Assert.Equal(1, source.DisposeCount);
        Assert.True(position <= 600000 + 1024 * 1024 + 256 * 1024, $"Read {position} bytes after a cancel at 600000.");
        await TestUtil.WaitUntil(() => harness.Process.OpenStreamCount == 0, Wait, "the stream was released");
        Assert.Equal(0, await harness.ViolationsAsync());
    }, TimeSpan.FromSeconds(60));

    [Fact]
    public Task ManyCanceledStreamsDisposeEverySource() => TestUtil.RunScenario(async () =>
    {
        await using var harness = await StartAsync();
        var sources = new System.Collections.Concurrent.ConcurrentBag<PatternDataSource>();
        var window = await CreateProxyWindow(harness, _ =>
        {
            var source = new PatternDataSource(8L * 1024 * 1024);
            sources.Add(source);
            return Http.Stream(source);
        });

        const int count = 100;
        var responses = Enumerable.Range(0, count)
            .Select(i => harness.CommandAsync(new JsonObject { ["cmd"] = "proxyRequest", ["browserId"] = window.Identifier, ["cancelAfterBytes"] = i % 3 == 0 ? 0 : 100000 * i }, "proxyResponse"))
            .ToList();
        await Task.WhenAll(responses).WithTimeout(Wait);

        await TestUtil.WaitUntil(() => sources.Count == count && sources.All(s => s.DisposeCount == 1), Wait, "every source disposed");
        await TestUtil.WaitUntil(() => harness.Process.OpenStreamCount == 0, Wait, "all streams released");
        Assert.All(sources, s => Assert.Equal(1, s.DisposeCount));
        Assert.Equal(0, await harness.ViolationsAsync());
    }, TimeSpan.FromSeconds(60));

    [Fact]
    public Task DisposeReleasesOpenStreams() => TestUtil.RunScenario(async () =>
    {
        var harness = await StartAsync();
        var source = new PatternDataSource(long.MaxValue / 2);
        var window = await CreateProxyWindow(harness, _ => Http.Stream(source));
        await harness.CommandAsync(new JsonObject { ["cmd"] = "proxyRequest", ["browserId"] = window.Identifier, ["readDelayMs"] = 50 }, "proxyResponse");
        await TestUtil.WaitUntil(() => source.Position > 0, Wait, "the stream started");

        harness.Process.Dispose();
        await source.Disposed.Task.WithTimeout(Wait);
        await Task.Delay(200);
        Assert.Equal(1, source.DisposeCount);
        await harness.DisposeAsync();
    }, TimeSpan.FromSeconds(60));
}
