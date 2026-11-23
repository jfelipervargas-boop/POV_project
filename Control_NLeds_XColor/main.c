#include <stdint.h>
#include <stm32f4xx.h>   /* CMSIS: RCC, FLASH, GPIOA, DWT, CoreDebug */

/* =====================================================================
 *  PARAMETROS QUE VAS A EDITAR
 * ===================================================================== */
#define COLOR_HEX    0x010101UL   /* Color en formato 0xRRGGBB             */
#define LED_MASK     0xFFFFU      /* Que LEDs encender (16 bits, ver abajo) */
#define BRIGHTNESS   64           /* 0..255 (255 = maximo brillo)          */

/* LED_MASK: un bit por LED, 16 LEDs en total (2 tarjetas en serie).
 *   bit0..bit7  = tarjeta 1 (LED 1..8)
 *   bit8..bit15 = tarjeta 2 (LED 9..16)
 *   0xFFFF = todos            0x00FF = toda la tarjeta 1
 *   0xFF00 = toda la tarjeta 2   0x0001 = solo el LED 1
 *   0x8000 = solo el LED 16      0x0F0F = LEDs 1-4 y 9-12
 *   0x5555 = LEDs impares        0xAAAA = LEDs pares
 * Ejemplos de color: 0xFF0000 rojo, 0x00FF00 verde, 0x0000FF azul,
 *   0xFFFFFF blanco, 0xFFA500 naranja, 0x800080 morado, 0x000000 apagado */

#define LED_PIN      0        /* Dato (DIN) en PA0 */
#define NUM_LEDS     16       /* 2 tarjetas x 8 LEDs, en serie */

/* Descomenta si tu adaptador de nivel INVIERTE la senal. */
/* #define INVERTED_OUTPUT */

/* ---------------- Temporizacion WS2812 @ 84 MHz ----------------
 * Periodo de bit = 105 ciclos = 1.25 us
 *   bit 0: alto 30 ciclos (0.36 us)   bit 1: alto 63 ciclos (0.75 us) */
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
    /* SYSCLK = 84 MHz desde HSI: VCO_in = 2 MHz, VCO = 336 MHz, /4 = 84 MHz */
    RCC->CR |= RCC_CR_HSION;
    while (!(RCC->CR & RCC_CR_HSIRDY));

    FLASH->ACR = FLASH_ACR_PRFTEN | FLASH_ACR_ICEN | FLASH_ACR_DCEN |
                 FLASH_ACR_LATENCY_2WS;
    while ((FLASH->ACR & FLASH_ACR_LATENCY) != FLASH_ACR_LATENCY_2WS);

    RCC->PLLCFGR = (8   << RCC_PLLCFGR_PLLM_Pos) |
                   (168 << RCC_PLLCFGR_PLLN_Pos) |
                   (1   << RCC_PLLCFGR_PLLP_Pos) |
                   (7   << RCC_PLLCFGR_PLLQ_Pos) |
                   RCC_PLLCFGR_PLLSRC_HSI;

    RCC->CR |= RCC_CR_PLLON;
    while (!(RCC->CR & RCC_CR_PLLRDY));

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
    (void)RCC->AHB1ENR;

    GPIOA->MODER    = (GPIOA->MODER & ~(3U << (LED_PIN * 2))) | (1U << (LED_PIN * 2));
    GPIOA->OTYPER  &= ~(1U << LED_PIN);
    GPIOA->OSPEEDR |= (3U << (LED_PIN * 2));
    GPIOA->PUPDR   &= ~(3U << (LED_PIN * 2));

    PIN_LOW();
}

static void ws2812_send(const uint8_t *buf, uint32_t len)
{
    uint32_t t = DWT->CYCCNT;

    __disable_irq();
    for (uint32_t n = 0; n < len; n++) {
        uint8_t byte = buf[n];
        for (int i = 7; i >= 0; i--) {
            uint32_t high = (byte & (1U << i)) ? CYC_T1H : CYC_T0H;

            t += CYC_PERIOD;
            while ((int32_t)(DWT->CYCCNT - t) < 0);

            PIN_HIGH();
            while ((DWT->CYCCNT - t) < high);
            PIN_LOW();
        }
    }
    __enable_irq();

    delay_cycles(CYC_RESET);   /* latch: linea en bajo > 280 us */
}

/* Enciende con 'hex' (0xRRGGBB) los LEDs cuyo bit este en 'mask'.
 * Los LEDs que no esten en la mascara quedan apagados. */
static void ws2812_show(uint32_t hex, uint16_t mask)
{
    uint8_t frame[NUM_LEDS * 3];

    uint32_t r = (((hex >> 16) & 0xFF) * BRIGHTNESS) / 255;
    uint32_t g = (((hex >>  8) & 0xFF) * BRIGHTNESS) / 255;
    uint32_t b = (( hex        & 0xFF) * BRIGHTNESS) / 255;

    for (int i = 0; i < NUM_LEDS; i++) {
        int on = (mask >> i) & 1U;
        frame[i * 3 + 0] = on ? (uint8_t)g : 0;   /* WS2812: orden GRB */
        frame[i * 3 + 1] = on ? (uint8_t)r : 0;
        frame[i * 3 + 2] = on ? (uint8_t)b : 0;
    }
    ws2812_send(frame, sizeof frame);
}

int main(void)
{
    clock_config();
    dwt_init();
    gpio_init();

    delay_ms(50);

    while (1) {
        ws2812_show(COLOR_HEX, LED_MASK);
        delay_ms(100);     /* se reenvia periodicamente por robustez */
    }
}