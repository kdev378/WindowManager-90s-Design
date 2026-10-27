#!/usr/bin/env python3
"""
gen-jis0208.py - src/jis0208.c を生成する (SPEC §4.5.4, §2.3.1)

JIS X 0208 <-> UCS の対応表を Python 標準の iso2022_jp コーデックから作る。
生成物は const 配列だけなので .rodata に載り、Private_Dirty には計上されない
(SPEC §9。共有ライブラリのテキストと同じく「読むだけのページ」)。

使い方:
    python3 tools/gen-jis0208.py > src/jis0208.c

用途は 2 つ:
  1. COMPOUND_TEXT (ESC $ ( B / ESC $ ) B) の復号 … jis -> ucs
  2. jisx0208.1983-0 のコアフォントで日本語を描く … ucs -> jis
"""
import sys

ROWS = 94


def main():
    to_ucs = [0] * (ROWS * ROWS)
    for r in range(ROWS):
        for c in range(ROWS):
            raw = b"\x1b$B" + bytes([0x21 + r, 0x21 + c]) + b"\x1b(B"
            try:
                s = raw.decode("iso2022_jp")
            except UnicodeDecodeError:
                continue
            if len(s) != 1:
                continue
            cp = ord(s)
            if cp > 0xFFFF:
                continue
            to_ucs[r * ROWS + c] = cp

    # ucs 昇順に並べた添字表 (二分探索用)。同じ ucs が 2 箇所に出たら先勝ち
    seen = set()
    by_ucs = []
    for idx in sorted(range(len(to_ucs)), key=lambda i: (to_ucs[i], i)):
        u = to_ucs[idx]
        if u == 0 or u in seen:
            continue
        seen.add(u)
        by_ucs.append(idx)

    out = sys.stdout
    out.write("/*\n")
    out.write(" * jis0208.c - JIS X 0208 <-> UCS 対応表 (自動生成。手で編集しない)\n")
    out.write(" *\n")
    out.write(" * 生成: python3 tools/gen-jis0208.py > src/jis0208.c\n")
    out.write(" * 出典: Python 標準の iso2022_jp コーデック\n")
    out.write(" *\n")
    out.write(" * const 配列のみ。.rodata に載るので常駐の Private_Dirty は増えない\n")
    out.write(" * (SPEC §9)。合計 %d バイト。\n" % (len(to_ucs) * 2 + len(by_ucs) * 2))
    out.write(" */\n")
    out.write("#include <stddef.h>\n#include <stdint.h>\n\n")
    out.write('#include "w98wm.h"\n\n')
    out.write("#define JIS_ROWS %d\n" % ROWS)
    out.write("#define JIS_NMAP %d\n\n" % len(by_ucs))

    out.write("/* [区-1][点-1] -> UCS。0 は未定義 */\n")
    out.write("static const uint16_t jis_to_ucs[JIS_ROWS * JIS_ROWS] = {\n")
    for i in range(0, len(to_ucs), 12):
        out.write("\t" + ", ".join("0x%04X" % v for v in to_ucs[i:i + 12]) + ",\n")
    out.write("};\n\n")

    out.write("/* jis_to_ucs の添字を UCS 昇順に並べたもの (二分探索用) */\n")
    out.write("static const uint16_t jis_by_ucs[JIS_NMAP] = {\n")
    for i in range(0, len(by_ucs), 12):
        out.write("\t" + ", ".join("%d" % v for v in by_ucs[i:i + 12]) + ",\n")
    out.write("};\n\n")

    out.write(BODY)


BODY = r'''/*
 * 表に無いが、実際の UTF-8 テキストでは同じ字として使われる異体の対応。
 * Windows 由来の文字列は「〜」を U+FF5E、「‖」を U+2225 で持ってくる
 * (CP932 の対応表が JIS の規格票と違うため)。これを拾わないと、
 * よく使う記号だけが豆腐になる。
 */
static const struct { uint16_t ucs, jis; } jis_alias[] = {
	{ 0x00A2, 0x2171 },   /* ¢ -> ￠ */
	{ 0x00A3, 0x2172 },   /* £ -> ￡ */
	{ 0x00AC, 0x224C },   /* ¬ -> ￢ */
	{ 0x2014, 0x213D },   /* — (EM DASH) -> ― */
	{ 0x2225, 0x2142 },   /* ∥ -> ‖ */
	{ 0xFF0D, 0x215D },   /* － -> − */
	{ 0xFF5E, 0x2141 },   /* ～ -> 〜 */
	{ 0xFFE0, 0x2171 },
	{ 0xFFE1, 0x2172 },
	{ 0xFFE2, 0x224C },
};

uint16_t jis0208_to_ucs(uint8_t b1, uint8_t b2)
{
	b1 &= 0x7Fu;
	b2 &= 0x7Fu;
	if (b1 < 0x21u || b1 > 0x7Eu || b2 < 0x21u || b2 > 0x7Eu)
		return 0;
	return jis_to_ucs[(b1 - 0x21u) * JIS_ROWS + (b2 - 0x21u)];
}

uint16_t ucs_to_jis0208(uint16_t ucs)
{
	size_t lo = 0, hi = JIS_NMAP;

	if (ucs < 0x80u)
		return 0;
	while (lo < hi) {
		size_t mid = lo + (hi - lo) / 2;
		uint16_t idx = jis_by_ucs[mid];
		uint16_t u = jis_to_ucs[idx];

		if (u == ucs)
			return (uint16_t)(((idx / JIS_ROWS + 0x21u) << 8) |
			                  (idx % JIS_ROWS + 0x21u));
		if (u < ucs)
			lo = mid + 1;
		else
			hi = mid;
	}
	for (lo = 0; lo < sizeof(jis_alias) / sizeof(jis_alias[0]); lo++)
		if (jis_alias[lo].ucs == ucs)
			return jis_alias[lo].jis;
	return 0;
}
'''

if __name__ == "__main__":
    main()
