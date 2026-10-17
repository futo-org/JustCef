using System.Text.Json;
using System.Text.Json.Serialization;

namespace JustCef;

/// <summary>A browser-process request. Kind is permission, certificate, download, or newWindow.
/// Permission details contain origin, type (media/prompt), CEF permission flags, and canRemember for media.
/// Certificate details contain url, error, certificateStatus, subject, and issuer.
/// Download details contain id, url, suggestedName, and mimeType. Respond with Allow and a Path or ShowDialog.
/// New-window details contain url, disposition, and userGesture. Download updates use OnDownloadUpdated.
/// Native popup windows inherit their opener's request and decision handlers; BrowserIdentifier identifies the source.</summary>
public sealed record BrowserFeatureRequest(string Kind, JsonElement Detail, int BrowserIdentifier = 0);
public sealed record BrowserDecision(bool Allow = false, bool Remember = false, string? Path = null, bool ShowDialog = false);
[Flags]
public enum MediaPermission { Microphone = 1, Camera = 2, Both = 3 }
public enum PermissionSetting { Default = 0, Allow = 1, Block = 2, Ask = 3 }
public sealed record MediaPermissionSettings(PermissionSetting Microphone, PermissionSetting Camera);
public sealed record ScreenshotOptions(bool FullPage = false, string Format = "png", int Quality = 90);
/// <summary>Paper dimensions and margins are in inches.</summary>
public sealed record PdfOptions(bool Landscape = false, bool PrintBackground = true, double PaperWidth = 8.5,
    double PaperHeight = 11, double MarginTop = 0.4, double MarginBottom = 0.4, double MarginLeft = 0.4,
    double MarginRight = 0.4, string PageRanges = "", double Scale = 1, [property: JsonPropertyName("preferCSSPageSize")] bool PreferCssPageSize = false);

public partial class JustCefBrowser
{
    private static readonly JsonSerializerOptions FeatureJson = new() { PropertyNamingPolicy = JsonNamingPolicy.CamelCase };
    private Func<BrowserFeatureRequest, CancellationToken, Task<BrowserDecision?>>? _browserRequestHandler;
    public event Action<JsonElement>? OnDownloadUpdated;

    /// <summary>Return null to let the authorized host element respond. Unanswered decisions expire after 60 seconds.</summary>
    public void SetBrowserRequestHandler(Func<BrowserFeatureRequest, CancellationToken, Task<BrowserDecision?>>? handler)
        => _browserRequestHandler = handler;

    internal void InvokeDownloadUpdated(string json)
    {
        using var document = JsonDocument.Parse(json);
        var detail = document.RootElement.Clone();
        _process.PostEvent(() => OnDownloadUpdated?.Invoke(detail));
    }

    internal async Task<string> HandleBrowserRequestAsync(string kind, string json, CancellationToken cancellationToken)
    {
        using var document = JsonDocument.Parse(json);
        var detail = document.RootElement.Clone();
        var handler = _browserRequestHandler;
        if (handler == null) return "{}";
        using var cancellation = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
        cancellation.CancelAfter(TimeSpan.FromSeconds(60));
        void Cancel() => cancellation.Cancel();
        OnClose += Cancel;
        try
        {
            if (ClosedByNative) return "{}";
            var answer = await handler(new(kind, detail, Identifier), cancellation.Token).WaitAsync(cancellation.Token).ConfigureAwait(false);
            return answer == null ? "{}" : JsonSerializer.Serialize(answer, FeatureJson);
        }
        finally { OnClose -= Cancel; }
    }

    /// <summary>Interception opens approved URLs in independent windows; window.open returns null in that mode.
    /// To open externally, launch the URL from the request handler and return a denied decision.</summary>
    public async Task SetNewWindowsAllowedAsync(bool allow, bool interceptRequests = false, CancellationToken cancellationToken = default)
        => _ = await FeatureCommandAsync("windowOpenPolicy", new { allow, intercept = interceptRequests }, cancellationToken).ConfigureAwait(false);

    /// <summary>Opens the native print UI. Completion means the request was accepted, not that printing finished.</summary>
    public async Task PrintAsync(CancellationToken cancellationToken = default)
        => _ = await FeatureCommandAsync("print", new { }, cancellationToken).ConfigureAwait(false);
    public async Task StartDownloadAsync(string url, CancellationToken cancellationToken = default)
        => _ = await FeatureCommandAsync("download", new { url }, cancellationToken).ConfigureAwait(false);
    public async Task ControlDownloadAsync(uint id, string action, CancellationToken cancellationToken = default)
    {
        if (action is not ("pause" or "resume" or "cancel")) throw new ArgumentException("Expected pause, resume, or cancel.", nameof(action));
        _ = await FeatureCommandAsync("downloadControl", new { id, action }, cancellationToken).ConfigureAwait(false);
    }
    public async Task<MediaPermissionSettings> GetMediaPermissionsAsync(string origin, CancellationToken cancellationToken = default, string? topLevelOrigin = null)
        => JsonSerializer.Deserialize<MediaPermissionSettings>(await FeatureCommandAsync("permission", new { origin, topLevelOrigin = topLevelOrigin ?? origin }, cancellationToken).ConfigureAwait(false), FeatureJson)!;
    public async Task SetMediaPermissionsAsync(string origin, MediaPermission permissions, PermissionSetting value, CancellationToken cancellationToken = default, string? topLevelOrigin = null)
    {
        _ = await FeatureCommandAsync("permission", new { origin, topLevelOrigin = topLevelOrigin ?? origin, permissions = (int)permissions, value = (int)value }, cancellationToken).ConfigureAwait(false);
    }
    private Task<string> FeatureCommandAsync(string name, object options, CancellationToken token)
        => _process.BrowserCommandAsync(Identifier, name, JsonSerializer.Serialize(options, FeatureJson), token);

    public async Task<byte[]> CaptureScreenshotAsync(ScreenshotOptions? options = null, CancellationToken cancellationToken = default)
    {
        options ??= new();
        if (options.Format is not ("png" or "jpeg" or "webp")) throw new ArgumentException("Unsupported screenshot format.", nameof(options));
        if (options.Quality is < 0 or > 100) throw new ArgumentOutOfRangeException(nameof(options));
        using var result = JsonDocument.Parse(await FeatureCommandAsync("captureScreenshot", options, cancellationToken).ConfigureAwait(false));
        return Convert.FromBase64String(result.RootElement.GetProperty("data").GetString()!);
    }

    public async Task<byte[]> PrintToPdfAsync(PdfOptions? options = null, CancellationToken cancellationToken = default)
    {
        options ??= new();
        if (!double.IsFinite(options.PaperWidth) || options.PaperWidth <= 0 || !double.IsFinite(options.PaperHeight) || options.PaperHeight <= 0 ||
            !double.IsFinite(options.Scale) || options.Scale < 0.1 || options.Scale > 2) throw new ArgumentOutOfRangeException(nameof(options));
        using var result = JsonDocument.Parse(await DevToolsResultAsync("Page.printToPDF", options, cancellationToken).ConfigureAwait(false));
        return Convert.FromBase64String(result.RootElement.GetProperty("data").GetString()!);
    }

    private async Task<byte[]> DevToolsResultAsync(string method, object parameters, CancellationToken token)
    {
        var result = await ExecuteDevToolsMethodAsync(method, JsonSerializer.Serialize(parameters, FeatureJson), token).ConfigureAwait(false);
        if (!result.Success) throw new InvalidOperationException($"{method} failed: {System.Text.Encoding.UTF8.GetString(result.Data)}");
        return result.Data;
    }
}

internal static class BrowserScheme
{
    internal static void Validate(string scheme)
    {
        if (string.IsNullOrEmpty(scheme) || scheme[0] is < 'a' or > 'z' || scheme.Any(c => !(c is >= 'a' and <= 'z' or >= '0' and <= '9' or '+' or '-' or '.')) ||
            scheme is "ftp" or "ws" or "wss" or "blob" or "filesystem" or "chrome-extension" or "chrome-untrusted" or "view-source" or "http" or "https" or "file" or "about" or "data" or "javascript" or "chrome" or "devtools" or "justcef")
            throw new ArgumentException("Expected a lowercase custom scheme name that does not replace a reserved scheme.", nameof(scheme));
    }
}
