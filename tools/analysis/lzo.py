"""Pure-Python LZO1X decompressor (same semantics as lzo1x_decompress_safe)."""

def decompress(src, out_len=None):
    dst = bytearray()
    ip = 0
    n = len(src)
    state = 0

    def lits(k):
        nonlocal ip
        dst.extend(src[ip:ip+k]); ip += k

    if src[0] > 17:
        t = src[0] - 17; ip = 1
        lits(t)
        state = t if t < 4 else 4
    while ip < n:
        t = src[ip]; ip += 1
        if t < 16:
            if state == 0:
                if t == 0:
                    while src[ip] == 0:
                        t += 255; ip += 1
                    t += 15 + src[ip]; ip += 1
                t += 3
                lits(t); state = 4
                continue
            elif state != 4:
                nxt = t & 3
                m = len(dst) - 1 - (t >> 2) - (src[ip] << 2); ip += 1
                length = 2
            else:
                nxt = t & 3
                m = len(dst) - (1 + 0x800) - (t >> 2) - (src[ip] << 2); ip += 1
                length = 3
        elif t >= 64:
            nxt = t & 3
            m = len(dst) - 1 - ((t >> 2) & 7) - (src[ip] << 3); ip += 1
            length = (t >> 5) + 1
        elif t >= 32:
            length = t & 31
            if length == 0:
                while src[ip] == 0:
                    length += 255; ip += 1
                length += 31 + src[ip]; ip += 1
            length += 2
            v = src[ip] | (src[ip+1] << 8); ip += 2
            m = len(dst) - 1 - (v >> 2)
            nxt = v & 3
        else:
            m = len(dst) - ((t & 8) << 11)
            length = t & 7
            if length == 0:
                while src[ip] == 0:
                    length += 255; ip += 1
                length += 7 + src[ip]; ip += 1
            length += 2
            v = src[ip] | (src[ip+1] << 8); ip += 2
            m -= v >> 2
            nxt = v & 3
            if m == len(dst):
                break
            m -= 0x4000
        if m < 0:
            raise ValueError(f'lookbehind overrun at ip={ip}')
        for k in range(length):
            dst.append(dst[m + k])
        state = nxt
        lits(nxt)
    if out_len is not None and len(dst) != out_len:
        raise ValueError(f'size mismatch {len(dst)} != {out_len}')
    return bytes(dst)
