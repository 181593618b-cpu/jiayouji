#include "key.h"
#include "Delay.h"


void Key_Init(void)
{
	// 使能GPIOA和AFIO时钟
    rcu_periph_clock_enable(RCU_GPIOA);
    rcu_periph_clock_enable(RCU_AF);
	
	rcu_periph_clock_enable(RCU_GPIOB);
	
	rcu_periph_clock_enable(RCU_GPIOC);
	
	
	// 配置PA11为上拉输入（按键）
    gpio_init(GPIOA, GPIO_MODE_IPU, GPIO_OSPEED_50MHZ, GPIO_PIN_11);
	

	
	// 配置PB0为电机控制阀
	gpio_init(GPIOB, GPIO_MODE_OUT_PP, GPIO_OSPEED_50MHZ, GPIO_PIN_0);
    gpio_bit_reset(GPIOB, GPIO_PIN_0);
	
	// 将PA11映射到EXTI线11
    gpio_exti_source_select(GPIO_PORT_SOURCE_GPIOA, GPIO_PIN_SOURCE_11);

    // 配置EXTI线11：中断模式、下降沿触发（按键按下时下降沿）
    exti_init(EXTI_11, EXTI_INTERRUPT, EXTI_TRIG_FALLING);
    exti_interrupt_flag_clear(EXTI_11); // 清除中断标志
	
	// 设置优先级分组（2位抢占优先级，2位子优先级）
    nvic_priority_group_set(NVIC_PRIGROUP_PRE2_SUB2);
    
    // 使能EXTI10~15中断线（PA11属于EXTI11）
    nvic_irq_enable(EXTI10_15_IRQn, 0, 2); // 抢占优先级0，子优先级2
	
	
}


// EXTI10~15共享此中断函数
void EXTI10_15_IRQHandler(void) 
{
	delay_ms(10);
    if (RESET != exti_interrupt_flag_get(EXTI_11))  // 检查EXTI11标志
    {
		exti_interrupt_flag_clear(EXTI_11); // 清除中断标志
        

		
	if (gpio_output_bit_get(GPIOB, GPIO_PIN_0) == SET)
        {
            gpio_bit_reset(GPIOB, GPIO_PIN_0);
        }
    else
        {
            gpio_bit_set(GPIOB, GPIO_PIN_0);   
        }
    }
}






