// the sound card, without miniaudio: the mixer in audio.c fills 44.1 kHz stereo float blocks, this just plays them
#include <stdlib.h>
#include <string.h>

#define FR 1024
typedef void (*fill_fn)(float *out, unsigned frames);
static fill_fn fill;

#ifdef _WIN32
#include <windows.h>
#define NB 4
static short pcm[NB][FR * 2];
static WAVEHDR hdr[NB];
static HWAVEOUT wo;

static void mix(WAVEHDR *h)
{
    float f[FR * 2];
    short *s = (short *)h->lpData;
    fill(f, FR);
    for (int i = 0; i < FR * 2; i++) s[i] = f[i] > 1 ? 32767 : f[i] < -1 ? -32767 : (short)(f[i] * 32767);
}

static void CALLBACK done(HWAVEOUT h, UINT msg, DWORD_PTR inst, DWORD_PTR a, DWORD_PTR b)
{
    if (msg != WOM_DONE) return;
    mix((WAVEHDR *)a);
    waveOutWrite(h, (WAVEHDR *)a, sizeof(WAVEHDR));
}

int out_open(fill_fn f)
{
    WAVEFORMATEX wf = { WAVE_FORMAT_PCM, 2, 44100, 44100 * 4, 4, 16, 0 };
    fill = f;
    if (waveOutOpen(&wo, WAVE_MAPPER, &wf, (DWORD_PTR)done, 0, CALLBACK_FUNCTION)) return 0;
    for (int i = 0; i < NB; i++) {
        hdr[i].lpData = (char *)pcm[i];
        hdr[i].dwBufferLength = sizeof pcm[i];
        waveOutPrepareHeader(wo, &hdr[i], sizeof hdr[i]);
        mix(&hdr[i]);
        waveOutWrite(wo, &hdr[i], sizeof hdr[i]);
    }
    return 1;
}
#elif defined(__APPLE__)
#include <AudioToolbox/AudioToolbox.h>

static void cb(void *u, AudioQueueRef q, AudioQueueBufferRef b)
{
    fill(b->mAudioData, FR);
    b->mAudioDataByteSize = FR * 8;
    AudioQueueEnqueueBuffer(q, b, 0, NULL);
}

int out_open(fill_fn f)
{
    AudioStreamBasicDescription d = { 44100, kAudioFormatLinearPCM, kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked, 8, 1, 8, 2, 32, 0 };
    AudioQueueRef q;
    fill = f;
    if (AudioQueueNewOutput(&d, cb, NULL, NULL, NULL, 0, &q)) return 0;
    for (int i = 0; i < 4; i++) {
        AudioQueueBufferRef b;
        AudioQueueAllocateBuffer(q, FR * 8, &b);
        cb(NULL, q, b);
    }
    return !AudioQueueStart(q, NULL);
}
#else
#include <dlfcn.h>
#include <pthread.h>

static int (*snd_open)(void **, const char *, int, int);
static int (*snd_params)(void *, int, int, unsigned, unsigned, int, unsigned);
static long (*snd_writei)(void *, const void *, unsigned long);
static int (*snd_recover)(void *, int, int);
static void *pcm;

static void *run(void *u)
{
    float f[FR * 2];
    short s[FR * 2];
    for (;;) {
        fill(f, FR);
        for (int i = 0; i < FR * 2; i++) s[i] = f[i] > 1 ? 32767 : f[i] < -1 ? -32767 : (short)(f[i] * 32767);
        long n = snd_writei(pcm, s, FR);
        if (n < 0) snd_recover(pcm, (int)n, 1);
    }
    return NULL;
}

int out_open(fill_fn f)
{
    void *lib = dlopen("libasound.so.2", RTLD_LAZY);
    pthread_t t;
    fill = f;
    if (!lib) return 0;
    snd_open = dlsym(lib, "snd_pcm_open");
    snd_params = dlsym(lib, "snd_pcm_set_params");
    snd_writei = dlsym(lib, "snd_pcm_writei");
    snd_recover = dlsym(lib, "snd_pcm_recover");
    if (!snd_open || !snd_params || !snd_writei || !snd_recover) return 0;
    // format 2 is S16_LE, access 3 is interleaved
    if (snd_open(&pcm, "default", 0, 0) || snd_params(pcm, 2, 3, 2, 44100, 1, 100000)) return 0;
    return !pthread_create(&t, NULL, run, NULL);
}
#endif
