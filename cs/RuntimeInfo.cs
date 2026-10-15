namespace JustCef;

public enum RuntimeOperatingSystem : byte
{
    Unknown = 0,
    Windows = 1,
    MacOS = 2,
    Linux = 3
}

public enum LinuxWindowingBackend : byte
{
    Unknown = 0,
    X11 = 1,
    Wayland = 2
}

public readonly record struct RuntimeInfo(RuntimeOperatingSystem OperatingSystem, LinuxWindowingBackend? LinuxWindowingBackend);
