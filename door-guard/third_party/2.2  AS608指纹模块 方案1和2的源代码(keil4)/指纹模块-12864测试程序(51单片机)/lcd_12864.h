#ifndef __LCD__
#define __LCD__

#define DataPort P2

extern char local_date,base_date;
extern unsigned char code *Main_Menu[];

sbit RS = P0^2;
sbit RW = P0^3;
sbit E  = P0^4;
sbit PSB   = P0^7;
sbit RES   = P0^5;

void DisplayUpdata(void);
void ClrScreen();
void LCD_PutString(unsigned char x,unsigned char y,unsigned char code *s);
void DisplayCGRAM(unsigned char x,unsigned char y);
void CGRAM();
void Init_ST7920();
void Write_Data(unsigned char Data);
void Write_Cmd(unsigned char Cmd);
void Check_Busy();

#endif
