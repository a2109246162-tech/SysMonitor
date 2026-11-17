import struct, os

BG   = (0x14, 0x16, 0x1C)  # RGB
BLUE = (0x00, 0x78, 0xD7)
TEAL = (0x00, 0xB7, 0xC3)
ORNG = (0xFF, 0x8C, 0x00)

def img(size):
    # pixel canvas RGBA (top-down)
    px = [[(BG[0], BG[1], BG[2], 255) for _ in range(size)] for _ in range(size)]

    def rect(x0, y0, x1, y1, c):
        for y in range(max(0,y0), min(size,y1)):
            for x in range(max(0,x0), min(size,x1)):
                px[y][x] = (c[0], c[1], c[2], 255)

    # 外框强调蓝边
    b = max(1, size//32)
    rect(0, 0, size, b, BLUE)
    rect(0, size-b, size, size, BLUE)
    rect(0, 0, b, size, BLUE)
    rect(size-b, 0, size, size, BLUE)

    # 三条指标条
    h  = max(2, int(size*0.10))
    x0 = int(size*0.20)
    specs = [(0.24, 0.56, BLUE), (0.47, 0.42, TEAL), (0.70, 0.30, ORNG)]
    for fy, fw, col in specs:
        y0 = int(size*fy)
        w  = int(size*fw)
        rect(x0, y0, x0+w, y0+h, col)

    return px

def bmp_image(size):
    px = img(size)
    # BITMAPINFOHEADER: height doubled (XOR + AND), 32bpp
    hdr = struct.pack('<IiiHHIIiiII', 40, size, size*2, 1, 32, 0, size*size*4, 0, 0, 0, 0)
    # XOR: bottom-up BGRA
    rows = []
    for y in range(size-1, -1, -1):
        row = bytearray()
        for x in range(size):
            r,g,b,a = px[y][x]
            row += bytes((b,g,r,a))
        rows.append(bytes(row))
    xor = b''.join(rows)
    # AND mask: 1bpp, all zero (opaque), rows padded to 4 bytes
    rowbytes = ((size + 31)//32)*4
    andm = bytes(rowbytes * size)
    return hdr + xor + andm

sizes = [32, 48, 64]
images = [bmp_image(s) for s in sizes]

out = bytearray()
out += struct.pack('<HHH', 0, 1, len(sizes))
offset = 6 + 16*len(sizes)
entries = b''
data = b''
for s, im in zip(sizes, images):
    entries += struct.pack('<BBBBHHII', s % 256, s % 256, 0, 0, 1, 32, len(im), offset)
    data += im
    offset += len(im)
out += entries + data

with open('app.ico', 'wb') as f:
    f.write(out)
print('app.ico', len(out), 'bytes, sizes=', sizes)
