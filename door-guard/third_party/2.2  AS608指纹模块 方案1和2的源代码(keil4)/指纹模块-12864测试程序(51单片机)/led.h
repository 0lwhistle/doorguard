#ifndef _LED_H
#define _LED_H
#include<reg52.h>

sbit led_green=P0^0;
sbit led_yellow=P0^1;

void Led_Init(void);
void Led_Green_On(void);// 录入指示灯 
void Led_Green_Off(void);

void Led_Yellow_On(void);	 //	识别指示灯 
void Led_Yellow_Off(void);

#endif
