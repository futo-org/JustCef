namespace JustCef
{
    public class JustCefBrowser
    {
        private protected readonly JustCefProcess _process;
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

        private Func<IPCRequest, IPCRequest?>? _requestModifier;
        private Func<IPCRequest, Task<IPCResponse?>>? _requestProxy;

        private readonly TaskCompletionSource _closeCompletionSource = new(TaskCreationOptions.RunContinuationsAsynchronously);
        private readonly object _loadingMutex = new();
        private bool _isLoading;
        private TaskCompletionSource _loadedCompletionSource = new(TaskCreationOptions.RunContinuationsAsynchronously);
        private readonly List<TaskCompletionSource> _unsentNavigations = new();
        private readonly List<TaskCompletionSource> _navigations = new();
        private readonly List<TaskCompletionSource> _startedNavigations = new();
        private const int ErrorAborted = -3;

        internal bool ClosedByNative { get; private set; }

        private protected JustCefBrowser(JustCefProcess process, int identifier, bool isLoading)
        {
            _process = process;
            Identifier = identifier;
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
        public Task LoadUrlAsync(string url, CancellationToken cancellationToken = default) => LoadUrlAsync(url, null, cancellationToken);

        private async Task LoadUrlAsync(string url, TaskCompletionSource? navigation, CancellationToken cancellationToken)
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
                await _process.WindowLoadUrlAsync(Identifier, url, navigation == null ? null : () => MarkNavigationSent(navigation), cancellationToken).ConfigureAwait(false);
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

        public async Task SetZoomAsync(double zoom, CancellationToken cancellationToken = default) => await _process.WindowSetZoomAsync(Identifier, zoom, cancellationToken).ConfigureAwait(false);
        public async Task<double> GetZoomAsync(CancellationToken cancellationToken = default) => await _process.WindowGetZoomAsync(Identifier, cancellationToken).ConfigureAwait(false);
        public async Task<string[]> PickFileAsync(bool multiple, (string Name, string Pattern)[] filters, CancellationToken cancellationToken = default)
            => await _process.WindowPickFileAsync(Identifier, multiple, filters, cancellationToken).ConfigureAwait(false);
        public async Task<string> PickDirectoryAsync(CancellationToken cancellationToken = default)
            => await _process.WindowPickDirectoryAsync(Identifier, cancellationToken).ConfigureAwait(false);
        public async Task<string> SaveFileAsync(string defaultName, (string Name, string Pattern)[] filters, CancellationToken cancellationToken = default)
            => await _process.WindowSaveFileAsync(Identifier, defaultName, filters, cancellationToken).ConfigureAwait(false);
        public async Task CloseAsync(bool forceClose = false, CancellationToken cancellationToken = default) => await _process.WindowCloseAsync(Identifier, forceClose, cancellationToken).ConfigureAwait(false);
        public async Task RequestFocusAsync(CancellationToken cancellationToken = default) => await _process.RequestFocusAsync(Identifier, cancellationToken).ConfigureAwait(false);
        public async Task SetDevelopmentToolsEnabledAsync(bool developmentToolsEnabled, CancellationToken cancellationToken = default)
            => await _process.WindowSetDevelopmentToolsEnabledAsync(Identifier, developmentToolsEnabled, cancellationToken).ConfigureAwait(false);
        public async Task SetDevelopmentToolsVisibleAsync(bool developmentToolsVisible, CancellationToken cancellationToken = default)
            => await _process.WindowSetDevelopmentToolsVisibleAsync(Identifier, developmentToolsVisible, cancellationToken).ConfigureAwait(false);
        public async Task<(bool Success, byte[] Data)> ExecuteDevToolsMethodAsync(string methodName, string? json = null,  CancellationToken cancellationToken = default)
            => await _process.WindowExecuteDevToolsMethodAsync(Identifier, methodName, json, cancellationToken).ConfigureAwait(false);
        public async Task AddUrlToProxyAsync(string url, CancellationToken cancellationToken = default)
            => await _process.WindowAddUrlToProxyAsync(Identifier, url, cancellationToken).ConfigureAwait(false);
        public async Task RemoveUrlToProxyAsync(string url, CancellationToken cancellationToken = default)
            => await _process.WindowRemoveUrlToProxyAsync(Identifier, url, cancellationToken).ConfigureAwait(false);
        public async Task AddDomainToProxyAsync(string url, CancellationToken cancellationToken = default)
            => await _process.WindowAddDomainToProxyAsync(Identifier, url, cancellationToken).ConfigureAwait(false);
        public async Task RemoveDomainToProxyAsync(string url, CancellationToken cancellationToken = default)
            => await _process.WindowRemoveDomainToProxyAsync(Identifier, url, cancellationToken).ConfigureAwait(false);
        public async Task AddUrlToModifyAsync(string url, CancellationToken cancellationToken = default)
            => await _process.WindowAddUrlToModifyAsync(Identifier, url, cancellationToken).ConfigureAwait(false);
        public async Task RemoveUrlToModifyAsync(string url, CancellationToken cancellationToken = default)
            => await _process.WindowRemoveUrlToModifyAsync(Identifier, url, cancellationToken).ConfigureAwait(false);
        public async Task AddDevToolsEventMethod(string method, CancellationToken cancellationToken = default)
            => await _process.WindowAddDevToolsEventMethod(Identifier, method, cancellationToken).ConfigureAwait(false);
        public async Task RemoveDevToolsEventMethod(string method, CancellationToken cancellationToken = default)
            => await _process.WindowRemoveDevToolsEventMethod(Identifier, method, cancellationToken).ConfigureAwait(false);

        public async Task SetProxyRequestsAsync(bool proxyRequests, CancellationToken cancellationToken = default)
        {
            if (proxyRequests && _requestProxy == null)
                throw new ArgumentException("When proxyRequests is true, _requestProxy must be set.");
            await _process.WindowSetProxyRequestsAsync(Identifier, proxyRequests, cancellationToken).ConfigureAwait(false);
        }

        private protected void SetRequestProxyCore(Func<IPCRequest, Task<IPCResponse?>>? requestProxy)
        {
            _requestProxy = requestProxy;
        }

        public async Task SetModifyRequestsAsync(bool modifyRequests, bool modifyBody, CancellationToken cancellationToken = default)
            => await _process.WindowSetModifyRequestsAsync(Identifier, modifyRequests, modifyBody, cancellationToken).ConfigureAwait(false);

        private protected void SetRequestModifierCore(Func<IPCRequest, IPCRequest?>? requestModifier)
        {
            _requestModifier = requestModifier;
        }

        internal void InvokeOnClose(bool closedByNative = false)
        {
            ClosedByNative = closedByNative;
            TaskCompletionSource? loadedCompletionSourceToFail = null;
            TaskCompletionSource[] navigationsToFail;
            lock (_loadingMutex)
            {
                if (_isLoading)
                {
                    _isLoading = false;
                    loadedCompletionSourceToFail = _loadedCompletionSource;
                }

                navigationsToFail = TakeNavigations();
                _closeCompletionSource.TrySetResult();
            }

            loadedCompletionSourceToFail?.TrySetException(new InvalidOperationException("Window was closed before loading completed."));
            foreach (var navigation in navigationsToFail)
                navigation.TrySetException(new InvalidOperationException("Window was closed before loading completed."));

            _process.PostEvent(() => OnClose?.Invoke());
        }

        internal void InvokeOnFocused() => _process.PostEvent(() => OnFocused?.Invoke());
        internal void InvokeOnUnfocused() => _process.PostEvent(() => OnUnfocused?.Invoke());
        internal void InvokeOnFullscreenChanged(bool fullscreen) => _process.PostEvent(() => OnFullscreenChanged?.Invoke(fullscreen));
        internal void InvokeOnFrameLoadStart(string? frameIdentifier, bool isMainFrame, string? url)
        {
            if (isMainFrame)
            {
                lock (_loadingMutex)
                    StartNavigations();
            }

            _process.PostEvent(() => OnFrameLoadStart?.Invoke(new FrameLoadStartInfo(
                FrameIdentifier: frameIdentifier,
                IsMainFrame: isMainFrame,
                Url: url)));
        }

        internal void InvokeOnFrameLoadEnd(string? frameIdentifier, bool isMainFrame, string? url, int httpStatusCode)
        {
            _process.PostEvent(() => OnFrameLoadEnd?.Invoke(new FrameLoadEndInfo(
                FrameIdentifier: frameIdentifier,
                IsMainFrame: isMainFrame,
                Url: url,
                HttpStatusCode: httpStatusCode)));
        }

        internal void InvokeOnFrameLoadError(string? frameIdentifier, bool isMainFrame, int errorCode, string? errorText, string? failedUrl)
        {
            if (isMainFrame && errorCode != ErrorAborted)
            {
                TaskCompletionSource[] navigationsToFail;
                lock (_loadingMutex)
                    navigationsToFail = TakeNavigations();

                foreach (var navigation in navigationsToFail)
                    navigation.TrySetException(new InvalidOperationException($"Navigation to '{failedUrl}' failed: {errorText} ({errorCode})."));
            }

            _process.PostEvent(() => OnFrameLoadError?.Invoke(new FrameLoadErrorInfo(
                FrameIdentifier: frameIdentifier,
                IsMainFrame: isMainFrame,
                ErrorCode: errorCode,
                ErrorText: errorText,
                FailedUrl: failedUrl)));
        }

        internal void InvokeOnLoadingStateChanged(bool isLoading, bool canGoBack, bool canGoForward)
        {
            TaskCompletionSource? loadedCompletionSourceToComplete = null;
            TaskCompletionSource[] navigationsToComplete = [];

            lock (_loadingMutex)
            {
                if (isLoading)
                {
                    if (!_isLoading)
                    {
                        _isLoading = true;
                        _loadedCompletionSource = new(TaskCreationOptions.RunContinuationsAsynchronously);
                    }

                    StartNavigations();
                }
                else
                {
                    if (_isLoading)
                    {
                        _isLoading = false;
                        loadedCompletionSourceToComplete = _loadedCompletionSource;
                    }

                    navigationsToComplete = _startedNavigations.ToArray();
                    _startedNavigations.Clear();
                }
            }

            loadedCompletionSourceToComplete?.TrySetResult();
            foreach (var navigation in navigationsToComplete)
                navigation.TrySetResult();

            _process.PostEvent(() => OnLoadingStateChanged?.Invoke(new LoadingStateChangedInfo(
                IsLoading: isLoading,
                CanGoBack: canGoBack,
                CanGoForward: canGoForward)));
        }

        internal void InvokeOnDevToolsEvent(string? method, byte[] parameters) => _process.PostEvent(() => OnDevToolsEvent?.Invoke(method, parameters));

        public void WaitForExit() => _closeCompletionSource.Task.Wait();
        public async Task WaitForExitAsync(CancellationToken cancellationToken = default)
        {
            await _closeCompletionSource.Task.WaitAsync(cancellationToken).ConfigureAwait(false);
        }

        public async Task<IPCResponse?> ProxyRequestAsync(IPCRequest request)
        {
            var requestProxy = _requestProxy;
            if (requestProxy == null)
            {
                Logger.Warning<JustCefWindow>($"Request proxy is not set for window {Identifier}.");
                return null;
            }

            return await requestProxy(request).ConfigureAwait(false);
        }

        public IPCRequest? ModifyRequest(IPCRequest request)
        {
            var requestModifier = _requestModifier;
            if (requestModifier == null)
                return null;
            return requestModifier(request);
        }

        public async Task NavigateAsync(string url, CancellationToken cancellationToken = default)
        {
            var navigation = BeginNavigation();
            try
            {
                await LoadUrlAsync(url, navigation, cancellationToken).ConfigureAwait(false);
                await navigation.Task.WaitAsync(cancellationToken).ConfigureAwait(false);
            }
            finally
            {
                EndNavigation(navigation);
            }
        }

        internal TaskCompletionSource BeginNavigation()
        {
            var navigation = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
            lock (_loadingMutex)
            {
                if (_closeCompletionSource.Task.IsCompleted)
                    navigation.TrySetException(new InvalidOperationException("Window was closed before loading completed."));
                else
                    _unsentNavigations.Add(navigation);
            }

            return navigation;
        }

        internal void MarkNavigationSent(TaskCompletionSource navigation)
        {
            lock (_loadingMutex)
            {
                if (_unsentNavigations.Remove(navigation))
                    _navigations.Add(navigation);
            }
        }

        internal void EndNavigation(TaskCompletionSource navigation)
        {
            lock (_loadingMutex)
            {
                _unsentNavigations.Remove(navigation);
                _navigations.Remove(navigation);
                _startedNavigations.Remove(navigation);
            }
        }

        private void StartNavigations()
        {
            _startedNavigations.AddRange(_navigations);
            _navigations.Clear();
        }

        private TaskCompletionSource[] TakeNavigations()
        {
            TaskCompletionSource[] navigations = [.. _unsentNavigations, .. _navigations, .. _startedNavigations];
            _unsentNavigations.Clear();
            _navigations.Clear();
            _startedNavigations.Clear();
            return navigations;
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

            await waitTask.WaitAsync(cancellationToken).ConfigureAwait(false);
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

    public class JustCefWindow : JustCefBrowser
    {
        private Func<JustCefWindow, string, string?, Task<string?>>? _bridgeRpcHandler;
        private Func<JustCefView, Task>? _viewCreatedHandler;

        public JustCefWindow(JustCefProcess process, int identifier, Func<JustCefWindow, IPCRequest, IPCRequest?>? requestModifier, Func<JustCefWindow, IPCRequest, Task<IPCResponse?>>? requestProxy, Func<JustCefWindow, string, string?, Task<string?>>? bridgeRpcHandler, bool isLoading)
            : base(process, identifier, isLoading)
        {
            SetRequestModifierCore(requestModifier == null ? null : request => requestModifier(this, request));
            SetRequestProxyCore(requestProxy == null ? null : request => requestProxy(this, request));
            _bridgeRpcHandler = bridgeRpcHandler;
        }

        public IReadOnlyList<JustCefView> Views => _process.GetViews(this);

        public async Task MaximizeAsync(CancellationToken cancellationToken = default) => await _process.WindowMaximizeAsync(Identifier, cancellationToken).ConfigureAwait(false);
        public async Task MinimizeAsync(CancellationToken cancellationToken = default) => await _process.WindowMinimizeAsync(Identifier, cancellationToken).ConfigureAwait(false);
        public async Task RestoreAsync(CancellationToken cancellationToken = default) => await _process.WindowRestoreAsync(Identifier, cancellationToken).ConfigureAwait(false);
        public async Task ShowAsync(CancellationToken cancellationToken = default) => await _process.WindowShowAsync(Identifier, cancellationToken).ConfigureAwait(false);
        public async Task HideAsync(CancellationToken cancellationToken = default) => await _process.WindowHideAsync(Identifier, cancellationToken).ConfigureAwait(false);
        public async Task ActivateAsync(CancellationToken cancellationToken = default) => await _process.WindowActivateAsync(Identifier, cancellationToken).ConfigureAwait(false);
        public async Task BringToTopAsync(CancellationToken cancellationToken = default) => await _process.WindowBringToTopAsync(Identifier, cancellationToken).ConfigureAwait(false);
        public async Task SetAlwaysOnTopAsync(bool alwaysOnTop, CancellationToken cancellationToken = default) => await _process.WindowSetAlwaysOnTopAsync(Identifier, alwaysOnTop, cancellationToken).ConfigureAwait(false);
        public async Task SetPositionAsync(int x, int y, CancellationToken cancellationToken = default) => await _process.WindowSetPositionAsync(Identifier, x, y, cancellationToken).ConfigureAwait(false);
        public async Task<(int X, int Y)> GetPositionAsync(CancellationToken cancellationToken = default) => await _process.WindowGetPositionAsync(Identifier, cancellationToken).ConfigureAwait(false);
        public async Task SetSizeAsync(int width, int height, CancellationToken cancellationToken = default) => await _process.WindowSetSizeAsync(Identifier, width, height, cancellationToken).ConfigureAwait(false);
        public async Task<(int Width, int Height)> GetSizeAsync(CancellationToken cancellationToken = default) => await _process.WindowGetSizeAsync(Identifier, cancellationToken).ConfigureAwait(false);
        public async Task SetFullscreenAsync(bool fullscreen, CancellationToken cancellationToken = default) => await _process.WindowSetFullscreenAsync(Identifier, fullscreen, cancellationToken).ConfigureAwait(false);
        public async Task<string> CallBridgeRpcAsync(string method, string? json = null, CancellationToken cancellationToken = default)
            => await _process.WindowBridgeRpcAsync(Identifier, method, json, cancellationToken).ConfigureAwait(false);
        public async Task SetTitleAsync(string title, CancellationToken cancellationToken = default)
            => await _process.WindowSetTitleAsync(Identifier, title, cancellationToken).ConfigureAwait(false);
        public async Task SetIconAsync(string iconPath, CancellationToken cancellationToken = default)
            => await _process.WindowSetIconAsync(Identifier, iconPath, cancellationToken).ConfigureAwait(false);
        public async Task CenterSelfAsync(CancellationToken cancellationToken = default) => await _process.WindowCenterSelfAsync(Identifier, cancellationToken).ConfigureAwait(false);

        public void SetRequestProxy(Func<JustCefWindow, IPCRequest, Task<IPCResponse?>>? requestProxy)
            => SetRequestProxyCore(requestProxy == null ? null : request => requestProxy(this, request));

        public void SetRequestModifier(Func<JustCefWindow, IPCRequest, IPCRequest>? requestModifier)
            => SetRequestModifierCore(requestModifier == null ? null : request => requestModifier(this, request));

        public void SetBridgeRpcHandler(Func<JustCefWindow, string, string?, Task<string?>>? bridgeRpcHandler)
        {
            _bridgeRpcHandler = bridgeRpcHandler;
        }

        public void SetViewCreatedHandler(Func<JustCefView, Task>? viewCreatedHandler)
        {
            _viewCreatedHandler = viewCreatedHandler;
        }

        internal async Task<string?> InvokeBridgeRpcAsync(string method, string? json)
        {
            if (_bridgeRpcHandler == null)
                throw new InvalidOperationException("No bridge RPC handler is registered for this window.");

            return await _bridgeRpcHandler(this, method, json).ConfigureAwait(false);
        }

        internal async Task InvokeViewCreatedAsync(JustCefView view)
        {
            if (_viewCreatedHandler != null)
                await _viewCreatedHandler(view).ConfigureAwait(false);
        }
    }
}
