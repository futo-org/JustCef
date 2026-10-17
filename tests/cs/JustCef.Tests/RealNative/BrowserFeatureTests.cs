using System.Buffers.Binary;
using System.Text;
using JustCef.Tests.Infrastructure;
using Xunit;

namespace JustCef.Tests.RealNative;

[Trait("Category", "RealNative")]
public class BrowserFeatureTests
{
    [RealNativeFact]
    public Task ScreenshotPdfAndPermissionBindings() => TestUtil.RunScenario(async () =>
    {
        await using var harness = await RealNativeHarness.StartAsync();
        harness.Route("/index.html", _ => Task.FromResult<IPCResponse?>(RealNativeHarness.Html("<!doctype html><style>body{height:1600px;background:#123456}</style><title>capture</title>")));
        var window = await harness.OpenPageAsync();
        var screenshot = await window.CaptureScreenshotAsync(new(FullPage: true));
        Assert.Equal(new byte[] { 137, 80, 78, 71, 13, 10, 26, 10 }, screenshot.Take(8));
        Assert.True(BinaryPrimitives.ReadUInt32BigEndian(screenshot.AsSpan(20, 4)) >= 1600);
        var pdf = await window.PrintToPdfAsync(new(PaperWidth: 8, PaperHeight: 10, PreferCssPageSize: true));
        Assert.StartsWith("%PDF-", Encoding.ASCII.GetString(pdf.AsSpan(0, 8)));
        await window.SetMediaPermissionsAsync("https://permission.test", MediaPermission.Both, PermissionSetting.Allow);
        Assert.Equal(new MediaPermissionSettings(PermissionSetting.Allow, PermissionSetting.Allow), await window.GetMediaPermissionsAsync("https://permission.test"));
        await window.SetMediaPermissionsAsync("https://permission.test", MediaPermission.Both, PermissionSetting.Default);
        Assert.NotEqual(PermissionSetting.Allow, (await window.GetMediaPermissionsAsync("https://permission.test")).Microphone);
    }, TimeSpan.FromSeconds(120));

    [RealNativeFact]
    public Task CustomSchemePopupsAreTrackedAndInheritResourceHandlers() => TestUtil.RunScenario(async () =>
    {
        await using var harness = await RealNativeHarness.StartAsync(p => p.CustomSchemes = new[] { "app" });
        var window = await harness.Process.CreateWindowAsync("app://host/index", 320, 240, requestProxy: (_, request) =>
            Task.FromResult<IPCResponse?>(RealNativeHarness.Html(request.Url.Contains("popup") ? "<!doctype html><title>popup</title>" : "<!doctype html><title>host</title>")));
        await window.WaitUntilLoadedAsync();
        var created = new TaskCompletionSource<JustCefWindow>(TaskCreationOptions.RunContinuationsAsynchronously);
        window.OnPopupCreated += p => created.TrySetResult(p);
        var result = await window.ExecuteDevToolsMethodAsync("Runtime.evaluate", "{\"expression\":\"window.open('app://popup/page');true\",\"userGesture\":true,\"returnByValue\":true}");
        Assert.True(result.Success);
        var popup = await created.Task.WithTimeout(RealNativeHarness.Wait);
        await popup.WaitUntilLoadedAsync().WithTimeout(RealNativeHarness.Wait);
        Assert.Same(popup, harness.Process.GetBrowser(popup.Identifier));
        Assert.Equal("popup", (await RealNativeHarness.EvaluateAsync(popup, "document.title")).GetString());
        Assert.Equal("undefined", (await RealNativeHarness.EvaluateAsync(popup, "typeof bridge")).GetString());
        Assert.Equal("app://host/index", (await RealNativeHarness.EvaluateAsync(window, "location.href")).GetString());
    }, TimeSpan.FromSeconds(120));

}
