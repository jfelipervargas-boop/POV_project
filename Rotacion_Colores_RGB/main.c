#include <stdint.h>
#include <stm32f4xx.h>   /* CMSIS: RCC, FLASH, GPIOA, DWT, CoreDebug */

/* ---------------- Configuracion ---------------- */
#define LED_PIN      0        /* Dato (DIN) en PA0 */
#define NUM_LEDS     8
#define BRIGHTNESS   64       /* 0..255 (255 = maximo brillo) */

/* Descomenta si tu adaptador de nivel INVIERTE la senal
 * (por ejemplo, un solo transistor NPN). */
/* #define INVERTED_OUTPUT */

/* ---------------- Temporizacion WS2812 @ 84 MHz ----------------
 * 1 ciclo = 11.9 ns.  Periodo de bit = 105 ciclos = 1.25 us
 *   bit 0: alto 30 ciclos (0.36 us), bajo el resto (0.89 us)
 *   bit 1: alto 63 ciclos (0.75 us), bajo el resto (0.50 us)
 * Valores dentro de tolerancia para WS2812 y WS2812B.            */
#define SYSCLK_HZ     84000000UL
#define CYC_PERIOD    105U
#define CYC_T0H       30U
#define CYC_T1H       63U
#define CYC_RESET     (SYSCLK_HZ / 1000000UL * 300UL)   /* 300 us de latch */

#ifdef INVERTED_OUTPUT
  #define PIN_HIGH()  (GPIOA->BSRR = (1U << (LED_PIN + 16)))
  #define PIN_LOW()   (GPIOA->BSRR = (1U << LED_PIN))
#else
  #define PIN_HIGH()  (GPIOA->BSRR = (1U << LED_PIN))
  #define PIN_LOW()   (GPIOA->BSRR = (1U << (LED_PIN + 16)))
#endif

static void clock_config(void)
{
    /* SYSCLK = 84 MHz desde HSI (16 MHz):
     * VCO_in = 16/8 = 2 MHz, VCO = 2*168 = 336 MHz,
     * SYSCLK = 336/4 = 84 MHz, USB = 336/7 = 48 MHz */
    RCC->CR |= RCC_CR_HSION;
    while (!(RCC->CR & RCC_CR_HSIRDY));

    /* 84 MHz a 3.3 V -> 2 estados de espera en FLASH */
    FLASH->ACR = FLASH_ACR_PRFTEN | FLASH_ACR_ICEN | FLASH_ACR_DCEN |
                 FLASH_ACR_LATENCY_2WS;
    while ((FLASH->ACR & FLASH_ACR_LATENCY) != FLASH_ACR_LATENCY_2WS);

    RCC->PLLCFGR = (8   << RCC_PLLCFGR_PLLM_Pos) |
                   (168 << RCC_PLLCFGR_PLLN_Pos) |
                   (1   << RCC_PLLCFGR_PLLP_Pos) |   /* 01 -> /4 */
                   (7   << RCC_PLLCFGR_PLLQ_Pos) |
                   RCC_PLLCFGR_PLLSRC_HSI;

    RCC->CR |= RCC_CR_PLLON;
    while (!(RCC->CR & RCC_CR_PLLRDY));

    /* AHB /1, APB1 /2 (max 50 MHz), APB2 /1 */
    RCC->CFGR &= ~(RCC_CFGR_HPRE | RCC_CFGR_PPRE1 | RCC_CFGR_PPRE2);
    RCC->CFGR |= RCC_CFGR_HPRE_DIV1 | RCC_CFGR_PPRE1_DIV2 | RCC_CFGR_PPRE2_DIV1;

    RCC->CFGR = (RCC->CFGR & ~RCC_CFGR_SW) | RCC_CFGR_SW_PLL;
    while ((RCC->CFGR & RCC_CFGR_SWS) != RCC_CFGR_SWS_PLL);
}

static void dwt_init(void)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0;
    DWT->CTRL  |= DWT_CTRL_CYCCNTENA_Msk;
}

static void delay_cycles(uint32_t cycles)
{
    uint32_t start = DWT->CYCCNT;
    while ((DWT->CYCCNT - start) < cycles);
}

static void delay_ms(uint32_t ms)
{
    while (ms--) delay_cycles(SYSCLK_HZ / 1000UL);
}

static void gpio_init(void)
{
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN;
    (void)RCC->AHB1ENR;                                   /* lectura dummy */

    GPIOA->MODER   = (GPIOA->MODER   & ~(3U << (LED_PIN * 2))) | (1U << (LED_PIN * 2)); /* salida */
    GPIOA->OTYPER &= ~(1U << LED_PIN);                                                  /* push-pull */
    GPIOA->OSPEEDR |= (3U << (LED_PIN * 2));              /* VERY HIGH speed: flancos rapidos */
    GPIOA->PUPDR   &= ~(3U << (LED_PIN * 2));             /* sin pull-up/down */

    PIN_LOW();
}

/* Envia todos los bytes sin pausas entre bits (programacion absoluta
 * sobre el contador de ciclos, no acumula deriva). */
static void ws2812_send(const uint8_t *buf, uint32_t len)
{
    uint32_t t = DWT->CYCCNT;

    __disable_irq();
    for (uint32_t n = 0; n < len; n++) {
        uint8_t byte = buf[n];
        for (int i = 7; i >= 0; i--) {
            uint32_t high = (byte & (1U << i)) ? CYC_T1H : CYC_T0H;

            t += CYC_PERIOD;                                  /* inicio de este bit */
            while ((int32_t)(DWT->CYCCNT - t) < 0);

            PIN_HIGH();
            while ((DWT->CYCCNT - t) < high);
            PIN_LOW();
        }
    }
    __enable_irq();

    /* Latch / reset: linea en bajo > 280 us (WS2812B nuevos) */
    delay_cycles(CYC_RESET);
}

static void ws2812_set_color(uint8_t r, uint8_t g, uint8_t b)
{
    uint8_t frame[NUM_LEDS * 3];

    r = (uint8_t)((r * BRIGHTNESS) / 255);
    g = (uint8_t)((g * BRIGHTNESS) / 255);
    b = (uint8_t)((b * BRIGHTNESS) / 255);

    for (int i = 0; i < NUM_LEDS; i++) {
        frame[i * 3 + 0] = g;     /* WS2812 espera orden GRB */
        frame[i * 3 + 1] = r;
        frame[i * 3 + 2] = b;
    }
    ws2812_send(frame, sizeof frame);
}

int main(void)
{
    clock_config();
    dwt_init();
    gpio_init();

    delay_ms(50);                 /* dejar estabilizar la alimentacion de los LEDs */
    ws2812_set_color(0, 0, 0);    /* limpiar estado inicial */

    /* DIAGNOSTICO: parpadeo de PC13 (LED de la placa) */
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOCEN;
    (void)RCC->AHB1ENR;
    GPIOC->MODER = (GPIOC->MODER & ~(3U << 26)) | (1U << 26);
    for (int k = 0; k < 6; k++) {
        GPIOC->ODR ^= (1U << 13);
        delay_ms(200);
    }

    while (1) {
        ws2812_set_color(255, 0, 0);   /* rojo  */
        delay_ms(500);

        ws2812_set_color(0, 255, 0);   /* verde */
        delay_ms(500);

        ws2812_set_color(0, 0, 255);   /* azul  */
        delay_ms(500);
    }
}