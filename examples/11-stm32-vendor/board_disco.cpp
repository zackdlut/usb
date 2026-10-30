// STM32F4 Discovery：USB 接在 OTG_FS（PA11 = D-，PA12 = D+，PA9 = VBUS 检测），
// 也就是板子上的 micro-USB 口 CN5，不是给 ST-LINK 用的那个 mini-USB 口。
#include "board.h"
#include "board_usb.h"
#include "tusb.h"

extern "C" {   // stm32 教程里的驱动是 C 写的
#include "tick.h"
#include "uart.h"
}

static constexpr uint32_t kSysclkHz = 168000000u;
static constexpr uint32_t kPclk1Hz = kSysclkHz / 4u;

// HSE 8 MHz -> /M=8 -> 1 MHz -> xN=336 -> VCO 336 MHz
//   /P=2 -> 168 MHz 系统时钟
//   /Q=7 ->  48 MHz 给 USB（全速 USB 要求时钟误差 < 0.25%，HSI 做不到，必须用晶振）
static void clock_init_168mhz()
{
    RCC->CR |= RCC_CR_HSEON;
    while (!(RCC->CR & RCC_CR_HSERDY)) {
    }
    RCC->APB1ENR |= RCC_APB1ENR_PWREN;
    PWR->CR |= PWR_CR_VOS;
    RCC->PLLCFGR = (8u << RCC_PLLCFGR_PLLM_Pos) | (336u << RCC_PLLCFGR_PLLN_Pos) |
                   (0u << RCC_PLLCFGR_PLLP_Pos) | RCC_PLLCFGR_PLLSRC_HSE |
                   (7u << RCC_PLLCFGR_PLLQ_Pos);
    RCC->CR |= RCC_CR_PLLON;
    while (!(RCC->CR & RCC_CR_PLLRDY)) {
    }
    FLASH->ACR = FLASH_ACR_LATENCY_5WS | FLASH_ACR_PRFTEN | FLASH_ACR_ICEN | FLASH_ACR_DCEN;
    RCC->CFGR = RCC_CFGR_HPRE_DIV1 | RCC_CFGR_PPRE1_DIV4 | RCC_CFGR_PPRE2_DIV2 | RCC_CFGR_SW_PLL;
    while ((RCC->CFGR & RCC_CFGR_SWS) != RCC_CFGR_SWS_PLL) {
    }
    SystemCoreClock = kSysclkHz;
}

void board_init()
{
    clock_init_168mhz();
    board_init_leds();
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN;
    gpio_set_mode(GPIOA, 0, GPIO_MODE_INPUT);   // PA0 用户按键，按下为高
    uart_init(kPclk1Hz, 115200);                 // USART2：PA2 TX，接 USB-TTL 看日志
    tick_init(kSysclkHz);
}

bool board_usb_start()
{
    gpio_set_af(GPIOA, 11, 10);   // AF10 = OTG_FS
    gpio_set_af(GPIOA, 12, 10);
    GPIOA->OSPEEDR |= (3u << (11 * 2)) | (3u << (12 * 2));
    gpio_set_mode(GPIOA, 9, GPIO_MODE_INPUT);
    RCC->AHB2ENR |= RCC_AHB2ENR_OTGFSEN;

    tud_configure_dwc2_t cfg = CFG_TUD_CONFIGURE_DWC2_DEFAULT;
    cfg.vbus_sensing = true;      // Discovery 把 VBUS 接到了 PA9：插上电脑才上拉 D+
    tud_configure(0, TUD_CFGID_DWC2, &cfg);
    tusb_rhport_init_t init = {TUSB_ROLE_DEVICE, TUSB_SPEED_AUTO};
    return tusb_init(0, &init);
}

void board_led(bool on)
{
    on ? led_on(LED_ORANGE) : led_off(LED_ORANGE);
}

bool board_button()
{
    return GPIOA->IDR & 1u;
}

uint8_t board_id()
{
    return 1;
}

const char* board_name()
{
    return "STM32F4 Discovery";
}

const uint32_t* board_uid()
{
    return reinterpret_cast<const uint32_t*>(0x1FFF7A10u);   // 参考手册 “Device electronic signature” 一章
}

extern "C" void OTG_FS_IRQHandler()
{
    tud_int_handler(0);   // 中断里只记录事件，真正的处理在 tud_task()
}
