// =======================================================
//   USER SETUP – Freenove ESP32 FNK0103S_4P0
//   ✅ Verified for ST7796 Display + XPT2046 Touch
// =======================================================

#define USER_SETUP_INFO "Freenove 4.0in FNK0103S_4P0 ST7796 + XPT2046"

// ---------- Display Configuration ----------
#define ST7796_DRIVER
#define TFT_WIDTH  320
#define TFT_HEIGHT 480
#define TFT_RGB_ORDER TFT_BGR
#define TFT_INVERSION_OFF

// ---------- Display SPI (VSPI) ----------
#define TFT_MOSI 13    // GPIO13  (HSPI MOSI)
#define TFT_MISO 12    // GPIO12  (HSPI MISO)
#define TFT_SCLK 14    // GPIO14  (HSPI CLK)
#define TFT_CS   15    // GPIO15  (CS)
#define TFT_DC    2    // GPIO2   (DC)
#define TFT_RST  -1    // -1 = using EN as reset
#define TFT_BL   27    // GPIO27  (Backlight)
#define TFT_BACKLIGHT_ON HIGH

// ---------- Touch Controller (XPT2046 on secondary SPI) ----------
#define TOUCH_CS    33    // Chip select
#define TOUCH_IRQ   36    // Optional IRQ
#define TOUCH_MOSI  32
#define TOUCH_MISO  39
#define TOUCH_SCLK  25
#define SPI_TOUCH_FREQUENCY 3000000

// ---------- Touch Orientation ----------
#define TOUCH_SWAP_XY
#define TOUCH_FLIP_X
//#define TOUCH_FLIP_Y


// ---------- SPI Speeds ----------
#define SPI_FREQUENCY        27000000
#define SPI_READ_FREQUENCY   20000000

// ---------- Fonts ----------
#define LOAD_GLCD
#define LOAD_FONT2
#define LOAD_FONT4
#define LOAD_FONT6
#define LOAD_FONT7
#define LOAD_FONT8
#define LOAD_GFXFF
#define SMOOTH_FONT

// ---------- Misc ----------
#define SUPPORT_TRANSACTIONS
