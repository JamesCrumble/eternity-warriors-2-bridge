/* Plays the guest's FMOD output: packets {magic 'EW3A', freq, bytes} + S16LE stereo PCM read from a fifo. */
#define _GNU_SOURCE
#include <SDL.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static int read_all(int fd, void *buf, size_t n)
{
    uint8_t *p = buf;
    while (n) {
        ssize_t r = read(fd, p, n);
        if (r <= 0)
            return -1;
        p += r;
        n -= (size_t)r;
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2)
        return 2;
    int fd = open(argv[1], O_RDWR);   /* RDWR: no blocking on open, no EOF between writers */
    if (fd >= 0)
        fcntl(fd, F_SETPIPE_SZ, 4096);   /* the default 64 KiB pipe alone holds ~0.7 s of audio */
    if (fd < 0 || SDL_Init(SDL_INIT_AUDIO) != 0)
        return 1;
    SDL_AudioDeviceID dev = 0;
    int cur = 0;
    static uint8_t pcm[65536];
    for (;;) {
        uint32_t h[3];
        if (read_all(fd, h, 12) || h[0] != 0x41335745u || h[2] > sizeof pcm || read_all(fd, pcm, h[2]))
            break;
        if ((int)h[1] != cur && h[1] > 0) {
            if (dev)
                SDL_CloseAudioDevice(dev);
            SDL_AudioSpec want = {0};
            want.freq = (int)h[1];
            want.format = AUDIO_S16LSB;
            want.channels = 2;
            want.samples = 512;
            dev = SDL_OpenAudioDevice(NULL, 0, &want, NULL, 0);
            if (dev) {
                SDL_PauseAudioDevice(dev, 0);
                cur = (int)h[1];
                fprintf(stderr, "[audio] opened %d Hz\n", cur);
            } else {
                fprintf(stderr, "[audio] open failed: %s\n", SDL_GetError());
            }
        }
        if (dev) {
            /* Keep at most ~4 blocks queued: when the guest runs ahead, drop the backlog instead of lagging. */
            if (SDL_GetQueuedAudioSize(dev) > 4 * h[2])
                SDL_ClearQueuedAudio(dev);
            SDL_QueueAudio(dev, pcm, h[2]);
        }
    }
    return 0;
}
