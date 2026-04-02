#include "Flowmeter.h"
#include "gd32f30x.h"
#include "FreeRTOS.h"
#include "semphr.h"
#include <stdio.h>


/* ================= 参数配置 ================= */
#define SLAVE_ADDRESS       0x7C    
#define ICSET               0xEA    
#define BLKCTL              0xF0    
#define DISCTL              0xA2    
#define MODESET             0xC8    
#define APCTL               0xFC    
#define ADSET               0x00    

/* 引脚定义 (请确保这里与你的硬件一致) */
#define TM1729_RCU          RCU_GPIOB
#define TM1729_PORT         GPIOB
#define TM1729_SCL_PIN      GPIO_PIN_10
#define TM1729_SDA_PIN      GPIO_PIN_11

/* ================= 全局变量 ================= */
static SemaphoreHandle_t xTM1729Mutex = NULL;
static uint8_t CODE[26] = {0}; 

/* 段码表 (与 TM1721 保持一致) 
   0: 0xFA, 1: 0x0A ... 
   Bit 0 是小数点，所以这里的基础数字 Bit 0 必须为 0 (除了本身就带点的特殊符号)
   0xFA (1111 1010) -> Bit0 是 0，正确
   0x0A (0000 1010) -> Bit0 是 0，正确
*/
static const uint8_t Smg_1729[12] = {
    0xFA, // 0
    0x0A, // 1
    0xBC, // 2
    0x9E, // 3
    0x4E, // 4
    0xD6, // 5
    0xF6, // 6
    0x8A, // 7
    0xFE, // 8
    0xCE, // 9
    0x00, // 10: 空白
    0x02  // 11: 负号 (-)
};

/* 映射表 */
static const uint8_t MAP_ROW1[8]  = {16, 17, 18, 19, 12, 13, 14, 15}; // 第一行
static const uint8_t MAP_ROW2[8]  = {10, 11, 24, 25, 6, 7, 8, 9};     // 第二行
static const uint8_t MAP_SMALL[5] = {22, 23, 2, 3, 4};                // 小屏
#define IDX_FLAG 5

/* ================= I2C 驱动 (保持之前逻辑) ================= */
#define SCL_H   gpio_bit_set(TM1729_PORT, TM1729_SCL_PIN)
#define SCL_L   gpio_bit_reset(TM1729_PORT, TM1729_SCL_PIN)
#define SDA_H   gpio_bit_set(TM1729_PORT, TM1729_SDA_PIN)
#define SDA_L   gpio_bit_reset(TM1729_PORT, TM1729_SDA_PIN)

static void i2c_delay(void) { volatile uint32_t i = 50; while(i--) __NOP(); }

static void I2C_Start(void) { SDA_H; i2c_delay(); SCL_H; i2c_delay(); SDA_L; i2c_delay(); SCL_L; i2c_delay(); }
static void I2C_Stop(void)  { SCL_L; i2c_delay(); SDA_L; i2c_delay(); SCL_H; i2c_delay(); SDA_H; i2c_delay(); }

static void I2C_SendByte(uint8_t dat) {
    for(uint8_t i=0; i<8; i++) {
        SCL_L; i2c_delay();
        if(dat & 0x80) SDA_H; else SDA_L;
        i2c_delay();
        SCL_H; i2c_delay();
        dat <<= 1;
    }
    SCL_L; i2c_delay(); SDA_H; i2c_delay(); SCL_H; i2c_delay(); SCL_L; i2c_delay();
}

static void TM1729_Flush(void) {
    I2C_Start(); I2C_SendByte(SLAVE_ADDRESS); I2C_SendByte(ADSET);
    for(uint8_t i=0; i<26; i++) I2C_SendByte(CODE[i]);
    I2C_Stop();
}

/* ================= 业务逻辑：核心修改点 ================= */

/* 字符串转段码填充函数 */
static void Fill_Buffer(char *str, const uint8_t *map, int len) {
    int map_idx = 0;
    int str_idx = 0;
    
    // 1. 先清空该区域
    for(int i=0; i<len; i++) CODE[map[i]] = 0x00;

    // 2. 填充数据
    while(str[str_idx] != '\0' && map_idx < len) {
        char c = str[str_idx];
        
        if(c == '.') {
            /* 核心逻辑：如果检测到小数点 */
            /* 找到前一个写入的位置，将其 Bit 0 置 1 */
            if(map_idx > 0) {
                // map_idx - 1 是当前数字的位置
                // 0x01 是小数点的位 (根据你的描述)
                CODE[map[map_idx-1]] |= 0x01; 
            }
        } 
        else if(c >= '0' && c <= '9') {
            /* 查表填入数字 */
            CODE[map[map_idx]] = Smg_1729[c - '0']; //如c是字符'5'但ASCII码是53,访问Smg_1729[5]需减去字符'0'
            map_idx++;
        }
        else if(c == '-') {
            CODE[map[map_idx]] = Smg_1729[11];
            map_idx++;
        }
        else if(c == ' ') {
            CODE[map[map_idx]] = Smg_1729[10];
            map_idx++;
        }
        
        str_idx++;
    }
}

void TM1729_Clear(void) {
    if(xSemaphoreTake(xTM1729Mutex, portMAX_DELAY) == pdTRUE) {
        for(int i=0; i<26; i++) CODE[i] = 0x00;
        TM1729_Flush();
        xSemaphoreGive(xTM1729Mutex);
    }
}

/* ================= 初始化与更新接口 ================= */
void TM1729_Init(void) {
    rcu_periph_clock_enable(TM1729_RCU);
    gpio_init(TM1729_PORT, GPIO_MODE_OUT_OD, GPIO_OSPEED_50MHZ, TM1729_SCL_PIN | TM1729_SDA_PIN);
    SCL_H; SDA_H;
    
    xTM1729Mutex = xSemaphoreCreateMutex();

    /* 必须加上适当的延时，防止上电瞬间芯片未准备好 */
    vTaskDelay(pdMS_TO_TICKS(50));

    I2C_Start(); I2C_SendByte(SLAVE_ADDRESS); I2C_SendByte(ICSET); I2C_Stop();
    vTaskDelay(pdMS_TO_TICKS(10));

    I2C_Start();
    I2C_SendByte(SLAVE_ADDRESS);
    I2C_SendByte(DISCTL);
    I2C_SendByte(BLKCTL);
    I2C_SendByte(APCTL);
    I2C_SendByte(MODESET);
    I2C_Stop();
    
    TM1729_Clear();
}

void TM1729_UpdateView(double vol, double amt, double price, uint8_t flag) {
    char buf[16];
    if(xSemaphoreTake(xTM1729Mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        
        /* 格式化为字符串，sprintf 会自动处理小数点字符 '.' */
        sprintf(buf, "%8.2f", vol);   
        Fill_Buffer(buf, MAP_ROW1, 8); // 调用上面的 Fill_Buffer 处理小数点
        
        sprintf(buf, "%8.2f", amt);   
        Fill_Buffer(buf, MAP_ROW2, 8);
        
        sprintf(buf, "%5.2f", price); 
        Fill_Buffer(buf, MAP_SMALL, 5);
        
        CODE[IDX_FLAG] = flag;
        
        TM1729_Flush();
        xSemaphoreGive(xTM1729Mutex);
    }
}
