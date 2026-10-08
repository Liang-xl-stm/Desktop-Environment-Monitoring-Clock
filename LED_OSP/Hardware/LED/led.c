#include "led.h"
#include "stm32f4xx_gpio.h"
#include "stm32f4xx_rcc.h"

void LED_Init(void)
{
    GPIO_InitTypeDef gpio;
    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOF, ENABLE);
    GPIO_SetBits(GPIOF, GPIO_Pin_9);
    GPIO_StructInit(&gpio);
    gpio.GPIO_Pin = GPIO_Pin_9;
    gpio.GPIO_Mode = GPIO_Mode_OUT;
    gpio.GPIO_OType = GPIO_OType_PP;
    gpio.GPIO_Speed = GPIO_Speed_2MHz;
    gpio.GPIO_PuPd = GPIO_PuPd_NOPULL;
    GPIO_Init(GPIOF, &gpio);
}

void LED_Set(uint8_t on)
{
    if (on) GPIO_ResetBits(GPIOF, GPIO_Pin_9);
    else GPIO_SetBits(GPIOF, GPIO_Pin_9);
}

void LED_Toggle(void)
{
    GPIO_ToggleBits(GPIOF, GPIO_Pin_9);
}
