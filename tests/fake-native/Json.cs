using System.Globalization;
using System.Text.Json.Nodes;

namespace FakeNative;

public static class Json
{
    public static string? Str(JsonObject o, string key)
    {
        var n = o[key];
        return n?.GetValue<string>();
    }

    public static int IntValue(JsonNode? n, int? newId)
    {
        if (n is JsonValue v)
        {
            if (v.TryGetValue<string>(out var s))
            {
                if (s == "$new")
                    return newId ?? throw new InvalidOperationException("\"$new\" is only valid inside a WindowCreate afterReply");
                return int.Parse(s, CultureInfo.InvariantCulture);
            }
            return v.GetValue<int>();
        }
        throw new InvalidOperationException("expected an integer");
    }

    public static int Int(JsonObject o, string key, int def, int? newId = null)
    {
        var n = o[key];
        return n == null ? def : IntValue(n, newId);
    }

    public static long Long(JsonObject o, string key, long def)
    {
        var n = o[key];
        return n == null ? def : n.GetValue<long>();
    }

    public static bool Bool(JsonObject o, string key, bool def)
    {
        var n = o[key];
        return n == null ? def : n.GetValue<bool>();
    }

    public static byte Opcode<T>(JsonObject o, string key) where T : struct, Enum
    {
        var n = o[key] ?? throw new ArgumentException($"{key} is required");
        if (n is JsonValue v && v.TryGetValue<string>(out var s))
        {
            if (byte.TryParse(s, NumberStyles.None, CultureInfo.InvariantCulture, out var parsed))
                return parsed;
            if (Enum.TryParse<T>(s, true, out var e))
                return Convert.ToByte(e, CultureInfo.InvariantCulture);
            throw new ArgumentException($"unknown {typeof(T).Name} {s}");
        }
        var value = n.GetValue<int>();
        if (value is < 0 or > 255)
            throw new ArgumentException($"opcode {value} is out of range");
        return (byte)value;
    }
}
