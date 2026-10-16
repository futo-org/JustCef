using JustCef.Tests.Infrastructure;
using Xunit;

namespace JustCef.Tests.Fake;

[Trait("Category", "Fake")]
public class NativeFileDragTests
{
    [Fact]
    public Task FilesSerializeClearAndDoNotSurviveNavigation() => TestUtil.RunScenario(async () =>
    {
        await using var harness = await FakeNativeHarness.StartAsync();
        var window = await harness.Process.CreateWindowAsync("https://app/", 320, 240);
        var paths = new[] { Path.Combine(Path.GetTempPath(), "spaced café 文件.txt") };
        await window.SetNextDragFilesAsync(paths);
        Assert.Equal(paths[0], (await harness.StateAsync())["windows"]![0]!["dragFiles"]![0]!.GetValue<string>());
        await window.SetNextDragFilesAsync(Array.Empty<string>());
        Assert.Empty((await harness.StateAsync())["windows"]![0]!["dragFiles"]!.AsArray());
        await window.SetNextDragFilesAsync(paths);
        await window.NavigateAsync("https://app/next");
        Assert.Empty((await harness.StateAsync())["windows"]![0]!["dragFiles"]!.AsArray());
        Assert.Equal(0, await harness.ViolationsAsync());
    }, TimeSpan.FromSeconds(60));

    [Fact]
    public Task InvalidPathsNeverReachNative() => TestUtil.RunScenario(async () =>
    {
        await using var harness = await FakeNativeHarness.StartAsync();
        var window = await harness.Process.CreateWindowAsync("https://app/", 320, 240);
        foreach (var path in new[] { "", "relative.txt", "invalid\0path", Path.Combine(Path.GetTempPath(), "..", "file.txt"), Path.Combine(Path.GetTempPath(), new string('é', 17000)) })
            await Assert.ThrowsAsync<ArgumentException>(() => window.SetNextDragFilesAsync(new[] { path }));
        await Assert.ThrowsAsync<ArgumentNullException>(() => window.SetNextDragFilesAsync(null!));
        await Assert.ThrowsAsync<ArgumentOutOfRangeException>(() => window.SetNextDragFilesAsync(new string[129]));
        Assert.Equal(0, await harness.ViolationsAsync());
    }, TimeSpan.FromSeconds(60));
}
