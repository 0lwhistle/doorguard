#ifndef _KEY_H_
#define _KEY_H_
#include<reg52.h>

sbit KEY_UP=P3^4;         //定义按键输入端口
sbit KEY_DOWN=P3^3;
sbit KEY_OK=P3^5;
sbit KEY_CANCEL=P3^2;

void Key_Init(void);

#endif
