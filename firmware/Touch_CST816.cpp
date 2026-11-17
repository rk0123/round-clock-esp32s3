#include "Touch_CST816.h"

struct CST816_Touch touch_data = {0};
uint8_t Touch_interrupts=0;

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// I2C
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
bool I2C_Read_Touch(uint16_t Driver_addr, uint8_t Reg_addr, uint8_t *Reg_data, uint32_t Length)
{
  I2C_Lock();                       // 触摸任务 100Hz 轮询，必须和电量计/RTC/ES8311 串行
  Wire.beginTransmission((uint8_t)Driver_addr);
  Wire.write(Reg_addr);
  if ( Wire.endTransmission(true)){
    // 静默失败：100Hz 轮询时偶发一次总线错误很正常，不能刷屏也不能当成"抬手"
    I2C_Unlock();
    return false;
  }
  uint8_t got = (uint8_t)Wire.requestFrom((uint8_t)Driver_addr, (uint8_t)Length);
  if (got != (uint8_t)Length) {
    while (Wire.available()) Wire.read();
    I2C_Unlock();
    return false;
  }
  for (uint32_t i = 0; i < Length; i++) {
    *Reg_data++ = (uint8_t)Wire.read();
  }
  I2C_Unlock();
  return true;
}
bool I2C_Write_Touch(uint8_t Driver_addr, uint8_t Reg_addr, const uint8_t *Reg_data, uint32_t Length)
{
  I2C_Lock();
  Wire.beginTransmission(Driver_addr);
  Wire.write(Reg_addr);       
  for (uint32_t i = 0; i < Length; i++) {
    Wire.write(*Reg_data++);
  }
  if ( Wire.endTransmission(true))
  {
    printf("The I2C transmission fails. - I2C Write\r\n");
    I2C_Unlock();
    return false;
  }
  I2C_Unlock();
  return true;
}
/*!
    @brief  handle interrupts
*/
void ARDUINO_ISR_ATTR Touch_CST816_ISR(void) {
  Touch_interrupts = true;
}

uint8_t Touch_Init(void) {
  pinMode(CST816_RST_PIN, OUTPUT);       // 将 GPIO5 设置为输出模式
  CST816_Touch_Reset();
  uint16_t Verification = CST816_Read_cfg();
  // 【必须关掉自动休眠】芯片睡着后不再连续上报坐标，滑动会被截断成"偶尔才灵"。
  // 旧代码这里写的是 CST816_AutoSleep(true)（=允许休眠），且写入值被硬编码成 10(0x0A)，
  // 寄存器根本没被正确设置 —— 这是"滑动不大灵"的根因之一。
  CST816_AutoSleep(false);

  pinMode(CST816_INT_PIN, INPUT_PULLUP);
  attachInterrupt(CST816_INT_PIN, Touch_CST816_ISR, FALLING);

  return true;
}
/* Reset controller */
uint8_t CST816_Touch_Reset(void)
{
  digitalWrite(CST816_RST_PIN, LOW); 
  vTaskDelay(pdMS_TO_TICKS(10));
  digitalWrite(CST816_RST_PIN, HIGH);
  vTaskDelay(pdMS_TO_TICKS(50));
  return true;
}
uint16_t CST816_Read_cfg(void) {

  uint8_t buf[3];
  I2C_Read_Touch(CST816_ADDR, CST816_REG_Version,buf, 1);
  printf("TouchPad_Version:0x%02x\r\n", buf[0]);
  I2C_Read_Touch(CST816_ADDR, CST816_REG_ChipID, buf, 3);
  printf("ChipID:0x%02x   ProjID:0x%02x   FwVersion:0x%02x \r\n",buf[0], buf[1], buf[2]);

  return true;
}
/*!
    @brief  Fall asleep automatically
    @param  Sleep_State true = 允许自动休眠, false = 禁止自动休眠（持续上报坐标）
*/
void CST816_AutoSleep(bool Sleep_State) {
  uint8_t v = Sleep_State ? 0x00 : 0x01;   // DisAutoSleep: 0=允许休眠, 1=禁止休眠
  I2C_Write_Touch(CST816_ADDR, CST816_REG_DisAutoSleep, &v, 1);
  uint8_t rb = 0xAA;
  I2C_Read_Touch(CST816_ADDR, CST816_REG_DisAutoSleep, &rb, 1);
  printf("[TOUCH] DisAutoSleep(0x%02X) <= 0x%02x  readback=0x%02x\n",
         CST816_REG_DisAutoSleep, v, rb);
}

/*!
    @brief  轮询原始坐标（不依赖芯片手势引擎）
    @return 1=读到有效帧
    自绘手势判定用：每 10ms 读一次 fingerNum + 坐标，抬手时自己算位移方向，
    阈值比芯片内置引擎宽松得多，且不受 UI 重绘帧率影响。
*/
uint8_t Touch_PollRaw(uint8_t *points, uint16_t *x, uint16_t *y) {
  uint8_t buf[6] = {0};
  if (!I2C_Read_Touch(CST816_ADDR, CST816_REG_GestureID, buf, 6)) return 0;   // 读失败=本帧无效
  uint8_t p = buf[1] & 0x0F;
  if (points) *points = p;
  if (p > 0) {
    if (x) *x = (uint16_t)(((buf[2] & 0x0F) << 8) | buf[3]);
    if (y) *y = (uint16_t)(((buf[4] & 0x0F) << 8) | buf[5]);
  }
  return 1;
}

// reads sensor and touches
// updates Touch Points
uint8_t Touch_Read_Data(void) {
  uint8_t buf[6];
  uint8_t touchpad_cnt = 0;
  I2C_Read_Touch(CST816_ADDR, CST816_REG_GestureID, buf, 6);
  /* touched gesture */
  if (buf[0] != 0x00) 
    touch_data.gesture = (GESTURE)buf[0];
  if (buf[1] != 0x00) {        
    noInterrupts(); 
    /* Number of touched points */
    touch_data.points = (uint8_t)buf[1];
    if(touch_data.points > CST816_LCD_TOUCH_MAX_POINTS)
        touch_data.points = CST816_LCD_TOUCH_MAX_POINTS;
    /* Fill coordinates */
    touch_data.x = ((buf[2] & 0x0F) << 8) + buf[3];               
    touch_data.y = ((buf[4] & 0x0F) << 8) + buf[5];
      
    interrupts(); 
    // printf(" points=%d \r\n",touch_data.points);
  }
  return true;
}
void example_touchpad_read(void){
  Touch_Read_Data();
  if (touch_data.gesture != NONE ||  touch_data.points != 0x00) {
      printf("Touch : X=%u Y=%u points=%d\r\n",  touch_data.x , touch_data.y,touch_data.points);
  } else {
      // data->state = LV_INDEV_STATE_REL;
  }
}
void Touch_Loop(void){
  if(Touch_interrupts){
    Touch_interrupts = false;
    example_touchpad_read();
  }
}


/*!
    @brief  get the gesture event name
*/
String Touch_GestureName(void) {
  switch (touch_data.gesture) {
    case NONE:
      return "NONE";
      break;
    case SWIPE_DOWN:
      return "SWIPE DOWN";
      break;
    case SWIPE_UP:
      return "SWIPE UP";
      break;
    case SWIPE_LEFT:
      return "SWIPE LEFT";
      break;
    case SWIPE_RIGHT:
      return "SWIPE RIGHT";
      break;
    case SINGLE_CLICK:
      return "SINGLE CLICK";
      break;
    case DOUBLE_CLICK:
      return "DOUBLE CLICK";
      break;
    case LONG_PRESS:
      return "LONG PRESS";
      break;
    default:
      return "UNKNOWN";
      break;
  }
}