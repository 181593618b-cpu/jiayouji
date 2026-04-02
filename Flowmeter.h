#ifndef __FLOWMETER_H__
#define __FLOWMETER_H__

#include <stdint.h>
#include "gd32f30x.h"

/* 分辨率提高 */
#define PULSE_PER_LITER  150000.0f

/* 硬件定义：使用 TIMER4 的 ETR (PA0) */
/* 请查阅数据手册确认 PA0 是否可以映射为 TIMER4 的外部时钟输入，
   或者使用 TIMER1 的 ETR (也是 PA0) */
#define PUMP_TIMER       TIMER0
#define PUMP_RCU_TIMER   RCU_TIMER0
#define PUMP_RCU_GPIO    RCU_GPIOA
#define PUMP_GPIO_PORT   GPIOA
#define PUMP_GPIO_PIN    GPIO_PIN_8

/* 电机控制保持不变 */
#define MOTOR_RCU        RCU_GPIOB
#define MOTOR_PORT       GPIOB
#define MOTOR_PIN        GPIO_PIN_3

void Pump_Init(void);
void Pump_Start(void);
void Pump_Stop(void);
uint64_t Pump_GetCount(void); // 返回值改为64位以防溢出

#endif
