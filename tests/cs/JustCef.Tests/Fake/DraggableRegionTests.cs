using JustCef.Tests.Infrastructure;
using Xunit;

namespace JustCef.Tests.Fake;

[Trait("Category", "Fake")]
public class DraggableRegionTests
{
    [Fact]
    public Task OverridesPreserveExclusionsAndCanRestoreCss() => TestUtil.RunScenario(async () =>
    {
        await using var harness = await FakeNativeHarness.StartAsync();
        var window = await harness.Process.CreateWindowAsync("https://app/", 320, 240, frameless: true);
        await window.SetDraggableRegionsAsync(new[]
        {
            new DraggableRegion(0, 0, 800, 40),
            new DraggableRegion(700, 0, 100, 40, false)
        });
        var regions = (await harness.StateAsync())["windows"]![0]!["draggableRegions"]!;
        Assert.Equal("[[0,0,800,40,true],[700,0,100,40,false]]", regions.ToJsonString());

        await window.NavigateAsync("https://app/next");
        Assert.Equal(regions.ToJsonString(), (await harness.StateAsync())["windows"]![0]!["draggableRegions"]!.ToJsonString());
        await window.SetDraggableRegionsAsync(Array.Empty<DraggableRegion>());
        Assert.Empty((await harness.StateAsync())["windows"]![0]!["draggableRegions"]!.AsArray());
        await window.SetDraggableRegionsAsync();
        Assert.Null((await harness.StateAsync())["windows"]![0]!["draggableRegions"]);
        Assert.Equal(0, await harness.ViolationsAsync());
    }, TimeSpan.FromSeconds(60));

    [Fact]
    public Task InvalidBoundsAndExcessiveCountsNeverReachNative() => TestUtil.RunScenario(async () =>
    {
        await using var harness = await FakeNativeHarness.StartAsync();
        var window = await harness.Process.CreateWindowAsync("https://app/", 0, 0, frameless: true);
        foreach (var region in new[]
        {
            new DraggableRegion(0, 0, 0, 40),
            new DraggableRegion(0, 0, 40, -1),
            new DraggableRegion(int.MaxValue, 0, 1, 40),
            new DraggableRegion(0, int.MaxValue, 40, 1)
        })
            await Assert.ThrowsAsync<ArgumentOutOfRangeException>(() => window.SetDraggableRegionsAsync(new[] { region }));
        await Assert.ThrowsAsync<ArgumentOutOfRangeException>(() => window.SetDraggableRegionsAsync(new DraggableRegion[4097]));
        await window.SetDraggableRegionsAsync(new[] { new DraggableRegion(-10, -10, 100, 40) });
        Assert.Equal(0, await harness.ViolationsAsync());
    }, TimeSpan.FromSeconds(60));

    [Fact]
    public Task FramedWindowsReportUnsupported() => TestUtil.RunScenario(async () =>
    {
        await using var harness = await FakeNativeHarness.StartAsync();
        var window = await harness.Process.CreateWindowAsync("https://app/", 0, 0);
        var error = await Assert.ThrowsAsync<JustCefRemoteException>(() => window.SetDraggableRegionsAsync());
        Assert.Equal(JustCefStatus.Unsupported, error.Status);
        Assert.Equal(0, await harness.ViolationsAsync());
    }, TimeSpan.FromSeconds(60));
}
