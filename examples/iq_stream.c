#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <math.h>
#include <signal.h>
#include <rtl-sdr.h>

static volatile sig_atomic_t running = 1;

static void stop_handler(int sig)
{
    (void)sig;
    running = 0;
}

int main(void)
{
    uint32_t count = rtlsdr_get_device_count();
    printf("RTL-SDR devices found: %u\n", count);

    if (count == 0) {
        fprintf(stderr, "No RTL-SDR device found.\n");
        return 1;
    }

    printf("Device 0: %s\n", rtlsdr_get_device_name(0));

    rtlsdr_dev_t *dev = NULL;
    int r = rtlsdr_open(&dev, 0);
    if (r < 0 || dev == NULL) {
        fprintf(stderr, "Cannot open RTL-SDR. Error: %d\n", r);
        return 1;
    }

    const uint32_t frequency = 7030000;
    const uint32_t sample_rate = 2048000;

    if (rtlsdr_set_sample_rate(dev, sample_rate) < 0 ||
        rtlsdr_set_center_freq(dev, frequency) < 0) {
        fprintf(stderr, "Could not configure RTL-SDR.\n");
        rtlsdr_close(dev);
        return 1;
    }

    rtlsdr_set_tuner_gain_mode(dev, 0);
    rtlsdr_reset_buffer(dev);

    printf("Frequency:   %.6f MHz\n", rtlsdr_get_center_freq(dev) / 1e6);
    printf("Sample rate: %.3f MS/s\n", rtlsdr_get_sample_rate(dev) / 1e6);

    signal(SIGINT, stop_handler);
    signal(SIGTERM, stop_handler);

    const int buffer_size = 32768;
    uint8_t *buffer = malloc((size_t)buffer_size);
    if (!buffer) {
        rtlsdr_close(dev);
        return 1;
    }

    printf("Receiving I/Q... Ctrl-C to stop.\n");

    while (running) {
        int bytes_read = 0;
        r = rtlsdr_read_sync(dev, buffer, buffer_size, &bytes_read);
        if (r < 0) {
            fprintf(stderr, "\nRead error: %d\n", r);
            break;
        }

        if (bytes_read < 2)
            continue;

        double power = 0.0;
        int samples = bytes_read / 2;

        for (int i = 0; i + 1 < bytes_read; i += 2) {
            double I = ((double)buffer[i]     - 127.5) / 127.5;
            double Q = ((double)buffer[i + 1] - 127.5) / 127.5;
            power += I * I + Q * Q;
        }

        power /= (double)samples;
        double dbfs = 10.0 * log10(power + 1e-20);
        printf("\rSignal level: %7.2f dBFS   Samples: %d", dbfs, samples);
        fflush(stdout);
    }

    printf("\nStopping.\n");
    free(buffer);
    rtlsdr_close(dev);
    return 0;
}
