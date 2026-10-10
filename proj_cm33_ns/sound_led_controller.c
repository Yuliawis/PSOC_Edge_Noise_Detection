#include "cybsp.h"
#include "sound_led_controller.h"
#include <stdio.h>
#include <string.h>

/* Calibrate room noise, then map the signal above it to brightness. */
#define SOUND_NOISE_FLOOR       (80U)
#define SOUND_BRIGHTNESS_RANGE  (4000U)
#define AUDIO_WINDOW_SAMPLES    (256U)
#define AUDIO_SETTLE_MS         (250U)
#define QUIET_CALIBRATION_MS    (1000U)
#define TICKS_PER_MS            (20U)
#define PWM_STEPS               (100U) /* 200 Hz PWM at a 20 kHz tick. */

static volatile uint32_t milliseconds;
static volatile uint32_t last_audio_ms[2];
static volatile uint32_t audio_level[2];
static volatile uint32_t brightness;
static volatile uint32_t mode = 1U;
static uint32_t audio_sum[2];
static uint32_t audio_count[2];
static volatile uint32_t mode_started_ms;
static volatile bool calibrating;
static volatile uint32_t quiet_threshold = SOUND_NOISE_FLOOR;
static volatile uint32_t calibration_sum[2];
static volatile uint32_t calibration_count[2];
static bool terminal_seen;

static void print_help(void)
{
    printf("\r\nPSOC Edge sound LEDs - terminal: 115200 baud, 8N1\r\n"
           "Type a command and press Enter:\r\n"
           "  1 : original LED1 blink (default)\r\n"
           "  2 : LED1 + LED2 brightness follows sound\r\n"
           "                quiet = off, louder = brighter\r\n"
           "  3 : show these instructions\r\n"
           "  4 : show mode, sound level and brightness\r\n"
           "  5 : measure quiet room noise again in sound mode\r\n"
           "Keep quiet for 1.25 seconds when entering sound mode.\r\n"
           "Current mode: %lu\r\n> ", (unsigned long)mode);
}

/* Interrupt-driven PWM keeps brightness steady while UART output blocks. */
static void led_tick(void)
{
    static uint32_t sub_ms;
    static uint32_t phase;
    static uint32_t blink_ms;
    static uint32_t previous_mode;
    static bool blink_on;
    bool on;

    if (++sub_ms == TICKS_PER_MS)
    {
        sub_ms = 0U;
        ++milliseconds;
        /* Update the quiet gate here so UART printing cannot delay LED off. */
        if ((mode == 2U) && calibrating &&
            (milliseconds - mode_started_ms >= AUDIO_SETTLE_MS + QUIET_CALIBRATION_MS))
        {
            /* Keep the LEDs off if either microphone has not supplied data. */
            if ((calibration_count[0] != 0U) && (calibration_count[1] != 0U))
            {
                uint32_t baseline0 = calibration_sum[0] / calibration_count[0];
                uint32_t baseline1 = calibration_sum[1] / calibration_count[1];
                uint32_t baseline = baseline0 > baseline1 ? baseline0 : baseline1;
                quiet_threshold = baseline + baseline / 2U + SOUND_NOISE_FLOOR;
                calibrating = false;
            }
        }
        uint32_t level = audio_level[0] > audio_level[1] ? audio_level[0] : audio_level[1];
        if ((mode == 2U) && !calibrating &&
            (milliseconds - last_audio_ms[0] < 100U) &&
            (milliseconds - last_audio_ms[1] < 100U) && (level > quiet_threshold))
        {
            uint32_t above_quiet = level - quiet_threshold;
            brightness = (above_quiet >= SOUND_BRIGHTNESS_RANGE) ? PWM_STEPS :
                         above_quiet * PWM_STEPS / SOUND_BRIGHTNESS_RANGE;
        }
        else
        {
            brightness = 0U;
        }
        if ((mode == 1U) && (++blink_ms >= 1000U))
        {
            blink_ms = 0U;
            blink_on = !blink_on;
        }
    }
    if (mode != previous_mode)
    {
        previous_mode = mode;
        blink_ms = 0U;
        blink_on = true;
        phase = 0U;
    }
    if (++phase >= PWM_STEPS)
    {
        phase = 0U;
    }
    on = (mode == 1U) ? blink_on :
         ((milliseconds - last_audio_ms[0] < 100U) &&
          (milliseconds - last_audio_ms[1] < 100U) && (phase < brightness));
    Cy_GPIO_Write(CYBSP_USER_LED1_PORT, CYBSP_USER_LED1_PIN,
                  on ? CYBSP_LED_STATE_ON : CYBSP_LED_STATE_OFF);
    Cy_GPIO_Write(CYBSP_USER_LED2_PORT, CYBSP_USER_LED2_PIN,
                  ((mode == 2U) && on) ? CYBSP_LED_STATE_ON : CYBSP_LED_STATE_OFF);
}

static void audio_interrupt(uint8_t channel, uint32_t index)
{
    uint32_t status = Cy_PDM_PCM_Channel_GetInterruptStatus(CYBSP_PDM_HW, channel);
    uint32_t count = Cy_PDM_PCM_Channel_GetNumInFifo(CYBSP_PDM_HW, channel);
    /* Drain a snapshot so ISR time stays bounded. Samples are signed 16-bit. */
    while (count-- != 0U)
    {
        int32_t sample = (int16_t)Cy_PDM_PCM_Channel_ReadFifo(CYBSP_PDM_HW, channel);
        audio_sum[index] += (uint32_t)((sample < 0) ? -sample : sample);
        if (++audio_count[index] == AUDIO_WINDOW_SAMPLES)
        {
            audio_level[index] = audio_sum[index] / AUDIO_WINDOW_SAMPLES;
            if (calibrating &&
                (milliseconds - mode_started_ms >= AUDIO_SETTLE_MS) &&
                (milliseconds - mode_started_ms < AUDIO_SETTLE_MS + QUIET_CALIBRATION_MS))
            {
                calibration_sum[index] += audio_level[index];
                ++calibration_count[index];
            }
            audio_sum[index] = 0U;
            audio_count[index] = 0U;
            last_audio_ms[index] = milliseconds;
        }
    }
    Cy_PDM_PCM_Channel_ClearInterrupt(CYBSP_PDM_HW, channel, status);
}

static void audio2_interrupt(void) { audio_interrupt(2U, 0U); }
static void audio3_interrupt(void) { audio_interrupt(3U, 1U); }

void sound_led_init(void)
{
    const cy_stc_sysint_t irq2 = {CYBSP_PDM_CHANNEL_2_IRQ, 3U};
    const cy_stc_sysint_t irq3 = {CYBSP_PDM_CHANNEL_3_IRQ, 3U};
    if ((Cy_PDM_PCM_Init(CYBSP_PDM_HW, &CYBSP_PDM_config) != CY_PDM_PCM_SUCCESS) ||
        (Cy_PDM_PCM_Channel_Init(CYBSP_PDM_HW, &channel_2_config, 2U) != CY_PDM_PCM_SUCCESS) ||
        (Cy_PDM_PCM_Channel_Init(CYBSP_PDM_HW, &channel_3_config, 3U) != CY_PDM_PCM_SUCCESS) ||
        (Cy_SysInt_Init(&irq2, audio2_interrupt) != CY_SYSINT_SUCCESS) ||
        (Cy_SysInt_Init(&irq3, audio3_interrupt) != CY_SYSINT_SUCCESS))
    {
        printf("Microphone initialization failed.\r\n");
        CY_ASSERT(false);
        for (;;) {}
    }
    Cy_PDM_PCM_Channel_SetInterruptMask(CYBSP_PDM_HW, 2U, CY_PDM_PCM_INTR_RX_TRIGGER);
    Cy_PDM_PCM_Channel_SetInterruptMask(CYBSP_PDM_HW, 3U, CY_PDM_PCM_INTR_RX_TRIGGER);
    NVIC_EnableIRQ(CYBSP_PDM_CHANNEL_2_IRQ);
    NVIC_EnableIRQ(CYBSP_PDM_CHANNEL_3_IRQ);
    Cy_PDM_PCM_Activate_Channel(CYBSP_PDM_HW, 2U);
    Cy_PDM_PCM_Activate_Channel(CYBSP_PDM_HW, 3U);
    Cy_SysTick_Init(CY_SYSTICK_CLOCK_SOURCE_CLK_CPU,
                   (SystemCoreClock / (1000U * TICKS_PER_MS)) - 1U);
    Cy_SysTick_SetCallback(0U, led_tick);
    NVIC_SetPriority(SysTick_IRQn, 2U);
    print_help();
}

static void run_command(const char *command)
{
    if ((strcmp(command, "1") == 0) || (strcmp(command, "2") == 0))
    {
        uint32_t next_mode = (strcmp(command, "1") == 0) ? 1U : 2U;
        uint32_t interrupt_state = Cy_SysLib_EnterCriticalSection();
        brightness = 0U;
        mode_started_ms = milliseconds;
        calibration_sum[0] = calibration_sum[1] = 0U;
        calibration_count[0] = calibration_count[1] = 0U;
        calibrating = (next_mode == 2U);
        mode = next_mode;
        Cy_SysLib_ExitCriticalSection(interrupt_state);
        printf("Mode %lu: %s\r\n", (unsigned long)mode,
               (mode == 1U) ? "LED blink" : "sound-controlled brightness");
        if (mode == 2U) printf("Keep quiet for 1.25 seconds to calibrate. LEDs stay off.\r\n");
    }
    else if (strcmp(command, "5") == 0)
    {
        if (mode == 2U)
        {
            run_command("2");
            return;
        }
        printf("Select sound mode with 2 first.\r\n");
    }
    else if (strcmp(command, "3") == 0)
    {
        print_help();
        return;
    }
    else if (strcmp(command, "4") == 0)
    {
        printf("Mode %lu, PCM levels %lu/%lu, brightness %lu%%, quiet cutoff %lu%s\r\n",
               (unsigned long)mode, (unsigned long)audio_level[0],
               (unsigned long)audio_level[1], (unsigned long)brightness,
               (unsigned long)quiet_threshold, calibrating ? " (calibrating)" : "");
    }
    else
    {
        printf("Unknown command. Type 3 for instructions.\r\n");
    }
    printf("> ");
}

void sound_led_poll_terminal(void)
{
    static char command[32];
    static uint32_t length;
    static bool too_long;
    static bool previous_cr;
    static uint32_t help_ms;
    /* KitProg UART has no terminal-open notification. Repeat help until input
     * arrives, so a terminal opened after boot also receives instructions. */
    if (!terminal_seen && (milliseconds - help_ms >= 10000U))
    {
        help_ms = milliseconds;
        print_help();
    }
    if (Cy_SCB_UART_GetNumInRxFifo(CYBSP_DEBUG_UART_HW) != 0U)
    {
        char ch = (char)Cy_SCB_UART_Get(CYBSP_DEBUG_UART_HW);
        terminal_seen = true;
        if ((ch == '\r') || (ch == '\n'))
        {
            if (!((ch == '\n') && previous_cr))
            {
                printf("\r\n");
                command[length] = '\0';
                if (too_long)
                {
                    printf("Command too long. Type 3 for instructions.\r\n> ");
                }
                else
                {
                    /* Allow surrounding whitespace. */
                    while ((length > 0U) && (command[length - 1U] == ' '))
                        command[--length] = '\0';
                    const char *start = command;
                    while (*start == ' ') ++start;
                    run_command(start);
                }
                length = 0U;
                too_long = false;
            }
            previous_cr = (ch == '\r');
        }
        else
        {
            previous_cr = false;
            if ((ch == '\b') || (ch == 127))
            {
                if (!too_long && (length > 0U))
                {
                    --length;
                    printf("\b \b");
                }
            }
            else if ((ch >= ' ') && (ch <= '~'))
            {
                if (!too_long && (length < sizeof(command) - 1U))
                {
                    command[length++] = ch;
                    putchar(ch);
                }
                else too_long = true;
            }
        }
    }
}
