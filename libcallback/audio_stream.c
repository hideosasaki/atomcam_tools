#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>

extern int local_sdk_speaker_clean_buf_data();
extern int local_sdk_speaker_set_volume(int volume);
extern int local_sdk_speaker_feed_pcm_data(unsigned char *buf, int size);
extern int local_sdk_speaker_set_ap_mode(int mode);
extern int local_sdk_speaker_set_pa_mode(int mode);
extern void CommandResponse(int fd, const char *res);

static int (*set_pa_mode)(int mode);
static pthread_t streamThread;
static int streamRunning = 0;
static int streamFd = -1;
static char streamPath[256];
static int streamVolume = 40;
static int streamAlaw = 0;  // 1 = decode a-law (PCMA) to s16le

// A-law to 16-bit linear PCM decode
static short alaw_decode(unsigned char alaw) {
  alaw ^= 0x55;
  int sign = alaw & 0x80;
  int exponent = (alaw >> 4) & 0x07;
  int mantissa = alaw & 0x0F;
  int magnitude;
  if(exponent == 0) {
    magnitude = (mantissa << 4) + 8;
  } else {
    magnitude = ((mantissa << 4) + 0x108) << (exponent - 1);
  }
  return sign ? -magnitude : magnitude;
}

static void *AudioStreamThread(void *arg) {

  static const int bufLength = 640;
  unsigned char buf[bufLength];

  printf("[astream] start: %s vol=%d alaw=%d\n", streamPath, streamVolume, streamAlaw);

  while(streamRunning) {
    // O_RDONLY blocks until a writer opens the FIFO.
    // Use a FIFO keeper (sleep <> fifo) to avoid this block.
    int fd = open(streamPath, O_RDONLY);
    if(fd < 0) {
      fprintf(stderr, "[astream] open %s failed: %s\n", streamPath, strerror(errno));
      usleep(1000 * 1000);
      continue;
    }

    local_sdk_speaker_clean_buf_data();
    local_sdk_speaker_set_volume(streamVolume);
    set_pa_mode(3);

    while(streamRunning) {
      if(streamAlaw) {
        // a-law: read half the buffer (1 byte alaw -> 2 bytes PCM)
        unsigned char alawBuf[bufLength / 2];
        ssize_t size = read(fd, alawBuf, bufLength / 2);
        if(size <= 0) {
          if(size < 0 && errno == EINTR) continue;
          break;
        }
        short *pcm = (short *)buf;
        for(int i = 0; i < size; i++) {
          pcm[i] = alaw_decode(alawBuf[i]);
        }
        int pcmSize = size * 2;
        while(streamRunning && local_sdk_speaker_feed_pcm_data(buf, pcmSize)) {
          usleep(10 * 1000);
        }
      } else {
        ssize_t size = read(fd, buf, bufLength);
        if(size <= 0) {
          if(size < 0 && errno == EINTR) continue;
          break;
        }
        while(streamRunning && local_sdk_speaker_feed_pcm_data(buf, size)) {
          usleep(10 * 1000);
        }
      }
    }

    close(fd);
    set_pa_mode(0);

    if(streamRunning) {
      printf("[astream] source closed, waiting for writer...\n");
      usleep(5000 * 1000);  // 5秒待機してCPU負荷を抑制
    }
  }

  set_pa_mode(0);
  printf("[astream] stopped\n");

  if(streamFd >= 0) {
    CommandResponse(streamFd, "ok");
    streamFd = -1;
  }
  return NULL;
}

char *AudioStream(int fd, char *tokenPtr) {

  if(!set_pa_mode) {
    fprintf(stderr, "[astream] err: speaker mode function not found\n");
    return "error";
  }

  char *p = strtok_r(NULL, " \t\r\n", &tokenPtr);
  if(!p) {
    if(streamRunning) return "running";
    return "stopped";
  }

  if(!strcmp(p, "stop")) {
    if(!streamRunning) return "ok";
    streamRunning = 0;
    // FIFOのブロッキングread解除のためにダミー書き込み
    int wfd = open(streamPath, O_WRONLY | O_NONBLOCK);
    if(wfd >= 0) close(wfd);
    pthread_join(streamThread, NULL);
    return "ok";
  }

  if(streamRunning) {
    fprintf(stderr, "[astream] err: already running\n");
    return "error";
  }

  strncpy(streamPath, p, 255);
  streamPath[255] = '\0';

  p = strtok_r(NULL, " \t\r\n", &tokenPtr);
  streamVolume = 40;
  streamAlaw = 0;
  if(p) streamVolume = atoi(p);

  p = strtok_r(NULL, " \t\r\n", &tokenPtr);
  if(p && !strcmp(p, "alaw")) streamAlaw = 1;

  streamRunning = 1;
  if(pthread_create(&streamThread, NULL, AudioStreamThread, NULL)) {
    fprintf(stderr, "[astream] pthread_create error\n");
    streamRunning = 0;
    return "error";
  }

  return "ok";
}

static void __attribute__((constructor)) AudioStreamInit(void) {

  set_pa_mode = local_sdk_speaker_set_ap_mode;
  if(!set_pa_mode) set_pa_mode = local_sdk_speaker_set_pa_mode;
}
