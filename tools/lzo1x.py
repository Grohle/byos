"""
Pure-Python LZO1X: a decompressor (equivalent to lzo1x_decompress_safe) and an
LZO1X-1 style compressor whose output is a standard LZO1X stream (readable by
liblzo2's lzo1x_decompress_safe and by the Linux kernel's lzo decompressor, which is
what squashfs uses).

Used by the ByOS patcher (tools/hibyos_patcher.py) so that it needs no native
libraries. Speed is modest (a few MB/s at best), which is fine for the small amount of
data the patcher has to (de)compress.
"""

__all__ = ["decompress", "compress", "LzoError"]


class LzoError(ValueError):
    pass


def decompress(src, max_out):
    """Decompress one LZO1X stream. Raises LzoError on any malformed input or if the
    output would exceed max_out bytes. The stream must end exactly with the EOF marker."""
    src = bytes(src)
    n = len(src)
    out = bytearray()
    ip = 0
    if n < 3:
        raise LzoError("input too short")

    def lit(count):
        nonlocal ip
        if ip + count > n:
            raise LzoError("input overrun (literals)")
        if len(out) + count > max_out:
            raise LzoError("output overrun (literals)")
        out.extend(src[ip:ip + count])
        ip += count

    def ext_len(base):
        # run of zero bytes (each worth 255) followed by a non-zero byte
        nonlocal ip
        zeros = 0
        while True:
            if ip >= n:
                raise LzoError("input overrun (length)")
            b = src[ip]
            ip += 1
            if b:
                return base + zeros * 255 + b
            zeros += 1
            if zeros > 1 << 20:
                raise LzoError("length too long")

    def copy_match(dist, length):
        start = len(out) - dist
        if dist <= 0 or start < 0:
            raise LzoError("lookbehind overrun")
        if len(out) + length > max_out:
            raise LzoError("output overrun (match)")
        if dist >= length:
            out.extend(out[start:start + length])
        else:
            pat = out[start:]
            reps, rem = divmod(length, dist)
            out.extend(pat * reps + pat[:rem])

    state = 0
    first = True
    while True:
        if ip >= n:
            raise LzoError("input overrun (no EOF marker)")
        t = src[ip]
        ip += 1
        if first:
            first = False
            if t > 17:
                t -= 17
                lit(t)
                state = t if t < 4 else 4
                continue
        if t < 16:
            if state == 0:
                if t == 0:
                    t = ext_len(15)
                lit(t + 3)
                state = 4
                continue
            if ip >= n:
                raise LzoError("input overrun")
            if state != 4:
                nxt = t & 3
                dist = 1 + (t >> 2) + (src[ip] << 2)
                ip += 1
                copy_match(dist, 2)
            else:
                nxt = t & 3
                dist = 1 + 0x800 + (t >> 2) + (src[ip] << 2)
                ip += 1
                copy_match(dist, 3)
        elif t >= 64:
            if ip >= n:
                raise LzoError("input overrun")
            nxt = t & 3
            dist = 1 + ((t >> 2) & 7) + (src[ip] << 3)
            ip += 1
            copy_match(dist, (t >> 5) + 1)
        elif t >= 32:
            length = (t & 31) + 2
            if length == 2:
                length = ext_len(33)
            if ip + 2 > n:
                raise LzoError("input overrun")
            v = src[ip] | (src[ip + 1] << 8)
            ip += 2
            nxt = v & 3
            copy_match(1 + (v >> 2), length)
        else:
            dist = (t & 8) << 11
            length = (t & 7) + 2
            if length == 2:
                length = ext_len(9)
            if ip + 2 > n:
                raise LzoError("input overrun")
            v = src[ip] | (src[ip + 1] << 8)
            ip += 2
            nxt = v & 3
            dist += v >> 2
            if dist == 0:
                # end of stream marker
                if length != 3:
                    raise LzoError("bad EOF marker")
                if ip != n:
                    raise LzoError("trailing data after EOF marker")
                return bytes(out)
            copy_match(dist + 0x4000, length)
        state = nxt
        if nxt:
            lit(nxt)


M2_MAX_LEN, M2_MAX_OFFSET = 8, 0x0800
M3_MAX_LEN, M3_MAX_OFFSET = 33, 0x4000
M4_MAX_LEN, M4_MAX_OFFSET = 9, 0xBFFF


def compress(src):
    """Compress bytes into a standard LZO1X stream (greedy LZO1X-1 style parser)."""
    src = bytes(src)
    n = len(src)
    out = bytearray()
    ii = 0          # first pending (not yet emitted) literal
    if n > M2_MAX_LEN + 5:
        table = {}
        ip = 4      # >= 4 literals before the first match (as in liblzo/kernel)
        ip_end = n - M2_MAX_LEN - 5
        tget = table.get
        while ip < ip_end:
            key = src[ip:ip + 4]
            cand = tget(key)
            table[key] = ip
            if cand is None or ip - cand > M4_MAX_OFFSET:
                ip += 1 + min((ip - ii) >> 5, 63)
                continue
            # extend the match (it may overlap the current position; that is valid)
            m_len = 4
            maxl = n - ip
            while m_len + 32 <= maxl and src[cand + m_len:cand + m_len + 32] == src[ip + m_len:ip + m_len + 32]:
                m_len += 32
            while m_len < maxl and src[cand + m_len] == src[ip + m_len]:
                m_len += 1
            # pending literals
            t = ip - ii
            if t:
                if t <= 3:
                    out[-2] |= t
                elif t <= 18:
                    out.append(t - 3)
                else:
                    tt = t - 18
                    out.append(0)
                    while tt > 255:
                        tt -= 255
                        out.append(0)
                    out.append(tt)
                out += src[ii:ip]
            m_off = ip - cand
            ip += m_len
            ii = ip
            if m_len <= M2_MAX_LEN and m_off <= M2_MAX_OFFSET:
                m_off -= 1
                out.append(((m_len - 1) << 5) | ((m_off & 7) << 2))
                out.append(m_off >> 3)
            elif m_off <= M3_MAX_OFFSET:
                m_off -= 1
                if m_len <= M3_MAX_LEN:
                    out.append(32 | (m_len - 2))
                else:
                    m_len -= M3_MAX_LEN
                    out.append(32)
                    while m_len > 255:
                        m_len -= 255
                        out.append(0)
                    out.append(m_len)
                out.append((m_off << 2) & 0xFF)
                out.append(m_off >> 6)
            else:
                m_off -= 0x4000
                if m_len <= M4_MAX_LEN:
                    out.append(16 | ((m_off >> 11) & 8) | (m_len - 2))
                else:
                    m_len -= M4_MAX_LEN
                    out.append(16 | ((m_off >> 11) & 8))
                    while m_len > 255:
                        m_len -= 255
                        out.append(0)
                    out.append(m_len)
                out.append((m_off << 2) & 0xFF)
                out.append((m_off >> 6) & 0xFF)
    # trailing literals
    t = n - ii
    if t:
        if not out and t <= 238:
            out.append(17 + t)
        elif t <= 3:
            out[-2] |= t
        elif t <= 18:
            out.append(t - 3)
        else:
            tt = t - 18
            out.append(0)
            while tt > 255:
                tt -= 255
                out.append(0)
            out.append(tt)
        out += src[ii:]
    out += b"\x11\x00\x00"
    return bytes(out)
