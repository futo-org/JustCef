using System.Text;
using System.Text.Json.Nodes;
using JustCef.Tests.Infrastructure;
using Xunit;
using static JustCef.Tests.Infrastructure.FakeNativeHarness;

namespace JustCef.Tests.Fake;

[Trait("Category", "Fake")]
public class HandlerTests
{
    private static readonly TimeSpan Wait = TimeSpan.FromSeconds(15);

    [Fact]
    public Task BridgeRpcFromJavaScript() => TestUtil.RunScenario(async () =>
    {
        await using var harness = await StartAsync();
        var window = await harness.Process.CreateWindowAsync("", 0, 0, bridgeEnabled: true, bridgeRpcHandler: async (w, method, json) =>
        {
            await Task.Yield();
            return method switch
            {
                "add" => "3",
                "size" => $"{{\"width\":{(await w.GetSizeAsync()).Width}}}",
                "echo" => json,
                "nothing" => null,
                _ => throw new InvalidOperationException($"unknown method {method}")
            };
        });

        var add = await harness.CommandAsync(new JsonObject { ["cmd"] = "bridgeRpc", ["browserId"] = window.Identifier, ["method"] = "add", ["json"] = "[1,2]" }, "bridgeRpcResponse");
        Assert.Equal(0, (int)add["status"]!);
        Assert.Equal("3", (string?)add["json"]);

        var size = await harness.CommandAsync(new JsonObject { ["cmd"] = "bridgeRpc", ["browserId"] = window.Identifier, ["method"] = "size" }, "bridgeRpcResponse");
        Assert.Equal("{\"width\":800}", (string?)size["json"]);

        var nothing = await harness.CommandAsync(new JsonObject { ["cmd"] = "bridgeRpc", ["browserId"] = window.Identifier, ["method"] = "nothing" }, "bridgeRpcResponse");
        Assert.Equal("null", (string?)nothing["json"]);

        var big = await harness.CommandAsync(new JsonObject { ["cmd"] = "bridgeRpc", ["browserId"] = window.Identifier, ["method"] = "echo", ["size"] = 20 * 1024 * 1024 }, "bridgeRpcResponse", TimeSpan.FromSeconds(60));
        Assert.Equal(0, (int)big["status"]!);
        Assert.Equal(20 * 1024 * 1024, (int)big["jsonSize"]!);

        var unknown = await harness.CommandAsync(new JsonObject { ["cmd"] = "bridgeRpc", ["browserId"] = window.Identifier, ["method"] = "nope" }, "bridgeRpcResponse");
        Assert.Equal(1, (int)unknown["status"]!);
        Assert.Equal("unknown method nope", (string?)unknown["message"]);

        window.SetBridgeRpcHandler(null);
        var missing = await harness.CommandAsync(new JsonObject { ["cmd"] = "bridgeRpc", ["browserId"] = window.Identifier, ["method"] = "add" }, "bridgeRpcResponse");
        Assert.Equal(1, (int)missing["status"]!);
        Assert.Equal(0, await harness.ViolationsAsync());
    }, TimeSpan.FromSeconds(90));

    [Fact]
    public Task BridgeRpcNativeCancelAnswersCanceledAndDropsLateResult() => TestUtil.RunScenario(async () =>
    {
        await using var harness = await StartAsync();
        var started = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var release = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var finished = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var window = await harness.Process.CreateWindowAsync("", 0, 0, bridgeEnabled: true, bridgeRpcHandler: async (_, _, _) =>
        {
            started.TrySetResult();
            await release.Task;
            finished.TrySetResult();
            return "1";
        });

        var response = harness.WaitForEventAsync(e => Name(e) == "bridgeRpcResponse");
        await harness.SendAsync(new JsonObject { ["cmd"] = "bridgeRpc", ["browserId"] = window.Identifier, ["method"] = "slow" });
        await started.Task.WithTimeout(Wait);
        await harness.CommandAsync(new JsonObject { ["cmd"] = "cancelLastRequest", ["opcode"] = 9 }, "cancelSent");
        var result = await response;
        Assert.Equal(2, (int)result["status"]!);
        Assert.True((bool)result["cancelSent"]!);
        Assert.Equal(0, harness.Process.InflightHandlerCount);

        release.TrySetResult();
        await finished.Task.WithTimeout(Wait);
        await Task.Delay(200);
        Assert.Single(harness.EventsNamed("bridgeRpcResponse"));
        Assert.Equal(0, await harness.ViolationsAsync());
    }, TimeSpan.FromSeconds(60));

    [Fact]
    public Task ControllerBridgeAndDevToolsLargeResults() => TestUtil.RunScenario(async () =>
    {
        await using var harness = await StartAsync();
        var window = await harness.Process.CreateWindowAsync("", 0, 0, bridgeEnabled: true);

        Assert.Equal("null", await window.CallBridgeRpcAsync("anything", "[1]"));

        const int size = 20 * 1024 * 1024;
        await harness.ConfigureAsync("WindowExecuteDevToolsMethod", "ok", c => c["payloadSize"] = size);
        var result = await window.ExecuteDevToolsMethodAsync("Runtime.evaluate", "{\"expression\":\"1\"}").WithTimeout(TimeSpan.FromSeconds(60));
        Assert.True(result.Success);
        Assert.Equal(size, result.Data.Length);
        Assert.Equal((byte)'"', result.Data[0]);
        Assert.Equal((byte)'a', result.Data[1]);

        await harness.ConfigureAsync("WindowBridgeRpc", "ok", c => c["payloadSize"] = size);
        string json = await window.CallBridgeRpcAsync("big", new string('x', 1024)).WithTimeout(TimeSpan.FromSeconds(60));
        Assert.Equal(size, json.Length);

        await harness.ConfigureAsync("WindowBridgeRpc", "error", c => c["message"] = "js failed");
        var exception = await Assert.ThrowsAsync<JustCefRemoteException>(() => window.CallBridgeRpcAsync("boom"));
        Assert.Equal(JustCefStatus.Error, exception.Status);
        Assert.Equal("js failed", exception.Message);
        Assert.Equal(0, await harness.ViolationsAsync());
    }, TimeSpan.FromSeconds(90));

    [Fact]
    public Task ModifyRequestRoundTrip() => TestUtil.RunScenario(async () =>
    {
        await using var harness = await StartAsync();
        var window = await harness.Process.CreateWindowAsync("", 0, 0, modifyRequests: true, modifyRequestBody: true, requestModifier: (_, request) =>
        {
            Thread.Sleep(50);
            if (request.Url.EndsWith("skip"))
                return null;

            request.Url = request.Url + "?modified=1";
            request.Method = "POST";
            request.Headers["X-Added"] = new List<string> { "1" };
            request.Elements.Add(new IPCProxyBodyElementBytes(Encoding.UTF8.GetBytes("tail")));
            return request;
        });

        var response = await harness.CommandAsync(new JsonObject
        {
            ["cmd"] = "modifyRequest",
            ["browserId"] = window.Identifier,
            ["method"] = "GET",
            ["url"] = "https://app/page",
            ["headers"] = new JsonObject { ["A"] = "b" },
            ["bodySize"] = 3,
            ["files"] = new JsonArray("/tmp/file.bin")
        }, "modifyResponse");

        Assert.Equal(0, (int)response["status"]!);
        Assert.Equal("POST", (string?)response["method"]);
        Assert.Equal("https://app/page?modified=1", (string?)response["url"]);
        var headers = response["headers"]!.AsObject();
        Assert.Equal("b", (string?)headers["A"]![0]);
        Assert.Equal("1", (string?)headers["X-Added"]![0]);
        var elements = response["elements"]!.AsArray();
        Assert.Equal(3, elements.Count);
        Assert.Equal(1, (int)elements[0]!["type"]!);
        Assert.Equal(3, (int)elements[0]!["size"]!);
        Assert.Equal("/tmp/file.bin", (string?)elements[1]!["path"]);
        Assert.Equal(4, (int)elements[2]!["size"]!);

        var skipped = await harness.CommandAsync(new JsonObject { ["cmd"] = "modifyRequest", ["browserId"] = window.Identifier, ["url"] = "https://app/skip" }, "modifyResponse");
        Assert.Equal(4, (int)skipped["status"]!);

        var big = await harness.CommandAsync(new JsonObject { ["cmd"] = "modifyRequest", ["browserId"] = window.Identifier, ["url"] = "https://app/upload", ["bodySize"] = 20 * 1024 * 1024 }, "modifyResponse", TimeSpan.FromSeconds(60));
        Assert.Equal(0, (int)big["status"]!);
        Assert.Equal(20 * 1024 * 1024, (int)big["elements"]![0]!["size"]!);
        Assert.Equal(0, await harness.ViolationsAsync());
    }, TimeSpan.FromSeconds(90));

    [Fact]
    public Task ProxyRequestInlineAndNotHandled() => TestUtil.RunScenario(async () =>
    {
        await using var harness = await StartAsync();
        IPCRequest? seen = null;
        var window = await harness.Process.CreateWindowAsync("", 0, 0, proxyRequests: true, requestProxy: (_, request) =>
        {
            seen = request;
            return Task.FromResult(request.Url.EndsWith("none") ? null : Http.Text("hello"));
        });

        var response = await harness.CommandAsync(new JsonObject
        {
            ["cmd"] = "proxyRequest",
            ["browserId"] = window.Identifier,
            ["method"] = "POST",
            ["url"] = "https://app/api",
            ["headers"] = new JsonObject { ["Accept"] = new JsonArray("a", "b") },
            ["bodySize"] = 1000
        }, "proxyResponse");
        Assert.Equal(0, (int)response["status"]!);
        Assert.Equal(Http.Sha256Hex(Encoding.UTF8.GetBytes("hello")), (string?)response["sha256"]);
        Assert.Equal("text/plain", (string?)response["headers"]!["Content-Type"]![0]);
        Assert.NotNull(seen);
        Assert.Equal("POST", seen!.Method);
        Assert.Equal(new[] { "a", "b" }, seen.Headers["accept"]);
        var body = Assert.IsType<IPCProxyBodyElementBytes>(Assert.Single(seen.Elements));
        Assert.Equal(1000, body.Data.Length);
        Assert.Equal(250, body.Data[250]);

        var none = await harness.CommandAsync(new JsonObject { ["cmd"] = "proxyRequest", ["browserId"] = window.Identifier, ["url"] = "https://app/none" }, "proxyResponse");
        Assert.Equal(4, (int)none["status"]!);

        window.SetRequestProxy(null);
        var unset = await harness.CommandAsync(new JsonObject { ["cmd"] = "proxyRequest", ["browserId"] = window.Identifier }, "proxyResponse");
        Assert.Equal(4, (int)unset["status"]!);
        await Assert.ThrowsAsync<ArgumentException>(() => window.SetProxyRequestsAsync(true));
        Assert.Equal(0, await harness.ViolationsAsync());
    }, TimeSpan.FromSeconds(60));

    [Fact]
    public Task ViewCreatedAllowDenyThrow() => TestUtil.RunScenario(async () =>
    {
        await using var harness = await StartAsync();
        var proxiedByView = new TaskCompletionSource<int>(TaskCreationOptions.RunContinuationsAsynchronously);
        var window = await harness.Process.CreateWindowAsync("", 0, 0, viewsEnabled: true, viewCreatedHandler: async view =>
        {
            await Task.Delay(10);
            switch (view.Identifier)
            {
                case 10:
                    view.SetRequestProxy((v, _) =>
                    {
                        proxiedByView.TrySetResult(v.Identifier);
                        return Task.FromResult<IPCResponse?>(Http.Text("view"));
                    });
                    break;
                case 11:
                    throw new InvalidOperationException("view denied");
                default:
                    throw new InvalidOperationException("view boom");
            }
        });

        var allowed = await harness.CommandAsync(new JsonObject { ["cmd"] = "viewCreated", ["parentId"] = window.Identifier, ["viewId"] = 10, ["src"] = "https://app/view" }, "viewCreatedResponse");
        Assert.Equal(0, (int)allowed["status"]!);
        Assert.True((bool)allowed["allow"]!);

        var view = Assert.IsType<JustCefView>(harness.Process.GetBrowser(10));
        Assert.Same(window, view.Parent);
        Assert.Contains(view, window.Views);
        Assert.DoesNotContain(harness.Process.Windows, w => w.Identifier == 10);

        var duplicate = await harness.CommandAsync(new JsonObject { ["cmd"] = "viewCreated", ["parentId"] = window.Identifier, ["viewId"] = 10 }, "viewCreatedResponse");
        Assert.False((bool)duplicate["allow"]!);
        Assert.Same(view, harness.Process.GetBrowser(10));
        Assert.Single(window.Views);

        var proxied = await harness.CommandAsync(new JsonObject { ["cmd"] = "proxyRequest", ["browserId"] = 10 }, "proxyResponse");
        Assert.Equal(0, (int)proxied["status"]!);
        Assert.Equal(10, await proxiedByView.Task.WithTimeout(Wait));

        var denied = await harness.CommandAsync(new JsonObject { ["cmd"] = "viewCreated", ["parentId"] = window.Identifier, ["viewId"] = 11 }, "viewCreatedResponse");
        Assert.Equal(0, (int)denied["status"]!);
        Assert.False((bool)denied["allow"]!);

        var thrown = await harness.CommandAsync(new JsonObject { ["cmd"] = "viewCreated", ["parentId"] = window.Identifier, ["viewId"] = 12 }, "viewCreatedResponse");
        Assert.Equal(0, (int)thrown["status"]!);
        Assert.False((bool)thrown["allow"]!);
        Assert.Null(harness.Process.GetBrowser(11));
        Assert.Null(harness.Process.GetBrowser(12));

        var orphan = await harness.CommandAsync(new JsonObject { ["cmd"] = "viewCreated", ["parentId"] = 777, ["viewId"] = 13 }, "viewCreatedResponse");
        Assert.Equal(3, (int)orphan["status"]!);

        var viewClosed = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        view.OnClose += () => viewClosed.TrySetResult();
        await window.CloseAsync();
        await viewClosed.Task.WithTimeout(Wait);
        await window.WaitForExitAsync().WithTimeout(Wait);
        Assert.Empty(window.Views);
        Assert.Equal(0, await harness.ViolationsAsync());
    }, TimeSpan.FromSeconds(60));

    [Fact]
    public Task EventsArriveInOrderWithPayloads() => TestUtil.RunScenario(async () =>
    {
        await using var harness = await StartAsync();
        var window = await harness.Process.CreateWindowAsync("", 0, 0);
        var log = new System.Collections.Concurrent.ConcurrentQueue<string>();
        window.OnFocused += () => log.Enqueue("focused");
        window.OnUnfocused += () => log.Enqueue("unfocused");
        window.OnFullscreenChanged += fullscreen => log.Enqueue($"fullscreen:{fullscreen}");
        window.OnFrameLoadStart += info => log.Enqueue($"start:{info.Url}:{info.IsMainFrame}");
        window.OnFrameLoadEnd += info => log.Enqueue($"end:{info.HttpStatusCode}");
        window.OnFrameLoadError += info => log.Enqueue($"error:{info.ErrorCode}:{info.ErrorText}");
        window.OnDevToolsEvent += (method, data) => log.Enqueue($"devtools:{method}:{data.Length}");
        window.OnLoadingStateChanged += info => log.Enqueue($"loading:{info.IsLoading}:{info.CanGoBack}");
        window.OnFocused += () => throw new InvalidOperationException("handler failure must not stop the dispatcher");

        await harness.NotifyAsync(5, window.Identifier);
        await harness.NotifyAsync(12, window.Identifier, c => c["fullscreen"] = true);
        await harness.NotifyAsync(13, window.Identifier, c => c["url"] = "https://a/");
        await harness.NotifyAsync(17, window.Identifier, c => { c["isLoading"] = true; c["canGoBack"] = true; });
        await harness.NotifyAsync(15, window.Identifier, c => c["errorCode"] = -105);
        await harness.NotifyAsync(14, window.Identifier, c => c["httpStatusCode"] = 404);
        await harness.NotifyAsync(16, window.Identifier, c => c["size"] = 5 * 1024 * 1024);
        await harness.NotifyAsync(6, window.Identifier);

        await TestUtil.WaitUntil(() => log.Count == 8, Wait, "all events");
        Assert.Equal(new[]
        {
            "focused",
            "fullscreen:True",
            "start:https://a/:True",
            "loading:True:True",
            "error:-105:net::ERR_FAILED",
            "end:404",
            $"devtools:Test.event:{5 * 1024 * 1024}",
            "unfocused"
        }, log.ToArray());
        Assert.False(window.WaitUntilLoadedAsync().IsCompleted);
    }, TimeSpan.FromSeconds(60));

    [Fact]
    public Task PickersHaveNoDefaultTimeout() => TestUtil.RunScenario(async () =>
    {
        await using var harness = await StartAsync(process => process.DefaultCallTimeout = TimeSpan.FromMilliseconds(300));
        var window = await harness.Process.CreateWindowAsync("", 0, 0);
        await harness.ConfigureAsync("PickDirectory", "ok", c => c["delayMs"] = 1000);
        await harness.ConfigureAsync("PickFile", "ok", c => c["delayMs"] = 1000);
        Assert.Equal("", await window.PickDirectoryAsync().WithTimeout(Wait));
        Assert.Empty(await window.PickFileAsync(true, new[] { ("All", "*") }).WithTimeout(Wait));

        using var cts = new CancellationTokenSource(200);
        await harness.ConfigureAsync("SaveFile", "hold");
        await Assert.ThrowsAnyAsync<OperationCanceledException>(() => window.SaveFileAsync("a.txt", Array.Empty<(string, string)>(), cts.Token).WithTimeout(Wait));
        await harness.WaitForEventAsync(e => Name(e) == "cancelReceived" && (int)e["opcode"]! == 41);
    }, TimeSpan.FromSeconds(60));
}
