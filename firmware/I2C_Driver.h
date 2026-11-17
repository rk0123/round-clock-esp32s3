#pragma once
#include <Wire.h> 
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#define I2C_SCL_PIN       10
#define I2C_SDA_PIN       11

void I2C_Init(void);

bool I2C_Read(uint8_t Driver_addr, uint8_t Reg_addr, uint8_t *Reg_data, uint32_t Length);
bool I2C_Write(uint8_t Driver_addr, uint8_t Reg_addr, const uint8_t *Reg_data, uint32_t Length);

// ⭐ 全局 I²C 互斥：这块板上触摸(Core1 100Hz)、电量计、RTC、ES8311 共用一条 Wire 总线，
//   而 Arduino 的 TwoWire 不是线程安全的 —— 两个任务同时 beginTransmission 会让事务互相插队，
//   表现为「写进去了但少几字节 / 读回来字节错位 / 某个寄存器自己在按秒跳」。
//   所有直接操作 Wire 的地方（含 Touch_CST816.cpp、es8311.cpp）都必须 I2C_Lock()/I2C_Unlock()。
void I2C_Lock(void);
void I2C_Unlock(void);
