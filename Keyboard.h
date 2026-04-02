#ifndef __KEYBOARD_H__

#define __KEYBOARD_H__



#include <stdint.h>

#include "gd32f30x.h"

#include "FreeRTOS.h"

#include "semphr.h"



/* 互斥量（在 main.c 中定义） */

extern SemaphoreHandle_t xTM1721Mutex;



/* 外部标志位（在 keyboard.c 中定义） */

extern uint8_t CONFIRM_FLAG;

extern uint8_t MODE_FLAG;

extern uint8_t SETTING_FLAG;



/* KeyID: 按键逻辑定义（对应扫描码） */

typedef enum {

    KEY_NONE = 0,



    /* KEY[0] 列（row0） */

    KEY_DIGIT_9,   // scan 0x01

    KEY_DIGIT_6,   // scan 0x02

    KEY_DIGIT_3,   // scan 0x04

    KEY_DIGIT_0,   // scan 0x08

    KEY_DIGIT_8,   // scan 0x10

    KEY_DIGIT_5,   // scan 0x20

    KEY_DIGIT_2,   // scan 0x40

    KEY_CLEAR,     // scan 0x80



    /* KEY[1] 列（row1） */

    KEY_DIGIT_7,   // scan 0x01

    KEY_DIGIT_4,   // scan 0x02

    KEY_DIGIT_1,   // scan 0x04



    KEY_SET,       // scan 0x40 (设置键)

    KEY_MODE,      // scan 0x80

    KEY_CONFIRM,   // scan 0x08

		KEY_DOT,
		
		KEY_STOP,

		KEY_DIGIT_3_LONG,

} KeyID_t;

/* TM1721 / keyboard API */

void TM1721_Init(void);

void TM1721_ReadKeys_raw(uint8_t keybuf[3]); /* caller should hold xTM1721Mutex before calling */

void TM1721_DisplayDigits(const uint8_t buf[5]);

uint8_t TM1721_DigitCode(uint8_t digit);



/* 高层显示/键盘 API（线程安全） */

void Keyboard_ClearDisplay(void);

void Keyboard_AddDigit(uint8_t digit);        /* 左移，最新放最右（index 0） */

void Keyboard_DisplayAtPos(uint8_t pos, uint8_t digit);

void Keyboard_AddDot(void);

/* 映射/工具 */

KeyID_t MapScanToKeyID(uint8_t row, uint8_t scan);

int KeyID_ToDigit(KeyID_t k);



/* 调试用延时（busy-loop）——生产请用定时器替代 */

void tm_delay_us(uint32_t us);



#endif /* __KEYBOARD_H__ */
