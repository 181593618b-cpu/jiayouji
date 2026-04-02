#ifndef __FLASH_STORE_H__
#define __FLASH_STORE_H__

#include "gd32f30x.h"

/* ================= 配置区域 ================= */
/* 假设使用 256KB 容量的芯片，最后一页地址 0x0803F800 */
/* 如果是 C8T6 (64KB)，改为 0x0800FC00 */
#define FLASH_SAVE_ADDR  0x0803F800  

#define MAGIC_NUMBER     0xA5A55A5A  // 用于标记 Flash 里是否有有效数据

/* 记录结构体 (和 main.c 保持一致) */
typedef struct {
    uint8_t  hour;
    uint8_t  min;
    double   amount;
    double   volume;
} FuelRecord_t;

/* 存储管理结构体 (包含索引和数据) */
typedef struct {
    uint32_t magic;       // 有效标志
    uint32_t write_idx;   // 当前写到第几个了
    uint32_t count;       // 当前存了多少条
    FuelRecord_t records[10]; // 数据数组
} FuelStore_t;

/* 函数声明 */
void Flash_Write_History(uint32_t idx, uint32_t cnt, FuelRecord_t *arr);
void Flash_Load_History(uint32_t *idx, uint32_t *cnt, FuelRecord_t *arr);

#endif
