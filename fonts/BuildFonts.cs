// SPDX-License-Identifier: GPL-3.0-only
// Bitmap format facts verified against local Morrowind files and OpenMW's font loader.
using System;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Imaging;
using System.Drawing.Text;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;

public static class BuildFonts
{
    const int Scale = 2;
    const int AtlasWidth = 512;
    const int Padding = 4;

    sealed class Glyph : IDisposable
    {
        public GraphicsPath Path;
        public RectangleF Bounds;
        public float Advance;
        public int X, Y, Width, Height;
        public void Dispose() { if (Path != null) Path.Dispose(); }
    }

    static char Decode(int code, int codePage)
    {
        if (code < 32 || code == 127) return '\0';
        return Encoding.GetEncoding(codePage).GetString(new byte[] { (byte)code })[0];
    }

    static void Build(string target, FontFamily family, int lineHeight, int codePage)
    {
        string name = "modern_sans_" + lineHeight;
        string textureName = name + "_0_Lod_A";
        float emPixels = lineHeight * Scale;
        float gdiAscent = family.GetCellAscent(FontStyle.Regular) * emPixels / family.GetEmHeight(FontStyle.Regular);
        // Keep descenders within each logical line. GDI+ path coordinates are measured
        // from its own line origin; convert them to Morrowind's baseline below.
        float baseline = lineHeight * 0.78f;
        Glyph[] glyphs = new Glyph[256];
        int cursorX = Padding, cursorY = Padding, rowHeight = 0;
        using (Bitmap measure = new Bitmap(8, 8))
        using (Graphics graphics = Graphics.FromImage(measure))
        using (Font font = new Font(family, emPixels, FontStyle.Regular, GraphicsUnit.Pixel))
        using (StringFormat format = (StringFormat)StringFormat.GenericTypographic.Clone())
        {
            format.FormatFlags |= StringFormatFlags.MeasureTrailingSpaces;
            for (int code = 0; code < 256; ++code)
            {
                Glyph glyph = new Glyph();
                glyphs[code] = glyph;
                char ch = Decode(code, codePage);
                if (ch == '\0') continue;
                string text = ch.ToString();
                glyph.Advance = graphics.MeasureString(text, font, Int32.MaxValue, format).Width / Scale;
                if (Char.IsWhiteSpace(ch)) continue;
                glyph.Path = new GraphicsPath();
                glyph.Path.AddString(text, family, (int)FontStyle.Regular, emPixels, new PointF(0, 0), format);
                if (glyph.Path.PointCount == 0) continue;
                glyph.Bounds = glyph.Path.GetBounds();
                glyph.Width = Math.Max(1, (int)Math.Ceiling(glyph.Bounds.Width) + 2);
                glyph.Height = Math.Max(1, (int)Math.Ceiling(glyph.Bounds.Height) + 2);
                if (cursorX + glyph.Width + Padding > AtlasWidth)
                {
                    cursorX = Padding;
                    cursorY += rowHeight + Padding;
                    rowHeight = 0;
                }
                glyph.X = cursorX;
                glyph.Y = cursorY;
                cursorX += glyph.Width + Padding;
                rowHeight = Math.Max(rowHeight, glyph.Height);
            }
        }
        int atlasHeight = 1;
        while (atlasHeight < cursorY + rowHeight + Padding) atlasHeight *= 2;
        if (atlasHeight > 2048) throw new InvalidOperationException("Font exceeds Direct3D 8 texture limits");
        Directory.CreateDirectory(target);
        using (Bitmap atlas = new Bitmap(AtlasWidth, atlasHeight, PixelFormat.Format32bppArgb))
        using (Graphics graphics = Graphics.FromImage(atlas))
        {
            graphics.Clear(Color.Transparent);
            graphics.SmoothingMode = SmoothingMode.AntiAlias;
            graphics.PixelOffsetMode = PixelOffsetMode.HighQuality;
            for (int code = 0; code < 256; ++code)
            {
                Glyph glyph = glyphs[code];
                if (glyph.Path == null || glyph.Path.PointCount == 0) continue;
                using (Matrix transform = new Matrix())
                {
                    transform.Translate(glyph.X + 1 - glyph.Bounds.X, glyph.Y + 1 - glyph.Bounds.Y);
                    glyph.Path.Transform(transform);
                }
                graphics.FillPath(Brushes.White, glyph.Path);
            }
            atlas.Save(Path.Combine(target, name + "-atlas.png"), ImageFormat.Png);
            using (BinaryWriter writer = new BinaryWriter(File.Create(Path.Combine(target, textureName + ".tex"))))
            {
                writer.Write(AtlasWidth);
                writer.Write(atlasHeight);
                // .tex is tightly packed, top-to-bottom RGBA, without a DXT header.
                // Use white RGB even outside ink to avoid dark bilinear edge fringes.
                BitmapData locked = atlas.LockBits(new Rectangle(0, 0, AtlasWidth, atlasHeight), ImageLockMode.ReadOnly, PixelFormat.Format32bppArgb);
                try
                {
                    byte[] row = new byte[AtlasWidth * 4];
                    byte[] rgba = new byte[AtlasWidth * 4];
                    for (int y = 0; y < atlasHeight; ++y)
                    {
                        Marshal.Copy(IntPtr.Add(locked.Scan0, y * locked.Stride), row, 0, row.Length);
                        for (int x = 0; x < AtlasWidth; ++x)
                        {
                            int offset = x * 4;
                            rgba[offset] = rgba[offset + 1] = rgba[offset + 2] = 255;
                            rgba[offset + 3] = row[offset + 3];
                        }
                        writer.Write(rgba);
                    }
                }
                finally { atlas.UnlockBits(locked); }
            }
        }
        using (BinaryWriter writer = new BinaryWriter(File.Create(Path.Combine(target, name + ".fnt"))))
        {
            writer.Write((float)lineHeight);
            writer.Write(1);
            writer.Write(1);
            byte[] nameBuffer = new byte[284];
            Encoding.ASCII.GetBytes(textureName).CopyTo(nameBuffer, 0);
            writer.Write(nameBuffer);
            for (int code = 0; code < 256; ++code)
            {
                Glyph glyph = glyphs[code];
                bool visible = glyph.Width > 0;
                float left = visible ? (float)glyph.X / AtlasWidth : 0;
                float top = visible ? (float)glyph.Y / atlasHeight : 0;
                float right = visible ? (float)(glyph.X + glyph.Width) / AtlasWidth : 0;
                float bottom = visible ? (float)(glyph.Y + glyph.Height) / atlasHeight : 0;
                float width = glyph.Width / (float)Scale;
                float height = glyph.Height / (float)Scale;
                float bearingLeft = visible ? (glyph.Bounds.X - 1) / Scale : 0;
                float topRelativeToGdiBaseline = (glyph.Bounds.Y - 1 - gdiAscent) / Scale;
                float bearingTop = visible ? baseline + topRelativeToGdiBaseline : 0;
                writer.Write(0.0f);
                writer.Write(left); writer.Write(top);
                writer.Write(right); writer.Write(top);
                writer.Write(left); writer.Write(bottom);
                writer.Write(right); writer.Write(bottom);
                writer.Write(width); writer.Write(height);
                writer.Write(bearingLeft);
                writer.Write(glyph.Advance - width);
                writer.Write(visible ? lineHeight - bearingTop : 0);
            }
        }
        foreach (Glyph glyph in glyphs) glyph.Dispose();
        Console.WriteLine("{0}: {1}x{2}, logical height {3}, code page {4}", name, AtlasWidth, atlasHeight, lineHeight, codePage);
    }

    public static int Main(string[] args)
    {
        try
        {
            string root = args.Length > 0 ? Path.GetFullPath(args[0]) : AppDomain.CurrentDomain.BaseDirectory;
            using (PrivateFontCollection fonts = new PrivateFontCollection())
            {
                fonts.AddFontFile(Path.Combine(root, "source", "NotoSans-Regular.ttf"));
                FontFamily family = fonts.Families[0];
                if (!family.Name.StartsWith("Noto Sans", StringComparison.Ordinal)) throw new InvalidOperationException("Unexpected font: " + family.Name);
                foreach (int codePage in new int[] { 437, 1251 })
                    foreach (int height in new int[] { 16, 20 })
                        Build(Path.Combine(root, "cp" + codePage), family, height, codePage);
            }
            return 0;
        }
        catch (Exception error)
        {
            Console.Error.WriteLine(error);
            return 1;
        }
    }
}
