using JustCef.Tests.Infrastructure;
using System.Text.Json.Nodes;
using Xunit;

namespace JustCef.Tests.Fake;

[Trait("Category", "Fake")]
public class RuntimeInfoTests
{
    [Fact]
    public Task WaitingForInitializationCanBeCanceled() => TestUtil.RunScenario(async () =>
    {
        await using var harness = await FakeNativeHarness.StartAsync(extraArguments: "--fake-no-ready", waitForReady: false);
        using var cancellation = new CancellationTokenSource();
        var query = harness.Process.GetRuntimeInfoAsync(cancellation.Token);
        Assert.False(query.IsCompleted);
        cancellation.Cancel();
        await Assert.ThrowsAnyAsync<OperationCanceledException>(() => query);
        Assert.Equal(0, await harness.ViolationsAsync());
    }, TimeSpan.FromSeconds(60));

    [Fact]
    public Task ReportsNativeOsAndBackendWithoutCreatingAWindow() => TestUtil.RunScenario(async () =>
    {
        await using var harness = await FakeNativeHarness.StartAsync();
        foreach (var (os, backend) in new[] { (0, 0), (1, 0), (2, 0), (3, 0), (3, 1), (3, 2) })
        {
            await harness.ConfigureAsync("GetRuntimeInfo", "ok", command => command["runtimeInfo"] = new JsonArray(os, backend));
            var info = await harness.Process.GetRuntimeInfoAsync();
            Assert.Equal((RuntimeOperatingSystem)os, info.OperatingSystem);
            Assert.Equal(os == 3 ? (LinuxWindowingBackend?)backend : null, info.LinuxWindowingBackend);
        }
        Assert.Empty(harness.Process.Windows);
        Assert.Equal(0, await harness.ViolationsAsync());
    }, TimeSpan.FromSeconds(60));

    [Fact]
    public Task RejectsMalformedResponses() => TestUtil.RunScenario(async () =>
    {
        await using var harness = await FakeNativeHarness.StartAsync();
        foreach (var payload in new[] { new byte[] { }, new byte[] { 3 }, new byte[] { 3, 2, 0 },
            new byte[] { 4, 0 }, new byte[] { 3, 3 }, new byte[] { 1, 2 } })
        {
            await harness.ConfigureAsync("GetRuntimeInfo", "ok", command =>
                command["runtimeInfo"] = new JsonArray(payload.Select(value => (JsonNode?)JsonValue.Create(value)).ToArray()));
            await Assert.ThrowsAsync<InvalidOperationException>(() => harness.Process.GetRuntimeInfoAsync());
        }
        await harness.ConfigureAsync("GetRuntimeInfo", "status", command => command["status"] = (byte)JustCefStatus.Unsupported);
        var error = await Assert.ThrowsAsync<JustCefRemoteException>(() => harness.Process.GetRuntimeInfoAsync());
        Assert.Equal(JustCefStatus.Unsupported, error.Status);
        Assert.Equal(0, await harness.ViolationsAsync());
    }, TimeSpan.FromSeconds(60));
}
