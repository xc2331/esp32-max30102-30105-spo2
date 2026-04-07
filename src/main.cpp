#include <Wire.h>
#include "MAX30105.h"
#include "BluetoothSerial.h"

BluetoothSerial SerialBT;

void Blue_receive()
{
  if (SerialBT.available())
  {
    String received = SerialBT.readString();
    // Echo back to phone / 回显给手机
    SerialBT.print("ESP32收到: ");
    SerialBT.println(received);
  }
}

void Blue_send(String Msg)
{
    String msg = Msg;
    SerialBT.print("ESP32发送: ");
    SerialBT.println(msg);
}

MAX30105 particleSensor;

#define debug Serial
#define VERSION "1.0.0"
// Buffer size (algorithm standard: 100 points) / 缓冲区大小 (算法标准：100 个点)
#define BUFFER_SIZE 200

// Global buffers (store raw values) / 全局缓冲区 (存储纯数字)
uint32_t redBuffer[BUFFER_SIZE];
uint32_t irBuffer[BUFFER_SIZE];
int dataCount = 0;

/**
 * @brief Calculate array average (DC component) / 计算数组的平均值 (DC 分量)
 * @param buffer Input array / 输入数组
 * @param size Array size / 数组大小
 * @return Average value / 平均值
 */
uint32_t calculateAverage(uint32_t *buffer, int size)
{
  uint64_t sum = 0; // Use 64-bit to prevent overflow / 使用 64 位防止溢出
  for (int i = 0; i < size; i++)
  {
    sum += buffer[i];
  }
  return (uint32_t)(sum / size);
}

/**
 * @brief Remove DC component and apply moving average filter / 执行去直流和移动平均滤波
 * @param rawBuffer Raw data input / 原始数据输入
 * @param procBuffer Processed data output (can be negative, use int32_t) / 处理后数据输出 (可以是负数，所以用 int32_t)
 * @param size Data length / 数据长度
 * @param dcValue Pre-calculated DC component (average) / 已经计算好的直流分量 (平均值)
 */
void processSignal(uint32_t *rawBuffer, int32_t *procBuffer, int size, uint32_t dcValue)
{
  // Temporary array for DC-removed data / 临时数组用于存储去直流后的数据
  int32_t tempAC[size];

  // 1. Remove DC (AC = Raw - DC) / 1. 去直流 (AC = Raw - DC)
  // Note: MAX30105 waveform is typically downward pulse, we use: Processed = DC - Raw
  // 注意：MAX30105 的波形通常是向下的脉冲，为了方便找波峰，我们使用：Processed = DC - Raw
  for (int i = 0; i < size; i++)
  {
    tempAC[i] = (int32_t)dcValue - (int32_t)rawBuffer[i];
  }

  // 2. 4-point moving average filter / 2. 4 点移动平均滤波
  // Formula: Out[i] = (In[i] + In[i+1] + In[i+2] + In[i+3]) / 4
  // Note: Last 3 points cannot do complete 4-point average / 注意：最后 3 个点无法做完整的 4 点平均
  for (int i = 0; i < size; i++)
  {
    if (i <= size - 4)
    {
      int32_t sum = tempAC[i] + tempAC[i + 1] + tempAC[i + 2] + tempAC[i + 3];
      procBuffer[i] = sum / 4;
    }
    else
    {
      // Keep original DC-removed value for remaining points / 尾部剩余数据，直接保留去直流后的值
      procBuffer[i] = tempAC[i];
    }
  }
}

/**
 * @brief Find peak positions in processed signal / 寻找波峰位置
 * @param buffer Processed signal array (AC component) / 处理后的信号数组 (AC 分量)
 * @param size Array length / 数组长度
 * @param peaks Array to store peak indices / 存放波峰索引的数组
 * @param maxPeaks Maximum number of peaks to find / 最多寻找的波峰数量
 * @return Actual number of peaks found / 实际找到的波峰数量
 */
int findPeaks(int32_t *buffer, int size, int *peaks, int maxPeaks)
{
  int peakCount = 0;
  // Threshold: must be > 0 and > 20% of max amplitude to be valid peak / 阈值：必须大于 0 且大于最大幅度的 20% 才认为是有效波峰
  // Find max value as reference / 先简单找最大值作为参考
  int32_t maxVal = -100000;
  for (int i = 0; i < size; i++)
    if (buffer[i] > maxVal)
      maxVal = buffer[i];

  int32_t threshold = maxVal / 5;
  if (threshold < 10)
    threshold = 10; // Minimum threshold protection / 最小阈值保护

  // Simple local maximum detection / 简单的局部最大值检测
  for (int i = 1; i < size - 1; i++)
  {
    if (buffer[i] > buffer[i - 1] && buffer[i] > buffer[i + 1])
    {
      if (buffer[i] > threshold)
      {
        // Avoid detecting fake peaks too close (min interval 20 points, ~0.2s, max HR 300bpm) / 避免检测到靠得太近的假波峰 (最小间隔 20 个点)
        if (peakCount > 0)
        {
          if ((i - peaks[peakCount - 1]) > 20)
          {
            peaks[peakCount++] = i;
          }
        }
        else
        {
          peaks[peakCount++] = i;
        }

        if (peakCount >= maxPeaks)
          break;
      }
    }
  }
  return peakCount;
}

/**
 * @brief Calculate blood oxygen saturation (SpO2) / 计算血氧饱和度 (SpO2)
 * @param redAC Red light AC component average (absolute value) / 红光交流分量平均值 (绝对值)
 * @param redDC Red light DC component / 红光直流分量
 * @param irAC Infrared AC component average (absolute value) / 红外光交流分量平均值 (绝对值)
 * @param irDC Infrared DC component / 红外光直流分量
 * @return Estimated SpO2 percentage (0-100) / 估算的 SpO2 百分比 (0-100)
 */
int calculateSpO2(float redAC, uint32_t redDC, float irAC, uint32_t irDC)
{
  if (redDC == 0 || irDC == 0 || irAC == 0)
    return 0;

  // Calculate R value = (AC_Red / DC_Red) / (AC_IR / DC_IR) / 计算 R 值
  float rRatio = (redAC / (float)redDC) / (irAC / (float)irDC);

  // Classic empirical formula (MAX30102/30105 common fitting curve) / 经典经验公式
  // More accurate polynomial fitting: SpO2 = -45.060*R^2 + 30.354*R + 94.845
  float spo2 = -45.060 * rRatio * rRatio + 30.354 * rRatio + 94.845;

  if (spo2 < 0)
    spo2 = 0;
  if (spo2 > 100)
    spo2 = 100;

  return (int)spo2;
}

/**
 * @brief Find adjacent valleys between peaks and calculate real AC amplitude / 根据波峰位置，寻找相邻波谷并计算真实的 AC 幅度
 * @param buffer Processed signal array (AC component) / 处理后的信号数组 (AC 分量)
 * @param peaks Array of detected peak indices / 已检测到的波峰索引数组
 * @param peakCount Number of peaks / 波峰数量
 * @param size Total array size / 数组总长度
 * @return Calculated average AC amplitude (returns 0 if data invalid) / 计算出的平均 AC 幅度 (如果数据无效返回 0)
 */
float calculateRealAC(int32_t *buffer, int *peaks, int peakCount, int size)
{
  if (peakCount < 2)
    return 0.0; // Need at least 2 peaks for one complete cycle / 至少需要两个波峰才能算一个完整的周期

  float totalAC = 0.0;
  int validCycles = 0;

  // Iterate through each pair of adjacent peaks / 遍历每一对相邻的波峰
  for (int i = 0; i < peakCount - 1; i++)
  {
    int peakIdx1 = peaks[i];
    int peakIdx2 = peaks[i + 1];

    // Find minimum value (valley) between two peaks / 在两个波峰之间寻找最小值 (波谷)
    int32_t minVal = 32767; // Initialize with large value / 初始化为一个较大的数
    int minIdx = -1;

    for (int j = peakIdx1; j <= peakIdx2; j++)
    {
      if (buffer[j] < minVal)
      {
        minVal = buffer[j];
        minIdx = j;
      }
    }

    // Get peak value / 获取波峰的值
    int32_t peakVal = buffer[peakIdx1];

    // Calculate AC amplitude for this cycle = (peak - valley) / 2.0 / 计算该周期的 AC 幅度
    // Peak-to-Peak value / 差值即为峰峰值 (Peak-to-Peak)
    float peakToPeak = (float)peakVal - (float)minVal;

    // Simple validity check: peak-to-peak not too small (noise) or too large (abnormal) / 简单的有效性检查
    if (peakToPeak > 20 && peakToPeak < 5000)
    {
      totalAC += (peakToPeak / 2.0);
      validCycles++;
    }
  }

  if (validCycles > 0)
  {
    return totalAC / validCycles;
  }
  else
  {
    return 0.0;
  }
}

int lastValidSpo2 = -1;        // Record last valid SpO2, -1 means no data / 记录上一次有效的血氧值，初始为 -1 表示无数据
#define SPO2_MIN_THRESHOLD 85  // SpO2 minimum threshold / 血氧下限阈值
#define SPO2_MAX_THRESHOLD 100 // SpO2 maximum threshold / 血氧上限阈值
#define SPO2_MAX_JUMP 15       // Maximum allowed jump (e.g., 98% -> 90% not allowed) / 允许的最大跳变幅度

void setup()
{
  debug.begin(115200);

  Serial.begin(115200);
  SerialBT.begin("ESP32_BT"); // Bluetooth device name / 蓝牙设备名称
  Serial.println("蓝牙已启动，等待连接... / Bluetooth started, waiting for connection...");

  // Initialize sensor / 初始化传感器
  if (particleSensor.begin() == false)
  {
    while (1)
      ; // Sensor not found / 传感器未找到
  }

  particleSensor.setup(); // Configure sensor. Use 6.4mA for LED drive / 配置传感器
  dataCount = 0;
}

void loop()
{
  // Get raw values directly from getRed() and getIR() / 直接使用 getRed() 和 getIR() 获取纯数字
  uint32_t redValue = particleSensor.getRed();
  uint32_t irValue = particleSensor.getIR();

  // Simple validity check (avoid storing 0 or abnormally large values) / 简单的有效性检查
  if (redValue > 1000 && irValue > 1000)
  {
    // Store in array / 存入数组
    redBuffer[dataCount] = redValue;
    irBuffer[dataCount] = irValue;
    dataCount++;
  }
  else
  {
    Serial.print("MSG:");
    Blue_send("MSG:");
    Serial.println("InValid Data");
    Blue_send("InValid Data");
  }

  // Check if buffer is full / 检查是否集齐
  if (dataCount >= BUFFER_SIZE)
  {
    dataCount = 0;
    // Arrays for processed data (must be int32_t, because DC removal produces negative values) / 处理后数据的数组
    static int32_t redProcessed[BUFFER_SIZE];
    static int32_t irProcessed[BUFFER_SIZE];

    // 1. Calculate DC (average) / 1. 计算 DC (平均值)
    uint32_t redDC = calculateAverage(redBuffer, BUFFER_SIZE);
    uint32_t irDC = calculateAverage(irBuffer, BUFFER_SIZE);

    // 2. Remove DC and apply filter / 2. 执行去直流和滤波
    processSignal(redBuffer, redProcessed, BUFFER_SIZE, redDC);
    processSignal(irBuffer, irProcessed, BUFFER_SIZE, irDC);
    
    // Prepare array for peaks (max 10 peaks) / 准备存储波峰的数组 (最多存 10 个波峰)
    int redPeaks[10];

    // 2. Execute peak detection (use red signal for heart rate, as red waveform is usually clearer) / 执行波峰检测
    int numPeaks = findPeaks(redProcessed, BUFFER_SIZE, redPeaks, 10);

    int spo2Value = 0;
    // Normal physiological range validation (0.3 ~ 4.0) / 正常生理范围校验
    bool rangeValid = false;
    bool jumpValid = false;
    bool isDataValid = false;
    int rawSpo2Value = 0;
    
    // Calculate average AC amplitude for entire buffer / 计算整个缓冲区的平均 AC 幅度
    float realRedAC = calculateRealAC(redProcessed, redPeaks, numPeaks, BUFFER_SIZE);
    float realIrAC = calculateRealAC(irProcessed, redPeaks, numPeaks, BUFFER_SIZE);

    if (numPeaks >= 2)
    {
      // 1. Basic signal strength check / 基础信号强度检查
      if (realRedAC > 10.0 && realIrAC > 10.0)
      {
        // Get DC values again / 获取 DC 值
        uint32_t currentRedDC = calculateAverage(redBuffer, BUFFER_SIZE);
        uint32_t currentIrDC = calculateAverage(irBuffer, BUFFER_SIZE);

        // Prevent division by zero / 防止除以零
        if (currentRedDC > 0 && currentIrDC > 0 && realIrAC > 0)
        {
          // 2. Calculate raw SpO2 / 2. 计算原始 SpO2
          rawSpo2Value = calculateSpO2(realRedAC, currentRedDC, realIrAC, currentIrDC);

          // 3. Calculate R value and perform anomaly filtering / 计算 R 值并进行异常过滤
          float rRatio = (realRedAC / (float)currentRedDC) / (realIrAC / (float)currentIrDC);

          if (rRatio >= 0.3 && rRatio <= 4.0)
          {
            isDataValid = true;
          }

          // Range filter: must be between 85% - 100% / 范围过滤：必须在 85% - 100% 之间
          if (rawSpo2Value >= SPO2_MIN_THRESHOLD && rawSpo2Value <= SPO2_MAX_THRESHOLD)
          {
            rangeValid = true;
          }

          // Jump filter: prevent drastic value changes / 跳变过滤：防止数值剧烈波动
          if (lastValidSpo2 == -1)
          {
            // First valid data, jump is considered valid / 如果是第一个有效数据，直接认为跳变合法
            jumpValid = true;
          }
          else
          {
            int diff = abs(rawSpo2Value - lastValidSpo2);
            if (diff <= SPO2_MAX_JUMP)
            {
              jumpValid = true;
            }
          }

          // 3. Comprehensive judgment: R value normal + range normal + jump normal / 综合判断
          if (rangeValid && jumpValid)
          {
            isDataValid = true;
            // Update history / 更新历史记录
            lastValidSpo2 = rawSpo2Value;
          }
          else
          {
            isDataValid = false; // Even if R value is correct, mark as invalid if value is unreasonable / 即使 R 值对，但数值不合理，也标记为无效
          }
        }
      }
    }

    // Output result / 输出结果
    if (isDataValid)
    {
      // Format: DATA:value / Python will recognize "DATA:" prefix
      Serial.print("DATA:");
      Blue_send("DATA:");
      Serial.println(rawSpo2Value);
      Blue_send((String)rawSpo2Value);
    }
    else
    {
      // Format: MSG:reason / Python will recognize "MSG:" prefix
      String reason = "";

      // Determine specific reason / 判断具体是哪种原因
      if (realRedAC < 10.0 || realIrAC < 10.0)
      {
        reason = "信号太弱 (请压紧手指) / Signal too weak (please press finger tighter)";
      }
      else if (!rangeValid)
      {
        reason = "数值异常 (<85% 或 >100%) / Value abnormal";
      }
      else if (!jumpValid)
      {
        reason = "数值跳变过大 (请保持静止) / Value jump too large (please keep still)";
      }
      else if (numPeaks < 2)
      {
        reason = "未检测到完整心跳波形 / No complete heartbeat waveform detected";
      }
      else
      {
        reason = "R值超出正常范围 / R value out of normal range";
      }

      Serial.print("MSG:");
      Blue_send("MSG:");
      Serial.println(reason);
      Blue_send(reason);
    }
  }
}
