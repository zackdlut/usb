// 立创·地阔星 STM32F103C8T6：USB 接在 PA11 = D-、PA12 = D+。
//
// F103 的 USB 控制器内部没有 D+ 上拉电阻，开发板在 PA12 上焊了一个 1.5 kΩ
// 到 3.3 V 的上拉（以你手上板子的原理图为准）。上拉是固定的，所以复位后
// 主机可能还以为“老设备”在线；启动时先把 PA12 拉低 10 ms，假装拔掉一次，
// 主机就会重新枚举。
#include "board.h"
#include "board_usb.h"
#include "tusb.h"

extern "C" {   // stm32 教程里的驱动是 C 写的
#include "tick.h"
#include "uart.h"
}

static constexpr uint32_t kSysclkHz = 72000000u;

// HSE 8 MHz --PLL x9--> 72 MHz 系统时钟
// USB 时钟 = PLL / 1.5 = 48 MHz（RCC_CFGR 的 USBPRE 位保持 0 就是 /1.5）
// 所以 F103 想用 USB，系统时钟只能是 72 MHz 或 48 MHz。
static void clock_init_72mhz()
{
    RCC->CR |= RCC_CR_HSEON;
    while (!(RCC->CR & RCC_CR_HSERDY)) {
    }
    FLASH->ACR = FLASH_ACR_PRFTBE | FLASH_ACR_LATENCY_2;   // 72 MHz 需要 2 个等待周期
    RCC->CFGR = RCC_CFGR_PLLSRC | RCC_CFGR_PLLMULL9 | RCC_CFGR_PPRE1_DIV2;   // APB1 最高 36 MHz
    RCC->CR |= RCC_CR_PLLON;
    while (!(RCC->CR & RCC_CR_PLLRDY)) {
    }
    RCC->CFGR |= RCC_CFGR_SW_PLL;
    while ((RCC->CFGR & RCC_CFGR_SWS) != RCC_CFGR_SWS_PLL) {
    }
    SystemCoreClock = kSysclkHz;
}

void board_init()
{
    clock_init_72mhz();
    board_init_led();
    RCC->APB2ENR |= RCC_APB2ENR_IOPAEN;
    gpio_config(GPIOA, 0, GPIO_CFG_IN_PULL);   // PA0 外接按键到 3.3 V，内部下拉
    GPIOA->BRR = 1u;
    uart_init(kSysclkHz, 115200);               // USART1 在 APB2（72 MHz）：PA9 TX
    tick_init(kSysclkHz);
}

bool board_usb_start()
{
    // 假装拔掉：D+ 拉低 10 ms
    gpio_config(GPIOA, 12, GPIO_CFG_OUT_PP_2MHZ);
    GPIOA->BRR = 1u << 12;
    delay_ms(10);
    gpio_config(GPIOA, 12, GPIO_CFG_IN_FLOATING);   // 交还给 USB 控制器

    RCC->APB1ENR |= RCC_APB1ENR_USBEN;
    tusb_rhport_init_t init = {TUSB_ROLE_DEVICE, TUSB_SPEED_AUTO};
    return tusb_init(0, &init);
}

void board_led(bool on)
{
    on ? led_on() : led_off();
}

bool board_button()
{
    return GPIOA->IDR & 1u;
}

uint8_t board_id()
{
    return 2;
}

const char* board_name()
{
    return "LCSC DKX STM32F103";
}

const uint32_t* board_uid()
{
    return reinterpret_cast<const uint32_t*>(0x1FFFF7E8u);   // 参考手册 “Device electronic signature” 一章
}

// F103 的 USB 和 CAN 共用中断向量
extern "C" void USB_HP_CAN1_TX_IRQHandler()
{
    tud_int_handler(0);
}

extern "C" void USB_LP_CAN1_RX0_IRQHandler()
{
    tud_int_handler(0);
}
