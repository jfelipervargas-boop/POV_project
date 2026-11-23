#include <stdint.h>
#include <stm32f4xx.h>   /* CMSIS: RCC, FLASH, GPIOA, DWT, CoreDebug */

/* =====================================================================
 *  PARAMETROS QUE VAS A EDITAR
 * ===================================================================== */
#define ANIM_STATIC    0   /* color fijo en los LEDs de LED_MASK            */
#define ANIM_BLINK     1   /* parpadeo del color en los LEDs de LED_MASK    */
#define ANIM_CHASE     2   /* un LED que recorre las tarjetas               */
#define ANIM_BREATHE   3   /* el color sube y baja de brillo suavemente     */
#define ANIM_RAINBOW   4   /* arcoiris que se desplaza                      */

#define ANIMATION       ANIM_RAINBOW

#define COLOR_HEX       0xFF4000UL  /* Color 0xRRGGBB (STATIC/BLINK/CHASE/BREATHE) */
#define LED_MASK        0xFFFFU     /* LEDs activos: bit0 = LED 1 ... bit15 = LED 16 */
#define BRIGHTNESS      64          /* 0..255 (brillo global)                */

/* Tiempo entre pasos de la animacion, en microsegundos.
 *   20000 = 20 ms (50 pasos/s)    1000 = 1 ms
 *   0     = lo mas rapido posible (queda limitado por el tiempo de envio) */
#define ANIM_PERIOD_US  10UL

#define BLINK_STEPS     25          /* pasos que dura cada estado (BLINK)    */
#define BREATHE_STEPS   256         /* pasos por ciclo completo (BREATHE)    */
#define RAINBOW_SPREAD  16          /* cuanto cambia el tono entre LEDs      */

/* Tiempo de "latch" en bajo al final de cada trama. 280 us es seguro para
 * cualquier WS2812/WS2812B. Si tus LEDs son WS2812 originales puedes
 * probar ~60 us para ir mas rapido; si ves parpadeos, vuelve a 280. */
#define LATCH_US        280UL

#define LED_PIN         0           /* Dato (DIN) en PA0 */
#define NUM_LEDS        16          /* 2 tarjetas x 8 LEDs, en serie */

/* #define INVERTED_OUTPUT */       /* descomenta si tu adaptador invierte */

/* ---------------- Temporizacion WS2812 @ 84 MHz ---------------- */
#define SYSCLK_HZ     84000000UL
#define CYC_PER_US    (SYSCLK_HZ / 1000000UL)
#define CYC_PERIOD    105U          /* 1.25 us por bit */
#define CYC_T0H       30U           /* 0.36 us */
#define CYC_T1H       63U           /* 0.75 us */
#define CYC_LATCH     (CYC_PER_US * LATCH_US)

#ifdef INVERTED_OUTPUT
  #define PIN_HIGH()  (GPIOA->BSRR = (1U << (LED_PIN + 16)))
  #define PIN_LOW()   (GPIOA->BSRR = (1U << LED_PIN))
#else
  #define PIN_HIGH()  (GPIOA->BSRR = (1U << LED_PIN))
  #define PIN_LOW()   (GPIOA->BSRR = (1U << (LED_PIN + 16)))
#endif

/* Buffer de colores (0xRRGGBB). Puedes modificarlo desde cualquier parte
 * del programa; los cambios se ven en el siguiente leds_show(). */
static uint32_t pixels[NUM_LEDS];

static void clock_config(void)
{
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

/* ---------------- Envio al WS2812 ---------------- */
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

    delay_cycles(CYC_LATCH);
}

/* Convierte pixels[] (RGB + brillo global) a GRB y lo envia. */
static void leds_show(void)
{
    uint8_t frame[NUM_LEDS * 3];

    for (int i = 0; i < NUM_LEDS; i++) {
        uint32_t c = pixels[i];
        frame[i * 3 + 0] = (uint8_t)((((c >>  8) & 0xFF) * BRIGHTNESS) / 255);  /* G */
        frame[i * 3 + 1] = (uint8_t)((((c >> 16) & 0xFF) * BRIGHTNESS) / 255);  /* R */
        frame[i * 3 + 2] = (uint8_t)((( c        & 0xFF) * BRIGHTNESS) / 255);  /* B */
    }
    ws2812_send(frame, sizeof frame);
}

/* ---------------- Utilidades de color ---------------- */
static void led_fill(uint32_t hex)
{
    for (int i = 0; i < NUM_LEDS; i++) pixels[i] = hex;
}

/* Escala un color 0xRRGGBB por v/255. */
static uint32_t scale_hex(uint32_t hex, uint32_t v)
{
    uint32_t r = (((hex >> 16) & 0xFF) * v) / 255;
    uint32_t g = (((hex >>  8) & 0xFF) * v) / 255;
    uint32_t b = (( hex        & 0xFF) * v) / 255;
    return (r << 16) | (g << 8) | b;
}

/* Rueda de color: pos 0..255 recorre rojo -> verde -> azul -> rojo. */
static uint32_t wheel(uint8_t pos)
{
    pos = (uint8_t)(255 - pos);
    if (pos < 85)  return ((uint32_t)(255 - pos * 3) << 16) | (uint32_t)(pos * 3);
    if (pos < 170) { pos -= 85;  return ((uint32_t)(pos * 3) << 8) | (uint32_t)(255 - pos * 3); }
    pos -= 170;
    return ((uint32_t)(pos * 3) << 16) | ((uint32_t)(255 - pos * 3) << 8);
}

/* ---------------- Animaciones: actualizan pixels[] en cada paso ---------------- */
static void animate(uint32_t step)
{
    switch (ANIMATION) {

    case ANIM_STATIC:
        for (int i = 0; i < NUM_LEDS; i++)
            pixels[i] = ((LED_MASK >> i) & 1U) ? COLOR_HEX : 0;
        break;

    case ANIM_BLINK: {
        uint32_t on = ((step / BLINK_STEPS) & 1U) == 0;
        for (int i = 0; i < NUM_LEDS; i++)
            pixels[i] = (on && ((LED_MASK >> i) & 1U)) ? COLOR_HEX : 0;
        break;
    }

    case ANIM_CHASE:
        led_fill(0);
        pixels[step % NUM_LEDS] = COLOR_HEX;
        break;

    case ANIM_BREATHE: {
        uint32_t ph = (step % BREATHE_STEPS) * 512U / BREATHE_STEPS;   /* 0..511 */
        uint32_t v  = (ph < 256U) ? ph : (511U - ph);                  /* triangulo 0..255 */
        uint32_t c  = scale_hex(COLOR_HEX, v);
        for (int i = 0; i < NUM_LEDS; i++)
            pixels[i] = ((LED_MASK >> i) & 1U) ? c : 0;
        break;
    }

    case ANIM_RAINBOW:
        for (int i = 0; i < NUM_LEDS; i++)
            pixels[i] = ((LED_MASK >> i) & 1U)
                        ? wheel((uint8_t)(step + (uint32_t)i * RAINBOW_SPREAD)) : 0;
        break;
    }
}

int main(void)
{
    clock_config();
    dwt_init();
    gpio_init();

    delay_cycles(CYC_PER_US * 50000UL);   /* 50 ms para estabilizar alimentacion */

    uint32_t step = 0;
    uint32_t next = DWT->CYCCNT;

    while (1) {
        /* Espera activa hasta el siguiente paso. Aqui puedes hacer otras
         * tareas (leer botones, UART, etc.) mientras llega el momento. */
        while ((int32_t)(DWT->CYCCNT - next) < 0);
        next += CYC_PER_US * ANIM_PERIOD_US;

        animate(step++);
        leds_show();

        /* Si el envio tardo mas que el periodo, resincroniza (sin rafagas). */
        if ((int32_t)(DWT->CYCCNT - next) > 0) next = DWT->CYCCNT;
    }
}