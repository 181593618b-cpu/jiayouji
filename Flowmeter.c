#include "Flowmeter.h"
#include "gd32f30x.h"
#include "FreeRTOS.h"
#include "task.h"

static volatile uint32_t timer_overflow_count = 0;

static void Motor_Control(uint8_t state) {
    if(state) gpio_bit_set(MOTOR_PORT, MOTOR_PIN);
    else      gpio_bit_reset(MOTOR_PORT, MOTOR_PIN);
}

void Pump_Init(void)
{
    /* 1. 开启时钟 */
    rcu_periph_clock_enable(PUMP_RCU_GPIO);
    rcu_periph_clock_enable(PUMP_RCU_TIMER); /* TIMER0 时钟 */
    rcu_periph_clock_enable(MOTOR_RCU);
    rcu_periph_clock_enable(RCU_AF);         /* 开启复用时钟 */

		gpio_pin_remap_config(GPIO_SWJ_SWDPENABLE_REMAP, ENABLE);
	
    /* 2. 配置电机引脚 */
    gpio_init(MOTOR_PORT, GPIO_MODE_OUT_PP, GPIO_OSPEED_50MHZ, MOTOR_PIN);
    Motor_Control(0);

    /* 3. 配置脉冲引脚 (PA8) 为浮空输入 */
    gpio_init(PUMP_GPIO_PORT, GPIO_MODE_IN_FLOATING, GPIO_OSPEED_50MHZ, PUMP_GPIO_PIN);

    /* 4. 定时器配置 (TIMER0) */
    timer_deinit(PUMP_TIMER);

    /* 配置通道0 (PA8) 为输入模式 */
    timer_ic_parameter_struct timer_icinitpara;
    
    /* 手动赋值 */
    timer_icinitpara.icpolarity  = TIMER_IC_POLARITY_RISING;    /* 上升沿计数 */
    timer_icinitpara.icselection = TIMER_IC_SELECTION_DIRECTTI; /* 映射到 TI0 */
    timer_icinitpara.icprescaler = TIMER_IC_PSC_DIV1;           
    timer_icinitpara.icfilter    = 0x0F;                         
    
    timer_input_capture_config(PUMP_TIMER, TIMER_CH_0, &timer_icinitpara);

    /* 触发源：CI0FE0 代表 Channel 0 滤波后输入 (即 PA8) */
    timer_input_trigger_source_select(PUMP_TIMER, TIMER_SMCFG_TRGSEL_CI0FE0);

    /* 设置为“外部时钟模式 0” (Slave Mode: External 1) */
    timer_slave_mode_select(PUMP_TIMER, TIMER_SLAVE_MODE_EXTERNAL0);

    /* 设置最大重装载值 */
    timer_autoreload_value_config(PUMP_TIMER, 0xFFFF);
    
    /* 5. 配置溢出中断 (关键修改点) */
    /* 高级定时器的更新中断名为 TIMER0_UP_IRQn */
    nvic_irq_enable(TIMER0_UP_IRQn, 1, 0); 
    timer_interrupt_enable(PUMP_TIMER, TIMER_INT_UP);
    
    timer_disable(PUMP_TIMER);
}

void Pump_Start(void)
{
    timer_overflow_count = 0;
    timer_counter_value_config(PUMP_TIMER, 0);
    Motor_Control(1);
    timer_enable(PUMP_TIMER);
}

void Pump_Stop(void)
{
    Motor_Control(0);
    timer_disable(PUMP_TIMER);
}

//uint64_t Pump_GetCount(void)
//{
//    uint32_t count_val = timer_counter_read(PUMP_TIMER);
//    return ((uint64_t)timer_overflow_count << 16) | count_val;
//}

uint64_t Pump_GetCount(void)
{
    uint64_t overflows;
    uint32_t count_val;

    /* 1. 进入微观临界区，屏蔽中断，锁死物理时间 */
    taskENTER_CRITICAL();
    
    /* 2. 依次读取软件溢出次数和硬件计数值 */
    overflows = timer_overflow_count;
    count_val = timer_counter_read(PUMP_TIMER);
    
    /* 3. 【神级细节】：检查是否发生了“幽灵中断”！
     * 场景：硬件定时器刚刚溢出归零，标志位亮了，但由于我们关了中断，ISR还没来得及执行。
     * 如果不处理，我们会读到新的 count_val(0x0000)，但搭配的却是旧的 overflows！
     */
    if ( timer_interrupt_flag_get(PUMP_TIMER,TIMER_INT_FLAG_CH0) == SET ) // 判断溢出标志位是否置位
    {
        /* 既然溢出了，但中断还没执行，我们就手动在本次计算中把溢出次数加 1 */
        overflows++; 
        
        /* 极其重要：由于我们在读 count_val 的前后随时可能发生物理溢出，
           必须重新读一次硬件，确保拿到的是溢出后、从 0 开始的新计数值！*/
        count_val = timer_counter_read(PUMP_TIMER); 
    }

    /* 4. 退出临界区，交还中断控制权 */
    /* 此时如果刚才有积压的溢出中断，系统会立刻跑去执行 ISR 更新全局变量，
       但不影响我们手里的这份完美快照 */
    taskEXIT_CRITICAL();

    /* 5. 安全拼接并返回 */
    return (overflows << 16) | count_val;
}

/* 关键修改点：高级定时器0 的更新中断服务函数 */
void TIMER0_UP_IRQHandler(void)
{
    if(timer_interrupt_flag_get(PUMP_TIMER, TIMER_INT_UP) != RESET) {
        timer_overflow_count++;
        timer_interrupt_flag_clear(PUMP_TIMER, TIMER_INT_UP);
    }
}
