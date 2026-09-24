/* Reference implementation of glyph outline hash v1 (see build/glyphhash.py). */
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
static uint64_t fnv(uint64_t h, int32_t v) {
	unsigned char b[4] = { v & 0xff, (v >> 8) & 0xff, (v >> 16) & 0xff, (v >> 24) & 0xff };
	for (int i = 0; i < 4; i++) { h ^= b[i]; h *= 0x100000001b3ULL; }
	return h;
}
int main(int argc, char **argv) {
	FILE *f = fopen(argv[1], "rb"); fseek(f, 0, SEEK_END); long n = ftell(f); rewind(f);
	unsigned char *buf = malloc(n); fread(buf, 1, n, f); fclose(f);
	FT_Library lib; FT_Init_FreeType(&lib);
	FT_Face face; if (FT_New_Memory_Face(lib, buf, n, 0, &face)) { fprintf(stderr, "open failed\n"); return 1; }
	for (int gid = 0; gid < face->num_glyphs; gid++) {
		if (FT_Load_Glyph(face, gid, FT_LOAD_NO_SCALE | FT_LOAD_NO_HINTING | FT_LOAD_IGNORE_TRANSFORM)) continue;
		FT_Outline *o = &face->glyph->outline;
		if (o->n_points == 0) continue;
		uint64_t h = 0xcbf29ce484222325ULL;
		h = fnv(h, o->n_contours);
		for (int i = 0; i < o->n_contours; i++) h = fnv(h, o->contours[i]);
		h = fnv(h, o->n_points);
		for (int i = 0; i < o->n_points; i++) {
			h = fnv(h, (int32_t)o->points[i].x); h = fnv(h, (int32_t)o->points[i].y); h = fnv(h, o->tags[i] & 3);
		}
		printf("%d %016llx\n", gid, (unsigned long long)h);
	}
	return 0;
}
