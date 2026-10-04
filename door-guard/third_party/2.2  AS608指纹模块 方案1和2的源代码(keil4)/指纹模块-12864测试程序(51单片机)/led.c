#include "led.h"
#include "delay.h"

void Led_Init(void)
{
	led_green=1;
	led_yellow=1;
}
void Led_Green_On(void)
{
	 led_green=0;
}
void Led_Green_Off(void)
{
	 led_green=1;
}

void Led_Yellow_On(void)
{
		led_yellow=0;
}
void Led_Yellow_Off(void)
{
		led_yellow=1;
}
