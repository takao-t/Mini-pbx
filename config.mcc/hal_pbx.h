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

// 400HzのNCO値
#define TONE_400HZ 0x1a
// 800HzのNCO値
#define TONE_800Hz 0x34
// CID送出関連(FSKモデム)
#define FSK_MARK  0x55  // 1300Hz用 NCOインクリメント値
#define FSK_SPACE 0x8a  // 2100Hz用 NCOインクリメント値

// コード定義
#define DLE 0x10
#define SOH 0x01
#define STX 0x02
#define ETX 0x03

// ==========================================================
// Flash (NVM) 保存領域の設定
// ==========================================================
// プログラムの最後尾付近から2ページ(64ワード)分をデータ領域として確保
// 将来別のPICに変更した場合でも、PROGMEM_SIZE をもとに自動で最後尾に配置
// このHALでは最大2回線まで対応
#ifndef HAL_STORAGE_SIZE
#define HAL_STORAGE_SIZE (PROGMEM_PAGE_SIZE * 2)
#endif
#ifndef HAL_STORAGE_START_ADDR
#define HAL_STORAGE_START_ADDR (PROGMEM_SIZE - HAL_STORAGE_SIZE)
#endif

#define NVM_MAGIC_VAL  0x5C
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

// トーン制御
void HardwareToneControl(uint8_t line, bool enable);
void NCO1_SetIncrement(uint32_t inc_val);
// スイッチ制御
void HAL_Switch_Control(bool onoff);

// CID送出
bool Send_NTT_CallerID(uint8_t line, char *num_str);

#endif // HAL_PBX_H