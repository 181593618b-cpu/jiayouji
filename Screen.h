#ifndef __SCREEN_H__
#define __SCREEN_H__

#include <stdint.h>

/* 中文提示符定义 (对应 CODE[5] 的值, 根据实际情况修改) */
#define FLAG_SETTING    0x01  // "设置"
#define FLAG_FUELING    0x20  // "加油"
#define FLAG_NONE       0x00
#define FLAG_YUAN     	0x02  // Bit 1: 元/P (金额模式)
#define FLAG_LITER    	0x04  // Bit 2: 升/L (油量模式)
#define FLAG_STOP				0x40	// Bit 6: 停止

/* 初始化 TM1729 (GPIO & I2C配置) */
void TM1729_Init(void);

/* 刷新屏幕视图 */
/* vol: 油量, amt: 金额, price: 单价, flag: 中文状态 */
void TM1729_UpdateView(double vol, double amt, double price, uint8_t flag);

/* 清屏 */
void TM1729_Clear(void);

#endif
