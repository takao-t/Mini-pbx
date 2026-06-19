/*
PBXCore HAL
ハードウエア(PICのピン配置など)依存部分はここで定義する
MCCでピン名を設定しhal_pbx.hとhal_pbx.cで設定すること
(PICのピンも直接指定せずMCCの"名前"で指定する)
*/

#ifndef HAL_PBX_H
#define HAL_PBX_H

#include <stdint.h>
#include <stdbool.h>

// トーンの種類定義
typedef enum {
    TONE_OFF,      // TG1=H, TG2=H (トーン停止)
    TONE_DIAL,     // TG1=L, TG2=L (400Hz連続)
    TONE_RINGBACK, // TG1=H, TG2=L (400Hz変調断続)
    TONE_BUSY      // TG1=L, TG2=H (400Hz断続)
} ToneType;

// このハードウェア構成での最大回線数
#define TOTAL_MAX_LINES 2

// フック状態の定義
#define HOOK_ON_HOOK  false // L
#define HOOK_OFF_HOOK true  // H

// スイッチボードへの送信用ソフトウェアUART
#define SW_BAUD_RATE 9600
#define BIT_DELAY_US (1000000 / SW_BAUD_RATE)

// 初期化関数
void HAL_PBX_Init(void);

// 回線制御API
void HAL_SetTone(uint8_t line, ToneType tone);
void HAL_SetRing(uint8_t line, bool enable);
bool HAL_GetHook(uint8_t line);
uint8_t HAL_GetMaxLines(void);

// スイッチボード制御API (Bit-banging)
void HAL_SoftwareUART_WriteByte(uint8_t data);

#endif // HAL_PBX_H