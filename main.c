/**
 * @file    main.c
 * @brief   Reloj POV de hora (HH:MM) y día de la semana (Lun-Dom) sobre STM32F411CEU6.
 * @note    Programado a nivel de registros (bare-metal) sin bibliotecas HAL/LL.
 *
 * DECLARACIÓN DE USO DE IA (Regla 10):
 * Se utilizó Gemini como herramienta de apoyo técnico específicamente en:
 *   1. La disposición radial de la matriz (codificación columnar 3x7 y mapeo de dos anillos).
 *   2. La formulación matemática de discretización angular en tiempo real (dt * COLUMNAS / t_vuelta).
 * Dichos bloques se encuentran marcados con la etiqueta [APOYO IA - GEMINI].
 */

#include <stdint.h>
#include <stm32f4xx.h>

/* ---------------- AJUSTES DE HORA, COLOR Y GEOMETRÍA ---------------- */
#define HORA_INI     12      // Hora inicial (formato 24h: 0..23)
#define MIN_INI      59      // Minuto inicial (0..59)
#define DIA_INI      7       // Día inicial: 1=Lun, 2=Mar, 3=Mie, 4=Jue, 5=Vie, 6=Sab, 7=Dom

#define COLOR        0xFF0000UL // Color en formato RGB de 24 bits (0xFF0000 = Rojo puro)
#define BRILLO       65         // Escala de intensidad (0..255) para limitar consumo en CR2032
#define COLUMNAS     32U        // Resolución angular: 32 divisiones por vuelta (11.25 grados/col)
#define GIRO         0U         // Desfase angular en columnas (0..31) para centrar el texto
// #define ESPEJO               // Descomentar si el texto se lee invertido horizontalmente
// #define DE_CABEZA            // Descomentar si el texto se lee invertido verticalmente

/* ---------------- VARIABLES GLOBALES DE ESTADO ---------------- */
static uint32_t leds[15];                   // Buffer RGB de los 15 LEDs: [0..6]=Día, [7]=Espacio, [8..14]=Hora
static uint8_t  col_hora[COLUMNAS];         // Mapa de bits por columna angular para la hora
static uint8_t  col_dia[COLUMNAS];          // Mapa de bits por columna angular para el día
static uint16_t ult_pulso = 0;              // Marca de tiempo (en ticks de 10 us) del último pulso óptico
static uint32_t t_vuelta  = 1714U;          // Período de revolución inicial (~3500 RPM = 17.14 ms)

/**
 * @brief Inicializa reloj del sistema (84 MHz), contador DWT, GPIO, TIM1 y RTC interno.
 */
static void init_hw(void) {
    /* 1. Configuración de reloj: HSI (16 MHz) -> PLL -> SYSCLK = 84 MHz */
    RCC->CR |= RCC_CR_HSION;                          // Enciende oscilador interno HSI de 16 MHz
    while (!(RCC->CR & RCC_CR_HSIRDY));               // Espera estabilización del HSI

    // Habilita prefetch, caché de instrucciones/datos y 2 wait-states en Flash (requerido a 84 MHz)
    FLASH->ACR = FLASH_ACR_PRFTEN | FLASH_ACR_ICEN | FLASH_ACR_DCEN | FLASH_ACR_LATENCY_2WS;

    // Configura PLL: PLLM=8 (2 MHz entrada VCO), PLLN=168 (336 MHz VCO), PLLP=4 (84 MHz SYSCLK), PLLQ=7
    RCC->PLLCFGR = 8U | (168U << 6) | (1U << 16) | (7U << 24);
    RCC->CR |= RCC_CR_PLLON;                          // Enciende el PLL
    while (!(RCC->CR & RCC_CR_PLLRDY));               // Espera enganche (lock) del PLL

    // Prescaler APB1 = /2 (42 MHz máx.) y selecciona PLL como reloj principal del sistema
    RCC->CFGR = (RCC->CFGR & ~RCC_CFGR_SW) | RCC_CFGR_PPRE1_DIV2 | RCC_CFGR_SW_PLL;
    while ((RCC->CFGR & RCC_CFGR_SWS) != RCC_CFGR_SWS_PLL);

    /* 2. Habilitación del contador de ciclos de CPU (DWT->CYCCNT) para temporización en nanosegundos */
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;   // Habilita bloque de traza y depuración
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;              // Activa el contador incremental de 32 bits (1 tick = 11.9 ns)

    /* 3. Configuración de GPIO y Timer 1 (Captura de entrada en PA10 y salida LED en PB9) */
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN | RCC_AHB1ENR_GPIOBEN; // Habilita reloj en puertos A y B
    RCC->APB2ENR |= RCC_APB2ENR_TIM1EN;                        // Habilita reloj en TIM1 (bus APB2 a 84 MHz)

    // PB9 como salida push-pull de muy alta velocidad (50/100 MHz) para datos WS2812B
    GPIOB->MODER   = (GPIOB->MODER & ~(3U << 18)) | (1U << 18);
    GPIOB->OSPEEDR |= (3U << 18);

    // PA10 en Función Alternativa AF1 (TIM1_CH3) con resistencia pull-up interna (~40 kOhm)
    GPIOA->MODER   = (GPIOA->MODER & ~(3U << 20)) | (2U << 20);
    GPIOA->PUPDR   = (GPIOA->PUPDR & ~(3U << 20)) | (1U << 20);
    GPIOA->AFR[1]  = (GPIOA->AFR[1] & ~(0xFU << 8)) | (1U << 8);

    // TIM1 con base de tiempo de 10 us (84 MHz / (839 + 1) = 100 kHz) y conteo libre de 16 bits
    TIM1->PSC   = 839U;
    TIM1->ARR   = 0xFFFFU;
    TIM1->CCMR2 = TIM_CCMR2_CC3S_0 | (0xFU << 4);     // Canal 3 como entrada TI3 con filtro digital máximo (N=8)
    TIM1->CCER  = TIM_CCER_CC3E | TIM_CCER_CC3P;      // Habilita captura en flanco de bajada (detección HW-870)
    TIM1->CR1   = TIM_CR1_CEN;                        // Arranca el contador de TIM1

    /* 4. Configuración del RTC interno usando el oscilador LSI (~32 kHz) */
    RCC->APB1ENR |= RCC_APB1ENR_PWREN;                // Habilita interfaz de control de potencia
    PWR->CR |= PWR_CR_DBP;                            // Desbloquea escritura en el dominio de respaldo (Backup Domain)
    RCC->BDCR |= RCC_BDCR_BDRST;                      // Reinicia dominio de respaldo para cargar la nueva hora/día
    RCC->BDCR &= ~RCC_BDCR_BDRST;

    RCC->CSR |= RCC_CSR_LSION;                        // Enciende oscilador interno de baja velocidad LSI
    while (!(RCC->CSR & RCC_CSR_LSIRDY));             // Espera estabilización del LSI

    RCC->BDCR = RCC_BDCR_RTCSEL_1 | RCC_BDCR_RTCEN;   // Selecciona LSI como fuente del RTC y lo habilita
    RTC->WPR = 0xCAU; RTC->WPR = 0x53U;               // Secuencia de desbloqueo de registros protegidos del RTC
    RTC->ISR |= RTC_ISR_INIT;                         // Entra en modo de inicialización del calendario
    while (!(RTC->ISR & RTC_ISR_INITF));              // Espera confirmación de modo inicialización

    // Preescaladores para obtener 1 Hz a partir de 32 kHz: 32000 / ((127+1)*(249+1)) = 1 Hz
    RTC->PRER = (127U << 16) | 249U;

    // Carga hora (formato BCD de 24h) y día de la semana (bits [15:13] de DR)
    RTC->TR = (((HORA_INI / 10U) << 20) | ((HORA_INI % 10U) << 16) |
               ((MIN_INI  / 10U) << 12) | ((MIN_INI  % 10U) << 8));
    RTC->DR = ((uint32_t)DIA_INI << 13) | (1U << 8) | 1U;

    RTC->ISR &= ~(RTC_ISR_INIT | RTC_ISR_RSF);        // Sale de modo inicialización y limpia bandera de sincronía
    while (!(RTC->ISR & RTC_ISR_RSF));                // Espera sincronización de registros sombra antes de leer
    RTC->WPR = 0xFFU;                                 // Reactiva protección contra escritura en RTC
}

/**
 * @brief Envía la trama de 15 LEDs WS2812B (360 bits en orden GRB) por PB9 usando DWT.
 */
static void enviar_leds(void) {
    uint8_t buf[45]; // 15 LEDs * 3 canales (Verde, Rojo, Azul)

    // Convierte RGB a formato nativo GRB de los WS2812B aplicando factor de brillo
    for (int i = 0; i < 15; i++) {
        buf[i * 3 + 0] = (((leds[i] >>  8) & 0xFFU) * BRILLO) / 255U; // Canal G
        buf[i * 3 + 1] = (((leds[i] >> 16) & 0xFFU) * BRILLO) / 255U; // Canal R
        buf[i * 3 + 2] = (( leds[i]        & 0xFFU) * BRILLO) / 255U; // Canal B
    }

    uint32_t t = DWT->CYCCNT;
    __disable_irq(); // Sección crítica: evita interrupciones que alteren los anchos de pulso de nanosegundos
    for (int n = 0; n < 45; n++) {
        for (int b = 7; b >= 0; b--) {
            // Bit 1: 60 ciclos en alto (714 ns) | Bit 0: 29 ciclos en alto (345 ns)
            uint32_t alto = (buf[n] & (1U << b)) ? 60U : 29U;
            t += 92U;                                         // Período total por bit: 92 ciclos = 1.095 us
            while ((int32_t)(DWT->CYCCNT - t) < 0);           // Espera inicio exacto del siguiente bit
            GPIOB->BSRR = (1U << 9);                          // Pone PB9 en alto (1 lógico)
            while ((DWT->CYCCNT - t) < alto);                 // Mantiene nivel alto durante t_H
            GPIOB->BSRR = (1U << 25);                         // Pone PB9 en bajo (reset bit 9 = 9 + 16 = 25)
        }
    }
    __enable_irq();

    // Pulso de reset/latch > 50 us (5500 ciclos @ 84 MHz = 65.5 us) para fijar colores en los LEDs
    t = DWT->CYCCNT;
    while ((DWT->CYCCNT - t) < 5500U);
}

/* ============================================================================
 * [APOYO IA - GEMINI] DISPOSICIÓN RADIAL DE LA MATRIZ Y FUENTE COMPACTA 3x7
 * Cada carácter se codifica en 3 columnas de 7 bits (bit 0 = fila superior,
 * bit 6 = fila inferior) para proyectar texto legible sin estiramiento angular
 * sobre las 32 divisiones de la circunferencia del ventilador.
 * ============================================================================ */
static const char chars[] = "0123456789:DJLMSVabeinorum";
static const uint8_t font[26][3] = {
    {0x7F,0x41,0x7F}, // 0
    {0x00,0x7F,0x00}, // 1
    {0x79,0x49,0x4F}, // 2
    {0x49,0x49,0x7F}, // 3
    {0x0F,0x08,0x7F}, // 4
    {0x4F,0x49,0x79}, // 5
    {0x7F,0x49,0x79}, // 6
    {0x01,0x01,0x7F}, // 7
    {0x7F,0x49,0x7F}, // 8
    {0x4F,0x49,0x7F}, // 9
    {0x00,0x36,0x00}, // :
    {0x7F,0x41,0x3E}, // D
    {0x30,0x40,0x3F}, // J
    {0x7F,0x40,0x40}, // L
    {0x7F,0x06,0x7F}, // M
    {0x46,0x49,0x31}, // S
    {0x1F,0x60,0x1F}, // V
    {0x20,0x54,0x78}, // a
    {0x7F,0x48,0x30}, // b
    {0x38,0x54,0x58}, // e
    {0x00,0x7A,0x00}, // i
    {0x7C,0x04,0x78}, // n
    {0x38,0x44,0x38}, // o
    {0x7C,0x08,0x04}, // r
    {0x3C,0x40,0x7C}, // u
    {0x7C,0x18,0x7C}  // m
};
static const char *dias[8] = {"", "Lun", "Mar", "Mie", "Jue", "Vie", "Sab", "Dom"};

/**
 * @brief [APOYO IA - GEMINI] Escribe un glifo de 3 columnas + 1 columna de espacio en el buffer angular.
 */
static void poner_letra(uint8_t *dst, char c, int col) {
    int k = 0;
    while (chars[k] && chars[k] != c) k++;
    for (int j = 0; j < 3; j++) {
        if (col + j < (int)COLUMNAS) dst[col + j] = chars[k] ? font[k][j] : 0;
    }
    if (col + 3 < (int)COLUMNAS) dst[col + 3] = 0; // Columna en blanco de separación entre caracteres
}

/**
 * @brief [APOYO IA - GEMINI] Distribuye la hora (col 0..19) y el día (col 3..14) en los dos anillos radiales.
 */
static void armar_texto(uint8_t h, uint8_t m, uint8_t d) {
    for (uint32_t i = 0; i < COLUMNAS; i++) col_hora[i] = col_dia[i] = 0;

    // Anillo exterior (5 caracteres * 4 columnas = columnas 0..19)
    char txt[5] = {'0' + h / 10, '0' + h % 10, ':', '0' + m / 10, '0' + m % 10};
    for (int k = 0; k < 5; k++) poner_letra(col_hora, txt[k], k * 4);

    // Anillo interior (3 caracteres * 4 columnas = columnas 3..14, centrado bajo la hora)
    if (d >= 1 && d <= 7) {
        for (int k = 0; k < 3; k++) poner_letra(col_dia, dias[d][k], 3 + k * 4);
    }
}

/* ---------------- BUCLE PRINCIPAL EN TIEMPO REAL ---------------- */
int main(void) {
    init_hw();
    uint8_t  ult_min = 255; // Fuerza actualización de texto en la primera iteración
    uint32_t ult_col = 999; // Fuerza refresco de LEDs en la primera iteración

    while (1) {
        // 1. Lectura y decodificación BCD de hora y día desde el RTC interno
        uint32_t tr = RTC->TR, dr = RTC->DR;
        uint8_t h = ((tr >> 20) & 3U) * 10U + ((tr >> 16) & 0xFU);
        uint8_t m = ((tr >> 12) & 7U) * 10U + ((tr >> 8)  & 0xFU);
        uint8_t d = (dr >> 13) & 7U;
        if (m != ult_min) {
            ult_min = m;
            armar_texto(h, m, d);
        }

        // 2. Medición del período de revolución Tv mediante captura de hardware en TIM1_CH3
        if (TIM1->SR & TIM_SR_CC3IF) {
            uint16_t ahora = TIM1->CCR3;              // Instante exacto del flanco (limpia CC3IF al leer)
            uint16_t per   = ahora - ult_pulso;       // Aritmética modular de 16 bits (maneja desbordes)
            ult_pulso = ahora;
            // Filtro de rango válido: entre 10 ms (6000 RPM) y 200 ms (300 RPM)
            if (per > 1000U && per < 20000U) t_vuelta = per;
        }

        /* ====================================================================
         * [APOYO IA - GEMINI] MATEMÁTICA DE ACTUALIZACIÓN ANGULAR EN TIEMPO REAL
         * Convierte el tiempo transcurrido desde la marca óptica (dt) en un
         * índice discreto de columna angular: col = floor((dt * COLUMNAS) / Tv).
         * Si dt > 2*Tv (ventilador detenido), se apagan los LEDs.
         * ==================================================================== */
        uint32_t dt  = (uint16_t)(TIM1->CNT - ult_pulso);
        uint32_t col = (dt > 2U * t_vuelta) ? COLUMNAS : (dt * COLUMNAS) / t_vuelta;

        // 3. Renderizado de columna: solo transmite si el aspa avanzó a una nueva división angular
        if (col != ult_col) {
            ult_col = col;
            for (int i = 0; i < 15; i++) leds[i] = 0;

            if (col < COLUMNAS) {
                uint32_t c = (col + GIRO) % COLUMNAS;
                #ifdef ESPEJO
                c = COLUMNAS - 1U - c;
                #endif

                // Mapeo radial de bits hacia las dos regletas (leds[0..6] = día, leds[8..14] = hora)
                for (int i = 0; i < 7; i++) {
                    #ifdef DE_CABEZA
                    int f = 6 - i;
                    #else
                    int f = i;
                    #endif
                    if ((col_dia[c]  >> (6 - f)) & 1U) leds[i]     = COLOR;
                    if ((col_hora[c] >> (6 - f)) & 1U) leds[i + 8] = COLOR;
                }
            }
            enviar_leds();
        }
    }
}