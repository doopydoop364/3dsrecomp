/* Host audio output: frames from the DSP mixer go to an SDL audio stream.
 * settings.toml: [audio] enabled = true, volume = 100 (percent). RB_WAV=file.wav also writes the output to a
 * WAV file (works headless, used for testing). Under RB_FAST the device would overflow, so queued audio beyond
 * a quarter second is dropped. */
#include "audio.h"
#include "settings.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static SDL_AudioStream *stream;
static FILE *wav;
static uint32_t wav_bytes;
static float volume = 1.0f;

static void wav_header(void) {
    uint32_t rate = AUDIO_RATE, brate = AUDIO_RATE * 4, riff = 36 + wav_bytes, fmt = 16;
    uint16_t pcm = 1, ch = 2, align = 4, bits = 16;
    fseek(wav, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, wav); fwrite(&riff, 4, 1, wav); fwrite("WAVEfmt ", 1, 8, wav); fwrite(&fmt, 4, 1, wav);
    fwrite(&pcm, 2, 1, wav); fwrite(&ch, 2, 1, wav); fwrite(&rate, 4, 1, wav); fwrite(&brate, 4, 1, wav);
    fwrite(&align, 2, 1, wav); fwrite(&bits, 2, 1, wav); fwrite("data", 1, 4, wav); fwrite(&wav_bytes, 4, 1, wav);
    fseek(wav, 0, SEEK_END);
}
static void wav_close(void) { if (wav) { wav_header(); fclose(wav); wav = 0; } }

void audio_init(void) {
    const char *w = getenv("RB_WAV");
    if (w && (wav = fopen(w, "wb"))) { wav_header(); atexit(wav_close); }
    volume = settings_int("audio.volume", 100) / 100.0f;
    if (getenv("RB_HEADLESS") || !settings_int("audio.enabled", 1)) return;
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) { fprintf(stderr, "audio: SDL audio is not available (%s), running silent\n", SDL_GetError()); return; }
    SDL_AudioSpec spec = { SDL_AUDIO_S16, 2, AUDIO_RATE };
    stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, 0, 0);
    if (!stream) { fprintf(stderr, "audio: cannot open the audio device (%s), running silent\n", SDL_GetError()); return; }
    SDL_ResumeAudioStreamDevice(stream);
}

void audio_push(const int16_t *s, int frames) {
    int16_t buf[2 * 512];
    if (frames > 512) frames = 512;
    const int16_t *out = s;
    if (volume != 1.0f) {
        for (int i = 0; i < 2 * frames; i++) { float x = s[i] * volume; buf[i] = (int16_t)(x > 32767 ? 32767 : x < -32768 ? -32768 : x); }
        out = buf;
    }
    if (wav) { fwrite(out, 4, (size_t)frames, wav); wav_bytes += 4u * (uint32_t)frames; }
    if (stream && SDL_GetAudioStreamQueued(stream) < AUDIO_RATE * 4 * 7 / 100)   /* keep at most ~70 ms queued: low latency */
        SDL_PutAudioStreamData(stream, out, frames * 4);
}
