#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <math.h>
#include <signal.h>
#include <pthread.h>
#include <string.h>

#include <rtl-sdr.h>
#include <AudioToolbox/AudioToolbox.h>


// ============================================================
// CONFIGURATION
// ============================================================

#define CW_FREQUENCY       7030000      // RF frequency: 7.030 MHz
#define CW_PITCH           700          // audible CW tone in Hz

#define RTL_SAMPLE_RATE    2400000      // 2.4 MS/s
#define AUDIO_SAMPLE_RATE  48000        // Mac audio

#define DECIMATION         (RTL_SAMPLE_RATE / AUDIO_SAMPLE_RATE)

#define RTL_BUFFER_SIZE    65536

#define AUDIO_QUEUE_FRAMES 1024
#define AUDIO_QUEUE_COUNT  3

#define RING_SIZE          (AUDIO_SAMPLE_RATE * 4)


// ============================================================
// GLOBALS
// ============================================================

static volatile sig_atomic_t running = 1;


// Audio ring buffer
static float audioRing[RING_SIZE];

static size_t ringWrite = 0;
static size_t ringRead  = 0;
static size_t ringCount = 0;

static pthread_mutex_t ringMutex = PTHREAD_MUTEX_INITIALIZER;


// ============================================================
// CTRL-C
// ============================================================

static void stopHandler(int sig)
{
    (void)sig;
    running = 0;
}


// ============================================================
// SIMPLE AUDIO RING BUFFER
// ============================================================

static void ringPush(float sample)
{
    pthread_mutex_lock(&ringMutex);

    if (ringCount < RING_SIZE) {

        audioRing[ringWrite] = sample;

        ringWrite++;

        if (ringWrite >= RING_SIZE)
            ringWrite = 0;

        ringCount++;

    } else {

        // Drop oldest sample if buffer is full

        audioRing[ringWrite] = sample;

        ringWrite++;

        if (ringWrite >= RING_SIZE)
            ringWrite = 0;

        ringRead = ringWrite;
    }

    pthread_mutex_unlock(&ringMutex);
}


static float ringPop(void)
{
    float sample = 0.0f;

    pthread_mutex_lock(&ringMutex);

    if (ringCount > 0) {

        sample = audioRing[ringRead];

        ringRead++;

        if (ringRead >= RING_SIZE)
            ringRead = 0;

        ringCount--;
    }

    pthread_mutex_unlock(&ringMutex);

    return sample;
}


// ============================================================
// CW BANDPASS FILTER
//
// RBJ biquad band-pass
//
// Center = 700 Hz
// Bandwidth roughly 400 Hz
// ============================================================

typedef struct {

    double b0;
    double b1;
    double b2;

    double a1;
    double a2;

    double x1;
    double x2;

    double y1;
    double y2;

} Biquad;


static void biquadInitBandpass(Biquad *f,
                               double sampleRate,
                               double centerFreq,
                               double bandwidth)
{
    double Q = centerFreq / bandwidth;

    double w0 =
        2.0 * M_PI * centerFreq / sampleRate;

    double alpha =
        sin(w0) / (2.0 * Q);

    double cosw =
        cos(w0);

    double a0 =
        1.0 + alpha;

    f->b0 = alpha / a0;
    f->b1 = 0.0;
    f->b2 = -alpha / a0;

    f->a1 = (-2.0 * cosw) / a0;
    f->a2 = (1.0 - alpha) / a0;

    f->x1 = 0.0;
    f->x2 = 0.0;

    f->y1 = 0.0;
    f->y2 = 0.0;
}


static inline float biquadProcess(Biquad *f,
                                  float x)
{
    double y =
        f->b0 * x +
        f->b1 * f->x1 +
        f->b2 * f->x2 -
        f->a1 * f->y1 -
        f->a2 * f->y2;

    f->x2 = f->x1;
    f->x1 = x;

    f->y2 = f->y1;
    f->y1 = y;

    return (float)y;
}


// ============================================================
// AUDIOQUEUE CALLBACK
// ============================================================

static void audioCallback(void *userData,
                          AudioQueueRef queue,
                          AudioQueueBufferRef buffer)
{
    (void)userData;

    float *out =
        (float *)buffer->mAudioData;

    for (int i = 0;
         i < AUDIO_QUEUE_FRAMES;
         i++)
    {
        out[i] = ringPop();
    }

    buffer->mAudioDataByteSize =
        AUDIO_QUEUE_FRAMES * sizeof(float);

    AudioQueueEnqueueBuffer(queue,
                            buffer,
                            0,
                            NULL);
}


// ============================================================
// START MAC AUDIO
// ============================================================

static AudioQueueRef startAudio(void)
{
    AudioStreamBasicDescription format;

    memset(&format,
           0,
           sizeof(format));

    format.mSampleRate =
        AUDIO_SAMPLE_RATE;

    format.mFormatID =
        kAudioFormatLinearPCM;

    format.mFormatFlags =
        kAudioFormatFlagIsFloat |
        kAudioFormatFlagIsPacked;

    format.mFramesPerPacket = 1;

    format.mChannelsPerFrame = 1;

    format.mBitsPerChannel =
        sizeof(float) * 8;

    format.mBytesPerFrame =
        sizeof(float);

    format.mBytesPerPacket =
        sizeof(float);


    AudioQueueRef queue;

    OSStatus status =
        AudioQueueNewOutput(
            &format,
            audioCallback,
            NULL,
            NULL,
            NULL,
            0,
            &queue
        );

    if (status != noErr) {

        printf("AudioQueueNewOutput failed: %d\n",
               (int)status);

        return NULL;
    }


    for (int i = 0;
         i < AUDIO_QUEUE_COUNT;
         i++)
    {
        AudioQueueBufferRef buffer;

        AudioQueueAllocateBuffer(
            queue,
            AUDIO_QUEUE_FRAMES *
            sizeof(float),
            &buffer
        );

        memset(buffer->mAudioData,
               0,
               AUDIO_QUEUE_FRAMES *
               sizeof(float));

        buffer->mAudioDataByteSize =
            AUDIO_QUEUE_FRAMES *
            sizeof(float);

        AudioQueueEnqueueBuffer(
            queue,
            buffer,
            0,
            NULL
        );
    }


    status =
        AudioQueueStart(queue,
                        NULL);

    if (status != noErr) {

        printf("AudioQueueStart failed: %d\n",
               (int)status);

        AudioQueueDispose(queue,
                          true);

        return NULL;
    }


    return queue;
}


// ============================================================
// MAIN
// ============================================================

int main(int argc,
         const char *argv[])
{
    (void)argc;
    (void)argv;


    signal(SIGINT,
           stopHandler);

    signal(SIGTERM,
           stopHandler);


    // --------------------------------------------------------
    // FIND RTL-SDR
    // --------------------------------------------------------

    uint32_t count =
        rtlsdr_get_device_count();

    printf("RTL-SDR devices found: %u\n",
           count);

    if (count == 0) {

        printf("No RTL-SDR found.\n");

        return 1;
    }


    printf("Device 0: %s\n",
           rtlsdr_get_device_name(0));


    // --------------------------------------------------------
    // OPEN RTL-SDR
    // --------------------------------------------------------

    rtlsdr_dev_t *dev = NULL;

    int r =
        rtlsdr_open(&dev,
                    0);

    if (r < 0 ||
        dev == NULL)
    {
        printf("Cannot open RTL-SDR: %d\n",
               r);

        return 1;
    }


    printf("RTL-SDR opened.\n");


    // --------------------------------------------------------
    // SAMPLE RATE
    // --------------------------------------------------------

    r =
        rtlsdr_set_sample_rate(
            dev,
            RTL_SAMPLE_RATE
        );

    if (r < 0) {

        printf("Cannot set sample rate.\n");

        rtlsdr_close(dev);

        return 1;
    }


    // --------------------------------------------------------
    // TUNE 700 Hz BELOW CW SIGNAL
    //
    // Carrier therefore appears at +700 Hz baseband.
    // --------------------------------------------------------

    uint32_t tunerFrequency =
        CW_FREQUENCY -
        CW_PITCH;


    r =
        rtlsdr_set_center_freq(
            dev,
            tunerFrequency
        );

    if (r < 0) {

        printf("Cannot tune RTL-SDR.\n");

        rtlsdr_close(dev);

        return 1;
    }


    // Automatic tuner gain

    rtlsdr_set_tuner_gain_mode(
        dev,
        0
    );


    rtlsdr_reset_buffer(dev);


    printf("\nCW receiver\n");
    printf("RF frequency : %.6f MHz\n",
           CW_FREQUENCY / 1000000.0);

    printf("RTL tune     : %.6f MHz\n",
           tunerFrequency / 1000000.0);

    printf("CW pitch     : %d Hz\n",
           CW_PITCH);

    printf("RTL rate     : %.3f MS/s\n",
           RTL_SAMPLE_RATE / 1000000.0);

    printf("Audio rate   : %d Hz\n",
           AUDIO_SAMPLE_RATE);

    printf("Decimation   : %d\n\n",
           DECIMATION);


    // --------------------------------------------------------
    // AUDIO
    // --------------------------------------------------------

    AudioQueueRef audioQueue =
        startAudio();

    if (audioQueue == NULL) {

        rtlsdr_close(dev);

        return 1;
    }


    // --------------------------------------------------------
    // CW FILTER
    // --------------------------------------------------------

    Biquad cwFilter;

    biquadInitBandpass(
        &cwFilter,
        AUDIO_SAMPLE_RATE,
        CW_PITCH,
        400.0
    );


    // --------------------------------------------------------
    // RTL BUFFER
    // --------------------------------------------------------

    uint8_t *buffer =
        malloc(RTL_BUFFER_SIZE);

    if (!buffer) {

        printf("Cannot allocate RTL buffer.\n");

        AudioQueueStop(audioQueue,
                       true);

        AudioQueueDispose(audioQueue,
                          true);

        rtlsdr_close(dev);

        return 1;
    }


    printf("Receiving CW...\n");
    printf("Ctrl-C to stop.\n\n");


    // --------------------------------------------------------
    // DECIMATOR
    // --------------------------------------------------------

    double accumulator = 0.0;

    int decimationCounter = 0;


    // --------------------------------------------------------
    // RECEIVE LOOP
    // --------------------------------------------------------

    while (running) {

        int bytesRead = 0;

        r =
            rtlsdr_read_sync(
                dev,
                buffer,
                RTL_BUFFER_SIZE,
                &bytesRead
            );


        if (r < 0) {

            printf("\nRTL read error: %d\n",
                   r);

            break;
        }


        for (int n = 0;
             n + 1 < bytesRead;
             n += 2)
        {
            /*
             RTL samples:

                 I Q I Q ...

             We only need I here because
             the CW carrier has deliberately
             been tuned to ±700 Hz.
            */

            double I =
                ((double)buffer[n] -
                 127.5) /
                127.5;


            // ------------------------------------------------
            // Simple integrate-and-dump decimator
            // ------------------------------------------------

            accumulator += I;

            decimationCounter++;


            if (decimationCounter ==
                DECIMATION)
            {
                float audioSample =
                    (float)(
                        accumulator /
                        DECIMATION
                    );


                accumulator = 0.0;

                decimationCounter = 0;


                // --------------------------------------------
                // CW audio filter
                // --------------------------------------------

                audioSample =
                    biquadProcess(
                        &cwFilter,
                        audioSample
                    );


                // modest output gain

                audioSample *= 4.0f;


                // clip

                if (audioSample > 1.0f)
                    audioSample = 1.0f;

                if (audioSample < -1.0f)
                    audioSample = -1.0f;


                ringPush(audioSample);
            }
        }
    }


    // ========================================================
    // CLEANUP
    // ========================================================

    printf("\nStopping...\n");


    AudioQueueStop(
        audioQueue,
        true
    );

    AudioQueueDispose(
        audioQueue,
        true
    );


    free(buffer);

    rtlsdr_close(dev);


    printf("Done.\n");

    return 0;
}
