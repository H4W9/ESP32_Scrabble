#!/usr/bin/env python3
"""
generate_scrabble_dict.py
=====================================================================
Build compact DAWG word lists for the ESP32 Scrabble firmware from the
SAME dictionary .xml files the ESP32 Library (dictionary mode) uses.

Why a second index?  The /dictionary/*.xml files are 120-160 MB of
"word - definition" entries.  The .toc/.pgx pair makes ONE user-initiated
definition lookup fast enough, but Scrabble needs sub-100 ms membership
tests on every play and the CPU opponent tests thousands of candidate
words per turn.  So we extract just the headwords once, offline, into a
minimised DAWG that loads into PSRAM.

Definitions still come from the big .xml at play time - this file only
answers "is that a word?" and "what letters can follow this prefix?".

Usage
-----
    python generate_scrabble_dict.py de ../../ESP32_Library/sd_prep/dictionary_out/de-en.xml
    python generate_scrabble_dict.py en ../../ESP32_Library/sd_prep/dictionary_out/en-de.xml

Output (default ./scrabble_out/):
    <lang>.dwg   minimised DAWG, see FORMAT below
    <lang>.txt   the accepted word list (plain, for eyeballing / diffing)

Copy the .dwg files into  /scrabble/  on the SD card.

FORMAT (<lang>.dwg, little-endian)
----------------------------------
    magic   "DWG1"                     4 bytes
    letters uint8  = alphabet size N   1 byte
    pad     3 bytes (0)
    root    uint32 node index          4 bytes
    nedges  uint32 edge count          4 bytes
    nwords  uint32 accepted words      4 bytes
    alpha   N bytes, the alphabet in order (umlauts as firmware private codes)
    edges   nedges * uint32:
              bit 31     end-of-word  (a word terminates on this edge)
              bit 30     last edge of this node
              bits 29-24 letter index into alpha
              bits 23-0  target node index (0 = no children)

A node is a run of consecutive edges ending at the one with bit 30 set,
so a node is addressed by the index of its first edge.
"""

import argparse
import os
import re
import sys

# Firmware private byte codes for the German umlauts (see BibleInterface.cpp).
# Keeping the SAME encoding as the reader means one alphabet across the project.
#
# Ä/Ö/Ü are real tiles in German Scrabble (6/8/6 points), so they stay distinct
# letters. ß is NOT a tile -- the German set is 102 tiles with no eszett, and a
# word containing one is played as SS. So ß EXPANDS to two S here; keeping it as
# its own letter (as the reader firmware does) would put words in the list that
# can never legally be played.
UML = {"Ä": b"\x80", "ä": b"\x80",   # A-umlaut (case-folded to one tile)
       "Ö": b"\x82", "ö": b"\x82",   # O-umlaut
       "Ü": b"\x84", "ü": b"\x84",   # U-umlaut
       "ß": b"SS"}                   # eszett -> SS, not a tile

ALPHA_EN = [bytes([c]) for c in range(ord("A"), ord("Z") + 1)]
ALPHA_DE = ALPHA_EN + [bytes([0x80]), bytes([0x82]), bytes([0x84])]

MIN_LEN, MAX_LEN = 2, 15

# dict.cc / wiktionary markers that flag an entry we must not accept as a
# playable word: proper nouns, abbreviations, multi-word phrases, prefixes.
REJECT_MARKERS = re.compile(
    r"\[(?:Eigenname|name|proper\s*n\.?|abbrev\.?|Abk\.|phrase|Phrase|"
    r"prov\.?|Sprichw\.|Präfix|prefix|Suffix|suffix)\]", re.I)

VERSE_RE = re.compile(rb"<verse osisID=\"[^\"]*\">(.*?)</verse>", re.S)

# --- "human-like vocabulary" (the CPU's reduced word list) -------------------
# A "block rare/unknown words" list: the computer plays from a smaller, more
# everyday list so it doesn't win with obscurities.
#
# The exports carry no frequency or register data (no "archaic"/"rare" tags
# survived the original conversion), so commonness is inferred from the two
# sources disagreeing in a useful way:
#
#   * the bilingual dict.cc export is a PRACTICAL translation dictionary — its
#     headwords are broadly words people actually use;
#   * the Wiktionary export adds a huge long tail of obscure standalone entries,
#     but ALSO the inflected forms that German play depends on.
#
# So the reduced list = every bilingual headword, plus any Wiktionary entry that
# is an inflection OF one of those headwords. That keeps "Gräser" (plural of
# Gras, which dict.cc knows) while dropping Wiktionary-only oddities.
#
# The gloss must be matched tightly: a bare " of " also appears in ordinary
# prose ("the first letter of the alphabet"), which would let nearly everything
# through. Requiring an inflection keyword first cuts that out.
# Glosses that describe a word as something OTHER than ordinary current usage.
# These were found by reading the actual entries behind junk suggestions:
#   ez -> "Abbreviation of easy."      ky -> "Alternative form of kye"
#   ya -> "Nonstandard spelling of you"
# Wiktionary states all of this in prose, which is the only register signal the
# export preserved.
REJECT_GLOSS = re.compile(
    r"(?:abbreviation|initialism|acronym|alternative\s+form|alternative\s+spelling|"
    r"nonstandard\s+spelling|informal\s+spelling|eye\s+dialect|misspelling|"
    r"obsolete|archaic|dated|poetic|dialectal|offensive|slur)", re.I)

# Suffixes a modern English inflection may add. Applied ONLY when the inflected
# form actually starts with its lemma, so German forms that prefix instead of
# suffix (spielen -> gespielt) and vowel-changing plurals (Gras -> Gräser) are
# unaffected. This is what rejects "asketh" while keeping "asks"/"asked".
GOOD_SUFFIX = ("S", "ES", "ED", "D", "ING", "ER", "EST", "IES", "IED", "N", "EN", "'S")

INFLECT_RE = re.compile(
    r"(?:plural|singular|participle|preterite|subjunctive|imperative|genitive|"
    r"dative|accusative|nominative|comparative|superlative|inflection|"
    r"infinitive|past tense|present tense|[a-z]+-person)"
    r"[^.;]{0,40}? of ([^\s,;.]{2,30})", re.I)


# --- corpus frequency ---------------------------------------------------------
# The soundest "is this an everyday word" signal available locally is simply how
# often the word occurs in a body of ordinary prose. For English the dictionary
# files supply that themselves: their definitions are millions of words of plain
# English, and counting tokens across them separates cleanly --
#   ask 820  tries 125  house 5415  ox 266   vs.
#   skirmishings 1  inhalings 1  tempested 1  ky 19  ya 110
# CHOOSE THE CORPUS BY THE LANGUAGE OF THE DEFINITIONS, NOT THE HEADWORDS:
#   en-de.xml has ENGLISH headwords and GERMAN definitions
#   wiktionary-en.xml has English headwords and ENGLISH definitions  <- English prose
# Passing en-de.xml for an English run leaks German words (ALS, AUF, BIS) into
# the reduced list. Neither German file has German definitions, so a German run
# needs a real German corpus -- the reader firmware's bible/song XMLs serve.
TOKEN_RE = re.compile(r"[^\W\d_]{2,15}", re.UNICODE)


def build_freq(paths, alphabet_set):
    """Word frequencies over the DEFINITION PROSE of the given files.

    Only the text after the headword counts, and bracketed material is stripped
    first. Tokenising the raw file instead pulls in the XML itself (osisID, the
    letter-bucket codes) and the grammatical tags ([n.], [pl.], [adj.]), which
    inflates exactly the two-letter tokens the reduced list is most sensitive to
    -- an early version let CD, CF, QM and SJ through that way.
    """
    from collections import Counter
    freq = Counter()
    brackets = re.compile(r"[\[\(\{][^\]\)\}]*[\]\)\}]")
    for path in paths:
        with open(path, "rb") as f:
            tail = b""
            while True:
                chunk = f.read(1 << 22)
                if not chunk:
                    break
                buf = tail + chunk
                last = 0
                for m in VERSE_RE.finditer(buf):
                    last = m.end()
                    entry = m.group(1).decode("utf-8", "replace")
                    # Dictionary entries are "word - definition"; a plain prose
                    # corpus (e.g. an OSIS bible) has no separator, so use it whole.
                    _, sep, rest = entry.partition(" - ")
                    if not sep:
                        rest = entry
                    if not rest:
                        continue
                    rest = brackets.sub(" ", rest)
                    for t in TOKEN_RE.finditer(rest):
                        w = normalise(t.group(0), alphabet_set)
                        if w:
                            freq[w] += 1
                tail = buf[last:] if last else buf[-4096:]
    return freq


def inflection_ok(lemma, form):
    """Is `form` a plausible modern inflection of `lemma`?

    Only judged when the form is the lemma plus a suffix. Anything else (German
    ge- participles, umlaut plurals like Gras -> Graeser) is left alone, because
    this rule has no opinion about those.

    Handles the two regular English spelling changes as well as the plain case,
    so quiz -> quizzes and try -> tries survive while ask -> asketh does not.
    """
    if len(form) <= len(lemma):
        return True
    tail = None
    if form.startswith(lemma):
        tail = form[len(lemma):]                       # ask -> asks
    elif lemma and form.startswith(lemma + lemma[-1:]):
        tail = form[len(lemma) + 1:]                   # quiz -> quizzes, stop -> stopped
    elif lemma.endswith(b"Y") and form.startswith(lemma[:-1] + b"I"):
        tail = form[len(lemma):]                       # try -> tries
    if tail is None:
        return True                                    # not a suffixing inflection
    return tail.decode("latin1").upper() in GOOD_SUFFIX


def normalise(word, alphabet_set):
    """Fold a headword to firmware bytes, or return None if unplayable."""
    out = bytearray()
    for ch in word:
        if ch in UML:
            out += UML[ch]                   # may expand (ß -> SS)
        elif "a" <= ch <= "z":
            out.append(ord(ch.upper()))
        elif "A" <= ch <= "Z":
            out.append(ord(ch))
        else:
            return None                      # space, hyphen, digit, apostrophe...
    if not (MIN_LEN <= len(out) <= MAX_LEN):
        return None
    for b in out:
        if bytes([b]) not in alphabet_set:
            return None
    return bytes(out)


def harvest(xml_path, alphabet, seen, proper, tagged_ok, tagged,
            base=None, common=None, gloss_bad=None, gloss_good=None):
    """Stream one dictionary .xml, adding headwords to `seen`.

    Entries tagged as proper nouns go into `proper` instead - the caller
    subtracts that set at the end.  This matters because the bilingual
    dict.cc exports do NOT tag proper nouns (so "Berlin"/"London" leak in),
    while the Wiktionary exports DO tag them [Eigenname]/[proper n.].
    Cross-filtering one source with the other cleans both.
    """
    alphabet_set = set(alphabet)
    kept = dropped = 0
    with open(xml_path, "rb") as f:
        tail = b""
        while True:
            chunk = f.read(1 << 20)
            if not chunk:
                break
            buf = tail + chunk
            last = 0
            for m in VERSE_RE.finditer(buf):
                last = m.end()
                entry = m.group(1).decode("utf-8", "replace")
                head, _, rest = entry.partition(" - ")
                if not head:
                    continue
                # strip trailing qualifiers the headword sometimes carries
                head = re.sub(r"\s*[\[\(\{].*$", "", head).strip()
                w = normalise(head, alphabet_set)
                if w is None:
                    dropped += 1
                    continue
                if REJECT_MARKERS.search(rest):
                    proper.add(w)
                    dropped += 1
                    continue
                # Not a proper noun, but not everyday usage either: keep it in
                # the FULL list (it is still a legal word) and out of the
                # reduced one by never offering it to the common-list rules.
                gloss_ok = not REJECT_GLOSS.search(rest)
                if tagged:
                    (gloss_good if gloss_ok else gloss_bad).add(w)
                if tagged:
                    # a non-proper sense in the tagged source vindicates the word
                    tagged_ok.add(w)
                    # Reduced list: keep it if the bilingual source already knows
                    # the word, or if it inflects a word the bilingual source knows.
                    if common is not None and base is not None and gloss_ok:
                        if w in base:
                            common.add(w)
                        else:
                            m = INFLECT_RE.search(rest)
                            if m:
                                lem = normalise(m.group(1).strip(), alphabet_set)
                                if lem and lem in base and inflection_ok(lem, w):
                                    common.add(w)
                elif base is not None:
                    # Bilingual headword. How MANY entries a headword has is the
                    # one frequency proxy in this data: a word with several
                    # translation senses is everyday vocabulary, a word with one
                    # is usually a technical term or a German compound. The
                    # count decides membership of the reduced list later.
                    base[w] = base.get(w, 0) + 1
                if w not in seen:
                    seen.add(w)
                    kept += 1
            tail = buf[last:] if last else buf[-4096:]
    return kept, dropped


class Dawg:
    """Incremental DAWG construction (Daciuk et al.) over sorted input."""

    class Node:
        __slots__ = ("final", "edges", "id")

        def __init__(self):
            self.final = False
            self.edges = {}          # letter index -> Node
            self.id = None

        def key(self):
            return (self.final,
                    tuple((k, id(v)) for k, v in sorted(self.edges.items())))

    def __init__(self, alphabet):
        self.alphabet = alphabet
        self.index = {bytes([a[0]]): i for i, a in enumerate(alphabet)}
        self.root = self.Node()
        self.registry = {}
        self.prev = b""
        self.stack = []              # (parent, letter_index, child) unminimised path

    def insert(self, word):
        # find common prefix with the previous word
        common = 0
        while (common < len(word) and common < len(self.prev)
               and word[common] == self.prev[common]):
            common += 1
        self._minimise(common)

        node = self.root if not self.stack else self.stack[-1][2]
        for b in word[common:]:
            li = self.index[bytes([b])]
            child = self.Node()
            node.edges[li] = child
            self.stack.append((node, li, child))
            node = child
        node.final = True
        self.prev = word

    def _minimise(self, down_to):
        while len(self.stack) > down_to:
            parent, li, child = self.stack.pop()
            k = child.key()
            found = self.registry.get(k)
            if found is not None:
                parent.edges[li] = found
            else:
                self.registry[k] = child

    def finish(self):
        self._minimise(0)

    def serialise(self, nwords):
        """Lay nodes out as consecutive edge runs; return (bytes, nedges)."""
        # assign each distinct node a first-edge index
        order = []

        def visit(n):
            if n.id is not None or not n.edges:
                return
            n.id = -1                       # mark visiting
            order.append(n)
            for _, c in sorted(n.edges.items()):
                visit(c)

        visit(self.root)

        pos = 0
        for n in order:
            n.id = pos
            pos += len(n.edges)
        nedges = pos

        edges = bytearray(nedges * 4)
        for n in order:
            items = sorted(n.edges.items())
            for i, (li, child) in enumerate(items):
                target = child.id if child.edges else 0
                if target is None or target < 0:
                    target = 0
                v = (target & 0xFFFFFF) | ((li & 0x3F) << 24)
                if i == len(items) - 1:
                    v |= 1 << 30
                if child.final:
                    v |= 1 << 31
                off = (n.id + i) * 4
                edges[off:off + 4] = v.to_bytes(4, "little")

        alpha = b"".join(self.alphabet)
        header = (b"DWG1"
                  + bytes([len(self.alphabet), 0, 0, 0])
                  + (self.root.id if self.root.edges else 0).to_bytes(4, "little")
                  + nedges.to_bytes(4, "little")
                  + nwords.to_bytes(4, "little")
                  + alpha)
        return header + bytes(edges), nedges


def verify(blob, words, alphabet):
    """Walk the serialised DAWG for every word - catches layout bugs."""
    n = blob[4]
    root = int.from_bytes(blob[8:12], "little")
    nedges = int.from_bytes(blob[12:16], "little")
    base = 20 + n
    alpha = {bytes([blob[20 + i]]): i for i in range(n)}

    def edge(i):
        v = int.from_bytes(blob[base + i * 4: base + i * 4 + 4], "little")
        return (v >> 31) & 1, (v >> 30) & 1, (v >> 24) & 0x3F, v & 0xFFFFFF

    def contains(w):
        node = root
        for k, b in enumerate(w):
            li = alpha[bytes([b])]
            i = node
            while True:
                final, last, letter, target = edge(i)
                if letter == li:
                    if k == len(w) - 1:
                        return bool(final)
                    if not target:
                        return False
                    node = target
                    break
                if last:
                    return False
                i += 1
        return False

    bad = [w for w in words[::max(1, len(words) // 5000)] if not contains(w)]
    return bad, nedges


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("lang", choices=["de", "en"])
    ap.add_argument("xml", nargs="+",
                    help="dictionary_out/*.xml to harvest headwords from. Pass BOTH "
                         "the bilingual export and the Wiktionary one: the former has "
                         "the base vocabulary, the latter the inflected forms German "
                         "Scrabble needs (Graeser, Haeuser, spielte, gespielt).")
    ap.add_argument("--out", default="scrabble_out")
    ap.add_argument("--corpus", nargs="*", default=None,
                    help="prose files whose word frequencies decide the CPU's "
                         "reduced list. For English pass the dictionary .xml "
                         "files themselves (their definitions ARE English "
                         "prose); for German pass a German corpus. Without this "
                         "the older sense-count heuristic is used instead.")
    ap.add_argument("--freq-min", type=int, default=12, dest="freq_min",
                    help="occurrences needed in the corpus (default 12)")
    ap.add_argument("--freq-min-short", type=int, default=200, dest="freq_min_short",
                    help="occurrences needed for words of 3 letters or fewer "
                         "(default 200). Short words are played constantly, and "
                         "the corpus separates them cleanly: OX 266 and up are "
                         "real, YA 110 and below are noise")
    ap.add_argument("--short-min-senses", type=int, default=5, dest="short_min_senses",
                    help="stricter threshold for words of 3 letters or fewer "
                         "(default 7). Short words get played constantly, so an "
                         "obscure one is far more visible than an obscure long one")
    ap.add_argument("--min-senses", type=int, default=3, dest="min_senses",
                    help="entries a bilingual headword needs before it counts as "
                         "everyday vocabulary for the CPU's reduced word list "
                         "(default 3; 1 = no filtering, 4+ = stricter)")
    args = ap.parse_args()

    alphabet = ALPHA_DE if args.lang == "de" else ALPHA_EN
    os.makedirs(args.out, exist_ok=True)

    seen, proper, tagged_ok = set(), set(), set()
    sense_count, common = {}, set()
    gloss_bad, gloss_good = set(), set()

    # Only the Wiktionary exports carry [Eigenname]/[proper n.] sense tags; they
    # are the authority on what is merely a name. They are also the long-tail
    # source, so the reduced list is judged against the bilingual ones — which
    # means the bilingual files have to be read FIRST.
    def is_tagged(p):
        return "wiktionary" in os.path.basename(p).lower()
    paths = sorted(args.xml, key=is_tagged)

    for path in paths:
        tagged = is_tagged(path)
        if tagged and not common:
            # Bilingual passes are done: freeze the everyday vocabulary before
            # the long tail arrives, so inflections are matched against it.
            common.update(w for w, c in sense_count.items()
                          if c >= (args.short_min_senses if len(w) <= 3 else args.min_senses))
            print(f"  reduced-list core: {len(common)} of {len(sense_count)} bilingual "
                  f"headwords (>= {args.min_senses} senses, "
                  f">= {args.short_min_senses} for 2-3 letters)")
        print(f"harvesting {path} ...{' (sense-tagged)' if tagged else ''}")
        kept, dropped = harvest(path, alphabet, seen, proper, tagged_ok, tagged,
                                sense_count if not tagged else common, common,
                                gloss_bad, gloss_good)
        print(f"  accepted {kept} headwords, rejected {dropped}")

    # Drop a word only when the tagged source knows it EXCLUSIVELY as a proper
    # noun. Words it also lists as a common noun/verb (Haus, House) survive, and
    # words it never mentions at all are left to the bilingual source's judgement.
    kill = (proper - tagged_ok) & seen
    seen -= kill
    words = sorted(seen)
    print(f"  dropped {len(kill)} proper-noun-only words via the tagged source")

    d = Dawg(alphabet)
    for w in words:
        d.insert(w)
    d.finish()
    blob, nedges = d.serialise(len(words))

    bad, _ = verify(blob, words, alphabet)
    if bad:
        print(f"  !! DAWG verify FAILED on {len(bad)} sampled words, e.g. {bad[:3]}")
        return 1
    print(f"  DAWG verify OK (sampled)")

    dwg = os.path.join(args.out, args.lang + ".dwg")
    with open(dwg, "wb") as f:
        f.write(blob)
    with open(os.path.join(args.out, args.lang + ".txt"), "wb") as f:
        for w in words:
            f.write(w + b"\n")

    print(f"  {len(words)} words, {nedges} edges, {len(blob)/1048576:.2f} MB -> {dwg}")

    # Reduced "human-like" list for the CPU opponent.
    if args.corpus:
        freq = build_freq(args.corpus, set(alphabet))
        # SHORT WORDS ARE NOT FREQUENCY-GATED.
        #
        # Two- and three-letter words are the connective tissue of the game:
        # almost every tile laid beside an existing one forms one, so the CPU
        # needs the whole set or it fails nearly every cross-check and ends up
        # skipping turn after turn. An earlier build gated them at 200
        # occurrences and lost 62 of the 98 real two-letter words, which is
        # exactly what made the opponent pass every turn.
        #
        # They are still filtered, just by MEANING rather than frequency: the
        # gloss filter drops the "abbreviation of"/"alternative form of" entries
        # that make up most of the junk at this length.
        common = set()
        for w in seen:
            if len(w) <= 3:
                common.add(w)
            elif freq.get(w, 0) >= args.freq_min:
                common.add(w)
        nshort = sum(1 for w in common if len(w) <= 3)
        print(f"  corpus: {len(freq)} distinct tokens -> {len(common)} words "
              f"(>= {args.freq_min}; all {nshort} words of 1-3 letters kept)")

    # Drop anything the tagged source knows ONLY as an abbreviation, alternative
    # spelling, archaism and so on — even if the bilingual source listed it.
    slop = (gloss_bad - gloss_good) & common
    common -= slop
    print(f"  dropped {len(slop)} non-standard forms (abbrev./variant/archaic)")
    common &= seen                     # never offer the CPU a word the rules reject
    cwords = sorted(common)
    if len(cwords) < 2:
        print("  !! reduced list came out empty - skipping")
        return 0
    cd = Dawg(alphabet)
    for w in cwords:
        cd.insert(w)
    cd.finish()
    cblob, cedges = cd.serialise(len(cwords))
    bad, _ = verify(cblob, cwords, alphabet)
    if bad:
        print(f"  !! reduced DAWG verify FAILED, e.g. {bad[:3]}")
        return 1
    cdwg = os.path.join(args.out, args.lang + "_common.dwg")
    with open(cdwg, "wb") as f:
        f.write(cblob)
    with open(os.path.join(args.out, args.lang + "_common.txt"), "wb") as f:
        for w in cwords:
            f.write(w + b"\n")
    print(f"  reduced: {len(cwords)} words ({100*len(cwords)//max(len(words),1)}% of full), "
          f"{cedges} edges, {len(cblob)/1048576:.2f} MB -> {cdwg}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
