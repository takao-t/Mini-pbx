// 2ラインPIC直結用HAL
// SLICユニットを使用せず2回線専用の簡易PBX向け(16F18326)
// main.cは変更不要。このHALのみでハードウェアの差異は吸収
// HAL用にタイマ(1mS割り込み有:TMR4)が必要
//
// 各ピンは以下の命名基準
//   Lx_TONE : TONE出力ピン。内部でNCO(400Hz)からの出力をルーティングのこと
//   Lx_RM   : SLICのRM(RINGM ODE)
//   Lx_FR   : SLICのF/R
//   SW_CTRL : L1とL2の音声を繋ぐスイッチ出力。接続する/しないだけなので単純なoutput
//     SLICのHOOK(SHK)はCLCに接続しデバウンスする
//     HALのプログラムはCLC出力を読み取るだけ
//        L1_SHK -> CLC3 -> CLC1 -> CLC1_OutputStatus()
//        L2_SHK -> CLC4 -> CLC2 -> CLC2_OutputStatus()

#include "hal_pbx.h"
#include "mcc_generated_files/system/pins.h"
#include "mcc_generated_files/system/system.h"
#include "mcc_generated_files/timer/tmr4.h"
#include "mcc_generated_files/timer/tmr4_deprecated.h"


// 各回線のハードウェア状態を保持する構造体
typedef struct {
    ToneType current_tone;
    bool is_ringing;
    uint16_t cadence_timer;
    uint8_t ring_toggle_counter;
    bool fr_state;
} LineHardwareState;

static LineHardwareState line_hw[TOTAL_MAX_LINES];

// RBT音生成時に16Hz変調するためのカウンタ
static uint8_t modulation_cycle = 62;

// ---------------------------------------------------------
// ハードウェア直接制御関数群
// ---------------------------------------------------------

// 400HzトーンのON/OFF制御 (NCO出力をTRISAでオン/オフ)
static void HardwareToneControl(uint8_t line, bool enable) {
    if (line == 0) {
        if (enable) L1_TONE_SetDigitalOutput();
        else        L1_TONE_SetDigitalInput();
    } else {
        if (enable) L2_TONE_SetDigitalOutput();
        else        L2_TONE_SetDigitalInput();
    }
}

// ベル鳴動時のF/Rピン(極性反転)制御
static void HardwareRingFRControl(uint8_t line, bool state) {
    if (line == 0) {
        if (state) L1_FR_SetHigh(); else L1_FR_SetLow();
    } else {
        if (state) L2_FR_SetHigh(); else L2_FR_SetLow();
    }
}

// ---------------------------------------------------------
// バックグラウンド処理 (1ms Tick)
// ---------------------------------------------------------

// TMR4などの1msタイマー割り込みからコールバックされる関数
// 1ms Tickで動作し回数をカウントすることでケイデンス,変調を行う
void HAL_CadenceTick_1ms(void) {
    for (uint8_t i = 0; i < TOTAL_MAX_LINES; i++) {
        
        // --- トーンのケイデンス制御 ---
        if (line_hw[i].current_tone != TONE_OFF) {
            line_hw[i].cadence_timer++;
            switch (line_hw[i].current_tone) {
                case TONE_DIAL:     
                    // ダイヤルトーン(連続音)
                    HardwareToneControl(i, true);
                    break;
                    
                case TONE_RINGBACK: 
                    // リングバック音 1秒 ON / 2秒 OFF
                    if (line_hw[i].cadence_timer < 1000){
                        // リングバック音の変調処理
                        if(modulation_cycle > 31){ // 16Hz変調するため1mS×31サイクルでON/OFFさせる
                            HardwareToneControl(i, true);
                        }
                        else{
                            HardwareToneControl(i, false);
                        }
                        modulation_cycle--;
                        if(modulation_cycle == 0) modulation_cycle = 62; 
                    }
                    else if (line_hw[i].cadence_timer < 3000) HardwareToneControl(i, false);
                    else line_hw[i].cadence_timer = 0;
                    break;
                    
                case TONE_BUSY:     
                    // ビジー音 0.5秒 ON / 0.5秒 OFF (話中音)
                    if (line_hw[i].cadence_timer < 500)       HardwareToneControl(i, true);
                    else if (line_hw[i].cadence_timer < 1000) HardwareToneControl(i, false);
                    else line_hw[i].cadence_timer = 0;
                    break;
                    
                default: break;
            }
        }

        // 電話機のベル鳴動処理
        // ベル鳴動のケイデンス(1s鳴動/2s休止)と20Hz生成
        if (line_hw[i].is_ringing) {
            line_hw[i].cadence_timer++;
            
            // 最初の1秒間は20HzでF/Rをトグルして鳴動させる
            if (line_hw[i].cadence_timer < 1000) {
                line_hw[i].ring_toggle_counter++;
                // 25msごとに極性を反転 (50ms周期 = 20Hz)
                if (line_hw[i].ring_toggle_counter >= 25) {
                    line_hw[i].ring_toggle_counter = 0;
                    line_hw[i].fr_state = !line_hw[i].fr_state;
                    HardwareRingFRControl(i, line_hw[i].fr_state);
                }
            } 
            // 次の2秒間は休止
            else if (line_hw[i].cadence_timer < 3000) {
                HardwareRingFRControl(i, false);
            } 
            // 3秒経過でサイクルリセット
            else {
                line_hw[i].cadence_timer = 0;
            }
        }
    }
}

// ---------------------------------------------------------
// main から使用するHALのAPI
// ---------------------------------------------------------

void HAL_PBX_Init(void) {
    for (uint8_t i = 0; i < TOTAL_MAX_LINES; i++) {
        line_hw[i].current_tone = TONE_OFF;
        line_hw[i].is_ringing = false;
        line_hw[i].cadence_timer = 0;
        line_hw[i].ring_toggle_counter = 0;
        line_hw[i].fr_state = false;
        
        HardwareToneControl(i, false);
        HardwareRingFRControl(i, false);
    }
    
    // SLICのRMをOFFにする
    L1_RM_SetLow();
    L2_RM_SetLow();

    // 2回線専用なのでスイッチ制御用ピンは固定Low
    SW_CTRL_SetLow();

    // TMR4の割り込みハンドラ(HALのtick用)を登録
    TMR4_OverflowCallbackRegister(HAL_CadenceTick_1ms); 
}

void HAL_SetTone(uint8_t line, ToneType tone) {
    if (line >= TOTAL_MAX_LINES) return;

    if (line_hw[line].current_tone != tone) {
        line_hw[line].current_tone = tone;
        line_hw[line].cadence_timer = 0; // ケイデンスを最初から開始
        
        if (tone == TONE_OFF) {
            HardwareToneControl(line, false);
        }
    }
}

void HAL_SetRing(uint8_t line, bool enable) {
    if (line >= TOTAL_MAX_LINES) return;

    if (line_hw[line].is_ringing != enable) {
        line_hw[line].is_ringing = enable;
        line_hw[line].cadence_timer = 0;
        line_hw[line].ring_toggle_counter = 0;
        line_hw[line].fr_state = false;
        
        if (enable) {
            // 鳴動開始時にRMをHighに
            if (line == 0) L1_RM_SetHigh(); else L2_RM_SetHigh();
        } else {
            // 鳴動停止時にRMをLowに戻し、F/Rも安全のためLow固定
            if (line == 0) L1_RM_SetLow(); else L2_RM_SetLow();
            HardwareRingFRControl(line, false);
        }
    }
}

// フック状態の取得（CLCのデバウンス出力をそのまま読み取る）
bool HAL_GetHook(uint8_t line) {
    switch (line) {
        case 0: return CLC1_OutputStatusGet();
        case 1: return CLC2_OutputStatusGet();
        default: return false; 
    }
}

//　他のアーキテクチャではスイッチボードにシリアル接続だが2回線専用では単にポートのON/OFF
// main.cからの互換性維持のため"SoftwareUART_WriteByte"という関数名のままにする
void HAL_SoftwareUART_WriteByte(uint8_t data) {
    // 互換性維持のためCが来たら接続Rが来たら開放
    if(data == 'C') SW_CTRL_SetHigh();
    else if(data == 'R') SW_CTRL_SetLow();
}

uint8_t HAL_GetMaxLines(void) {
    return TOTAL_MAX_LINES;
}