using JustCef.Tests.Infrastructure;
using Xunit;

namespace JustCef.Tests.Fake;

[Trait("Category", "Fake")]
public class CaptionButtonRegionTests
{
    [Fact]
    public Task RolesSerializeAndNavigationClearsTargets() => TestUtil.RunScenario(async () =>
    {
        await using var harness = await FakeNativeHarness.StartAsync();
        var window = await harness.Process.CreateWindowAsync("https://app/", 320, 240, frameless: true);
        var regions = new[] {
            new CaptionButtonRegion(660, 4, 46, 36, CaptionButton.Minimize),
            new CaptionButtonRegion(706, 4, 46, 36, CaptionButton.Maximize),
            new CaptionButtonRegion(752, 4, 46, 36, CaptionButton.Close)
        };
        await window.SetCaptionButtonRegionsAsync(regions);
        Assert.Equal("[[660,4,46,36,1],[706,4,46,36,2],[752,4,46,36,3]]",
            (await harness.StateAsync())["windows"]![0]!["captionButtonRegions"]!.ToJsonString());
        await window.NavigateAsync("https://app/next");
        Assert.Empty((await harness.StateAsync())["windows"]![0]!["captionButtonRegions"]!.AsArray());
        await window.SetCaptionButtonRegionsAsync(regions);
        await window.SetCaptionButtonRegionsAsync();
        Assert.Empty((await harness.StateAsync())["windows"]![0]!["captionButtonRegions"]!.AsArray());
        Assert.Equal(0, await harness.ViolationsAsync());
    }, TimeSpan.FromSeconds(60));

    [Fact]
    public Task RejectsInvalidAmbiguousTargetsBeforeSending() => TestUtil.RunScenario(async () =>
    {
        await using var harness = await FakeNativeHarness.StartAsync();
        var window = await harness.Process.CreateWindowAsync("https://app/", 0, 0, frameless: true);
        var valid = new CaptionButtonRegion(10, 10, 20, 20, CaptionButton.Maximize);
        foreach (var invalid in new[] {
            valid with { Width = 0 }, valid with { Height = -1 },
            valid with { X = int.MaxValue }, valid with { Y = int.MaxValue },
            valid with { Button = (CaptionButton)0 }, valid with { Button = (CaptionButton)255 }
        })
            await Assert.ThrowsAsync<ArgumentOutOfRangeException>(() => window.SetCaptionButtonRegionsAsync(new[] { invalid }));
        await Assert.ThrowsAsync<ArgumentOutOfRangeException>(() => window.SetCaptionButtonRegionsAsync(new[] { valid, valid with { X = 100 } }));
        await Assert.ThrowsAsync<ArgumentOutOfRangeException>(() => window.SetCaptionButtonRegionsAsync(new[] { valid, valid with { Button = CaptionButton.Close } }));
        await Assert.ThrowsAsync<ArgumentOutOfRangeException>(() => window.SetCaptionButtonRegionsAsync(new CaptionButtonRegion[4]));
        await window.SetCaptionButtonRegionsAsync(new[] { valid });
        Assert.Equal(0, await harness.ViolationsAsync());
    }, TimeSpan.FromSeconds(60));

    [Fact]
    public Task UnsupportedIsReported() => TestUtil.RunScenario(async () =>
    {
        await using var harness = await FakeNativeHarness.StartAsync();
        var window = await harness.Process.CreateWindowAsync("https://app/", 0, 0);
        var error = await Assert.ThrowsAsync<JustCefRemoteException>(() => window.SetCaptionButtonRegionsAsync());
        Assert.Equal(JustCefStatus.Unsupported, error.Status);
        Assert.Equal(0, await harness.ViolationsAsync());
    }, TimeSpan.FromSeconds(60));
}
