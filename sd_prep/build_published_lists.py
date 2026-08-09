#!/usr/bin/env python3
"""
build_published_lists.py
=====================================================================
Build the four firmware .dwg word lists that ship with the repo, into
./scrabble_out/ :

    en.dwg  en_common.dwg  de.dwg  de_common.dwg

The FULL lists (en.dwg, de.dwg) are the source word lists, folded to the
firmware's letter bytes and minimised into a DAWG.

The COMMON lists (the CPU's "human-like" everyday vocabulary) are the
reduced lists PLUS the 100 most frequent words that are in the full list but
were missing from the reduced one. "Most frequent" is measured over the
definition prose of the matching Wiktionary export (English prose for en, German
prose for de), reusing generate_scrabble_dict.build_freq. Those 100 are real,
common words the reduced list happened to omit.

Everything here is derived from the word-list .txt in ../example/word_lists/ and
the dictionary XML in the ESP32_Library tree; both are inputs you supply, and
neither is committed. See README for the source/licensing note.

    python build_published_lists.py                 # both languages
    python build_published_lists.py --extra 100     # words added per common list
"""

import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from generate_scrabble_dict import Dawg, ALPHA_EN, ALPHA_DE, verify, build_freq
from wordlist_to_dawg import fold

WL  = os.path.join(HERE, "..", "example", "word_lists")
XML = os.path.join(HERE, "..", "..", "ESP32_Bible", "sd_prep", "dictionary_out")
OUT = os.path.join(HERE, "scrabble_out")

CONFIG = {
    "en": dict(full="en_csw24.txt", reduced="en_reduced.txt",
               corpus=["wiktionary-en.xml"], alpha=ALPHA_EN),
    "de": dict(full="de.txt", reduced="de_reduced.txt",
               corpus=["wiktionary-de.xml"], alpha=ALPHA_DE),
}


def load(path, aset):
    words = set()
    with open(path, encoding="utf-8") as f:
        for line in f:
            w = fold(line.strip(), aset)
            if w:
                words.add(w)
    return words


def build_dwg(words, alphabet, out):
    ws = sorted(words)
    d = Dawg(alphabet)
    for w in ws:
        d.insert(w)
    d.finish()
    blob, nedges = d.serialise(len(ws))
    bad, _ = verify(blob, ws, alphabet)
    if bad:
        raise SystemExit(f"  !! DAWG verify FAILED, e.g. {bad[:3]}")
    with open(out, "wb") as f:
        f.write(blob)
    return len(ws), nedges, len(blob)


def show(word_bytes):
    """Firmware bytes back to something readable for the log."""
    m = {0x80: "AE", 0x82: "OE", 0x84: "UE"}
    return "".join(m.get(b, chr(b)) for b in word_bytes)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--extra", type=int, default=100,
                    help="high-frequency words to add to each common list")
    ap.add_argument("--out", default=OUT)
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)

    for lang, cfg in CONFIG.items():
        aset = set(cfg["alpha"])
        full = load(os.path.join(WL, cfg["full"]), aset)
        reduced = load(os.path.join(WL, cfg["reduced"]), aset)
        print(f"[{lang}] full {len(full)}, reduced {len(reduced)}")

        # The 100 most frequent words in the full list that the reduced one
        # lacks. freq is keyed by the same folded bytes fold() produces.
        corpus = [os.path.join(XML, c) for c in cfg["corpus"]]
        print(f"[{lang}] reading corpus frequency from {', '.join(cfg['corpus'])} ...")
        freq = build_freq(corpus, aset)
        cands = [w for w in (full - reduced) if freq.get(w, 0) > 0]
        cands.sort(key=lambda w: (freq[w], w), reverse=True)
        add = cands[:args.extra]
        print(f"[{lang}] adding {len(add)} words to the common list: "
              f"{', '.join(show(w) for w in add[:20])}"
              f"{' ...' if len(add) > 20 else ''}")
        common = reduced | set(add)

        n, e, b = build_dwg(full, cfg["alpha"], os.path.join(args.out, f"{lang}.dwg"))
        print(f"[{lang}] {lang}.dwg        {n} words, {b/1048576:.2f} MB")
        n, e, b = build_dwg(common, cfg["alpha"], os.path.join(args.out, f"{lang}_common.dwg"))
        print(f"[{lang}] {lang}_common.dwg {n} words, {b/1048576:.2f} MB\n")

    return 0


if __name__ == "__main__":
    sys.exit(main())
