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
using DotCef;

namespace DotCef
{
    public class DotCefProcess : IDisposable
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
            //WindowSetZoom = 9,
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
            StreamOpen = 35,  //TODO: Make sure that StreamOpen, Close, Data all get processed in order
            StreamClose = 36,
            StreamData = 37,
            PickFile = 38,
            PickDirectory = 39,
            SaveFile = 40,
            WindowExecuteDevToolsMethod = 41,
            WindowSetDevelopmentToolsVisible = 42,
            WindowSetTitle = 43,
            WindowSetIcon = 44,
            WindowAddUrlToProxy = 45,
            WindowRemoveUrlToProxy = 46,
            WindowAddUrlToModify = 47,
            WindowRemoveUrlToModify = 48,
            WindowGetSize = 49,
            WindowSetSize = 50,
            WindowAddDevToolsEventMethod = 51,
            WindowRemoveDevToolsEventMethod = 52,
            WindowAddDomainToProxy = 53,
            WindowRemoveDomainToProxy = 54
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
            StreamClose = 5
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

        [StructLayout(LayoutKind.Sequential, Pack = 1)]
        private struct IPCPacketHeader
        {
            public uint Size;
            public uint RequestId;
            public PacketType PacketType;
            public byte Opcode;
        }

        private class PendingRequest
        {
            public OpcodeController Opcode;
            public uint RequestId;
            public readonly TaskCompletionSource<byte[]> ResponseBodyTaskCompletionSource = new TaskCompletionSource<byte[]>();

            public PendingRequest(OpcodeController opcode, uint requestId)
            {
                Opcode = opcode;
                RequestId = requestId;
            }
        }

        private static ArrayPool<byte> BufferPool = ArrayPool<byte>.Create();
        private readonly TaskCompletionSource _readyTaskCompletionSource = new TaskCompletionSource();

        private const int MaxIPCSize = 10 * 1024 * 1024;
        private const int HeaderSize = 4 + 4 + 1 + 1;
        private readonly AnonymousPipeServerStream _writer;
        private readonly AnonymousPipeServerStream _reader;
        private readonly Dictionary<uint, PendingRequest> _pendingRequests = new Dictionary<uint, PendingRequest>();
        private Process? _childProcess;
        private bool _started = false;
        private SemaphoreSlim _writeSemaphore = new SemaphoreSlim(1);
        private uint _requestIdCounter = 0;
        private readonly List<DotCefWindow> _windows = new List<DotCefWindow>();
        private CancellationTokenSource _cancellationTokenSource = new CancellationTokenSource();
        private uint _streamIdentifierGenerator = 0;
        private Dictionary<uint, CancellationTokenSource> _streamCancellationTokens = new Dictionary<uint, CancellationTokenSource>();

        public List<DotCefWindow> Windows
        {
            get
            {
                lock (_windows)
                {
                    return _windows.ToList();
                }
            }
        }

        public DotCefWindow? GetWindow(int identifier)
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

        public DotCefProcess()
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
            
            string? nativePath = null;

#if !HARDCODED_PATHS
            string[] searchPaths = GenerateSearchPaths();
            Logger.Info<DotCefProcess>("Searching for dotcefnative, search paths:");
            foreach (var path in searchPaths)
                Logger.Info<DotCefProcess>(" - " + path);

            foreach (string path in searchPaths)
            {
                
                if (File.Exists(path))
                    nativePath = path;                    
            }

            if (nativePath == null)
                throw new Exception("Failed to find dotcefnative");

            var workingDirectory = GetDirectory(nativePath);
            Logger.Info<DotCefProcess>($"Working directory '{workingDirectory}'.");
            Logger.Info<DotCefProcess>($"CEF exe path '{nativePath}'.");

            if (!File.Exists(nativePath))
            {
                Logger.Error<DotCefProcess>($"File not found at native path '{nativePath}'.");
                throw new Exception("Native executable not found.");
            }
#else
            Logger.Info<DotCefProcess>($"USING HARDCODED PATHS.");
#endif

            ProcessStartInfo psi = new ProcessStartInfo
            {
#if HARDCODED_PATHS
                FileName = OperatingSystem.IsMacOS()
                    ? "/Users/koen/Projects/Grayjay.Desktop/JustCef/native/build/Debug/dotcefnative.app/Contents/MacOS/dotcefnative"
                    : OperatingSystem.IsWindows() 
                        ? """C:\Users\Koen\Projects\Grayjay.Desktop\JustCef\native\build\Release\dotcefnative.exe"""
                        : "/home/koen/Projects/JustCef/native/build/Debug/dotcefnative",
                WorkingDirectory = OperatingSystem.IsMacOS()
                    ? "/Users/koen/Projects/Grayjay.Desktop/JustCef/native/build/Debug/"
                    : OperatingSystem.IsWindows() 
                        ? """C:\Users\Koen\Projects\Grayjay.Desktop\JustCef\native\build\Release\"""
                        : "/home/koen/Projects/JustCef/native/build/Debug/",
#else
                FileName = nativePath,
                WorkingDirectory = workingDirectory,
#endif   
                Arguments = $"--change-stack-guard-on-fork=disable --parent-to-child {_writer.GetClientHandleAsString()} --child-to-parent {_reader.GetClientHandleAsString()}" + ((string.IsNullOrEmpty(args)) ? "" : " " + args),
                UseShellExecute = false,
                RedirectStandardError = true,
                RedirectStandardOutput = true
            };

            Logger.Info<DotCefProcess>(psi.Arguments);

            var process = new Process();
            process.StartInfo = psi;
            process.ErrorDataReceived += (_, args) =>
            {
                var d = args?.Data;
                if (d != null)
                    Logger.Info<DotCefProcess>(d);
            };
            process.OutputDataReceived += (_, args) =>
            {
                var d = args?.Data;
                if (d != null)
                    Logger.Info<DotCefProcess>(d);
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
                    Logger.Info<DotCefProcess>("Receive loop started.");

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
                            Logger.Error<DotCefProcess>("Invalid packet size. Shutting down.");
                            Dispose();
                            return;
                        }

                        RentedBuffer<byte>? rentedBodyBuffer = null;
                        if (bodySize > 0)
                        {
                            var rb = new RentedBuffer<byte>(BufferPool, bodySize);
                            await _reader.ReadExactlyAsync(rb.Buffer, 0, bodySize, _cancellationTokenSource.Token);
                            rentedBodyBuffer = rb;
                        }

                        _ = Task.Run(async () =>
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
                                    }

                                    if (foundPendingRequest && pendingRequest != null)
                                        pendingRequest.ResponseBodyTaskCompletionSource.SetResult(rentedBodyBuffer != null ? rentedBodyBuffer.Value.Buffer.AsSpan().Slice(0, rentedBodyBuffer.Value.Length).ToArray() : Array.Empty<byte>());
                                    else
                                        Logger.Error<DotCefProcess>($"Received a packet response for a request that no longer has an awaiter (request id = {requestId}).");
                                }
                                else if (packetType == PacketType.Request)
                                {
                                    var packetReader = new PacketReader(rentedBodyBuffer != null ? rentedBodyBuffer.Value.Buffer : Array.Empty<byte>(), rentedBodyBuffer != null ? rentedBodyBuffer.Value.Length : 0);
                                    var packetWriter = new PacketWriter();
                                    await HandleRequestAsync((OpcodeClient)opcode, packetReader, packetWriter);
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
                                        await _writeSemaphore.WaitAsync(_cancellationTokenSource.Token);
                                        await _writer.WriteAsync(rentedBuffer.Buffer, 0, packetSize, _cancellationTokenSource.Token);
                                    }
                                    finally
                                    {
                                        _writeSemaphore.Release();
                                    }
                                }
                                else if (packetType == PacketType.Notification)
                                {
                                    var packetReader = new PacketReader(rentedBodyBuffer != null ? rentedBodyBuffer.Value.Buffer : Array.Empty<byte>(), rentedBodyBuffer != null ? rentedBodyBuffer.Value.Length : 0);
                                    HandleNotification((OpcodeClientNotification)opcode, packetReader);
                                }
                            }
                            catch (Exception e)
                            {
                                Logger.Error<DotCefProcess>($"An exception occurred in the IPC while handling a packet", e);
                                //TODO: If packetType == PacketType.Request, write back an error?
                            }
                            finally
                            {
                                rentedBodyBuffer?.Dispose();
                            }
                        });
                    }
                }
                catch (Exception e)
                {
                    Logger.Error<DotCefProcess>($"An exception occurred in the IPC", e);
                }
                finally
                {
                    Logger.Info<DotCefProcess>("Receive loop stopped.");
                    Dispose();
                }


            });
        }

        private async Task HandleRequestAsync(OpcodeClient opcode, PacketReader reader, PacketWriter writer)
        {
            try
            {
                switch (opcode)
                {
                    case OpcodeClient.Ping:
                        break;
                    case OpcodeClient.Print:
                        Logger.Info<DotCefProcess>(reader.ReadString(reader.RemainingSize));
                        break;
                    case OpcodeClient.Echo:
                        writer.WriteBytes(reader.ReadBytes(reader.RemainingSize));
                        break;
                    case OpcodeClient.WindowProxyRequest:
                        await HandleWindowProxyRequestAsync(reader, writer);
                        break;
                    case OpcodeClient.WindowModifyRequest:
                        HandleWindowModifyRequest(reader, writer);
                        break;
                    case OpcodeClient.StreamClose:
                        uint identifier = reader.Read<uint>();
                        lock (_streamCancellationTokens)
                        {
                            //Console.WriteLine($"Stream closed {identifier}.");
                            if (_streamCancellationTokens.TryGetValue(identifier, out var token))
                            {
                                token.Cancel();
                                _streamCancellationTokens.Remove(identifier);
                            }
                        }
                        break;
                    default:
                        Logger.Warning<DotCefProcess>($"Received unhandled opcode {opcode}.");
                        break;
                }
            }
            catch (Exception e)
            {
                Logger.Error<DotCefProcess>($"Exception occurred while processing call", e);
                Debugger.Break();
            }
        }

        private async Task HandleWindowProxyRequestAsync(PacketReader reader, PacketWriter writer)
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
            uint elementCount = reader.Read<uint>();
            var elements = new List<IPCProxyBodyElement>();
            for (uint i = 0; i < elementCount; i++)
            {
                IPCProxyBodyElementType elementType = (IPCProxyBodyElementType)reader.Read<byte>();
                if (elementType == IPCProxyBodyElementType.Bytes)
                {
                    uint dataSize = reader.Read<uint>();
                    byte[] data = reader.ReadBytes((int)dataSize);
                    elements.Add(new IPCProxyBodyElementBytes
                    {
                        Type = elementType,
                        Data = data
                    });
                }
                else if (elementType == IPCProxyBodyElementType.File)
                {
                    string fileName = reader.ReadSizePrefixedString()!;
                    elements.Add(new IPCProxyBodyElementFile
                    {
                        Type = elementType,
                        FileName = fileName
                    });
                }
            }

            var response = await window.ProxyRequestAsync(new IPCRequest
            {
                Method = method,
                Url = url,
                Headers = headers,
                Elements = elements,
            });

            if (response == null)
                return;

            var responseHeaders = new Dictionary<string, List<string>>(response.Headers.Where(header =>
            {
                if (string.Equals(header.Key, "transfer-encoding", StringComparison.InvariantCultureIgnoreCase) && header.Value.Any(v => string.Equals(v, "chunked", StringComparison.InvariantCultureIgnoreCase)))
                {
                    return false;
                }

                return true;
            }), StringComparer.OrdinalIgnoreCase);

            writer.Write((uint)response.StatusCode);
            writer.WriteSizePrefixedString(response.StatusText);

            // Serialize headers
            writer.Write(responseHeaders.Count());
            foreach (var header in responseHeaders)
            {
                //Do not add transfer-encoding header
                if (string.Equals(header.Key, "transfer-encoding", StringComparison.InvariantCultureIgnoreCase) && header.Value.Any(v => string.Equals(v, "chunked", StringComparison.InvariantCultureIgnoreCase)))
                    continue;

                writer.WriteSizePrefixedString(header.Key);
                writer.WriteSizePrefixedString(string.Join(", ", header.Value));
            }

            bool hasContentType = responseHeaders.ContainsKey("content-type");
            bool isHead = string.Compare(method, "head", true) == 0;
            int? contentLength = response.Headers.TryGetValue("content-length", out var contentLengths) && contentLengths.Count > 0 ? int.Parse(contentLengths[0]) : null;

            if (response.BodyStream != null)
            {
                if (contentLength != null)
                {
                    if (contentLength < (int)(MaxIPCSize - writer.Size))
                    {
                        writer.Write((byte)1);
                        writer.Write((uint)contentLength.Value);

                        byte[] buffer = ArrayPool<byte>.Shared.Rent(contentLength.Value);
                        try
                        {
                            await response.BodyStream.ReadExactlyAsync(buffer, 0, contentLength.Value);
                            writer.WriteBytes(buffer, 0, contentLength.Value);
                        }
                        finally
                        {
                            ArrayPool<byte>.Shared.Return(buffer);
                        }
                    }
                    else
                    {
                        writer.Write((byte)2);
                        await HandleLargeOrChunkedContentAsync(response.BodyStream, writer, contentLength);
                    }
                }
                else
                { 
                    writer.Write((byte)2);
                    await HandleLargeOrChunkedContentAsync(response.BodyStream, writer, null);
                }
            }
            else
                writer.Write((byte)0);
        }

        private async Task HandleLargeOrChunkedContentAsync(Stream stream, PacketWriter writer, long? contentLength = null)
        {
            uint streamIdentifier = Interlocked.Increment(ref _streamIdentifierGenerator);
            CancellationTokenSource cancellationTokenSource = new CancellationTokenSource();
            lock (_streamCancellationTokens)
            {
                //Console.WriteLine($"Stream opened {streamIdentifier}.");
                _streamCancellationTokens[streamIdentifier] = cancellationTokenSource;
            }

            await StreamOpenAsync(streamIdentifier, cancellationTokenSource.Token);
            writer.Write(streamIdentifier);

            _ = Task.Run(async () =>
            {
                try
                {
                    byte[] buffer = new byte[65536];
                    int bytesRead;
                    long totalBytesRead = 0;

                    while ((bytesRead = await stream.ReadAsync(buffer, 0, contentLength != null ? (int)Math.Min(buffer.Length, contentLength.Value - totalBytesRead) : buffer.Length, cancellationTokenSource.Token)) > 0)
                    {
                        cancellationTokenSource.Token.ThrowIfCancellationRequested();

                        if (!await StreamDataAsync(streamIdentifier, buffer, 0, bytesRead, cancellationTokenSource.Token))
                            throw new Exception("Stream closed.");

                        totalBytesRead += bytesRead;

                        if (contentLength.HasValue && totalBytesRead >= contentLength.Value)
                            break;
                    }
                }
                catch (Exception e)
                {
                    Logger.Error<DotCefProcess>($"Failed to stream body", e);
                }
                finally
                {
                    stream.Close();
                    stream.Dispose();
                    await StreamCloseAsync(streamIdentifier);

                    lock (_streamCancellationTokens)
                    {
                        //Console.WriteLine($"Stream closed in finally {streamIdentifier}.");
                        _streamCancellationTokens.Remove(streamIdentifier);
                    }
                }
            });
        }

        private void HandleWindowModifyRequest(PacketReader reader, PacketWriter writer)
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
            uint elementCount = reader.Read<uint>();
            var elements = new List<IPCProxyBodyElement>();
            for (uint i = 0; i < elementCount; i++)
            {
                IPCProxyBodyElementType elementType = (IPCProxyBodyElementType)reader.Read<byte>();
                if (elementType == IPCProxyBodyElementType.Bytes)
                {
                    uint dataSize = reader.Read<uint>();
                    byte[] data = reader.ReadBytes((int)dataSize);
                    elements.Add(new IPCProxyBodyElementBytes
                    {
                        Type = elementType,
                        Data = data
                    });
                }
                else if (elementType == IPCProxyBodyElementType.File)
                {
                    string fileName = reader.ReadSizePrefixedString()!;
                    elements.Add(new IPCProxyBodyElementFile
                    {
                        Type = elementType,
                        FileName = fileName
                    });
                }
            }

            var modifiedRequest = window.ModifyRequest(new IPCRequest
            {
                Method = method,
                Url = url,
                Headers = headers,
                Elements = elements,
            });

            if (modifiedRequest == null)
                return;

            writer.WriteSizePrefixedString(modifiedRequest.Method);
            writer.WriteSizePrefixedString(modifiedRequest.Url);

            // Serialize headers
            writer.Write(modifiedRequest.Headers.Count);
            foreach (var header in modifiedRequest.Headers)
            {
                foreach (var v in header.Value)
                {
                    writer.WriteSizePrefixedString(header.Key);
                    writer.WriteSizePrefixedString(v);
                }
            }

            // Serialize elements
            writer.Write((uint)modifiedRequest.Elements.Count);
            foreach (var element in modifiedRequest.Elements)   
            {
                writer.Write((byte)element.Type);
                if (element is IPCProxyBodyElementBytes b)
                {
                    writer.Write((uint)b.Data.Length);
                    writer.WriteBytes(b.Data);
                }
                else if (element is IPCProxyBodyElementFile f)
                    writer.WriteSizePrefixedString(f.FileName);
            }
        }

        private void HandleNotification(OpcodeClientNotification opcode, PacketReader reader)
        {
            Logger.Info<DotCefProcess>($"Received notification {opcode}");

            switch (opcode)
            {
                case OpcodeClientNotification.Exit:
                    Logger.Info<DotCefProcess>("CEF process is exiting.");
                    Dispose();
                    break;
                case OpcodeClientNotification.Ready:
                    Logger.Info<DotCefProcess>("Client is ready.");
                    _readyTaskCompletionSource.SetResult();
                    break;
                case OpcodeClientNotification.WindowOpened:
                    Logger.Info<DotCefProcess>($"Window opened: {reader.Read<int>()}");
                    break;
                case OpcodeClientNotification.WindowClosed:
                    {
                        DotCefWindow? window;
                        lock (_windows)
                        {
                            var identifier = reader.Read<int>();
                            window = _windows.FirstOrDefault(v => v.Identifier == identifier);
                            if (window != null)
                            {
                                _windows.Remove(window);
                            }
                        }

                        Logger.Info<DotCefProcess>($"Window closed: {window}");
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
                    Logger.Info<DotCefProcess>($"Received unhandled notification opcode {opcode}.");
                    break;
            }
        }

        public static RentedBuffer<byte> RentedBytesFromStruct<TStruct>(TStruct s) where TStruct : struct
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

        private async Task<PacketReader> CallAsync(OpcodeController opcode, CancellationToken cancellationToken = default)
        {
            EnsureStarted();

            var linkedCts = CancellationTokenSource.CreateLinkedTokenSource(_cancellationTokenSource.Token, cancellationToken);
            var requestId = Interlocked.Increment(ref _requestIdCounter);
            var pendingRequest = new PendingRequest(opcode, requestId);

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
                await _writeSemaphore.WaitAsync(linkedCts.Token);
                await _writer.WriteAsync(rentedBuffer.Buffer, 0, packetLength, linkedCts.Token);
            }
            finally
            {
                _writeSemaphore.Release();
            }

            byte[] responseBody;
            using (linkedCts.Token.Register(() => pendingRequest.ResponseBodyTaskCompletionSource.TrySetCanceled()))
            {
                try
                {
                    responseBody = await pendingRequest.ResponseBodyTaskCompletionSource.Task;
                }
                finally
                {
                    lock (_pendingRequests)
                    {
                        _pendingRequests.Remove(requestId);
                    }
                }
            }

            return new PacketReader(responseBody);
        }

        private async Task<PacketReader> CallAsync(OpcodeController opcode, byte[] body, CancellationToken cancellationToken = default)
        {
            return await CallAsync(opcode, body, 0, body.Length, cancellationToken);
        }

        private async Task<PacketReader> CallAsync(OpcodeController opcode, byte[] body, int offset, int size, CancellationToken cancellationToken = default)
        {
            EnsureStarted();

            var linkedCts = CancellationTokenSource.CreateLinkedTokenSource(_cancellationTokenSource.Token, cancellationToken);
            var requestId = Interlocked.Increment(ref _requestIdCounter);
            var pendingRequest = new PendingRequest(opcode, requestId);

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

            try
            {
                await _writeSemaphore.WaitAsync(linkedCts.Token);
                await _writer.WriteAsync(rentedBuffer.Buffer, 0, packetLength, linkedCts.Token);
            }
            finally
            {
                _writeSemaphore.Release();
            }

            byte[] responseBody;
            using (linkedCts.Token.Register(() => pendingRequest.ResponseBodyTaskCompletionSource.TrySetCanceled()))
            {
                try
                {
                    responseBody = await pendingRequest.ResponseBodyTaskCompletionSource.Task;
                }
                finally
                {
                    lock (_pendingRequests)
                    {
                        _pendingRequests.Remove(requestId);
                    }
                }
            }

            return new PacketReader(responseBody);
        }

        private async Task NotifyAsync(OpcodeControllerNotification opcode, byte[] body, int offset, int size, CancellationToken cancellationToken = default)
        {
            EnsureStarted();

            var linkedCts = CancellationTokenSource.CreateLinkedTokenSource(_cancellationTokenSource.Token, cancellationToken);
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

            try
            {
                await _writeSemaphore.WaitAsync(linkedCts.Token);
                await _writer.WriteAsync(rentedBuffer.Buffer, 0, packetLength, linkedCts.Token);
            }
            finally
            {
                _writeSemaphore.Release();
            }
        }

        private async Task NotifyAsync(OpcodeControllerNotification opcode, CancellationToken cancellationToken = default)
        {
            EnsureStarted();

            var linkedCts = CancellationTokenSource.CreateLinkedTokenSource(_cancellationTokenSource.Token, cancellationToken);
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

            try
            {
                await _writeSemaphore.WaitAsync(linkedCts.Token);
                await _writer.WriteAsync(rentedBuffer.Buffer, 0, packetLength, linkedCts.Token);
            }
            finally
            {
                _writeSemaphore.Release();
            }
        }

        public async Task EchoAsync(byte[] data, CancellationToken cancellationToken = default) => await CallAsync(OpcodeController.Echo, data, cancellationToken);
        public async Task PingAsync(CancellationToken cancellationToken = default) => await CallAsync(OpcodeController.Ping, cancellationToken);

        public async Task PrintAsync(string message, CancellationToken cancellationToken = default) => await CallAsync(OpcodeController.Print, Encoding.UTF8.GetBytes(message), cancellationToken);

        private void EnsureStarted()
        {
            if (!_started)
                throw new Exception("Process should be started.");
        }

        public async Task<DotCefWindow> CreateWindowAsync(string url, int minimumWidth, int minimumHeight, int preferredWidth = 0, int preferredHeight = 0,
            bool fullscreen = false, bool contextMenuEnable = false, bool shown = true, bool developerToolsEnabled = false, bool resizable = true, bool frameless = false,
            bool centered = true, bool proxyRequests = false, bool logConsole = false, Func<DotCefWindow, IPCRequest, Task<IPCResponse?>>? requestProxy = null, bool modifyRequests = false, Func<DotCefWindow, IPCRequest, IPCRequest?>? requestModifier = null, bool modifyRequestBody = false,
            string? title = null, string? iconPath = null, string? appId = null, CancellationToken cancellationToken = default)
        {
            EnsureStarted();

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
            writer.Write(minimumWidth);
            writer.Write(minimumHeight);
            writer.Write(preferredWidth);
            writer.Write(preferredHeight);
            writer.WriteSizePrefixedString(url);
            writer.WriteSizePrefixedString(title);
            writer.WriteSizePrefixedString(iconPath);
            writer.WriteSizePrefixedString(appId);

            var reader = await CallAsync(OpcodeController.WindowCreate, writer.Data, 0, writer.Size, cancellationToken);
            var window = new DotCefWindow(this, reader.Read<int>(), requestModifier, requestProxy);
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

        public void WaitForExit()
        {
            EnsureStarted();
            _childProcess?.WaitForExit();
        }

        public async Task WaitForExitAsync(CancellationToken cancellationToken = default)
        {
            EnsureStarted();
            if (_childProcess != null)
            {
                try
                {
                    await _childProcess.WaitForExitAsync(cancellationToken);
                }
                catch
                {
                    //Ignored
                }
            }
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

        public async Task<string[]> PickFileAsync(bool multiple, (string Name, string Pattern)[] filters,  CancellationToken cancellationToken = default)
        {
            if (OperatingSystem.IsWindows())
                return DialogWindows.PickFiles(multiple, filters);
            
            var writer = new PacketWriter();
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

        public async Task<string> PickDirectoryAsync(CancellationToken cancellationToken = default)
        {
            var reader = await CallAsync(OpcodeController.PickDirectory, cancellationToken);
            return reader.ReadSizePrefixedString()!;
        }

        public async Task<string> SaveFileAsync(string defaultName, (string Name, string Pattern)[] filters,  CancellationToken cancellationToken = default)
        {
            var writer = new PacketWriter();
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
            _writer.Dispose();
            _reader.Dispose();

            _childProcess?.Close();

            _readyTaskCompletionSource.TrySetCanceled();

            //TODO: Can this deadlock due to invoked originating from TrySetCanceled? seems unlikely
            lock (_pendingRequests)
            {
                foreach (var pendingRequest in _pendingRequests)
                    pendingRequest.Value.ResponseBodyTaskCompletionSource.TrySetCanceled();
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
            
            _writeSemaphore.Dispose();
        }

        private static string GetNativeFileName()
        {
            if (OperatingSystem.IsWindows())
                return "dotcefnative.exe";
            else if (OperatingSystem.IsMacOS())
                return "dotcefnative";
            else if (OperatingSystem.IsLinux())
                return "dotcefnative";
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
                searchPaths.Add(Path.Combine(baseDirectory, $"dotcefnative.app/Contents/MacOS/{nativeFileName}"));
                searchPaths.Add(Path.Combine(baseDirectory, $"JustCef.app/Contents/MacOS/{nativeFileName}"));
            }
            if (OperatingSystem.IsMacOS())
            {
                searchPaths.Add(Path.Combine(baseDirectory, $"../Frameworks/dotcefnative.app/Contents/MacOS/{nativeFileName}"));
                searchPaths.Add(Path.Combine(baseDirectory, $"../Frameworks/JustCef.app/Contents/MacOS/{nativeFileName}"));
            }
            searchPaths.Add(Path.Combine(baseDirectory, cefDir, nativeFileName));
            searchPaths.Add(Path.Combine(baseDirectory, nativeFileName));

            if (assemblyDirectory != null)
            {
                if (OperatingSystem.IsMacOS())
                {
                    searchPaths.Add(Path.Combine(assemblyDirectory, $"dotcefnative.app/Contents/MacOS/{nativeFileName}"));
                    searchPaths.Add(Path.Combine(assemblyDirectory, $"JustCef.app/Contents/MacOS/{nativeFileName}"));
                }
                if (OperatingSystem.IsMacOS())
                {
                    searchPaths.Add(Path.Combine(assemblyDirectory, $"../Frameworks/dotcefnative.app/Contents/MacOS/{nativeFileName}"));
                    searchPaths.Add(Path.Combine(assemblyDirectory, $"../Frameworks/JustCef.app/Contents/MacOS/{nativeFileName}"));
                }
                searchPaths.Add(Path.Combine(assemblyDirectory, cefDir, nativeFileName));
                searchPaths.Add(Path.Combine(assemblyDirectory, nativeFileName));
            }

            if (executableDirectory != null)
            {
                if (OperatingSystem.IsMacOS())
                {
                    searchPaths.Add(Path.Combine(executableDirectory, $"dotcefnative.app/Contents/MacOS/{nativeFileName}"));
                    searchPaths.Add(Path.Combine(executableDirectory, $"JustCef.app/Contents/MacOS/{nativeFileName}"));
                }
                if (OperatingSystem.IsMacOS())
                {
                    searchPaths.Add(Path.Combine(executableDirectory, $"../Frameworks/dotcefnative.app/Contents/MacOS/{nativeFileName}"));
                    searchPaths.Add(Path.Combine(executableDirectory, $"../Frameworks/JustCef.app/Contents/MacOS/{nativeFileName}"));
                }
                searchPaths.Add(Path.Combine(executableDirectory, cefDir, nativeFileName));
                searchPaths.Add(Path.Combine(executableDirectory, nativeFileName));
            }

            if (OperatingSystem.IsMacOS())
            {
                searchPaths.Add(Path.Combine(currentWorkingDirectory, $"dotcefnative.app/Contents/MacOS/{nativeFileName}"));
                searchPaths.Add(Path.Combine(currentWorkingDirectory, $"JustCef.app/Contents/MacOS/{nativeFileName}"));
            }
            if (OperatingSystem.IsMacOS())
            {
                searchPaths.Add(Path.Combine(currentWorkingDirectory, $"../Frameworks/dotcefnative.app/Contents/MacOS/{nativeFileName}"));
                searchPaths.Add(Path.Combine(currentWorkingDirectory, $"../Frameworks/JustCef.app/Contents/MacOS/{nativeFileName}"));
            }
            searchPaths.Add(Path.Combine(currentWorkingDirectory, nativeFileName));
            searchPaths.Add(Path.Combine(currentWorkingDirectory, cefDir, nativeFileName));

            return searchPaths.Distinct().ToArray();
        }
    }
}
