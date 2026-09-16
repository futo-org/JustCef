using System.Buffers.Binary;
using System.Text.Json;

namespace FakeNative;

public static class SelfTest
{
    public static int Run(string? path)
    {
        path ??= FindVectors();
        if (path == null || !File.Exists(path))
        {
            Console.Error.WriteLine("self-test: vectors.json not found");
            return 2;
        }
        Console.WriteLine($"self-test: {path}");

        var expected = new Dictionary<string, (string Direction, string Hex)>();
        using (var doc = JsonDocument.Parse(File.ReadAllText(path)))
        {
            foreach (var item in doc.RootElement.EnumerateArray())
            {
                var name = item.GetProperty("name").GetString()!;
                var direction = item.TryGetProperty("direction", out var d) ? d.GetString() ?? "" : "";
                expected[name] = (direction, item.GetProperty("hex").GetString()!.ToLowerInvariant());
            }
        }

        var failures = 0;
        var passed = 0;
        foreach (var vector in Vectors.All)
        {
            if (!expected.Remove(vector.Name, out var want))
            {
                Console.WriteLine($"FAIL {vector.Name}: missing from vectors.json");
                failures++;
                continue;
            }
            var bytes = vector.Build().ToArray();
            var actual = Convert.ToHexString(bytes).ToLowerInvariant();
            if (want.Direction != vector.Direction)
            {
                Console.WriteLine($"FAIL {vector.Name}: direction {want.Direction}, expected {vector.Direction}");
                failures++;
                continue;
            }
            if (actual != want.Hex)
            {
                Console.WriteLine($"FAIL {vector.Name}:\n  expected {want.Hex}\n  encoded  {actual}");
                failures++;
                continue;
            }
            var error = CheckFraming(bytes) ?? (vector.Direction == Vectors.ControllerToNative ? Decode(bytes) : null);
            if (error != null)
            {
                Console.WriteLine($"FAIL {vector.Name}: {error}");
                failures++;
                continue;
            }
            passed++;
        }
        foreach (var name in expected.Keys)
        {
            Console.WriteLine($"FAIL {name}: no encoder for this vector");
            failures++;
        }
        Console.WriteLine($"self-test: {passed} passed, {failures} failed");
        return failures == 0 ? 0 : 1;
    }

    private static string? FindVectors()
    {
        foreach (var start in new[] { Directory.GetCurrentDirectory(), AppContext.BaseDirectory })
        {
            for (var dir = new DirectoryInfo(start); dir != null; dir = dir.Parent)
            {
                var candidate = Path.Combine(dir.FullName, "tests", "protocol", "vectors.json");
                if (File.Exists(candidate))
                    return candidate;
            }
        }
        var local = Path.Combine(AppContext.BaseDirectory, "vectors.json");
        return File.Exists(local) ? local : null;
    }

    private static string? CheckFraming(byte[] packet)
    {
        if (packet.Length < Protocol.HeaderSize)
            return "shorter than the header";
        var size = BinaryPrimitives.ReadUInt32LittleEndian(packet);
        if (size != packet.Length - 4)
            return $"size field {size} does not match length {packet.Length}";
        if (packet[8] > (byte)PacketType.Cancel)
            return $"unknown packet type {packet[8]}";
        return null;
    }

    private static string? Decode(byte[] packet)
    {
        var requestId = BinaryPrimitives.ReadUInt32LittleEndian(packet.AsSpan(4));
        var type = (PacketType)packet[8];
        var opcode = packet[9];
        var body = packet.AsSpan(Protocol.HeaderSize).ToArray();
        try
        {
            switch (type)
            {
                case PacketType.Request:
                    if (requestId == 0)
                        return "request with requestId 0";
                    Codec.ParseControllerRequest(opcode, body);
                    break;
                case PacketType.Response:
                    var head = Codec.ParseResponseHead(body);
                    if (head.Status == Status.Ok)
                        Codec.ValidateClientResponse(opcode, head.Payload);
                    break;
                case PacketType.Notification:
                    if (requestId != 0)
                        return "notification with nonzero requestId";
                    Codec.ParseControllerNotification(opcode, body);
                    break;
                case PacketType.Cancel:
                    if (requestId == 0 || body.Length != 0)
                        return "cancel must have a requestId and an empty body";
                    break;
            }
        }
        catch (ProtocolException e)
        {
            return $"decode failed: {e.Message}";
        }
        return null;
    }
}
