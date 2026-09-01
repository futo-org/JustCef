using System.Diagnostics;

namespace JustCef
{
    public class JustCefWindow
    {
        private readonly JustCefProcess _process;
        public readonly int Identifier;

        public event Action? OnClose;
        public event Action? OnFocused;
        public event Action? OnUnfocused;
        public event Action<bool>? OnFullscreenChanged;
        public event Action<FrameLoadStartInfo>? OnFrameLoadStart;
        public event Action<FrameLoadEndInfo>? OnFrameLoadEnd;
        public event Action<FrameLoadErrorInfo>? OnFrameLoadError;
        public event Action<LoadingStateChangedInfo>? OnLoadingStateChanged;
        public event Action<string?, byte[]>? OnDevToolsEvent;

        private Func<JustCefWindow, IPCRequest, IPCRequest?>? _requestModifier;
        private Func<JustCefWindow, IPCRequest, Task<IPCResponse?>>? _requestProxy;
        private Func<JustCefWindow, string, string?, Task<string?>>? _bridgeRpcHandler;

        private readonly TaskCompletionSource _closeCompletionSource = new(TaskCreationOptions.RunContinuationsAsynchronously);
        private readonly object _loadingMutex = new();
        private bool _isLoading;
        private TaskCompletionSource _loadedCompletionSource = new(TaskCreationOptions.RunContinuationsAsynchronously);

        public JustCefWindow(JustCefProcess process, int identifier, Func<JustCefWindow, IPCRequest, IPCRequest?>? requestModifier, Func<JustCefWindow, IPCRequest, Task<IPCResponse?>>? requestProxy, Func<JustCefWindow, string, string?, Task<string?>>? bridgeRpcHandler, bool isLoading)
        {
            _process = process;
            Identifier = identifier;
            _requestModifier = requestModifier;
            _requestProxy = requestProxy;
            _bridgeRpcHandler = bridgeRpcHandler;
            if (isLoading) 
            {
                lock (_loadingMutex)
                {
                    if (!_isLoading)
                    {
                        _isLoading = true;
                        _loadedCompletionSource = new(TaskCreationOptions.RunContinuationsAsynchronously);
                    }
                }
            }
        }
        public async Task MaximizeAsync(CancellationToken cancellationToken = default) => await _process.WindowMaximizeAsync(Identifier, cancellationToken);
        public async Task MinimizeAsync(CancellationToken cancellationToken = default) => await _process.WindowMinimizeAsync(Identifier, cancellationToken);
        public async Task RestoreAsync(CancellationToken cancellationToken = default) => await _process.WindowRestoreAsync(Identifier, cancellationToken);
        public async Task ShowAsync(CancellationToken cancellationToken = default) => await _process.WindowShowAsync(Identifier, cancellationToken);
        public async Task HideAsync(CancellationToken cancellationToken = default) => await _process.WindowHideAsync(Identifier, cancellationToken);
        public async Task ActivateAsync(CancellationToken cancellationToken = default) => await _process.WindowActivateAsync(Identifier, cancellationToken);
        public async Task BringToTopAsync(CancellationToken cancellationToken = default) => await _process.WindowBringToTopAsync(Identifier, cancellationToken);
        public async Task SetAlwaysOnTopAsync(bool alwaysOnTop, CancellationToken cancellationToken = default) => await _process.WindowSetAlwaysOnTopAsync(Identifier, alwaysOnTop, cancellationToken);
        public async Task LoadUrlAsync(string url, CancellationToken cancellationToken = default)
        {
            lock (_loadingMutex)
            {
                if (!_isLoading)
                {
                    _isLoading = true;
                    _loadedCompletionSource = new(TaskCreationOptions.RunContinuationsAsynchronously);
                }
            }
            
            try
            {
                await _process.WindowLoadUrlAsync(Identifier, url, cancellationToken);
            }
            catch (Exception ex)
            {
                TaskCompletionSource? tcs = null;
                lock (_loadingMutex)
                {
                    if (_isLoading)
                    {
                        _isLoading = false;
                        tcs = _loadedCompletionSource;
                    }
                }

                tcs?.TrySetException(ex);
                throw;
            }
        }

        public async Task SetPositionAsync(int x, int y, CancellationToken cancellationToken = default) => await _process.WindowSetPositionAsync(Identifier, x, y, cancellationToken);
        public async Task<(int X, int Y)> GetPositionAsync(CancellationToken cancellationToken = default) => await _process.WindowGetPositionAsync(Identifier, cancellationToken);
        public async Task SetSizeAsync(int width, int height, CancellationToken cancellationToken = default) => await _process.WindowSetSizeAsync(Identifier, width, height, cancellationToken);
        public async Task<(int Width, int Height)> GetSizeAsync(CancellationToken cancellationToken = default) => await _process.WindowGetSizeAsync(Identifier, cancellationToken);
        public async Task SetZoomAsync(double zoom, CancellationToken cancellationToken = default) => await _process.WindowSetZoomAsync(Identifier, zoom, cancellationToken);
        public async Task<double> GetZoomAsync(CancellationToken cancellationToken = default) => await _process.WindowGetZoomAsync(Identifier, cancellationToken);
        public async Task<string[]> PickFileAsync(bool multiple, (string Name, string Pattern)[] filters, CancellationToken cancellationToken = default)
            => await _process.WindowPickFileAsync(Identifier, multiple, filters, cancellationToken);
        public async Task<string> PickDirectoryAsync(CancellationToken cancellationToken = default)
            => await _process.WindowPickDirectoryAsync(Identifier, cancellationToken);
        public async Task<string> SaveFileAsync(string defaultName, (string Name, string Pattern)[] filters, CancellationToken cancellationToken = default)
            => await _process.WindowSaveFileAsync(Identifier, defaultName, filters, cancellationToken);
        public async Task CloseAsync(bool forceClose = false, CancellationToken cancellationToken = default) => await _process.WindowCloseAsync(Identifier, forceClose, cancellationToken);
        public async Task SetFullscreenAsync(bool fullscreen, CancellationToken cancellationToken = default) => await _process.WindowSetFullscreenAsync(Identifier, fullscreen, cancellationToken);
        public async Task RequestFocusAsync(CancellationToken cancellationToken = default) => await _process.RequestFocusAsync(Identifier, cancellationToken);
        public async Task SetDevelopmentToolsEnabledAsync(bool developmentToolsEnabled, CancellationToken cancellationToken = default)
            => await _process.WindowSetDevelopmentToolsEnabledAsync(Identifier, developmentToolsEnabled, cancellationToken);
        public async Task SetDevelopmentToolsVisibleAsync(bool developmentToolsVisible, CancellationToken cancellationToken = default)
            => await _process.WindowSetDevelopmentToolsVisibleAsync(Identifier, developmentToolsVisible, cancellationToken);
        public async Task<(bool Success, byte[] Data)> ExecuteDevToolsMethodAsync(string methodName, string? json = null,  CancellationToken cancellationToken = default)
            => await _process.WindowExecuteDevToolsMethodAsync(Identifier, methodName, json, cancellationToken);
        public async Task<string> CallBridgeRpcAsync(string method, string? json = null, CancellationToken cancellationToken = default)
            => await _process.WindowBridgeRpcAsync(Identifier, method, json, cancellationToken);
        public async Task SetTitleAsync(string title, CancellationToken cancellationToken = default)
            => await _process.WindowSetTitleAsync(Identifier, title, cancellationToken);
        public async Task SetIconAsync(string iconPath, CancellationToken cancellationToken = default)
            => await _process.WindowSetIconAsync(Identifier, iconPath, cancellationToken);
        public async Task AddUrlToProxyAsync(string url, CancellationToken cancellationToken = default)
            => await _process.WindowAddUrlToProxyAsync(Identifier, url, cancellationToken);
        public async Task RemoveUrlToProxyAsync(string url, CancellationToken cancellationToken = default)
            => await _process.WindowRemoveUrlToProxyAsync(Identifier, url, cancellationToken);
        public async Task AddDomainToProxyAsync(string url, CancellationToken cancellationToken = default)
            => await _process.WindowAddDomainToProxyAsync(Identifier, url, cancellationToken);
        public async Task RemoveDomainToProxyAsync(string url, CancellationToken cancellationToken = default)
            => await _process.WindowRemoveDomainToProxyAsync(Identifier, url, cancellationToken);
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

        public void SetRequestProxy(Func<JustCefWindow, IPCRequest, Task<IPCResponse?>>? requestProxy)
        {
            _requestProxy = requestProxy;
        }

        public void SetBridgeRpcHandler(Func<JustCefWindow, string, string?, Task<string?>>? bridgeRpcHandler)
        {
            _bridgeRpcHandler = bridgeRpcHandler;
        }

        public async Task SetModifyRequestsAsync(bool modifyRequests, bool modifyBody, CancellationToken cancellationToken = default)
            => await _process.WindowSetModifyRequestsAsync(Identifier, modifyRequests, modifyBody, cancellationToken);

        public void SetRequestModifier(Func<JustCefWindow, IPCRequest, IPCRequest>? requestModifier)
        {
            _requestModifier = requestModifier;
        }

        internal void InvokeOnClose()
        {
            TaskCompletionSource? loadedCompletionSourceToFail = null;
            lock (_loadingMutex)
            {
                if (_isLoading)
                {
                    _isLoading = false;
                    loadedCompletionSourceToFail = _loadedCompletionSource;
                }
            }

            loadedCompletionSourceToFail?.TrySetException(new InvalidOperationException("Window was closed before loading completed."));

            _closeCompletionSource.TrySetResult();
            OnClose?.Invoke();
        }

        internal void InvokeOnFocused() => OnFocused?.Invoke();
        internal void InvokeOnUnfocused() => OnUnfocused?.Invoke();
        internal void InvokeOnFullscreenChanged(bool fullscreen) => OnFullscreenChanged?.Invoke(fullscreen);
        internal void InvokeOnFrameLoadStart(string? frameIdentifier, bool isMainFrame, string? url)
        {
            OnFrameLoadStart?.Invoke(new FrameLoadStartInfo(
                FrameIdentifier: frameIdentifier,
                IsMainFrame: isMainFrame,
                Url: url));
        }

        internal void InvokeOnFrameLoadEnd(string? frameIdentifier, bool isMainFrame, string? url, int httpStatusCode)
        {
            OnFrameLoadEnd?.Invoke(new FrameLoadEndInfo(
                FrameIdentifier: frameIdentifier,
                IsMainFrame: isMainFrame,
                Url: url,
                HttpStatusCode: httpStatusCode));
        }

        internal void InvokeOnFrameLoadError(string? frameIdentifier, bool isMainFrame, int errorCode, string? errorText, string? failedUrl)
        {
            OnFrameLoadError?.Invoke(new FrameLoadErrorInfo(
                FrameIdentifier: frameIdentifier,
                IsMainFrame: isMainFrame,
                ErrorCode: errorCode,
                ErrorText: errorText,
                FailedUrl: failedUrl));
        }

        internal void InvokeOnLoadingStateChanged(bool isLoading, bool canGoBack, bool canGoForward)
        {
            TaskCompletionSource? loadedCompletionSourceToComplete = null;

            lock (_loadingMutex)
            {
                if (isLoading)
                {
                    if (!_isLoading)
                    {
                        _isLoading = true;
                        _loadedCompletionSource = new(TaskCreationOptions.RunContinuationsAsynchronously);
                    }
                }
                else
                {
                    if (_isLoading)
                    {
                        _isLoading = false;
                        loadedCompletionSourceToComplete = _loadedCompletionSource;
                    }
                }
            }

            loadedCompletionSourceToComplete?.TrySetResult();

            OnLoadingStateChanged?.Invoke(new LoadingStateChangedInfo(
                IsLoading: isLoading,
                CanGoBack: canGoBack,
                CanGoForward: canGoForward));
        }

        internal void InvokeOnDevToolsEvent(string? method, byte[] parameters) => OnDevToolsEvent?.Invoke(method, parameters);

        public void WaitForExit() => _closeCompletionSource.Task.Wait();
        public async Task WaitForExitAsync(CancellationToken cancellationToken = default)
        {
            await _closeCompletionSource.Task.WaitAsync(cancellationToken);
        }

        public async Task<IPCResponse?> ProxyRequestAsync(IPCRequest request)
        {
            try
            {
                return await _requestProxy!(this, request);
            }
            catch (Exception e)
            {
                Logger.Error<JustCefWindow>($"Exception occurred while processing request proxy", e);
                Debugger.Break();

                _ = Task.Run(async () =>
                {
                    try
                    {
                        await CloseAsync(true);
                    }
                    catch (Exception ex)
                    {
                        Logger.Error<JustCefWindow>($"Exception occurred while trying to close window after request proxy exception.", ex);
                    }
                });

                return new IPCResponse()
                {
                    StatusCode = 404,
                    StatusText = "Not Found",
                    Headers = new(StringComparer.InvariantCultureIgnoreCase),
                    DataSource = null
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
                Logger.Error<JustCefWindow>($"Exception occurred while processing modify request", e);
                Debugger.Break();

                _ = Task.Run(async () =>
                {
                    try
                    {
                        await CloseAsync(true);
                    }
                    catch (Exception ex)
                    {
                        Logger.Error<JustCefWindow>($"Exception occurred while trying to close window after modify request exception.", ex);
                    }
                });
                return request;
            }
        }

        internal async Task<string?> InvokeBridgeRpcAsync(string method, string? json)
        {
            if (_bridgeRpcHandler == null)
                throw new InvalidOperationException("No bridge RPC handler is registered for this window.");

            return await _bridgeRpcHandler(this, method, json);
        }

        public async Task NavigateAsync(string url, CancellationToken cancellationToken = default)
        {
            await LoadUrlAsync(url, cancellationToken);
            await WaitUntilLoadedAsync(cancellationToken);
        }

        public async Task WaitUntilLoadedAsync(CancellationToken cancellationToken = default)
        {
            Task waitTask;

            lock (_loadingMutex)
            {
                if (!_isLoading)
                    return;

                waitTask = _loadedCompletionSource.Task;
            }

            await waitTask.WaitAsync(cancellationToken);
        }

        public readonly record struct FrameLoadStartInfo(
            string? FrameIdentifier,
            bool IsMainFrame,
            string? Url);

        public readonly record struct FrameLoadEndInfo(
            string? FrameIdentifier,
            bool IsMainFrame,
            string? Url,
            int HttpStatusCode);

        public readonly record struct FrameLoadErrorInfo(
            string? FrameIdentifier,
            bool IsMainFrame,
            int ErrorCode,
            string? ErrorText,
            string? FailedUrl);

        public readonly record struct LoadingStateChangedInfo(
            bool IsLoading,
            bool CanGoBack,
            bool CanGoForward);
    }
}
