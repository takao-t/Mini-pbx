#include "mcc_generated_files/system/pins.h"
#include "mcc_generated_files/system/system.h"
#include "hal_pbx.h"
#include <stdio.h>
#include <string.h>

// =========================================================
// CID送出関連処理
// 2回線 MINI PBX専用
// 注意: NCOはトーン生成とFSKモデムで兼用している
//       NCOは1つを2回線で使いまわしを行うので処理注意
// SLICユニットとは異なるMini-PBX専用なので、line(0 or 1)を引数
// =========================================================


// CIDに使える最大バッファ長
#define MAX_CID_BUFSIZE 16

// 1200bpsの1ビットあたりの時間(要調整)
// 1200bpsだと833uSだが実際には他の処理に引きずられるのでこれより小さい値になる
#define BIT_DELAY_V23_US 804

// 進行中のCRC値を保持するグローバル変数 
static uint16_t running_crc = 0;

// ==ハードウェア制御層==
void NCO1_SetIncrement(uint32_t inc_val) {
    NCO1INCL = (uint8_t)(inc_val & 0xFF);         
    NCO1INCH = (uint8_t)((inc_val >> 8) & 0xFF);  
    NCO1INCU = (uint8_t)((inc_val >> 16) & 0x0F); 
}

static void FSK_SendBit(bool is_mark) {
    if (is_mark) {
        NCO1_SetIncrement(FSK_MARK);
    } else {
        NCO1_SetIncrement(FSK_SPACE);
    }
    __delay_us(BIT_DELAY_V23_US);
}

void FSK_SendMarkPreamble(uint8_t duration_ms) {
    NCO1_SetIncrement(FSK_MARK);
    for(uint8_t i = 0; i < duration_ms; i++) {
        __delay_ms(1);
    }
}

// ==データリンク層 (オンザフライ処理)==
// 7bitデータを偶数パリティ付き8bitに変換
uint8_t add_even_parity_fast(uint8_t data) {
    uint8_t temp = data & 0x7F; 
    uint8_t p = temp;
    p ^= p >> 4;
    p ^= p >> 2;
    p ^= p >> 1;
    p &= 1; 
    return (p << 7) | temp; 
}

// 8ビットのビット順を反転する関数
uint8_t ReverseBits(uint8_t data) {
    data = ((data & 0xAA) >> 1) | ((data & 0x55) << 1);
    data = ((data & 0xCC) >> 2) | ((data & 0x33) << 2);
    data = ((data & 0xF0) >> 4) | ((data & 0x0F) << 4);
    return data;
}

// 1バイト送出関数
void FSK_SendByte(uint8_t data, bool is_lsb_first) {
    FSK_SendBit(false); // Start bit

    if (is_lsb_first) {
        for (uint8_t i = 0; i < 8; i++) {
            FSK_SendBit((data >> i) & 0x01);
        }
    } else {
        for (int8_t i = 7; i >= 0; i--) { // MSBファースト
            FSK_SendBit((data >> i) & 0x01);
        }
    }

    FSK_SendBit(true); // Stop bit
}

// 1バイト分のCRCを更新する関数
static void UpdateRunningCRC(uint8_t data) {
    uint8_t rev_data = ReverseBits(data);
    running_crc ^= ((uint16_t)rev_data << 8);
    for (uint8_t j = 0; j < 8; j++) {
        if (running_crc & 0x8000) {
            running_crc = (running_crc << 1) ^ 0x1021;
        } else {
            running_crc <<= 1;
        }
    }
}

// [A] 単純送信 (パリティ付加のみ、CRC計算対象外)
static void SendRawByte(uint8_t val) {
    FSK_SendByte(add_even_parity_fast(val), true);
}

// [B] 制御コード送信 (パリティ付加 ＋ CRC更新、スタッフィングなし)
static void SendControlCodeWithCRC(uint8_t val) {
    uint8_t parity_val = add_even_parity_fast(val);
    UpdateRunningCRC(parity_val);
    FSK_SendByte(parity_val, true);
}

// [C] データ送信 (パリティ付加 ＋ CRC更新 ＋ DLEスタッフィングあり)
static void SendDataWithStuffingAndCRC(uint8_t val) {
    uint8_t parity_val = add_even_parity_fast(val);
    
    // 1バイト目を処理して送信
    UpdateRunningCRC(parity_val);
    FSK_SendByte(parity_val, true);
    
    // 送信データがDLE(0x10)だった場合はもう一度同じバイトを処理して送信
    if (val == DLE) { 
        UpdateRunningCRC(parity_val);
        FSK_SendByte(parity_val, true);
    }
}

// ==========================================
// ナンバーディスプレイ ペイロード一括送出関数
// ==========================================
// 最小構成のデータ（発信電話番号のみ）をオンザフライで生成・送出
// 引数:number 番号の文字列または空文字列
//      空文字列の場合には番号通知不可(サービス提供不可)として情報送出
void SendMinimalCallerID_OnTheFly(const char* number) {
    // 1. プリアンブル (Mark 80ms)
    FSK_SendMarkPreamble(80);
    
    // CRC演算器をリセット
    running_crc = 0; 

    // --- [1] 先頭コード (CRC計算対象外) ---
    SendRawByte(DLE);
    SendRawByte(SOH);
    
    // --- [2] 制御用コード (CRC対象、スタッフィングなし) ---
    SendControlCodeWithCRC(0x07); // ヘッダ
    SendControlCodeWithCRC(DLE);  // DLE
    SendControlCodeWithCRC(STX);  // STX
    
    // --- [3] サービス種別 ---
    SendDataWithStuffingAndCRC(0x40); // 通信前情報通知

    // --- [4] & [5] メッセージ内容長とパラメータブロックの構築 ---
    // 引数がNULLまたは空文字列かを判定
    bool has_number = (number != NULL && number[0] != '\0');
    
    if (has_number) {
        // --- 電話番号を通知する場合 ---
        uint8_t num_len = (uint8_t)strlen(number);
        uint8_t msg_len = 2 + num_len; // 種別(1) + 長さ(1) + 桁数
        
        SendDataWithStuffingAndCRC(msg_len); 
        
        SendDataWithStuffingAndCRC(0x02); // パラメータ種別: 発信電話番号
        SendDataWithStuffingAndCRC(num_len); // 情報内容長
        for(uint8_t i = 0; i < num_len; i++) {
            SendDataWithStuffingAndCRC(number[i]);
        }
    } else {
        // --- 電話番号を通知できない場合 (非通知理由を送出) ---
        uint8_t msg_len = 3; // 種別(1) + 長さ(1) + データ(1)
        
        SendDataWithStuffingAndCRC(msg_len); 
        
        SendDataWithStuffingAndCRC(0x04); // パラメータ種別: 発信電話番号非通知理由
        SendDataWithStuffingAndCRC(1);    // 情報内容長
        SendDataWithStuffingAndCRC('O');  // サービス提供不可のため通知不可
    }

    // --- [6] 終端制御コード (CRC対象、スタッフィングなし) ---
    SendControlCodeWithCRC(DLE);
    SendControlCodeWithCRC(ETX);

    // --- [7] CRC送出 (上位->下位、パリティなし、MSBファースト) ---
    FSK_SendByte((running_crc >> 8) & 0xFF, false);
    FSK_SendByte(running_crc & 0xFF, false);
    
    // --- [8] ポストアンブル (Mark 20ms) ---
    FSK_SendMarkPreamble(20);
    // NCOをトーン生成用に戻す
    NCO1_SetIncrement(TONE_400HZ); 
}

// CAR信号(情報端末起動信号)送出
// 注: SLIC制御用の各ピンはMCCで名前設定のこと
bool Send_NTT_CAR(uint8_t line){
    // 極性反転
    if(line == 0){
        L1_FR_Toggle();
    } else {
        L2_FR_Toggle();
    }
    __delay_ms(120); //極性反転から送出まで0.1秒以上

    // CAR信号送出:Ring 0.5s,Wait 0.5s. 1sec/frame.
    // 待ちタイミングは6秒なので6回まで送る
    for(uint8_t j=0; j<6; j++){
        CLRWDT(); //この処理で6秒ブロックする可能性があるのでWDTクリア
        if(line == 0){ //RING用電圧生成開始
            L1_RM_SetHigh();
        } else {
            L2_RM_SetHigh();
        }

        for(uint8_t i=0; i<20; i++){ //鳴動サイクル:FRピンをトグルすることで20Hz駆動
            uint8_t shk_value;
            if(line == 0){ // HOOK状態監視
                shk_value = L1_SHK_GetValue();
            } else {
                shk_value = L2_SHK_GetValue();
            }
            if(shk_value == HIGH){  //端末が起動するとOffHook状態になる(1次応答) CLCを止めているので入力から
                // 端末が起動したので送出タイミング(0.1～3sec)を待ってtrueリターン
                if(line == 0){
                    L1_RM_SetLow();
                } else {
                    L2_RM_SetLow();
                }
                __delay_ms(200);
                return(true);
            } 
            if(line == 0){ // RING信号生成
                L1_FR_Toggle();
            } else {
                L2_FR_Toggle();
            }
            __delay_ms(25);
        }
        if(line == 0){ // RING電圧停止
            L1_RM_SetLow();
        } else {
            L2_RM_SetLow();
        }
        __delay_ms(500); // 休止 0.5Sec
    }
    //端末が起動していないのでfalseで戻る
    if(line == 0){
        L1_RM_SetLow();
    } else {
        L2_RM_SetLow();
    }
    return(false);
}

// NTT形式でのCallerID送出
// 注意:モデムのタイミング維持のためこの関数を呼び出す前に割り込みを禁止し
// 　　 リターンした後に割り込みを再許可すること
bool Send_NTT_CallerID(uint8_t line, char *num_str){ 
    
    // 注: CAR信号送出からCID送信までの間のフック状態をPBXコアに伝えないために
    //     CLCを停止(=Lowなのでオンフック状態)
    //     フック状態はCLCの入力(Lx_SHK = SLICの出力)から直接読む
    //     CID送出先(呼び出し先)のFSKモデムとしてNCOを使用する
    ///    その間は発信側は無音とするためトーン出力停止(RBT前のタイミング)
    // この関数からの復帰時にCLCをEnableに戻す
    if(line == 0){
        CLC1_Disable();
        HardwareToneControl(0, true);
        HardwareToneControl(1, false);
    } else {
        CLC2_Disable();
        HardwareToneControl(0, false);
        HardwareToneControl(1, true);
    }

    //CAR信号の送出がOKならCID送出
    if(Send_NTT_CAR(line) == true){

        // モデムタイミング維持のためモデム送出中は割り込み禁止
        INTERRUPT_GlobalInterruptDisable();
        SendMinimalCallerID_OnTheFly(num_str);
        INTERRUPT_GlobalInterruptEnable();

        // 送出完了待ち(max 7秒)
        // 端末で受信が完了するとオンフックするのでそれを待つ
        for(uint8_t i=0; i<70; i++){
            CLRWDT(); //この処理で7秒ブロックする可能性があるのでWDTクリア
            uint8_t shk_value;
            if(line == 0){
                shk_value = L1_SHK_GetValue();
            } else {
                shk_value = L2_SHK_GetValue();
            }
            if(shk_value == LOW){
                //オンフックしたら送出完了
                // 待ち(300mS以上)
                __delay_ms(350);
                if(line == 0){ // CLC出力復旧
                    CLC1_Enable();
                } else {
                    CLC2_Enable();
                }
                //対象ラインのトーン出力を停止
                HardwareToneControl(line, false);
                return(true);
            }
            __delay_ms(100);
        }

        //forループを最後まで回り切ったら送出失敗
    }

    // CAR信号送出失敗 or 端末応答タイムアウト
    if(line == 0){ // 失敗した場合でもCLCを復旧
        CLC1_Enable();
    } else {
        CLC2_Enable();
    }
    //対象ラインのトーン出力を停止
    HardwareToneControl(line, false);
    return(false);
}