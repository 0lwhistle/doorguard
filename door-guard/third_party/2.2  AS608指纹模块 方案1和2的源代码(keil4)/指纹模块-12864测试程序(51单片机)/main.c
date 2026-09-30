#include<reg52.h>
#include<intrins.h>
#include"led.h"
#include"buzz.h"
#include"key.h"
#include"uart.h"
#include"delay.h"
#include"FPM10A.h"
#include"lcd_12864.h"


void main()
{	
	Init_ST7920();   //初始化
  CGRAM();     //写入自定义字符 
  Uart_Init();
	Key_Init();
	Led_Init();
	Buzz_Init();
	Device_Check();
	DisplayUpdata();
	Delay_Ms(500); //延时500MS，等待指纹模块复位
	Buzz_Times(1);

	while(1)
	{
		if(local_date!=base_date) //光标位置变化，则更新显示 
		{
	    DisplayUpdata();
			base_date=local_date;
		}

		if(KEY_OK == 0)
			{	 
		 	 while(KEY_OK == 0);//等待松开按键								
			 switch(local_date)
				{
					case 0:	 //添加指纹
					Led_Green_On();
					FPM10A_Add_Fingerprint();
					Led_Green_Off();
					local_date = 0;
					DisplayUpdata();	
					break; 
												
					case 1://搜索指纹
					Led_Yellow_On();						
					FPM10A_Find_Fingerprint();
					Led_Yellow_Off();							
					local_date = 1;
					DisplayUpdata();																	
					break;									
					
					case 2:	//清空指纹
					FPM10A_Delete_All_Fingerprint();
					local_date = 2;									
					DisplayUpdata();
		  		break;
				}
			}
		  //下
			if(KEY_DOWN == 0)
			{
			 while(KEY_DOWN == 0); //等待松开按键				
	  	 if(local_date<=2)
				{
					local_date++;
					if(local_date==3)
						local_date=0;						
				}				
			}			
		  //上
			if(KEY_UP == 0)
			{ 
			  while(KEY_UP == 0); //等待松开按键
		    if(local_date>=0)
				{
					local_date--;
					if(local_date==-1)
						local_date=2;	
				}					
			}			
			Delay_Ms(100); //延时判断100MS检测一次		
	}
}
