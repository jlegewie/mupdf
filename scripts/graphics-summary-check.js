// Check the graphics-summary device (fork addition) with `mutool run`:
//
//   mutool run scripts/graphics-summary-check.js [--max-pages N] [--timing] file.pdf...
//
// For every page, structured text built through the graphics-summary tee must be
// identical (asJSON) to plain toStructuredText, for each option set below. The
// summary must be well formed: header fields consistent, records inside the page
// bounds, record count within the cap. Prints one line per file and a total; exits
// with an error if any page differs or any summary is malformed.
//
// Before the files, synthetic pages built in memory check specific summaries: an
// image is recorded identically standalone and through the tee under every option
// set, and a tiling-pattern fill gives one tiled record covering the painted area.

var OPTION_SETS = [
	"preserve-whitespace",
	"preserve-whitespace,ignore-actualtext",
	"preserve-whitespace,preserve-images",
	"preserve-whitespace,use-known-glyph-outlines,map-symbol-private-use,use-glyph-name-for-garbage,space-after-symbols",
	"preserve-whitespace,preserve-images,use-known-glyph-outlines,map-symbol-private-use,use-glyph-name-for-garbage,space-after-symbols,use-cid-for-unknown-unicode,use-glyph-name-for-unknown-unicode",
];
var HEADER = 16, STRIDE = 12, GRID = 32;
var FLAG_TILED = 16;

var maxPages = 1e9, timing = false, files = [];
for (var i = 0; i < scriptArgs.length; i++) {
	if (scriptArgs[i] === "--max-pages") maxPages = parseInt(scriptArgs[++i], 10);
	else if (scriptArgs[i] === "--timing") timing = true;
	else files.push(scriptArgs[i]);
}

function checkSummary(s, bounds) {
	if (s[0] !== 1) return "bad version " + s[0];
	if (s[2] !== STRIDE) return "bad stride " + s[2];
	var count = s[1], gridN = s[9];
	var expected = HEADER + count * STRIDE + (s[8] ? GRID * GRID : 0);
	if (s.length !== expected) return "length " + s.length + " != " + expected;
	if (s[8] && gridN !== GRID) return "overflow without grid";
	var seen = s[3] + s[4] + s[5] + s[6] + s[7];
	if (count > seen) return "more records than primitives seen";
	var eps = 0.5;
	for (var r = 0; r < count; r++) {
		var o = HEADER + r * STRIDE;
		var kind = s[o];
		if (kind < 1 || kind > 5) return "bad kind " + kind;
		if (s[o + 5] < 0 || s[o + 5] > 31) return "bad flags " + s[o + 5] + " in record " + r;
		if (!(s[o + 1] <= s[o + 3] && s[o + 2] <= s[o + 4])) return "inverted bbox in record " + r;
		if (s[o + 1] < bounds[0] - eps || s[o + 2] < bounds[1] - eps || s[o + 3] > bounds[2] + eps || s[o + 4] > bounds[3] + eps)
			return "record " + r + " outside page bounds";
	}
	return null;
}

function records(s) {
	var out = [];
	for (var r = 0; r < s[1]; r++)
		out.push(Array.prototype.slice.call(s, HEADER + r * STRIDE, HEADER + (r + 1) * STRIDE));
	return out;
}

function near(a, b) {
	for (var i = 0; i < b.length; i++)
		if (Math.abs(a[i] - b[i]) > 0.01) return false;
	return true;
}

// Synthetic pages: returns the number of failed assertions.
function checkSynthetic() {
	var failures = 0;
	function fail(msg) { failures++; print("FAIL synthetic: " + msg); }

	var doc = new PDFDocument();
	var pix = new Pixmap(ColorSpace.DeviceRGB, [0, 0, 8, 8], false);
	pix.clear(128);
	var image = doc.addImage(new Image(pix));
	doc.insertPage(-1, doc.addPage([0, 0, 200, 200], 0,
		doc.addObject({ XObject: { Im0: image } }),
		"q 100 0 0 50 20 30 cm /Im0 Do Q"));

	// Pattern cell: a 5x5 square, repeated every 10 units.
	var pattern = doc.addStream("0 0 1 rg 0 0 5 5 re f", {
		Type: "Pattern", PatternType: 1, PaintType: 1, TilingType: 1,
		BBox: [0, 0, 10, 10], XStep: 10, YStep: 10, Resources: {}
	});
	// Pattern cell drawing nothing visible (no record expected).
	var empty = doc.addStream("", {
		Type: "Pattern", PatternType: 1, PaintType: 1, TilingType: 1,
		BBox: [0, 0, 10, 10], XStep: 10, YStep: 10, Resources: {}
	});
	// Pattern cell filled with another tiling pattern (a red 1x1 dot every 2 units).
	var inner = doc.addStream("1 0 0 rg 0 0 1 1 re f", {
		Type: "Pattern", PatternType: 1, PaintType: 1, TilingType: 1,
		BBox: [0, 0, 2, 2], XStep: 2, YStep: 2, Resources: {}
	});
	var outer = doc.addStream("/Pattern cs /Pi scn 0 0 8 8 re f", {
		Type: "Pattern", PatternType: 1, PaintType: 1, TilingType: 1,
		BBox: [0, 0, 10, 10], XStep: 10, YStep: 10, Resources: { Pattern: { Pi: inner } }
	});
	var patRes = doc.addObject({ Pattern: { P0: pattern, P1: empty, P2: outer } });
	// The whole page filled with the pattern.
	doc.insertPage(-1, doc.addPage([0, 0, 200, 200], 0, patRes, "/Pattern cs /P0 scn 0 0 200 200 re f"));
	// A region whose clip excludes the pattern's own cell at the origin.
	doc.insertPage(-1, doc.addPage([0, 0, 200, 200], 0, patRes, "/Pattern cs /P0 scn 100 100 50 50 re f"));
	// An empty pattern cell next to a plain rectangle.
	doc.insertPage(-1, doc.addPage([0, 0, 200, 200], 0, patRes, "/Pattern cs /P1 scn 0 0 200 200 re f 1 0 0 rg 10 10 20 20 re f"));
	// A pattern nested in a pattern cell.
	doc.insertPage(-1, doc.addPage([0, 0, 200, 200], 0, patRes, "/Pattern cs /P2 scn 20 20 100 60 re f"));

	// Image: the same record from the standalone device and the tee under every option set.
	var page = doc.loadPage(0);
	var want = records(page.getGraphicsSummary(0));
	if (want.length !== 1 || want[0][0] !== 3 || !near(want[0], [3, 20, 120, 120, 170]))
		fail("image page: standalone summary should hold one image record at [20,120,120,170], got " + JSON.stringify(want));
	for (var k = 0; k < OPTION_SETS.length; k++) {
		var got = records(page.toStructuredTextWithGraphics(OPTION_SETS[k], 0)[1]);
		if (JSON.stringify(got) !== JSON.stringify(want))
			fail("image page: tee with '" + OPTION_SETS[k] + "' gave " + JSON.stringify(got) + ", standalone " + JSON.stringify(want));
	}

	function checkTiled(p, bbox, label, rgb) {
		var pg = doc.loadPage(p);
		var sums = [pg.getGraphicsSummary(0)];
		for (var k = 0; k < OPTION_SETS.length; k++)
			sums.push(pg.toStructuredTextWithGraphics(OPTION_SETS[k], 0)[1]);
		for (var i = 0; i < sums.length; i++) {
			var recs = records(sums[i]);
			if (recs.length !== 1 || recs[0][0] !== 1 || !(recs[0][5] & FLAG_TILED) || !near(recs[0], [1].concat(bbox)) ||
					recs[0][6] !== rgb || sums[i][3] !== 1)
				fail(label + ": expected one tiled fill record of colour " + rgb + " at " + JSON.stringify(bbox) + " (fill paths seen 1), got " +
					JSON.stringify(recs) + " (fill paths seen " + sums[i][3] + ")");
		}
	}
	checkTiled(1, [0, 0, 200, 200], "tiled page", 0x0000ff);
	checkTiled(2, [100, 50, 150, 100], "tiled region outside the cell", 0x0000ff);
	checkTiled(4, [20, 120, 120, 180], "nested tiling patterns", 0xff0000);

	var recs = records(doc.loadPage(3).getGraphicsSummary(0));
	if (recs.length !== 1 || recs[0][5] & FLAG_TILED || !near(recs[0], [1, 10, 170, 30, 190]))
		fail("empty pattern cell: expected only the plain rectangle, got " + JSON.stringify(recs));

	// The tee must leave the structured text of the synthetic pages unchanged too.
	for (var p = 0; p < doc.countPages(); p++) {
		var pg = doc.loadPage(p);
		for (var k = 0; k < OPTION_SETS.length; k++)
			if (pg.toStructuredTextWithGraphics(OPTION_SETS[k], 0)[0].asJSON() !== pg.toStructuredText(OPTION_SETS[k]).asJSON())
				fail("synthetic page " + p + ": teed structured text differs with " + OPTION_SETS[k]);
	}

	print((failures ? "FAIL " : "ok   ") + "synthetic pages (image through the tee, tiling patterns)");
	return failures;
}

var totalPages = 0, totalFailures = checkSynthetic(), tPlain = 0, tTee = 0, tSummary = 0;
for (var f = 0; f < files.length; f++) {
	var doc, failures = 0, pages = 0, records = 0, overflow = 0;
	try {
		doc = Document.openDocument(files[f]);
	} catch (e) {
		print("SKIP " + files[f] + ": " + e);
		continue;
	}
	var n = Math.min(doc.countPages(), maxPages);
	for (var p = 0; p < n; p++) {
		var page;
		try { page = doc.loadPage(p); } catch (e) { continue; }
		var bounds = page.getBounds();
		for (var k = 0; k < OPTION_SETS.length; k++) {
			var plain, tee, t0 = Date.now();
			try { plain = page.toStructuredText(OPTION_SETS[k]).asJSON(); } catch (e) { plain = "ERROR " + e; }
			var t1 = Date.now();
			try { tee = page.toStructuredTextWithGraphics(OPTION_SETS[k], 0); } catch (e) { tee = ["ERROR " + e, null]; }
			var t2 = Date.now();
			var teeJson = typeof tee[0] === "string" ? tee[0] : tee[0].asJSON();
			if (teeJson !== plain) {
				failures++;
				print("DIFF " + files[f] + " page " + p + " options " + OPTION_SETS[k]);
			}
			if (k === 0) {
				tPlain += t1 - t0; tTee += t2 - t1;
				if (tee[1]) {
					var err = checkSummary(tee[1], bounds);
					if (err) { failures++; print("BAD SUMMARY " + files[f] + " page " + p + ": " + err); }
					records += tee[1][1]; overflow += tee[1][8];
				}
				var t3 = Date.now();
				page.getGraphicsSummary(0);
				tSummary += Date.now() - t3;
			}
		}
		pages++;
	}
	totalPages += pages;
	totalFailures += failures;
	print((failures ? "FAIL " : "ok   ") + files[f] + ": " + pages + " pages, " + records + " records, " + overflow + " overflowing pages");
}
print("pages " + totalPages + ", failures " + totalFailures);
if (timing)
	print("ms: stext " + tPlain + ", stext+summary " + tTee + ", summary only " + tSummary);
if (totalFailures)
	throw new Error("graphics summary check failed");
