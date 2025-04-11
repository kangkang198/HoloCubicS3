/***
 * Required libraries:
 * Arduino_GFX: https://github.com/moononournation/Arduino_GFX.git
 * libhelix: https://github.com/pschatzmann/arduino-libhelix.git
 */
// auto fall back to MP3 if AAC file not available
#define AAC_FILENAME "/44100.aac"
#define MP3_FILENAME "/44100.mp3"
#define MJPEG_FILENAME "/240_30fps.mjpeg"
#define FPS 30
#define MJPEG_BUFFER_SIZE (240 * 240 * 2 / 4)
#define AUDIOASSIGNCORE 1

#include <WiFi.h>
#include <SD.h>
#include <SD_MMC.h>
#include <driver/i2s.h>

#include <Arduino_GFX_Library.h>
// #define TFT_BL -1
// #define TFT_BRIGHTNESS 128
Arduino_HWSPI *bus = new Arduino_HWSPI(14 /* DC */, -1 /* CS */, 18 /* SCK */, 19 /* MOSI */, -1 /* MISO */);
Arduino_GFX *gfx = new Arduino_ST7789(bus, 20 /* RST */, 4 /* rotation */, true /* IPS */, 240 /* width */, 240 /* height */, 0 /* col offset 1 */, 0 /* row offset 1 */);

/* audio */
#include "esp32_audio_task.h"

/* MJPEG Video */
#include "MjpegClass.h"
static MjpegClass mjpeg;

int next_frame = 0;
int skipped_frames = 0;
unsigned long total_read_video_ms = 0;
unsigned long total_play_video_ms = 0;
unsigned long total_remain = 0;
unsigned long start_ms, curr_ms, next_frame_ms;
void setup()
{
  WiFi.mode(WIFI_OFF);
  Serial.begin(115200);

  // Init Video
  gfx->begin();
  gfx->fillScreen(BLACK);

#ifdef TFT_BL
  ledcAttachPin(TFT_BL, 1);     // assign TFT_BL pin to channel 1
  ledcSetup(1, 12000, 8);       // 12 kHz PWM, 8-bit resolution
  ledcWrite(1, TFT_BRIGHTNESS); // brightness 0 - 255
#endif

  Serial.println("Init I2S");
  gfx->println("Init I2S");
  esp_err_t ret_val = i2s_init(I2S_NUM_0, 44100, -1 /* MCLK */, 12 /* SCLK */, 13 /* LRCK */, 11 /* DOUT */, -1 /* DIN */);

  if (ret_val != ESP_OK)
  {
    Serial.printf("i2s_init failed: %d\n", ret_val);
  }
  i2s_zero_dma_buffer(I2S_NUM_0);

  Serial.println("Init FS");
  gfx->println("Init FS");

  // Init SD card
  SPIClass spi = SPIClass(HSPI);
  spi.begin(6/* SCK */, 7 /* MISO */, 5 /* MOSI */, 4 /* SS */);
  if (!SD.begin(4 /* SS */, spi, 80000000))
  {
    Serial.println("ERROR: File system mount failed!");
    gfx->println("ERROR: File system mount failed!");
  }
  else
  {
    bool aac_file_available = false;
    Serial.println("Open AAC file: " AAC_FILENAME);
    gfx->println("Open AAC file: " AAC_FILENAME);
    // File aFile = LittleFS.open(AAC_FILENAME);
    // File aFile = SPIFFS.open(AAC_FILENAME);
    // File aFile = FFat.open(AAC_FILENAME);
    File aFile = SD.open(AAC_FILENAME);
    // File aFile = SD_MMC.open(AAC_FILENAME);
    if (aFile)
    {
      aac_file_available = true;
    }
    else
    {
      Serial.println("Open MP3 file: " MP3_FILENAME);
      gfx->println("Open MP3 file: " MP3_FILENAME);
      // aFile = LittleFS.open(MP3_FILENAME);
      // aFile = SPIFFS.open(MP3_FILENAME);
      // aFile = FFat.open(MP3_FILENAME);
      aFile = SD.open(MP3_FILENAME);
      // aFile = SD_MMC.open(MP3_FILENAME);
    }

    if (!aFile || aFile.isDirectory())
    {
      Serial.println("ERROR: Failed to open " AAC_FILENAME " or " MP3_FILENAME " file for reading");
      gfx->println("ERROR: Failed to open " AAC_FILENAME " or " MP3_FILENAME " file for reading");
    }
    else
    {
      File vFile = SD.open(MJPEG_FILENAME);
      // File vFile = SD_MMC.open(MJPEG_FILENAME);
      if (!vFile || vFile.isDirectory())
      {
        Serial.println(F("ERROR: Failed to open " MJPEG_FILENAME " file for reading"));
        gfx->println(F("ERROR: Failed to open " MJPEG_FILENAME " file for reading"));
      }
      else
      {
          uint8_t *mjpeg_buf = (uint8_t *)malloc(MJPEG_BUFFER_SIZE);
          if (!mjpeg_buf)
          {
            Serial.println("mjpeg_buf malloc failed!");
          }
          else
          {
            Serial.println("Init video");
            gfx->println("Init video");
            mjpeg.setup(vFile, mjpeg_buf, (Arduino_TFT *)gfx, true);
            Serial.println("Start play audio task");
            gfx->println("Start play audio task");
            BaseType_t ret_val;
            if (aac_file_available)
            {
              ret_val = aac_player_task_start(&aFile, AUDIOASSIGNCORE);
            }
            else
            {
              ret_val = mp3_player_task_start(&aFile, AUDIOASSIGNCORE);
            }
            if (ret_val != pdPASS)
            {
              Serial.printf("Audio player task start failed: %d\n", ret_val);
              gfx->printf("Audio player task start failed: %d\n", ret_val);
            }

            Serial.println("Start play video");
            gfx->println("Start play video");
            start_ms = millis();
            curr_ms = millis();
            next_frame_ms = start_ms + (++next_frame * 1000 / FPS);

            while (vFile.available() && aFile.available())
            {
              // Read video
              mjpeg.readMjpegBuf();
              total_read_video_ms += millis() - curr_ms;
              curr_ms = millis();

              if (millis() < next_frame_ms) // check show frame or skip frame
              {
                // Play video
                mjpeg.drawJpg();
                total_play_video_ms += millis() - curr_ms;

                int remain_ms = next_frame_ms - millis();
                total_remain += remain_ms;
                if (remain_ms > 0)
                {
                  delay(remain_ms);
                }
              }
              else
              {
                ++skipped_frames;
                Serial.println("Skip frame");
              }

              curr_ms = millis();
              next_frame_ms = start_ms + (++next_frame * 1000 / FPS);
            }
            int time_used = millis() - start_ms;
            Serial.println("AV end");
            vFile.close();
            aFile.close();
            int played_frames = next_frame - 1 - skipped_frames;
            float fps = 1000.0 * played_frames / time_used;
            Serial.printf("Played frames: %d\n", played_frames);
            Serial.printf("Skipped frames: %d (%0.1f %%)\n", skipped_frames, 100.0 * skipped_frames / played_frames);
            Serial.printf("Time used: %d ms\n", time_used);
            Serial.printf("Expected FPS: %d\n", FPS);
            Serial.printf("Actual FPS: %0.1f\n", fps);
            Serial.printf("Read audio: %lu ms (%0.1f %%)\n", total_read_audio_ms, 100.0 * total_read_audio_ms / time_used);
            Serial.printf("Decode audio: %lu ms (%0.1f %%)\n", total_decode_audio_ms, 100.0 * total_decode_audio_ms / time_used);
            Serial.printf("Play audio: %lu ms (%0.1f %%)\n", total_play_audio_ms, 100.0 * total_play_audio_ms / time_used);
            Serial.printf("SD read MJPEG: %d ms (%0.1f %%)\n", total_read_video_ms, 100.0 * total_read_video_ms / time_used);
            Serial.printf("Play video: %d ms (%0.1f %%)\n", total_play_video_ms, 100.0 * total_play_video_ms / time_used);
            Serial.printf("Remain: %d ms (%0.1f %%)\n", total_remain, 100.0 * total_remain / time_used);

#define CHART_MARGIN 24
#define LEGEND_A_COLOR 0xE0C3
#define LEGEND_B_COLOR 0x33F7
#define LEGEND_C_COLOR 0x4D69
#define LEGEND_D_COLOR 0x9A74
#define LEGEND_E_COLOR 0xFBE0
#define LEGEND_F_COLOR 0xFFE6
#define LEGEND_G_COLOR 0xA2A5
            gfx->setCursor(0, 0);
            gfx->setTextColor(WHITE);
            gfx->printf("Played frames: %d\n", played_frames);
            gfx->printf("Skipped frames: %d (%0.1f %%)\n", skipped_frames, 100.0 * skipped_frames / played_frames);
            gfx->printf("Actual FPS: %0.1f\n\n", fps);
            int16_t r1 = ((gfx->height() - CHART_MARGIN - CHART_MARGIN) / 2);
            int16_t r2 = r1 / 2;
            int16_t cx = gfx->width() - gfx->height() + CHART_MARGIN + CHART_MARGIN - 1 + r1;
            int16_t cy = r1 + CHART_MARGIN;
            float arc_start = 0;
            float arc_end = max(2.0, 360.0 * total_read_audio_ms / time_used);
            for (int i = arc_start + 1; i < arc_end; i += 2)
            {
              gfx->fillArc(cx, cy, r1, r2, arc_start - 90.0, i - 90.0, LEGEND_D_COLOR);
            }
            gfx->fillArc(cx, cy, r1, r2, arc_start - 90.0, arc_end - 90.0, LEGEND_D_COLOR);
            gfx->setTextColor(LEGEND_D_COLOR);
            gfx->printf("Read audio:\n%0.1f %%\n", 100.0 * total_read_audio_ms / time_used);

            arc_start = arc_end;
            arc_end += max(2.0, 360.0 * total_play_audio_ms / time_used);
            for (int i = arc_start + 1; i < arc_end; i += 2)
            {
              gfx->fillArc(cx, cy, r1, r2, arc_start - 90.0, i - 90.0, LEGEND_C_COLOR);
            }
            gfx->fillArc(cx, cy, r1, r2, arc_start - 90.0, arc_end - 90.0, LEGEND_C_COLOR);
            gfx->setTextColor(LEGEND_C_COLOR);
            gfx->printf("Play audio:\n%0.1f %%\n", 100.0 * total_play_audio_ms / time_used);

            arc_start = arc_end;
            arc_end += max(2.0, 360.0 * total_read_video_ms / time_used);
            for (int i = arc_start + 1; i < arc_end; i += 2)
            {
              gfx->fillArc(cx, cy, r1, r2, arc_start - 90.0, i - 90.0, LEGEND_B_COLOR);
            }
            gfx->fillArc(cx, cy, r1, r2, arc_start - 90.0, arc_end - 90.0, LEGEND_B_COLOR);
            gfx->setTextColor(LEGEND_B_COLOR);
            gfx->printf("Read MJPEG:\n%0.1f %%\n", 100.0 * total_read_video_ms / time_used);

            arc_start = arc_end;
            arc_end += max(2.0, 360.0 * total_play_video_ms / time_used);
            for (int i = arc_start + 1; i < arc_end; i += 2)
            {
              gfx->fillArc(cx, cy, r1, 0, arc_start - 90.0, i - 90.0, LEGEND_A_COLOR);
            }
            gfx->fillArc(cx, cy, r1, 0, arc_start - 90.0, arc_end - 90.0, LEGEND_A_COLOR);
            gfx->setTextColor(LEGEND_A_COLOR);
            gfx->printf("Play video:\n%0.1f %%\n", 100.0 * total_play_video_ms / time_used);
        }
      }
    }
  }
#ifdef TFT_BL
  delay(60000);
  ledcDetachPin(TFT_BL);
#endif
}

void loop()
{
}
