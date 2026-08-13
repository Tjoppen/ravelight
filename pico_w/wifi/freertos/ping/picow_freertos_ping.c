/**
 * Copyright (c) 2022 Raspberry Pi (Trading) Ltd.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <math.h>

#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"
#include "hardware/adc.h"

#include "lwip/ip4_addr.h"

#include "FreeRTOS.h"
#include "task.h"
//#include "ping.h"
#include "dhcpserver.h"
#include "semphr.h"

/*#ifndef PING_ADDR
#define PING_ADDR "142.251.35.196"
#endif*/
#ifndef RUN_FREERTOS_ON_CORE
#define RUN_FREERTOS_ON_CORE 0
#endif

#if configNUMBER_OF_CORES != 2
#error need 2 cores
#endif

#define TEST_TASK_PRIORITY ( tskIDLE_PRIORITY + 1UL )
#define ADC_TASK_PRIORITY ( tskIDLE_PRIORITY + 2UL )

#define LED_GPIO 0
#define UDP_PORT 4445
#define SAMPLE_RATE 48000
#define NUM_SAMPLES 480
// generate idle signal if audio has been quiet for this long
#define QUIET_TIME (configTICK_RATE_HZ * 4)

static int16_t sample_buffers[2][NUM_SAMPLES];
static uint64_t sample_square_sum[2];
static int sample_last_buffer = 0;
static SemaphoreHandle_t sample_semaphore;

void adc_task(__unused void *params) {
    printf("started adc_task\n");
    adc_init();
    // Allowable GPIO numbers are 26 to 29 inclusive on RP2040 or RP2350A, 40-48 inclusive on RP2350B
    adc_gpio_init(26);
    adc_select_input(0);
    // ADC clock is 48 MHz
    adc_set_clkdiv(48000000/SAMPLE_RATE - 1);

    printf("adc_task enter loop\n");
    for (;;) {
        for (int buf = 0; buf < 2; buf++) {
            //printf("adc_task buf %i\n", buf);
            uint64_t sum = 0;
            for (int x = 0; x < NUM_SAMPLES; x++) {
                hw_set_bits(&adc_hw->cs, ADC_CS_START_ONCE_BITS);

                // yield
                vTaskDelay(1);

                while (!(adc_hw->cs & ADC_CS_READY_BITS));

                // 12-bit result
                int16_t sample = (uint16_t) adc_hw->result;
                // TODO: high-pass?
                sample -= (1 << 11);
                sample *= (1 << 4);
                sample_buffers[buf][x] = sample;
                sum += sample*sample;
            }

            sample_square_sum[buf] = sum;
            sample_last_buffer = buf;
            xSemaphoreGive(sample_semaphore);
        }
    }
}

void main_task(__unused void *params) {
    printf("cyw43_arch_init\n");
    if (cyw43_arch_init()) {
        printf("failed to initialise\n");
        return;
    }
    /*cyw43_arch_enable_sta_mode();
    printf("Connecting to Wi-Fi...\n");
    if (cyw43_arch_wifi_connect_timeout_ms(WIFI_SSID, WIFI_PASSWORD, CYW43_AUTH_WPA2_AES_PSK, 30000)) {
        printf("failed to connect.\n");
        exit(1);
    } else {
        printf("Connected.\n");
    }

    ip_addr_t ping_addr;
    ipaddr_aton(PING_ADDR, &ping_addr);
    ping_init(&ping_addr);*/

    printf("cyw43_arch_enable_ap_mode\n");
    cyw43_arch_enable_ap_mode("hackerrave", "hackerrave", CYW43_AUTH_WPA2_AES_PSK);

#if LWIP_IPV6
#define IP(x) ((x).u_addr.ip4)
#else
#define IP(x) (x)
#endif

    ip_addr_t gw;
    ip4_addr_t mask;
    IP(gw).addr = PP_HTONL(CYW43_DEFAULT_IP_AP_ADDRESS);
    IP(mask).addr = PP_HTONL(CYW43_DEFAULT_IP_MASK);
    dhcp_server_t dhcp_server;
    dhcp_server_init(&dhcp_server, &gw, &mask);
    
    /*gpio_init(LED_GPIO);
    gpio_set_dir(LED_GPIO, GPIO_OUT);*/

    //t += x;

    //memcpy(p->payload, samples, sizeof(samples));
    /*char *req = (char *)p->payload;
    memset(req, 0, BEACON_MSG_LEN_MAX+1);
    snprintf(req, BEACON_MSG_LEN_MAX, "%d\n", counter);*/


    struct udp_pcb* pcb = udp_new();;
    ip_addr_t addr;
    if (!pcb) {
        printf("pcb == NULL\n");
    }
    ipaddr_aton("255.255.255.255", &addr);
    
    TickType_t xLastWakeTime = xTaskGetTickCount();
    TickType_t quiettime = xLastWakeTime + QUIET_TIME;

    printf("main_task enter loop\n");
    for (int xx = 0;;) {
        //printf("main_task xSemaphoreTake\n");
        if (xSemaphoreTake(sample_semaphore, portMAX_DELAY) == pdTRUE) {
            //printf("main_task xSemaphoreTake OK\n");
            xx++;
            // TODO: don't reuse pbuf?
            struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, 2*NUM_SAMPLES, PBUF_RAM);
            if (!p) {
                printf("pbuf NULL?\n");
            }

            /* static int t = 0;
            int x;
            int16_t *samples = p->payload;

            for (x = 0; x < NUM_SAMPLES; x++, t++) {
                samples[x] = 16000*(1+sin(2*M_PI*t/(float)SAMPLE_RATE))*sin(M_PI*t/(float)SAMPLE_RATE*1000);
            }*/

            uint64_t s = sample_square_sum[sample_last_buffer] / NUM_SAMPLES;
            s = sqrt(s);
            //printf("sample_square_sum[sample_last_buffer] = %5"PRIu64"\n", s);
            static bool quiet = false;
            if (s < 400) {
                // quiet
                if (xTaskGetTickCount() >= quiettime) {
                    if (!quiet) {
                        printf("entering quiet mode\n");
                    }
                    quiet = true;
                }
            } else {
                // not quiet - reset timer
                if (quiet) {
                    printf("detected audio\n");
                }
                quiettime = xTaskGetTickCount() + QUIET_TIME;
                quiet = false;
            }

            if (quiet) {
                static int t = 0;
                int16_t *samples = p->payload;
                for (int x = 0; x < NUM_SAMPLES; x++, t++) {
                    samples[x] = 16000*(1+sin(2*M_PI*t/(float)SAMPLE_RATE))*sin(M_PI*t/(float)SAMPLE_RATE*1000);
                }
            } else {
                memcpy(p->payload, sample_buffers[sample_last_buffer], 2*NUM_SAMPLES);
            }

            err_t er = udp_sendto(pcb, p, &addr, UDP_PORT);
            pbuf_free(p);

            if (er != ERR_OK) {
                // first sendto always fails?
                printf("Failed to send UDP packet! error=%d", er);
                cyw43_gpio_set(&cyw43_state, LED_GPIO, 1);
            } else {
                //printf("sent packet\n");
                //printf("Sent %i B packet\n", sizeof(samples));
                //counter++;
                // not much to do as LED is in another task, and we're using RAW (callback) lwIP API
                //printf("gpio_put %i\n", x & 1);
                //gpio_put(LED_GPIO, x & 1);
                cyw43_gpio_set(&cyw43_state, LED_GPIO, (xx >> 4) & 1);
            }

            //vTaskDelay(NUM_SAMPLES * configTICK_RATE_HZ / 48000);
            //vTaskDelayUntil(&xLastWakeTime, NUM_SAMPLES * configTICK_RATE_HZ / SAMPLE_RATE);
        } else {
            printf("main_task xSemaphoreTake failed\n");
        }
    }

    cyw43_arch_deinit();
}

void vLaunch( void) {
    TaskHandle_t task;

    sample_semaphore = xSemaphoreCreateBinary();
    if (!sample_semaphore) {
        printf("NULL semaphore..\n");
    }

    xTaskCreate(main_task, "TestMainThread", configMINIMAL_STACK_SIZE, NULL, TEST_TASK_PRIORITY, &task);
    xTaskCreate(adc_task, "ADC task", configMINIMAL_STACK_SIZE, NULL, ADC_TASK_PRIORITY, NULL);

#if NO_SYS && configUSE_CORE_AFFINITY && configNUMBER_OF_CORES > 1
    // we must bind the main task to one core (well at least while the init is called)
    // (note we only do this in NO_SYS mode, because cyw43_arch_freertos
    // takes care of it otherwise)
    vTaskCoreAffinitySet(task, 1);
#endif

    /* Start the tasks and timer running. */
    vTaskStartScheduler();
}

int main( void )
{
    stdio_init_all();

    /* Configure the hardware ready to run the demo. */
    const char *rtos_name;
#if ( configNUMBER_OF_CORES > 1 )
    rtos_name = "FreeRTOS SMP";
#else
    rtos_name = "FreeRTOS";
#endif

#if ( configNUMBER_OF_CORES == 2 )
    printf("Starting %s on both cores:\n", rtos_name);
    vLaunch();
#elif ( RUN_FREERTOS_ON_CORE == 1 )
    printf("Starting %s on core 1:\n", rtos_name);
    multicore_launch_core1(vLaunch);
    while (true);
#else
    printf("Starting %s on core 0:\n", rtos_name);
    vLaunch();
#endif
    return 0;
}
