/**
 * Copyright (c) 2020 Raspberry Pi (Trading) Ltd.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "pico/stdlib.h"
#include "pico/sem.h"
#include "pico/mutex.h"
#include "pico/time.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/adc.h"
#include "hardware/watchdog.h"
#include "ws2812.pio.h"

#include "lwipopts.h"
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"

#include "lwip/pbuf.h"
#include "lwip/udp.h"

#define FRAC_BITS 0
// 800 kbps / 24 bit = 33 1/3 kHz
#define NUM_PIXELS (60 /* length of test strip */ * 5 /* number of meters */)
// 5 meters = 300 pixles ≃ 100 Hz
#define WS2812_PIN_BASE 2

// Check the pin is compatible with the platform
#if WS2812_PIN_BASE >= NUM_BANK0_GPIOS
#error Attempting to use a pin>=32 on a platform that does not support it
#endif

#if PICO_CYW43_ARCH_POLL
#error PICO_CYW43_ARCH_POLL
#endif

// horrible temporary hack to avoid changing pattern code
static uint8_t *current_strip_out;
static bool current_strip_4color;

static inline void put_pixel(uint32_t pixel_grb) {
    *current_strip_out++ = (pixel_grb >> 16u) & 0xffu;
    *current_strip_out++ = (pixel_grb >> 8u) & 0xffu;
    *current_strip_out++ = pixel_grb & 0xffu;
    if (current_strip_4color) {
        *current_strip_out++ = 0;  // todo adjust?
    }
}

static inline uint32_t urgb_u32(uint8_t r, uint8_t g, uint8_t b) {
    return 
            ((uint32_t) (r) << 8) |
            ((uint32_t) (g) << 16) |
            (uint32_t) (b);
}

/*void pattern_snakes(uint len, uint t) {
    for (uint i = 0; i < len; ++i) {
        uint x = (i + (t >> 1)) % 64;
        if (x < 10)
            put_pixel(urgb_u32(0xff, 0, 0));
        else if (x >= 15 && x < 25)
            put_pixel(urgb_u32(0, 0xff, 0));
        else if (x >= 30 && x < 40)
            put_pixel(urgb_u32(0, 0, 0xff));
        else
            put_pixel(0);
    }
}

void pattern_random(uint len, uint t) {
    if (t % 8)
        return;
    for (uint i = 0; i < len; ++i)
        put_pixel(rand());
}

void pattern_sparkle(uint len, uint t) {
    if (t % 8)
        return;
    for (uint i = 0; i < len; ++i)
        put_pixel(rand() % 16 ? 0 : 0xffffffff);
}

void pattern_greys(uint len, uint t) {
    uint max = 100; // let's not draw too much current!
    t %= max;
    for (uint i = 0; i < len; ++i) {
        put_pixel(t * 0x10101);
        if (++t >= max) t = 0;
    }
}

void pattern_solid(uint len, uint t) {
    t = 1;
    for (uint i = 0; i < len; ++i) {
        put_pixel(t * 0x10101);
    }
}

int level = 8;

void pattern_fade(uint len, uint t) {
    uint shift = 4;

    uint max = 16; // let's not draw too much current!
    max <<= shift;

    uint slow_t = t / 32;
    slow_t = level;
    slow_t %= max;

    static int error = 0;
    slow_t += error;
    error = slow_t & ((1u << shift) - 1);
    slow_t >>= shift;
    slow_t *= 0x010101;

    for (uint i = 0; i < len; ++i) {
        put_pixel(slow_t);
    }
}

typedef void (*pattern)(uint len, uint t);
const struct {
    pattern pat;
    const char *name;
} pattern_table[] = {
        {pattern_snakes,  "Snakes!"},
        {pattern_random,  "Random data"},
        {pattern_sparkle, "Sparkles"},
        {pattern_greys,   "Greys"},
//        {pattern_solid,  "Solid!"},
//        {pattern_fade, "Fade"},
};*/

#define VALUE_PLANE_COUNT (8 + FRAC_BITS)
// we store value (8 bits + fractional bits of a single color (R/G/B/W) value) for multiple
// strips of pixels, in bit planes. bit plane N has the Nth bit of each strip of pixels.
typedef struct {
    // stored MSB first
    uint32_t planes[VALUE_PLANE_COUNT];
} value_bits_t;

// Add FRAC_BITS planes of e to s and store in d
void add_error(value_bits_t *d, const value_bits_t *s, const value_bits_t *e) {
    uint32_t carry_plane = 0;
    // add the FRAC_BITS low planes
    for (int p = VALUE_PLANE_COUNT - 1; p >= 8; p--) {
        uint32_t e_plane = e->planes[p];
        uint32_t s_plane = s->planes[p];
        d->planes[p] = (e_plane ^ s_plane) ^ carry_plane;
        carry_plane = (e_plane & s_plane) | (carry_plane & (s_plane ^ e_plane));
    }
    // then just ripple carry through the non fractional bits
    for (int p = 7; p >= 0; p--) {
        uint32_t s_plane = s->planes[p];
        d->planes[p] = s_plane ^ carry_plane;
        carry_plane &= s_plane;
    }
}

typedef struct {
    uint8_t *data;
    uint data_len;
    //uint frac_brightness; // 256 = *1.0;
} strip_t;

// takes 8 bit color values, multiply by brightness and store in bit planes
void transform_strips(strip_t **strips, uint num_strips, value_bits_t *values, uint value_length) {
    for (uint v = 0; v < value_length; v++) {
        memset(&values[v], 0, sizeof(values[v]));
        for (uint i = 0; i < num_strips; i++) {
            if (v < strips[i]->data_len) {
                // todo clamp?
                uint32_t value = strips[i]->data[v]; // * strips[i]->frac_brightness) >> 8u;
                //value = (value * frac_brightness) >> 8u;
                for (int j = 0; j < VALUE_PLANE_COUNT && value; j++, value >>= 1u) {
                    if (value & 1u) values[v].planes[VALUE_PLANE_COUNT - 1 - j] |= 1u << i;
                }
            }
        }
    }
}

void dither_values(const value_bits_t *colors, value_bits_t *state, const value_bits_t *old_state, uint value_length) {
    for (uint i = 0; i < value_length; i++) {
        add_error(state + i, colors + i, old_state + i);
    }
}

// requested colors * 4 to allow for RGBW
static value_bits_t colors[NUM_PIXELS * 4];
// double buffer the state of the pixel strip, since we update next version in parallel with DMAing out old version
static value_bits_t states[2][NUM_PIXELS * 4];

// example - strip 0 is RGB only
static uint8_t strip0_data[NUM_PIXELS * 3];
// example - strip 1 is RGBW
//static uint8_t strip1_data[NUM_PIXELS * 4];

strip_t strip0 = {
        .data = strip0_data,
        .data_len = sizeof(strip0_data),
        //.frac_brightness = 0x40,
};

/*strip_t strip1 = {
        .data = strip1_data,
        .data_len = sizeof(strip1_data),
        .frac_brightness = 0x100,
};*/

strip_t *strips[] = {
        &strip0,
        &strip0,
        &strip0,
        &strip0,
};

// bit plane content dma channel
#define DMA_CHANNEL 0
// chain channel for configuring main dma channel to output from disjoint 8 word fragments of memory
#define DMA_CB_CHANNEL 1

#define DMA_CHANNEL_MASK (1u << DMA_CHANNEL)
#define DMA_CB_CHANNEL_MASK (1u << DMA_CB_CHANNEL)
#define DMA_CHANNELS_MASK (DMA_CHANNEL_MASK | DMA_CB_CHANNEL_MASK)

// start of each value fragment (+1 for NULL terminator)
static uintptr_t fragment_start[NUM_PIXELS * 4 + 1];

// posted when it is safe to output a new set of values
static struct semaphore reset_delay_complete_sem;
// alarm handle for handling delay
alarm_id_t reset_delay_alarm_id;

int64_t reset_delay_complete(__unused alarm_id_t id, __unused void *user_data) {
    reset_delay_alarm_id = 0;
    sem_release(&reset_delay_complete_sem);
    // no repeat
    return 0;
}

void __isr dma_complete_handler() {
    if (dma_hw->ints0 & DMA_CHANNEL_MASK) {
        // clear IRQ
        dma_hw->ints0 = DMA_CHANNEL_MASK;
        // when the dma is complete we start the reset delay timer
        if (reset_delay_alarm_id) cancel_alarm(reset_delay_alarm_id);
        reset_delay_alarm_id = add_alarm_in_us(400, reset_delay_complete, NULL, true);
    }
}

void dma_init(PIO pio, uint sm) {
    dma_claim_mask(DMA_CHANNELS_MASK);

    // main DMA channel outputs 8 word fragments, and then chains back to the chain channel
    dma_channel_config channel_config = dma_channel_get_default_config(DMA_CHANNEL);
    channel_config_set_dreq(&channel_config, pio_get_dreq(pio, sm, true));
    channel_config_set_chain_to(&channel_config, DMA_CB_CHANNEL);
    channel_config_set_irq_quiet(&channel_config, true);
    dma_channel_configure(DMA_CHANNEL,
                          &channel_config,
                          &pio->txf[sm],
                          NULL, // set by chain
                          8, // 8 words for 8 bit planes
                          false);

    // chain channel sends single word pointer to start of fragment each time
    dma_channel_config chain_config = dma_channel_get_default_config(DMA_CB_CHANNEL);
    dma_channel_configure(DMA_CB_CHANNEL,
                          &chain_config,
                          &dma_channel_hw_addr(
                                  DMA_CHANNEL)->al3_read_addr_trig,  // ch DMA config (target "ring" buffer size 4) - this is (read_addr trigger)
                          NULL, // set later
                          1,
                          false);

    irq_set_exclusive_handler(DMA_IRQ_0, dma_complete_handler);
    dma_channel_set_irq0_enabled(DMA_CHANNEL, true);
    irq_set_enabled(DMA_IRQ_0, true);
}

void output_strips_dma(value_bits_t *bits, uint value_length) {
    for (uint i = 0; i < value_length; i++) {
        fragment_start[i] = (uintptr_t) bits[i].planes; // MSB first
    }
    fragment_start[value_length] = 0;
    dma_channel_hw_addr(DMA_CB_CHANNEL)->al3_read_addr_trig = (uintptr_t) fragment_start;
}

#define SAMPLE_RATE 48000
//#define EFFECT_MAX 256     // how many samples any effect will request at max
#define RX_SAMPLES  480     // transmitter sends 480 samples always
// buffer size can't be too high if we are to keep in sync with the music
#define SAMPLES_SIZE (SAMPLE_RATE/25)

#if SAMPLES_SIZE < 2 * RX_SAMPLES
#error SAMPLES_SIZE too small
#endif

// earlier versions had this as a ring buffer,
// but there's really no need to
volatile int16_t samples_flat[SAMPLES_SIZE];
volatile int samples_count = 0;
// there's no need for a mutex if we're only using one core
//auto_init_mutex(samples_mutex);
// no need to protect this with volatile and mutex,
// since only the main thread pops samples
int32_t samples_popped = 0;

static int samples_fullness(void) {
    //printf("samples_fullness\n");
    //mutex_enter_blocking(&samples_mutex);
    //printf("samples_fullness locked\n");
    int fullness = samples_count;
    //mutex_exit(&samples_mutex);
    //printf("samples_fullness done\n");
    return fullness;
}

// wait for ring buffer to fill up to n
static int samples_wait(int n) {
    //printf("samples_wait: %i\n", n);
    for (;;) {
        int fullness = samples_fullness();
        if (fullness >= n) {
            //printf("samples_wait done\n");
            return fullness;
        }
        //printf("samples_wait: sleep\n");
        sleep_ms(1);
    }
}


static void samples_push(const int16_t *samples_in, int n) {
    /*if (samples_fullness() < 1000) {
        printf("samples_push: %i / %i\n", n, samples_fullness());
    }*/
    //mutex_enter_blocking(&samples_mutex);
    //printf("samples_push locked\n");
    {
        int capacity = SAMPLES_SIZE - samples_count;
        if (n > capacity) {
            // discard excess samples
            //printf(" discard %i", n - capacity);
            n = capacity;
        }
        if (n > 0) {
            memcpy((void*)&samples_flat[samples_count], samples_in, n*sizeof(*samples_flat));
            samples_count += n;
        }
    }
    //printf("\n");
    //mutex_exit(&samples_mutex);
    //printf("samples_push done\n", n);
}

// copies n samples from the ring buffer to samples_out,
// but does not actually pop any samples
// blocking
/*static void samples_peek(int16_t *samples_out, int n) {
    samples_wait(n);

    mutex_enter_blocking(&samples_mutex);
    int samples_start_copy = samples_start;

    while (n > 0) {
        if (samples_start_copy == samples_end) {
            // no data - user probably didn't call samples_wait()
            break;
        } else {
            int capacity;
            if (samples_start_copy < samples_end) {
                capacity = samples_end  - samples_start_copy;
            } else {
                // read until the end of the buffer,
                // then wrap around and read until samples_end
                capacity = SAMPLES_SIZE - samples_start_copy;
            }

            int to_read = capacity < n ? capacity : n;

            memcpy(samples_out, &samples[samples_start_copy], to_read*sizeof(*samples));
            samples_start_copy = (samples_start_copy + to_read) % SAMPLES_SIZE;
            n -= to_read;
            samples_out += to_read;
        }
    }

    mutex_exit(&samples_mutex);
}*/

// pops n6*6 samples
// it is *6 because 1000000/48000*6 = 125 µs
static void samples_pop6(int n6) {
    static uint64_t next = 0;
    uint64_t t = time_us_64();

    if (next == 0) {
        next = t;
    } else if (t < next) {
        // sleep until we get there
        //printf("sleep %"PRIu64" - %"PRIu64" = %"PRIu64" next += %i\n", next, t, next - t, n6*125);
        sleep_us(next - t);
    } else {
        // cap behindness
        if (t - next > 1000000) {
            next = t - 1000000;
        }
        //printf("%"PRIu64" µs behind\n", t - next);
    }
    next += n6*125;

    //printf("samples_pop: %i / %i\n", n6*6, samples_fullness());
    samples_popped += n6*6;
    //sleep_us(n6*125);
    //time_us_64()
    //sleep_ms(10);

    //mutex_enter_blocking(&samples_mutex);
    //printf("samples_pop locked\n");
    int n = n6*6;
    {
        if (n > samples_count) {
            //printf("n > samples_count\n");
            n = samples_count;
        }

        if (n < samples_count) {
            // not memcpy() because of overlap
            //printf("n < samples_count %i samples\n", samples_count - n);
            memmove((void*)samples_flat, (void*)&samples_flat[n], (samples_count - n)*sizeof(*samples_flat));
        }

        samples_count -= n;
        //printf("samples_pop %i left\n", samples_count);
    }
    //mutex_exit(&samples_mutex);
    //printf("samples_pop done\n");
}

// this seems to work way better with non-fragmented packets
static void recv(void *arg, struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *addr, u16_t port) {
    //printf("recv: %p %i %i\n", p, p->len, p->tot_len);
    struct pbuf *p_in = p;
    //static int cnt = 255;
    //samples_push(p->payload, p->len / sizeof(*samples_flat));
    int packets = 0, bytes = 0;
    for (;;) {
        packets++;
        bytes += p->len;
        //printf("push %p %i %i\n", p->payload, p->len, p->tot_len);
        samples_push(p->payload, p->len / sizeof(*samples_flat));
        if (p->len == p->tot_len) {
            break;
        }
        p = p->next;
    }
    /*int16_t buf[4096];
    for (int x = 0; x < 4096; x++) {
        buf[x] = x*x;
    }
    //cnt = (cnt + 16) % 256;
    //samples_push(buf, 1024); //p->len / sizeof(*samples_flat));
    samples_count = 2048;*/
    pbuf_free(p_in);
    if (packets > 1) {
        printf("recv done: %i packets %i B\n", packets, bytes);
    }
    watchdog_update();
}

uint current = 0;

static void swap_buffers() {
    transform_strips(strips, count_of(strips), states[current], NUM_PIXELS * 4);
    //dither_values(colors, states[current], states[current ^ 1], NUM_PIXELS * 4);
    sem_acquire_blocking(&reset_delay_complete_sem);
    output_strips_dma(states[current], NUM_PIXELS * 4);

    current ^= 1;
}


static bool samples_topper_upper(repeating_timer_t *rt) {
    /*while (samples_fullness() < 1024) {
        // TODO: remove DC bias
        // TODO: use DMA
        int16_t sample = (adc_read() - (1 << 11)) * 16;
        samples_push(&sample, 1);
    }*/
    return true;
}


static int amplitude(void) {
    uint64_t amp = 0;
    for (int x = 0; x < NUM_PIXELS; x++) {
        amp += samples_flat[x] * samples_flat[x];
    }
    amp /= NUM_PIXELS;
    amp = sqrt(amp);
    int ret = amp / 1024;
    if (ret > 255) ret = 255;
    return ret;
}

static void pp2rgb(int pp, int *r, int *g, int *b) {
    pp &= 7;
    if (pp == 0) {
        pp = 7;
    }
    *r = pp & 1;
    *g = (pp >> 1) & 1;
    *b = (pp >> 2) & 1;
}

static void amplitude_general(int pp) {
    samples_wait(NUM_PIXELS);
    int amp = amplitude();
    int r, g, b;
    pp2rgb(pp, &r, &g, &b);

    for (int x = 0; x < NUM_PIXELS; x++) {
        put_pixel(urgb_u32(r * amp, g * amp, b * amp));
    }

    samples_pop6(NUM_PIXELS / 6);
}

/*static void amplitude_white(int pp) {
    amplitude_common(1, 1, 1);
}

static void amplitude_red(int pp) {
    amplitude_common(1, 0, 0);
}

static void amplitude_green(int pp) {
    amplitude_common(0, 1, 0);
}

static void amplitude_blue(int pp) {
    amplitude_common(0, 0, 1);
}

static void amplitude_yellow(int pp) {
    amplitude_common(1, 1, 0);
}

static void amplitude_cyan(int pp) {
    amplitude_common(0, 1, 1);
}

static void amplitude_purple(int pp) {
    amplitude_common(1, 0, 1);
}*/

static void pulses(int pp) {
    int amp = amplitude();
    uint64_t t = time_us_64();

    int r, g, b;
    pp2rgb(pp, &r, &g, &b);
    for (int x = 0; x < NUM_PIXELS; x++) {
        float f = 0.5+0.5*sin(2*M_PI*(2*t/1000000.f - x/30.f));
        f *= f;
        put_pixel(urgb_u32(r * amp * f, g * amp * f, b * amp * f));
    }

    samples_pop6(NUM_PIXELS / 6);
}

static void sparkles(int pp) {
    int amp = amplitude();

    int r, g, b;
    pp2rgb(pp, &r, &g, &b);
    for (int x = 0; x < NUM_PIXELS; x++) {
        int f = rand() % 100;
        if (f <= 1) {
            f = 1;
        } else {
            f = 0;
        }
        put_pixel(urgb_u32(r * amp * f, g * amp * f, b * amp * f));
    }

    samples_pop6(NUM_PIXELS / 6);
}

// TODO: waveform
// TODO: lågpassfiltrerat ljud, amplitud per sample, ut på slingorna
static void sparkles(int pp) {
    int amp = amplitude();

    int r, g, b;
    pp2rgb(pp, &r, &g, &b);
    for (int x = 0; x < NUM_PIXELS; x++) {
        int f = rand() % 100;
        if (f <= 1) {
            f = 1;
        } else {
            f = 0;
        }
        put_pixel(urgb_u32(r * amp * f, g * amp * f, b * amp * f));
    }

    samples_pop6(NUM_PIXELS / 6);
}


static const struct {
    void (*fn)(int);
} patterns[] = {
    /*{amplitude_white},
    {amplitude_red},
    {amplitude_green},
    {amplitude_blue},
    {amplitude_yellow},
    {amplitude_cyan},
    {amplitude_purple},*/

    {amplitude_general},
    {pulses},
    {sparkles},
};

int main() {
    //set_sys_clock_48();
    stdio_init_all();
    if (watchdog_enable_caused_reboot()) {
        printf("Rebooted by watchdog\n");
    }
    watchdog_enable(15000, 1);
    printf("WS2812 parallel using pin %d\n", WS2812_PIN_BASE);

    PIO pio;
    uint sm;
    uint offset;

    // This will find a free pio and state machine for our program and load it for us
    // We use pio_claim_free_sm_and_add_program_for_gpio_range (for_gpio_range variant)
    // so we will get a PIO instance suitable for addressing gpios >= 32 if needed and supported by the hardware
    bool success = pio_claim_free_sm_and_add_program_for_gpio_range(&ws2812_parallel_program, &pio, &sm, &offset, WS2812_PIN_BASE, count_of(strips), true);
    hard_assert(success);

    ws2812_parallel_program_init(pio, sm, offset, WS2812_PIN_BASE, count_of(strips), 800000);

    sem_init(&reset_delay_complete_sem, 1, 1); // initially posted so we don't block first time
    dma_init(pio, sm);


    if (cyw43_arch_init()) {
        printf("failed to initialise\n");
        return 1;
    }

    cyw43_arch_enable_sta_mode();

    printf("Connecting to Wi-Fi...\n");

    static const struct {
        const char *name, *pass;
        int auth;
    } wifis[] = {
        {"hackerrave",          "hackerrave",       CYW43_AUTH_WPA2_AES_PSK},
    };

    // red
    for (int x = 0; x < count_of(strip0_data)/3; x++) {
        // grb
        strip0_data[3*x+0] = 0x00;
        strip0_data[3*x+1] = 0x10;
        strip0_data[3*x+2] = 0x00;
    }
    swap_buffers();
    
    for (int x = 0;; x = (x + 1) % count_of(wifis)) {
        watchdog_update();
        printf("connecting to %s\n", wifis[x].name);
        if (cyw43_arch_wifi_connect_timeout_ms(wifis[x].name, wifis[x].pass, wifis[x].auth, 10000)) {
            for (int y = 0; y <= x; y++) {
                // blue
                for (int x = 0; x < count_of(strip0_data)/3; x++) {
                    // grb
                    strip0_data[3*x+0] = 0x00;
                    strip0_data[3*x+1] = 0x00;
                    strip0_data[3*x+2] = 0x10;
                }
                swap_buffers();
                sleep_us(200e3);
                // red
                for (int x = 0; x < count_of(strip0_data)/3; x++) {
                    // grb
                    strip0_data[3*x+0] = 0x00;
                    strip0_data[3*x+1] = 0x10;
                    strip0_data[3*x+2] = 0x00;
                }
                swap_buffers();
                sleep_us(200e3);
            }
            printf("failed to connect to %s. Trying another\n", wifis[x].name);
        } else {
            printf("Connected to %s\n", wifis[x].name);
            break;
        }
    }

    bool linkup = netif_is_link_up(netif_default);
    bool anyip = ip4_addr_isany_val(*netif_ip4_addr(netif_default));
    char ip[32];
    ip4addr_ntoa_r (&cyw43_state.netif[0].ip_addr, ip, sizeof(ip));
    printf("linkup: %i anyip: %i IP: %s\nnewline test\n", (int)linkup, (int)anyip, ip);

    // green
    for (int x = 0; x < count_of(strip0_data)/3; x++) {
        // grb
        strip0_data[3*x+0] = 0x10;
        strip0_data[3*x+1] = 0x00;
        strip0_data[3*x+2] = 0x00;
    }
    swap_buffers();

    // TODO: free
    struct udp_pcb* pcb = udp_new();
    printf("pcb %p\n", pcb);

    printf("udp_bind: %i == %i ?\n", udp_bind(pcb, IP_ADDR_ANY, 4445), ERR_OK);
    udp_recv(pcb, recv, NULL);

    while (samples_fullness() < SAMPLES_SIZE / 2);

    // set up electret microphone, ADC etc
    adc_init();
    // Allowable GPIO numbers are 26 to 29 inclusive on RP2040 or RP2350A, 40-48 inclusive on RP2350B
    adc_gpio_init(26);
    adc_select_input(0);
    // ADC clock is 48 MHz
    adc_set_clkdiv(48000000/SAMPLE_RATE - 1);

    repeating_timer_t timer;
    add_repeating_timer_us(-12500, samples_topper_upper, NULL, &timer);

    int pp = 0;
    while (1) {
        //int pat = rand() % count_of(pattern_table);
        /*int dir = (rand() >> 30) & 1 ? 1 : -1;
        if (rand() & 1) dir = 0;
        puts(pattern_table[pat].name);
        puts(dir == 1 ? "(forward)" : dir ? "(backward)" : "(still)");
        int brightness = 0;
        uint current = 0;
        for (pati = 0; pati < 1000; ++pati) {
            current_strip_out = strip0.data;
            current_strip_4color = false;
            pattern_table[pat].pat(NUM_PIXELS, t);
            current_strip_out = strip1.data;
            current_strip_4color = true;
            pattern_table[pat].pat(NUM_PIXELS, t);

            transform_strips(strips, count_of(strips), colors, NUM_PIXELS * 4, brightness);
            dither_values(colors, states[current], states[current ^ 1], NUM_PIXELS * 4);
            sem_acquire_blocking(&reset_delay_complete_sem);
            output_strips_dma(states[current], NUM_PIXELS * 4);

            current ^= 1;
            t += dir;
            brightness++;
            if (brightness == (0x20 << FRAC_BITS)) brightness = 0;
        }*/
        //for (int pat = 0; pat < count_of(patterns); pat++) {
        int pat = rand() % count_of(patterns);
        pp = rand() % 8;
            samples_popped = 0;

            // TODO: switch on beats
            while (samples_popped < 4*48000) {
                current_strip_out = strip0.data;
                current_strip_4color = false;
                patterns[pat].fn(pp);

                swap_buffers();
            }
            //pp++;
        //}
        //memset(&states, 0, sizeof(states)); // clear out errors
    }

    // This will free resources and unload our program
    pio_remove_program_and_unclaim_sm(&ws2812_parallel_program, pio, sm, offset);
}
