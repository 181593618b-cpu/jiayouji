#include "flash_store.h"
#include <string.h> // 需要 memcpy
#include "gd32f30x_fmc.h"

#ifndef FMC_FLAG_END
    /* 对应 FMC_STAT 寄存器的 Bit 5 (EOP/END) - 操作结束标志 */
    #define FMC_FLAG_END    ((uint32_t)0x00000020U)
#endif

#ifndef FMC_FLAG_WPER
    /* 对应 FMC_STAT 寄存器的 Bit 4 (WPERR) - 写保护错误标志 */
    #define FMC_FLAG_WPER   ((uint32_t)0x00000010U)
#endif

#ifndef FMC_FLAG_PGERR
    /* 对应 FMC_STAT 寄存器的 Bit 2 (PGERR) - 编程错误标志 */
    #define FMC_FLAG_PGERR  ((uint32_t)0x00000004U)
#endif

/* 功能：将内存中的历史记录保存到 Flash 
   原理：解锁 -> 擦除页 -> 写入结构体 -> 上锁
*/
void Flash_Write_History(uint32_t idx, uint32_t cnt, FuelRecord_t *arr)
{
    FuelStore_t store_data;
    uint32_t *p_data;
    uint32_t i;
    
    /* 1. 准备要写入的数据 */
    store_data.magic = MAGIC_NUMBER;
    store_data.write_idx = idx;
    store_data.count = cnt;
    // 拷贝整个数组到临时结构体
    memcpy(store_data.records, arr, sizeof(FuelRecord_t) * 10);
    
    /* 2. 解锁 Flash */
    fmc_unlock();
    
    /* 3. 清除潜在标志位 */
    fmc_flag_clear(FMC_FLAG_END | FMC_FLAG_WPER | FMC_FLAG_PGERR);
    
    /* 4. 擦除页面 (必须先擦后写) */
    if (fmc_page_erase(FLASH_SAVE_ADDR) == FMC_READY) {
        
        /* 5. 开始写入 (按 32位 Word 写入) */
        p_data = (uint32_t *)&store_data;
        
        /* 计算需要写多少个 32位字 */
        /* sizeof(store_data) 最好是4的倍数 */
        uint32_t words_to_write = sizeof(FuelStore_t) / 4; 
        if (sizeof(FuelStore_t) % 4 != 0) words_to_write++; // 补齐
        
        for (i = 0; i < words_to_write; i++) {
            fmc_word_program(FLASH_SAVE_ADDR + (i * 4), p_data[i]);
            
            /* 可选：校验一下是否写进去了，略 */
        }
    }
    
    /* 6. 上锁 */
    fmc_lock();
}

/* 功能：上电时从 Flash 读取数据 
*/
void Flash_Load_History(uint32_t *idx, uint32_t *cnt, FuelRecord_t *arr)
{
    /* 直接通过指针访问 Flash 地址 */
    FuelStore_t *p_store = (FuelStore_t *)FLASH_SAVE_ADDR;
    
    /* 1. 检查魔数：确认 Flash 里是不是真的有数据 */
    if (p_store->magic == MAGIC_NUMBER) {
        /* 有数据，读出来 */
        *idx = p_store->write_idx;
        *cnt = p_store->count;
        memcpy(arr, p_store->records, sizeof(FuelRecord_t) * 10);
    } 
    else {
        /* 没数据 (可能是第一次烧录，全是 0xFF)，初始化为 0 */
        *idx = 0;
        *cnt = 0;
        memset(arr, 0, sizeof(FuelRecord_t) * 10);
    }
}
