using System.Text.Json.Nodes;
using JustCef.Tests.Infrastructure;
using Xunit;

namespace JustCef.Tests.Fake;

[Trait("Category", "Fake")]
public class BrowserFeatureTests
{
    [Fact]
    public Task DecisionCanCallNativeWithoutDeadlocking() => TestUtil.RunScenario(async () =>
    {
        await using var harness = await FakeNativeHarness.StartAsync();
        var window = await harness.Process.CreateWindowAsync("https://app/", 320, 240);
        window.SetBrowserRequestHandler(async (request, token) => {
            Assert.Equal("permission", request.Kind);
            Assert.Equal("https://camera.test", request.Detail.GetProperty("origin").GetString());
            await window.GetSizeAsync(token);
            return new BrowserDecision(Allow: true, Remember: true);
        });
        var response = await harness.CommandAsync(new JsonObject { ["cmd"] = "browserDecision", ["browserId"] = window.Identifier,
            ["kind"] = "permission", ["json"] = "{\"origin\":\"https://camera.test\",\"permissions\":3}" }, "browserDecisionResponse");
        Assert.Equal(0, (int)response["status"]!);
        Assert.True((bool)response["answer"]!["allow"]!);
        Assert.True((bool)response["answer"]!["remember"]!);
        Assert.Equal(0, await harness.ViolationsAsync());
    }, TimeSpan.FromSeconds(60));

    [Fact]
    public Task RemoteCancellationCancelsDecisionToken() => TestUtil.RunScenario(async () =>
    {
        await using var harness = await FakeNativeHarness.StartAsync();
        var window = await harness.Process.CreateWindowAsync("https://app/", 320, 240);
        var started = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var canceled = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        window.SetBrowserRequestHandler(async (_, token) => {
            started.TrySetResult();
            try { await Task.Delay(Timeout.Infinite, token); }
            catch (OperationCanceledException) { canceled.TrySetResult(); throw; }
            return null;
        });
        var response = harness.WaitForEventAsync(e => FakeNativeHarness.Name(e) == "browserDecisionResponse");
        await harness.SendAsync(new JsonObject { ["cmd"] = "browserDecision", ["browserId"] = window.Identifier });
        await started.Task.WithTimeout(FakeNativeHarness.DefaultWait);
        await harness.CommandAsync(new JsonObject { ["cmd"] = "cancelLastRequest", ["opcode"] = 12 }, "cancelSent");
        await canceled.Task.WithTimeout(FakeNativeHarness.DefaultWait);
        Assert.Equal(2, (int)(await response)["status"]!);
    }, TimeSpan.FromSeconds(60));

    [Fact]
    public Task DisconnectCancelsDecisionEvenWhenCloseSubscriberThrows() => TestUtil.RunScenario(async () =>
    {
        await using var harness = await FakeNativeHarness.StartAsync();
        var window = await harness.Process.CreateWindowAsync("https://app/", 320, 240);
        window.OnClose += () => throw new InvalidOperationException("subscriber failure");
        var started = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var canceled = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        window.SetBrowserRequestHandler(async (_, token) => {
            started.TrySetResult();
            try { await Task.Delay(Timeout.Infinite, token); }
            catch (OperationCanceledException) { canceled.TrySetResult(); throw; }
            return null;
        });
        await harness.SendAsync(new JsonObject { ["cmd"] = "browserDecision", ["browserId"] = window.Identifier });
        await started.Task.WithTimeout(FakeNativeHarness.DefaultWait);
        await harness.SendAsync(new JsonObject { ["cmd"] = "exit" });
        await canceled.Task.WithTimeout(FakeNativeHarness.DefaultWait);
    }, TimeSpan.FromSeconds(60));

    [Fact]
    public Task PrintAndWindowPolicyUseTheSelectedBrowser() => TestUtil.RunScenario(async () =>
    {
        await using var harness = await FakeNativeHarness.StartAsync();
        var window = await harness.Process.CreateWindowAsync("https://app/", 320, 240);
        await window.PrintAsync();
        await window.SetNewWindowsAllowedAsync(true, interceptRequests: true);
        await harness.WaitForEventAsync(e => FakeNativeHarness.Name(e) == "browserCommand" && (string?)e["name"] == "windowOpenPolicy");
        var requests = harness.EventsNamed("browserCommand");
        Assert.Contains(requests, r => (string?)r["name"] == "print" && (int)r["browserId"]! == window.Identifier);
        Assert.Contains(requests, r => (string?)r["name"] == "windowOpenPolicy" && (bool)r["options"]!["intercept"]!);
    }, TimeSpan.FromSeconds(60));

    [Fact]
    public Task DownloadUpdatesKeepWireOrder() => TestUtil.RunScenario(async () =>
    {
        await using var harness = await FakeNativeHarness.StartAsync();
        var window = await harness.Process.CreateWindowAsync("https://app/", 320, 240);
        var received = new System.Collections.Concurrent.ConcurrentQueue<int>();
        window.OnDownloadUpdated += status => received.Enqueue(status.GetProperty("receivedBytes").GetInt32());
        for (int i = 0; i < 200; ++i)
            await harness.SendAsync(new JsonObject { ["cmd"] = "downloadUpdate", ["browserId"] = window.Identifier,
                ["json"] = $"{{\"id\":1,\"receivedBytes\":{i},\"complete\":{(i == 199 ? "true" : "false")}}}" });
        await TestUtil.WaitUntil(() => received.Count == 200, FakeNativeHarness.DefaultWait, "all download updates delivered");
        Assert.Equal(Enumerable.Range(0, 200), received);
        Assert.Equal(0, await harness.ViolationsAsync());
    }, TimeSpan.FromSeconds(60));

    [Fact]
    public Task CapturePermissionAndDownloadBindingsPreserveOptions() => TestUtil.RunScenario(async () =>
    {
        await using var harness = await FakeNativeHarness.StartAsync();
        var window = await harness.Process.CreateWindowAsync("https://app/", 320, 240);
        Assert.Equal(new byte[] { 1, 2, 3, 4 }, await window.CaptureScreenshotAsync(new(FullPage: true, Format: "webp", Quality: 37)));
        Assert.Equal("%PDF-", System.Text.Encoding.ASCII.GetString(await window.PrintToPdfAsync(new(Landscape: true, PageRanges: "2-3", Scale: 1.5, PreferCssPageSize: true))));
        await window.SetMediaPermissionsAsync("https://request.test", MediaPermission.Camera, PermissionSetting.Block, topLevelOrigin: "https://top.test");
        Assert.Equal(new MediaPermissionSettings(PermissionSetting.Allow, PermissionSetting.Block), await window.GetMediaPermissionsAsync("https://request.test"));
        await window.StartDownloadAsync("https://files.test/spaced%20file.bin");
        foreach (var action in new[] { "pause", "resume", "cancel" })
            await window.ControlDownloadAsync(42, action);
        await harness.WaitForEventAsync(e => FakeNativeHarness.Name(e) == "browserCommand" && (string?)e["name"] == "downloadControl" && (string?)e["options"]!["action"] == "cancel");
        var commands = harness.EventsNamed("browserCommand");
        var capture = Assert.Single(commands, e => (string?)e["name"] == "captureScreenshot");
        Assert.Equal(window.Identifier, (int)capture["browserId"]!);
        Assert.True((bool)capture["options"]!["fullPage"]!);
        Assert.Equal("webp", (string?)capture["options"]!["format"]);
        Assert.Equal(37, (int)capture["options"]!["quality"]!);
        var setting = Assert.Single(commands, e => (string?)e["name"] == "permission" && e["options"]!["value"] != null);
        Assert.Equal(2, (int)setting["options"]!["permissions"]!);
        Assert.Equal(2, (int)setting["options"]!["value"]!);
        Assert.Equal("https://top.test", (string?)setting["options"]!["topLevelOrigin"]);
        Assert.Contains(commands, e => (string?)e["name"] == "download" && (string?)e["options"]!["url"] == "https://files.test/spaced%20file.bin");
        Assert.Equal(new[] { "pause", "resume", "cancel" }, commands.Where(e => (string?)e["name"] == "downloadControl").Select(e => (string?)e["options"]!["action"]));
        Assert.All(commands.Where(e => (string?)e["name"] == "downloadControl"), e => Assert.Equal(42, (int)e["options"]!["id"]!));
        var pdf = Assert.Single(harness.EventsNamed("pdfCommand"));
        Assert.True((bool)pdf["options"]!["landscape"]!);
        Assert.True((bool)pdf["options"]!["preferCSSPageSize"]!);
        Assert.Equal("2-3", (string?)pdf["options"]!["pageRanges"]);
        Assert.Equal(1.5, (double)pdf["options"]!["scale"]!);
        await Assert.ThrowsAsync<ArgumentException>(() => window.CaptureScreenshotAsync(new(Format: "gif")));
        await Assert.ThrowsAsync<ArgumentOutOfRangeException>(() => window.CaptureScreenshotAsync(new(Quality: 101)));
        await Assert.ThrowsAsync<ArgumentException>(() => window.ControlDownloadAsync(42, "invalid"));
        Assert.Equal(0, await harness.ViolationsAsync());
    }, TimeSpan.FromSeconds(60));

}
