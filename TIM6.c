#include "TIM6.h"

// 配置TIM6定时器（1秒中断）
void TIM6_Init(void) {
    // 1. 使能TIM6时钟
    rcu_periph_clock_enable(RCU_TIMER6);

    // 2. 计算分频值（120MHz主频→1Hz中断）
    timer_prescaler_config(TIMER6, 12000 - 1, TIMER_PSC_RELOAD_NOW);  // 120MHz / 12000 = 10kHz
    timer_autoreload_value_config(TIMER6, 10000 - 1); // 10kHz / 10000 = 1Hz

    // 3. 配置时基：向上计数模式
    timer_counter_up_direction(TIMER6);
    timer_auto_reload_shadow_enable(TIMER6);

    // 4. 使能更新中断
    timer_interrupt_enable(TIMER6, TIMER_INT_UP);
    nvic_irq_enable(TIMER6_IRQn, 2, 0); // 优先级低于EXTI

    // 5. 启动定时器
    timer_enable(TIMER6);
}
