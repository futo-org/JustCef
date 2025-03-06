using System.Diagnostics;

namespace DotCef
{
    public class DotCefWindow
    {
        private readonly DotCefProcess _process;
        public readonly int Identifier;

        public event Action? OnClose;
        public event Action? OnFocused;
        public event Action? OnUnfocused;
        public event Action<bool>? OnFullscreenChanged;
        public event Action<string?>? OnLoadStart;
        public event Action<string?>? OnLoadEnd;
        public event Action<int, string?, string?>? OnLoadError;
        public event Action<string?, byte[]>? OnDevToolsEvent;

        private Func<DotCefWindow, IPCRequest, IPCRequest?>? _requestModifier;
        private Func<DotCefWindow, IPCRequest, Task<IPCResponse?>>? _requestProxy;

        private readonly TaskCompletionSource _closeCompletionSource = new TaskCompletionSource();

        public DotCefWindow(DotCefProcess process, int identifier, Func<DotCefWindow, IPCRequest, IPCRequest?>? requestModifier, Func<DotCefWindow, IPCRequest, Task<IPCResponse?>>? requestProxy)
        {
            _process = process;
            Identifier = identifier;
            _requestModifier = requestModifier;
            _requestProxy = requestProxy;
        }

        public async Task MaximizeAsync(CancellationToken cancellationToken = default) => await _process.WindowMaximizeAsync(Identifier, cancellationToken);
        public async Task MinimizeAsync(CancellationToken cancellationToken = default) => await _process.WindowMinimizeAsync(Identifier, cancellationToken);
        public async Task RestoreAsync(CancellationToken cancellationToken = default) => await _process.WindowRestoreAsync(Identifier, cancellationToken);
        public async Task ShowAsync(CancellationToken cancellationToken = default) => await _process.WindowShowAsync(Identifier, cancellationToken);
        public async Task HideAsync(CancellationToken cancellationToken = default) => await _process.WindowHideAsync(Identifier, cancellationToken);
        public async Task ActivateAsync(CancellationToken cancellationToken = default) => await _process.WindowActivateAsync(Identifier, cancellationToken);
        public async Task BringToTopAsync(CancellationToken cancellationToken = default) => await _process.WindowBringToTopAsync(Identifier, cancellationToken);
        public async Task SetAlwaysOnTopAsync(bool alwaysOnTop, CancellationToken cancellationToken = default) => await _process.WindowSetAlwaysOnTopAsync(Identifier, alwaysOnTop, cancellationToken);
        public async Task LoadUrlAsync(string url, CancellationToken cancellationToken = default) => await _process.WindowLoadUrlAsync(Identifier, url, cancellationToken);
        public async Task SetPositionAsync(int x, int y, CancellationToken cancellationToken = default) => await _process.WindowSetPositionAsync(Identifier, x, y, cancellationToken);
        public async Task<(int X, int Y)> GetPositionAsync(CancellationToken cancellationToken = default) => await _process.WindowGetPositionAsync(Identifier, cancellationToken);
        public async Task SetSizeAsync(int width, int height, CancellationToken cancellationToken = default) => await _process.WindowSetSizeAsync(Identifier, width, height, cancellationToken);
        public async Task<(int Width, int Height)> GetSizeAsync(CancellationToken cancellationToken = default) => await _process.WindowGetSizeAsync(Identifier, cancellationToken);
        public async Task CloseAsync(bool forceClose = false, CancellationToken cancellationToken = default) => await _process.WindowCloseAsync(Identifier, forceClose, cancellationToken);
        public async Task SetFullscreenAsync(bool fullscreen, CancellationToken cancellationToken = default) => await _process.WindowSetFullscreenAsync(Identifier, fullscreen, cancellationToken);
        public async Task RequestFocusAsync(CancellationToken cancellationToken = default) => await _process.RequestFocusAsync(Identifier, cancellationToken);
        public async Task SetDevelopmentToolsEnabledAsync(bool developmentToolsEnabled, CancellationToken cancellationToken = default)
            => await _process.WindowSetDevelopmentToolsEnabledAsync(Identifier, developmentToolsEnabled, cancellationToken);
        public async Task SetDevelopmentToolsVisibleAsync(bool developmentToolsVisible, CancellationToken cancellationToken = default)
            => await _process.WindowSetDevelopmentToolsVisibleAsync(Identifier, developmentToolsVisible, cancellationToken);
        public async Task<(bool Success, byte[] Data)> ExecuteDevToolsMethodAsync(string methodName, string? json = null,  CancellationToken cancellationToken = default)
            => await _process.WindowExecuteDevToolsMethodAsync(Identifier, methodName, json, cancellationToken);
        public async Task SetTitleAsync(string title, CancellationToken cancellationToken = default)
            => await _process.WindowSetTitleAsync(Identifier, title, cancellationToken);
        public async Task SetIconAsync(string iconPath, CancellationToken cancellationToken = default)
            => await _process.WindowSetIconAsync(Identifier, iconPath, cancellationToken);
        public async Task AddUrlToProxyAsync(string url, CancellationToken cancellationToken = default)
            => await _process.WindowAddUrlToProxyAsync(Identifier, url, cancellationToken);
        public async Task RemoveUrlToProxyAsync(string url, CancellationToken cancellationToken = default)
            => await _process.WindowRemoveUrlToProxyAsync(Identifier, url, cancellationToken);
        public async Task AddUrlToModifyAsync(string url, CancellationToken cancellationToken = default)
            => await _process.WindowAddUrlToModifyAsync(Identifier, url, cancellationToken);
        public async Task RemoveUrlToModifyAsync(string url, CancellationToken cancellationToken = default)
            => await _process.WindowRemoveUrlToModifyAsync(Identifier, url, cancellationToken);
        public async Task AddDevToolsEventMethod(string method, CancellationToken cancellationToken = default)
            => await _process.WindowAddDevToolsEventMethod(Identifier, method, cancellationToken);
        public async Task RemoveDevToolsEventMethod(string method, CancellationToken cancellationToken = default)
            => await _process.WindowRemoveDevToolsEventMethod(Identifier, method, cancellationToken);

        public async Task CenterSelfAsync(CancellationToken cancellationToken = default) => await _process.WindowCenterSelfAsync(Identifier, cancellationToken);

        public async Task SetProxyRequestsAsync(bool proxyRequests, CancellationToken cancellationToken = default)
        {
            if (proxyRequests && _requestProxy == null)
                throw new ArgumentException("When proxyRequests is true, _requestProxy must be set.");
            await _process.WindowSetProxyRequestsAsync(Identifier, proxyRequests, cancellationToken);
        }

        public void SetRequestProxy(Func<DotCefWindow, IPCRequest, Task<IPCResponse?>>? requestProxy)
        {
            _requestProxy = requestProxy;
        }

        public async Task SetModifyRequestsAsync(bool modifyRequests, bool modifyBody, CancellationToken cancellationToken = default)
            => await _process.WindowSetModifyRequestsAsync(Identifier, modifyRequests, modifyBody, cancellationToken);

        public void SetRequestModifier(Func<DotCefWindow, IPCRequest, IPCRequest>? requestModifier)
        {
            _requestModifier = requestModifier;
        }

        public void InvokeOnClose() 
        {
            _closeCompletionSource.TrySetResult();
            OnClose?.Invoke();
        }

        public void InvokeOnFocused() => OnFocused?.Invoke();
        public void InvokeOnUnfocused() => OnUnfocused?.Invoke();
        public void InvokeOnFullscreenChanged(bool fullscreen) => OnFullscreenChanged?.Invoke(fullscreen);
        public void InvokeOnLoadStart(string? url) => OnLoadStart?.Invoke(url);
        public void InvokeOnLoadEnd(string? url) => OnLoadEnd?.Invoke(url);
        public void InvokeOnLoadError(int errorCode, string? errorText, string? failedUrl) => OnLoadError?.Invoke(errorCode, errorText, failedUrl);
        public void InvokeOnDevToolsEvent(string? method, byte[] parameters) => OnDevToolsEvent?.Invoke(method, parameters);

        public void WaitForExit() => _closeCompletionSource.Task.Wait();
        public async Task WaitForExitAsync(CancellationToken cancellationToken = default) 
        {
            await Task.WhenAny(_closeCompletionSource.Task, Task.Delay(Timeout.Infinite, cancellationToken));
        }

        public async Task<IPCResponse?> ProxyRequestAsync(IPCRequest request)
        {
            try
            {
                return await _requestProxy!(this, request);
            }
            catch (Exception e)
            {
                Logger.Error<DotCefWindow>($"Exception occurred while processing request proxy", e);
                Debugger.Break();

                _ = Task.Run(async () =>
                {
                    try
                    {
                        await CloseAsync(true);
                    }
                    catch (Exception ex)
                    {
                        Logger.Error<DotCefWindow>($"Exception occurred while trying to close window after request proxy exception.", ex);
                    }
                });

                return new IPCResponse()
                {
                    StatusCode = 404,
                    StatusText = "Not Found",
                    Headers = new(StringComparer.InvariantCultureIgnoreCase),
                    BodyStream = null
                };
            }
        }

        public IPCRequest? ModifyRequest(IPCRequest request)
        {
            try
            {
                if (_requestModifier != null)
                    return _requestModifier(this, request);
                return request;
            }
            catch (Exception e)
            {
                Logger.Error<DotCefWindow>($"Exception occurred while processing modify request", e);
                Debugger.Break();

                _ = Task.Run(async () =>
                {
                    try
                    {
                        await CloseAsync(true);
                    }
                    catch (Exception ex)
                    {
                        Logger.Error<DotCefWindow>($"Exception occurred while trying to close window after modify request exception.", ex);
                    }
                });
                return request;
            }
        }
    }
}