#include "driver/dac_continuous.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_vfs_fat.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "sdmmc_cmd.h"
#include "soc/soc_caps.h"
#include <dirent.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/unistd.h>

// server define
#define BASE_URL "http://1002seok.iptime.org:3500" // 3500->3000 port forward
#define FILE_LIST_URL                                                          \
  "http://1002seok.iptime.org:3501/filelist.txt" // 3501->3001 port forward
#define WIFI_SSID "jung"
#define WIFI_PASS "msjungjkkr1002*"
// #define WIFI_SSID "jung_tplink"
// #define WIFI_PASS "msjungjkkr1002*"
// #define WIFI_SSID "msjung"
// #define WIFI_PASS "msjungjkkr1002*"
// #define WIFI_SSID "Rolab_Workroom"
// #define WIFI_PASS "rolabmaker2023"
// #define WIFI_SSID "k-net"
// #define WIFI_PASS "knetwifi512"
// #define WIFI_SSID "zaksim1"
// #define WIFI_PASS "zaksim0910"

static char current_file_name[100] = "";
char AUDIO_URL[200] = "";

// streaming define
static TaskHandle_t streaming_start_handle = NULL;
static float vol_control = 0.3f;

// dac define
// output: pin 25
#define SAMPLE_RATE 44100
#define MIN_PROCESS_SIZE 1024 // 최소 처리 단위 (4096에서 1024로 감소)
#define MAX_BUFFER_SIZE 4096  // 8192-->4096
dac_continuous_handle_t dac_handle = NULL;
RingbufHandle_t audio_ring_buffer = NULL;
static int dac_ready = 0;

// BT detect define
#define BAUD_RATE_BT_DETECT 9600
#define UART1_TXD_PIN 19
#define UART1_RXD_PIN 18
#define CHECK_SIZE 3
#define BT_DETECT_SIZE 20
#define RX_BUF_SIZE 256
static int bt_detect_ready = 0;

// BT define
#define UART2_TXD_PIN 17
#define UART2_RXD_PIN 16
#define BAUD_RATE_BT 9600
static int bt_ready = 0;

// SD define
#define MISO 4  // 4
#define MOSI 15 // 15
#define CLK 14  // 14
#define CS 13   // 13
#define MAX_FILE_NUM 5
#define MAX_FILE_NAME_LEN 50
int file_count = 0;
EventGroupHandle_t sd_write_group;
EventGroupHandle_t sd_read_group;
#define POLLING_BIT 0x01
#define BT_DETECT_BIT 0x02
#define BT_BIT 0x04
#define DOWNLOAD_BUFFER_SIZE 8192

sdmmc_card_t *card;
esp_err_t get_latest_filename_from_server(char *filename_buffer,
                                          size_t buffer_size);
static bool bt_detect_download_done = false;
sdmmc_host_t host = SDSPI_HOST_DEFAULT();
esp_err_t sd_read_file(struct dirent *sd_dir);
static int gpio_and_spi_ready = 0;

static bool read_save_sync = false;
SemaphoreHandle_t stream_read_sync = NULL;

// adc define
static int adc_ready = 0;
static int adc_raw;
static float voltage;
////////////////////////////////////////////////////// SD

// sd initialize
esp_err_t sd_init() {

  // wait until gpio and spi init done
  while (gpio_and_spi_ready != 1) {
    ESP_LOGE("SD_INIT", "gpio and spi init is not done");
    vTaskDelay(pdMS_TO_TICKS(1000));
  }

  esp_err_t ret;

  // mount config
  ESP_LOGI("SD_INIT", "mount config");
  esp_vfs_fat_sdmmc_mount_config_t mount_config = {
      .format_if_mount_failed = false,
      .max_files = 5,
      .allocation_unit_size = 16 * 1024};

  // spi device config
  ESP_LOGI("SD_INIT", "spi config");
  sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
  slot_config.gpio_cs = CS;
  slot_config.host_id = host.slot;

  // mount start
  ESP_LOGI("SD_INIT", "Mounting filesystem");
  ret = esp_vfs_fat_sdspi_mount("/sdcard", &host, &slot_config, &mount_config,
                                &card);

  if (ret != ESP_OK) {
    if (ret == ESP_FAIL) {
      ESP_LOGE("SD_INIT",
               "Failed to mount filesystem. "
               "If you want the card to be formatted, set the "
               "CONFIG_EXAMPLE_FORMAT_IF_MOUNT_FAILED menuconfig option.");
    } else {
      ESP_LOGE("SD_INIT",
               "Failed to initialize the card (%s). "
               "Make sure SD card lines have pull-up resistors in place.",
               esp_err_to_name(ret));
    }
    return ret;
  }
  ESP_LOGI("SD_INIT", "Filesystem mounted");

  sdmmc_card_print_info(stdout, card);
  return ESP_OK;
}

typedef struct {
  FILE *file;
  size_t total_bytes;
} download_context_t;

// http evt handler
esp_err_t http_download_event_handler(esp_http_client_event_t *evt) {
  download_context_t *file_info = (download_context_t *)evt->user_data;

  switch (evt->event_id) {
  case HTTP_EVENT_ON_DATA:
    if (file_info->file) {
      // 받은 데이터를 바로 SD카드 파일에 쓰기
      size_t written = fwrite(evt->data, 1, evt->data_len, file_info->file);
      if (written != evt->data_len) {
        ESP_LOGI("HTTP_DOWNLOAD", "파일 쓰기 실패");
        return ESP_FAIL;
      }
      file_info->total_bytes += written;
      //   ESP_LOGI("HTTP_DOWNLOAD", "다운로드 중: %d bytes", evt->data_len);
    }
    break;

  case HTTP_EVENT_ON_FINISH:
    ESP_LOGI("HTTP_DOWNLOAD", "다운로드 완료: 총 %d bytes",
             file_info->total_bytes);
    break;

  case HTTP_EVENT_ERROR:
    ESP_LOGE("HTTP_DOWNLOAD", "HTTP 에러 발생");
    break;

  default:
    break;
  }
  return ESP_OK;
}

// download file to sd card from http server func
esp_err_t download_to_sd_proc(const char *filename) {
  // init
  esp_err_t ret = ESP_OK;

  do {
    ret = sd_init(); // mount
    if (ret != ESP_OK) {
      ESP_LOGE("SD", "music write from http fail!");
      ESP_LOGE("SD", "retry SD init");
      // spi_bus_free(host.slot);
      esp_vfs_fat_sdcard_unmount("/sdcard", card);
      vTaskDelay(pdTICKS_TO_MS(5000));
    }
  } while (ret != ESP_OK);
  ESP_LOGI("SD", "SD init success");
  // 1st. create file dir
  char file_dir[MAX_FILE_NUM][MAX_FILE_NAME_LEN];

  snprintf(file_dir[file_count], MAX_FILE_NAME_LEN, "/sdcard/broadcast_%d.raw",
           file_count);

  // 2nd. open file
  FILE *file_ptr = fopen(file_dir[file_count], "wb");
  if (!file_ptr) {
    ESP_LOGE("SD", "file %s open fail", file_dir[file_count]);
    ESP_LOGE("SD", "errno: %d - %s", errno, strerror(errno)); // ← 추가
    return ESP_FAIL;
  }

  // 다운로드 컨텍스트 초기화
  download_context_t download_file_info = {.file = file_ptr, .total_bytes = 0};

  // 3rd. http
  // AUDIO_URL을 filename으로 snprintf사용해서 직접 만들든 아니면
  // polling_task에서 글로벌 변수 AUDIO_URL초기화 해 놓은 것을 그대로 사용하기
  esp_http_client_config_t sd_download_config = {
      .url = AUDIO_URL,
      .event_handler = http_download_event_handler,
      .user_data = &download_file_info,
      .buffer_size = DOWNLOAD_BUFFER_SIZE,
      .timeout_ms = 30000,
  };

  ESP_LOGI("HTTP", "다운로드 시작: %s → %s", AUDIO_URL, file_dir);

  // http client create and execute
  esp_http_client_handle_t sd_download_client =
      esp_http_client_init(&sd_download_config);
  // ret ESP_OK or ESP_FAIL
  ret = esp_http_client_perform(sd_download_client);

  if (ret == ESP_OK) {
    ESP_LOGI("HTTP", "다운로드 성공: %s (%d bytes)", filename,
             download_file_info.total_bytes);
    file_count++;

  } else {
    // if http client fail delete the file
    ESP_LOGE("HTTP", "다운로드 실패: %s", esp_err_to_name(ret));
    // 실패 시 파일 삭제
    unlink(file_dir[file_count]);
    esp_http_client_cleanup(sd_download_client);
    esp_vfs_fat_sdcard_unmount("/sdcard", card);
    ESP_LOGI("SD", "Card unmounted");
    // spi_bus_free(host.slot);

    return ESP_FAIL;
  }

  // 4th. file download to sd complete and close file
  fclose(file_ptr);
  esp_http_client_cleanup(sd_download_client);
  esp_vfs_fat_sdcard_unmount("/sdcard", card);
  ESP_LOGI("SD", "Card unmounted");
  // spi_bus_free(host.slot);

  return ESP_OK;
}

// sd save task
void sd_save_task(void *param) {
  char filename[100];

  while (1) {

    // for checking current bits
    EventBits_t current_bit_bt_detect;
    current_bit_bt_detect = xEventGroupGetBits(sd_write_group);
    ESP_LOGI("SD_SAVE", "before wait bits polling bit: %d",
             (current_bit_bt_detect & POLLING_BIT) ? 1 : 0);

    EventBits_t sd_bits_save_cond;

    // 1st sd card saving
    // wait until polling bit(polling task 1sec periode) and bt_detect bit are
    // setted
    if ((sd_bits_save_cond = xEventGroupWaitBits(
             sd_write_group, BT_DETECT_BIT | POLLING_BIT, pdFALSE, pdTRUE,
             portMAX_DELAY)) == (BT_DETECT_BIT | POLLING_BIT)) {
      // to check current polling bit
      current_bit_bt_detect = xEventGroupGetBits(sd_write_group);
      ESP_LOGI("SD_SAVE", "after wait bits polling bit: %d",
               (current_bit_bt_detect & POLLING_BIT) ? 1 : 0);

      if ((sd_bits_save_cond & (BT_DETECT_BIT | POLLING_BIT)) ==
              (BT_DETECT_BIT | POLLING_BIT) &&
          !bt_detect_download_done) { // bt_detect=yes, polling bit=1,
        // bt_detect_download_done추가한 이유:다운로드 도중
        // 또 bt_detect "no"가 들어와도 다시 저장지 않도록
        // 하기 위함

        ESP_LOGI("SD_SAVE", "Lora==\"no\" & new file exists");
        ESP_LOGI("SD_SAVE", "START SAVING .raw to SDCARD");
        // 현재 스트리밍 중인 파일 이름을 확인하고 음원을 sd에 저장한다.
        // http streaming
        if (get_latest_filename_from_server(filename, sizeof(filename)) ==
            ESP_OK) {
          // bt_detect_download_done가 true로 바뀌어야 더이상 음원이 재생되는
          // 동안 polling bit=1, bt deteect bit=1이 되어도 접근이 불가함.
          bt_detect_download_done = true;
          // sd card빠져있거나 에러나면 sd init이 안되므로 sd_init을 계속 시도
          esp_err_t ret = download_to_sd_proc(filename);
          if (ret == ESP_OK) {
            ESP_LOGI("SD_SAVE", "file save to sd complete!");
          } else {
            ESP_LOGI("SD_SAVE", "file save fail!");
          }
          // download_to_sd_proc 종료되면(한방에 .raw가 다운로드
          // 됨)polling_bit=0으로 복귀 download_to_sd_proc실행기간동안은
          // polling_bit=1을 유지함.
          xEventGroupClearBits(sd_write_group, POLLING_BIT);
        }
      }
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

// sd_read_task
void sd_read_task(void *param) {
  char filename[100];

  while (1) {

    // for checking current bits
    EventBits_t current_bit_bt;
    current_bit_bt = xEventGroupGetBits(sd_read_group);
    ESP_LOGI("SD_READ", "before wait bits bt bit: %d",
             (current_bit_bt & BT_BIT) ? 1 : 0);
    EventBits_t sd_bits_read;

    // 2nd. BT_BIT를 wait
    if ((sd_bits_read = xEventGroupWaitBits(sd_read_group, BT_BIT, pdFALSE,
                                            pdTRUE, portMAX_DELAY)) == BT_BIT) {

      // 현재비트 uart출력으로 확인하려고
      current_bit_bt = xEventGroupGetBits(sd_read_group);
      ESP_LOGI("SD_READ", "after wait bits bt bit: %d",
               (current_bit_bt & BT_BIT) ? 1 : 0);

      ////////////////////중복해제 이슈때문에 pdTRUE사용
      BaseType_t mutex_taken = pdFALSE;
      if (xSemaphoreTake(stream_read_sync, portMAX_DELAY) == pdTRUE) {
        mutex_taken = pdTRUE;
      }
      // xSemaphoreTake(stream_read_sync, portMAX_DELAY);

      current_bit_bt = xEventGroupGetBits(sd_read_group);
      if ((current_bit_bt & BT_BIT) == 0) {
        ESP_LOGI("SD_READ", "BT_BIT cleared during wait - aborting");
        xSemaphoreGive(stream_read_sync);
        continue;
      }
      ////////////////////
      // BT_BIT가 맞는 경우 read all proc실시
      if ((sd_bits_read & BT_BIT) == BT_BIT) {

        ESP_LOGI("SD_READ", "User req saved music");

        // 1. sd init과정
        esp_err_t ret = ESP_OK;
        do {
          ret = sd_init(); // sd init에 마운트도 포함해서 모든 초기전과정
                           // 포함해야됨. 왜냐하면 sd카드를 한번 뺐다가 다시
                           // 꽂은경우 초기화 전 과정 필요
          if (ret != ESP_OK) {
            ESP_LOGE("SD_READ", "music read from sd fail!");
            ESP_LOGE("SD_READ", "retry SD init");
            // spi_bus_free(host.slot);
            esp_vfs_fat_sdcard_unmount("/sdcard", card);
            vTaskDelay(pdMS_TO_TICKS(4000));
          }
        } while (ret != ESP_OK);
        ESP_LOGI("SD_READ", "sd_init success");

        // sd_init이 opendir보다 먼저 와야됨
        // 2. open_dir
        DIR *sd_stream;
        while ((sd_stream = opendir("/sdcard")) == NULL) {
          ESP_LOGE("SD_READ", "opendir fail");
          vTaskDelay(pdMS_TO_TICKS(2000));
        }
        ESP_LOGI("SD_READ", "opendir success");

        struct dirent *sd_dir;
        bool has_more_files = false;

        // readdir앞에는 sd init, opendir이 먼저 와야된다.
        // 3. readdir과정, 파일 스트림의 1개 dir만 읽는다
        if ((sd_dir = readdir(sd_stream)) == NULL) {
          ESP_LOGI("SD_READ", "readdir ponit NULL");
        }
        ESP_LOGI("SD_READ", "readdir success");

        // 필요없는 내용은 제끼고 다음위치로 ptr이동
        if (strcmp(sd_dir->d_name, "System Volume Information") == 0) {
          sd_dir = readdir(sd_stream);
        }

        // sd_init과정 완료
        // opendir완료
        // read_dir완료
        // 4. dir stream의 dir를 모두 다 읽을 때까지 sd_read_file하기
        char file_dir[512] = "";

        // read all file
        while (1) {
          // 해당 dir의 내용을 읽고, 바로 삭제
          ESP_LOGI("SD_READ", "%s", sd_dir->d_name);
          // fopen, fread하고 fclose하고 종료(file read)
          if (sd_read_file(sd_dir) == ESP_OK) {
            ESP_LOGI(
                "SD_READ", "reading %s complete",
                sd_dir->d_name); // 해당 dir의 파일내용 읽었으면 해당 dir unlink

            // 1. 해당 dir만 삭제, unlink
            snprintf(file_dir, 400, "/sdcard/%s", sd_dir->d_name);
            if (unlink(file_dir) == 0) {
              ESP_LOGI("SD_READ", "%s delete success", file_dir);
            }
          } else { // sd_read_file 에러나도 삭제
            ESP_LOGE("SD_READ", "sd_read_file error");

            // 1. 해당 dir만 삭제, unlink
            snprintf(file_dir, 256, "/sdcard/sd_dir");
            if (unlink(file_dir) == 0) {
              ESP_LOGI("SD_READ", "%s delete success", file_dir);
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
          }

          // 2. 현재 file stream ptr 위치 기억
          long current_pos = telldir(sd_stream);

          // 3. 남은 파일 확인을 위해 offset1칸이동 by readdir(ptr 1칸 이동)
          if ((sd_dir = readdir(sd_stream)) != NULL) {
            ESP_LOGI("SD_READ", "some dir is exist");
            // 만약 1개 이상 남았으면 직전 ptr이 가리키는 위치로 다시 복귀
            seekdir(sd_stream, current_pos);
          } else { // 다 읽었으면 whlie(1)을 탈출하고 끝내기
            ESP_LOGI("SD_READ", "no file left");
            break;
          }
        }

        // sd_read_file끝나면 1. cloese dir, 2. sd카드 unmount, 3.
        // spifree하기--x,이건 제외

        // 4. close dir, 스트림 닫기
        closedir(sd_stream);
        file_count = 0;
        xEventGroupClearBits(sd_read_group, BT_BIT);

        // 5. unmount
        esp_vfs_fat_sdcard_unmount("/sdcard", card);

        // 3. spi free
        // spi_bus_free(host.slot);
        ESP_LOGI("SD_READ", "Card unmounted");
        ESP_LOGI("SD_READ", "Card reading END");
        ///////중복해제 방지 위해
        if (mutex_taken == pdTRUE) {
          xSemaphoreGive(stream_read_sync);
        }
        // xSemaphoreGive(stream_read_sync);
      }
    }
    vTaskDelay(pdMS_TO_TICKS(1000)); // sd_read_task를 1초단위로, 한번
                                     // read하기로 했으면 모든 파일 다 읽음
  }
} // task while

esp_err_t sd_read_file(struct dirent *sd_dir) {
  // init
  char file_dir[512] = "";
  // file dir making
  snprintf(file_dir, 512, "/sdcard/%s", sd_dir->d_name); // FIFO

  ESP_LOGI("SD_READ", "sd read proc file dir is %s", file_dir);
  // 기존의 sd_init()호출 부분을 빼버림
  FILE *file_ptr = NULL;

  uint32_t delay_ms;
  const int sample_count = 512; //
  int16_t *pcm_buffer = (int16_t *)malloc(sample_count * sizeof(int16_t));
  size_t samples_read;
  uint8_t *dac_buffer = NULL;

  // 1. open file
  while ((file_ptr = fopen(file_dir, "rb")) == NULL) {
    vTaskDelay(pdMS_TO_TICKS(1000));
    ESP_LOGE("SD_READ", "file open fail");
  }

  ESP_LOGI("SD_READ", "file open success");

  // 2. read file
  while ((samples_read =
              fread(pcm_buffer, sizeof(int16_t), sample_count, file_ptr)) > 0) {
    // ESP_LOGI("SD_READ", "READ SAMPLES number is %zu(%zu bytes)",
    // samples_read,
    //  samples_read * 2);
    dac_buffer = (uint8_t *)malloc(samples_read);
    if (dac_buffer == NULL) {
      ESP_LOGE("SD_READ", "Memory allocation failed");
      return ESP_ERR_NO_MEM;
    }
    // 16->8
    for (size_t i = 0; i < samples_read; i++) {
      // dac_buffer[i] = (pcm_buffer[i] + 32768) >> 8;
      int16_t adjust = (int16_t)(pcm_buffer[i] * vol_control);
      dac_buffer[i] = (adjust + 32768) >> 8;
    }

    // save to ring buffer
    if (xRingbufferSend(audio_ring_buffer, dac_buffer, samples_read,
                        pdMS_TO_TICKS(1000)) !=
        pdTRUE) { // pdMS_TO_TICKS(40)-->1000
      ESP_LOGE("SD_READ", "audio_buffer is full");
    }
    free(dac_buffer);

    // s_dac_wait_to_load_dma_data Get available descriptor timeout발생 방지
    // delay_ms = (samples_read * 1000) / 44100;
    // if (delay_ms > 0) {
    //   vTaskDelay(pdMS_TO_TICKS(delay_ms));
    // }
  }

  free(pcm_buffer);
  // 3. closing the file
  fclose(file_ptr);

  // 링 버퍼 비우기
  void *item;
  size_t item_size;
  int cleared_count = 0;

  while ((item = xRingbufferReceive(audio_ring_buffer, &item_size, 0)) !=
         NULL) {
    // 타임아웃 0 → 데이터 있으면 즉시 받고, 없으면 즉시 NULL 리턴
    vRingbufferReturnItem(audio_ring_buffer, item);
    cleared_count++;
  }

  ESP_LOGI("CLEAR", "Cleared %d items from ring buffer", cleared_count);

  // Ring Buffer가 16378이상 free될때까지 대기
  int cur_ring_buf_size = 0;
  while ((cur_ring_buf_size = xRingbufferGetCurFreeSize(audio_ring_buffer)) <
         16376) {
    vTaskDelay(pdMS_TO_TICKS(50));
    ESP_LOGI("CLEAR", "current free size of ring buf is %d", cur_ring_buf_size);
  }
  // int wait_count = 0;
  // while (xRingbufferGetCurFreeSize(audio_ring_buffer) < 32768 &&
  //        wait_count < 100) {
  //   vTaskDelay(pdMS_TO_TICKS(50));
  //   wait_count++;
  // }

  //  DAC를 잠깐 정지하고 다시 시작
  dac_continuous_disable(dac_handle);
  vTaskDelay(pdMS_TO_TICKS(100)); // 100ms 대기
  dac_continuous_enable(dac_handle);

  ESP_LOGI("SD_READ", "DAC reset complete");
  /*
  // delete the raw file
  if (unlink(file_dir) == 0) {
    ESP_LOGI("SD_READ", "%s delete success", file_dir);
  }


  esp_vfs_fat_sdcard_unmount("/sdcard", card);
  spi_bus_free(host.slot);
  ESP_LOGI("SD_READ", "Card unmounted");
  ESP_LOGI("SD_READ", "Card reading END");
*/
  return ESP_OK;
}

////////////////////////////////////////////////////// wifi
void wifi_connect_and_wait() {
  wifi_config_t wifi_config = {
      .sta =
          {
              .ssid = WIFI_SSID,
              .password = WIFI_PASS,
          },
  };

  esp_netif_init();
  esp_event_loop_create_default();
  esp_netif_create_default_wifi_sta();

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  esp_wifi_init(&cfg);
  esp_wifi_set_mode(WIFI_MODE_STA);
  esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
  esp_wifi_start();
  esp_wifi_connect();
  ESP_LOGI("WIFI", "WiFi 연결 중...");
  vTaskDelay(pdMS_TO_TICKS(5000));

  wifi_ap_record_t ap_info;
  while (esp_wifi_sta_get_ap_info(&ap_info) !=
         ESP_OK) { // 연결될 때까지 계속 연결시도
    esp_wifi_connect();
    vTaskDelay(pdMS_TO_TICKS(2000));
  }

  vTaskDelay(pdMS_TO_TICKS(1000));
  ESP_LOGI("WIFI", "WiFi 연결 완료");
}

////////////////////////////////////////////////////// dac
void audio_output_task(void *param) {
  uint8_t *audio_data;
  size_t item_size;

  while (dac_ready == 0) {
    ESP_LOGE("DAC", "dac is not ready");
    vTaskDelay(pdMS_TO_TICKS(1000));
  }

  while (1) {
    audio_data = (uint8_t *)xRingbufferReceive(
        audio_ring_buffer, &item_size,
        pdMS_TO_TICKS(20)); // *edit pdMS_TO_TICKS(200)->pdMS_TO_TICKS(20)
    // ESP_LOGI("DAC", "오디오 태스크 실행 코어: %d", xPortGetCoreID());
    if (audio_data != NULL) {
      size_t bytes_written = 0;

      // 타임아웃을 늘려서 안정성 확보
      esp_err_t ret = dac_continuous_write(dac_handle, audio_data, item_size,
                                           &bytes_written, 500);

      /*
            if (ret != ESP_OK) {
        ESP_LOGW("DAC", "DAC 쓰기 실패: %s", esp_err_to_name(ret));
        // DAC 에러 시 해제
        dac_continuous_disable(dac_handle);
        vTaskDelay(pdMS_TO_TICKS(
            10)); // 10ms만 대기 음원 끊김없이 출력하려면 반응시간 중요

        // DAC다시 시작
        dac_continuous_enable(dac_handle);
        ESP_LOGW("DAC", "DAC 빠른 복구 완료");
      }
       */

      vRingbufferReturnItem(audio_ring_buffer, (void *)audio_data);
    }
  }
}

////////////////////////////////////////////////////// http

// filelist.txt에 기록된 filename을 추출하는 function.
// buffer size는 그냥 추출하려고 하는 filename의 크기
esp_err_t get_latest_filename_from_server(char *filename_buffer,
                                          size_t buffer_size) {

  // to get filelist.txt's contents
  esp_http_client_config_t config = {
      .url = FILE_LIST_URL,
      .timeout_ms = 10000,
  };
  esp_http_client_handle_t client = esp_http_client_init(&config);
  esp_err_t err = esp_http_client_open(client, 0);

  if (err != ESP_OK) {
    ESP_LOGE("GET FILELIST", "HTTP 연결 실패: %s", esp_err_to_name(err));
    esp_http_client_cleanup(client);
    vTaskDelay(pdMS_TO_TICKS(2000));
    return err;
  }

  // http header에서 content_length정보 부분을 return 하는 함수
  int content_length = esp_http_client_fetch_headers(client);

  if (content_length > 0 && content_length < buffer_size) {
    int data_read = esp_http_client_read_response(
        client, filename_buffer,
        content_length); // 본문(body)에 담긴 내용을 가져오는 함수
    filename_buffer[data_read] = '\0';

    // CR, LF (줄바꿈) 제거
    char *newline = strchr(filename_buffer, '\n');
    if (newline)
      *newline = '\0';

    char *carriage = strchr(filename_buffer, '\r');
    if (carriage)
      *carriage = '\0';

    // ESP_LOGI("GET FILELIST", "서버에서 받은 파일명: %s", filename_buffer);
  }

  esp_http_client_close(client);
  esp_http_client_cleanup(client);
  return ESP_OK;
}

//  filelist.txt polling handler
void file_polling_task(void *param) {
  char filename_buffer[100] = "";
  // ESP_LOGI("POLLING", "=======Check file's change repeatly=======");

  // check repeatly, get_latest_filename_from_server에서 http resp fail해도
  // while문안에 있으므로 다시 req시도
  while (1) {

    if (ESP_OK == get_latest_filename_from_server(filename_buffer,
                                                  sizeof(filename_buffer))) {
      // if new file exist, restart download http
      if (strcmp(filename_buffer, current_file_name) != 0) {
        // event group, setting polling bit
        xEventGroupSetBits(sd_write_group, POLLING_BIT);

        // EventBits_t current_bits = xEventGroupGetBits(sd_write_group);
        // ESP_LOGI("POLLING", "polling bit: %d",
        //          (current_bits & POLLING_BIT) ? 1 : 0);

        // 서버에 올라간 파일 명이 다르면 steraming을 허용한다. 파일스트리밍용
        // 동기화 장치
        xTaskNotifyGive(streaming_start_handle);
        // ESP_LOGI("POLLING", "new file name is %s", filename_buffer);

        // make AUDIO_URL for http_streaming
        sprintf(AUDIO_URL, "%s/%s", BASE_URL, filename_buffer);
        // current filename update
        strcpy(current_file_name, filename_buffer);

        // bt detect one shot flag
        // 현재 재생되는 음원은 단 1회만 저장하도록 하기 위함
        // 만약 현재 재생되는 음원이 끝나고 한번 다운받은 상태이면
        // bt_detect_download_done=true 상태라 다음 음원이 들어오기 전에
        // bt_detect_download_done=true를 유지함
        bt_detect_download_done = false;

      } else {
        // ESP_LOGI("POLLING", "file is still %s", filename_buffer);
      }

    } else {
      ESP_LOGE("POLLING", "no filelist.txt in server");
      vTaskDelay(pdMS_TO_TICKS(2000));
    }
    // 1초에 한번 호출된다.
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

uint8_t dac_buffer[1024];
// http handler
esp_err_t http_event_handler(esp_http_client_event_t *evt) {
  static uint8_t temp_buffer[MAX_BUFFER_SIZE];
  static size_t temp_pos = 0;

  switch (evt->event_id) {
  case HTTP_EVENT_ON_DATA:
    // ESP_LOGI("HTTP", "데이터 수신: %d bytes", evt->data_len);

    // 받은 데이터를 임시 버퍼에 저장
    if (temp_pos + evt->data_len <= MAX_BUFFER_SIZE) {
      memcpy(temp_buffer + temp_pos, evt->data, evt->data_len);
      temp_pos += evt->data_len; // 누적 저장 바이트
    } else {
      ESP_LOGW("HTTP", "임시 버퍼 오버플로우 방지");
      return ESP_FAIL;
    }

    // temp_pos가 2048byte보다 작아질 때까지 dac출력
    // temp_pos가 2048byte까지 찰 때까지 대기
    int16_t *pcm_in = (int16_t *)temp_buffer;
    while (temp_pos >= 2048) {

      for (size_t i = 0; i < 1024; i++) {
        // 16bit (-32768~32767) → 8bit (0~255) 변환
        int16_t adjust = (int16_t)(pcm_in[i] * vol_control);
        dac_buffer[i] = (adjust + 32768) >> 8;
      }

      // 링버퍼로 네트워크와 dac의 속력 조절
      if (xRingbufferSend(audio_ring_buffer, dac_buffer, 1024,
                          pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGW("HTTP", "오디오 버퍼 가득참 - 데이터 손실");
      }

      // 처리된 데이터 제거
      temp_pos -= 2048;
      if (temp_pos > 0) {
        memmove(temp_buffer, temp_buffer + 2048, temp_pos);
      }
    }
    break;

  case HTTP_EVENT_ON_FINISH:
    // ESP_LOGI("HTTP", "파일 스트리밍 완료");

    // 남은 데이터도 처리
    if (temp_pos > 0) {
      int16_t *pcm_in = (int16_t *)temp_buffer;

      for (size_t i = 0; i < temp_pos / 2; i++) {
        int16_t adjust = (int16_t)(pcm_in[i] * vol_control);
        dac_buffer[i] = (adjust + 32768) >> 8;
      }
      xRingbufferSend(audio_ring_buffer, dac_buffer, temp_pos / 2,
                      pdMS_TO_TICKS(1000));

      temp_pos = 0;
    }
    break;

  case HTTP_EVENT_ERROR:
    ESP_LOGE("HTTP", "HTTP 에러 발생");
    break;

  default:
    break;
  }

  return ESP_OK;
}

// music streaming task
void http_streaming_task(void *param) {
  while (1) {

    // streaming_start_handle = xTaskGetCurrentTaskHandle();
    // file_polling_task의 xtasknotifygive로부터의 알림을 기다림, 새 음원이
    // 업로드 된 경우에 스트리밍 정확히 딱 !! 1번씩만 하기 위함
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    // mutex take가 notifytake보다 앞에 있으면
    // 이미 기존 음원이 서버에 계속 올라와 있는 경우 mutex를 또 가져가
    // mutex가져간 상태에서 stop하는 문제 발생

    BaseType_t mutex_taken = pdFALSE;
    if (xSemaphoreTake(stream_read_sync, portMAX_DELAY) == pdTRUE) {
      mutex_taken = pdTRUE;
    }
    // xSemaphoreTake(stream_read_sync, portMAX_DELAY);
    esp_http_client_config_t config = {.url = AUDIO_URL,
                                       .event_handler = http_event_handler,
                                       .buffer_size = 2048, //
                                       .timeout_ms = 30000,
                                       .keep_alive_enable = true};
    // ESP_LOGI("HTTP", "서버에서 오디오 스트리밍 시작: %s", AUDIO_URL);
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_err_t err = esp_http_client_perform(client);

    if (err == ESP_OK) {
      // ESP_LOGI("HTTP", "스트리밍 성공");
    } else {
      // ESP_LOGE("HTTP", "스트리밍 실패: %s", esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);

    // ESP_LOGI("HTTP", "HTTP 스트리밍 태스크 종료");
    // 스트리밍 중인데 폰에서 yes를 보내면 BT_SET이 된다.
    // 그리고 스트리밍 종료후에 자동으로 sd카드에 저장된 음원이 출력이된다.
    // 스트리밍 종료 됨과 동시에 폰에서 yes를 1분 전에 눌렀는데 이어서 sd카드에
    // 저장된 음원이 재생된다는 건 좀 이상함.항상 값을 최신으로 유지하는게 더
    // 자연스러움

    // 이를 방지하게 위해, 스트리밍 중이지 않고 bt yes를 보내야 sd에 저장된 것이
    // 출력되도록 하기 위함
    xEventGroupClearBits(sd_read_group, BT_BIT);

    if (mutex_taken == pdTRUE) {
      xSemaphoreGive(stream_read_sync);
    }

    // xSemaphoreGive(stream_read_sync);
  }
  // vTaskDelete(NULL); // 태스크 자체 삭제
}

////////////////////////////////////////////////////// bt_detect
// 특정 패턴을 찾는 함수
int find_pattern(char *data, int data_len, char *pattern, int pattern_len) {
  for (int i = 0; i <= data_len - pattern_len; i++) {
    int match = 1;
    for (int j = 0; j < pattern_len; j++) {
      if (data[i + j] != pattern[j]) {
        match = 0;
        break;
      }
    }
    if (match)
      return 1;
  }
  return 0;
}

void bt_detect_task() {

  while (bt_detect_ready == 0) {
    ESP_LOGE("BT_DETECT", "BT_DETECT is not ready yet");
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
  // receive "detect" or "nondetect"
  // char detected[BT_DETECT_SIZE] = {0x79, 0x65, 0x73}; // yes
  static char undetected[BT_DETECT_SIZE] = {0x6E, 0x6F}; // no
  static char data[BT_DETECT_SIZE];
  memset(data, 0, BT_DETECT_SIZE);
  uart_flush_input(UART_NUM_1);

  while (1) {
    const int rx_bytes = uart_read_bytes(
        UART_NUM_1, data, BT_DETECT_SIZE,
        10 / portTICK_PERIOD_MS); // bt_detect delay가 dac출력에 영향 미침
    // data[rx_bytes] = '\0';
    vTaskDelay(pdMS_TO_TICKS(500));
    if (rx_bytes > 0) {
      // ESP_LOGI("LORA", "rx_bytes:%d", rx_bytes);
      for (int i = 0; i < rx_bytes; i++) {
        // ESP_LOGI("LORA", "data[%d]: 0x%02X", i, (unsigned char)data[i]);
      }

      if (find_pattern(data, rx_bytes, undetected, 2)) {
        // ESP_LOGI("LORA", "no people"); // return no==>no people
        // sd write group의 bt_detect_bit
        xEventGroupSetBits(sd_write_group, BT_DETECT_BIT);

      } else {
        // ESP_LOGI("LORA", "people exist");
        xEventGroupClearBits(sd_write_group, BT_DETECT_BIT);
      }
    } else {
      // ESP_LOGI("LORA", "LORA 0 byte received");
      xEventGroupClearBits(sd_write_group, BT_DETECT_BIT);
    }
    // vTaskDelay(pdMS_TO_TICKS(1000)); // delay 시간 적절히 조절
  }
  // vTaskDelete(NULL); // 태스크 자체 삭제
}

////////////////////////////////////////////////////// bluetooth

void bluetooth_task(void *param) {
  adc_oneshot_unit_handle_t adc1_handle = (adc_oneshot_unit_handle_t)param;

  while ((adc_ready == 0) | (bt_ready == 0)) {
    ESP_LOGE("BLUETOOTH", "BLUETOOTH or ADC is not ready yet");
    vTaskDelay(pdMS_TO_TICKS(1000));
  }

  // ESP_LOGI("BLUETOOTH", "BT_BIT explicitly cleared at startup");
  static char from_phone[RX_BUF_SIZE]; // to make data string
  while (1) {
    //////// ADC task
    ESP_ERROR_CHECK(adc_oneshot_read(adc1_handle, ADC_CHANNEL_7, &adc_raw));
    voltage = adc_raw * 3.3 / 4095.0;
    // ESP_LOGI("ADC", "ADC%d Channel[%d] Raw Data: %d Voltage: %.3fV",
    // ADC_UNIT_1,
    //          ADC_CHANNEL_7, adc_raw, voltage);

    if (voltage < 0.9) {
      // ESP_LOGI("ADC", "1");
      vol_control = 0.35f;
    } else if (voltage >= 1.0 && voltage < 1.1) {
      // ESP_LOGI("ADC", "2");
      vol_control = 0.45f;
    } else if (voltage >= 1.1 && voltage < 1.2) {
      // ESP_LOGI("ADC", "3");
      vol_control = 0.6f;
    } else if (voltage >= 1.2 && voltage < 1.3) {
      // ESP_LOGI("ADC", "4");
      vol_control = 0.8f;
    } else if (voltage >= 1.3) {
      // ESP_LOGI("ADC", "5");
      vol_control = 1.0f;
    }

    //////// BT task
    const int rx_bytes = uart_read_bytes(UART_NUM_2, from_phone, RX_BUF_SIZE,
                                         1000 / portTICK_PERIOD_MS);
    if (rx_bytes > 0) {
      if (find_pattern(from_phone, rx_bytes, "yes", 3)) {
        // ESP_LOGI("BLUETOOTH", "user play music");
        // set bt bits until clear
        xEventGroupSetBits(sd_read_group, BT_BIT);
        // ESP_LOGI("BLUETOOTH", "bluetooth read %d bytes", rx_bytes);
      }
    } else {
      // ESP_LOGI("BLUETOOTH", "bluetooth nothing read");
      xEventGroupClearBits(sd_read_group, BT_BIT);
    }
    vTaskDelay(pdMS_TO_TICKS(300));
  }

  // vTaskDelete(NULL);
}

void spi_init() {
  ////////////////// spi init
  host.max_freq_khz = 5000;

  spi_bus_config_t bus_cfg = {
      .mosi_io_num = MOSI,
      .miso_io_num = MISO,
      .sclk_io_num = CLK,
      .quadwp_io_num = -1,
      .quadhd_io_num = -1,
      .max_transfer_sz = 4000,
  };

  esp_err_t ret = ESP_OK;
  while ((ret = spi_bus_initialize(host.slot, &bus_cfg, SDSPI_DEFAULT_DMA)) !=
         ESP_OK) {
    ESP_LOGE("SPI_INIT", "Failed to initialize spi bus.");
    ESP_LOGE("SPI_INIT", "Retrying spi init...");

    spi_bus_free(host.slot); // spi init fail하면 재시도
    vTaskDelay(pdMS_TO_TICKS(4000));
  }
  ESP_LOGI("SPI_INIT", "spi init success");
}
////////////////////////////////////////////////////// main
void app_main(void) {
  ESP_LOGI("MAIN", "ESP32 오디오 스트리밍 시작");

  // mutex 생성
  do {
    stream_read_sync = xSemaphoreCreateMutex();
    ESP_LOGE("MAIN", "Failed to create mutex");
    vTaskDelay(pdMS_TO_TICKS(1000));
  } while (stream_read_sync == NULL);

  ESP_LOGI("MAIN", "Mutex created successfully");

  ////////////////// spi init
  spi_init();

  // CS PIN config
  gpio_config_t spi_cs_config = {
      .pin_bit_mask = 1ULL << CS,
      .mode = GPIO_MODE_OUTPUT,
      .pull_up_en = GPIO_PULLUP_ENABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
  };
  gpio_config(&spi_cs_config);

  gpio_and_spi_ready = 1;
  // 0. spi&gpio init
  // 1. mount config
  // 2. spi device config
  // 3. sd mount순으로 돼야 함
  ESP_LOGI("SD_INIT", "sd init done");

  ////////////////// dac init
  dac_continuous_config_t dac_cfg = {
      .chan_mask = DAC_CHANNEL_MASK_CH0,
      .chan_mode = DAC_CHANNEL_MODE_SIMUL,
      .desc_num = 10,   // 10 descriptor 개수 증가
      .buf_size = 2048, // 4096-->1024 더 작은 버퍼로 지연 시간 감소
      .freq_hz = SAMPLE_RATE,
      .offset = 0,
      .clk_src = DAC_DIGI_CLK_SRC_APLL};
  esp_err_t ret = ESP_OK;
  ret = dac_continuous_new_channels(&dac_cfg, &dac_handle);
  if (ret != ESP_OK) {
    ESP_LOGE("DAC", "DAC 초기화 실패");
    vTaskDelay(pdMS_TO_TICKS(000));
    return;
  }
  dac_continuous_enable(dac_handle);

  dac_ready = 1;
  ESP_LOGI("DAC", "DAC 초기화 완료");

  ////////////////// ADC init
  // ADC1 Init
  adc_oneshot_unit_handle_t adc1_handle;
  adc_oneshot_unit_init_cfg_t init_config1 = {
      .unit_id = ADC_UNIT_1,
  };
  ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config1, &adc1_handle));

  // ADC1 Config
  adc_oneshot_chan_cfg_t config = {
      .atten = ADC_ATTEN_DB_12,
      .bitwidth = ADC_BITWIDTH_DEFAULT,
  };
  ESP_ERROR_CHECK(
      adc_oneshot_config_channel(adc1_handle, ADC_CHANNEL_7, &config));

  adc_ready = 1;
  ESP_LOGI("ADC", "ADC 초기화 완료");

  ////////////////// BT_DETECT init
  // gpio config
  gpio_config_t bt_detect_rx_config = {
      .pin_bit_mask = 1ULL << UART1_RXD_PIN,
      .mode = GPIO_MODE_INPUT,
      .pull_up_en = GPIO_PULLUP_ENABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
  };
  gpio_config_t bt_detect_tx_config = {
      .pin_bit_mask = 1ULL << UART1_TXD_PIN,
      .mode = GPIO_MODE_OUTPUT,
      .pull_up_en = GPIO_PULLUP_ENABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
  };
  gpio_config(&bt_detect_rx_config);
  gpio_config(&bt_detect_tx_config);

  // uart init
  const uart_config_t uart_config_bt_detect = {
      .baud_rate = BAUD_RATE_BT_DETECT,
      .data_bits = UART_DATA_8_BITS,
      .parity = UART_PARITY_DISABLE,
      .stop_bits = UART_STOP_BITS_1,
      .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
      .source_clk = UART_SCLK_DEFAULT,
  };

  uart_driver_install(UART_NUM_1, RX_BUF_SIZE * 2, 0, 0, NULL, 0);
  uart_param_config(UART_NUM_1, &uart_config_bt_detect);
  uart_set_pin(UART_NUM_1, UART1_TXD_PIN, UART1_RXD_PIN, UART_PIN_NO_CHANGE,
               UART_PIN_NO_CHANGE);

  bt_detect_ready = 1;
  ESP_LOGI("LORA", "LORA 초기화 완료");

  ////////////////// bt init
  //  gpio config
  gpio_config_t bt_rx_config = {
      .pin_bit_mask = 1ULL << UART2_RXD_PIN,
      .mode = GPIO_MODE_INPUT,
      .pull_up_en = GPIO_PULLUP_ENABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
  };
  gpio_config_t bt_tx_config = {
      .pin_bit_mask = 1ULL << UART2_TXD_PIN,
      .mode = GPIO_MODE_OUTPUT,
      .pull_up_en = GPIO_PULLUP_ENABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
  };
  gpio_config(&bt_rx_config);
  gpio_config(&bt_tx_config);
  // uart init
  const uart_config_t uart_config_bt = {
      .baud_rate = BAUD_RATE_BT,
      .data_bits = UART_DATA_8_BITS,
      .parity = UART_PARITY_DISABLE,
      .stop_bits = UART_STOP_BITS_1,
      .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
      .source_clk = UART_SCLK_DEFAULT,
  };

  uart_driver_install(UART_NUM_2, RX_BUF_SIZE * 2, 0, 0, NULL, 0);
  uart_param_config(UART_NUM_2, &uart_config_bt);
  uart_set_pin(UART_NUM_2, UART2_TXD_PIN, UART2_RXD_PIN, UART_PIN_NO_CHANGE,
               UART_PIN_NO_CHANGE);
  uart_flush_input(UART_NUM_2);

  bt_ready = 1;
  ESP_LOGI("BLUETOOTH", "BLUETOOTH 초기화 완료");

  ////////////////// 기타 init

  nvs_flash_init();
  wifi_connect_and_wait();

  ////////////////// ringbuf init
  audio_ring_buffer = xRingbufferCreate(32768, RINGBUF_TYPE_NOSPLIT);

  if (audio_ring_buffer == NULL) {
    ESP_LOGE("MAIN", "Ring Buffer 생성 실패");
    vTaskDelay(2000);
    return;
  }
  ESP_LOGI("MAIN", "Ring Buffer 초기화 완료");

  ////////////////// event group create
  sd_write_group = xEventGroupCreate();
  sd_read_group = xEventGroupCreate();
  xEventGroupClearBits(sd_write_group, POLLING_BIT);
  xEventGroupClearBits(sd_read_group, BT_BIT);
  ////////////////// task create
  xTaskCreatePinnedToCore(audio_output_task, "audio_out", 12288, NULL, 16, NULL,
                          1);
  xTaskCreatePinnedToCore(http_streaming_task, "http_receive", 12288, NULL, 16,
                          &streaming_start_handle, 1);

  xTaskCreatePinnedToCore(sd_save_task, "sd_save", 8192, NULL, 14, NULL, 0);
  xTaskCreatePinnedToCore(sd_read_task, "sd_read", 8192, NULL, 13, NULL, 1);

  xTaskCreatePinnedToCore(file_polling_task, "file_polling", 8192, NULL, 12,
                          NULL, 0);
  xTaskCreatePinnedToCore(bluetooth_task, "bluetooth", 4096, adc1_handle, 11,
                          NULL, 0);
  xTaskCreatePinnedToCore(bt_detect_task, "bt_detect", 2048, NULL, 8, NULL, 0);
}