# ESP32 Audio Streaming System

Wi-Fi를 통해 서버에서 `.raw` 오디오 파일을 스트리밍하거나, SD카드에 저장 후 재생하는 ESP32 기반 시스템.

---

## 시스템 구성

```
[PC - Node.js Server]
    ├── port 3001 : filelist.txt 제공 (폴링용)
    └── port 3000 : .raw 파일 제공 (스트리밍/다운로드용)
          │
          │ Wi-Fi (HTTP)
          ▼
[ESP32 - Client]
    ├── file_polling_task   : 1초 주기로 서버 파일명 폴링
    ├── http_streaming_task : .raw 스트리밍 → Ring Buffer
    ├── sd_save_task        : .raw SD카드 저장 (LoRa "no" 감지 시)
    ├── sd_read_task        : SD카드 재생 (BT "yes" 수신 시)
    ├── audio_output_task   : Ring Buffer → DAC 출력 (pin 25)
    ├── bluetooth_task      : BT UART 수신 + ADC 볼륨 조절
    └── bt_detect_task      : LoRa UART 수신 (사람 감지 여부)
```

---

## Server (`server.js`)

Node.js + Express 기반. `.raw` 파일을 ESP32에 제공하는 역할.

### 실행 방법

```bash
npm install express
node server.js
```

실행 시 `.raw` 파일 경로를 입력 프롬프트로 요청한다.

### 포트 구성

| 포트 | 역할 |
|------|------|
| 3001 | `filelist.txt` 제공. ESP32가 1초 주기로 폴링하여 현재 파일명 확인 |
| 3000 | `.raw` 파일 제공. ESP32가 스트리밍 또는 다운로드 요청 |

> ESP32에서는 공유기 포트포워딩을 통해 3000 → 3500, 3001 → 3501로 접근.

### 동작 흐름

1. 실행 시 `.raw` 파일 경로를 입력받아 `currentFile`에 저장
2. `uploads/filelist.txt`를 생성하고 `currentFile` 값을 기록
3. port 3001: GET `/filelist.txt` 요청에 `currentFile` 문자열 응답
4. port 3000: GET `/{filename}` 요청에 해당 파일을 `sendFile`로 응답
5. `SIGINT` / `SIGTERM` / `SIGQUIT` 수신 시 `filelist.txt` 삭제 후 종료

### 주요 변수

| 변수 | 설명 |
|------|------|
| `currentFile` | 현재 제공 중인 `.raw` 파일 경로 |
| `fileListPath` | `uploads/filelist.txt` 절대 경로 |

---

## Client (`iot.c`)

ESP-IDF 기반 ESP32 펌웨어. FreeRTOS 멀티태스크 구조로 동작.

### 설정 (`#define`)

| 항목 | 값 | 설명 |
|------|----|------|
| `BASE_URL` | `http://1002seok.iptime.org:3500` | `.raw` 파일 서버 주소 |
| `FILE_LIST_URL` | `http://1002seok.iptime.org:3501/filelist.txt` | 파일명 폴링 주소 |
| `WIFI_SSID` / `WIFI_PASS` | (설정값) | Wi-Fi 접속 정보 |
| `SAMPLE_RATE` | 44100 | DAC 샘플레이트 (Hz) |

### 핀 배치

| 기능 | 핀 |
|------|----|
| DAC 출력 | GPIO 25 |
| SPI MISO | GPIO 4 |
| SPI MOSI | GPIO 15 |
| SPI CLK | GPIO 14 |
| SPI CS | GPIO 13 |
| LoRa RXD (UART1) | GPIO 18 |
| LoRa TXD (UART1) | GPIO 19 |
| BT RXD (UART2) | GPIO 16 |
| BT TXD (UART2) | GPIO 17 |
| ADC (볼륨 조절) | ADC1 CH7 |

### 초기화 순서 (`app_main`)

```
Mutex 생성
→ SPI 초기화 + CS GPIO 설정
→ DAC 초기화 (44100Hz, APLL)
→ ADC 초기화 (ADC1 CH7)
→ LoRa UART 초기화 (UART1, 9600bps)
→ BT UART 초기화 (UART2, 9600bps)
→ NVS + Wi-Fi 연결
→ Ring Buffer 생성 (32768 bytes)
→ EventGroup 생성 (sd_write_group, sd_read_group)
→ 태스크 생성
```

### 태스크 목록

| 태스크 | 코어 | 우선순위 | 스택 | 설명 |
|--------|------|----------|------|------|
| `audio_output_task` | Core 1 | 16 | 12KB | Ring Buffer → DAC 연속 출력 |
| `http_streaming_task` | Core 1 | 16 | 12KB | HTTP `.raw` 수신 → Ring Buffer |
| `sd_save_task` | Core 0 | 14 | 8KB | HTTP → SD카드 저장 |
| `sd_read_task` | Core 1 | 13 | 8KB | SD카드 → Ring Buffer 재생 |
| `file_polling_task` | Core 0 | 12 | 8KB | 서버 filelist.txt 1초 폴링 |
| `bluetooth_task` | Core 0 | 11 | 4KB | BT UART 수신 + ADC 볼륨 |
| `bt_detect_task` | Core 0 | 8 | 2KB | LoRa UART 사람 감지 수신 |

### 동작 흐름

#### 스트리밍 재생

```
file_polling_task: 서버 filelist.txt 1초 폴링
→ 파일명 변경 감지 시 POLLING_BIT set + TaskNotify
→ http_streaming_task: ulTaskNotifyTake로 대기 중 해제
→ AUDIO_URL로 HTTP GET → http_event_handler 콜백
→ 수신 데이터: 16bit PCM → 8bit 변환 (볼륨 적용) → Ring Buffer 전송
→ audio_output_task: Ring Buffer → dac_continuous_write → GPIO 25 출력
```

#### SD카드 저장 (LoRa "no" 수신 시)

```
bt_detect_task: UART1에서 "no" 수신
→ sd_write_group의 BT_DETECT_BIT set
→ sd_save_task: POLLING_BIT & BT_DETECT_BIT 동시 대기
→ 조건 충족 시 download_to_sd_proc 실행
→ HTTP GET → SD카드 `/sdcard/broadcast_N.raw` 저장
→ bt_detect_download_done = true (중복 저장 방지)
```

#### SD카드 재생 (BT "yes" 수신 시)

```
bluetooth_task: UART2에서 "yes" 수신
→ sd_read_group의 BT_BIT set
→ sd_read_task: BT_BIT 대기 중 해제 + Mutex 취득
→ SD카드 마운트 → opendir → readdir
→ sd_read_file: fread → 16bit PCM to 8bit → Ring Buffer 전송
→ 파일 재생 완료 후 unlink → 다음 파일 처리 → unmount
→ Mutex 반환
```

#### 볼륨 조절

```
bluetooth_task: ADC1 CH7 전압 측정 (주기: 300ms)
< 0.9V  → vol_control = 0.35
1.0~1.1V → vol_control = 0.45
1.1~1.2V → vol_control = 0.60
1.2~1.3V → vol_control = 0.80
≥ 1.3V  → vol_control = 1.00
```

### 동기화 구조

| 동기화 수단 | 사용처 |
|-------------|--------|
| `sd_write_group` EventGroup | `POLLING_BIT`, `BT_DETECT_BIT` - SD 저장 조건 동기화 |
| `sd_read_group` EventGroup | `BT_BIT` - SD 재생 조건 동기화 |
| `stream_read_sync` Mutex | `http_streaming_task` ↔ `sd_read_task` 상호 배제 |
| `streaming_start_handle` TaskNotify | `file_polling_task` → `http_streaming_task` 1회 트리거 |

### 오디오 데이터 흐름

```
HTTP / SD카드 (.raw, 16bit PCM signed, 44100Hz)
→ 16bit → 8bit 변환: dac_buf[i] = ((sample * vol) + 32768) >> 8
→ Ring Buffer (32768 bytes, NOSPLIT)
→ audio_output_task: dac_continuous_write (DMA)
→ GPIO 25 (DAC CH0) 아날로그 출력
```

### SD카드 드라이버

- 인터페이스: SPI (SDSPI)
- 주파수: 5 MHz
- 마운트 포인트: `/sdcard`
- 파일 형식: `/sdcard/broadcast_N.raw` (N = 0, 1, 2, ...)
- 최대 저장 파일 수: `MAX_FILE_NUM = 5`