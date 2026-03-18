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

// Latency measurement helpers
static double getTimeMs(void) {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return tv.tv_sec * 1000.0 + tv.tv_usec / 1000.0;
}

static void *AudioStreamThread(void *arg) {

  static const int bufLength = 640;
  unsigned char buf[bufLength];

  // Latency measurement state
  int logCount = 0;
  double readTotal = 0, feedTotal = 0, feedRetryTotal = 0;
  int feedRetryCount = 0;
  const int LOG_INTERVAL = 100; // log every 100 reads

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

    // Flush stale data accumulated in FIFO before we started
    {
      int flags = fcntl(fd, F_GETFL, 0);
      fcntl(fd, F_SETFL, flags | O_NONBLOCK);
      while(read(fd, buf, bufLength) > 0) {}
      fcntl(fd, F_SETFL, flags);
    }

    double openTime = getTimeMs();

    local_sdk_speaker_clean_buf_data();
    local_sdk_speaker_set_volume(streamVolume);
    set_pa_mode(3);

    // Stale data recovery: when feed_pcm_data needs retries, the speaker
    // buffer is congested with stale data. Flush speaker + FIFO, then
    // discard reads until read interval stabilizes (>15ms = real-time).
    double startTime = getTimeMs();
    int firstRead = 1;
    int skippedBytes = 0;
    int skippedReads = 0;
    int firstFeed = 1;
    int draining = 0;       // 1 = discarding stale data after flush
    double lastReadTime = 0;

    logCount = 0;
    readTotal = feedTotal = feedRetryTotal = 0;
    feedRetryCount = 0;

    while(streamRunning) {
      double t0 = getTimeMs();

      if(streamAlaw) {
        // a-law: read half the buffer (1 byte alaw -> 2 bytes PCM)
        unsigned char alawBuf[bufLength / 2];
        ssize_t size = read(fd, alawBuf, bufLength / 2);
        double t1 = getTimeMs();
        if(size <= 0) {
          if(size < 0 && errno == EINTR) continue;
          break;
        }
        if(firstRead) {
          firstRead = 0;
          FILE *mfp = fopen("/tmp/astream_measure.log", "a");
          if(mfp) {
            fprintf(mfp, "[measure] open=%.0f first_read=%.0f wait_ms=%.0f\n",
                    openTime, t1, t1 - openTime);
            fclose(mfp);
          }
        }
        // While draining, discard data until read interval > 15ms (real-time)
        if(draining) {
          skippedBytes += size;
          skippedReads++;
          if(lastReadTime > 0 && (t1 - lastReadTime) > 15.0) {
            draining = 0;
            local_sdk_speaker_clean_buf_data();
            FILE *mfp = fopen("/tmp/astream_measure.log", "a");
            if(mfp) {
              fprintf(mfp, "[measure] drain_done: skipped_bytes=%d skipped_reads=%d\n",
                      skippedBytes, skippedReads);
              fclose(mfp);
            }
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
          usleep(10 * 1000);
          retries++;
        }
        double t2 = getTimeMs();

        // If feed needed retries, speaker buffer is congested.
        // Flush everything and enter drain mode.
        if(retries > 0 && !firstFeed) {
          local_sdk_speaker_clean_buf_data();
          int flags = fcntl(fd, F_GETFL, 0);
          fcntl(fd, F_SETFL, flags | O_NONBLOCK);
          int flushed = 0;
          unsigned char tmpBuf[bufLength];
          while(read(fd, tmpBuf, bufLength) > 0) flushed++;
          fcntl(fd, F_SETFL, flags);
          draining = 1;
          lastReadTime = 0;
          FILE *mfp = fopen("/tmp/astream_measure.log", "a");
          if(mfp) {
            fprintf(mfp, "[measure] stale_detected: retries=%d flushed_reads=%d at=%.0f\n",
                    retries, flushed, t2);
            fclose(mfp);
          }
          continue;
        }

        if(firstFeed) {
          firstFeed = 0;
          FILE *mfp = fopen("/tmp/astream_measure.log", "a");
          if(mfp) {
            fprintf(mfp, "[measure] first_feed=%.0f feed_ms=%.0f total_ms=%.0f retries=%d\n",
                    t2, t2 - t1, t2 - startTime, retries);
            fclose(mfp);
          }
        }

        readTotal += (t1 - t0);
        feedTotal += (t2 - t1);
        if(retries > 0) {
          feedRetryCount++;
          feedRetryTotal += retries * 10.0;
        }
      } else {
        ssize_t size = read(fd, buf, bufLength);
        double t1 = getTimeMs();
        if(size <= 0) {
          if(size < 0 && errno == EINTR) continue;
          break;
        }
        if(firstRead) {
          firstRead = 0;
          FILE *mfp = fopen("/tmp/astream_measure.log", "a");
          if(mfp) {
            fprintf(mfp, "[measure] open=%.0f first_read=%.0f wait_ms=%.0f\n",
                    openTime, t1, t1 - openTime);
            fclose(mfp);
          }
        }
        if(draining) {
          skippedBytes += size;
          skippedReads++;
          if(lastReadTime > 0 && (t1 - lastReadTime) > 15.0) {
            draining = 0;
            local_sdk_speaker_clean_buf_data();
            FILE *mfp = fopen("/tmp/astream_measure.log", "a");
            if(mfp) {
              fprintf(mfp, "[measure] drain_done: skipped_bytes=%d skipped_reads=%d\n",
                      skippedBytes, skippedReads);
              fclose(mfp);
            }
            skippedBytes = 0;
            skippedReads = 0;
          }
          lastReadTime = t1;
          continue;
        }
        int retries = 0;
        while(streamRunning && local_sdk_speaker_feed_pcm_data(buf, size)) {
          usleep(10 * 1000);
          retries++;
        }
        double t2 = getTimeMs();

        if(retries > 0 && !firstFeed) {
          local_sdk_speaker_clean_buf_data();
          int flags = fcntl(fd, F_GETFL, 0);
          fcntl(fd, F_SETFL, flags | O_NONBLOCK);
          int flushed = 0;
          unsigned char tmpBuf[bufLength];
          while(read(fd, tmpBuf, bufLength) > 0) flushed++;
          fcntl(fd, F_SETFL, flags);
          draining = 1;
          lastReadTime = 0;
          FILE *mfp = fopen("/tmp/astream_measure.log", "a");
          if(mfp) {
            fprintf(mfp, "[measure] stale_detected: retries=%d flushed_reads=%d at=%.0f\n",
                    retries, flushed, t2);
            fclose(mfp);
          }
          continue;
        }

        if(firstFeed) {
          firstFeed = 0;
          FILE *mfp = fopen("/tmp/astream_measure.log", "a");
          if(mfp) {
            fprintf(mfp, "[measure] first_feed=%.0f feed_ms=%.0f total_ms=%.0f retries=%d\n",
                    t2, t2 - t1, t2 - startTime, retries);
            fclose(mfp);
          }
        }

        readTotal += (t1 - t0);
        feedTotal += (t2 - t1);
        if(retries > 0) {
          feedRetryCount++;
          feedRetryTotal += retries * 10.0;
        }
      }

      logCount++;
      if(logCount >= LOG_INTERVAL) {
        FILE *logfp = fopen("/tmp/astream_latency.log", "a");
        if(logfp) {
          fprintf(logfp, "[astream] latency: read_avg=%.1fms feed_avg=%.1fms feed_retry=%d/%d (%.0fms total)\n",
                  readTotal / logCount, feedTotal / logCount,
                  feedRetryCount, logCount, feedRetryTotal);
          fclose(logfp);
        }
        logCount = 0;
        readTotal = feedTotal = feedRetryTotal = 0;
        feedRetryCount = 0;
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
