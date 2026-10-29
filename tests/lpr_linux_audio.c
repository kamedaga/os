#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/soundcard.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <unistd.h>

/* Run against a real QEMU sound device. Checks the descriptor/PCM boundary
 * before OpenAL, including shared flags and last-close across fork. */
int main(void)
{
    struct stat metadata;
    assert(!stat("/dev/dsp", &metadata) && S_ISCHR(metadata.st_mode));
    assert(major(metadata.st_rdev) == 14 && minor(metadata.st_rdev) == 3);
    errno = 0;
    assert(open("/dev/dsp", O_RDONLY) < 0 && errno == EACCES);
    assert(!symlink("/dev/dsp", "/tmp/pcm-alias"));
    int fd = open("/tmp/pcm-alias", O_WRONLY | O_NONBLOCK);
    assert(!unlink("/tmp/pcm-alias"));
    if (fd < 0) { perror("open /dev/dsp"); return 1; }
    struct stat stat;
    assert(!fstat(fd, &stat) && S_ISCHR(stat.st_mode));
    assert(major(stat.st_rdev) == 14 && minor(stat.st_rdev) == 3);
    int fragment = (4 << 16) | 11, format = AFMT_S16_LE, channels = 2, rate = 48000;
    if (ioctl(fd, SNDCTL_DSP_SETFRAGMENT, &fragment)) { perror("SETFRAGMENT"); return 2; }
    assert(!ioctl(fd, SNDCTL_DSP_SETFMT, &format) && format == AFMT_S16_LE);
    assert(!ioctl(fd, SNDCTL_DSP_CHANNELS, &channels) && channels == 2);
    assert(!ioctl(fd, SNDCTL_DSP_SPEED, &rate) && rate == 48000);
    audio_buf_info space;
    assert(!ioctl(fd, SNDCTL_DSP_GETOSPACE, &space));
    assert(space.fragsize > 0 && space.fragstotal > 0);
    int other = dup(fd); assert(other >= 0); assert(!close(fd));
    assert(fcntl(other, F_GETFL) & O_NONBLOCK);
    assert(!fcntl(other, F_SETFL, 0));
    assert(!(fcntl(other, F_GETFL) & O_NONBLOCK));
    pid_t child = fork(); assert(child >= 0);
    if (!child) {
        if (fcntl(other, F_SETFL, O_NONBLOCK) < 0) _exit(2);
        _exit(0);
    }
    int status;
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status));
    assert(fcntl(other, F_GETFL) & O_NONBLOCK);
    assert(!fcntl(other, F_SETFL, 0));
    int unsupported = 0;
    errno = 0;
    assert(ioctl(other, _IOW('X', 0x42, int), &unsupported) == -1 && errno == ENOTTY);
    int16_t samples[4800 * 2];
    for (unsigned i = 0; i < 4800; ++i) samples[i*2] = samples[i*2+1] = i % 48 < 24 ? 1000 : -1000;
    for (size_t done = 0; done < sizeof(samples);) {
        struct pollfd pollfd = {.fd = other, .events = POLLOUT};
        int ready = poll(&pollfd, 1, 1000);
        if (ready != 1 || !(pollfd.revents & POLLOUT)) {
            int delay = -1; count_info pointer = {0};
            int space_result = ioctl(other, SNDCTL_DSP_GETOSPACE, &space);
            int pointer_result = ioctl(other, SNDCTL_DSP_GETOPTR, &pointer);
            int delay_result = ioctl(other, SNDCTL_DSP_GETODELAY, &delay);
            fprintf(stderr, "PCM poll ready=%d revents=%x errno=%d done=%zu space=%d/%d ptr=%d/%d delay=%d/%d\n", ready, pollfd.revents, errno, done, space_result, space.bytes, pointer_result, pointer.bytes, delay_result, delay);
            return 3;
        }
        ssize_t count = write(other, (char *)samples + done, sizeof(samples) - done);
        assert(count > 0); done += (size_t)count;
    }
    assert(!ioctl(other, SNDCTL_DSP_SYNC));
    assert(!close(other));
    for (unsigned i = 0; i < 8; ++i) {
        fd = open("/dev/dsp", O_WRONLY); assert(fd >= 0); assert(!close(fd));
    }
    puts("PCM_DEVICE=PASS stereo48000 poll write sync dup fork shared-flags close/reopen=8 ENOTTY");
    return 0;
}
