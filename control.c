#include "control.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <stm32l1xx.h>
#include <FreeRTOS.h>
#include <FreeRTOS_CLI.h>
#include <task.h>
#include <semphr.h>
#include <queue.h>
#include <timers.h>
#include "messages.h"
#include "hpt_timer.h"
#include "ogn_lib.h"
#include "spirit1.h"
#include "options.h"
#include "console.h"
#include "gps.h"
#include "timer_const.h"


/* -------- defines -------- */
#define MAX_HPT_TABLE_LEN  16
/* -------- variables -------- */
HPT_Event hpt_table[MAX_HPT_TABLE_LEN];
TimerHandle_t xPowerDownTimer;
TimerHandle_t xCtrlTaskTimer;
/* data for jamming random packet */
uint8_t jam_packet[OGN_PKT_LEN];

/* pointer to TX packet data */
uint8_t* TX_pkt_data = NULL;

/* Console task queue */
xQueueHandle  control_queue;
/* -------- interrupt handlers -------- */

/* interrupt for raising GPS_PPS line */
void EXTI9_5_IRQHandler(void)
{
   BaseType_t xHigherPriorityTaskWoken = pdFALSE;
   
   if(EXTI_GetITStatus(EXTI_Line6) != RESET)
   {
      /* Clear the EXTI line 6 pending bit */
      EXTI_ClearITPendingBit(EXTI_Line6); 
      xHigherPriorityTaskWoken = HPT_RestartFromISR();      
   }
   portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
   
}

/* interrupt for falling Wakeup line */
void EXTI15_10_IRQHandler(void)
{
   BaseType_t xHigherPriorityTaskWoken = pdFALSE;
      
   if(EXTI_GetITStatus(EXTI_Line13) != RESET)
   {
      /* Clear the EXTI line 13 pending bit */
      EXTI_ClearITPendingBit(EXTI_Line13);  
      xTimerStartFromISR(xPowerDownTimer, &xHigherPriorityTaskWoken);     
   }
   portYIELD_FROM_ISR(xHigherPriorityTaskWoken); 
}

/* interrupt for rising B1 button line */
void EXTI2_IRQHandler(void)
{      
   if(EXTI_GetITStatus(EXTI_Line2) != RESET)
   {
      /* Clear the EXTI line 2 pending bit */
      EXTI_ClearITPendingBit(EXTI_Line2); 
      if (GPIO_ReadInputDataBit(GPIOC, GPIO_Pin_2) == Bit_SET)
      {
         /* turn off RX LED */
         GPIO_SetBits(GPIOB, GPIO_Pin_1);
      }
      else
      {
         /* turn on RX LED */
         GPIO_ResetBits(GPIOB, GPIO_Pin_1);
      }
   }
}

/* -------- functions -------- */
xQueueHandle* Get_ControlQueue()
{
   return &control_queue;
}

/**
* @brief  Initiates tracker shut down.
* @param  None
* @retval None
*/
void PreShutDownSequence(void)
{
    Console_Send("Shutdown...\r\n", 1);
    /* Disable SP1 by SHDN line */
    Spirit1EnterShutdown();
    /* Disable GPS using NMEA sentence */
    GPS_Off();
    /* Deactivate port C GPIO lines - entering shutdown generates glitches if active */
    GPIO_DeInit(GPIOC);
    
    /* There is no way to shut-down Independent Watchdog other than CPU reset */
    /* Shut-down will be finished after reset in main()/HandlePowerUpMode() function. */
    /* So we need to configure power-up mode so after reset CPU will shut itself down. */
    
    RTC_WriteBackupRegister(SHDN_REG_NUM, SHDN_MAGIC_NUM);
    vTaskDelay(500);
    NVIC_SystemReset();  
}

void vPwrDownTimerCallback(TimerHandle_t pxTimer)
{
    if (GPIO_ReadInputDataBit(GPIOC, GPIO_Pin_13) == Bit_SET)
    {
        PreShutDownSequence();
    }
}

void vCtrlTaskTimerCallback(TimerHandle_t pxTimer)
{
    xQueueHandle* sp1_task_queue = Get_SP1Queue();
    task_message sp1_msg;
    uint8_t jam_ratio = *(uint8_t *)GetOption(OPT_JAM_RATIO);

    uint8_t rnd_100 = rand()%100;
    if (rnd_100 < jam_ratio)
    {
        sp1_msg.msg_data   = 0;
        sp1_msg.msg_len    = 0;
        sp1_msg.msg_opcode = SP1_TX_PACKET;
        sp1_msg.src_id     = CONSOLE_USART_SRC_ID;
        /* TX packet */
        xQueueSend(*sp1_task_queue, &sp1_msg, portMAX_DELAY);
    }
    /* wait for another packet chance */
    xTimerStart(xCtrlTaskTimer, 0);
}

/**
* @brief  Configures the High Precision Timer Table for OGN oper. mode.
* @param  pointer to hpt_table data to be filled.
* @retval length of filled data.
*/
uint8_t Create_HPT_Table_OGN(HPT_Event* hpt_table_arr)
{
   const HPT_Event Table_OGN[] = 
   {   /* time,          event,             event data (optional) */
       { TIMER_MS(150),  HPT_COPY_PKT,      0   },  /* Copy packet to TX buffer (manchester encoding) */  
       { TIMER_MS(300),  HPT_SP1_CHANNEL,   4   },  /* Change channel to 868.4 */
       { TIMER_MS(400),  HPT_TX_PKT_LBT,    380 },  /* Start random transmit within next 380 ms */
       { TIMER_MS(800),  HPT_SP1_CHANNEL,   2   },  /* Change channel to 868.2 */
       { TIMER_MS(800),  HPT_TX_PKT_LBT,    380 },  /* Start random transmit within next 380 ms */
       { TIMER_MS(925),  HPT_IWDG_RELOAD,   0   },  /* Kick Independent Watchdog */
       { TIMER_MS(950),  HPT_PREPARE_PKT,   0   },  /* Prepare packet from GPS position */
       { TIMER_MS(1000), HPT_RESTART,       0   }   /* Restart table */ 
   }; 
   
   memcpy(hpt_table_arr, Table_OGN, sizeof(Table_OGN));
   return (sizeof(Table_OGN)/sizeof(HPT_Event));
}

/**
* @brief  Configures the High Precision Timer Table for Idle mode, 
* @brief  used also by CW oper. mode.
* @param  pointer to hpt_table data to be filled.
* @retval length of filled data.
*/
uint8_t Create_HPT_Table_Idle(HPT_Event* hpt_table_arr)
{
   const HPT_Event Table_Idle[] = 
   {   /* time,          event,             event data (optional) */ 
       { TIMER_MS(925),  HPT_IWDG_RELOAD,   0   },  /* Kick Independent Watchdog */
       { TIMER_MS(1000), HPT_RESTART,       0   }   /* Restart table */      
   };  
   
   memcpy(hpt_table_arr, Table_Idle, sizeof(Table_Idle));
   return (sizeof(Table_Idle)/sizeof(HPT_Event));
}

/**
* @brief  Configures the High Precision Timer Table for idle with freq. switch, 
* @brief  used by jammer oper. mode.
* @param  pointer to hpt_table data to be filled.
* @retval length of filled data.
*/
uint8_t Create_HPT_Table_Idle_Freq(HPT_Event* hpt_table_arr)
{
   const HPT_Event Table_Idle_Freq[] = 
   {   /* time,          event,             event data (optional) */
       { TIMER_MS(300),  HPT_SP1_CHANNEL,   4   },  /* Change channel to 868.4 */
       { TIMER_MS(800),  HPT_SP1_CHANNEL,   2   },  /* Change channel to 868.2 */
       { TIMER_MS(925),  HPT_IWDG_RELOAD,   0   },  /* Kick Independent Watchdog */
       { TIMER_MS(1000), HPT_RESTART,       0   }   /* Restart table */ 
   };
  
   memcpy(hpt_table_arr, Table_Idle_Freq, sizeof(Table_Idle_Freq));
   return (sizeof(Table_Idle_Freq)/sizeof(HPT_Event));  
}

/**
* @brief  Configures Independent Watchdog IWDG.
* @param  None
* @retval None
*/
void IWDG_Config(void)
{
    uint8_t iwdg_dis = *(uint8_t *)GetOption(OPT_IWDG);
    
    if (!iwdg_dis)
    {    
        /* Enable write access to IWDG_PR and IWDG_RLR registers */
        IWDG_WriteAccessCmd(IWDG_WriteAccess_Enable);
        /* IWDG Clock: 40kHz/64 */
        IWDG_SetPrescaler(IWDG_Prescaler_64);
        /* Configure the IWDG counter value - 6 second at /64 prescaler*/
        IWDG_SetReload(0xfff);
        /* Reload Counter */
        IWDG_ReloadCounter();
        /* Enable IWDG */
        IWDG_Enable();
    }
}

/**
* @brief  Configures the Control Task Peripherals.
* @param  None
* @retval None
*/
void Control_Config(void)
{
   GPIO_InitTypeDef GPIO_InitStructure;
   EXTI_InitTypeDef EXTI_InitStructure;
   NVIC_InitTypeDef NVIC_InitStructure;

   /* Power button should be pressed for 1 second to power off, xPowerDownTimer is used for counting this time */
   /* when power button is pressed timer is restarted */
   xPowerDownTimer = xTimerCreate(
      "PDTimer",
      /* The timer period in ms. */
      TIMER_MS(1000),
      /* The timer will stop when expire. */
      pdFALSE,
      /* unique id */
      ( void * )PWR_DOWN_TIMER_ID,
      /* Each timer calls the same callback when it expires. */
      vPwrDownTimerCallback
    );
    
    xCtrlTaskTimer = xTimerCreate(
      "CtrlTaskTimer",
      /* The timer period in ms. */
      TIMER_MS(1000),
      /* The timer will stop when expire. */
      pdFALSE,
      /* unique id */
      ( void * )CTRL_TASK_TIMER_ID,
      /* Each timer calls the same callback when it expires. */
      vCtrlTaskTimerCallback
    );
    
   /* Configure PC6 Pin (GPS_PPS) as GPIO interrupt */
   RCC_AHBPeriphClockCmd(RCC_AHBPeriph_GPIOC, ENABLE);  
   /* Enable SYSCFG clock */
   RCC_APB2PeriphClockCmd(RCC_APB2Periph_SYSCFG, ENABLE);
   
   GPIO_InitStructure.GPIO_Mode  = GPIO_Mode_IN;
   GPIO_InitStructure.GPIO_OType = GPIO_OType_PP;
   GPIO_InitStructure.GPIO_PuPd  = GPIO_PuPd_NOPULL;
   GPIO_InitStructure.GPIO_Speed = GPIO_Speed_2MHz;
   GPIO_Init(GPIOC, &GPIO_InitStructure);

   SYSCFG_EXTILineConfig(EXTI_PortSourceGPIOC, EXTI_PinSource6); 

   EXTI_InitStructure.EXTI_Line    = EXTI_Line6;
   EXTI_InitStructure.EXTI_Mode    = EXTI_Mode_Interrupt;
   EXTI_InitStructure.EXTI_Trigger = EXTI_Trigger_Rising;
   EXTI_InitStructure.EXTI_LineCmd = ENABLE;
   EXTI_Init(&EXTI_InitStructure);
   
   /* Configure PC13 Pin (Wakeup) as GPIO interrupt */  
   GPIO_InitStructure.GPIO_Mode  = GPIO_Mode_IN;
   GPIO_InitStructure.GPIO_OType = GPIO_OType_PP;
   GPIO_InitStructure.GPIO_PuPd  = GPIO_PuPd_NOPULL;
   GPIO_InitStructure.GPIO_Speed = GPIO_Speed_2MHz;
   GPIO_InitStructure.GPIO_Pin   = GPIO_Pin_13;
   GPIO_Init(GPIOC, &GPIO_InitStructure);

   SYSCFG_EXTILineConfig(EXTI_PortSourceGPIOC, EXTI_PinSource13); 

   EXTI_InitStructure.EXTI_Line    = EXTI_Line13;
   EXTI_InitStructure.EXTI_Mode    = EXTI_Mode_Interrupt;
   EXTI_InitStructure.EXTI_Trigger = EXTI_Trigger_Rising;
   EXTI_InitStructure.EXTI_LineCmd = ENABLE;
   EXTI_Init(&EXTI_InitStructure);
   
   /* enable Power Button input lines interrupt */
   NVIC_InitStructure.NVIC_IRQChannel = EXTI15_10_IRQn;
   NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority = configPWR_BTN_INTERRUPT_PRIORITY;
   NVIC_InitStructure.NVIC_IRQChannelSubPriority = 0;
   NVIC_InitStructure.NVIC_IRQChannelCmd = ENABLE;
   NVIC_Init(&NVIC_InitStructure);
  
   /* Configure PC2 Pin (Button B1) as GPIO interrupt */  
   GPIO_InitStructure.GPIO_Mode  = GPIO_Mode_IN;
   GPIO_InitStructure.GPIO_OType = GPIO_OType_PP;
   GPIO_InitStructure.GPIO_PuPd  = GPIO_PuPd_NOPULL;
   GPIO_InitStructure.GPIO_Speed = GPIO_Speed_2MHz;
   GPIO_InitStructure.GPIO_Pin   = GPIO_Pin_2;
   GPIO_Init(GPIOC, &GPIO_InitStructure);

   SYSCFG_EXTILineConfig(EXTI_PortSourceGPIOC, EXTI_PinSource2); 

   EXTI_InitStructure.EXTI_Line    = EXTI_Line2;
   EXTI_InitStructure.EXTI_Mode    = EXTI_Mode_Interrupt;
   EXTI_InitStructure.EXTI_Trigger = EXTI_Trigger_Rising_Falling;
   EXTI_InitStructure.EXTI_LineCmd = ENABLE;
   EXTI_Init(&EXTI_InitStructure);
   
   /* enable B1 Button input lines interrupt */
   NVIC_InitStructure.NVIC_IRQChannel = EXTI2_IRQn;
   NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority = configPWR_BTN_INTERRUPT_PRIORITY;
   NVIC_InitStructure.NVIC_IRQChannelSubPriority = 0;
   NVIC_InitStructure.NVIC_IRQChannelCmd = ENABLE;
   NVIC_Init(&NVIC_InitStructure);

   /* line PC4 (TRK_EN#) is used for controlling power of devices that are unable to perform software shut-down */
   /* TRK_EN# should be put low when tracker is activated, and Hi-Z after shut-down */
   
   RCC_AHBPeriphClockCmd(RCC_AHBPeriph_GPIOC, ENABLE);

   GPIO_InitStructure.GPIO_Mode  = GPIO_Mode_OUT;
   GPIO_InitStructure.GPIO_OType = GPIO_OType_PP;
   GPIO_InitStructure.GPIO_PuPd  = GPIO_PuPd_NOPULL;
   GPIO_InitStructure.GPIO_Speed = GPIO_Speed_2MHz;
   GPIO_InitStructure.GPIO_Pin   = GPIO_Pin_4;
   GPIO_Init(GPIOC, &GPIO_InitStructure);
   
   GPIO_ResetBits(GPIOC, GPIO_Pin_4);
   
   /* line PC8 (GPS_ANT_SW) is used for controlling GPS external antenna status */
   GPIO_InitStructure.GPIO_Pin   = GPIO_Pin_8;
   GPIO_Init(GPIOC, &GPIO_InitStructure);
   
   /* Control GPS antenna type used */
   uint8_t gps_ant = *(uint8_t *)GetOption(OPT_GPS_ANT);
   if (gps_ant)
   {
      /* External antenna */
      GPIO_SetBits(GPIOC, GPIO_Pin_8);
   }
   else
   {
      /* Internal antenna */
      GPIO_ResetBits(GPIOC, GPIO_Pin_8);
   }   
   
   /* Enable the GPIOC Clock */
   RCC_AHBPeriphClockCmd(RCC_AHBPeriph_GPIOC, ENABLE);
   /* Configure PC.0 (ADC Channel10) in analog mode */
   GPIO_InitStructure.GPIO_Pin   =  GPIO_Pin_0;
   GPIO_InitStructure.GPIO_Mode  = GPIO_Mode_AN;
   GPIO_InitStructure.GPIO_PuPd  = GPIO_PuPd_NOPULL;
   GPIO_Init(GPIOC, &GPIO_InitStructure);
    
   IWDG_Config();
   HPT_Config();

}

/**
* @brief  Executes necessary action before entering event loop.
* @param  mode - selected mode.
* @retval None
*/
void StartMode(oper_modes mode)
{
    task_message sp1_msg;
    xQueueHandle* sp1_task_queue;
    uint8_t i;
    
    switch(mode)
    {
        case MODE_CW:
            /* wait for Spirit1 task */
            vTaskDelay(1000);
            sp1_task_queue = Get_SP1Queue();
            if (sp1_task_queue)
            {
                /* Start CW */
                sp1_msg.msg_data   = 0;
                sp1_msg.msg_len    = 0;
                sp1_msg.msg_opcode = SP1_START_CW;
                sp1_msg.src_id     = CONTROL_SRC_ID;
                xQueueSend(*sp1_task_queue, &sp1_msg, portMAX_DELAY);
            }
            break;
            
         case MODE_RX:
            /* wait for Spirit1 task */
            vTaskDelay(1000);
            sp1_task_queue = Get_SP1Queue();
            if (sp1_task_queue)
            {
                /* Start RX */
                sp1_msg.msg_data   = 0;
                sp1_msg.msg_len    = 0;
                sp1_msg.msg_opcode = SP1_START_RX;
                sp1_msg.src_id     = CONTROL_SRC_ID;
                xQueueSend(*sp1_task_queue, &sp1_msg, portMAX_DELAY);
            }
            break;
            
         case MODE_JAMMER:
            vTaskDelay(1000);
            for (i=0;i<OGN_PKT_LEN;i++) jam_packet[i] = (uint8_t)rand();
            sp1_task_queue = Get_SP1Queue();

            sp1_msg.msg_data   = (uint32_t)&jam_packet;
            sp1_msg.msg_len    = OGN_PKT_LEN;
            sp1_msg.msg_opcode = SP1_COPY_OGN_PKT;
            sp1_msg.src_id     = CONTROL_SRC_ID;
            /* Send packet data to Spirit1 task */
            xQueueSend(*sp1_task_queue, &sp1_msg, portMAX_DELAY);
            /* Try to send packet every 2 ms */
            xTimerChangePeriod(xCtrlTaskTimer, TIMER_MS(2), 0);
            xTimerStart(xCtrlTaskTimer, 0);
            break;
        
        default:
            /* do nothing for other modes */
            break;
    }
}

/* borrowed from commands.c */
static uint8_t print_hex_val(uint8_t data, char* dest)
{
   char t;
   t = data>>4;
   dest[0] = t > 9 ? t+'A'-0x0A : t+'0';
   t = data&0x0F;
   dest[1] = t > 9 ? t+'A'-0x0A : t+'0';
   return 2;
}


void Print_packet(rcv_packet_str* packet)
{
    char buffer[80];
    int i, Neg=0;

    float rssi = packet->rssi;
    int lqi = packet->lqi;
    int pqi = packet->pqi;
    int sqi = packet->sqi;

    if(rssi<0) { Neg=1; rssi=(-rssi); }
    int Int = (int)floor(rssi);
    int Frac = (int)floor((rssi-Int)*10);
    if(Neg) Int=(-Int);
    sprintf(buffer, "Packet received: RSSI: %+d.%ddBm, LQI: %d, PQI: %d, SQI: %d\r\n", Int, Frac, lqi, pqi, sqi);
    Console_Send(buffer, 1);

    int ctr=0;
    for (i=0; i < OGN_PKT_LEN; i++)
    {
       ctr+= print_hex_val(packet->data_ptr[i], &buffer[ctr]);
    }
    buffer[ctr++] = '\r'; buffer[ctr++] = '\n';
    buffer[ctr++] = '\0';
    Console_Send(buffer, 1);

    ctr=0;
    for (i=0; i < OGN_PKT_LEN; i++)
    {
       ctr+= print_hex_val(packet->err_ptr[i], &buffer[ctr]);
    }
    buffer[ctr++] = '\r'; buffer[ctr++] = '\n';
    buffer[ctr++] = '\0';
    Console_Send(buffer, 1);

}

/**
* @brief  Handle messages received from Spirit1 task.
* @param  Message structure.
* @retval None
*/
static void Handle_sp1_msgs(task_message* msg)
{
    switch (msg->msg_opcode)
    {
        case SP1_OUT_PKT_READY:
            Print_packet((rcv_packet_str*)msg->msg_data);
            break;
            
        default:
            break;
    }
}

/**
* @brief  Handle messages received from High Precision Timer task.
* @param  Message structure.
* @retval None
*/
static void Handle_hpt_msgs(task_message* msg)
{
    task_message sp1_msg;
    
    switch (msg->msg_opcode)
    {
        case HPT_PREPARE_PKT:
            TX_pkt_data = OGN_PreparePacket();
            break;

        case HPT_COPY_PKT:
            sp1_msg.msg_data   = (uint32_t)TX_pkt_data; /* null allowed - packet data will be cleared */
            sp1_msg.msg_len    = OGN_PKT_LEN;
            sp1_msg.msg_opcode = SP1_COPY_OGN_PKT;
            sp1_msg.src_id     = CONTROL_SRC_ID;
            xQueueHandle* sp1_task_queue = Get_SP1Queue();
            xQueueSend(*sp1_task_queue, &sp1_msg, portMAX_DELAY);
            break;
            
        default:
            break;
     }    
}

/**
* @brief  Main Control Task.
* @param  None
* @retval None
*/
void vTaskControl(void* pvParameters)
{
   NVIC_InitTypeDef NVIC_InitStructure;
   task_message msg;
   oper_modes oper_mode;
   
   control_queue = xQueueCreate(10, sizeof(task_message));
      
   /* Select timer table depending on operation mode */
   oper_mode = *(uint8_t *)GetOption(OPT_OPER_MODE); 
   switch (oper_mode)
   {
        case MODE_OGN:      
            Create_HPT_Table_OGN(hpt_table);
            break;
        case MODE_IDLE:
        case MODE_CW: 
        case MODE_RX:          
            Create_HPT_Table_Idle(hpt_table);
            break;
        case MODE_JAMMER:          
            Create_HPT_Table_Idle_Freq(hpt_table);
            break;
    
        default:
            /* in case of error - fall to OGN mode */         
            Create_HPT_Table_OGN(hpt_table);
            break;
   } 
   HPT_Start(hpt_table);

   /* enable GPS PPS input lines interrupts */
   NVIC_InitStructure.NVIC_IRQChannel = EXTI9_5_IRQn;
   NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority = configGPS_PPS_INTERRUPT_PRIORITY;
   NVIC_InitStructure.NVIC_IRQChannelSubPriority = 0;
   NVIC_InitStructure.NVIC_IRQChannelCmd = ENABLE;
   NVIC_Init(&NVIC_InitStructure);
      
   StartMode(oper_mode);
   
   for(;;)
   {
        xQueueReceive(control_queue, &msg, portMAX_DELAY);
        switch (msg.src_id)
        {
            case HPT_SRC_ID:
                Handle_hpt_msgs(&msg);
                break;
            
            case SPIRIT1_SRC_ID:
                Handle_sp1_msgs(&msg);
                break;
                
            default:
                break;
      }
   }
}
