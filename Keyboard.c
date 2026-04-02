#include "keyboard.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

/* 引脚定义（TM1721） */
#define TM1721_STB_PORT    GPIOA
#define TM1721_STB_PIN     GPIO_PIN_3
#define TM1721_CLK_PORT    GPIOA
#define TM1721_CLK_PIN     GPIO_PIN_4
#define TM1721_DIO_PORT    GPIOA
#define TM1721_DIO_PIN     GPIO_PIN_5

#define BLANK 0x00

/* 外部标志定义 */
uint8_t CONFIRM_FLAG = 0;
uint8_t MODE_FLAG = 1;
uint8_t SETTING_FLAG = 0;

/* 段码表 */
static const uint8_t Smg[11] = {
    0xFA, 0x0A, 0xBC, 0x9E, 0x4E, 0xD6, 0xF6, 0x8A, 0xFE, 0xCE, 0x00
};

/* 显示缓冲（段码）, index 0..4 */
static uint8_t displayDigits_local[5] = { BLANK, BLANK, BLANK, BLANK, BLANK };
static uint8_t digitCount_local = 0;

/* --- 低层 I/O 辅助 --- */
static inline void STB_Out(int a){ if(a) gpio_bit_set(TM1721_STB_PORT, TM1721_STB_PIN); else gpio_bit_reset(TM1721_STB_PORT, TM1721_STB_PIN); }
static inline void CLK_Out(int a){ if(a) gpio_bit_set(TM1721_CLK_PORT, TM1721_CLK_PIN); else gpio_bit_reset(TM1721_CLK_PORT, TM1721_CLK_PIN); }
static inline void DIO_Out(int a){ if(a) gpio_bit_set(TM1721_DIO_PORT, TM1721_DIO_PIN); else gpio_bit_reset(TM1721_DIO_PORT, TM1721_DIO_PIN); }

/* 微秒延时 */
void tm_delay_us(uint32_t us)
{
    volatile uint32_t n = us * 40ul;
    while(n--) { __NOP(); }
}

/* DIO 输入/输出切换 */
void set_dio_input(void)
{
    gpio_init(TM1721_DIO_PORT, GPIO_MODE_IPU, GPIO_OSPEED_50MHZ, TM1721_DIO_PIN);
}
void set_dio_output(void)
{
    gpio_init(TM1721_DIO_PORT, GPIO_MODE_OUT_PP, GPIO_OSPEED_50MHZ, TM1721_DIO_PIN);
}

/* 初始化 GPIO */
void TM1721_Init(void)
{
    rcu_periph_clock_enable(RCU_GPIOA);
    gpio_init(TM1721_DIO_PORT, GPIO_MODE_OUT_PP, GPIO_OSPEED_50MHZ, TM1721_DIO_PIN);
    gpio_bit_set(TM1721_DIO_PORT, TM1721_DIO_PIN);
    gpio_init(TM1721_STB_PORT, GPIO_MODE_OUT_PP, GPIO_OSPEED_50MHZ, TM1721_STB_PIN);
    gpio_bit_set(TM1721_STB_PORT, TM1721_STB_PIN);
    gpio_init(TM1721_CLK_PORT, GPIO_MODE_OUT_PP, GPIO_OSPEED_50MHZ, TM1721_CLK_PIN);
    gpio_bit_set(TM1721_CLK_PORT, TM1721_CLK_PIN);
}

/* 发送8bit（低位先）*/
void write_8bit(uint8_t dat)
{
    for (uint8_t i = 0; i < 8; i++)
    {
        CLK_Out(0);
        if (dat & 0x01) DIO_Out(1);
        else DIO_Out(0);
        tm_delay_us(2); // 稍微加点延时更稳
        CLK_Out(1);
        dat >>= 1;
    }
    CLK_Out(0);
    DIO_Out(0);
}

/* 发送控制命令 */
void write_command(uint8_t Cmd)
{
    STB_Out(1);
    tm_delay_us(2);
    STB_Out(0);
    write_8bit(Cmd);
}

/* 读取原始按键 (3 字节)，调用前应持有互斥 */
void TM1721_ReadKeys_raw(uint8_t keybuf[3])
{
    write_command(0x42); /* 读键命令 */
    DIO_Out(1);
    set_dio_input();

    for (uint8_t j = 0; j < 3; j++)
    {
        keybuf[j] = 0;
        for (uint8_t i = 0; i < 8; i++)
        {
            CLK_Out(0);
            keybuf[j] >>= 1;
            CLK_Out(1);
            if (gpio_input_bit_get(TM1721_DIO_PORT, TM1721_DIO_PIN) == SET)
                keybuf[j] |= 0x80;
            tm_delay_us(2);
        }
        tm_delay_us(2);
    }
    set_dio_output();
    CLK_Out(0);
    DIO_Out(0);
    STB_Out(1);
}

/* 线程安全的显示接口：写5字节段码到屏 */
void TM1721_DisplayDigits(const uint8_t buf[5])
{
    extern SemaphoreHandle_t xTM1721Mutex;
    if (xTM1721Mutex) {
        if (xSemaphoreTake(xTM1721Mutex, pdMS_TO_TICKS(50)) != pdTRUE) return;
    }

    write_command(0x3F); // 预设

    /* 使用固定地址模式逐个写入，确保连续地址不丢数据 */
    for (uint8_t i = 0; i < 5; i++)
    {
        write_command(0x44);          // 固定地址模式
        write_command(0xC0 + i);      // 地址 C0, C1, C2...
        write_8bit(buf[i]);
        STB_Out(1);                   // 每次写完拉高锁存
        tm_delay_us(2);
    }
    
    write_command(0x97); // 开显示
    STB_Out(1);

    if (xTM1721Mutex) {
        xSemaphoreGive(xTM1721Mutex);
    }
}

/* 将数字 0..9 转为段码 */
uint8_t TM1721_DigitCode(uint8_t digit)
{
    if (digit <= 9) return Smg[digit];
    return Smg[10];
}

/* 高层显示函数 */
void Keyboard_ClearDisplay(void)
{
    for (uint8_t i = 0; i < 5; i++) displayDigits_local[i] = BLANK;
    digitCount_local = 0;
    TM1721_DisplayDigits(displayDigits_local);
}

/* 追加数字（左移，最新放 index 0）*/
void Keyboard_AddDigit(uint8_t digit)
{
    uint8_t seg = TM1721_DigitCode(digit);

    if (digitCount_local >= 5) {
        for (uint8_t i = 0; i < 5; i++) displayDigits_local[i] = BLANK;
        digitCount_local = 0;
    }

    displayDigits_local[4] = displayDigits_local[3];
    displayDigits_local[3] = displayDigits_local[2];
    displayDigits_local[2] = displayDigits_local[1];
    displayDigits_local[1] = displayDigits_local[0];
    displayDigits_local[0] = seg;

    digitCount_local++;
    TM1721_DisplayDigits(displayDigits_local);
}

void Keyboard_DisplayAtPos(uint8_t pos, uint8_t digit)
{
    if (pos > 4) return;
    uint8_t seg = TM1721_DigitCode(digit);
    displayDigits_local[pos] = seg;
    TM1721_DisplayDigits(displayDigits_local); // 这里用全刷新的方式比较安全，保持 buffer 一致
}

/* 扫描映射 */
KeyID_t MapScanToKeyID(uint8_t row, uint8_t scan)
{
    // ... 保持你原来的代码不变 ...
    // 为节省篇幅，这里略去具体的 switch case，请保留原代码
    if (row == 0) {
        switch (scan) {
            case 0x01: return KEY_DIGIT_9;
            case 0x02: return KEY_DIGIT_6;
            case 0x04: return KEY_DIGIT_3;
            case 0x08: return KEY_DIGIT_0;
            case 0x10: return KEY_DIGIT_8;
            case 0x20: return KEY_DIGIT_5;
            case 0x40: return KEY_DIGIT_2;
            case 0x80: return KEY_CLEAR;
            default: return KEY_NONE;
        }
    } else if (row == 1) {
        switch (scan) {
            case 0x01: return KEY_DIGIT_7;
            case 0x02: return KEY_DIGIT_4;
            case 0x04: return KEY_DIGIT_1;
            case 0x40: return KEY_SET;
            case 0x80: return KEY_MODE;
            case 0x08: return KEY_CONFIRM;
						case 0x10: return KEY_DOT;   
            case 0x20: return KEY_STOP;  
            default: return KEY_NONE;
        }
    }
    return KEY_NONE;
}

/* KeyID -> 数字 */
int KeyID_ToDigit(KeyID_t k)
{
    switch (k) {
        case KEY_DIGIT_0: return 0;
        case KEY_DIGIT_1: return 1;
        case KEY_DIGIT_2: return 2;
        case KEY_DIGIT_3: return 3;
        case KEY_DIGIT_4: return 4;
        case KEY_DIGIT_5: return 5;
        case KEY_DIGIT_6: return 6;
        case KEY_DIGIT_7: return 7;
        case KEY_DIGIT_8: return 8;
        case KEY_DIGIT_9: return 9;
        default: return -1;
    }
}

void Keyboard_AddDot(void)
{
    displayDigits_local[0] |= 0x01; // Bit0 置 1 点亮小数点
    
    TM1721_DisplayDigits(displayDigits_local);
}
