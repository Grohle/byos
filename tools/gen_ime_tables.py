#!/usr/bin/env python3
"""Generate src/ime_zh.inc.c: the pinyin tables of the on-screen keyboard (simplified and traditional).

Run it in WSL (Ubuntu 24.04), from the repository root:
    python3 -I tools/gen_ime_tables.py

Inputs, all local (nothing is downloaded):
  - the GB2312 character set (Python's built-in gb2312 codec): level 1 (3755 common characters)
    and level 2 (3008 less common ones);
  - the system ICU library (libicui18n.so.74, already installed in Ubuntu 24.04), through ctypes:
    "Han-Latin" gives one reading per character and "Hans-Hant" the traditional form;
  - tools/ime_zh_freq.txt: characters in order of frequency (orders the candidates);
  - the hand-made lists below: extra readings of common polyphones and one-to-many traditional forms.
The output is committed, so building the firmware never needs ICU or Python.
"""
import ctypes
import os
import sys
import unicodedata

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "src", "ime_zh.inc.c")
FREQ = os.path.join(ROOT, "tools", "ime_zh_freq.txt")

# ---------------------------------------------------------------- ICU (generation time only)
_icu = None
for name in ("libicui18n.so.74", "libicui18n.so"):
    try:
        _icu = ctypes.CDLL(name)
        break
    except OSError:
        pass
if not _icu:
    sys.exit("libicui18n not found (Ubuntu: it comes with the system, package libicu74)")


def _fn(base):
    for suf in ("_74", ""):
        f = getattr(_icu, base + suf, None)
        if f:
            return f
    sys.exit("ICU function not found: " + base)


_open = _fn("utrans_openU")
_open.restype = ctypes.c_void_p
_open.argtypes = [ctypes.c_char_p, ctypes.c_int32, ctypes.c_int, ctypes.c_void_p, ctypes.c_int32,
                  ctypes.c_void_p, ctypes.POINTER(ctypes.c_int)]
_trans = _fn("utrans_transUChars")
_trans.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.POINTER(ctypes.c_int32), ctypes.c_int32,
                   ctypes.c_int32, ctypes.POINTER(ctypes.c_int32), ctypes.POINTER(ctypes.c_int)]
_cache = {}


def translit(tid, s):
    if tid not in _cache:
        e = ctypes.c_int(0)
        t = _open(tid.encode("utf-16-le") + b"\0\0", -1, 0, None, 0, None, ctypes.byref(e))
        if e.value > 0 or not t:
            sys.exit("ICU transliterator %s: error %d" % (tid, e.value))
        _cache[tid] = t
    b = s.encode("utf-16-le")
    buf = ctypes.create_string_buffer(b + b"\0" * 256)
    n = ctypes.c_int32(len(b) // 2)
    lim = ctypes.c_int32(len(b) // 2)
    e = ctypes.c_int(0)
    _trans(_cache[tid], buf, ctypes.byref(n), 120, 0, ctypes.byref(lim), ctypes.byref(e))
    return buf.raw[:n.value * 2].decode("utf-16-le")


def toneless(py):
    """'lǜ' -> 'lv', 'zhōng' -> 'zhong' (keyboard pinyin: v stands for u with umlaut)."""
    d = unicodedata.normalize("NFD", py.lower())
    o = []
    for i, ch in enumerate(d):
        if unicodedata.combining(ch):
            continue
        if ch == "u" and i + 1 < len(d) and d[i + 1] == "̈":
            o.append("v")
        elif "a" <= ch <= "z":
            o.append(ch)
        else:
            return ""
    return "".join(o)


# ---------------------------------------------------------------- hand-made data
# Polyphones: readings in order (the first is the main one). They replace ICU's single reading.
POLY = {
    "行": "xing hang", "长": "chang zhang", "了": "le liao", "还": "hai huan", "都": "dou du",
    "得": "de dei", "着": "zhe zhao zhuo", "重": "zhong chong", "乐": "le yue", "会": "hui kuai",
    "地": "di de", "和": "he huo hu", "觉": "jue jiao", "调": "diao tiao", "朝": "chao zhao",
    "传": "chuan zhuan", "弹": "tan dan", "当": "dang", "发": "fa", "干": "gan", "种": "zhong",
    "便": "bian pian", "大": "da dai", "说": "shuo shui", "曾": "ceng zeng", "差": "cha chai ci",
    "藏": "cang zang", "参": "can shen cen", "角": "jiao jue", "系": "xi ji", "什": "shen shi",
    "似": "si shi", "校": "xiao jiao", "省": "sheng xing", "降": "jiang xiang", "盛": "sheng cheng",
    "区": "qu ou", "解": "jie xie", "薄": "bao bo", "血": "xue xie", "落": "luo la lao", "露": "lu lou",
    "奇": "qi ji", "恶": "e wu", "率": "lv shuai", "模": "mo mu", "的": "de di", "那": "na nei",
    "哪": "na nei", "这": "zhe zhei", "给": "gei ji", "没": "mei mo", "否": "fou pi", "车": "che ju",
    "仇": "chou qiu", "单": "dan shan", "卡": "ka qia", "塞": "sai se", "色": "se shai", "剥": "bo bao",
    "宿": "su xiu", "厦": "sha xia", "吓": "xia he", "属": "shu zhu", "堡": "bao pu", "叶": "ye xie",
    "拓": "tuo ta", "迫": "po pai", "沈": "shen chen", "期": "qi ji", "缝": "feng", "称": "cheng chen",
    "数": "shu", "只": "zhi", "为": "wei", "少": "shao", "好": "hao", "更": "geng", "要": "yao",
    "看": "kan", "教": "jiao", "相": "xiang", "中": "zhong", "处": "chu", "难": "nan", "将": "jiang",
    "空": "kong", "分": "fen", "应": "ying", "冲": "chong", "结": "jie", "间": "jian", "背": "bei",
    "倒": "dao", "量": "liang", "片": "pian", "尽": "jin", "正": "zheng",
    "睡": "shui", "乘": "cheng sheng", "折": "zhe she",
    "茄": "qie jia", "曲": "qu", "华": "hua", "阿": "a e", "番": "fan pan",
    "尉": "wei yu", "蔓": "man wan", "纶": "lun guan", "囤": "tun dun",
    "壳": "ke qiao", "削": "xiao xue", "疟": "nve yao", "略": "lve", "约": "yue yao", "钥": "yao yue",
    "绿": "lv lu", "六": "liu lu", "石": "shi dan", "伯": "bo bai", "柏": "bai bo",
    "朴": "piao pu po", "查": "cha zha", "曝": "pu bao", "瀑": "pu bao", "暴": "bao pu",
}
POLY = {k: v for k, v in POLY.items() if len(k) == 1 and v}

# Simplified characters with several traditional forms: the extra forms shown as candidates.
S2T_ALT = {
    "发": "發髮", "后": "後后", "干": "乾幹干", "面": "面麵", "里": "裡裏里", "台": "臺台颱檯",
    "复": "復複覆", "系": "系係繫", "只": "只隻", "松": "松鬆", "范": "範范", "制": "制製",
    "准": "準准", "冲": "沖衝", "历": "歷曆", "钟": "鐘鍾", "获": "獲穫", "尽": "盡儘", "恶": "惡噁",
    "汇": "匯彙", "了": "了瞭", "余": "餘余", "云": "雲云", "斗": "鬥斗", "谷": "谷穀", "丑": "醜丑",
    "卷": "卷捲", "板": "板闆", "胡": "胡鬍", "须": "須鬚", "表": "表錶", "征": "征徵", "姜": "姜薑",
    "朴": "朴樸", "坛": "壇罈", "划": "划劃", "舍": "舍捨", "苏": "蘇甦", "当": "當噹", "回": "回迴",
    "团": "團糰", "致": "致緻", "签": "簽籤", "才": "才纔", "价": "價价", "借": "借藉", "几": "幾几",
    "凶": "凶兇", "千": "千韆", "吁": "吁籲", "咸": "咸鹹", "布": "布佈", "弦": "弦絃", "志": "志誌",
    "念": "念唸", "托": "托託", "扣": "扣釦", "折": "折摺", "沈": "沈瀋", "游": "遊游", "症": "症癥",
    "秋": "秋鞦", "着": "著着", "周": "周週", "注": "注註", "杯": "杯盃", "厘": "釐厘", "向": "向嚮",
    "党": "黨党", "愿": "願愿", "郁": "鬱郁", "采": "採采", "术": "術朮", "朱": "朱硃", "家": "家傢",
    "据": "據据", "占": "佔占", "仆": "僕仆", "蒙": "蒙矇濛懞", "灶": "灶竈",
}

# ---------------------------------------------------------------- characters
def gb2312_chars():
    lv1, lv2 = [], []
    for row in range(0xB0, 0xF8):
        for col in range(0xA1, 0xFF):
            try:
                c = bytes([row, col]).decode("gb2312")
            except UnicodeDecodeError:
                continue
            (lv1 if row < 0xD8 else lv2).append(c)
    return lv1, lv2


def main():
    lv1, lv2 = gb2312_chars()
    chars = lv1 + lv2
    freq = []
    seen = set()
    with open(FREQ, encoding="utf-8") as f:
        for line in f:
            if line.startswith("#"):
                continue
            for ch in line.strip():
                if not ch.isspace() and ch not in seen:
                    seen.add(ch)
                    freq.append(ch)
    rank = {}
    for i, c in enumerate(freq):
        rank[c] = i
    base = len(freq)
    for i, c in enumerate(chars):
        if c not in rank:
            rank[c] = base + i
    gbset = set(chars)
    missing_freq = [c for c in freq if c not in gbset]

    # readings
    table = {}  # syllable -> [(rank, char)]
    noread = []
    for c in chars:
        if c in POLY:
            reads = POLY[c].split()
        else:
            r = toneless(translit("Han-Latin", c))
            reads = [r] if r else []
        if not reads:
            noread.append(c)
            continue
        for k, py in enumerate(reads):
            rk = rank[c] if k == 0 else rank[c] + 200   # second readings: after the common characters
            table.setdefault(py, []).append((rk, c))
    # lue/nue: the spelling Sogou, Microsoft Pinyin and iOS also accept for lve/nve (lu+e and
    # nu+e are not syllables), so "celue" gives 策略 instead of splitting it as lu + e
    for alias, real in (("lue", "lve"), ("nue", "nve")):
        if real in table:
            table[alias] = list(table[real])
    syls = sorted(table)
    for s in syls:
        table[s].sort()

    # traditional
    s2t = []
    for c in chars:
        t = translit("Hans-Hant", c)
        if len(t) == 1 and t != c:
            s2t.append((ord(c), ord(t)))
    s2t.sort()
    alts = sorted((ord(k), v) for k, v in S2T_ALT.items())

    def rbucket(r):
        return r if r < 64 else min(255, 64 + (r - 64) // 16)

    # ---------------------------------------------------------- output
    o = []
    o.append("/* ime_zh.inc.c - GENERATED by tools/gen_ime_tables.py (do not edit by hand).")
    o.append(" * Pinyin -> characters for the on-screen keyboard (GB2312 levels 1 and 2, readings from ICU")
    o.append(" * Han-Latin without tones, v = u with umlaut) and simplified -> traditional pairs (ICU Hans-Hant).")
    o.append(" * Included from kb_ime.inc.c. */")
    o.append("#define IME_PY_N %d" % len(syls))
    o.append("static const char IME_PY_SYL[IME_PY_N][7] = {")
    line = "   "
    for s in syls:
        item = ' "%s",' % s
        if len(line) + len(item) > 110:
            o.append(line)
            line = "   "
        line += item
    o.append(line)
    o.append("};")
    offs = [0]
    flat = []
    for s in syls:
        flat.extend(table[s])
        offs.append(len(flat))
    o.append("static const unsigned short IME_PY_OFF[IME_PY_N + 1] = {")
    for i in range(0, len(offs), 16):
        o.append("    " + ", ".join(str(x) for x in offs[i:i + 16]) + ",")
    o.append("};")
    o.append("#define IME_PY_NCH %d" % len(flat))
    o.append("/* characters of each syllable, most common first */")
    o.append("static const unsigned short IME_PY_CH[IME_PY_NCH] = {")
    for i in range(0, len(flat), 14):
        o.append("    " + ", ".join("0x%04X" % ord(c) for _, c in flat[i:i + 14]) + ",")
    o.append("};")
    o.append("/* frequency bucket of each entry (0 = most common; merges the lists of several syllables) */")
    o.append("static const unsigned char IME_PY_RK[IME_PY_NCH] = {")
    for i in range(0, len(flat), 24):
        o.append("    " + ", ".join(str(rbucket(r)) for r, _ in flat[i:i + 24]) + ",")
    o.append("};")
    o.append("#define IME_S2T_N %d" % len(s2t))
    o.append("/* simplified -> traditional, sorted by the simplified form (characters that do not change are absent) */")
    o.append("static const unsigned short IME_S2T[IME_S2T_N][2] = {")
    for i in range(0, len(s2t), 8):
        o.append("    " + " ".join("{ 0x%04X, 0x%04X }," % p for p in s2t[i:i + 8]))
    o.append("};")
    o.append("#define IME_S2T_ALT_N %d" % len(alts))
    o.append("/* simplified characters with several traditional forms: all of them, most common first */")
    o.append("static const struct { unsigned short s; const char *t; } IME_S2T_ALT[IME_S2T_ALT_N] = {")
    for i in range(0, len(alts), 6):
        o.append("    " + " ".join('{ 0x%04X, "%s" },' % a for a in alts[i:i + 6]))
    o.append("};")
    data = len(syls) * 7 + len(offs) * 2 + len(flat) * 3 + len(s2t) * 4 + len(alts) * 8 + sum(len(v.encode()) + 1 for _, v in alts)
    o.append("/* %d syllables, %d entries, %d traditional pairs: about %.1f KB of tables */" % (len(syls), len(flat), len(s2t), data / 1024))
    with open(OUT, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(o) + "\n")
    print("%s: %d syllables, %d entries, %d s2t pairs, %d alternates, ~%.1f KB" % (OUT, len(syls), len(flat), len(s2t), len(alts), data / 1024))
    if noread:
        print("no reading (skipped): %d, e.g. %s" % (len(noread), "".join(noread[:20])))
    if missing_freq:
        print("frequency list characters outside GB2312 (ignored): %s" % "".join(missing_freq[:40]))


if __name__ == "__main__":
    main()
