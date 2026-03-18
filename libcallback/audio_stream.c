#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/time.h>

extern int local_sdk_speaker_clean_buf_data();
extern int local_sdk_speaker_set_volume(int volume);
extern int local_sdk_speaker_feed_pcm_data(unsigned char *buf, int size);
extern int local_sdk_speaker_set_ap_mode(int mode);
extern int local_sdk_speaker_set_pa_mode(int mode);
extern void CommandResponse(int fd, const char *res);

// Buffer and timing constants
#define BUF_LENGTH          640       // PCM buffer: 320 samples * 2 bytes = 40ms @ 8kHz
#define ALAW_BUF_LEN        320       // a-law buffer: half of PCM buffer
#define DRAIN_THRESHOLD_MS  15.0      // read interval threshold for real-time detection
#define FEED_RETRY_US       (10*1000) // 10ms retry interval for speaker feed
#define FIFO_REOPEN_US      (1000*1000) // 1s wait before FIFO reopen on error
#define SOURCE_CLOSE_WAIT_US (5000*1000) // 5s wait after source closes
#define SPEAKER_MODE_ACTIVE 3
#define SPEAKER_MODE_OFF    0
#define DEFAULT_VOLUME      40
#define MAX_VOLUME          100
#define MAX_PATH_LEN        256

static int (*set_pa_mode)(int mode);
static pthread_t streamThread;
static volatile int streamRunning = 0;
static int streamFd = -1;
static char streamPath[MAX_PATH_LEN];
static volatile int streamVolume = DEFAULT_VOLUME;
static volatile int streamAlaw = 0;  // 1 = decode a-law (PCMA) to s16le

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

// Millisecond timer for stale data detection
static double getTimeMs(void) {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return tv.tv_sec * 1000.0 + tv.tv_usec / 1000.0;
}

static void *AudioStreamThread(void *arg) {

  unsigned char buf[BUF_LENGTH];

  printf("[astream] start: %s vol=%d alaw=%d\n", streamPath, streamVolume, streamAlaw);

  while(streamRunning) {
    // O_RDONLY blocks until a writer opens the FIFO.
    // Use a FIFO keeper (sleep <> fifo) to avoid this block.
    int fd = open(streamPath, O_RDONLY);
    if(fd < 0) {
      fprintf(stderr, "[astream] open %s failed: %s\n", streamPath, strerror(errno));
      usleep(FIFO_REOPEN_US);
      continue;
    }

    // Flush stale data accumulated in FIFO before we started
    {
      int flags = fcntl(fd, F_GETFL, 0);
      fcntl(fd, F_SETFL, flags | O_NONBLOCK);
      while(read(fd, buf, BUF_LENGTH) > 0) {}
      fcntl(fd, F_SETFL, flags);
    }

    local_sdk_speaker_clean_buf_data();
    local_sdk_speaker_set_volume(streamVolume);
    set_pa_mode(SPEAKER_MODE_ACTIVE);

    // Stale data recovery: when feed_pcm_data needs retries, the speaker
    // buffer is congested with stale data. Flush speaker + FIFO, then
    // discard reads until read interval stabilizes (>15ms = real-time).
    int firstRead = 1;
    int skippedBytes = 0;
    int skippedReads = 0;
    int firstFeed = 1;
    int draining = 0;       // 1 = discarding stale data after flush
    double lastReadTime = 0;

    while(streamRunning) {
      if(streamAlaw) {
        // a-law: read half the buffer (1 byte alaw -> 2 bytes PCM)
        unsigned char alawBuf[ALAW_BUF_LEN];
        ssize_t size = read(fd, alawBuf, ALAW_BUF_LEN);
        double t1 = getTimeMs();
        if(size <= 0) {
          if(size < 0 && errno == EINTR) continue;
          break;
        }
        if(firstRead) firstRead = 0;
        // While draining, discard data until read interval > 15ms (real-time)
        if(draining) {
          skippedBytes += size;
          skippedReads++;
          if(lastReadTime > 0 && (t1 - lastReadTime) > DRAIN_THRESHOLD_MS) {
            draining = 0;
            local_sdk_speaker_clean_buf_data();
            printf("[astream] drain done: skipped_bytes=%d skipped_reads=%d\n",
                   skippedBytes, skippedReads);
            skippedBytes = 0;
            skippedReads = 0;
          }
          lastReadTime = t1;
          continue;
        }
        short *pcm = (short *)buf;
        for(int i = 0; i < size; i++) {
          pcm[i] = alaw_decode(alawBuf[i]);
        }
        int pcmSize = size * 2;
        int retries = 0;
        while(streamRunning && local_sdk_speaker_feed_pcm_data(buf, pcmSize)) {
          usleep(FEED_RETRY_US);
          retries++;
        }

        // If feed needed retries, speaker buffer is congested.
        // Flush everything and enter drain mode.
        if(retries > 0 && !firstFeed) {
          local_sdk_speaker_clean_buf_data();
          int flags = fcntl(fd, F_GETFL, 0);
          fcntl(fd, F_SETFL, flags | O_NONBLOCK);
          int flushed = 0;
          unsigned char tmpBuf[BUF_LENGTH];
          while(read(fd, tmpBuf, BUF_LENGTH) > 0) flushed++;
          fcntl(fd, F_SETFL, flags);
          draining = 1;
          lastReadTime = 0;
          printf("[astream] stale detected: retries=%d flushed=%d\n", retries, flushed);
          continue;
        }

        if(firstFeed) firstFeed = 0;
      } else {
        ssize_t size = read(fd, buf, BUF_LENGTH);
        double t1 = getTimeMs();
        if(size <= 0) {
          if(size < 0 && errno == EINTR) continue;
          break;
        }
        if(firstRead) firstRead = 0;
        if(draining) {
          skippedBytes += size;
          skippedReads++;
          if(lastReadTime > 0 && (t1 - lastReadTime) > DRAIN_THRESHOLD_MS) {
            draining = 0;
            local_sdk_speaker_clean_buf_data();
            printf("[astream] drain done: skipped_bytes=%d skipped_reads=%d\n",
                   skippedBytes, skippedReads);
            skippedBytes = 0;
            skippedReads = 0;
          }
          lastReadTime = t1;
          continue;
        }
        int retries = 0;
        while(streamRunning && local_sdk_speaker_feed_pcm_data(buf, size)) {
          usleep(FEED_RETRY_US);
          retries++;
        }

        if(retries > 0 && !firstFeed) {
          local_sdk_speaker_clean_buf_data();
          int flags = fcntl(fd, F_GETFL, 0);
          fcntl(fd, F_SETFL, flags | O_NONBLOCK);
          int flushed = 0;
          unsigned char tmpBuf[BUF_LENGTH];
          while(read(fd, tmpBuf, BUF_LENGTH) > 0) flushed++;
          fcntl(fd, F_SETFL, flags);
          draining = 1;
          lastReadTime = 0;
          printf("[astream] stale detected: retries=%d flushed=%d\n", retries, flushed);
          continue;
        }

        if(firstFeed) firstFeed = 0;
      }
    }

    close(fd);
    set_pa_mode(SPEAKER_MODE_OFF);

    if(streamRunning) {
      printf("[astream] source closed, waiting for writer...\n");
      usleep(SOURCE_CLOSE_WAIT_US);
    }
  }

  set_pa_mode(SPEAKER_MODE_OFF);
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

  strncpy(streamPath, p, MAX_PATH_LEN - 1);
  streamPath[MAX_PATH_LEN - 1] = '\0';

  p = strtok_r(NULL, " \t\r\n", &tokenPtr);
  streamVolume = DEFAULT_VOLUME;
  streamAlaw = 0;
  if(p) {
    streamVolume = atoi(p);
    if(streamVolume < 0) streamVolume = 0;
    if(streamVolume > MAX_VOLUME) streamVolume = MAX_VOLUME;
  }

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
