#include"FPM10A.h"
#include<reg52.h>
#include"key.h"
#include"uart.h"
#include"lcd_12864.h"
#include"delay.h"
#include"buzz.h"
#include"led.h"

volatile unsigned char FPM10A_RECEICE_BUFFER[32];
unsigned int finger_id = 0;

//FINGERPRINT通信协议定义

code unsigned char FPM10A_Get_Device[10] ={0x01,0x00,0x07,0x13,0x00,0x00,0x00,0x00,0x00,0x1b};//口令验证
code unsigned char FPM10A_Pack_Head[6] = {0xEF,0x01,0xFF,0xFF,0xFF,0xFF};  //协议包头
code unsigned char FPM10A_Get_Img[6] = {0x01,0x00,0x03,0x01,0x00,0x05};    //获得指纹图像
code unsigned char FPM10A_Get_Templete_Count[6] ={0x01,0x00,0x03,0x1D,0x00,0x21 }; //获得模版总数
code unsigned char FPM10A_Search[11]={0x01,0x00,0x08,0x04,0x01,0x00,0x00,0x03,0xE7,0x00,0xF8}; //搜索指纹搜索范围0 - 999,使用BUFFER1中的特征码搜索
code unsigned char FPM10A_Search_0_9[11]={0x01,0x00,0x08,0x04,0x01,0x00,0x00,0x00,0x13,0x00,0x21}; //搜索0-9号指纹
code unsigned char FPM10A_Img_To_Buffer1[7]={0x01,0x00,0x04,0x02,0x01,0x00,0x08}; //将图像放入到BUFFER1
code unsigned char FPM10A_Img_To_Buffer2[7]={0x01,0x00,0x04,0x02,0x02,0x00,0x09}; //将图像放入到BUFFER2
code unsigned char FPM10A_Reg_Model[6]={0x01,0x00,0x03,0x05,0x00,0x09}; //将BUFFER1跟BUFFER2合成特征模版
code unsigned char FPM10A_Delete_All_Model[6]={0x01,0x00,0x03,0x0d,0x00,0x11};//删除指纹模块里所有的模版
volatile unsigned char  FPM10A_Save_Finger[9]={0x01,0x00,0x06,0x06,0x01,0x00,0x0B,0x00,0x19};//将BUFFER1中的特征码存放到指定的位置
//volatile:系统总是重新从它所在的内存读取数据，即使它前面的指令刚刚从该处读取过数据
/*------------------ FINGERPRINT命令字 --------------------------*/
 //发送包头
void FPM10A_Cmd_Send_Pack_Head(void)
{
	int i;	
	for(i=0;i<6;i++) //包头
   {
     Uart_Send_Byte(FPM10A_Pack_Head[i]);   
    }		
}
//发送指令
void FPM10A_Cmd_Check(void)
{
	int i=0;
	FPM10A_Cmd_Send_Pack_Head(); //发送通信协议包头
	for(i=0;i<10;i++)
	 {		
		Uart_Send_Byte(FPM10A_Get_Device[i]);
	  }
}
//接收反馈数据缓冲
void FPM10A_Receive_Data(unsigned char ucLength)
{
  unsigned char i;

  for (i=0;i<ucLength;i++)
     FPM10A_RECEICE_BUFFER[i] = Uart_Receive_Byte();

}

//FINGERPRINT_获得指纹图像命令
void FPM10A_Cmd_Get_Img(void)
{
    unsigned char i;
    FPM10A_Cmd_Send_Pack_Head(); //发送通信协议包头
    for(i=0;i<6;i++) //发送命令 0x1d
	{
       Uart_Send_Byte(FPM10A_Get_Img[i]);
	}
}
//讲图像转换成特征码存放在Buffer1中
void FINGERPRINT_Cmd_Img_To_Buffer1(void)
{
 	unsigned char i;
	FPM10A_Cmd_Send_Pack_Head(); //发送通信协议包头      
   	for(i=0;i<7;i++)   //发送命令 将图像转换成 特征码 存放在 CHAR_buffer1
     {
      Uart_Send_Byte(FPM10A_Img_To_Buffer1[i]);
   	  }
}
//将图像转换成特征码存放在Buffer2中
void FINGERPRINT_Cmd_Img_To_Buffer2(void)
{
     unsigned char i;
     for(i=0;i<6;i++)    //发送包头
	 {
    	Uart_Send_Byte(FPM10A_Pack_Head[i]);   
   	 }
     for(i=0;i<7;i++)   //发送命令 将图像转换成 特征码 存放在 CHAR_buffer1
      {
      	Uart_Send_Byte(FPM10A_Img_To_Buffer2[i]);
   	  }
}
//搜索全部用户999枚
void FPM10A_Cmd_Search_Finger(void)
{
       unsigned char i;	   	    
			 FPM10A_Cmd_Send_Pack_Head(); //发送通信协议包头
       for(i=0;i<11;i++)
           {
    	      Uart_Send_Byte(FPM10A_Search[i]);   
   		   }
}

void FPM10A_Cmd_Reg_Model(void)
{
       unsigned char i;	   
	    
			 FPM10A_Cmd_Send_Pack_Head(); //发送通信协议包头

       for(i=0;i<6;i++)
           {
    	      Uart_Send_Byte(FPM10A_Reg_Model[i]);   
   		   }


}
//删除指纹模块里的所有指纹模版
void FINGERPRINT_Cmd_Delete_All_Model(void)
{
     unsigned char i;    
    for(i=0;i<6;i++) //包头
      Uart_Send_Byte(FPM10A_Pack_Head[i]);   
    for(i=0;i<6;i++) //命令合并指纹模版
	   {
      Uart_Send_Byte(FPM10A_Delete_All_Model[i]);   
		 }	
}
//保存指纹
void FPM10A_Cmd_Save_Finger( unsigned int storeID )
{
       unsigned long temp = 0;
		   unsigned char i;
       FPM10A_Save_Finger[5] =(storeID&0xFF00)>>8;
       FPM10A_Save_Finger[6] = (storeID&0x00FF);
		   for(i=0;i<7;i++)   //计算校验和
		   	   temp = temp + FPM10A_Save_Finger[i]; 
		   FPM10A_Save_Finger[7]=(temp & 0x00FF00) >> 8; //存放校验数据
		   FPM10A_Save_Finger[8]= temp & 0x0000FF;		   
       FPM10A_Cmd_Send_Pack_Head(); //发送通信协议包头	
       for(i=0;i<9;i++)  
      		Uart_Send_Byte(FPM10A_Save_Finger[i]);      //发送命令 将图像转换成 特征码 存放在 CHAR_buffer1
}
//添加指纹
void FPM10A_Add_Fingerprint()
{
	unsigned char id_show[]={0,0,0};
	ClrScreen(); //清空显示屏  
	while(1)
	{
	 LCD_PutString(0,1,"请输入指纹ID:");
	 LCD_PutString(0,2,"上：+ 下：-");
	 //按返回键直接回到主菜单
	 if(KEY_CANCEL == 0) 
		{
		 while(KEY_CANCEL==0);
		 break;
		 }
	 //按上键finger值加一 
	 if(KEY_UP==0)
		{
			 while(KEY_UP==0);
		   if(finger_id == 999)
			   {
				  finger_id = 0;
				 }
				else
				 {
					finger_id = finger_id + 1;
				 };
		}
		//按上键finger值减一
	  if(KEY_DOWN == 0)
		 {
				 while(KEY_DOWN==0);
				 if(finger_id == 0)
					 {
						finger_id = 999;
					  }
					else
						finger_id = finger_id - 1;
			}
	 //finger值显示处理 
	 id_show[0]=finger_id/100;
	 id_show[1]=finger_id%100/10;
	 id_show[2]=finger_id%100%10;
 	 LCD_PutString(0,3,"ID:");
	 Write_Cmd(0x88+4);
	 Write_Data(0x30+id_show[0]);
	 Write_Cmd(0x88+5);
	 Write_Data(0x30+id_show[1]);
	 Write_Cmd(0x88+6);
	 Write_Data(0x30+id_show[2]);
	 //按确认键开始录入指纹信息 		 			
	 if(KEY_OK == 0)
	  {	
			  while(KEY_OK==0);
			  ClrScreen(); //清空显示屏
			  LCD_PutString(0,1,"请输入指纹");
			  do
		   {
			  //按下返回键退出录入返回fingerID调整状态   
				if(KEY_CANCEL == 0) 
				 {
				  while(KEY_CANCEL==0);
				  break;
				  }
				FPM10A_Cmd_Get_Img(); //获得指纹图像
				FPM10A_Receive_Data(12);
				//判断接收到的确认码,等于0指纹获取成功
				if(FPM10A_RECEICE_BUFFER[9]==0)
				 {
					Delay_Ms(100);
					FINGERPRINT_Cmd_Img_To_Buffer1();
				  FPM10A_Receive_Data(12);
					LCD_PutString(0,2,"输入成功");
					Buzz_Times(1);
					Delay_Ms(500);
					LCD_PutString(0,3,"请确认，输入指纹");
					do
					{
					 FPM10A_Cmd_Get_Img(); //获得指纹图像
					 FPM10A_Receive_Data(12);
					//判断接收到的确认码,等于0指纹获取成功
					if(FPM10A_RECEICE_BUFFER[9]==0)
					 {
						Delay_Ms(100);
						FINGERPRINT_Cmd_Img_To_Buffer2();
				  	FPM10A_Receive_Data(12);
						FPM10A_Cmd_Reg_Model();//转换成特征码
	          FPM10A_Receive_Data(12); 
					  FPM10A_Cmd_Save_Finger(finger_id++);                		         
	          FPM10A_Receive_Data(12);
						LCD_PutString(0,4,"指纹录入成功");
						Buzz_Times(1);
						Delay_Ms(1000);
				    break;
				  	}
				   }while(KEY_CANCEL == 1);
	        break;
					}
				}while(KEY_CANCEL == 1);
			ClrScreen(); //清空显示屏
			}
		//	Delay_Ms(500);
		}
}

//搜索指纹
void FPM10A_Find_Fingerprint()
{
	 unsigned int find_fingerid = 0;
	 unsigned char id_show[]={0,0,0};
   ClrScreen(); //清空显示屏
	 LCD_PutString(0,1,"请输入指纹 ");
	 do
	{
		FPM10A_Cmd_Get_Img(); //获得指纹图像
		FPM10A_Receive_Data(12);		
		//判断接收到的确认码,等于0指纹获取成功
		if(FPM10A_RECEICE_BUFFER[9]==0)
		{			
			  Delay_Ms(100);
				FINGERPRINT_Cmd_Img_To_Buffer1();
			  FPM10A_Receive_Data(12);		
			  FPM10A_Cmd_Search_Finger();
				FPM10A_Receive_Data(16);			
			  if(FPM10A_RECEICE_BUFFER[9] == 0) //搜索到  
				{
					ClrScreen();
					LCD_PutString(0,1,"请输入指纹 ");
				  LCD_PutString(0,2,"搜索成功");
					Buzz_Times(1);					
					//拼接指纹ID数
					find_fingerid = FPM10A_RECEICE_BUFFER[10]*256 + FPM10A_RECEICE_BUFFER[11];					
					id_show[0]=find_fingerid/100;
					id_show[1]=find_fingerid%100/10;
				  id_show[2]=find_fingerid%100%10;
				 	LCD_PutString(0,3,"ID:");
					Write_Cmd(0x88+4);
					Write_Data(0x30+id_show[0]);
					Write_Cmd(0x88+5);
					Write_Data(0x30+id_show[1]);
					Write_Cmd(0x88+6);
					Write_Data(0x30+id_show[2]);						
				  Delay_Ms(2000);				
			   }
				else //没有找到
				{
				 ClrScreen();
				 LCD_PutString(0,1,"请输入指纹 ");
				 LCD_PutString(0,2,"未找到指纹信息 ");
				 Buzz_Times(3);
				}
			}		
		}while(KEY_CANCEL == 1);
}
//删除所有存贮的指纹库
void FPM10A_Delete_All_Fingerprint()
{
		unsigned char i=0;
		ClrScreen(); //清空显示屏
		LCD_PutString(1,1,"清空指纹库？");
		do
		 {
			if(KEY_OK==0)
			{
				while(KEY_OK==0);
				LCD_PutString(0,2,"清空中");
				Delay_Ms(300);
				for(i=0;i<8;i++)
				 {
					LCD_PutString(i,3,"*");
					Delay_Ms(200);
				 }
				FINGERPRINT_Cmd_Delete_All_Model();
			  FPM10A_Receive_Data(12); 
				LCD_PutString(2,4,"清空成功");
				Buzz_Times(3);
				break;
			}
		 }while(KEY_CANCEL==1);
}

void Device_Check(void)
{
		unsigned char i=0;
		FPM10A_RECEICE_BUFFER[9]=1;
		ClrScreen(); //清空显示屏
		LCD_PutString(1,1,"设备加载中");
		Delay_Ms(500);
		for(i=0;i<8;i++)
		{
			LCD_PutString(i,2,"*");
			Delay_Ms(300);
		}
		LCD_PutString(0,3,"设备加载失败");
		FPM10A_Cmd_Check();
		FPM10A_Receive_Data(12);
// 		if(FPM10A_RECEICE_BUFFER[9] == 0)
		if(1)
		{
			LCD_PutString(0,3,"设备加载成功");
			Buzz_Times(1);
			Delay_Ms(1000);
			ClrScreen();
			LCD_PutString(2,1,"欢迎使用");
			LCD_PutString(1,2,"指纹识别系统");
			Delay_Ms(1000);
			LCD_PutString(1,3,"系统初始化中");
			Delay_Ms(500);
			for(i=0;i<8;i++)
		{
			LCD_PutString(i,4,"*");
			Delay_Ms(300);
		}
		Buzz_Times(3);
		Delay_Ms(1000);
		}
}





