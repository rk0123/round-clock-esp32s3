#include "I2C_Driver.h"

static SemaphoreHandle_t s_i2cMux = NULL;

void I2C_Lock(void) {
  if (!s_i2cMux) s_i2cMux = xSemaphoreCreateMutex();   // 只会在 setup() 里第一次用到时创建
  if (s_i2cMux) xSemaphoreTake(s_i2cMux, portMAX_DELAY);
}

void I2C_Unlock(void) {
  if (s_i2cMux) xSemaphoreGive(s_i2cMux);
}

void I2C_Init(void) {
  if (!s_i2cMux) s_i2cMux = xSemaphoreCreateMutex();
  Wire.begin( I2C_SDA_PIN, I2C_SCL_PIN);
}


bool I2C_Read(uint8_t Driver_addr, uint8_t Reg_addr, uint8_t *Reg_data, uint32_t Length)
{
  I2C_Lock();
  bool fail = false;
  Wire.beginTransmission(Driver_addr);
  Wire.write(Reg_addr); 
  if ( Wire.endTransmission(true)){
    printf("The I2C transmission fails. - I2C Read\r\n");
    fail = true;
  } else {
    Wire.requestFrom(Driver_addr, Length);
    for (uint32_t i = 0; i < Length; i++) {
      *Reg_data++ = Wire.read();
    }
  }
  I2C_Unlock();
  return fail ? -1 : 0;
}
bool I2C_Write(uint8_t Driver_addr, uint8_t Reg_addr, const uint8_t *Reg_data, uint32_t Length)
{
  I2C_Lock();
  bool fail = false;
  Wire.beginTransmission(Driver_addr);
  Wire.write(Reg_addr);       
  for (uint32_t i = 0; i < Length; i++) {
    Wire.write(*Reg_data++);
  }
  if ( Wire.endTransmission(true))
  {
    printf("The I2C transmission fails. - I2C Write\r\n");
    fail = true;
  }
  I2C_Unlock();
  return fail ? -1 : 0;
}
