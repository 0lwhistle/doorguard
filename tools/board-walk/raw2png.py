import sys, struct

# dg_screen.raw: XRGB8888 720x1280, stride=2880 -> bottom-up 32bpp BMP(字节序天然一致)
def main(src, dst, w=720, h=1280):
    data = open(src, 'rb').read()
    stride = w * 4
    assert len(data) == stride * h, "size %d != %d" % (len(data), stride * h)
    pixel = b''.join(data[r * stride:(r + 1) * stride] for r in reversed(range(h)))
    fh = struct.pack('<2sIHHI', b'BM', 14 + 40 + len(pixel), 0, 0, 14 + 40)
    ih = struct.pack('<IiiHHIIiiII', 40, w, h, 1, 32, 0, len(pixel), 0, 0, 0, 0)
    open(dst, 'wb').write(fh + ih + pixel)
    print("ok", dst)

if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])
