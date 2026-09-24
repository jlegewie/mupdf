#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CORPUS_DIR="${1:-$ROOT/fork-regressions/data}"
MUTOOL="${MUTOOL:-$ROOT/build/debug/mutool}"

if [[ ! -x "$MUTOOL" ]]; then
	printf 'error: mutool not found or not executable: %s\n' "$MUTOOL" >&2
	printf 'hint: run `make build=debug build/debug/mutool`, or set MUTOOL=/path/to/mutool\n' >&2
	exit 1
fi

if [[ ! -d "$CORPUS_DIR" ]]; then
	printf 'error: corpus directory does not exist: %s\n' "$CORPUS_DIR" >&2
	exit 1
fi

files=()
while IFS= read -r -d '' file; do
	files+=("$file")
done < <(find "$CORPUS_DIR" -type f ! -name '.gitkeep' ! -name '.DS_Store' -print0 | sort -z)

if (( ${#files[@]} == 0 )); then
	printf 'No fork regression files found in %s\n' "$CORPUS_DIR"
	exit 0
fi

printf 'Running %d fork regression file(s) with %s\n' "${#files[@]}" "$MUTOOL"

# Smoke pass: every corpus file must extract without crashing/hanging. Several
# files are intentionally damaged PDFs (the repair fixes), so mutool legitimately
# reports recovery errors and a normal non-zero exit — that is tolerated. But a
# crash (segfault/abort/kill) is exactly what this smoke check exists to catch,
# so a signal exit (128+N) still fails the run. The goal is "does not crash",
# not "exit 0".
smoke_failed=0
for file in "${files[@]}"; do
	rel="${file#"$ROOT"/}"
	printf '==> %s\n' "$rel"
	status=0
	"$MUTOOL" draw -st "$file" || status=$?
	if (( status >= 128 )); then
		printf '   CRASH: mutool terminated by signal %d on %s\n' "$(( status - 128 ))" "$rel" >&2
		smoke_failed=1
	elif (( status != 0 )); then
		printf '   (mutool exited %d on damaged input — tolerated)\n' "$status"
	fi
done
if (( smoke_failed )); then
	printf 'error: one or more corpus files crashed during the smoke pass\n' >&2
	exit 1
fi

# Content assertion for the `use-glyph-name-for-unknown-unicode` option:
# OFF must still emit U+FFFD (so an unmapped text layer stays detectable),
# ON must recover readable text. NB: `mutool draw -O` is the spots option and
# does NOT parse stext flags, so this must go through `toStructuredText` via
# `mutool run`.
GLYPH_SAMPLE="$CORPUS_DIR/distiller-c-glyphs/sample.pdf"
if [[ -f "$GLYPH_SAMPLE" ]]; then
	printf '==> glyph-name recovery assertion (%s)\n' "${GLYPH_SAMPLE#"$ROOT"/}"
	script="$(mktemp -t glyphname.XXXXXX.js)"
	trap 'rm -f "$script"' EXIT
	cat > "$script" <<'JS'
var path = scriptArgs[0];
var page = Document.openDocument(path).loadPage(0);
var off = page.toStructuredText("preserve-whitespace").asText();
var on = page.toStructuredText("preserve-whitespace,use-glyph-name-for-unknown-unicode").asText();
var FFFD = String.fromCharCode(0xFFFD);
if (off.indexOf(FFFD) < 0)
	throw new Error("FAIL: option OFF should leave U+FFFD (unmapped layer must stay detectable)");
if (on.indexOf("Management of risks") < 0)
	throw new Error("FAIL: option ON should recover readable text");
if (on.indexOf(FFFD) >= 0)
	throw new Error("FAIL: option ON should resolve the U+FFFD runs");
print("OK: glyph-name recovery off=U+FFFD on=recovered");
JS
	"$MUTOOL" run "$script" "$GLYPH_SAMPLE"
fi

# Content assertion for the `use-known-glyph-outlines` option: a symbol font
# whose glyph "m" draws a mu (Elsevier Advent font, no ToUnicode). OFF must keep
# upstream output; ON must recover the mu and leave ordinary Latin text alone.
OUTLINE_SAMPLE="$CORPUS_DIR/known-glyph-outlines/sample.pdf"
if [[ -f "$OUTLINE_SAMPLE" ]]; then
	printf '==> known-glyph-outline assertion (%s)\n' "${OUTLINE_SAMPLE#"$ROOT"/}"
	script="$(mktemp -t knownoutline.XXXXXX.js)"
	trap 'rm -f "$script"' EXIT
	cat > "$script" <<'JS'
var page = Document.openDocument(scriptArgs[0]).loadPage(0);
var off = page.toStructuredText("preserve-whitespace").asText();
var on = page.toStructuredText("preserve-whitespace,use-known-glyph-outlines").asText();
if (off.indexOf("20 mg of NP-Ova") < 0)
	throw new Error("FAIL: option OFF should keep upstream output (20 mg)");
if (on.indexOf("20 μg of NP-Ova") < 0)
	throw new Error("FAIL: option ON should recover the mu (20 μg)");
if (on.indexOf("footpad-immunized") < 0 || on.length != off.length)
	throw new Error("FAIL: option ON should leave ordinary text unchanged");
print("OK: known-glyph-outline off=upstream on=recovered");
JS
	"$MUTOOL" run "$script" "$OUTLINE_SAMPLE"
fi

# Same option, font WITH a broken ToUnicode CMap: Elsevier's ToUnicode maps its
# "=" to "¼". ON must replace that implausible value; OFF keeps upstream output.
TU_SAMPLE="$CORPUS_DIR/known-glyph-outlines/tounicode-sample.pdf"
if [[ -f "$TU_SAMPLE" ]]; then
	printf '==> known-glyph-outline ToUnicode assertion (%s)\n' "${TU_SAMPLE#"$ROOT"/}"
	script="$(mktemp -t knownoutlinetu.XXXXXX.js)"
	trap 'rm -f "$script"' EXIT
	cat > "$script" <<'JS'
var page = Document.openDocument(scriptArgs[0]).loadPage(0);
var off = page.toStructuredText("preserve-whitespace").asText();
var on = page.toStructuredText("preserve-whitespace,use-known-glyph-outlines").asText();
if (off.indexOf("Nc ¼ N") < 0)
	throw new Error("FAIL: option OFF should keep the ToUnicode value (Nc ¼ N)");
if (on.indexOf("Nc = N −Nt") < 0)
	throw new Error("FAIL: option ON should replace the broken ToUnicode value (Nc = N −Nt)");
if (on.indexOf("¼") >= 0)
	throw new Error("FAIL: option ON should leave no ¼ for the equals outline");
print("OK: known-glyph-outline ToUnicode off=upstream on=recovered");
JS
	"$MUTOOL" run "$script" "$TU_SAMPLE"
fi

# Same option inside marked content: explicit /ActualText wins. Text that
# ActualText confirms (exact match, or a matching prefix/suffix) must not be
# rewritten by the outline heuristic; text outside ActualText still is.
AT_SAMPLE="$CORPUS_DIR/known-glyph-outlines/actualtext-sample.pdf"
if [[ -f "$AT_SAMPLE" ]]; then
	printf '==> known-glyph-outline ActualText assertion (%s)\n' "${AT_SAMPLE#"$ROOT"/}"
	script="$(mktemp -t knownoutlineat.XXXXXX.js)"
	trap 'rm -f "$script"' EXIT
	cat > "$script" <<'JS'
var page = Document.openDocument(scriptArgs[0]).loadPage(0);
function chars(o) { return page.toStructuredText(o).asText().replace(/\s+/g, ""); }
var off = chars("preserve-whitespace");
var on = chars("preserve-whitespace,use-known-glyph-outlines");
var ign = chars("preserve-whitespace,use-known-glyph-outlines,ignore-actualtext");
if (off != "mmm")
	throw new Error("FAIL: option OFF should give mmm, got " + off);
if (on != "mmμ")
	throw new Error("FAIL: option ON must keep ActualText-confirmed m and recover only the plain one, got " + on);
if (ign != "μμμ")
	throw new Error("FAIL: with ignore-actualtext every glyph should be recovered, got " + ign);
print("OK: known-glyph-outline respects ActualText");
JS
	"$MUTOOL" run "$script" "$AT_SAMPLE"
fi

# Same option, ToUnicode maps a TeX-style extension font's summation to the
# ASCII letter "X": ON must replace it (an ASCII letter is overridden only by a
# symbol from the table).
ASCII_SAMPLE="$CORPUS_DIR/known-glyph-outlines/ascii-tounicode-sample.pdf"
if [[ -f "$ASCII_SAMPLE" ]]; then
	printf '==> known-glyph-outline ASCII ToUnicode assertion (%s)\n' "${ASCII_SAMPLE#"$ROOT"/}"
	script="$(mktemp -t knownoutlineascii.XXXXXX.js)"
	trap 'rm -f "$script"' EXIT
	cat > "$script" <<'JS'
var page = Document.openDocument(scriptArgs[0]).loadPage(0);
function count(o, c) { return page.toStructuredText(o).asText().split(c).length - 1; }
var off = "preserve-whitespace", on = "preserve-whitespace,use-known-glyph-outlines";
if (count(off, "X") != 2 || count(off, "∑") != 0)
	throw new Error("FAIL: option OFF should keep the ToUnicode X");
if (count(on, "X") != 0 || count(on, "∑") != 2)
	throw new Error("FAIL: option ON should replace the ToUnicode X with ∑");
print("OK: known-glyph-outline ASCII ToUnicode off=upstream on=recovered");
JS
	"$MUTOOL" run "$script" "$ASCII_SAMPLE"
fi

# `space-after-symbols`: a word gap after a math symbol becomes a space only
# with the option (upstream never adds one after U+2100 and above).
if [[ -f "$TU_SAMPLE" ]]; then
	printf '==> space-after-symbols assertion (%s)\n' "${TU_SAMPLE#"$ROOT"/}"
	script="$(mktemp -t spacesym.XXXXXX.js)"
	trap 'rm -f "$script"' EXIT
	cat > "$script" <<'JS'
var page = Document.openDocument(scriptArgs[0]).loadPage(0);
var off = page.toStructuredText("preserve-whitespace,use-known-glyph-outlines").asText();
var on = page.toStructuredText("preserve-whitespace,use-known-glyph-outlines,space-after-symbols").asText();
if (off.indexOf("Nc = N −Nt") < 0)
	throw new Error("FAIL: without space-after-symbols the gap after − should stay closed");
if (on.indexOf("Nc = N − Nt") < 0)
	throw new Error("FAIL: space-after-symbols should turn the gap after − into a space");
print("OK: space-after-symbols off=upstream on=spaced");
JS
	"$MUTOOL" run "$script" "$TU_SAMPLE"
fi

# `space-after-symbols` boundaries: a gap after a symbol becomes a space before
# Latin text and halfwidth Hangul (Korean uses word spaces), but not before a
# closing quote or bracket, an ASCII quote, or Chinese/Japanese text.
SPACING_SAMPLE="$CORPUS_DIR/known-glyph-outlines/symbol-spacing-sample.pdf"
if [[ -f "$SPACING_SAMPLE" ]]; then
	printf '==> space-after-symbols boundary assertion (%s)\n' "${SPACING_SAMPLE#"$ROOT"/}"
	script="$(mktemp -t spacebound.XXXXXX.js)"
	trap 'rm -f "$script"' EXIT
	cat > "$script" <<'JS'
var page = Document.openDocument(scriptArgs[0]).loadPage(0);
function lines(o) { return page.toStructuredText(o).asText().split("\n").filter(function (l) { return l.length; }).join("|"); }
var off = lines("preserve-whitespace"), on = lines("preserve-whitespace,space-after-symbols");
var wantOff = "→A|→ﾡﾤ|→”|→)|→中|→\"|→'";
var wantOn = "→ A|→ ﾡﾤ|→”|→)|→中|→\"|→'";
if (off != wantOff)
	throw new Error("FAIL: without the option no space should follow the symbol, got " + JSON.stringify(off));
if (on != wantOn)
	throw new Error("FAIL: space-after-symbols boundaries wrong, got " + JSON.stringify(on));
print("OK: space-after-symbols boundaries (Latin, halfwidth Hangul, closing quote, bracket, CJK, ASCII quotes)");
JS
	"$MUTOOL" run "$script" "$SPACING_SAMPLE"
fi
