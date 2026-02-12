// Use board "ESP32 Dev Module" (last tested on v3.2.0)
#include <Arduino_GFX_Library.h>  // Install "GFX Library for Arduino" with the Library Manager (last tested on v1.6.0)
#include "MjpegClass.h" // Install "JPEGDEC" with the Library Manager (last tested on v1.8.2)
#include "SD.h" // Included with the Espressif Arduino Core (last tested on v3.2.0)

// 硬件引脚定义
#define BL_PIN      21    // 背光引脚
#define SD_CS       5     // SD卡CS
#define SD_MISO     19    // SD卡MISO
#define SD_MOSI     23    // SD卡MOSI
#define SD_SCK      18    // SD卡SCK
#define BOOT_PIN    0     // BOOT按键
#define BOOT_DEBOUNCE 400 // 按键防抖时间(ms)

// Some model of cheap Yellow display works only at 40Mhz
// #define DISPLAY_SPI_SPEED 40000000L // 40MHz 
#define DISPLAY_SPI_SPEED 80000000L
#define SD_SPI_SPEED      80000000L
const char *MJPEG_FOLDER = "/mjpeg";

// 扩展文件组数据结构
typedef struct {
  String triggerFile;  // 触发文件（aoutput2_*.mjpeg）
  String loopFile;     // 循环文件（aoutput_*.mjpeg）
} MjpegGroup;

// 全局变量
Arduino_DataBus *bus = new Arduino_HWSPI(2 /* DC */, 15 /* CS */, 14 /* SCK */, 13 /* MOSI */, 12 /* MISO */);
Arduino_GFX *gfx = new Arduino_ILI9341(bus);
SPIClass sd_spi(VSPI);

MjpegClass mjpeg;
uint8_t *mjpeg_buf = nullptr;
bool isFirstFrame = false; // 当前文件第一帧标记

// 文件检测
bool fileOkStart = false;    // start.mjpeg存在标记
bool fileOkInit = false;     // output1.mjpeg存在标记
bool fileOkDefaultLoop = false; // output.mjpeg存在标记
#define MAX_GROUP_COUNT 10  // 最大扩展组数量
MjpegGroup mjpegGroups[MAX_GROUP_COUNT];
int groupCount = 0;         // 实际检测到的扩展组数量
int currentGroupIndex = -1; // 当前扩展组索引（-1为默认）

// 播放状态
bool playedStartFile = false; // 启动文件播放完成标记
bool playedInitFile = false;  // 片头文件播放完成标记
String currentLoopFile;       // 当前循环播放文件
volatile bool skipRequested = false; // 跳过播放请求
volatile bool switchGroupRequested = false; // 切换扩展组请求
uint32_t lastPressTime = 0;  // 上次按键时间

// 按键中断处理
void IRAM_ATTR onButtonPress() {
  uint32_t now = xTaskGetTickCountFromISR();
  if ((now - lastPressTime) > (BOOT_DEBOUNCE / portTICK_PERIOD_MS)) {
    lastPressTime = now;
    skipRequested = true;
    if (groupCount > 0) {
      switchGroupRequested = true;
    }
  }
}

// 检查文件是否存在
bool fileExists(const char *filename) {
  String fullPath = String(MJPEG_FOLDER) + "/" + filename;
  File file = SD.open(fullPath.c_str(), "r");
  bool exists = (file && !file.isDirectory());
  if (file) file.close();
  return exists;
}

// JPEG绘制回调
int jpegDrawCallback(JPEGDRAW *pDraw) {
  if (!isFirstFrame) {
    gfx->fillScreen(RGB565_BLACK);
    isFirstFrame = true;
  }
  gfx->draw16bitBeRGBBitmap(pDraw->x, pDraw->y, pDraw->pPixels, pDraw->iWidth, pDraw->iHeight);
  return 1;
}

// 扫描扩展文件组
void scanMjpegGroups() {
  groupCount = 0;
  // 兼容无数字后缀扩展组（第0组）
  if (fileExists("aoutput2.mjpeg") && fileExists("aoutput.mjpeg")) {
    mjpegGroups[groupCount].triggerFile = "aoutput2.mjpeg";
    mjpegGroups[groupCount].loopFile = "aoutput.mjpeg";
    groupCount++;
  }

  // 扫描带数字后缀扩展组（1~MAX_GROUP_COUNT）
  for (int i = 2; i <= MAX_GROUP_COUNT; i++) {
    String triggerName = "aoutput2_" + String(i) + ".mjpeg";
    String loopName = "aoutput_" + String(i) + ".mjpeg";
    if (fileExists(triggerName.c_str()) && fileExists(loopName.c_str())) {
      if (groupCount >= MAX_GROUP_COUNT) break;
      mjpegGroups[groupCount].triggerFile = triggerName;
      mjpegGroups[groupCount].loopFile = loopName;
      groupCount++;
    }
  }
}

// 播放指定MJPEG文件
void playMjpegFile(const char *filename, bool allowSkip = true) {
  if (filename == nullptr || strlen(filename) == 0) return;

  String fullPath = String(MJPEG_FOLDER) + "/" + filename;
  char fn[128];
  fullPath.toCharArray(fn, sizeof(fn));

  File mjpegFile = SD.open(fn, "r");
  if (!mjpegFile || mjpegFile.isDirectory()) return;

  isFirstFrame = false;

  mjpeg.setup(
    &mjpegFile, mjpeg_buf, jpegDrawCallback, true,
    0, 0, gfx->width(), gfx->height()
  );

  while (mjpegFile.available() && mjpeg.readMjpegBuf()) {
    if (allowSkip && skipRequested) {
      break; // 允许跳过且有跳过请求时，终止播放
    }
    mjpeg.drawJpg();
  }

  mjpegFile.close();
  skipRequested = false; // 重置跳过请求
}

// 初始化
void setup() {
  Serial.begin(115200);
  delay(100);

  // 显示屏初始化
  if (!gfx->begin(DISPLAY_SPI_SPEED)) {
    Serial.println("Err: Display init failed!");
    while (1);
  }
  gfx->setRotation(0);
  gfx->fillScreen(RGB565_BLACK);

  // SD卡初始化
  sd_spi.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  if (!SD.begin(SD_CS, sd_spi, SD_SPI_SPEED, "/sd")) {
    Serial.println("Err: SD card init failed!");
    while (1);
  }

  // 背光初始化
  pinMode(BL_PIN, OUTPUT);
  digitalWrite(BL_PIN, HIGH);

  // MJPEG缓冲区初始化
  long mjpegBufSize = gfx->width() * gfx->height() * 2 / 5;
  mjpeg_buf = (uint8_t*)heap_caps_malloc(mjpegBufSize, MALLOC_CAP_8BIT);
  
  if (!mjpeg_buf) {
    Serial.println("Err: Buffer alloc failed!");
    while (1);
  }

  // 文件检测
  fileOkStart = fileExists("start.mjpeg");
  fileOkInit = fileExists("output1.mjpeg");
  fileOkDefaultLoop = fileExists("output.mjpeg");
  
  // 必需文件缺失检查
  if (!fileOkInit || !fileOkDefaultLoop) {
    Serial.println("Err: Required files missing!");
    while (1);
  }

  // 扫描扩展文件组
  scanMjpegGroups();

  // 按键中断初始化
  pinMode(BOOT_PIN, INPUT);
  attachInterrupt(digitalPinToInterrupt(BOOT_PIN), onButtonPress, FALLING);

  currentLoopFile = "output.mjpeg";
  Serial.println("Init OK");
}

// 主循环
void loop() {
  // 播放启动文件（仅1次）
  if (!playedStartFile && fileOkStart) {
    playMjpegFile("start.mjpeg"); // 启动文件允许跳过
    playedStartFile = true;
  }
  else if (!playedStartFile && !fileOkStart) {
    playedStartFile = true;
  }

  // 播放片头文件（仅1次，禁止跳过）
  if (playedStartFile && !playedInitFile) {
    playMjpegFile("output1.mjpeg", false); //传入false禁止跳过
    playedInitFile = true;
  }

  // 处理扩展组切换
  if (switchGroupRequested && groupCount > 0) {
    currentGroupIndex = (currentGroupIndex + 1) % groupCount;
    MjpegGroup &targetGroup = mjpegGroups[currentGroupIndex];
    
    playMjpegFile(targetGroup.triggerFile.c_str()); // 触发文件允许跳过
    currentLoopFile = targetGroup.loopFile;
    
    switchGroupRequested = false;
  }

  // 循环播放当前文件（允许跳过）
  playMjpegFile(currentLoopFile.c_str());
}
