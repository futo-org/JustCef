//#define HARDCODED_PATHS

using System.Buffers;
using System.Buffers.Binary;
using System.Collections.Concurrent;
using System.Diagnostics;
using System.IO.Pipes;
using System.Net;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Runtime.Versioning;
using System.Text;
using System.Threading.Channels;
using Microsoft.Win32.SafeHandles;

namespace JustCef
{
    public class JustCefProcess : IDisposable
    {
        public enum PacketType : byte
        {
            Request = 0,
            Response = 1,
            Notification = 2,
            Cancel = 3
        }

        public enum OpcodeController : byte
        {
            Ping = 0,
            Print = 1,
            Echo = 2,
            WindowCreate = 3,
            //WindowCreatePositioned = 4,
            WindowSetDevelopmentToolsEnabled = 5,
            WindowLoadUrl = 6,
            //WindowLoadHtml = 7,
            //WindowExecuteJavascript = 8,
            WindowSetZoom = 9,
            //WindowSetResizable = 10,
            //WindowSetWindowless = 11,
            //WindowGetWindowSize = 12,
            //WindowSetWindowSize = 13,
            WindowGetPosition = 14,
            WindowSetPosition = 15,
            //WindowCenterWindow = 16,
            WindowMaximize = 17,
            WindowMinimize = 18,
            WindowRestore = 19,
            WindowShow = 20,
            WindowHide = 21,
            WindowClose = 22,
            //WindowSetRequestModificationEnabled = 23,
            //WindowModifyRequest = 24,
            WindowRequestFocus = 25,
            //WindowRegisterKeyboardListener = 26,
            //WindowSetTitle = 27,
            WindowActivate = 28,
            WindowBringToTop = 29,
            WindowSetAlwaysOnTop = 30,
            WindowSetFullscreen = 31,
            WindowCenterSelf = 32,
            WindowSetProxyRequests = 33,
            WindowSetModifyRequests = 34,
            //StreamOpen = 35,
            //StreamClose = 36,
            //StreamData = 37,
            //StreamCancel = 38,
            PickFile = 39,
            PickDirectory = 40,
            SaveFile = 41,
            WindowExecuteDevToolsMethod = 42,
            WindowSetDevelopmentToolsVisible = 43,
            WindowSetTitle = 44,
            WindowSetIcon = 45,
            WindowAddUrlToProxy = 46,
            WindowRemoveUrlToProxy = 47,
            WindowAddUrlToModify = 48,
            WindowRemoveUrlToModify = 49,
            WindowGetSize = 50,
            WindowSetSize = 51,
            WindowAddDevToolsEventMethod = 52,
            WindowRemoveDevToolsEventMethod = 53,
            WindowAddDomainToProxy = 54,
            WindowRemoveDomainToProxy = 55,
            WindowGetZoom = 56,
            WindowBridgeRpc = 57,
            //StreamEnd = 58,
            GetWidevineStatus = 59
        }

        public enum OpcodeControllerNotification : byte
        {
            Exit = 0,
            StreamData = 1,
            StreamEnd = 2,
            StreamError = 3
        }

        public enum OpcodeClient : byte
        {
            Ping = 0,
            Print = 1,
            Echo = 2,
            WindowProxyRequest = 3,
            WindowModifyRequest = 4,
            //StreamOpen = 5,
            //StreamData = 6,
            //StreamClose = 7,
            //StreamCancel = 8,
            WindowBridgeRpc = 9,
            //StreamEnd = 10,
            WindowViewCreated = 11
        }

        public enum OpcodeClientNotification : byte
        {
            Ready = 0,
            Exit = 1,
            WindowOpened = 2,
            WindowClosed = 3,
            //WindowResized = 4,
            WindowFocused = 5,
            WindowUnfocused = 6,
            //WindowMinimized = 7,
            //WindowMaximized = 8,
            //WindowRestored = 9,
            //WindowMoved = 10,
            //WindowKeyboardEvent = 11,
            WindowFullscreenChanged = 12,
            WindowFrameLoadStart = 13,
            WindowFrameLoadEnd = 14,
            WindowFrameLoadError = 15,
            WindowDevToolsEvent = 16,
            WindowLoadingStateChanged = 17,
            StreamCredit = 18,
            StreamCancel = 19
        }

        public enum ModifyTimeoutPolicy : byte
        {
            Continue = 0,
            Cancel = 1
        }

        private sealed class DeferredOutgoingStreams
        {
            private readonly List<Action> _starts = new();
            private Action? _abort;

            public bool HasAny => _starts.Count > 0;

            public void Add(Action start, Action cleanup)
            {
                _starts.Add(() =>
                {
                    try
                    {
                        start();
                    }
                    catch
                    {
                        cleanup();
                        throw;
                    }
                });
                _abort += cleanup;
            }

            public void StartAll()
            {
                _abort = null;

                foreach (var start in _starts)
                {
                    try
                    {
                        start();
                    }
                    catch (Exception e)
                    {
                        Logger.Error<JustCefProcess>("Failed to start deferred outgoing stream.", e);
                    }
                }

                _starts.Clear();
            }

            public void CleanupAll()
            {
                Action? abort = _abort;
                _abort = null;
                _starts.Clear();
                abort?.Invoke();
            }
        }

        private sealed class OutgoingStream
        {
            public readonly uint Identifier;
            public readonly CancellationTokenSource Cancellation = new CancellationTokenSource();
            private long _credit = StreamInitialCredit;
            private TaskCompletionSource? _creditAvailable;

            public OutgoingStream(uint identifier)
            {
                Identifier = identifier;
            }

            public void AddCredit(uint bytes)
            {
                TaskCompletionSource? creditAvailable;
                lock (this)
                {
                    _credit += bytes;
                    creditAvailable = _creditAvailable;
                    _creditAvailable = null;
                }

                creditAvailable?.TrySetResult();
            }

            public void SpendCredit(int bytes)
            {
                lock (this)
                    _credit -= bytes;
            }

            public async Task<int> WaitForCreditAsync(CancellationToken cancellationToken)
            {
                while (true)
                {
                    Task creditAvailable;
                    lock (this)
                    {
                        if (_credit > 0)
                            return (int)Math.Min(_credit, StreamChunkSize);

                        _creditAvailable ??= new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
                        creditAvailable = _creditAvailable.Task;
                    }

                    await creditAvailable.WaitAsync(cancellationToken).ConfigureAwait(false);
                }
            }
        }

        private sealed class PendingRequest
        {
            public readonly TaskCompletionSource<byte[]> Completion = new(TaskCreationOptions.RunContinuationsAsynchronously);
            public readonly byte Opcode;
            private readonly Action<PacketReader>? _onResponse;

            public PendingRequest(byte opcode, Action<PacketReader>? onResponse)
            {
                Opcode = opcode;
                _onResponse = onResponse;
            }

            public void Complete(byte[] body, int size)
            {
                byte[] payload;
                try
                {
                    var reader = new PacketReader(body, size);
                    var status = (JustCefStatus)reader.Read<byte>();
                    if (status != JustCefStatus.Ok)
                    {
                        Completion.TrySetException(new JustCefRemoteException(status, reader.RemainingSize >= sizeof(int) ? reader.ReadSizePrefixedString() : null));
                        return;
                    }

                    payload = reader.ReadBytes(reader.RemainingSize);
                    _onResponse?.Invoke(new PacketReader(payload));
                }
                catch (Exception e)
                {
                    Completion.TrySetException(e);
                    return;
                }

                Completion.TrySetResult(payload);
            }
        }

        private static ArrayPool<byte> BufferPool = ArrayPool<byte>.Create();
        private readonly TaskCompletionSource _readyTaskCompletionSource = new(TaskCreationOptions.RunContinuationsAsynchronously);

        private const int MaxIPCSize = 256 * 1024 * 1024;
        private const int HeaderSize = 4 + 4 + 1 + 1;
        private const uint ProtocolVersion = 2;
        private const int StreamChunkSize = 256 * 1024;
        private const int StreamInitialCredit = 1024 * 1024;
        private const long UnknownStreamLength = -1;
        private static readonly TimeSpan ExitWithoutEofGracePeriod = TimeSpan.FromMilliseconds(500);
        private static readonly TimeSpan ShutdownGracePeriod = TimeSpan.FromSeconds(5);
        private readonly AnonymousPipeServerStream _writer;
        private readonly AnonymousPipeServerStream _reader;
        private readonly Dictionary<uint, PendingRequest> _pendingRequests = new Dictionary<uint, PendingRequest>();
        private readonly HashSet<uint> _inflightRequests = new HashSet<uint>();
        private Process? _childProcess;
        private bool _started = false;
        private readonly BlockingCollection<RentedBuffer<byte>> _writeQueue = new BlockingCollection<RentedBuffer<byte>>();
        private Thread? _readerThread;
        private Thread? _writerThread;
        private uint _requestIdCounter = 0;
        private readonly List<JustCefBrowser> _browsers = new List<JustCefBrowser>();
        private volatile bool _shutdown;
        private int _disposed;
        private uint _streamIdentifierGenerator = 0;
        private readonly Dictionary<uint, OutgoingStream> _outgoingStreams = new Dictionary<uint, OutgoingStream>();
        private readonly Channel<Action> _events = Channel.CreateUnbounded<Action>(new UnboundedChannelOptions { SingleReader = true });
        private EventHandler? _onProcessExit;
        private const int MaxRememberedClosedBrowsers = 256;
        private readonly Queue<int> _closedBrowsers = new();
        private readonly TaskCompletionSource _exitTaskCompletionSource = new(TaskCreationOptions.RunContinuationsAsynchronously);

        private const int CefResultCodeProfileInUse = 21;
        private const int CefResultCodeNormalExitProcessNotified = 24;

        private void SignalStartupFailed(int exitCode)
        {
            if (_readyTaskCompletionSource.Task.IsCompleted)
                return;

            var failure = exitCode switch
            {
                CefResultCodeProfileInUse => JustCefStartupFailure.ProfileInUse,
                CefResultCodeNormalExitProcessNotified => JustCefStartupFailure.ProcessNotified,
                _ => JustCefStartupFailure.Unknown
            };

            var message = failure switch
            {
                JustCefStartupFailure.ProfileInUse => "Another process is already using the root cache path.",
                JustCefStartupFailure.ProcessNotified => "The launch arguments were forwarded to the process that already owns the root cache path.",
                _ => "The native process exited before it became ready."
            };

            Logger.Info<JustCefProcess>(message);
            _readyTaskCompletionSource.TrySetException(new JustCefStartupException(exitCode, failure, message));
        }

        private void SignalExited()
        {
            _exitTaskCompletionSource.TrySetResult();
        }

        public List<JustCefWindow> Windows
        {
            get
            {
                lock (_browsers)
                {
                    return _browsers.OfType<JustCefWindow>().ToList();
                }
            }
        }

        public JustCefWindow? GetWindow(int identifier) => GetBrowser(identifier) as JustCefWindow;

        public JustCefBrowser? GetBrowser(int identifier)
        {
            lock (_browsers)
            {
                return _browsers.FirstOrDefault(v => v.Identifier == identifier);
            }
        }

        internal List<JustCefView> GetViews(JustCefWindow parent)
        {
            lock (_browsers)
            {
                return _browsers.OfType<JustCefView>().Where(v => v.Parent == parent).ToList();
            }
        }

        private bool RegisterBrowser(JustCefBrowser browser)
        {
            lock (_browsers)
            {
                if (!_shutdown && !_closedBrowsers.Contains(browser.Identifier) &&
                    !_browsers.Any(existing => existing.Identifier == browser.Identifier) &&
                    (browser is not JustCefView view || _browsers.Contains(view.Parent)))
                {
                    _browsers.Add(browser);
                    return true;
                }
            }

            browser.InvokeOnClose();
            return false;
        }

        private void RemoveBrowser(JustCefBrowser browser)
        {
            bool removed;
            lock (_browsers)
            {
                removed = _browsers.Remove(browser);
            }

            if (removed)
                browser.InvokeOnClose();
        }

        public bool HasExited
        {
            get
            {
                try
                {
                    return _childProcess?.HasExited ?? true;
                }
                catch
                {
                    return true;
                }
            }
        }

        public TimeSpan DefaultCallTimeout { get; set; } = TimeSpan.FromSeconds(30);

        internal string? NativeExecutablePath { get; set; }
        internal Process? ChildProcess => _childProcess;
        internal int ReaderThreadId => _readerThread?.ManagedThreadId ?? 0;
        internal int WriterThreadId => _writerThread?.ManagedThreadId ?? 0;
        internal SafePipeHandle WriterClientHandle => _writer.ClientSafePipeHandle;
        internal SafePipeHandle ReaderClientHandle => _reader.ClientSafePipeHandle;

        internal int PendingCallCount
        {
            get
            {
                lock (_pendingRequests)
                    return _pendingRequests.Count;
            }
        }

        internal int OpenStreamCount
        {
            get
            {
                lock (_outgoingStreams)
                    return _outgoingStreams.Count;
            }
        }

        internal int InflightHandlerCount
        {
            get
            {
                lock (_inflightRequests)
                    return _inflightRequests.Count;
            }
        }

        public JustCefProcess()
        {
            //var writer = new AnonymousPipeServerStream(PipeDirection.Out, HandleInheritability.None);
            //writer.Dispose();
            _writer = new AnonymousPipeServerStream(PipeDirection.Out, HandleInheritability.Inheritable);

            //var reader = new AnonymousPipeServerStream(PipeDirection.In, HandleInheritability.None);
            //reader.Dispose();
            _reader = new AnonymousPipeServerStream(PipeDirection.In, HandleInheritability.Inheritable);

            _onProcessExit = (_, _) =>
            {
                _writer.Dispose();
                _reader.Dispose();
                _childProcess?.Dispose();
            };
            AppDomain.CurrentDomain.ProcessExit += _onProcessExit;

            _ = Task.Run(DispatchEventsAsync);
        }

        public void Start(string? args = null)
        {
            if (_started)
                throw new Exception("Already started.");

            _started = true;
            
#if !HARDCODED_PATHS
            string? nativePath = null;
            string[] searchPaths = NativeExecutablePath != null ? [NativeExecutablePath] : GenerateSearchPaths();
            Logger.Info<JustCefProcess>("Searching for justcefnative, search paths:");
            foreach (var path in searchPaths)
                Logger.Info<JustCefProcess>(" - " + path);

            foreach (string path in searchPaths)
            {
                
                if (File.Exists(path))
                    nativePath = path;                    
            }

            if (nativePath == null)
                throw new Exception("Failed to find justcefnative");

            var workingDirectory = GetDirectory(nativePath);
            Logger.Info<JustCefProcess>($"Working directory '{workingDirectory}'.");
            Logger.Info<JustCefProcess>($"CEF exe path '{nativePath}'.");

            if (!File.Exists(nativePath))
            {
                Logger.Error<JustCefProcess>($"File not found at native path '{nativePath}'.");
                throw new Exception("Native executable not found.");
            }
#else
            Logger.Info<JustCefProcess>($"USING HARDCODED PATHS.");
#endif

            ProcessStartInfo psi = new ProcessStartInfo
            {
#if HARDCODED_PATHS
                FileName = OperatingSystem.IsMacOS()
                    ? "/Users/koen/Projects/Grayjay.Desktop/JustCef/native/build/Debug/justcefnative.app/Contents/MacOS/justcefnative"
                    : OperatingSystem.IsWindows() 
                        ? """C:\Users\Koen\Projects\Grayjay.Desktop\JustCef\native\build\Release\justcefnative.exe"""
                        : "/home/koen/Projects/JustCef/native/build/Debug/justcefnative",
                WorkingDirectory = OperatingSystem.IsMacOS()
                    ? "/Users/koen/Projects/Grayjay.Desktop/JustCef/native/build/Debug/"
                    : OperatingSystem.IsWindows() 
                        ? """C:\Users\Koen\Projects\Grayjay.Desktop\JustCef\native\build\Release\"""
                        : "/home/koen/Projects/JustCef/native/build/Debug",
#else
                FileName = nativePath,
                WorkingDirectory = workingDirectory,
#endif   
                Arguments = $"--change-stack-guard-on-fork=disable --parent-to-child {_writer.GetClientHandleAsString()} --child-to-parent {_reader.GetClientHandleAsString()}" + ((string.IsNullOrEmpty(args)) ? "" : " " + args),
                UseShellExecute = false,
                RedirectStandardError = true,
                RedirectStandardOutput = true
            };

            Logger.Info<JustCefProcess>(psi.Arguments);

            var process = new Process();
            process.StartInfo = psi;
            process.EnableRaisingEvents = true;
            process.Exited += (_, _) =>
            {
                int exitCode;
                try
                {
                    exitCode = process.ExitCode;
                }
                catch
                {
                    exitCode = -1;
                }

                Logger.Info<JustCefProcess>($"Child process exited with code {exitCode}.");
                SignalStartupFailed(exitCode);
                SignalExited();
                _ = Task.Delay(ExitWithoutEofGracePeriod).ContinueWith(_ => Shutdown(), TaskScheduler.Default);
            };
            process.ErrorDataReceived += (_, args) =>
            {
                var d = args?.Data;
                if (d != null)
                    Logger.Info<JustCefProcess>(d);
            };
            process.OutputDataReceived += (_, args) =>
            {
                var d = args?.Data;
                if (d != null)
                    Logger.Info<JustCefProcess>(d);
            };

            if (!process.Start())
                throw new Exception("Failed to start process.");

            process.BeginOutputReadLine();
            process.BeginErrorReadLine();
            _childProcess = process;

            _writer.DisposeLocalCopyOfClientHandle();
            _reader.DisposeLocalCopyOfClientHandle();

            StartConnection();
        }

        internal void StartConnection()
        {
            _started = true;

            _readerThread = new Thread(() =>
            {
                try
                {
                    Logger.Info<JustCefProcess>("Receive loop started.");

                    byte[] headerBuffer = new byte[HeaderSize];

                    while (true)
                    {
                        _reader.ReadExactly(headerBuffer, 0, HeaderSize);

                        var size = BitConverter.ToUInt32(headerBuffer, 0);
                        var requestId = BitConverter.ToUInt32(headerBuffer, 4);
                        var packetType = (PacketType)headerBuffer[8];
                        var opcode = headerBuffer[9];

                        int bodySize = (int)size + 4 - HeaderSize;
                        if (bodySize < 0 || bodySize > MaxIPCSize)
                        {
                            Logger.Error<JustCefProcess>("Invalid packet size. Shutting down.");
                            return;
                        }

                        RentedBuffer<byte>? rentedBodyBuffer = null;
                        if (bodySize > 0)
                        {
                            var rb = new RentedBuffer<byte>(BufferPool, bodySize);
                            try
                            {
                                _reader.ReadExactly(rb.Buffer, 0, bodySize);
                                rentedBodyBuffer = rb;
                            }
                            catch
                            {
                                rb.Dispose();
                                throw;
                            }
                        }

                        async Task RunPacket()
                        {
                            try
                            {
                                if (packetType == PacketType.Response)
                                {
                                    bool foundPendingRequest;
                                    PendingRequest? pendingRequest;
                                    lock (_pendingRequests)
                                    {
                                        foundPendingRequest = _pendingRequests.TryGetValue(requestId, out pendingRequest);
                                        if (foundPendingRequest && pendingRequest != null && pendingRequest.Opcode != opcode)
                                        {
                                            foundPendingRequest = false;
                                            pendingRequest = null;
                                            Logger.Error<JustCefProcess>($"Received a response with opcode {opcode} for request id {requestId}, which used a different opcode.");
                                        }
                                        else if (foundPendingRequest)
                                            _pendingRequests.Remove(requestId);
                                    }

                                    if (foundPendingRequest && pendingRequest != null)
                                        pendingRequest.Complete(rentedBodyBuffer != null ? rentedBodyBuffer.Buffer : Array.Empty<byte>(), rentedBodyBuffer != null ? rentedBodyBuffer.Length : 0);
                                    else
                                        Logger.Error<JustCefProcess>($"Received a packet response for a request that no longer has an awaiter (request id = {requestId}).");
                                }
                                else if (packetType == PacketType.Request)
                                {
                                    var packetReader = new PacketReader(rentedBodyBuffer != null ? rentedBodyBuffer.Buffer : Array.Empty<byte>(), rentedBodyBuffer != null ? rentedBodyBuffer.Length : 0);
                                    var packetWriter = new PacketWriter();
                                    var deferredOutgoingStreams = new DeferredOutgoingStreams();
                                    JustCefStatus status;
                                    string? message = null;
                                    try
                                    {
                                        status = await HandleRequestAsync((OpcodeClient)opcode, packetReader, packetWriter, deferredOutgoingStreams).ConfigureAwait(false);
                                    }
                                    catch (Exception e)
                                    {
                                        Logger.Error<JustCefProcess>($"An exception occurred in the IPC while handling request packet", e);
                                        deferredOutgoingStreams.CleanupAll();
                                        status = JustCefStatus.Error;
                                        message = e.Message;
                                    }

                                    if (status == JustCefStatus.Ok && packetWriter.Size >= MaxIPCSize)
                                    {
                                        deferredOutgoingStreams.CleanupAll();
                                        status = JustCefStatus.TooLarge;
                                    }

                                    if (status != JustCefStatus.Ok)
                                    {
                                        packetWriter.Dispose();
                                        packetWriter = new PacketWriter().WriteSizePrefixedString(message);
                                    }

                                    try
                                    {
                                        if (!CompleteInflightRequest(requestId) || !SendResponse(requestId, opcode, status, packetWriter))
                                        {
                                            deferredOutgoingStreams.CleanupAll();
                                            return;
                                        }

                                        deferredOutgoingStreams.StartAll();
                                    }
                                    finally
                                    {
                                        packetWriter.Dispose();
                                    }
                                }
                                else if (packetType == PacketType.Notification)
                                {
                                    var packetReader = new PacketReader(rentedBodyBuffer != null ? rentedBodyBuffer.Buffer : Array.Empty<byte>(), rentedBodyBuffer != null ? rentedBodyBuffer.Length : 0);
                                    HandleNotification((OpcodeClientNotification)opcode, packetReader);
                                }
                                else if (packetType == PacketType.Cancel)
                                {
                                    if (CompleteInflightRequest(requestId))
                                    {
                                        using var packetWriter = new PacketWriter().WriteSizePrefixedString(null);
                                        SendResponse(requestId, opcode, JustCefStatus.Canceled, packetWriter);
                                    }
                                }
                            }
                            catch (Exception e)
                            {
                                Logger.Error<JustCefProcess>($"An exception occurred in the IPC while handling a packet", e);
                            }
                            finally
                            {
                                rentedBodyBuffer?.Dispose();
                            }
                        }

                        if (packetType == PacketType.Request)
                        {
                            bool accepted;
                            lock (_inflightRequests)
                                accepted = _inflightRequests.Add(requestId);

                            if (!accepted)
                            {
                                Logger.Error<JustCefProcess>($"Received a duplicate request id {requestId} from native.");
                                SendResponse(requestId, opcode, JustCefStatus.InvalidRequest, new PacketWriter());
                            }
                            else
                                _ = Task.Run(RunPacket);
                        }
                        else
                            RunPacket().GetAwaiter().GetResult();
                    }
                }
                catch (EndOfStreamException)
                {
                    Logger.Info<JustCefProcess>("IPC pipe closed.");
                }
                catch (Exception e)
                {
                    Logger.Error<JustCefProcess>($"An exception occurred in the IPC", e);
                }
                finally
                {
                    Logger.Info<JustCefProcess>("Receive loop stopped.");
                    _writeQueue.CompleteAdding();
                    Shutdown();
                    _reader.Dispose();
                }
            }) { IsBackground = true, Name = "JustCef IPC reader" };
            _writerThread = new Thread(WriteLoop) { IsBackground = true, Name = "JustCef IPC writer" };
            _readerThread.Start();
            _writerThread.Start();
        }

        private async Task<JustCefStatus> HandleRequestAsync(OpcodeClient opcode, PacketReader reader, PacketWriter writer, DeferredOutgoingStreams deferredOutgoingStreams)
        {
            switch (opcode)
            {
                case OpcodeClient.Ping:
                    break;
                case OpcodeClient.Print:
                    Logger.Info<JustCefProcess>(reader.ReadString(reader.RemainingSize));
                    break;
                case OpcodeClient.Echo:
                    writer.WriteBytes(reader.ReadBytes(reader.RemainingSize));
                    break;
                case OpcodeClient.WindowProxyRequest:
                    return await HandleWindowProxyRequestAsync(reader, writer, deferredOutgoingStreams).ConfigureAwait(false);
                case OpcodeClient.WindowModifyRequest:
                    return HandleWindowModifyRequest(reader, writer);
                case OpcodeClient.WindowBridgeRpc:
                    return await HandleWindowBridgeRpcAsync(reader, writer).ConfigureAwait(false);
                case OpcodeClient.WindowViewCreated:
                    return await HandleWindowViewCreatedAsync(reader, writer).ConfigureAwait(false);
                default:
                    Logger.Warning<JustCefProcess>($"Received unhandled opcode {opcode}.");
                    return JustCefStatus.Unsupported;
            }

            return JustCefStatus.Ok;
        }

        private void HandleClientStreamCredit(PacketReader reader)
        {
            uint identifier = reader.Read<uint>();
            uint bytes = reader.Read<uint>();
            lock (_outgoingStreams)
            {
                if (_outgoingStreams.TryGetValue(identifier, out var stream))
                    stream.AddCredit(bytes);
            }
        }

        private void HandleClientStreamCancel(PacketReader reader)
        {
            uint identifier = reader.Read<uint>();
            lock (_outgoingStreams)
            {
                if (_outgoingStreams.TryGetValue(identifier, out var stream))
                {
                    _ = stream.Cancellation.CancelAsync();
                    _outgoingStreams.Remove(identifier);
                }
            }
        }

        private async Task<JustCefStatus> HandleWindowProxyRequestAsync(PacketReader reader, PacketWriter writer, DeferredOutgoingStreams deferredOutgoingStreams)
        {
            int identifier = reader.Read<int>();
            var window = GetBrowser(identifier);
            if (window == null)
                return JustCefStatus.NotFound;

            string method = reader.ReadSizePrefixedString()!;
            string url = reader.ReadSizePrefixedString()!;

            // Deserialize headers
            int headerCount = reader.Read<int>();
            var headers = new Dictionary<string, List<string>>(StringComparer.InvariantCultureIgnoreCase);

            for (int i = 0; i < headerCount; i++)
            {
                string key = reader.ReadSizePrefixedString()!;
                string value = reader.ReadSizePrefixedString()!;
                if (headers.TryGetValue(key, out var v))
                    v.Add(value);
                else
                    headers[key] = new List<string>([ value ]);
            }

            // Deserialize elements
            var elements = DeserializeBodyElements(reader);

            IPCResponse? response = null;
            bool transferred = false;
            try
            {
                response = await window.ProxyRequestAsync(new IPCRequest
                {
                    Method = method,
                    Url = url,
                    Headers = headers,
                    Elements = elements,
                }).ConfigureAwait(false);

                if (response == null)
                    return JustCefStatus.NotHandled;

                if (response.Body != null && response.DataSource != null)
                    throw new InvalidOperationException("IPCResponse cannot define both Body and DataSource.");

                var responseHeaders = response.Headers
                    .SelectMany(header => header.Value
                        .Where(value =>
                            !(string.Equals(header.Key, "transfer-encoding", StringComparison.InvariantCultureIgnoreCase) &&
                              string.Equals(value, "chunked", StringComparison.InvariantCultureIgnoreCase)))
                        .Select(value => new KeyValuePair<string, string>(header.Key, value)))
                    .ToList();

                writer.Write((uint)response.StatusCode);
                writer.WriteSizePrefixedString(response.StatusText);

                // Serialize headers
                writer.Write(responseHeaders.Count);
                foreach (var header in responseHeaders)
                {
                    writer.WriteSizePrefixedString(header.Key);
                    writer.WriteSizePrefixedString(header.Value);
                }

                long? contentLength = null;
                if (response.Headers.TryGetValue("content-length", out var contentLengths) &&
                    contentLengths.Count > 0 &&
                    long.TryParse(contentLengths[0], out long parsedContentLength) &&
                    parsedContentLength >= 0)
                {
                    contentLength = parsedContentLength;
                }

                if (response.Body != null)
                {
                    writer.Write((byte)1);
                    writer.Write((uint)response.Body.Length);
                    writer.WriteBytes(response.Body);
                }
                else if (response.DataSource != null)
                {
                    writer.Write((byte)2);
                    writer.Write(contentLength ?? UnknownStreamLength);
                    HandleLargeOrChunkedContent(response.DataSource, writer, deferredOutgoingStreams, contentLength);
                    transferred = true;
                }
                else
                    writer.Write((byte)0);

                return JustCefStatus.Ok;
            }
            finally
            {
                if (response?.DataSource != null && !transferred)
                    DisposeQuietly(response.DataSource);
            }
        }

        private void HandleLargeOrChunkedContent(IDataSource dataSource, PacketWriter writer, DeferredOutgoingStreams deferredOutgoingStreams, long? contentLength = null)
        {
            AddDeferredOutgoingStream(
                writer,
                deferredOutgoingStreams,
                async (stream, cancellationToken) =>
                {
                    byte[] buffer = ArrayPool<byte>.Shared.Rent(sizeof(uint) + StreamChunkSize);
                    long totalBytesRead = 0;
                    try
                    {
                        BinaryPrimitives.WriteUInt32LittleEndian(buffer, stream.Identifier);
                        while (!contentLength.HasValue || totalBytesRead < contentLength.Value)
                        {
                            int requestedBytes = await stream.WaitForCreditAsync(cancellationToken).ConfigureAwait(false);
                            if (contentLength.HasValue)
                                requestedBytes = (int)Math.Min(requestedBytes, contentLength.Value - totalBytesRead);

                            int bytesRead = await dataSource.ReadAsync(new Memory<byte>(buffer, sizeof(uint), requestedBytes), cancellationToken).ConfigureAwait(false);
                            if (bytesRead <= 0)
                            {
                                ThrowIfEndedBeforeExpectedLength(totalBytesRead, contentLength, "proxy response body");
                                break;
                            }

                            cancellationToken.ThrowIfCancellationRequested();

                            stream.SpendCredit(bytesRead);
                            if (!Notify(OpcodeControllerNotification.StreamData, buffer, 0, sizeof(uint) + bytesRead))
                                return;

                            totalBytesRead += bytesRead;
                        }

                        Notify(OpcodeControllerNotification.StreamEnd, new PacketWriter().Write(stream.Identifier).Write((ulong)totalBytesRead));
                    }
                    finally
                    {
                        ArrayPool<byte>.Shared.Return(buffer);
                    }
                },
                () => DisposeQuietly(dataSource));
        }

        private static void ThrowIfEndedBeforeExpectedLength(long totalBytesRead, long? expectedLength, string description)
        {
            if (expectedLength.HasValue && totalBytesRead < expectedLength.Value)
            {
                throw new EndOfStreamException($"Stream for {description} ended after {totalBytesRead} bytes, expected {expectedLength.Value}.");
            }
        }

        private List<IPCProxyBodyElement> DeserializeBodyElements(PacketReader reader)
        {
            uint elementCount = reader.Read<uint>();
            var elements = new List<IPCProxyBodyElement>((int)elementCount);

            for (uint i = 0; i < elementCount; i++)
            {
                IPCProxyBodyElementType elementType = (IPCProxyBodyElementType)reader.Read<byte>();
                if (elementType == IPCProxyBodyElementType.Bytes)
                {
                    uint dataSize = reader.Read<uint>();
                    byte[] data = reader.ReadBytes((int)dataSize);
                    elements.Add(new IPCProxyBodyElementBytes(data));
                }
                else if (elementType == IPCProxyBodyElementType.File)
                {
                    string fileName = reader.ReadSizePrefixedString()!;
                    elements.Add(new IPCProxyBodyElementFile(fileName));
                }
                else
                    throw new InvalidOperationException($"Unknown proxy body element type '{elementType}'.");
            }

            return elements;
        }

        private void SerializeBodyElements(PacketWriter writer, IReadOnlyList<IPCProxyBodyElement> elements)
        {
            writer.Write((uint)elements.Count);
            foreach (var element in elements)
            {
                switch (element)
                {
                    case IPCProxyBodyElementBytes bytesElement:
                        writer.Write((byte)IPCProxyBodyElementType.Bytes);
                        writer.Write((uint)bytesElement.Data.Length);
                        writer.WriteBytes(bytesElement.Data);
                        break;
                    case IPCProxyBodyElementFile fileElement:
                        writer.Write((byte)IPCProxyBodyElementType.File);
                        writer.WriteSizePrefixedString(fileElement.FileName);
                        break;
                    default:
                        throw new InvalidOperationException($"Unsupported proxy body element type '{element.GetType().Name}'.");
                }
            }
        }

        private void AddDeferredOutgoingStream(
            PacketWriter writer,
            DeferredOutgoingStreams deferredOutgoingStreams,
            Func<OutgoingStream, CancellationToken, Task> transferAsync,
            Action? additionalCleanup = null)
        {
            uint streamIdentifier = Interlocked.Increment(ref _streamIdentifierGenerator);
            if (streamIdentifier == 0)
                streamIdentifier = Interlocked.Increment(ref _streamIdentifierGenerator);

            var stream = new OutgoingStream(streamIdentifier);
            lock (_outgoingStreams)
            {
                _outgoingStreams[streamIdentifier] = stream;
                if (_shutdown)
                    _ = stream.Cancellation.CancelAsync();
            }

            int cleanupState = 0;
            void Cleanup()
            {
                if (Interlocked.Exchange(ref cleanupState, 1) != 0)
                    return;

                lock (_outgoingStreams)
                {
                    if (_outgoingStreams.TryGetValue(streamIdentifier, out var current) && current == stream)
                        _outgoingStreams.Remove(streamIdentifier);
                }

                if (additionalCleanup != null)
                {
                    try
                    {
                        additionalCleanup();
                    }
                    catch
                    {
                    }
                }
            }

            try
            {
                writer.Write(streamIdentifier);
            }
            catch
            {
                Cleanup();
                throw;
            }

            void Start()
            {
                if (stream.Cancellation.IsCancellationRequested)
                {
                    Cleanup();
                    return;
                }

                _ = Task.Run(async () =>
                {
                    try
                    {
                        await transferAsync(stream, stream.Cancellation.Token).ConfigureAwait(false);
                    }
                    catch (OperationCanceledException) when (stream.Cancellation.IsCancellationRequested)
                    {
                    }
                    catch (Exception e)
                    {
                        Logger.Error<JustCefProcess>($"Failed to stream body", e);
                        if (!stream.Cancellation.IsCancellationRequested)
                            Notify(OpcodeControllerNotification.StreamError, new PacketWriter().Write(streamIdentifier).WriteSizePrefixedString(e.Message));
                    }
                    finally
                    {
                        Cleanup();
                    }
                });
            }

            deferredOutgoingStreams.Add(Start, Cleanup);
        }

        private static void DisposeQuietly(IDisposable disposable)
        {
            try
            {
                disposable.Dispose();
            }
            catch
            {
            }
        }

        private JustCefStatus HandleWindowModifyRequest(PacketReader reader, PacketWriter writer)
        {
            var identifier = reader.Read<int>();
            var window = GetBrowser(identifier);
            if (window == null)
                return JustCefStatus.NotFound;

            string method = reader.ReadSizePrefixedString()!;
            string url = reader.ReadSizePrefixedString()!;

            // Deserialize headers
            int headerCount = reader.Read<int>();
            var headers = new Dictionary<string, List<string>>(StringComparer.InvariantCultureIgnoreCase);
            for (int i = 0; i < headerCount; i++)
            {
                string key = reader.ReadSizePrefixedString()!;
                string value = reader.ReadSizePrefixedString()!;
                if (headers.TryGetValue(key, out var v))
                    v.Add(value);
                else
                    headers[key] = new List<string>([ value ]);
            }

            // Deserialize elements
            var elements = DeserializeBodyElements(reader);

            var modifiedRequest = window.ModifyRequest(new IPCRequest
            {
                Method = method,
                Url = url,
                Headers = headers,
                Elements = elements,
            });

            if (modifiedRequest == null)
                return JustCefStatus.NotHandled;

            writer.WriteSizePrefixedString(modifiedRequest.Method);
            writer.WriteSizePrefixedString(modifiedRequest.Url);

            // Serialize headers
            var modifiedHeaders = modifiedRequest.Headers
                .SelectMany(header => header.Value.Select(value => new KeyValuePair<string, string>(header.Key, value)))
                .ToList();

            writer.Write(modifiedHeaders.Count);
            foreach (var header in modifiedHeaders)
            {
                writer.WriteSizePrefixedString(header.Key);
                writer.WriteSizePrefixedString(header.Value);
            }

            SerializeBodyElements(writer, modifiedRequest.Elements);
            return JustCefStatus.Ok;
        }

        private async Task<JustCefStatus> HandleWindowBridgeRpcAsync(PacketReader reader, PacketWriter writer)
        {
            int identifier = reader.Read<int>();
            var window = GetWindow(identifier);
            if (window == null)
                return JustCefStatus.NotFound;

            string? method = reader.ReadSizePrefixedString();
            if (string.IsNullOrWhiteSpace(method))
                throw new InvalidOperationException("Bridge RPC method must be a non-empty string.");

            string json = reader.ReadString((int)reader.Read<uint>());
            string? resultJson = await window.InvokeBridgeRpcAsync(method, json).ConfigureAwait(false);
            byte[] resultBytes = Encoding.UTF8.GetBytes(resultJson ?? "null");
            writer.Write((uint)resultBytes.Length);
            writer.WriteBytes(resultBytes);
            return JustCefStatus.Ok;
        }

        private async Task<JustCefStatus> HandleWindowViewCreatedAsync(PacketReader reader, PacketWriter writer)
        {
            int parentIdentifier = reader.Read<int>();
            int viewIdentifier = reader.Read<int>();
            string? src = reader.ReadSizePrefixedString();

            var parent = GetWindow(parentIdentifier);
            if (parent == null)
            {
                Logger.Warning<JustCefProcess>($"View {viewIdentifier} was created for unknown parent window {parentIdentifier}.");
                return JustCefStatus.NotFound;
            }

            var view = new JustCefView(this, viewIdentifier, parent);
            if (!RegisterBrowser(view))
            {
                writer.Write(false);
                return JustCefStatus.Ok;
            }

            try
            {
                await parent.InvokeViewCreatedAsync(view).ConfigureAwait(false);
                writer.Write(true);
            }
            catch (Exception e)
            {
                Logger.Error<JustCefProcess>($"Exception occurred while configuring view {viewIdentifier} ({src}).", e);
                RemoveBrowser(view);
                writer.Write(false);
            }

            return JustCefStatus.Ok;
        }

        private void HandleNotification(OpcodeClientNotification opcode, PacketReader reader)
        {
            Logger.Info<JustCefProcess>($"Received notification {opcode}");

            switch (opcode)
            {
                case OpcodeClientNotification.Exit:
                    Logger.Info<JustCefProcess>("CEF process is exiting.");
                    Shutdown();
                    break;
                case OpcodeClientNotification.Ready:
                {
                    uint protocolVersion = reader.Read<uint>();
                    if (protocolVersion != ProtocolVersion)
                    {
                        string message = $"The native process speaks IPC protocol version {protocolVersion}, expected {ProtocolVersion}.";
                        Logger.Error<JustCefProcess>(message);
                        _readyTaskCompletionSource.TrySetException(new JustCefStartupException(-1, JustCefStartupFailure.ProtocolMismatch, message));
                        break;
                    }

                    Logger.Info<JustCefProcess>("Client is ready.");
                    _readyTaskCompletionSource.TrySetResult();
                    break;
                }
                case OpcodeClientNotification.WindowOpened:
                    Logger.Info<JustCefProcess>($"Window opened: {reader.Read<int>()}");
                    break;
                case OpcodeClientNotification.WindowClosed:
                    {
                        JustCefBrowser? window;
                        lock (_browsers)
                        {
                            var identifier = reader.Read<int>();
                            window = _browsers.FirstOrDefault(v => v.Identifier == identifier);
                            if (window != null)
                            {
                                _browsers.Remove(window);
                            }

                            _closedBrowsers.Enqueue(identifier);
                            while (_closedBrowsers.Count > MaxRememberedClosedBrowsers)
                                _closedBrowsers.Dequeue();
                        }

                        Logger.Info<JustCefProcess>($"Window closed: {window}");
                        window?.InvokeOnClose(true);
                        break;
                    }
                case OpcodeClientNotification.WindowFocused:
                    GetBrowser(reader.Read<int>())?.InvokeOnFocused();
                    break;
                case OpcodeClientNotification.WindowUnfocused:
                    GetBrowser(reader.Read<int>())?.InvokeOnUnfocused();
                    break;
                case OpcodeClientNotification.WindowFullscreenChanged:
                {
                    int identifier = reader.Read<int>();
                    bool fullscreen = reader.Read<bool>();
                    GetBrowser(identifier)?.InvokeOnFullscreenChanged(fullscreen);
                    break;
                }
                case OpcodeClientNotification.WindowFrameLoadStart:
                {
                    int identifier = reader.Read<int>();
                    string? frameIdentifier = reader.ReadSizePrefixedString();
                    bool isMainFrame = reader.Read<bool>();
                    string? url = reader.ReadSizePrefixedString();
                    GetBrowser(identifier)?.InvokeOnFrameLoadStart(frameIdentifier, isMainFrame, url);
                    //Logger.Info<JustCefProcess>($"WindowFrameLoadStart (frameIdentifier = {frameIdentifier}, isMainFrame = {isMainFrame}, url = {url}).");
                    break;
                }
                case OpcodeClientNotification.WindowFrameLoadEnd:
                {
                    int identifier = reader.Read<int>();
                    string? frameIdentifier = reader.ReadSizePrefixedString();
                    bool isMainFrame = reader.Read<bool>();
                    string? url = reader.ReadSizePrefixedString();
                    int httpStatusCode = reader.Read<int>();
                    GetBrowser(identifier)?.InvokeOnFrameLoadEnd(frameIdentifier, isMainFrame, url, httpStatusCode);
                    //Logger.Info<JustCefProcess>($"WindowFrameLoadEnd (frameIdentifier = {frameIdentifier}, isMainFrame = {isMainFrame}, url = {url}, httpStatusCode = {httpStatusCode}).");
                    break;
                }
                case OpcodeClientNotification.WindowFrameLoadError:
                {
                    int identifier = reader.Read<int>();
                    string? frameIdentifier = reader.ReadSizePrefixedString();
                    bool isMainFrame = reader.Read<bool>();
                    int errorCode = reader.Read<int>();
                    string? errorText = reader.ReadSizePrefixedString();
                    string? failedUrl = reader.ReadSizePrefixedString();
                    GetBrowser(identifier)?.InvokeOnFrameLoadError(frameIdentifier, isMainFrame, errorCode, errorText, failedUrl);
                    //Logger.Info<JustCefProcess>($"WindowFrameLoadError (frameIdentifier = {frameIdentifier}, isMainFrame = {isMainFrame}, failedUrl = {failedUrl}, errorCode = {errorCode}, errorText = {errorText}).");
                    break;
                }
                case OpcodeClientNotification.WindowLoadingStateChanged:
                {
                    int identifier = reader.Read<int>();
                    bool isLoading = reader.Read<bool>();
                    bool canGoBack = reader.Read<bool>();
                    bool canGoForward = reader.Read<bool>();
                    //Logger.Info<JustCefProcess>($"LoadingStateChanged (isLoading = {isLoading}).");
                    GetBrowser(identifier)?.InvokeOnLoadingStateChanged(isLoading, canGoBack, canGoForward);
                    break;
                }
                case OpcodeClientNotification.WindowDevToolsEvent:
                {
                    int identifier = reader.Read<int>();
                    string? method = reader.ReadSizePrefixedString();
                    var parameters = reader.ReadBytes((int)reader.Read<uint>());
                    GetBrowser(identifier)?.InvokeOnDevToolsEvent(method, parameters);
                    break;
                }
                case OpcodeClientNotification.StreamCredit:
                    HandleClientStreamCredit(reader);
                    break;
                case OpcodeClientNotification.StreamCancel:
                    HandleClientStreamCancel(reader);
                    break;
                default:
                    Logger.Info<JustCefProcess>($"Received unhandled notification opcode {opcode}.");
                    break;
            }
        }

        private static RentedBuffer<byte> RentedBytesFromStruct<TStruct>(TStruct s) where TStruct : struct
        {
            var span = MemoryMarshal.AsBytes(MemoryMarshal.CreateReadOnlySpan(ref s, 1));
            var buffer = new RentedBuffer<byte>(BufferPool, span.Length);
            span.CopyTo(buffer.Buffer);
            return buffer;
        }

        public static TStruct BytesToStruct<TStruct>(byte[] bytes) where TStruct : struct
        {
            return MemoryMarshal.Read<TStruct>(new ReadOnlySpan<byte>(bytes));
        }
        
        public async Task<PacketReader> CallAsync(OpcodeController opcode, PacketWriter writer, CancellationToken cancellationToken = default)
        {
            return await CallAsync(opcode, writer, DefaultCallTimeout, cancellationToken).ConfigureAwait(false);
        }

        private async Task<PacketReader> CallAsync(OpcodeController opcode, PacketWriter writer, TimeSpan timeout, CancellationToken cancellationToken = default, Action<PacketReader>? onResponse = null)
        {
            try
            {
                return await CallAsync(opcode, writer.Data, 0, writer.Size, timeout, cancellationToken, onResponse).ConfigureAwait(false);
            }
            finally
            {
                writer.Dispose();
            }
        }

        public async Task<PacketReader> CallAsync<TRequest>(OpcodeController opcode, TRequest request, CancellationToken cancellationToken = default)
            where TRequest : struct
        {
            using var requestBody = RentedBytesFromStruct(request);
            return await CallAsync(opcode, requestBody.Buffer, 0, requestBody.Length, cancellationToken).ConfigureAwait(false);
        }

        public async Task<TResult> CallAsync<TRequest, TResult>(OpcodeController opcode, TRequest request, CancellationToken cancellationToken = default)
            where TRequest : unmanaged
            where TResult : unmanaged
        {
            PacketReader reader;
            using (var requestBody = RentedBytesFromStruct(request))
            {
                reader = await CallAsync(opcode, requestBody.Buffer, 0, requestBody.Length, cancellationToken).ConfigureAwait(false);
                if (reader.RemainingSize < Unsafe.SizeOf<TResult>())
                    throw new InvalidOperationException("Response does not contain enough data to fill TResult.");
            }

            return reader.Read<TResult>();
        }

        private bool EnqueuePacket(RentedBuffer<byte> packet)
        {
            try
            {
                _writeQueue.Add(packet);
                return true;
            }
            catch (InvalidOperationException)
            {
                packet.Dispose();
                return false;
            }
        }

        private void WriteLoop()
        {
            try
            {
                foreach (var packet in _writeQueue.GetConsumingEnumerable())
                {
                    using (packet)
                        _writer.Write(packet.Buffer, 0, packet.Length);
                }
            }
            catch (Exception e)
            {
                Logger.Error<JustCefProcess>($"An exception occurred while writing to the IPC", e);
            }
            finally
            {
                _writeQueue.CompleteAdding();
                while (_writeQueue.TryTake(out var packet))
                    packet.Dispose();
                _writer.Dispose();
            }
        }

        private bool CompleteInflightRequest(uint requestId)
        {
            lock (_inflightRequests)
                return _inflightRequests.Remove(requestId);
        }

        private bool SendResponse(uint requestId, byte opcode, JustCefStatus status, PacketWriter packetWriter)
        {
            int packetSize = HeaderSize + 1 + packetWriter.Size;
            var rentedBuffer = new RentedBuffer<byte>(BufferPool, packetSize);

            using (var stream = new MemoryStream(rentedBuffer.Buffer, 0, packetSize))
            using (var writer = new BinaryWriter(stream))
            {
                writer.Write((uint)(packetSize - 4));
                writer.Write(requestId);
                writer.Write((byte)PacketType.Response);
                writer.Write(opcode);
                writer.Write((byte)status);

                if (packetWriter.Size > 0)
                    writer.Write(packetWriter.Data, 0, packetWriter.Size);
            }

            return EnqueuePacket(rentedBuffer);
        }

        private async Task<PacketReader> CallAsync(OpcodeController opcode, CancellationToken cancellationToken = default)
        {
            return await CallAsync(opcode, Array.Empty<byte>(), cancellationToken).ConfigureAwait(false);
        }

        private async Task<PacketReader> CallAsync(OpcodeController opcode, byte[] body, CancellationToken cancellationToken = default)
        {
            return await CallAsync(opcode, body, 0, body.Length, cancellationToken).ConfigureAwait(false);
        }

        private async Task<PacketReader> CallAsync(OpcodeController opcode, byte[] body, int offset, int size, CancellationToken cancellationToken = default)
        {
            return await CallAsync(opcode, body, offset, size, DefaultCallTimeout, cancellationToken).ConfigureAwait(false);
        }

        private async Task<PacketReader> CallAsync(OpcodeController opcode, byte[] body, int offset, int size, TimeSpan timeout, CancellationToken cancellationToken = default, Action<PacketReader>? onResponse = null)
        {
            EnsureStarted();

            if (size > MaxIPCSize)
                throw new InvalidOperationException("Exceeding max buffer size.");

            var requestId = Interlocked.Increment(ref _requestIdCounter);
            if (requestId == 0)
                requestId = Interlocked.Increment(ref _requestIdCounter);
            var pendingRequest = new PendingRequest((byte)opcode, onResponse);

            lock (_pendingRequests)
            {
                if (_shutdown)
                    throw new TaskCanceledException("The JustCef process has exited.");
                _pendingRequests[requestId] = pendingRequest;
            }

            try
            {
                if (!EnqueuePacket(BuildPacket(requestId, PacketType.Request, (byte)opcode, body, offset, size)))
                    throw new TaskCanceledException("The JustCef process has exited.");

                try
                {
                    return new PacketReader(await pendingRequest.Completion.Task.WaitAsync(timeout, cancellationToken).ConfigureAwait(false));
                }
                catch (Exception e) when (e is TimeoutException || e is OperationCanceledException && cancellationToken.IsCancellationRequested)
                {
                    bool abandoned;
                    lock (_pendingRequests)
                    {
                        abandoned = _pendingRequests.Remove(requestId);
                    }

                    if (abandoned)
                        EnqueuePacket(BuildPacket(requestId, PacketType.Cancel, (byte)opcode, Array.Empty<byte>(), 0, 0));

                    throw;
                }
            }
            finally
            {
                lock (_pendingRequests)
                {
                    _pendingRequests.Remove(requestId);
                }
            }
        }

        private static RentedBuffer<byte> BuildPacket(uint requestId, PacketType packetType, byte opcode, byte[] body, int offset, int size)
        {
            int packetLength = HeaderSize + size;
            var rentedBuffer = new RentedBuffer<byte>(BufferPool, packetLength);

            using (var stream = new MemoryStream(rentedBuffer.Buffer, 0, packetLength))
            using (var writer = new BinaryWriter(stream))
            {
                writer.Write((uint)(packetLength - 4));
                writer.Write(requestId);
                writer.Write((byte)packetType);
                writer.Write(opcode);

                if (size > 0)
                    writer.Write(body, offset, size);
            }

            return rentedBuffer;
        }

        private bool Notify(OpcodeControllerNotification opcode, byte[] body, int offset, int size)
        {
            EnsureStarted();
            return EnqueuePacket(BuildPacket(0, PacketType.Notification, (byte)opcode, body, offset, size));
        }

        private bool Notify(OpcodeControllerNotification opcode, PacketWriter writer)
        {
            try
            {
                return Notify(opcode, writer.Data, 0, writer.Size);
            }
            finally
            {
                writer.Dispose();
            }
        }

        private bool Notify(OpcodeControllerNotification opcode)
        {
            return Notify(opcode, Array.Empty<byte>(), 0, 0);
        }

        public async Task EchoAsync(byte[] data, CancellationToken cancellationToken = default) => await CallAsync(OpcodeController.Echo, data, cancellationToken).ConfigureAwait(false);
        public async Task PingAsync(CancellationToken cancellationToken = default) => await CallAsync(OpcodeController.Ping, cancellationToken).ConfigureAwait(false);

        public async Task PrintAsync(string message, CancellationToken cancellationToken = default) => await CallAsync(OpcodeController.Print, Encoding.UTF8.GetBytes(message), cancellationToken).ConfigureAwait(false);

        private void EnsureStarted()
        {
            if (!_started)
                throw new Exception("Process should be started.");
        }

        public async Task<JustCefWindow> CreateWindowAsync(string url, int minimumWidth, int minimumHeight, int preferredWidth = 0, int preferredHeight = 0,
            bool fullscreen = false, bool contextMenuEnable = false, bool shown = true, bool developerToolsEnabled = false, bool resizable = true, bool frameless = false,
            bool centered = true, bool proxyRequests = false, bool logConsole = false, Func<JustCefWindow, IPCRequest, Task<IPCResponse?>>? requestProxy = null, bool modifyRequests = false, Func<JustCefWindow, IPCRequest, IPCRequest?>? requestModifier = null, bool modifyRequestBody = false,
            string? title = null, string? iconPath = null, string? appId = null, CancellationToken cancellationToken = default, bool bridgeEnabled = false,
            Func<JustCefWindow, string, string?, Task<string?>>? bridgeRpcHandler = null, bool viewsEnabled = false, Func<JustCefView, Task>? viewCreatedHandler = null,
            TimeSpan? modifyTimeout = null, ModifyTimeoutPolicy modifyTimeoutPolicy = ModifyTimeoutPolicy.Continue, TimeSpan? proxyOpenTimeout = null)
        {
            EnsureStarted();

            if (bridgeRpcHandler != null && !bridgeEnabled)
                throw new ArgumentException("When bridgeRpcHandler is provided, bridgeEnabled must be true.", nameof(bridgeRpcHandler));
            if (viewCreatedHandler != null && !viewsEnabled)
                throw new ArgumentException("When viewCreatedHandler is provided, viewsEnabled must be true.", nameof(viewCreatedHandler));

            PacketWriter writer = new PacketWriter();
            writer.Write(resizable);
            writer.Write(frameless);
            writer.Write(fullscreen);
            writer.Write(centered);
            writer.Write(shown);
            writer.Write(contextMenuEnable);
            writer.Write(developerToolsEnabled);
            writer.Write(modifyRequests);
            writer.Write(modifyRequestBody);

            if (proxyRequests && requestProxy == null)  
                throw new ArgumentException("When proxyRequests is true, requestProxy must be non null.");

            writer.Write(proxyRequests);
            writer.Write(logConsole);
            writer.Write(bridgeEnabled);
            writer.Write(minimumWidth);
            writer.Write(minimumHeight);
            writer.Write(preferredWidth);
            writer.Write(preferredHeight);
            writer.WriteSizePrefixedString(url);
            writer.WriteSizePrefixedString(title);
            writer.WriteSizePrefixedString(iconPath);
            writer.WriteSizePrefixedString(appId);
            writer.Write(viewsEnabled);
            writer.Write(ToMilliseconds(modifyTimeout));
            writer.Write((byte)modifyTimeoutPolicy);
            writer.Write(ToMilliseconds(proxyOpenTimeout));

            var created = new TaskCompletionSource<JustCefWindow>(TaskCreationOptions.RunContinuationsAsynchronously);
            var call = CallAsync(OpcodeController.WindowCreate, writer, DefaultCallTimeout, cancellationToken, reader =>
            {
                var window = new JustCefWindow(this, reader.Read<int>(), requestModifier, requestProxy, bridgeRpcHandler, !String.IsNullOrEmpty(url));
                window.SetViewCreatedHandler(viewCreatedHandler);
                if (!RegisterBrowser(window))
                {
                    created.TrySetCanceled();
                    return;
                }

                created.TrySetResult(window);
            });

            _ = call.ContinueWith(task =>
            {
                if (task.IsCanceled)
                    created.TrySetCanceled();
                else
                    created.TrySetException(task.Exception!.InnerExceptions);
            }, CancellationToken.None, TaskContinuationOptions.ExecuteSynchronously | TaskContinuationOptions.NotOnRanToCompletion, TaskScheduler.Default);

            return await created.Task.ConfigureAwait(false);
        }

        private static uint ToMilliseconds(TimeSpan? timeout)
        {
            if (timeout is not { } value || value <= TimeSpan.Zero || value == Timeout.InfiniteTimeSpan)
                return 0;

            return (uint)Math.Clamp(Math.Ceiling(value.TotalMilliseconds), 1, uint.MaxValue);
        }

        public Task NotifyExitAsync(CancellationToken cancellationToken = default)
        {
            Notify(OpcodeControllerNotification.Exit);
            return Task.CompletedTask;
        }

        public void WaitForExit()
        {
            EnsureStarted();
            _exitTaskCompletionSource.Task.GetAwaiter().GetResult();
        }

        public async Task WaitForExitAsync(CancellationToken cancellationToken = default)
        {
            EnsureStarted();
            await _exitTaskCompletionSource.Task.WaitAsync(cancellationToken).ConfigureAwait(false);
        }

        public void WaitForReady()
        {
            EnsureStarted();
            _readyTaskCompletionSource.Task.Wait();
        }

        public async Task WaitForReadyAsync(CancellationToken cancellationToken = default)
        {
            EnsureStarted();
            await _readyTaskCompletionSource.Task.WaitAsync(cancellationToken).ConfigureAwait(false);
        }

        public async Task WindowMaximizeAsync(int identifier, CancellationToken cancellationToken = default) 
            => await CallAsync(OpcodeController.WindowMaximize, new PacketWriter().Write(identifier), cancellationToken).ConfigureAwait(false);

        public async Task WindowMinimizeAsync(int identifier, CancellationToken cancellationToken = default) 
            => await CallAsync(OpcodeController.WindowMinimize, new PacketWriter().Write(identifier), cancellationToken).ConfigureAwait(false);

        public async Task WindowRestoreAsync(int identifier, CancellationToken cancellationToken = default) 
            => await CallAsync(OpcodeController.WindowRestore, new PacketWriter().Write(identifier), cancellationToken).ConfigureAwait(false);

        public async Task WindowShowAsync(int identifier, CancellationToken cancellationToken = default) 
            => await CallAsync(OpcodeController.WindowShow, new PacketWriter().Write(identifier), cancellationToken).ConfigureAwait(false);

        public async Task WindowHideAsync(int identifier, CancellationToken cancellationToken = default) 
            => await CallAsync(OpcodeController.WindowHide, new PacketWriter().Write(identifier), cancellationToken).ConfigureAwait(false);

        public async Task WindowActivateAsync(int identifier, CancellationToken cancellationToken = default) 
            => await CallAsync(OpcodeController.WindowActivate, new PacketWriter().Write(identifier), cancellationToken).ConfigureAwait(false);

        public async Task WindowBringToTopAsync(int identifier, CancellationToken cancellationToken = default) 
            => await CallAsync(OpcodeController.WindowBringToTop, new PacketWriter().Write(identifier), cancellationToken).ConfigureAwait(false);

        public async Task WindowSetAlwaysOnTopAsync(int identifier, bool alwaysOnTop, CancellationToken cancellationToken = default)
        {
            await CallAsync(OpcodeController.WindowSetAlwaysOnTop, new PacketWriter().Write(identifier).Write(alwaysOnTop), cancellationToken).ConfigureAwait(false);
        }

        public async Task WindowSetFullscreenAsync(int identifier, bool fullscreen, CancellationToken cancellationToken = default)
        {
            await CallAsync(OpcodeController.WindowSetFullscreen, new PacketWriter()
                .Write(identifier)
                .Write(fullscreen), cancellationToken).ConfigureAwait(false);
        }

        public async Task WindowCenterSelfAsync(int identifier, CancellationToken cancellationToken = default)
        {
            await CallAsync(OpcodeController.WindowCenterSelf, new PacketWriter()
                .Write(identifier), cancellationToken).ConfigureAwait(false);
        }

        public async Task WindowSetProxyRequestsAsync(int identifier, bool enableProxyRequests, CancellationToken cancellationToken = default)
        {
            await CallAsync(OpcodeController.WindowSetProxyRequests, new PacketWriter()
                .Write(identifier)
                .Write(enableProxyRequests), cancellationToken).ConfigureAwait(false);
        }

        public async Task WindowSetModifyRequestsAsync(int identifier, bool enableModifyRequests, bool enableModifyBody, CancellationToken cancellationToken = default)
        {
            await CallAsync(OpcodeController.WindowSetModifyRequests, new PacketWriter()
                .Write(identifier).Write((byte)(((enableModifyBody ? 1 : 0) << 1) | (enableModifyRequests ? 1 : 0))), cancellationToken).ConfigureAwait(false);
        }

        public async Task RequestFocusAsync(int identifier, CancellationToken cancellationToken = default)
        {
            await CallAsync(OpcodeController.WindowRequestFocus, new PacketWriter()
                .Write(identifier), cancellationToken).ConfigureAwait(false);
        }

        public async Task WindowLoadUrlAsync(int identifier, string url, CancellationToken cancellationToken = default)
        {
            await WindowLoadUrlAsync(identifier, url, null, cancellationToken).ConfigureAwait(false);
        }

        internal async Task WindowLoadUrlAsync(int identifier, string url, Action? onSent, CancellationToken cancellationToken = default)
        {
            await CallAsync(OpcodeController.WindowLoadUrl, new PacketWriter()
                .Write(identifier)
                .WriteSizePrefixedString(url), DefaultCallTimeout, cancellationToken, onSent == null ? null : _ => onSent()).ConfigureAwait(false);
        }

        public async Task WindowSetPositionAsync(int identifier, int x, int y, CancellationToken cancellationToken = default)
        {
            await CallAsync(OpcodeController.WindowSetPosition, new PacketWriter()
                .Write(identifier)
                .Write(x)
                .Write(y), cancellationToken).ConfigureAwait(false);
        }

        public async Task<(int X, int Y)> WindowGetPositionAsync(int identifier, CancellationToken cancellationToken = default)
        {
            var reader = await CallAsync(OpcodeController.WindowGetPosition, new PacketWriter().Write(identifier), cancellationToken).ConfigureAwait(false);
            var x = reader.Read<int>();
            var y = reader.Read<int>();
            return (x, y);
        }

        public async Task WindowSetSizeAsync(int identifier, int width, int height, CancellationToken cancellationToken = default)
        {
            await CallAsync(OpcodeController.WindowSetSize, new PacketWriter()
                .Write(identifier)
                .Write(width)
                .Write(height), cancellationToken).ConfigureAwait(false);
        }

        public async Task<(int Width, int Height)> WindowGetSizeAsync(int identifier, CancellationToken cancellationToken = default)
        {
            var reader = await CallAsync(OpcodeController.WindowGetSize, new PacketWriter().Write(identifier), cancellationToken).ConfigureAwait(false);
            var width = reader.Read<int>();
            var height = reader.Read<int>();
            return (width, height);
        }

        public async Task WindowSetZoomAsync(int identifier, double zoom, CancellationToken cancellationToken = default)
        {
            await CallAsync(OpcodeController.WindowSetZoom, new PacketWriter()
                .Write(identifier)
                .Write(zoom), cancellationToken).ConfigureAwait(false);
        }

        public async Task<double> WindowGetZoomAsync(int identifier, CancellationToken cancellationToken = default)
        {
            var reader = await CallAsync(OpcodeController.WindowGetZoom, new PacketWriter().Write(identifier), cancellationToken).ConfigureAwait(false);
            return reader.Read<double>();
        }

        public async Task<WidevineStatus> GetWidevineStatusAsync(CancellationToken cancellationToken = default)
        {
            var reader = await CallAsync(OpcodeController.GetWidevineStatus, new PacketWriter(), cancellationToken).ConfigureAwait(false);
            var state = (WidevineComponentState)reader.Read<int>();
            var version = reader.ReadSizePrefixedString();
            return new WidevineStatus
            {
                State = state,
                Version = version,
                Registered = reader.Read<byte>() != 0,
                Installed = reader.Read<byte>() != 0,
                RequiresRestart = reader.Read<byte>() != 0
            };
        }

        public async Task WindowSetDevelopmentToolsEnabledAsync(int identifier, bool developmentToolsEnabled, CancellationToken cancellationToken = default)
        {
            await CallAsync(OpcodeController.WindowSetDevelopmentToolsEnabled, new PacketWriter()
                .Write(identifier)
                .Write(developmentToolsEnabled), cancellationToken).ConfigureAwait(false);
        }

        public async Task WindowSetDevelopmentToolsVisibleAsync(int identifier, bool developmentToolsVisible, CancellationToken cancellationToken = default)
        {
            await CallAsync(OpcodeController.WindowSetDevelopmentToolsVisible, new PacketWriter()
                .Write(identifier)
                .Write(developmentToolsVisible), cancellationToken).ConfigureAwait(false);
        }

        public async Task WindowCloseAsync(int identifier, bool forceClose = false, CancellationToken cancellationToken = default)
        {
            await CallAsync(OpcodeController.WindowClose, new PacketWriter()
                .Write(identifier)
                .Write(forceClose), cancellationToken).ConfigureAwait(false);
        }

        public async Task<string[]> WindowPickFileAsync(int identifier, bool multiple, (string Name, string Pattern)[] filters, CancellationToken cancellationToken = default)
        {
            var writer = new PacketWriter();
            writer.Write(identifier);
            writer.Write((byte)(multiple ? 1 : 0));
            writer.Write((uint)filters.Length);
            for (int i = 0; i < filters.Length; i++)
            {
                writer.WriteSizePrefixedString(filters[i].Name);
                writer.WriteSizePrefixedString(filters[i].Pattern);
            }

            var reader = await CallAsync(OpcodeController.PickFile, writer, Timeout.InfiniteTimeSpan, cancellationToken).ConfigureAwait(false);
            uint pathCount = reader.Read<uint>();
            string[] paths = new string[(int)pathCount];
            for (int i = 0; i < pathCount; i++)
                paths[i] = reader.ReadSizePrefixedString()!;

            return paths;
        }

        public async Task<string> WindowPickDirectoryAsync(int identifier, CancellationToken cancellationToken = default)
        {
            var reader = await CallAsync(OpcodeController.PickDirectory, new PacketWriter().Write(identifier), Timeout.InfiniteTimeSpan, cancellationToken).ConfigureAwait(false);
            return reader.ReadSizePrefixedString()!;
        }

        public async Task<string> WindowSaveFileAsync(int identifier, string defaultName, (string Name, string Pattern)[] filters, CancellationToken cancellationToken = default)
        {
            var writer = new PacketWriter();
            writer.Write(identifier);
            writer.WriteSizePrefixedString(defaultName);

            writer.Write((uint)filters.Length);
            for (int i = 0; i < filters.Length; i++)
            {
                writer.WriteSizePrefixedString(filters[i].Name);
                writer.WriteSizePrefixedString(filters[i].Pattern);
            }

            var reader = await CallAsync(OpcodeController.SaveFile, writer, Timeout.InfiniteTimeSpan, cancellationToken).ConfigureAwait(false);
            return reader.ReadSizePrefixedString()!;
        }

        public async Task<(bool Success, byte[] Data)> WindowExecuteDevToolsMethodAsync(int identifier, string methodName, string? json = null,  CancellationToken cancellationToken = default)
        {
            var writer = new PacketWriter();
            writer.Write(identifier);
            writer.WriteSizePrefixedString(methodName);
            writer.Write(json != null);
            if (json != null)
            {
                byte[] payload = Encoding.UTF8.GetBytes(json);
                writer.Write((uint)payload.Length);
                writer.WriteBytes(payload);
            }

            var reader = await CallAsync(OpcodeController.WindowExecuteDevToolsMethod, writer, cancellationToken).ConfigureAwait(false);
            var success = reader.Read<bool>();
            var result = reader.ReadBytes((int)reader.Read<uint>());
            return (success, result);
        }

        public async Task<string> WindowBridgeRpcAsync(int identifier, string method, string? json = null, CancellationToken cancellationToken = default)
        {
            if (string.IsNullOrWhiteSpace(method))
                throw new ArgumentException("Bridge RPC method must be a non-empty string.", nameof(method));

            var writer = new PacketWriter();
            writer.Write(identifier);
            writer.WriteSizePrefixedString(method);
            byte[] payload = Encoding.UTF8.GetBytes(json ?? "null");
            writer.Write((uint)payload.Length);
            writer.WriteBytes(payload);

            var reader = await CallAsync(OpcodeController.WindowBridgeRpc, writer, cancellationToken).ConfigureAwait(false);
            return reader.ReadString((int)reader.Read<uint>());
        }

        public async Task WindowSetTitleAsync(int identifier, string title, CancellationToken cancellationToken = default)
        {
            await CallAsync(OpcodeController.WindowSetTitle, new PacketWriter()
                .Write(identifier)
                .WriteSizePrefixedString(title), cancellationToken).ConfigureAwait(false);
        }

        public async Task WindowSetIconAsync(int identifier, string iconPath, CancellationToken cancellationToken = default)
        {
            await CallAsync(OpcodeController.WindowSetIcon, new PacketWriter()
                .Write(identifier)
                .WriteSizePrefixedString(iconPath), cancellationToken).ConfigureAwait(false);
        }

        public async Task WindowAddUrlToProxyAsync(int identifier, string url, CancellationToken cancellationToken = default)
        {
            await CallAsync(OpcodeController.WindowAddUrlToProxy, new PacketWriter()
                .Write(identifier)
                .WriteSizePrefixedString(url), cancellationToken).ConfigureAwait(false);
        }

        public async Task WindowRemoveUrlToProxyAsync(int identifier, string url, CancellationToken cancellationToken = default)
        {
            await CallAsync(OpcodeController.WindowRemoveUrlToProxy, new PacketWriter()
                .Write(identifier)
                .WriteSizePrefixedString(url), cancellationToken).ConfigureAwait(false);
        }

        public async Task WindowAddDomainToProxyAsync(int identifier, string url, CancellationToken cancellationToken = default)
        {
            await CallAsync(OpcodeController.WindowAddDomainToProxy, new PacketWriter()
                .Write(identifier)
                .WriteSizePrefixedString(url), cancellationToken).ConfigureAwait(false);
        }

        public async Task WindowRemoveDomainToProxyAsync(int identifier, string url, CancellationToken cancellationToken = default)
        {
            await CallAsync(OpcodeController.WindowRemoveDomainToProxy, new PacketWriter()
                .Write(identifier)
                .WriteSizePrefixedString(url), cancellationToken).ConfigureAwait(false);
        }

        public async Task WindowAddUrlToModifyAsync(int identifier, string url, CancellationToken cancellationToken = default)
        {
            await CallAsync(OpcodeController.WindowAddUrlToModify, new PacketWriter()
                .Write(identifier)
                .WriteSizePrefixedString(url), cancellationToken).ConfigureAwait(false);
        }

        public async Task WindowRemoveUrlToModifyAsync(int identifier, string url, CancellationToken cancellationToken = default)
        {
            await CallAsync(OpcodeController.WindowRemoveUrlToModify, new PacketWriter()
                .Write(identifier)
                .WriteSizePrefixedString(url), cancellationToken).ConfigureAwait(false);
        }

        public async Task WindowAddDevToolsEventMethod(int identifier, string method, CancellationToken cancellationToken = default)
        {
            await CallAsync(OpcodeController.WindowAddDevToolsEventMethod, new PacketWriter()
                .Write(identifier)
                .WriteSizePrefixedString(method), cancellationToken).ConfigureAwait(false);
        }

        public async Task WindowRemoveDevToolsEventMethod(int identifier, string method, CancellationToken cancellationToken = default)
        {
            await CallAsync(OpcodeController.WindowRemoveDevToolsEventMethod, new PacketWriter()
                .Write(identifier)
                .WriteSizePrefixedString(method), cancellationToken).ConfigureAwait(false);
        }

        public void Dispose()
        {
            if (Interlocked.Exchange(ref _disposed, 1) != 0)
                return;

            _readyTaskCompletionSource.TrySetCanceled();

            if (_started)
                Notify(OpcodeControllerNotification.Exit);
            _writeQueue.CompleteAdding();

            if (!_started)
            {
                _writer.Dispose();
                _reader.Dispose();
            }

            Shutdown();

            if (_onProcessExit != null)
            {
                AppDomain.CurrentDomain.ProcessExit -= _onProcessExit;
                _onProcessExit = null;
            }

            var process = _childProcess;
            if (process != null)
            {
                _ = Task.Delay(ShutdownGracePeriod).ContinueWith(_ =>
                {
                    KillChild(process);
                    _events.Writer.TryComplete();
                    process.Dispose();
                }, TaskScheduler.Default);
            }
            else
                _events.Writer.TryComplete();
        }

        private void Shutdown()
        {
            JustCefBrowser[] browsers;
            lock (_browsers)
            {
                if (_shutdown)
                    return;

                _shutdown = true;
                browsers = _browsers.ToArray();
                _browsers.Clear();
            }

            lock (_pendingRequests)
            {
                foreach (var pendingRequest in _pendingRequests)
                    pendingRequest.Value.Completion.TrySetCanceled();
                _pendingRequests.Clear();
            }

            foreach (var window in browsers)
                window.InvokeOnClose();

            lock (_outgoingStreams)
            {
                foreach (var pair in _outgoingStreams)
                    _ = pair.Value.Cancellation.CancelAsync();
            }

            SignalExited();
        }

        private static void KillChild(Process process)
        {
            try
            {
                if (process.HasExited)
                    return;

                Logger.Warning<JustCefProcess>("Native process did not exit within the grace period. Killing it.");
                process.Kill(true);
            }
            catch (Exception e)
            {
                Logger.Error<JustCefProcess>("Failed to kill the native process.", e);
            }
        }

        internal void PostEvent(Action action)
        {
            _events.Writer.TryWrite(action);
        }

        private async Task DispatchEventsAsync()
        {
            while (await _events.Reader.WaitToReadAsync().ConfigureAwait(false))
            {
                while (_events.Reader.TryRead(out var action))
                {
                    try
                    {
                        action();
                    }
                    catch (Exception e)
                    {
                        Logger.Error<JustCefProcess>("An event handler threw an exception.", e);
                    }
                }
            }
        }

        private static string GetNativeFileName()
        {
            if (OperatingSystem.IsWindows())
                return "justcefnative.exe";
            else if (OperatingSystem.IsMacOS())
                return "justcefnative";
            else if (OperatingSystem.IsLinux())
                return "justcefnative";
            else
                throw new PlatformNotSupportedException("Unsupported platform.");
        }

        private static string? GetDirectory(string? path)
        {
            return !string.IsNullOrEmpty(path) ? Path.GetDirectoryName(path) : null;
        }

        private static string[] GenerateSearchPaths()
        {
            const string cefDir = "cef";

            string baseDirectory = AppContext.BaseDirectory;
            string nativeFileName = GetNativeFileName();
            string? assemblyDirectory = GetDirectory(Assembly.GetEntryAssembly()?.Location);
            string? executableDirectory = GetDirectory(Process.GetCurrentProcess().MainModule?.FileName);
            string currentWorkingDirectory = Environment.CurrentDirectory;

            var searchPaths = new List<string>();

            if (OperatingSystem.IsMacOS())
            {
                searchPaths.Add(Path.Combine(baseDirectory, $"justcefnative.app/Contents/MacOS/{nativeFileName}"));
                searchPaths.Add(Path.Combine(baseDirectory, $"JustCef.app/Contents/MacOS/{nativeFileName}"));
            }
            if (OperatingSystem.IsMacOS())
            {
                searchPaths.Add(Path.Combine(baseDirectory, $"../Frameworks/justcefnative.app/Contents/MacOS/{nativeFileName}"));
                searchPaths.Add(Path.Combine(baseDirectory, $"../Frameworks/JustCef.app/Contents/MacOS/{nativeFileName}"));
            }
            searchPaths.Add(Path.Combine(baseDirectory, cefDir, nativeFileName));
            searchPaths.Add(Path.Combine(baseDirectory, nativeFileName));

            if (assemblyDirectory != null)
            {
                if (OperatingSystem.IsMacOS())
                {
                    searchPaths.Add(Path.Combine(assemblyDirectory, $"justcefnative.app/Contents/MacOS/{nativeFileName}"));
                    searchPaths.Add(Path.Combine(assemblyDirectory, $"JustCef.app/Contents/MacOS/{nativeFileName}"));
                }
                if (OperatingSystem.IsMacOS())
                {
                    searchPaths.Add(Path.Combine(assemblyDirectory, $"../Frameworks/justcefnative.app/Contents/MacOS/{nativeFileName}"));
                    searchPaths.Add(Path.Combine(assemblyDirectory, $"../Frameworks/JustCef.app/Contents/MacOS/{nativeFileName}"));
                }
                searchPaths.Add(Path.Combine(assemblyDirectory, cefDir, nativeFileName));
                searchPaths.Add(Path.Combine(assemblyDirectory, nativeFileName));
            }

            if (executableDirectory != null)
            {
                if (OperatingSystem.IsMacOS())
                {
                    searchPaths.Add(Path.Combine(executableDirectory, $"justcefnative.app/Contents/MacOS/{nativeFileName}"));
                    searchPaths.Add(Path.Combine(executableDirectory, $"JustCef.app/Contents/MacOS/{nativeFileName}"));
                }
                if (OperatingSystem.IsMacOS())
                {
                    searchPaths.Add(Path.Combine(executableDirectory, $"../Frameworks/justcefnative.app/Contents/MacOS/{nativeFileName}"));
                    searchPaths.Add(Path.Combine(executableDirectory, $"../Frameworks/JustCef.app/Contents/MacOS/{nativeFileName}"));
                }
                searchPaths.Add(Path.Combine(executableDirectory, cefDir, nativeFileName));
                searchPaths.Add(Path.Combine(executableDirectory, nativeFileName));
            }

            if (OperatingSystem.IsMacOS())
            {
                searchPaths.Add(Path.Combine(currentWorkingDirectory, $"justcefnative.app/Contents/MacOS/{nativeFileName}"));
                searchPaths.Add(Path.Combine(currentWorkingDirectory, $"JustCef.app/Contents/MacOS/{nativeFileName}"));
            }
            if (OperatingSystem.IsMacOS())
            {
                searchPaths.Add(Path.Combine(currentWorkingDirectory, $"../Frameworks/justcefnative.app/Contents/MacOS/{nativeFileName}"));
                searchPaths.Add(Path.Combine(currentWorkingDirectory, $"../Frameworks/JustCef.app/Contents/MacOS/{nativeFileName}"));
            }
            searchPaths.Add(Path.Combine(currentWorkingDirectory, nativeFileName));
            searchPaths.Add(Path.Combine(currentWorkingDirectory, cefDir, nativeFileName));

            return searchPaths.Distinct().ToArray();
        }
    }
}
