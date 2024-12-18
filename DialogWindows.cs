using System.ComponentModel;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Runtime.Versioning;
using System.Text;

namespace DotCef;

[SupportedOSPlatform("windows")]
public static class DialogWindows
{
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct OPENFILENAME
    {
        public int lStructSize;
        public IntPtr hwndOwner;
        public IntPtr hInstance;
        public string lpstrFilter;
        public string lpstrCustomFilter;
        public int nMaxCustFilter;
        public int nFilterIndex;
        public IntPtr lpstrFile;
        public int nMaxFile;
        public IntPtr lpstrFileTitle;
        public int nMaxFileTitle;
        public IntPtr lpstrInitialDir;
        public string lpstrTitle;
        public int Flags;
        public short nFileOffset;
        public short nFileExtension;
        public string lpstrDefExt;
        public IntPtr lCustData;
        public IntPtr lpfnHook;
        public string lpTemplateName;
    }

    [DllImport("comdlg32.dll", CharSet = CharSet.Unicode)]
    private static extern bool GetOpenFileName([In, Out] ref OPENFILENAME ofn);

    [DllImport("comdlg32.dll", CharSet = CharSet.Unicode)]
    private static extern uint CommDlgExtendedError();

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct BROWSEINFO
    {
        public IntPtr hwndOwner;
        public IntPtr pidlRoot;
        public IntPtr pszDisplayName;
        public string lpszTitle;
        public uint ulFlags;
        public IntPtr lpfn;
        public IntPtr lParam;
        public int iImage;
    }

    [DllImport("shell32.dll", CharSet = CharSet.Unicode)]
    private static extern IntPtr SHBrowseForFolder(ref BROWSEINFO lpbi);

    [DllImport("shell32.dll", CharSet = CharSet.Unicode)]
    private static extern bool SHGetPathFromIDList(IntPtr pidl, StringBuilder pszPath);

    [DllImport("ole32.dll")]
    private static extern void CoTaskMemFree(IntPtr ptr);

    private const int MAX_PATH = 260;
    private const int OFN_PATHMUSTEXIST = 0x00000800;
    private const int OFN_FILEMUSTEXIST = 0x00001000;
    private const int OFN_ALLOWMULTISELECT = 0x00000200;
    private const int OFN_EXPLORER = 0x00080000;
    private const int OFN_OVERWRITEPROMPT = 0x00000002;
    private const int OFN_NOCHANGEDIR = 0x00000008;

    [DllImport("comdlg32.dll", CharSet = CharSet.Unicode)]
    private static extern bool GetSaveFileName([In, Out] ref OPENFILENAME ofn);

    public unsafe static string[] PickFiles(bool multiple, params (string, string)[] filters)
    {
        var ofn = new OPENFILENAME();
        ofn.lStructSize = Marshal.SizeOf<OPENFILENAME>();
        ofn.lpstrFile = Marshal.AllocHGlobal(2 * 1024);
        Unsafe.InitBlockUnaligned((byte*)ofn.lpstrFile, 0, 2 * 1024);

        try
        {
            ofn.nMaxFile = 1024;
            ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST;

            if (multiple)
                ofn.Flags |= OFN_ALLOWMULTISELECT | OFN_EXPLORER;

            StringBuilder filterBuilder = new StringBuilder();
            foreach (var (description, extension) in filters)
                filterBuilder.Append($"{description}\0{extension}\0");
            filterBuilder.Append("\0");
            ofn.lpstrFilter = filterBuilder.ToString();

            var files = new List<string>();
            if (GetOpenFileName(ref ofn))
            {
                if (multiple)
                {
                    IntPtr currentPtr = ofn.lpstrFile;
                    string firstItem = Marshal.PtrToStringUni(currentPtr)!;
                    currentPtr += (firstItem.Length + 1) * 2;
                    string nextItem = Marshal.PtrToStringUni(currentPtr)!;
                    if (string.IsNullOrEmpty(nextItem))
                    {
                        files.Add(firstItem);
                    }
                    else
                    {
                        while (true)
                        {
                            nextItem = Marshal.PtrToStringUni(currentPtr)!;
                            if (string.IsNullOrEmpty(nextItem))
                                break;
                            files.Add(Path.Combine(firstItem, nextItem));
                            currentPtr += (nextItem.Length + 1) * 2;
                        }
                    }
                }
                else
                {
                    files.Add(Marshal.PtrToStringUni(ofn.lpstrFile)!);
                }
            }
            else
            {
                uint error = CommDlgExtendedError();

            }

            return files.ToArray();
        }
        finally
        {
            Marshal.FreeHGlobal(ofn.lpstrFile);
        }
    }

    public static string PickDirectory()
    {
        var bi = new BROWSEINFO();
        bi.lpszTitle = "Select Directory";
        IntPtr pidl = SHBrowseForFolder(ref bi);
        if (pidl != IntPtr.Zero)
        {
            StringBuilder path = new StringBuilder(MAX_PATH);
            if (SHGetPathFromIDList(pidl, path))
            {
                CoTaskMemFree(pidl);
                return path.ToString();
            }
            CoTaskMemFree(pidl);
        }

        return "";
    }

    public static string SaveFile(string defaultName, List<(string description, string extension)> filters)
    {
        var ofn = new OPENFILENAME();
        ofn.lStructSize = Marshal.SizeOf<OPENFILENAME>();
        ofn.hwndOwner = IntPtr.Zero;
        ofn.lpstrFile = Marshal.AllocHGlobal(2 * 1024);

        try
        {
            ofn.nMaxFile = 1024;
            ofn.lpstrFilter = "";
            ofn.nFilterIndex = 1;
            ofn.lpstrFileTitle = IntPtr.Zero;
            ofn.nMaxFileTitle = 0;
            ofn.lpstrInitialDir = IntPtr.Zero;
            ofn.Flags = OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;

            if (!string.IsNullOrEmpty(defaultName))
            {
                var nameBytes = Encoding.Unicode.GetBytes(defaultName);
                var numCharsToCopy = Math.Min(nameBytes.Length, 1024 - 1);
                Marshal.Copy(nameBytes, 0, ofn.lpstrFile, numCharsToCopy);
            }

            StringBuilder file = new StringBuilder(1024);
            file.Append(defaultName);

            StringBuilder filterBuilder = new StringBuilder();
            foreach (var (description, extension) in filters)
                filterBuilder.Append($"{description}\0*.{extension}\0");

            filterBuilder.Append("\0");
            ofn.lpstrFilter = filterBuilder.ToString();

            string fileName = "";
            if (GetSaveFileName(ref ofn))
                fileName = Marshal.PtrToStringUni(ofn.lpstrFile)!;

            return fileName;
        }
        finally
        {
            Marshal.FreeHGlobal(ofn.lpstrFile);
        }
    }
}