using JustCef.Tests.Infrastructure;
using Xunit;

namespace JustCef.Tests.Fake;

[Trait("Category", "Fake")]
public class SmokeTests
{
    [Fact]
    public Task CreateWindowAndCallOperations() => TestUtil.RunScenario(async () =>
    {
        await using var harness = await FakeNativeHarness.StartAsync();
        var window = await harness.Process.CreateWindowAsync("https://app/", 0, 0);
        Assert.Equal(1, window.Identifier);
        Assert.Same(window, harness.Process.GetWindow(1));

        await window.SetSizeAsync(640, 480);
        Assert.Equal((640, 480), await window.GetSizeAsync());
        await window.SetZoomAsync(1.5);
        Assert.Equal(1.5, await window.GetZoomAsync());
        await window.WaitUntilLoadedAsync();
        await harness.Process.EchoAsync(new byte[] { 1, 2, 3 });
        var echo = await harness.Process.CallAsync(JustCefProcess.OpcodeController.Echo, new PacketWriter().WriteBytes(new byte[] { 1, 2, 3 }));
        Assert.Equal(new byte[] { 1, 2, 3 }, echo.ReadBytes(echo.RemainingSize));
        await window.NavigateAsync("https://app/next");
        var widevine = await harness.Process.GetWidevineStatusAsync();
        Assert.False(widevine.Installed);

        var closed = new TaskCompletionSource();
        window.OnClose += () => closed.TrySetResult();
        await window.CloseAsync();
        await closed.Task.WithTimeout(TimeSpan.FromSeconds(10));
        Assert.Empty(harness.Process.Windows);
        Assert.Equal(0, await harness.ViolationsAsync());
    }, TimeSpan.FromSeconds(60));

    [Fact]
    public Task LauncherPathAndEnvironmentReachTheNativeProcess() => TestUtil.RunScenario(async () =>
    {
        if (OperatingSystem.IsWindows())
            return;

        string launcher = Path.Combine(Path.GetTempPath(), $"justcef-launcher-{Guid.NewGuid():N}.sh");
        File.WriteAllText(launcher, "#!/bin/sh\n[ \"$JUSTCEF_TEST_LAUNCHER\" = \"1\" ] || exit 3\nexec \"$@\"\n");
        File.SetUnixFileMode(launcher, UnixFileMode.UserRead | UnixFileMode.UserWrite | UnixFileMode.UserExecute);
        try
        {
            await using var harness = await FakeNativeHarness.StartAsync(process =>
            {
                process.LauncherPath = launcher;
                process.EnvironmentVariables["JUSTCEF_TEST_LAUNCHER"] = "1";
            });
            Assert.Equal(0, await harness.ViolationsAsync());
        }
        finally
        {
            File.Delete(launcher);
        }
    }, TimeSpan.FromSeconds(60));
}
