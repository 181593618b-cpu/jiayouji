#include "gd32f30x.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"
#include "flash_store.h"
#include "string.h"
/* 引入各模块头文件 */
/* 请务必检查这些头文件内部是否有完整的 #ifndef ... #endif 结构 */
#include "keyboard.h"  // TM1721
#include "Screen.h"    // 大屏 (原 tm1729.h)
#include "Flowmeter.h" // 油泵 (原 pump.h)
#include "math.h"

/* ================== 1. 全局句柄定义 ================== */
SemaphoreHandle_t xTM1721Mutex = NULL;
QueueHandle_t xKeyQueue = NULL;

/* ================== 2. 全局业务变量 ================== */
volatile double global_unit_price = 7.85; // 当前单价
volatile double current_vol = 0.0;        // 当前油量
volatile double current_amt = 0.0;        // 当前金额
volatile uint64_t saved_pulses = 0;
/* ================== 3. 函数声明与定义 ================== */
/* 新增：预设控制变量 */
volatile double target_limit = 0.0;       // 预设的目标值 (0表示不限制，加满为止)
volatile uint8_t preset_type = 0;         // 0:定额(元), 1:定升(L)

/* LED初始化 (定义在main之前，避免隐式声明警告) */
static void Board_LED_Init(void)
{
    rcu_periph_clock_enable(RCU_GPIOC);
    gpio_init(GPIOC, GPIO_MODE_OUT_PP, GPIO_OSPEED_50MHZ, GPIO_PIN_13);
    gpio_bit_write(GPIOC, GPIO_PIN_13, RESET);
}

/* ================== 4. 状态机与辅助函数 ================== */
typedef enum {
        STATE_IDLE = 0,
        STATE_PASSWORD,
        STATE_SET_PRICE,
		STATE_PRESET,        // [新增] 预设模式 (输入要加多少钱或多少升)  
		STATE_FUELING,
		STATE_PAUSE,
		STATE_QUERY
} SystemState_t;

volatile SystemState_t sys_state = STATE_IDLE;

/* 1. 定义一条加油记录结构体 */
//typedef struct {
//    uint8_t  hour;    // 时
//    uint8_t  min;     // 分
//    double   amount;  // 金额
//    double   volume;  // 体积
//} FuelRecord_t;

/* 2. 模拟数据库 (最多存10条，环形存储) */
FuelRecord_t history_db[10];
uint32_t db_write_idx = 0; // 写入位置
uint32_t db_count = 0;     // 当前总记录数

/* 3. 保存记录函数 (在每次加油结束 Pump_Stop 后调用) */
void Save_Record(double amt, double vol) {
    /* 1. 更新内存数组 (原有逻辑) */
    history_db[db_write_idx].amount = amt;
    history_db[db_write_idx].volume = vol;
    history_db[db_write_idx].hour = 12; // 这里如果有RTC需改为 get_rtc_hour()
    history_db[db_write_idx].min  = 30 + db_write_idx; 
    
    db_write_idx++;
    if (db_write_idx >= 10) db_write_idx = 0;
    if (db_count < 10) db_count++;
    
    /* 2. [新增] 立即同步写入 Flash */
    /* 注意：这会阻塞 CPU 几十毫秒，但在加油结束后的结算阶段是安全的 */
    Flash_Write_History(db_write_idx, db_count, history_db);
}

/* 查询专用变量 */
int8_t  query_index = 0;      // 当前查第几条 (0是最新)
uint8_t query_mode = 0;       // 0:进入查询界面, 1:日累查询(模式1)
uint8_t show_time_flag = 0;   // 0:看量/额, 1:看时间

/* 清除所有数据并保存到 Flash */
void Clear_System_Data(void) {
    /* 1. 清空 RAM 中的数据 */
    memset(history_db, 0, sizeof(history_db));
    db_write_idx = 0;
    db_count = 0;
    
    /* 2. 如果有总累/月累变量，也在这里清空 */
    // global_total_amount = 0.0;
    
    /* 3. 立即写入 Flash (覆盖掉旧数据) */
    Flash_Write_History(0, 0, history_db);
}

/* 刷新待机界面：大屏显示上次数据，小屏显示当前单价 */
/* 刷新待机界面 */
void View_ShowIdle(void) {
    /* 1. 更新大显示屏 (TM1729) */
    /* 显示当前的油量、金额、单价，并且清除所有中文标志 (FLAG_NONE) */
    TM1729_UpdateView(current_vol, current_amt, global_unit_price, FLAG_NONE);
    
    /* 2. 更新按键板显示屏 (TM1721) */
    /* 用户要求：待机时按键板不显示价格，保持黑屏 */
    Keyboard_ClearDisplay(); 
}

/* ================== 5. 任务函数 ================== */

void led_task(void *pvParameters) {
    (void)pvParameters;
    for (;;) {
        gpio_bit_write(GPIOC, GPIO_PIN_13, (bit_status)(1 - gpio_input_bit_get(GPIOC, GPIO_PIN_13)));
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

void vKeyScanTask(void *pvParameters)
{
    (void) pvParameters;
    uint8_t KEY_raw[3] = {0};
    uint8_t key_prev[3] = {0};

    /* [新增] 长按检测专用静态变量 */
    static uint16_t key3_timer = 0;       // 计时器：1次=10ms, 200次=2秒
    static uint8_t  key3_long_handled = 0;// 标志位：1表示已触发长按，松手时不发短按

    for (;;)
    {
        /* 1. 读取按键状态 (保持原样) */
        if (xSemaphoreTake(xTM1721Mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            TM1721_ReadKeys_raw(KEY_raw);
            xSemaphoreGive(xTM1721Mutex);
        } else {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        /* 2. 扫描处理循环 */
        for (uint8_t row = 0; row < 3; row++)
        {
            /* ============================================================ */
            /* A. [新增] 长按过程检测 (检测 Key 3 是否按住不放)               */
            /* ============================================================ */
            /* 只有当前按键是 3 键时，才开始计时 */
            /* 注意：这里通过 MapScanToKeyID 反查当前按住的键是不是 3 */
            if (MapScanToKeyID(row, KEY_raw[row]) == KEY_DIGIT_3) 
            {
                key3_timer++; // 累加计时 (每10ms一次)

                /* 如果按住超过 200次 (2秒) 且 还没触发过 */
                if (key3_timer >= 200 && key3_long_handled == 0) 
                {
                    KeyID_t kid_long = KEY_DIGIT_3_LONG;
                    xQueueSend(xKeyQueue, &kid_long, 0); // 发送长按消息
                    
                    key3_long_handled = 1; // 标记：长按已触发，松手别理我
                }
            }

            /* ============================================================ */
            /* B. 松手检测 (下降沿检测)                                     */
            /* ============================================================ */
            if (key_prev[row] != 0x00 && KEY_raw[row] == 0x00)
            {
                KeyID_t kid = MapScanToKeyID(row, key_prev[row]);
                
                if (kid != KEY_NONE)
                {
                    /* [关键修改] 特殊处理 Key 3 的松手逻辑 */
                    if (kid == KEY_DIGIT_3) 
                    {
                        /* 如果刚刚已经触发了长按 -> 啥也不干，只清标志位 */
                        if (key3_long_handled == 1) {
                            key3_long_handled = 0; // 复位，为下次准备
                            key3_timer = 0;
                        }
                        /* 如果没触发长按 (说明是短按) -> 发送正常短按消息 */
                        else {
                            xQueueSend(xKeyQueue, &kid, 0);
                            key3_timer = 0; // 清零计时器
                        }
                    }
                    /* 其他按键：直接发送 */
                    else 
                    {
                        xQueueSend(xKeyQueue, &kid, 0);
                    }
                }
            }
            
            /* 更新历史状态 */
            key_prev[row] = KEY_raw[row];
        }

        /* 3. 任务延时 (保持原样 10ms) */
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/* ================== 新增：油量监控任务 ================== */
void vFuelMonitorTask(void *pvParameters)
{
    (void) pvParameters;
    
    for (;;)
    {
        /* 只有在加油状态，或者暂停状态(为了保持数据不丢失)才进行监控 */
        if (sys_state == STATE_FUELING)
        {
            /* 1. 获取脉冲并计算 */
            //extern uint64_t saved_pulses; // 需要把 vKeyLogicTask 里的 saved_pulses 提成全局变量
            uint64_t current_hw_pulses = Pump_GetCount();
            uint64_t total_pulses = saved_pulses + current_hw_pulses;
            double new_vol = (double)total_pulses / PULSE_PER_LITER;
            
            int stop_signal = 0;
            
            /* 2. 最大流量限制 */
            if (new_vol >= 9999.99) {
                new_vol = 9999.99;
                stop_signal = 1;
            }
            double new_amt = new_vol * global_unit_price;
            
            /* 3. 预设停机判断 */
            if (target_limit > 0.001) {
                if (preset_type == 0) {
                    if (new_amt >= target_limit) {
                        new_amt = target_limit;
                        if(global_unit_price > 0.001) new_vol = new_amt / global_unit_price; 
                        stop_signal = 1;
                    }
                } else {
                    if (new_vol >= target_limit) {
                        new_vol = target_limit;
                        new_amt = new_vol * global_unit_price;
                        stop_signal = 1;
                    }
                }
            }

            /* 4. 刷新屏幕 (因为底层有 Mutex，这里调刷屏非常安全！) */
            if (fabs(new_vol - current_vol) > 0.001 || stop_signal) {
                current_vol = new_vol;
                current_amt = new_amt;
                TM1729_UpdateView(current_vol, current_amt, global_unit_price, FLAG_FUELING);
            }

            /* 5. 触发停机：完美的解耦设计！ */
            if (stop_signal) {
                Pump_Stop(); // 紧急掐断底层硬件
                
                /* 不要在这里修改 sys_state 和存 Flash！ */
                /* 伪造一个 "停止键" 发给 UI 任务，让 UI 任务去处理善后工作 */
                KeyID_t auto_stop = KEY_STOP;
                xQueueSend(xKeyQueue, &auto_stop, 0); 
            }
        }
        
        /* 绝对定时的轮询：每 20ms 算一次 */
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void vKeyLogicTask(void *pvParameters)
{
    (void) pvParameters;
    KeyID_t recv_key;
    
    uint32_t input_buffer = 0;		// 数字输入缓存
    uint8_t  password_count = 0;	// 密码计数
    uint8_t  pwd_arr[3] = {0};		// 密码数组
	uint8_t  current_flag = FLAG_NONE; // 当前显示的标志位
    /* 硬件初始化 */
    TM1729_Init();
    Pump_Init();
    
		Flash_Load_History(&db_write_idx, &db_count, history_db);
		
    /* 上电显示待机界面 */
    View_ShowIdle();

    for (;;)
    {
        if (xQueueReceive(xKeyQueue, &recv_key, portMAX_DELAY) == pdTRUE)
        {
            switch (sys_state)
            {
                /* ---------------- [状态: 待机] ---------------- */
                case STATE_IDLE:
                    if (recv_key == KEY_SET) {
                        /* 第一次按下设置键 -> 进入密码输入状态 */
                        sys_state = STATE_PASSWORD;
                        input_buffer = 0;
                        password_count = 0;
                        memset(pwd_arr, 0, sizeof(pwd_arr)); // 清空密码缓存
                        /* 按键板(TM1721)清空，准备输入密码 */
                        Keyboard_ClearDisplay(); 
                        
                        /* 大屏(TM1729)显示“设置”中文标志 (FLAG_SETTING) */
                        /* 此时数据可以归零显示，或者保持原样，这里演示归零 */
                        TM1729_UpdateView(0.0, 0.0, global_unit_price, FLAG_SETTING); 
                    }
										/* [新增] 模式键逻辑：进入预设模式 */
                    else if (recv_key == KEY_MODE) {
                        sys_state = STATE_PRESET;
                        preset_type = 0;   // 0代表元/P模式
                        input_buffer = 0;  // 清空输入
                        //target_limit = 0.0;
                        
                        /* 第一次按下显示 元/P */
                        current_flag = FLAG_YUAN;
                        Keyboard_ClearDisplay();
                        TM1729_UpdateView(0.0, 0.0, global_unit_price, current_flag);
                    }
										else if (recv_key == KEY_CONFIRM) {
                        sys_state = STATE_FUELING;
												saved_pulses = 0;
                        preset_type = 0; 
                        target_limit = 0.0; // 0代表无限制
                        current_vol = 0.0; current_amt = 0.0;
                        Pump_Start();
                        TM1729_UpdateView(0.0, 0.0, global_unit_price, FLAG_FUELING);
												Keyboard_ClearDisplay();
                    }
										else if (recv_key == KEY_DIGIT_3_LONG){
												sys_state = STATE_QUERY;
												query_mode = 0;     // 初始状态
												query_index = 0;    // 指向最新一条
												show_time_flag = 0; // 默认看数据
												
												/* 界面初始化：大屏显示“查询”，按键板清空 */
												Keyboard_ClearDisplay();
												// 如果有字符库，大屏可以显示 "Query" 或 "----"
												TM1729_UpdateView(0.0, 0.0, 0.0, FLAG_NONE);
										}
                    break;

                /* ---------------- [状态: 密码输入] ---------------- */
                case STATE_PASSWORD:
                    /* 1. 按【确认】键 -> 提交密码 */
                    if (recv_key == KEY_CONFIRM) {
                        /* 密码逻辑判断 */
                        
                        /* A. 密码 101 -> 设置单价 */
                        if (password_count == 3 && pwd_arr[0]==1 && pwd_arr[1]==0 && pwd_arr[2]==1) {
                            sys_state = STATE_SET_PRICE;
                            input_buffer = 0;
                            Keyboard_ClearDisplay();
                            TM1729_UpdateView(0.0, 0.0, global_unit_price, FLAG_SETTING);
                        }
                        /* B. 密码 604 -> 清除数据 */
                        else if (password_count == 3 && pwd_arr[0]==6 && pwd_arr[1]==0 && pwd_arr[2]==4) {
                            /* 执行清除 */
                            Clear_System_Data();
                            
                            /* 界面反馈：显示 CLr (Clear) 或 SUCC */
                            /* 这里假设 Keyboard_ClearDisplay能清空，我们简单闪烁一下 */
                            Keyboard_ClearDisplay();
                            // 如果按键板能显示字母，可以尝试显示，或者大屏显示 0
                            TM1729_UpdateView(0.0, 0.0, 0.0, FLAG_NONE); 
                            
                            vTaskDelay(pdMS_TO_TICKS(1000)); // 提示停留 1秒
                            
                            /* 返回待机 */
                            sys_state = STATE_IDLE;
                            View_ShowIdle();
                        }
                        /* C. 密码错误 -> 返回待机 */
                        else {
                            sys_state = STATE_IDLE;
                            View_ShowIdle();
                        }
                        password_count = 0; // 重置计数
                    }
                    /* 2. 按【清除】键 -> 退出 */
                    else if (recv_key == KEY_CLEAR || recv_key == KEY_SET) {
                        sys_state = STATE_IDLE;
                        View_ShowIdle();
                    }
                    /* 3. 输入数字 */
                    else {
                        int digit = KeyID_ToDigit(recv_key);
                        if (digit >= 0) {
                            if (password_count < 3) {
                                pwd_arr[password_count++] = digit;
                                Keyboard_AddDigit(digit);
                            }
                            /* 注意：这里去掉了原来的 "if (password_count == 3) { ... }" 自动跳转代码 */
                            /* 现在必须按确认键才会生效 */
                        }
                    }
                    break;

                /* ---------------- [状态: 单价设置] ---------------- */
                case STATE_SET_PRICE:
                    /* 新增逻辑：再次按下设置键 -> 放弃修改，回待机 */
                    if (recv_key == KEY_SET) {
                        sys_state = STATE_IDLE;
                        View_ShowIdle(); // FLAG_SETTING 消失
                    }
                    else if (recv_key == KEY_CONFIRM) {
                        /* 确认新单价 */
                        if (input_buffer > 0) {
                            global_unit_price = (double)input_buffer / 100.0;
                        }
                        sys_state = STATE_IDLE;
                        View_ShowIdle(); // 更新所有屏幕显示新单价，标志消失
                    }
                    else if (recv_key == KEY_CLEAR) {
												sys_state = STATE_IDLE;  //自己加的*********
												input_buffer = 0;				
                        Keyboard_ClearDisplay();
                    }
										else if (recv_key == KEY_DOT){
											Keyboard_AddDot();
										}
                    else {
                        int digit = KeyID_ToDigit(recv_key);
                        if (digit >= 0) {
                            if (input_buffer < 10000) {
                                input_buffer = input_buffer * 10 + digit;
                                /* 仅在设置时，按键板显示输入的数值 */
                                Keyboard_AddDigit(digit);
                            }
                        }
                    }
                    break;

/* ---------------- [新增状态: 预设模式] ---------------- */
                case STATE_PRESET:
                    /* 再次按下模式键：切换 元 <-> 升 */
                    if (recv_key == KEY_MODE) {
                        input_buffer = 0;     // 切换单位时清空输入，防止数值混淆
                        Keyboard_ClearDisplay();
                        
                        if (preset_type == 0) {
                            preset_type = 1;  // 切换为 升/L
                            current_flag = FLAG_LITER;
                        } else {
                            preset_type = 0;  // 切换为 元/P
                            current_flag = FLAG_YUAN;
                        }
                        /* 刷新标志位显示 */
                        TM1729_UpdateView(0.0, 0.0, global_unit_price, current_flag);
                    }
                    /* 确认键：保存预设值并开始加油 */
                    else if (recv_key == KEY_CONFIRM) {
                        /* 将输入转为浮点数 */
                        if (input_buffer > 0) {
                            if (preset_type == 0) {
                                /* 模式：定额 (输入的是元，例如 100元) */
                                target_limit = (double)input_buffer; // 这里假设输入整数，如按100就是100元
                            } else {
                                /* 模式：定升 (输入的是升，例如 20升) */
                                /* 注意：如果支持小数输入需要修改 input_buffer 逻辑，这里假设整数升 */
                                target_limit = (double)input_buffer; 
                            }
                        } else {
                            target_limit = 0.0; // 输入0或者直接确认则视为不限制
                        }

                        /* 转移状态 */
                        sys_state = STATE_FUELING;
                        current_vol = 0.0; current_amt = 0.0;
                        Pump_Start();
                        /* 开始加油，显示加油标志 */
                        TM1729_UpdateView(0.0, 0.0, global_unit_price, FLAG_FUELING);
                    }
                    /* 清除键：回待机 */
                    else if (recv_key == KEY_CLEAR) {
                        sys_state = STATE_IDLE;
                        target_limit = 0.0;
                        View_ShowIdle();
                    }
                    /* 数字键：输入预设值 */
                    else {
                        int digit = KeyID_ToDigit(recv_key);
                        if (digit >= 0) {
                            if (input_buffer < 10000) { // 防止溢出
                                input_buffer = input_buffer * 10 + digit;
                                Keyboard_AddDigit(digit); // 小屏显示输入的预设值
                            }
                        }
                    }
                    break;										
										
                /* ---------------- [状态: 加油中] ---------------- */
                case STATE_FUELING:
                    if (recv_key == KEY_CONFIRM) {
												saved_pulses += Pump_GetCount();
                        Pump_Stop();
												//Save_Record(current_amt,current_vol);
                        sys_state = STATE_PAUSE;
                        TM1729_UpdateView(current_vol, current_amt, global_unit_price, FLAG_NONE);
                        /* 加油结束，按键板清屏 */
                        //View_ShowIdle(); 
                    }
										
										else if (recv_key == KEY_STOP){
												Pump_Stop();
												saved_pulses += Pump_GetCount();
												current_vol = (double)saved_pulses / PULSE_PER_LITER;
												current_amt = current_vol * global_unit_price;
												Save_Record(current_amt,current_vol);
                        sys_state = STATE_IDLE;
												TM1729_UpdateView(current_vol, current_amt, global_unit_price, FLAG_STOP);
												vTaskDelay(pdMS_TO_TICKS(2000)); 
                        View_ShowIdle();
										}
										
										else {
                        double next_target = 0.0;
                        uint8_t rounding_triggered = 0;

                        /* 按 '1'：金额凑整到下一元 */
                        if (recv_key == KEY_DIGIT_1) {
                            /* floor(12.3) = 12.0; +1.0 = 13.0 */
                            next_target = floor(current_amt) + 1.0;
                            preset_type = 0; // 定额模式
                            rounding_triggered = 1;
                        }
                        /* 按 '2'：金额凑整到下一十元 */
                        else if (recv_key == KEY_DIGIT_2) {
                            /* 12.3 / 10 = 1.23; floor=1.0; +1.0=2.0; *10=20.0 */
                            next_target = (floor(current_amt / 10.0) + 1.0) * 10.0;
                            preset_type = 0; // 定额模式
                            rounding_triggered = 1;
                        }
                        /* 按 '.'：体积凑整到下一升 */
                        else if (recv_key == KEY_DOT) {
                            /* 5.2L -> 6.0L */
                            next_target = floor(current_vol) + 1.0;
                            preset_type = 1; // 定升模式
                            rounding_triggered = 1;
                        }

                        /* 如果触发了凑整，更新目标限制 */
                        if (rounding_triggered) {
                            /* 只有当新目标大于当前值时才生效 */
                            double current_compare = (preset_type == 0) ? current_amt : current_vol;
                            if (next_target > current_compare) {
                                target_limit = next_target;
                                /* 这里可以选择是否更新大屏标志位，提示用户已进入定额模式 */
                                // current_flag = (preset_type==0) ? FLAG_YUAN : FLAG_LITER;
                                // TM1729_UpdateView(..., current_flag);
                            }
                        }
                    }
                    break;
										
										/* ---------------- [状态: 暂停] ---------------- */
								case STATE_PAUSE:
											/* 1. 按“启动(确认)”键 -> 继续加油 */
                    if (recv_key == KEY_CONFIRM) {
                        sys_state = STATE_FUELING;
                        Pump_Start(); // 恢复泵
                        /* 保持“启动”字样 */
                        TM1729_UpdateView(current_vol, current_amt, global_unit_price, FLAG_FUELING);
                    }
                    /* 2. 按“停止”键 -> 彻底结束 */
                    else if (recv_key == KEY_STOP) {
                        sys_state = STATE_IDLE;
                        /* 泵已经停了，直接更新界面 */
                        Save_Record(current_amt, current_vol);
                        /* 显示“停止”字样 */
                        TM1729_UpdateView(current_vol, current_amt, global_unit_price, FLAG_STOP);
                        
                        vTaskDelay(pdMS_TO_TICKS(2000));
                        View_ShowIdle();
                    }
                    break;
										
									case STATE_QUERY:
																			/* 1. 短按 '3' -> 切换到“模式1 (日累)” */
											if (recv_key == KEY_DIGIT_3) {
													query_mode = 1;
													/* 需求：按键板显示 "1" */
													Keyboard_ClearDisplay();
													Keyboard_AddDigit(1);
													
													/* 大屏显示第一条记录 */
													if (db_count > 0) {
															// 计算环形缓冲区实际索引：(write_idx - 1 - query_index)
															// 这里为了简化演示，假设 query_index 就是数组下标
															// 实际工程需要处理环形回绕
															int real_idx = (db_write_idx - 1 - query_index + 10) % 10;
															TM1729_UpdateView(history_db[real_idx].volume, 
																								history_db[real_idx].amount, 
																								global_unit_price, FLAG_NONE);
													}
											}
											
											/* 仅在模式 1 下响应翻页 */
											if (query_mode == 1) {
													/* 2. 按 '2' (上一条) */
													if (recv_key == KEY_DIGIT_2) {
															if (query_index < db_count - 1) {
																	query_index++; // 往旧记录翻
																	show_time_flag = 0; // 翻页后重置回看数据
															}
													}
													/* 3. 按 '8' (下一条) */
													else if (recv_key == KEY_DIGIT_8) {
															if (query_index > 0) {
																	query_index--; // 往新记录翻
																	show_time_flag = 0;
															}
													}
													/* 4. 按 '6' (看时间) */
													else if (recv_key == KEY_DIGIT_6) {
															show_time_flag = !show_time_flag; // 切换显示状态
													}
													
													/* 刷新显示逻辑 */
													if (db_count > 0) {
															int real_idx = (db_write_idx - 1 - query_index + 10) % 10;
															
															if (show_time_flag) {
																	/* 显示时间：利用大屏的 Vol/Amt 栏位显示 时:分 */
																	/* 例如：Vol栏显示 12.00 (12点), Amt栏显示 30.00 (30分) */
																	double hour_display = (double)history_db[real_idx].hour;
																	double min_display  = (double)history_db[real_idx].min;
																	TM1729_UpdateView(hour_display, min_display, 0.0, FLAG_NONE);
															} else {
																	/* 显示数据：体积、金额 */
																	TM1729_UpdateView(history_db[real_idx].volume, 
																										history_db[real_idx].amount, 
																										global_unit_price, FLAG_NONE);
															}
													} else {
															// 没有记录，显示 0
															TM1729_UpdateView(0.0, 0.0, 0.0, FLAG_NONE);
													}
											}

											/* 5. 按 '清零' -> 退出 */
											if (recv_key == KEY_CLEAR) {
													sys_state = STATE_IDLE;
													Keyboard_ClearDisplay();
													View_ShowIdle(); // 回到待机画面
											}
											break;
            }
        }

        
    }
}

/* ================== 6. FreeRTOS Hooks (修复图2报错) ================== */

/* 必须添加此函数以修复 L6218E 错误 */
void vApplicationMallocFailedHook(void)
{
    taskDISABLE_INTERRUPTS();
    for(;;);
}

void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    (void)xTask; (void)pcTaskName;
    taskDISABLE_INTERRUPTS();
    for(;;);
}

void vAssertCalled(const char *file, int line)
{
    (void) file; (void) line;
    taskDISABLE_INTERRUPTS();
    for (;;);
}

/* ================== 7. Main函数 ================== */
int main(void)
{
    /* 初始化硬件 */
    TM1721_Init();
    Board_LED_Init();

    /* 创建句柄 */
    xTM1721Mutex = xSemaphoreCreateMutex();
    xKeyQueue = xQueueCreate(16, sizeof(KeyID_t));

    if (xTM1721Mutex == NULL || xKeyQueue == NULL) { while(1); }

    /* 创建任务 */
    xTaskCreate(led_task, "LED", 128, NULL, 1, NULL);
    xTaskCreate(vKeyScanTask, "Scan", 256, NULL, 2, NULL);
    xTaskCreate(vKeyLogicTask, "Logic", 1024, NULL, 3 , NULL); 
    xTaskCreate(vFuelMonitorTask, "Monitor", 512, NULL, 4, NULL);
    /* 启动 */
    vTaskStartScheduler();
    
    while(1);
}
