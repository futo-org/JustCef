#define HARDCODED_PATHS

using System.Buffers;
using System.Buffers.Binary;
using System.Diagnostics;
using System.IO.Pipes;
using System.Net;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Runtime.Versioning;
using System.Text;

namespace JustCef
{
    public class JustCefProcess : IDisposable
    {
        public enum PacketType : byte
        {
            Request = 0,
            Response = 1,
            Notification = 2
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
            StreamOpen = 35,
            StreamClose = 36,
            StreamData = 37,
            StreamCancel = 38,
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
            WindowBridgeRpc = 57
        }

        public enum OpcodeControllerNotification : byte
        {
            Exit = 0
        }

        public enum OpcodeClient : byte
        {
            Ping = 0,
            Print = 1,
            Echo = 2,
            WindowProxyRequest = 3,
            WindowModifyRequest = 4,
            StreamOpen = 5,
            StreamData = 6,
            StreamClose = 7,
            StreamCancel = 8,
            WindowBridgeRpc = 9
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
            WindowLoadStart = 13,
            WindowLoadEnd = 14,
            WindowLoadError = 15,
            WindowDevToolsEvent = 16
        }

        private enum BridgeRpcPayloadEncoding : byte
        {
            Inline = 0,
            Stream = 1
        }

        private sealed class IncomingStreamDispatcher
        {
            public readonly object SyncRoot = new();
            public readonly Queue<Func<Task>> Queue = new();
            public bool Running;
            public Task? WorkerTask;
        }

        private static ArrayPool<byte> BufferPool = ArrayPool<byte>.Create();
        private readonly TaskCompletionSource _readyTaskCompletionSource = new TaskCompletionSource();

        private const int MaxIPCSize = 10 * 1024 * 1024;
        private const int HeaderSize = 4 + 4 + 1 + 1;
        private const int InlineResponseBodyFramingSize = sizeof(byte) + sizeof(uint);
        private const int StreamChunkSize = 65536;
        private const int StreamElementFramingSize = sizeof(byte) + sizeof(long) + sizeof(uint);
        private const int ByteElementFramingSize = sizeof(byte) + sizeof(uint);
        private const int BridgeRpcInlinePayloadFramingSize = sizeof(byte) + sizeof(uint);
        private const long UnknownStreamLength = -1;
        private readonly AnonymousPipeServerStream _writer;
        private readonly AnonymousPipeServerStream _reader;
        private readonly Dictionary<uint, TaskCompletionSource<byte[]>> _pendingRequests = new Dictionary<uint, TaskCompletionSource<byte[]>>();
        private Process? _childProcess;
        private bool _started = false;
        private SemaphoreSlim _writeSemaphore = new SemaphoreSlim(1);
        private uint _requestIdCounter = 0;
        private readonly List<JustCefWindow> _windows = new List<JustCefWindow>();
        private CancellationTokenSource _cancellationTokenSource = new CancellationTokenSource();
        private uint _streamIdentifierGenerator = 0;
        private Dictionary<uint, CancellationTokenSource> _streamCancellationTokens = new Dictionary<uint, CancellationTokenSource>();
        private readonly Dictionary<uint, DataStream> _incomingStreams = new Dictionary<uint, DataStream>();
        private readonly HashSet<uint> _canceledIncomingStreams = new HashSet<uint>();
        private readonly Dictionary<uint, IncomingStreamDispatcher> _incomingStreamDispatchers = new Dictionary<uint, IncomingStreamDispatcher>();
        private readonly HashSet<Task> _backgroundStreamTasks = new HashSet<Task>();
        private readonly TaskCompletionSource _exitTaskCompletionSource = new(TaskCreationOptions.RunContinuationsAsynchronously);

        private void SignalExited()
        {
            _exitTaskCompletionSource.TrySetResult();
        }

        public List<JustCefWindow> Windows
        {
            get
            {
                lock (_windows)
                {
                    return _windows.ToList();
                }
            }
        }

        public JustCefWindow? GetWindow(int identifier)
        {
            lock (_windows)
            {
                return _windows.FirstOrDefault(v => v.Identifier == identifier);
            }
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

        public JustCefProcess()
        {
            //var writer = new AnonymousPipeServerStream(PipeDirection.Out, HandleInheritability.None);
            //writer.Dispose();
            _writer = new AnonymousPipeServerStream(PipeDirection.Out, HandleInheritability.Inheritable);

            //var reader = new AnonymousPipeServerStream(PipeDirection.In, HandleInheritability.None);
            //reader.Dispose();
            _reader = new AnonymousPipeServerStream(PipeDirection.In, HandleInheritability.Inheritable);

            AppDomain.CurrentDomain.ProcessExit += (_, _) =>
            {
                _writer.Dispose();
                _reader.Dispose();
            };
        }

        public void Start(string? args = null)
        {
            if (_started)
                throw new Exception("Already started.");

            _started = true;
            
#if !HARDCODED_PATHS
            string? nativePath = null;
            string[] searchPaths = GenerateSearchPaths();
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
                Logger.Info<JustCefProcess>("Child process exited.");
                SignalExited();
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

            AppDomain.CurrentDomain.ProcessExit += (_, _) =>
            {
                _childProcess?.Dispose();
            };

            _writer.DisposeLocalCopyOfClientHandle();
            _reader.DisposeLocalCopyOfClientHandle();

            _ = Task.Run(async () =>
            {
                try
                {
                    Logger.Info<JustCefProcess>("Receive loop started.");

                    byte[] headerBuffer = new byte[HeaderSize];

                    while (!_cancellationTokenSource.IsCancellationRequested && !HasExited)
                    {
                        await _reader.ReadExactlyAsync(headerBuffer, 0, HeaderSize, _cancellationTokenSource.Token);

                        var size = BitConverter.ToUInt32(headerBuffer, 0);
                        var requestId = BitConverter.ToUInt32(headerBuffer, 4);
                        var packetType = (PacketType)headerBuffer[8];
                        var opcode = headerBuffer[9];

                        int bodySize = (int)size + 4 - HeaderSize;
                        if (bodySize > MaxIPCSize)
                        {
                            Logger.Error<JustCefProcess>("Invalid packet size. Shutting down.");
                            Dispose();
                            return;
                        }

                        RentedBuffer<byte>? rentedBodyBuffer = null;
                        if (bodySize > 0)
                        {
                            var rb = new RentedBuffer<byte>(BufferPool, bodySize);
                            try
                            {
                                await _reader.ReadExactlyAsync(rb.Buffer, 0, bodySize, _cancellationTokenSource.Token);
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
                                if (_cancellationTokenSource.IsCancellationRequested)
                                    return;

                                if (packetType == PacketType.Response)
                                {
                                    bool foundPendingRequest;
                                    TaskCompletionSource<byte[]>? pendingRequest;
                                    lock (_pendingRequests)
                                    {
                                        foundPendingRequest = _pendingRequests.TryGetValue(requestId, out pendingRequest);
                                    }

                                    if (foundPendingRequest && pendingRequest != null)
                                        pendingRequest.SetResult(rentedBodyBuffer != null ? rentedBodyBuffer.Buffer.AsSpan().Slice(0, rentedBodyBuffer.Length).ToArray() : Array.Empty<byte>());
                                    else
                                        Logger.Error<JustCefProcess>($"Received a packet response for a request that no longer has an awaiter (request id = {requestId}).");
                                }
                                else if (packetType == PacketType.Request)
                                {
                                    var packetReader = new PacketReader(rentedBodyBuffer != null ? rentedBodyBuffer.Buffer : Array.Empty<byte>(), rentedBodyBuffer != null ? rentedBodyBuffer.Length : 0);
                                    var packetWriter = new PacketWriter();
                                    var deferredOutgoingStreams = new List<(Action Start, Action Cleanup)>();
                                    try
                                    {
                                        await HandleRequestAsync((OpcodeClient)opcode, packetReader, packetWriter, deferredOutgoingStreams, rentedBodyBuffer);
                                    }
                                    catch (Exception e)
                                    {
                                        Logger.Error<JustCefProcess>($"An exception occurred in the IPC while handling request packet", e);
                                        CleanupDeferredOutgoingStreams(deferredOutgoingStreams);
                                        deferredOutgoingStreams.Clear();
                                        packetWriter = new PacketWriter();
                                    }

                                    int packetSize = HeaderSize + packetWriter.Size;
                                    using var rentedBuffer = new RentedBuffer<byte>(BufferPool, packetSize);

                                    using (var stream = new MemoryStream(rentedBuffer.Buffer, 0, packetSize))
                                    using (var writer = new BinaryWriter(stream))
                                    {
                                        writer.Write((uint)(packetSize - 4));
                                        writer.Write(requestId);
                                        writer.Write((byte)PacketType.Response);
                                        writer.Write((byte)opcode);

                                        if (packetWriter.Size > 0)
                                            writer.Write(packetWriter.Data, 0, packetWriter.Size);
                                    }

                                    try
                                    {
                                        await WritePacketAsync(rentedBuffer.Buffer, 0, packetSize, _cancellationTokenSource.Token);
                                    }
                                    catch
                                    {
                                        CleanupDeferredOutgoingStreams(deferredOutgoingStreams);
                                        throw;
                                    }

                                    StartDeferredOutgoingStreams(deferredOutgoingStreams);
                                }
                                else if (packetType == PacketType.Notification)
                                {
                                    var packetReader = new PacketReader(rentedBodyBuffer != null ? rentedBodyBuffer.Buffer : Array.Empty<byte>(), rentedBodyBuffer != null ? rentedBodyBuffer.Length : 0);
                                    HandleNotification((OpcodeClientNotification)opcode, packetReader);
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
                        if (packetType == PacketType.Request
                            && ((OpcodeClient)opcode == OpcodeClient.StreamOpen || (OpcodeClient)opcode == OpcodeClient.StreamData || (OpcodeClient)opcode == OpcodeClient.StreamClose)
                            && rentedBodyBuffer != null
                            && rentedBodyBuffer.Length >= sizeof(uint))
                        {
                            uint streamIdentifier = BinaryPrimitives.ReadUInt32LittleEndian(rentedBodyBuffer.Buffer.AsSpan(0, sizeof(uint)));
                            QueueIncomingStreamWork(streamIdentifier, RunPacket);
                        }
                        else
                            _ = Task.Run(RunPacket);
                    }
                }
                catch (OperationCanceledException) when (_cancellationTokenSource.IsCancellationRequested)
                {
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
                    SignalExited();
                    Logger.Info<JustCefProcess>("Receive loop stopped.");
                    Dispose();
                }
            });
        }

        private async Task HandleRequestAsync(OpcodeClient opcode, PacketReader reader, PacketWriter writer, List<(Action Start, Action Cleanup)> deferredOutgoingStreams, RentedBuffer<byte>? rentedBodyBuffer)
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
                    await HandleWindowProxyRequestAsync(reader, writer, deferredOutgoingStreams);
                    break;
                case OpcodeClient.WindowModifyRequest:
                    await HandleWindowModifyRequestAsync(reader, writer, deferredOutgoingStreams);
                    break;
                case OpcodeClient.StreamOpen:
                    HandleClientStreamOpen(reader);
                    break;
                case OpcodeClient.StreamData:
                    HandleClientStreamData(reader, writer, rentedBodyBuffer);
                    break;
                case OpcodeClient.StreamClose:
                    HandleClientStreamClose(reader);
                    break;
                case OpcodeClient.StreamCancel:
                    HandleClientStreamCancel(reader);
                    break;
                case OpcodeClient.WindowBridgeRpc:
                    await HandleWindowBridgeRpcAsync(reader, writer, deferredOutgoingStreams);
                    break;
                default:
                    Logger.Warning<JustCefProcess>($"Received unhandled opcode {opcode}.");
                    break;
            }
        }

        private void QueueIncomingStreamWork(uint identifier, Func<Task> workItem)
        {
            if (_cancellationTokenSource.IsCancellationRequested)
            {
                _ = workItem();
                return;
            }

            IncomingStreamDispatcher dispatcher;
            lock (_incomingStreamDispatchers)
            {
                if (!_incomingStreamDispatchers.TryGetValue(identifier, out dispatcher!))
                {
                    dispatcher = new IncomingStreamDispatcher();
                    _incomingStreamDispatchers[identifier] = dispatcher;
                }
            }

            bool shouldSchedule;
            lock (dispatcher.SyncRoot)
            {
                dispatcher.Queue.Enqueue(workItem);
                shouldSchedule = !dispatcher.Running;
                if (shouldSchedule)
                    dispatcher.Running = true;
            }

            if (!shouldSchedule)
                return;

            var workerTask = Task.Run(() => ProcessIncomingStreamDispatcherAsync(identifier, dispatcher));
            lock (dispatcher.SyncRoot)
                dispatcher.WorkerTask = workerTask;
        }

        private void WaitForIncomingStreamDispatchers(int millisecondsTimeout)
        {
            Task[] tasks;
            lock (_incomingStreamDispatchers)
            {
                tasks = _incomingStreamDispatchers.Values
                    .Select(dispatcher => dispatcher.WorkerTask)
                    .Where(task => task != null)
                    .Cast<Task>()
                    .Distinct()
                    .ToArray();
            }

            if (tasks.Length == 0)
                return;

            try
            {
                Task.WaitAll(tasks, millisecondsTimeout);
            }
            catch
            {
            }
        }

        private async Task ProcessIncomingStreamDispatcherAsync(uint identifier, IncomingStreamDispatcher dispatcher)
        {
            try
            {
                while (true)
                {
                    Func<Task> workItem;
                    lock (dispatcher.SyncRoot)
                    {
                        if (dispatcher.Queue.Count == 0)
                        {
                            dispatcher.Running = false;
                            break;
                        }

                        workItem = dispatcher.Queue.Dequeue();
                    }

                    await workItem();
                }
            }
            catch (OperationCanceledException) when (_cancellationTokenSource.IsCancellationRequested)
            {
            }
            catch (Exception e)
            {
                Logger.Error<JustCefProcess>($"Incoming stream worker {identifier} failed.", e);
            }
            finally
            {
                lock (dispatcher.SyncRoot)
                    dispatcher.WorkerTask = null;

                lock (_incomingStreamDispatchers)
                {
                    if (_incomingStreamDispatchers.TryGetValue(identifier, out var existingDispatcher) && ReferenceEquals(existingDispatcher, dispatcher))
                        _incomingStreamDispatchers.Remove(identifier);
                }
            }
        }

        private void HandleClientStreamOpen(PacketReader reader)
        {
            uint identifier = reader.Read<uint>();
            lock (_incomingStreams)
            {
                if (_canceledIncomingStreams.Contains(identifier))
                    return;

                if (!_incomingStreams.ContainsKey(identifier))
                    _incomingStreams[identifier] = new DataStream(identifier, OnIncomingStreamDisposed);
            }
        }

        private void HandleClientStreamData(PacketReader reader, PacketWriter writer, RentedBuffer<byte>? rentedBodyBuffer)
        {
            uint identifier = reader.Read<uint>();
            DataStream? stream;
            lock (_incomingStreams)
            {
                if (_canceledIncomingStreams.Contains(identifier) || !_incomingStreams.TryGetValue(identifier, out stream))
                {
                    writer.Write(false);
                    return;
                }
            }

            int dataSize = reader.RemainingSize;
            if (dataSize == 0)
            {
                writer.Write(true);
                return;
            }

            if (rentedBodyBuffer != null)
            {
                int dataOffset = rentedBodyBuffer.Length - dataSize;
                writer.Write(stream.WriteData(rentedBodyBuffer.Buffer, dataOffset, dataSize));
                return;
            }

            byte[] buffer = reader.ReadBytes(dataSize);
            writer.Write(stream.WriteData(buffer, 0, dataSize));
        }

        private void HandleClientStreamClose(PacketReader reader)
        {
            uint identifier = reader.Read<uint>();
            DataStream? stream = null;
            lock (_incomingStreams)
            {
                if (_canceledIncomingStreams.Remove(identifier))
                {
                    _incomingStreams.Remove(identifier);
                    return;
                }

                _incomingStreams.TryGetValue(identifier, out stream);
            }

            stream?.CloseFromRemote();
        }

        private void HandleClientStreamCancel(PacketReader reader)
        {
            uint identifier = reader.Read<uint>();
            lock (_streamCancellationTokens)
            {
                if (_streamCancellationTokens.TryGetValue(identifier, out var token))
                {
                    token.Cancel();
                    _streamCancellationTokens.Remove(identifier);
                }
            }
        }

        private void OnIncomingStreamDisposed(uint identifier, bool notifyRemote)
        {
            bool removed;
            bool shouldNotifyRemote;
            lock (_incomingStreams)
            {
                removed = _incomingStreams.Remove(identifier);
                shouldNotifyRemote = removed && notifyRemote && !_cancellationTokenSource.IsCancellationRequested;
                if (shouldNotifyRemote)
                    _canceledIncomingStreams.Add(identifier);
            }

            if (!removed)
                return;

            if (!shouldNotifyRemote)
                return;

            QueueBackgroundStreamTask(async () =>
            {
                try
                {
                    await StreamCancelAsync(identifier);
                }
                catch (Exception) when (_cancellationTokenSource.IsCancellationRequested)
                {
                }
                catch (Exception e)
                {
                    Logger.Error<JustCefProcess>($"Failed to cancel incoming stream {identifier}", e);
                }
            });
        }

        private void QueueBackgroundStreamTask(Func<Task> work)
        {
            Task task = Task.Run(work);
            lock (_backgroundStreamTasks)
                _backgroundStreamTasks.Add(task);

            _ = task.ContinueWith(
                static (completedTask, state) =>
                {
                    var owner = (JustCefProcess)state!;
                    lock (owner._backgroundStreamTasks)
                        owner._backgroundStreamTasks.Remove(completedTask);
                },
                this,
                CancellationToken.None,
                TaskContinuationOptions.ExecuteSynchronously,
                TaskScheduler.Default);
        }

        private void WaitForBackgroundStreamTasks(int timeoutMilliseconds)
        {
            Task[] tasks;
            lock (_backgroundStreamTasks)
                tasks = _backgroundStreamTasks.ToArray();

            if (tasks.Length == 0)
                return;

            try
            {
                Task.WaitAll(tasks, timeoutMilliseconds);
            }
            catch (AggregateException)
            {
            }
        }

        private DataStream GetOrCreateIncomingStream(uint identifier)
        {
            lock (_incomingStreams)
            {
                if (_incomingStreams.TryGetValue(identifier, out var stream))
                    return stream;

                stream = new DataStream(identifier, OnIncomingStreamDisposed);
                _incomingStreams[identifier] = stream;
                return stream;
            }
        }

        private void ReleaseIncomingStream(uint identifier)
        {
            DataStream? stream = null;
            lock (_incomingStreams)
            {
                if (_incomingStreams.TryGetValue(identifier, out stream))
                    _incomingStreams.Remove(identifier);
            }

            stream?.CloseFromRemote();
        }

        private async Task HandleWindowProxyRequestAsync(PacketReader reader, PacketWriter writer, List<(Action Start, Action Cleanup)> deferredOutgoingStreams)
        {
            int identifier = reader.Read<int>();
            var window = GetWindow(identifier);
            if (window == null)
                return;

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
            HashSet<DataStream>? transferredStreams = null;
            try
            {
                response = await window.ProxyRequestAsync(new IPCRequest
                {
                    Method = method,
                    Url = url,
                    Headers = headers,
                    Elements = elements,
                });

                if (response == null)
                    return;

                if (response.Body != null && response.BodyStream != null)
                    throw new InvalidOperationException("IPCResponse cannot define both Body and BodyStream.");

                if (response.BodyStream != null)
                    transferredStreams = new HashSet<DataStream>(ReferenceEqualityComparer.Instance) { response.BodyStream };

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
                    long bodyLength = response.Body.LongLength;
                    long maxInlineBodySize = MaxIPCSize - writer.Size - InlineResponseBodyFramingSize;
                    if (bodyLength <= maxInlineBodySize)
                    {
                        writer.Write((byte)1);
                        writer.Write((uint)response.Body.Length);
                        writer.WriteBytes(response.Body);
                    }
                    else
                    {
                        writer.Write((byte)2);
                        HandleLargeBufferedContent(response.Body, writer, deferredOutgoingStreams);
                    }
                }
                else if (response.BodyStream != null)
                {
                    if (contentLength != null)
                    {
                        long maxInlineBodySize = MaxIPCSize - writer.Size - InlineResponseBodyFramingSize;
                        if (contentLength.Value <= maxInlineBodySize)
                        {
                            int inlineBodySize = checked((int)contentLength.Value);
                            writer.Write((byte)1);
                            writer.Write((uint)inlineBodySize);

                            byte[] buffer = ArrayPool<byte>.Shared.Rent(inlineBodySize);
                            try
                            {
                                try
                                {
                                    await ReadExactlyAsync(response.BodyStream, buffer, 0, inlineBodySize);
                                    writer.WriteBytes(buffer, 0, inlineBodySize);
                                }
                                finally
                                {
                                    DisposeQuietly(response.BodyStream);
                                }
                            }
                            finally
                            {
                                ArrayPool<byte>.Shared.Return(buffer);
                            }
                        }
                        else
                        {
                            writer.Write((byte)2);
                            HandleLargeOrChunkedContent(response.BodyStream, writer, deferredOutgoingStreams, contentLength);
                        }
                    }
                    else
                    {
                        writer.Write((byte)2);
                        HandleLargeOrChunkedContent(response.BodyStream, writer, deferredOutgoingStreams, null);
                    }
                }
                else
                    writer.Write((byte)0);
            }
            finally
            {
                DisposeIncomingStreamElements(elements, transferredStreams);
            }
        }

        private void HandleLargeBufferedContent(byte[] body, PacketWriter writer, List<(Action Start, Action Cleanup)> deferredOutgoingStreams)
        {
            AddDeferredOutgoingStream(
                writer,
                deferredOutgoingStreams,
                async (identifier, cancellationToken) =>
                {
                    int remaining = body.Length;
                    int offset = 0;

                    while (remaining > 0)
                    {
                        cancellationToken.ThrowIfCancellationRequested();

                        int chunkSize = Math.Min(StreamChunkSize, remaining);
                        if (!await StreamDataAsync(identifier, body, offset, chunkSize, cancellationToken))
                            throw new Exception("Stream closed.");

                        remaining -= chunkSize;
                        offset += chunkSize;
                    }
                });
        }

        private void HandleLargeOrChunkedContent(DataStream stream, PacketWriter writer, List<(Action Start, Action Cleanup)> deferredOutgoingStreams, long? contentLength = null)
        {
            AddDeferredOutgoingStream(
                writer,
                deferredOutgoingStreams,
                async (identifier, cancellationToken) =>
                {
                    byte[] buffer = new byte[StreamChunkSize];
                    long totalBytesRead = 0;

                    while (!contentLength.HasValue || totalBytesRead < contentLength.Value)
                    {
                        int requestedBytes = contentLength.HasValue
                            ? (int)Math.Min(buffer.Length, contentLength.Value - totalBytesRead)
                            : buffer.Length;

                        int bytesRead = await stream.ReadAsync(buffer, 0, requestedBytes, cancellationToken);
                        if (bytesRead <= 0)
                        {
                            ThrowIfEndedBeforeExpectedLength(totalBytesRead, contentLength, "proxy response body");
                            break;
                        }

                        cancellationToken.ThrowIfCancellationRequested();

                        if (!await StreamDataAsync(identifier, buffer, 0, bytesRead, cancellationToken))
                            throw new Exception("Stream closed.");

                        totalBytesRead += bytesRead;
                    }
                },
                () => DisposeQuietly(stream));
        }

        private static void ThrowIfEndedBeforeExpectedLength(long totalBytesRead, long? expectedLength, string description)
        {
            if (expectedLength.HasValue && totalBytesRead < expectedLength.Value)
            {
                throw new EndOfStreamException($"Stream for {description} ended after {totalBytesRead} bytes, expected {expectedLength.Value}.");
            }
        }

        private static async Task ReadExactlyAsync(DataStream stream, byte[] buffer, int offset, int count, CancellationToken cancellationToken = default)
        {
            int totalRead = 0;
            while (totalRead < count)
            {
                int bytesRead = await stream.ReadAsync(buffer, offset + totalRead, count - totalRead, cancellationToken);
                if (bytesRead <= 0)
                    throw new EndOfStreamException($"Data stream ended after {totalRead} bytes, expected {count}.");

                totalRead += bytesRead;
            }
        }

        private void SerializeBridgeRpcPayload(PacketWriter writer, string payload, List<(Action Start, Action Cleanup)> deferredOutgoingStreams, CancellationToken cancellationToken = default)
        {
            byte[] payloadBytes = Encoding.UTF8.GetBytes(payload);
            if (payloadBytes.Length <= MaxIPCSize - writer.Size - BridgeRpcInlinePayloadFramingSize)
            {
                writer.Write((byte)BridgeRpcPayloadEncoding.Inline);
                writer.Write((uint)payloadBytes.Length);
                writer.WriteBytes(payloadBytes);
                return;
            }

            writer.Write((byte)BridgeRpcPayloadEncoding.Stream);
            writer.Write((uint)payloadBytes.Length);

            CancellationTokenRegistration cancellationRegistration = default;
            AddDeferredOutgoingStream(
                writer,
                deferredOutgoingStreams,
                async (identifier, transferCancellationToken) =>
                {
                    int remaining = payloadBytes.Length;
                    int offset = 0;

                    while (remaining > 0)
                    {
                        transferCancellationToken.ThrowIfCancellationRequested();

                        int chunkSize = Math.Min(StreamChunkSize, remaining);
                        if (!await StreamDataAsync(identifier, payloadBytes, offset, chunkSize, transferCancellationToken))
                            throw new Exception("Stream closed.");

                        remaining -= chunkSize;
                        offset += chunkSize;
                    }
                },
                () => cancellationRegistration.Dispose(),
                cancellationTokenSource =>
                {
                    if (cancellationToken.CanBeCanceled)
                    {
                        cancellationRegistration = cancellationToken.Register(
                            static state => ((CancellationTokenSource)state!).Cancel(),
                            cancellationTokenSource);
                    }
                });
        }

        private async Task<string> DeserializeBridgeRpcPayloadAsync(PacketReader reader, string description, CancellationToken cancellationToken = default)
        {
            BridgeRpcPayloadEncoding encoding = (BridgeRpcPayloadEncoding)reader.Read<byte>();
            uint payloadByteLength = reader.Read<uint>();
            switch (encoding)
            {
                case BridgeRpcPayloadEncoding.Inline:
                    return reader.ReadString(checked((int)payloadByteLength));
                case BridgeRpcPayloadEncoding.Stream:
                {
                    uint streamIdentifier = reader.Read<uint>();
                    DataStream stream = GetOrCreateIncomingStream(streamIdentifier);
                    bool releaseStream = false;
                    try
                    {
                        byte[] payloadBytes = new byte[checked((int)payloadByteLength)];
                        if (payloadBytes.Length > 0)
                            await ReadExactlyAsync(stream, payloadBytes, 0, payloadBytes.Length, cancellationToken);

                        string payload = Encoding.UTF8.GetString(payloadBytes);
                        releaseStream = true;
                        return payload;
                    }
                    catch (EndOfStreamException e)
                    {
                        throw new EndOfStreamException($"Data stream for {description} ended before the declared payload length.", e);
                    }
                    finally
                    {
                        if (releaseStream)
                            ReleaseIncomingStream(streamIdentifier);
                        else
                            DisposeQuietly(stream);
                    }
                }
                default:
                    throw new InvalidOperationException($"Unsupported bridge RPC payload encoding '{encoding}'.");
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
                else if (elementType == IPCProxyBodyElementType.Stream)
                {
                    long length = reader.Read<long>();
                    uint streamIdentifier = reader.Read<uint>();
                    elements.Add(new IPCProxyBodyElementStreamedBytes(GetOrCreateIncomingStream(streamIdentifier), length >= 0 ? length : null));
                }
                else
                    throw new InvalidOperationException($"Unknown proxy body element type '{elementType}'.");
            }

            return elements;
        }

        private void SerializeBodyElements(PacketWriter writer, IReadOnlyList<IPCProxyBodyElement> elements, List<(Action Start, Action Cleanup)> deferredOutgoingStreams)
        {
            writer.Write((uint)elements.Count);
            foreach (var element in elements)
            {
                switch (element)
                {
                    case IPCProxyBodyElementBytes bytesElement when bytesElement.Data.Length <= MaxIPCSize - writer.Size - ByteElementFramingSize:
                        writer.Write((byte)IPCProxyBodyElementType.Bytes);
                        writer.Write((uint)bytesElement.Data.Length);
                        writer.WriteBytes(bytesElement.Data);
                        break;
                    case IPCProxyBodyElementBytes bytesElement:
                        writer.Write((byte)IPCProxyBodyElementType.Stream);
                        writer.Write((long)bytesElement.Data.Length);
                        AddDeferredOutgoingStream(
                            writer,
                            deferredOutgoingStreams,
                            async (identifier, cancellationToken) =>
                            {
                                int remaining = bytesElement.Data.Length;
                                int offset = 0;
                                while (remaining > 0)
                                {
                                    cancellationToken.ThrowIfCancellationRequested();

                                    int chunkSize = Math.Min(StreamChunkSize, remaining);
                                    if (!await StreamDataAsync(identifier, bytesElement.Data, offset, chunkSize, cancellationToken))
                                        throw new Exception("Stream closed.");

                                    remaining -= chunkSize;
                                    offset += chunkSize;
                                }
                            });
                        break;
                    case IPCProxyBodyElementFile fileElement:
                        writer.Write((byte)IPCProxyBodyElementType.File);
                        writer.WriteSizePrefixedString(fileElement.FileName);
                        break;
                    case IPCProxyBodyElementStreamedBytes streamedBytesElement:
                        var bodyStream = streamedBytesElement.BodyStream;
                        writer.Write((byte)IPCProxyBodyElementType.Stream);
                        long? streamLength = streamedBytesElement.Length;
                        writer.Write(streamLength ?? UnknownStreamLength);
                        AddDeferredOutgoingStream(
                            writer,
                            deferredOutgoingStreams,
                            async (identifier, cancellationToken) =>
                            {
                                byte[] buffer = new byte[StreamChunkSize];
                                long totalBytesRead = 0;

                                while (!streamLength.HasValue || totalBytesRead < streamLength.Value)
                                {
                                    int requestedBytes = streamLength.HasValue
                                        ? (int)Math.Min(buffer.Length, streamLength.Value - totalBytesRead)
                                        : buffer.Length;

                                    int bytesRead = await bodyStream.ReadAsync(buffer, 0, requestedBytes, cancellationToken);
                                    if (bytesRead <= 0)
                                    {
                                        ThrowIfEndedBeforeExpectedLength(totalBytesRead, streamLength, "modified request body element");
                                        break;
                                    }

                                    cancellationToken.ThrowIfCancellationRequested();

                                    if (!await StreamDataAsync(identifier, buffer, 0, bytesRead, cancellationToken))
                                        throw new Exception("Stream closed.");

                                    totalBytesRead += bytesRead;
                                }
                            },
                            () => DisposeQuietly(bodyStream));
                        break;
                    default:
                        throw new InvalidOperationException($"Unsupported proxy body element type '{element.GetType().Name}'.");
                }
            }
        }

        private void AddDeferredOutgoingStream(
            PacketWriter writer,
            List<(Action Start, Action Cleanup)> deferredOutgoingStreams,
            Func<uint, CancellationToken, Task> transferAsync,
            Action? additionalCleanup = null,
            Action<CancellationTokenSource>? configureCancellationTokenSource = null)
        {
            uint streamIdentifier = Interlocked.Increment(ref _streamIdentifierGenerator);
            CancellationTokenSource cancellationTokenSource = new CancellationTokenSource();
            lock (_streamCancellationTokens)
            {
                _streamCancellationTokens[streamIdentifier] = cancellationTokenSource;
            }

            int cleanupState = 0;
            void Cleanup()
            {
                if (Interlocked.Exchange(ref cleanupState, 1) != 0)
                    return;

                lock (_streamCancellationTokens)
                {
                    _streamCancellationTokens.Remove(streamIdentifier);
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

                cancellationTokenSource.Dispose();
            }

            try
            {
                writer.Write(streamIdentifier);
                configureCancellationTokenSource?.Invoke(cancellationTokenSource);
            }
            catch
            {
                Cleanup();
                throw;
            }

            void Start()
            {
                if (cancellationTokenSource.IsCancellationRequested)
                {
                    Cleanup();
                    return;
                }

                QueueBackgroundStreamTask(async () =>
                {
                    bool opened = false;
                    try
                    {
                        await StreamOpenAsync(streamIdentifier, cancellationTokenSource.Token);
                        opened = true;
                        await transferAsync(streamIdentifier, cancellationTokenSource.Token);
                    }
                    catch (OperationCanceledException) when (cancellationTokenSource.IsCancellationRequested || _cancellationTokenSource.IsCancellationRequested)
                    {
                    }
                    catch (ObjectDisposedException) when (_cancellationTokenSource.IsCancellationRequested)
                    {
                    }
                    catch (Exception e)
                    {
                        Logger.Error<JustCefProcess>($"Failed to stream body", e);
                    }
                    finally
                    {
                        if (opened)
                        {
                            try
                            {
                                await StreamCloseAsync(streamIdentifier);
                            }
                            catch (Exception) when (_cancellationTokenSource.IsCancellationRequested)
                            {
                            }
                            catch (Exception e)
                            {
                                Logger.Error<JustCefProcess>($"Failed to close outgoing stream {streamIdentifier}", e);
                            }
                        }

                        Cleanup();
                    }
                });
            }

            deferredOutgoingStreams.Add((Start, Cleanup));
        }

        private void StartDeferredOutgoingStreams(IReadOnlyList<(Action Start, Action Cleanup)> deferredOutgoingStreams)
        {
            foreach (var deferredOutgoingStream in deferredOutgoingStreams)
            {
                try
                {
                    deferredOutgoingStream.Start();
                }
                catch (Exception e)
                {
                    Logger.Error<JustCefProcess>("Failed to start deferred outgoing stream.", e);
                    deferredOutgoingStream.Cleanup();
                }
            }
        }

        private void CleanupDeferredOutgoingStreams(IEnumerable<(Action Start, Action Cleanup)> deferredOutgoingStreams)
        {
            foreach (var deferredOutgoingStream in deferredOutgoingStreams)
                deferredOutgoingStream.Cleanup();
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

        private static void DisposeIncomingStreamElements(IEnumerable<IPCProxyBodyElement> elements, HashSet<DataStream>? transferredStreams = null)
        {
            foreach (var element in elements)
            {
                if (element is not IPCProxyBodyElementStreamedBytes streamedBytesElement)
                    continue;

                if (transferredStreams != null && transferredStreams.Contains(streamedBytesElement.BodyStream))
                    continue;

                try
                {
                    streamedBytesElement.BodyStream.Dispose();
                }
                catch
                {
                }
            }
        }

        private async Task HandleWindowModifyRequestAsync(PacketReader reader, PacketWriter writer, List<(Action Start, Action Cleanup)> deferredOutgoingStreams)
        {
            var identifier = reader.Read<int>();
            var window = GetWindow(identifier);
            if (window == null)
                return;

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

            IPCRequest? modifiedRequest = null;
            HashSet<DataStream>? transferredStreams = null;
            try
            {
                modifiedRequest = window.ModifyRequest(new IPCRequest
                {
                    Method = method,
                    Url = url,
                    Headers = headers,
                    Elements = elements,
                });

                if (modifiedRequest == null)
                    return;

                foreach (var element in modifiedRequest.Elements)
                {
                    if (element is not IPCProxyBodyElementStreamedBytes streamedBytesElement)
                        continue;

                    transferredStreams ??= new HashSet<DataStream>(ReferenceEqualityComparer.Instance);
                    transferredStreams.Add(streamedBytesElement.BodyStream);
                }

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

                SerializeBodyElements(writer, modifiedRequest.Elements, deferredOutgoingStreams);
            }
            finally
            {
                DisposeIncomingStreamElements(elements, transferredStreams);
            }
        }

        private async Task HandleWindowBridgeRpcAsync(PacketReader reader, PacketWriter writer, List<(Action Start, Action Cleanup)> deferredOutgoingStreams)
        {
            int identifier = reader.Read<int>();
            var window = GetWindow(identifier);
            if (window == null)
            {
                writer.Write(false);
                SerializeBridgeRpcPayload(writer, "Bridge RPC target window no longer exists.", deferredOutgoingStreams, _cancellationTokenSource.Token);
                return;
            }

            string? method = reader.ReadSizePrefixedString();
            if (string.IsNullOrWhiteSpace(method))
            {
                writer.Write(false);
                SerializeBridgeRpcPayload(writer, "Bridge RPC method must be a non-empty string.", deferredOutgoingStreams, _cancellationTokenSource.Token);
                return;
            }

            try
            {
                string json = await DeserializeBridgeRpcPayloadAsync(reader, "bridge RPC request payload", _cancellationTokenSource.Token);
                string? resultJson = await window.InvokeBridgeRpcAsync(method, json);
                writer.Write(true);
                SerializeBridgeRpcPayload(writer, resultJson ?? "null", deferredOutgoingStreams, _cancellationTokenSource.Token);
            }
            catch (Exception e)
            {
                Logger.Error<JustCefProcess>("Exception occurred while processing bridge RPC", e);
                writer.Write(false);
                SerializeBridgeRpcPayload(writer, e.Message, deferredOutgoingStreams, _cancellationTokenSource.Token);
            }
        }

        private void HandleNotification(OpcodeClientNotification opcode, PacketReader reader)
        {
            Logger.Info<JustCefProcess>($"Received notification {opcode}");

            switch (opcode)
            {
                case OpcodeClientNotification.Exit:
                    Logger.Info<JustCefProcess>("CEF process is exiting.");
                    SignalExited();
                    Dispose();
                    break;
                case OpcodeClientNotification.Ready:
                    Logger.Info<JustCefProcess>("Client is ready.");
                    _readyTaskCompletionSource.SetResult();
                    break;
                case OpcodeClientNotification.WindowOpened:
                    Logger.Info<JustCefProcess>($"Window opened: {reader.Read<int>()}");
                    break;
                case OpcodeClientNotification.WindowClosed:
                    {
                        JustCefWindow? window;
                        lock (_windows)
                        {
                            var identifier = reader.Read<int>();
                            window = _windows.FirstOrDefault(v => v.Identifier == identifier);
                            if (window != null)
                            {
                                _windows.Remove(window);
                            }
                        }

                        Logger.Info<JustCefProcess>($"Window closed: {window}");
                        window?.InvokeOnClose();
                        break;
                    }
                case OpcodeClientNotification.WindowFocused:
                    GetWindow(reader.Read<int>())?.InvokeOnFocused();
                    break;
                case OpcodeClientNotification.WindowUnfocused:
                    GetWindow(reader.Read<int>())?.InvokeOnUnfocused();
                    break;
                case OpcodeClientNotification.WindowFullscreenChanged:
                {
                    int identifier = reader.Read<int>();
                    bool fullscreen = reader.Read<bool>();
                    GetWindow(identifier)?.InvokeOnFullscreenChanged(fullscreen);
                    break;
                }
                case OpcodeClientNotification.WindowLoadStart:
                {
                    int identifier = reader.Read<int>();
                    string? url = reader.ReadSizePrefixedString();
                    GetWindow(identifier)?.InvokeOnLoadStart(url);
                    break;
                }
                case OpcodeClientNotification.WindowLoadEnd:
                {
                    int identifier = reader.Read<int>();
                    string? url = reader.ReadSizePrefixedString();
                    GetWindow(identifier)?.InvokeOnLoadEnd(url);
                    break;
                }
                case OpcodeClientNotification.WindowLoadError:
                {
                    int identifier = reader.Read<int>();
                    int errorCode = reader.Read<int>();
                    string? errorText = reader.ReadSizePrefixedString();
                    string? failedUrl = reader.ReadSizePrefixedString();
                    GetWindow(identifier)?.InvokeOnLoadError(errorCode, errorText, failedUrl);
                    break;
                }
                case OpcodeClientNotification.WindowDevToolsEvent:
                {
                    int identifier = reader.Read<int>();
                    string? method = reader.ReadSizePrefixedString();
                    int paramsSize = reader.Read<int>();
                    var parameters = reader.ReadBytes(paramsSize);
                    GetWindow(identifier)?.InvokeOnDevToolsEvent(method, parameters);
                    break;
                }
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
            return await CallAsync(opcode, writer.Data, 0, writer.Size, cancellationToken);
        }

        private async Task<PacketReader> CallAsync(OpcodeController opcode, PacketWriter writer, List<(Action Start, Action Cleanup)> deferredOutgoingStreams, CancellationToken cancellationToken = default)
        {
            return await CallAsync(opcode, writer.Data, 0, writer.Size, deferredOutgoingStreams, cancellationToken);
        }

        public async Task<PacketReader> CallAsync<TRequest>(OpcodeController opcode, TRequest request, CancellationToken cancellationToken = default)
            where TRequest : struct
        {
            using var requestBody = RentedBytesFromStruct(request);
            return await CallAsync(opcode, requestBody.Buffer, 0, requestBody.Length, cancellationToken);
        }

        public async Task<TResult> CallAsync<TRequest, TResult>(OpcodeController opcode, TRequest request, CancellationToken cancellationToken = default)
            where TRequest : unmanaged
            where TResult : unmanaged
        {
            PacketReader reader;
            using (var requestBody = RentedBytesFromStruct(request))
            {
                reader = await CallAsync(opcode, requestBody.Buffer, 0, requestBody.Length, cancellationToken);
                if (reader.RemainingSize < Unsafe.SizeOf<TResult>())
                    throw new InvalidOperationException("Response does not contain enough data to fill TResult.");
            }

            return reader.Read<TResult>();
        }

        private async Task WritePacketAsync(byte[] buffer, int offset, int size, CancellationToken cancellationToken)
        {
            bool lockTaken = false;
            try
            {
                await _writeSemaphore.WaitAsync(cancellationToken);
                lockTaken = true;
                await _writer.WriteAsync(buffer, offset, size, cancellationToken);
            }
            finally
            {
                if (lockTaken)
                    _writeSemaphore.Release();
            }
        }

        private async Task<PacketReader> CallAsync(OpcodeController opcode, CancellationToken cancellationToken = default)
        {
            EnsureStarted();

            using var linkedCts = CancellationTokenSource.CreateLinkedTokenSource(_cancellationTokenSource.Token, cancellationToken);
            var requestId = Interlocked.Increment(ref _requestIdCounter);
            var pendingRequest = new TaskCompletionSource<byte[]>();

            lock (_pendingRequests)
            {
                _pendingRequests[requestId] = pendingRequest;
            }

            int packetLength = HeaderSize;
            using var rentedBuffer = new RentedBuffer<byte>(BufferPool, packetLength);

            using (var stream = new MemoryStream(rentedBuffer.Buffer, 0, packetLength))
            using (var writer = new BinaryWriter(stream))
            {
                writer.Write((uint)(packetLength - 4));
                writer.Write(requestId);
                writer.Write((byte)PacketType.Request);
                writer.Write((byte)opcode);
            }

            try
            {
                await WritePacketAsync(rentedBuffer.Buffer, 0, packetLength, linkedCts.Token);

                byte[] responseBody;
                using (linkedCts.Token.Register(() => pendingRequest.TrySetCanceled()))
                {
                    responseBody = await pendingRequest.Task;
                }

                return new PacketReader(responseBody);
            }
            finally
            {
                lock (_pendingRequests)
                {
                    _pendingRequests.Remove(requestId);
                }
            }
        }

        private async Task<PacketReader> CallAsync(OpcodeController opcode, byte[] body, CancellationToken cancellationToken = default)
        {
            return await CallAsync(opcode, body, 0, body.Length, cancellationToken);
        }

        private async Task<PacketReader> CallAsync(OpcodeController opcode, byte[] body, int offset, int size, CancellationToken cancellationToken = default)
        {
            return await CallAsync(opcode, body, offset, size, null, cancellationToken);
        }

        private async Task<PacketReader> CallAsync(OpcodeController opcode, byte[] body, int offset, int size, IReadOnlyList<(Action Start, Action Cleanup)>? deferredOutgoingStreams, CancellationToken cancellationToken = default)
        {
            EnsureStarted();

            using var linkedCts = CancellationTokenSource.CreateLinkedTokenSource(_cancellationTokenSource.Token, cancellationToken);
            var requestId = Interlocked.Increment(ref _requestIdCounter);
            var pendingRequest = new TaskCompletionSource<byte[]>();

            lock (_pendingRequests)
            {
                _pendingRequests[requestId] = pendingRequest;
            }

            int packetLength = HeaderSize + size;
            using var rentedBuffer = new RentedBuffer<byte>(BufferPool, packetLength);

            using (var stream = new MemoryStream(rentedBuffer.Buffer, 0, packetLength))
            using (var writer = new BinaryWriter(stream))
            {
                writer.Write((uint)(packetLength - 4));
                writer.Write(requestId);
                writer.Write((byte)PacketType.Request);
                writer.Write((byte)opcode);

                if (size > 0)
                    writer.Write(body, offset, size);
            }

            bool deferredStreamsStarted = false;
            try
            {
                await WritePacketAsync(rentedBuffer.Buffer, 0, packetLength, linkedCts.Token);
                if (deferredOutgoingStreams != null && deferredOutgoingStreams.Count > 0)
                {
                    StartDeferredOutgoingStreams(deferredOutgoingStreams);
                    deferredStreamsStarted = true;
                }

                byte[] responseBody;
                using (linkedCts.Token.Register(() => pendingRequest.TrySetCanceled()))
                {
                    responseBody = await pendingRequest.Task;
                }

                return new PacketReader(responseBody);
            }
            catch
            {
                if (!deferredStreamsStarted && deferredOutgoingStreams != null)
                    CleanupDeferredOutgoingStreams(deferredOutgoingStreams);

                throw;
            }
            finally
            {
                lock (_pendingRequests)
                {
                    _pendingRequests.Remove(requestId);
                }
            }
        }

        private async Task NotifyAsync(OpcodeControllerNotification opcode, byte[] body, int offset, int size, CancellationToken cancellationToken = default)
        {
            EnsureStarted();

            using var linkedCts = CancellationTokenSource.CreateLinkedTokenSource(_cancellationTokenSource.Token, cancellationToken);
            int packetLength = HeaderSize + size;
            using var rentedBuffer = new RentedBuffer<byte>(BufferPool, packetLength);

            using (var stream = new MemoryStream(rentedBuffer.Buffer, 0, packetLength))
            using (var writer = new BinaryWriter(stream))
            {
                writer.Write((uint)(packetLength - 4));
                writer.Write((uint)0);
                writer.Write((byte)PacketType.Notification);
                writer.Write((byte)opcode);

                if (size > 0)
                    writer.Write(body, offset, size);
            }

            await WritePacketAsync(rentedBuffer.Buffer, 0, packetLength, linkedCts.Token);
        }

        private async Task NotifyAsync(OpcodeControllerNotification opcode, CancellationToken cancellationToken = default)
        {
            EnsureStarted();

            using var linkedCts = CancellationTokenSource.CreateLinkedTokenSource(_cancellationTokenSource.Token, cancellationToken);
            int packetLength = HeaderSize;
            using var rentedBuffer = new RentedBuffer<byte>(BufferPool, packetLength);

            using (var stream = new MemoryStream(rentedBuffer.Buffer, 0, packetLength))
            using (var writer = new BinaryWriter(stream))
            {
                writer.Write((uint)(packetLength - 4));
                writer.Write((uint)0);
                writer.Write((byte)PacketType.Notification);
                writer.Write((byte)opcode);
            }

            await WritePacketAsync(rentedBuffer.Buffer, 0, packetLength, linkedCts.Token);
        }

        public async Task EchoAsync(byte[] data, CancellationToken cancellationToken = default) => await CallAsync(OpcodeController.Echo, data, cancellationToken);
        public async Task PingAsync(CancellationToken cancellationToken = default) => await CallAsync(OpcodeController.Ping, cancellationToken);

        public async Task PrintAsync(string message, CancellationToken cancellationToken = default) => await CallAsync(OpcodeController.Print, Encoding.UTF8.GetBytes(message), cancellationToken);

        private void EnsureStarted()
        {
            if (!_started)
                throw new Exception("Process should be started.");
        }

        public async Task<JustCefWindow> CreateWindowAsync(string url, int minimumWidth, int minimumHeight, int preferredWidth = 0, int preferredHeight = 0,
            bool fullscreen = false, bool contextMenuEnable = false, bool shown = true, bool developerToolsEnabled = false, bool resizable = true, bool frameless = false,
            bool centered = true, bool proxyRequests = false, bool logConsole = false, Func<JustCefWindow, IPCRequest, Task<IPCResponse?>>? requestProxy = null, bool modifyRequests = false, Func<JustCefWindow, IPCRequest, IPCRequest?>? requestModifier = null, bool modifyRequestBody = false,
            string? title = null, string? iconPath = null, string? appId = null, CancellationToken cancellationToken = default, bool bridgeEnabled = false,
            Func<JustCefWindow, string, string?, Task<string?>>? bridgeRpcHandler = null)
        {
            EnsureStarted();

            if (bridgeRpcHandler != null && !bridgeEnabled)
                throw new ArgumentException("When bridgeRpcHandler is provided, bridgeEnabled must be true.", nameof(bridgeRpcHandler));

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

            var reader = await CallAsync(OpcodeController.WindowCreate, writer.Data, 0, writer.Size, cancellationToken);
            var window = new JustCefWindow(this, reader.Read<int>(), requestModifier, requestProxy, bridgeRpcHandler);
            lock (_windows)
            {
                _windows.Add(window);
            }
            return window;
        }

        public async Task NotifyExitAsync(CancellationToken cancellationToken = default)
        {
            await NotifyAsync(OpcodeControllerNotification.Exit, cancellationToken);
        }

        public async Task StreamOpenAsync(uint identifier, CancellationToken cancellationToken = default)
        {
            var packetSize = sizeof(uint);
            byte[] packet = BufferPool.Rent(packetSize);

            try
            {
                using (var stream = new MemoryStream(packet, 0, packetSize))
                using (var writer = new BinaryWriter(stream))
                {
                    writer.Write(identifier);
                }

                await CallAsync(OpcodeController.StreamOpen, packet, 0, packetSize, cancellationToken);
            }
            finally
            {
                BufferPool.Return(packet);
            }
        }

        public async Task<bool> StreamDataAsync(uint identifier, byte[] data, int offset, int size, CancellationToken cancellationToken = default)
        {
            var packetSize = size + sizeof(uint);
            byte[] packet = BufferPool.Rent(packetSize);

            try
            {
                using (var stream = new MemoryStream(packet, 0, packetSize))
                using (var writer = new BinaryWriter(stream))
                {
                    writer.Write(identifier);
                    writer.Write(data, offset, size);
                }

                var response = await CallAsync(OpcodeController.StreamData, packet, 0, packetSize, cancellationToken);
                return response.Read<bool>();
            }
            finally
            {
                BufferPool.Return(packet);
            }
        }

        public async Task StreamCloseAsync(uint identifier, CancellationToken cancellationToken = default)
        {
            var packetSize = sizeof(uint);
            byte[] packet = BufferPool.Rent(packetSize);

            try
            {
                using (var stream = new MemoryStream(packet, 0, packetSize))
                using (var writer = new BinaryWriter(stream))
                {
                    writer.Write(identifier);
                }

                await CallAsync(OpcodeController.StreamClose, packet, 0, packetSize, cancellationToken);
            }
            finally
            {
                BufferPool.Return(packet);
            }
        }

        private async Task StreamCancelAsync(uint identifier, CancellationToken cancellationToken = default)
        {
            var packetSize = sizeof(uint);
            byte[] packet = BufferPool.Rent(packetSize);

            try
            {
                using (var stream = new MemoryStream(packet, 0, packetSize))
                using (var writer = new BinaryWriter(stream))
                {
                    writer.Write(identifier);
                }

                await CallAsync(OpcodeController.StreamCancel, packet, 0, packetSize, cancellationToken);
            }
            finally
            {
                BufferPool.Return(packet);
            }
        }

        public void WaitForExit()
        {
            EnsureStarted();
            _exitTaskCompletionSource.Task.GetAwaiter().GetResult();
        }

        public async Task WaitForExitAsync(CancellationToken cancellationToken = default)
        {
            EnsureStarted();
            await _exitTaskCompletionSource.Task.WaitAsync(cancellationToken);
        }

        public void WaitForReady()
        {
            EnsureStarted();
            _readyTaskCompletionSource.Task.Wait();
        }

        public async Task WaitForReadyAsync(CancellationToken cancellationToken = default)
        {
            EnsureStarted();
            await Task.WhenAny(_readyTaskCompletionSource.Task, Task.Delay(Timeout.Infinite, cancellationToken));
        }

        public async Task WindowMaximizeAsync(int identifier, CancellationToken cancellationToken = default) 
            => await CallAsync(OpcodeController.WindowMaximize, new PacketWriter().Write(identifier), cancellationToken);

        public async Task WindowMinimizeAsync(int identifier, CancellationToken cancellationToken = default) 
            => await CallAsync(OpcodeController.WindowMinimize, new PacketWriter().Write(identifier), cancellationToken);

        public async Task WindowRestoreAsync(int identifier, CancellationToken cancellationToken = default) 
            => await CallAsync(OpcodeController.WindowRestore, new PacketWriter().Write(identifier), cancellationToken);

        public async Task WindowShowAsync(int identifier, CancellationToken cancellationToken = default) 
            => await CallAsync(OpcodeController.WindowShow, new PacketWriter().Write(identifier), cancellationToken);

        public async Task WindowHideAsync(int identifier, CancellationToken cancellationToken = default) 
            => await CallAsync(OpcodeController.WindowHide, new PacketWriter().Write(identifier), cancellationToken);

        public async Task WindowActivateAsync(int identifier, CancellationToken cancellationToken = default) 
            => await CallAsync(OpcodeController.WindowActivate, new PacketWriter().Write(identifier), cancellationToken);

        public async Task WindowBringToTopAsync(int identifier, CancellationToken cancellationToken = default) 
            => await CallAsync(OpcodeController.WindowBringToTop, new PacketWriter().Write(identifier), cancellationToken);

        public async Task WindowSetAlwaysOnTopAsync(int identifier, bool alwaysOnTop, CancellationToken cancellationToken = default)
        {
            await CallAsync(OpcodeController.WindowSetAlwaysOnTop, new PacketWriter().Write(identifier).Write(alwaysOnTop), cancellationToken);
        }

        public async Task WindowSetFullscreenAsync(int identifier, bool fullscreen, CancellationToken cancellationToken = default)
        {
            await CallAsync(OpcodeController.WindowSetFullscreen, new PacketWriter()
                .Write(identifier)
                .Write(fullscreen), cancellationToken);
        }

        public async Task WindowCenterSelfAsync(int identifier, CancellationToken cancellationToken = default)
        {
            await CallAsync(OpcodeController.WindowCenterSelf, new PacketWriter()
                .Write(identifier), cancellationToken);
        }

        public async Task WindowSetProxyRequestsAsync(int identifier, bool enableProxyRequests, CancellationToken cancellationToken = default)
        {
            await CallAsync(OpcodeController.WindowSetProxyRequests, new PacketWriter()
                .Write(identifier)
                .Write(enableProxyRequests), cancellationToken);
        }

        public async Task WindowSetModifyRequestsAsync(int identifier, bool enableModifyRequests, bool enableModifyBody, CancellationToken cancellationToken = default)
        {
            await CallAsync(OpcodeController.WindowSetModifyRequests, new PacketWriter()
                .Write(identifier).Write((byte)(((enableModifyBody ? 1 : 0) << 1) | (enableModifyRequests ? 1 : 0))), cancellationToken);
        }

        public async Task RequestFocusAsync(int identifier, CancellationToken cancellationToken = default)
        {
            await CallAsync(OpcodeController.WindowRequestFocus, new PacketWriter()
                .Write(identifier), cancellationToken);
        }

        public async Task WindowLoadUrlAsync(int identifier, string url, CancellationToken cancellationToken = default)
        {
            await CallAsync(OpcodeController.WindowLoadUrl, new PacketWriter()
                .Write(identifier)
                .WriteSizePrefixedString(url), cancellationToken);
        }

        public async Task WindowSetPositionAsync(int identifier, int x, int y, CancellationToken cancellationToken = default)
        {
            await CallAsync(OpcodeController.WindowSetPosition, new PacketWriter()
                .Write(identifier)
                .Write(x)
                .Write(y), cancellationToken);
        }

        public async Task<(int X, int Y)> WindowGetPositionAsync(int identifier, CancellationToken cancellationToken = default)
        {
            var reader = await CallAsync(OpcodeController.WindowGetPosition, new PacketWriter().Write(identifier), cancellationToken);
            var x = reader.Read<int>();
            var y = reader.Read<int>();
            return (x, y);
        }

        public async Task WindowSetSizeAsync(int identifier, int width, int height, CancellationToken cancellationToken = default)
        {
            await CallAsync(OpcodeController.WindowSetSize, new PacketWriter()
                .Write(identifier)
                .Write(width)
                .Write(height), cancellationToken);
        }

        public async Task<(int Width, int Height)> WindowGetSizeAsync(int identifier, CancellationToken cancellationToken = default)
        {
            var reader = await CallAsync(OpcodeController.WindowGetSize, new PacketWriter().Write(identifier), cancellationToken);
            var width = reader.Read<int>();
            var height = reader.Read<int>();
            return (width, height);
        }

        public async Task WindowSetZoomAsync(int identifier, double zoom, CancellationToken cancellationToken = default)
        {
            await CallAsync(OpcodeController.WindowSetZoom, new PacketWriter()
                .Write(identifier)
                .Write(zoom), cancellationToken);
        }

        public async Task<double> WindowGetZoomAsync(int identifier, CancellationToken cancellationToken = default)
        {
            var reader = await CallAsync(OpcodeController.WindowGetZoom, new PacketWriter().Write(identifier), cancellationToken);
            return reader.Read<double>();
        }

        public async Task WindowSetDevelopmentToolsEnabledAsync(int identifier, bool developmentToolsEnabled, CancellationToken cancellationToken = default)
        {
            await CallAsync(OpcodeController.WindowSetDevelopmentToolsEnabled, new PacketWriter()
                .Write(identifier)
                .Write(developmentToolsEnabled), cancellationToken);
        }

        public async Task WindowSetDevelopmentToolsVisibleAsync(int identifier, bool developmentToolsVisible, CancellationToken cancellationToken = default)
        {
            await CallAsync(OpcodeController.WindowSetDevelopmentToolsVisible, new PacketWriter()
                .Write(identifier)
                .Write(developmentToolsVisible), cancellationToken);
        }

        public async Task WindowCloseAsync(int identifier, bool forceClose = false, CancellationToken cancellationToken = default)
        {
            await CallAsync(OpcodeController.WindowClose, new PacketWriter()
                .Write(identifier)
                .Write(forceClose), cancellationToken);
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

            var reader = await CallAsync(OpcodeController.PickFile, writer, cancellationToken);
            uint pathCount = reader.Read<uint>();
            string[] paths = new string[(int)pathCount];
            for (int i = 0; i < pathCount; i++)
                paths[i] = reader.ReadSizePrefixedString()!;

            return paths;
        }

        public async Task<string> WindowPickDirectoryAsync(int identifier, CancellationToken cancellationToken = default)
        {
            var reader = await CallAsync(OpcodeController.PickDirectory, new PacketWriter().Write(identifier), cancellationToken);
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

            var reader = await CallAsync(OpcodeController.SaveFile, writer, cancellationToken);
            return reader.ReadSizePrefixedString()!;
        }

        public async Task<(bool Succes, byte[] Data)> WindowExecuteDevToolsMethodAsync(int identifier, string methodName, string? json = null,  CancellationToken cancellationToken = default)
        {
            var writer = new PacketWriter();
            writer.Write(identifier);
            writer.WriteSizePrefixedString(methodName);
            if (json != null)
                writer.WriteSizePrefixedString(json);

            var reader = await CallAsync(OpcodeController.WindowExecuteDevToolsMethod, writer, cancellationToken);
            var success = reader.Read<bool>();
            var resultSize = reader.Read<uint>();
            var result = reader.ReadBytes((int)resultSize);
            return (success, result);
        }

        public async Task<string> WindowBridgeRpcAsync(int identifier, string method, string? json = null, CancellationToken cancellationToken = default)
        {
            if (string.IsNullOrWhiteSpace(method))
                throw new ArgumentException("Bridge RPC method must be a non-empty string.", nameof(method));

            var writer = new PacketWriter();
            writer.Write(identifier);
            writer.WriteSizePrefixedString(method);
            var deferredOutgoingStreams = new List<(Action Start, Action Cleanup)>();
            SerializeBridgeRpcPayload(writer, json ?? "null", deferredOutgoingStreams, cancellationToken);

            var reader = await CallAsync(OpcodeController.WindowBridgeRpc, writer, deferredOutgoingStreams, cancellationToken);
            bool success = reader.Read<bool>();
            string payload = await DeserializeBridgeRpcPayloadAsync(reader, success ? "bridge RPC response payload" : "bridge RPC error payload", cancellationToken);
            if (!success)
                throw new InvalidOperationException(payload);

            return payload;
        }

        public async Task WindowSetTitleAsync(int identifier, string title, CancellationToken cancellationToken = default)
        {
            await CallAsync(OpcodeController.WindowSetTitle, new PacketWriter()
                .Write(identifier)
                .WriteSizePrefixedString(title), cancellationToken);
        }

        public async Task WindowSetIconAsync(int identifier, string iconPath, CancellationToken cancellationToken = default)
        {
            await CallAsync(OpcodeController.WindowSetIcon, new PacketWriter()
                .Write(identifier)
                .WriteSizePrefixedString(iconPath), cancellationToken);
        }

        public async Task WindowAddUrlToProxyAsync(int identifier, string url, CancellationToken cancellationToken = default)
        {
            await CallAsync(OpcodeController.WindowAddUrlToProxy, new PacketWriter()
                .Write(identifier)
                .WriteSizePrefixedString(url), cancellationToken);
        }

        public async Task WindowRemoveUrlToProxyAsync(int identifier, string url, CancellationToken cancellationToken = default)
        {
            await CallAsync(OpcodeController.WindowRemoveUrlToProxy, new PacketWriter()
                .Write(identifier)
                .WriteSizePrefixedString(url), cancellationToken);
        }

        public async Task WindowAddDomainToProxyAsync(int identifier, string url, CancellationToken cancellationToken = default)
        {
            await CallAsync(OpcodeController.WindowAddDomainToProxy, new PacketWriter()
                .Write(identifier)
                .WriteSizePrefixedString(url), cancellationToken);
        }

        public async Task WindowRemoveDomainToProxyAsync(int identifier, string url, CancellationToken cancellationToken = default)
        {
            await CallAsync(OpcodeController.WindowRemoveDomainToProxy, new PacketWriter()
                .Write(identifier)
                .WriteSizePrefixedString(url), cancellationToken);
        }

        public async Task WindowAddUrlToModifyAsync(int identifier, string url, CancellationToken cancellationToken = default)
        {
            await CallAsync(OpcodeController.WindowAddUrlToModify, new PacketWriter()
                .Write(identifier)
                .WriteSizePrefixedString(url), cancellationToken);
        }

        public async Task WindowRemoveUrlToModifyAsync(int identifier, string url, CancellationToken cancellationToken = default)
        {
            await CallAsync(OpcodeController.WindowRemoveUrlToModify, new PacketWriter()
                .Write(identifier)
                .WriteSizePrefixedString(url), cancellationToken);
        }

        public async Task WindowAddDevToolsEventMethod(int identifier, string method, CancellationToken cancellationToken = default)
        {
            await CallAsync(OpcodeController.WindowAddDevToolsEventMethod, new PacketWriter()
                .Write(identifier)
                .WriteSizePrefixedString(method), cancellationToken);
        }

        public async Task WindowRemoveDevToolsEventMethod(int identifier, string method, CancellationToken cancellationToken = default)
        {
            await CallAsync(OpcodeController.WindowRemoveDevToolsEventMethod, new PacketWriter()
                .Write(identifier)
                .WriteSizePrefixedString(method), cancellationToken);
        }

        public void Dispose()
        {
            _cancellationTokenSource.Cancel();

            SignalExited();

            _childProcess?.Close();

            _readyTaskCompletionSource.TrySetCanceled();

            //TODO: Can this deadlock due to invoked originating from TrySetCanceled? seems unlikely
            lock (_pendingRequests)
            {
                foreach (var pendingRequest in _pendingRequests)
                    pendingRequest.Value.TrySetCanceled();
                _pendingRequests.Clear();
            }

            lock (_windows)
            {
                var windowsToClose = _windows.ToArray();
                foreach (var window in windowsToClose)
                    window.InvokeOnClose();
                _windows.Clear();
            }

            lock (_streamCancellationTokens)
            {
                foreach (var pair in _streamCancellationTokens)
                    pair.Value.Cancel();
                _streamCancellationTokens.Clear();
            }

            DataStream[] streamsToDispose;
            lock (_incomingStreams)
            {
                streamsToDispose = _incomingStreams.Values.ToArray();
                _incomingStreams.Clear();
                _canceledIncomingStreams.Clear();
            }

            foreach (var stream in streamsToDispose)
                stream.Dispose();

            WaitForIncomingStreamDispatchers(1000);
            WaitForBackgroundStreamTasks(1000);

            _writer.Dispose();
            _reader.Dispose();
            _writeSemaphore.Dispose();
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
