# Writes a PWG raster (RaS2) file from the spec: sync word, 1796-byte big-endian page header, PackBits-like lines.
import struct, sys
def header(w, h, dpi, bpp, cspace, ncolors, page_w_pt, page_h_pt, total_pages):
    hd = bytearray(1796)
    hd[0:9] = b'PwgRaster'
    struct.pack_into('>II', hd, 276, dpi, dpi)                     # HWResolution
    struct.pack_into('>II', hd, 352, page_w_pt, page_h_pt)         # PageSize (points)
    struct.pack_into('>II', hd, 372, w, h)                         # Width, Height
    struct.pack_into('>I', hd, 384, 8 if bpp > 1 else 1)           # BitsPerColor
    struct.pack_into('>I', hd, 388, bpp)                           # BitsPerPixel
    struct.pack_into('>I', hd, 392, (bpp * w + 7) // 8)            # BytesPerLine
    struct.pack_into('>I', hd, 396, 0)                             # ColorOrder chunked
    struct.pack_into('>I', hd, 400, cspace)                        # ColorSpace (18 sgray, 3 black)
    struct.pack_into('>I', hd, 420, ncolors)                       # NumColors
    struct.pack_into('>I', hd, 452, total_pages)                   # TotalPageCount
    struct.pack_into('>ii', hd, 456, 1, 1)                         # CrossFeed/FeedTransform
    hd[1732:1732+len('na_letter_8.5x11in')] = b'na_letter_8.5x11in'
    return bytes(hd)
def enc_line(line, bpp_bytes):
    out = bytearray(); px = [line[i:i+bpp_bytes] for i in range(0, len(line), bpp_bytes)]
    i = 0
    while i < len(px):
        j = i
        while j + 1 < len(px) and px[j+1] == px[i] and j - i < 127: j += 1
        if j > i:                                  # repeat run of (j-i+1) pixels
            out.append(j - i); out += px[i]; i = j + 1
        else:                                      # literal run until next repeat
            k = i
            while k < len(px) and k - i < 128 and not (k + 1 < len(px) and px[k+1] == px[k]): k += 1
            k = max(k, i + 1)
            if k - i == 1: out.append(0); out += px[i]          # a lone pixel is a repeat run of 1
            else: out.append(257 - (k - i)); out += b''.join(px[i:k])
            i = k
    return bytes(out)
def write(path, pages):
    with open(path, 'wb') as f:
        f.write(b'RaS2')
        for (w, h, bpp, cs, rows) in pages:
            f.write(header(w, h, 300, bpp, cs, 1, w * 72 // 300, h * 72 // 300, len(pages)))
            bb = max(1, bpp // 8); prev = None; cnt = 0
            for r in rows + [None]:
                if r is not None and r == prev and cnt < 256: cnt += 1; continue
                if prev is not None: f.write(bytes([cnt - 1])); f.write(enc_line(prev, bb))
                prev, cnt = r, 1
if __name__ == '__main__':
    w, h = 64, 48
    rows = []
    for y in range(h):
        rows.append(bytes((255 if (x // 8 + y // 8) % 2 == 0 else 0) for x in range(w)))   # 8-bit checkerboard
    write(sys.argv[1], [(w, h, 8, 18, rows), (w, h, 8, 18, rows[::-1])])
