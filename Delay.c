#include "Delay.h"


// 初始化SysTick（主频120MHz）
void Delay_Init(void) {
    SysTick->CTRL = 0;                          // 关闭SysTick
    SysTick->LOAD = 0xFFFFFF;                   // 24位最大值（支持长延时）[3,7](@ref)
    SysTick->VAL = 0;                           // 清空计数器
    SysTick->CTRL = SysTick_CTRL_ENABLE_Msk |   // 使能SysTick
                   SysTick_CTRL_CLKSOURCE_Msk; // 使用内核时钟（120MHz）
}

// 微秒级延时（核心函数）
void delay_us(uint32_t us) {
    uint32_t ticks = us * 120;                  // 计算总计数 (120MHz下1us=120周期)
    uint32_t start_val = SysTick->VAL;          // 记录起始值
    
    while (1) {
        uint32_t current_val = SysTick->VAL;
        uint32_t elapsed;
        
        // 处理计数器翻转（24位递减计数器）[3,7](@ref)
        if (current_val <= start_val) {
            elapsed = start_val - current_val;   // 未翻转
        } else {
            elapsed = (0xFFFFFF - current_val) + start_val; // 翻转补偿
        }
        
        if (elapsed >= ticks) break;            // 达到目标计数
    }
}

// 毫秒级延时（优化版）
void delay_ms(uint32_t ms) {
    while (ms--) {
        delay_us(1000);                         // 复用微秒延时
    }
}

// 秒级延时（带长延时优化）
void delay_s(uint32_t s) {
    const uint32_t max_block_ms = 60000;        // 单次最大延时60s（避免栈溢出）
    while (s > 0) {
        uint32_t block = (s > max_block_ms) ? max_block_ms : s;
        delay_ms(block * 1000);                 // 分块延时
        s -= block;
    }
}
