// ogg decoding for the sample bank (raylib's own copy of stb_vorbis, now that raudio is gone)
#include <stdlib.h>
#include <string.h>
#define STB_VORBIS_NO_STDIO
#include "external/stb_vorbis.c"

// first channel as float, like raylib's LoadWaveSamples did
float *ogg_decode(const unsigned char *blob, int len, int *n)
{
    stb_vorbis *v = stb_vorbis_open_memory(blob, len, NULL, NULL);
    if (!v) return NULL;
    int ch = stb_vorbis_get_info(v).channels, frames = stb_vorbis_stream_length_in_samples(v);
    short *p = malloc((size_t)frames * ch * 2);
    float *s = malloc(sizeof(float) * frames);
    stb_vorbis_get_samples_short_interleaved(v, ch, p, frames * ch);
    for (int i = 0; i < frames; i++) s[i] = p[i * ch] / 32768.0f;
    free(p);
    stb_vorbis_close(v);
    *n = frames;
    return s;
}
