# ESP32 IoT 오디오 스트리밍 시스템

Node.js 서버에서 `.raw` 음원을 서빙하고, ESP32가 Wi-Fi를 통해 HTTP 스트리밍으로 수신하여 DAC로 실시간 출력하는 시스템입니다.  
블루투스 명령과 인체 감지(LoRa) 결과에 따라 SD 카드 저장 및 로컬 재생도 지원합니다.

---

## 시스템 구성

```
┌──────────────────────────────────────────┐
│             Node.js Server               │
│  ┌─────────────────┐  ┌───────────────┐  │
│  │  Port 3001      │  │  Port 3000    │  │
│  │  filelist.txt   │  │  .raw 파일    │  │
│  │  서빙           │  │  서빙         │  │
│  └────────┬────────┘  └──────┬────────┘  │
└───────────┼───────────────── ┼───────────┘
            │ HTTP polling      │ HTTP streaming
            ▼                  ▼
┌──────────────────────────────────────────┐
│                 ESP32                    │
│                                          │
│  file_polling_task  →  http_streaming    │
│                              │           │
│                         Ring Buffer      │
│                              │           │
│                       audio_output_task  │
│                              │           │
│                           DAC (Pin 25)   │
│                         44100Hz / 8bit   │
└──────────────────────────────────────────┘
```

---

## Server (`server.js`)

### 역할

사용자가 지정한 `.raw` 음원 파일을 두 개의 Express 서버로 분리하여 서빙합니다.

| 포트 | 역할 | 엔드포인트 |
|------|------|-----------|
| 3001 | 현재 파일명 공지 | `GET /filelist.txt` |
| 3000 | `.raw` 파일 다운로드 | `GET /<filename>` |

### 동작 흐름

1. 실행 시 재생할 `.raw` 파일 경로를 CLI로 입력받음
2. `filelist.txt`를 생성하고, 해당 파일명을 3001 포트로 서빙
3. 3000 포트로 `.raw` 파일 자체를 서빙
4. 프로세스 종료 시 (`SIGINT` / `SIGTERM`) `filelist.txt` 자동 삭제

### 실행 방법

```bash
node server.js
# 실행 후 .raw 파일 경로 입력 (예: uploads/broadcast.raw)
```

### 종료

`Ctrl+C` 입력 시 `filelist.txt` 삭제 후 정상 종료됩니다.

---

## Client (`iot.c`, ESP32)

### 역할

FreeRTOS 기반 멀티태스크 구조로, HTTP 스트리밍 수신 → Ring Buffer → DAC 출력 파이프라인을 구성합니다.  
LoRa 인체 감지 및 블루투스 명령에 따라 SD 카드 저장/재생 기능도 병행합니다.

### 하드웨어 구성

| 기능 | 인터페이스 | 핀 |
|------|-----------|-----|
| DAC 오디오 출력 | DAC | GPIO 25 |
| SD 카드 | SPI | MOSI:15 / MISO:4 / CLK:14 / CS:13 |
| LoRa (인체감지) | UART1 | TX:19 / RX:18 |
| 블루투스 | UART2 | TX:17 / RX:16 |
| 볼륨 조절 | ADC1 CH7 | GPIO 35 |

### 태스크 구성

| 태스크 | 코어 | 우선순위 | 역할 |
|--------|------|---------|------|
| `audio_output_task` | Core 1 | 16 | Ring Buffer → DAC 출력 |
| `http_streaming_task` | Core 1 | 16 | HTTP로 `.raw` 수신 → Ring Buffer |
| `sd_save_task` | Core 0 | 14 | 서버 음원 → SD 카드 저장 |
| `sd_read_task` | Core 1 | 13 | SD 카드 음원 → Ring Buffer |
| `file_polling_task` | Core 0 | 12 | 서버 파일명 변경 감지 (1초 주기) |
| `bluetooth_task` | Core 0 | 11 | BT 명령 수신 + ADC 볼륨 제어 |
| `bt_detect_task` | Core 0 | 8 | LoRa 인체 감지 신호 수신 |

### 동작 흐름

**스트리밍 재생**
```
file_polling_task  →  파일명 변경 감지
                  →  TaskNotify → http_streaming_task 활성화
                  →  HTTP로 .raw 수신 → 16bit PCM을 8bit 변환
                  →  Ring Buffer (32KB)
                  →  audio_output_task → DAC 출력 (44100Hz)
```

**SD 카드 저장 (인체 미감지 시)**
```
bt_detect_task  →  LoRa "no" 수신  →  BT_DETECT_BIT set
file_polling_task  →  POLLING_BIT set
sd_save_task  →  두 비트 모두 set 확인  →  HTTP로 .raw 다운로드 → SD 저장
```

**SD 카드 재생 (블루투스 "yes" 수신 시)**
```
bluetooth_task  →  "yes" 수신  →  BT_BIT set
sd_read_task  →  SD 파일 읽기  →  Ring Buffer  →  DAC 출력
```

### 동기화 구조

| 동기화 수단 | 용도 |
|------------|------|
| `EventGroup: sd_write_group` | POLLING_BIT + BT_DETECT_BIT 조합으로 저장 조건 판단 |
| `EventGroup: sd_read_group` | BT_BIT로 SD 재생 트리거 |
| `Semaphore: stream_read_sync` | HTTP 스트리밍과 SD 재생의 상호 배제 |
| `TaskNotify` | 파일 변경 감지 시 스트리밍 태스크 즉시 활성화 |

### PCM 변환

서버에서 수신한 16bit Signed PCM을 DAC 출력을 위해 8bit Unsigned로 변환합니다.

```c
int16_t adjust = (int16_t)(pcm_in[i] * vol_control);
dac_buffer[i] = (adjust + 32768) >> 8;
// -32768~32767 → 0~255
```

볼륨은 ADC 입력 전압에 따라 0.35 ~ 1.0 범위에서 5단계로 실시간 조절됩니다.

---

## 개발 환경

| 항목 | 내용 |
|------|------|
| MCU | ESP32 (Dual Core, 240MHz) |
| RTOS | FreeRTOS (ESP-IDF) |
| 서버 | Node.js + Express |
| 오디오 포맷 | 16bit Signed PCM, 44100Hz, Mono |
| 네트워크 | Wi-Fi → HTTP (iptime DDNS + 포트포워딩) |
