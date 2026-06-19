 #include "mcc_generated_files/nvm/nvm.h"
#include "mcc_generated_files/system/pins.h"
#include "mcc_generated_files/system/system.h"
#include "hal_pbx.h"
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/*
    PBXメイン(core)プログラム
     ハードウェア依存部分はhal_pbx.hとhal_pbx.cで定義する
     SLICユニット、I2C拡張ユニット等の使用はHAL側で行うこと

    このプログラムに依存する部分としてMCCで以下を設定すること
     EUSART : 9600bps割り込み有、printfリダイレクト有
     TMR2 : 1ms tickを生成し割り込み有
     NVR : EEPROM APIあり
     WWDT : HFINTOSCで1:522499
*/

// 全般的な注意：
//   回線番号は内部配列では[0]開始なので0からだが表示上では1からなので
//   printf等で-1,+1している個所があるため読み違えないように
//   HALでは回線番号はL1_のように1からを使用すること
//   スイッチボードでは1,2を使うので01 02のように指定する

// 接続されるSLICユニットの最大数はHAL側で設定する
// 注意:回線数を増やす場合にはHALに追加/修正すること
// I/Oエクステンダ(I2C)使用の可否もHALで吸収している
// このmain.c自体はHALで定義した回線数に対応可能になっている
// このコード自体は回線数に依存しない(はず)


// 内線番号の最大桁数
#define MAX_DIGITS 2

// 接続されている回線数
uint8_t current_max_lines = 8;

// 各ポート毎の設定保持構造体
// 内線番号と物理ラインの紐付けもここで行う
typedef struct {
    uint8_t extension;     // 内線番号 (例: 11～)
    uint8_t initial_state; // 初期ステート (STATE_AUTOANSなど)
    uint8_t hotline_ext;   // ホットラインの発信先内線番号 (0xFFの場合は無効)
    uint8_t reserved;      // 予約(将来の拡張用・4バイト境界合わせ)
} PortConfig;

// 設定データの実体
PortConfig port_configs[TOTAL_MAX_LINES];

// ステート定義
typedef enum {
    STATE_IDLE = 0,     // オンフック（待機中）
    STATE_DIALTONE,     // オフフック直後、ダイヤルトーン送出中
    STATE_DIALING,      // ダイヤル受信中
    STATE_ROUTING,      // 番号確定、接続先判定中
    STATE_CALLING,      // 相手を呼び出し中（リングバックトーン送出）
    STATE_RINGING,      // 着信中（自回線のベル鳴動中）
    STATE_TALKING,      // 通話中
    STATE_BUSY,         // 話中（ビジートーン送出中）
    STATE_UNAVAIL,      // 回線使用不可(SLIC未接続など)
    STATE_AUTOANS       // オートアンサ設定
} PBX_State;

typedef struct {
    PBX_State state;        // 現在のステート
    bool current_hook;      // 現在のフック状態
    bool last_hook;         // 前回のフック状態
    
    // ダイヤル処理用
    uint8_t dp_count;       // 現在カウント中のパルス数
    uint8_t dialed_digits;  // 受信完了した桁数
    uint8_t dialed_number[MAX_DIGITS]; // 受信した番号を格納
    
    // タイマー（1msTickで減算）
    volatile uint16_t dp_timer;    
    volatile uint16_t state_timer; 
    volatile uint16_t hangup_timer;

    // 通話相手の物理ポート番号 (0-3)、未接続時は 0xFF などを入れる
    uint8_t target_port;    
} LineContext;

LineContext lines[TOTAL_MAX_LINES];

// EEPROM保存(内線番号・初期ステート)設定
// 注意: EEPROMの内容
//  0x01 マジックナンバー
//  0x02 内線番号1
//  0x03 初期ステート1
//  以降、偶数アドレスに内線番号、奇数アドレスにステート
#define EEPROM_MAGIC_ADDR 0x01 // マジックナンバーの保存先
#define EEPROM_MAGIC_VAL  0x5A // 適当な固定値（0xFFや0x00以外）
#define EEPROM_CFG_START  0x02 // 内線番号データの開始アドレス

// EEPROMへの保存 (SAVE_TO_EEPROMコマンド等から呼ぶ)
void SaveSettings(void) {
    uint8_t *ptr = (uint8_t *)port_configs;
    for (uint16_t i = 0; i < sizeof(port_configs); i++) {
        EEPROM_WRITE(EEPROM_CFG_START + i, ptr[i]);
    }
    EEPROM_WRITE(EEPROM_MAGIC_ADDR, EEPROM_MAGIC_VAL);
}

// EEPROMからの設定取得
void LoadSettings(void) {
    if (EEPROM_READ(EEPROM_MAGIC_ADDR) == EEPROM_MAGIC_VAL) {
        // 構造体サイズ分、バイト単位で一括読み込み
        uint8_t *ptr = (uint8_t *)port_configs;
        for (uint16_t i = 0; i < sizeof(port_configs); i++) {
            ptr[i] = EEPROM_READ(EEPROM_CFG_START + i);
        }
        
        // 読み込んだ設定をステートに反映
        for(uint8_t i = 0; i < current_max_lines; i++){
            if(port_configs[i].initial_state == STATE_AUTOANS){
                lines[i].state = STATE_AUTOANS;
            }
        }
    } else {
        // 初回起動時：デフォルト値を設定してEEPROMに保存
        for(uint8_t i = 0; i < TOTAL_MAX_LINES; i++){
            port_configs[i].extension = 11 + i;
            port_configs[i].initial_state = 0;
            port_configs[i].hotline_ext = 0xFF; // 0xFFで無効化
            port_configs[i].reserved = 0;
        }
        SaveSettings(); // 下記の保存関数を呼ぶ
    }
}

// ステートを文字列に変換するヘルパー関数
const char* GetStateString(PBX_State state) {
    switch(state) {
        case STATE_IDLE:     return "IDLE";
        case STATE_DIALTONE: return "DIALTONE";
        case STATE_DIALING:  return "DIALING";
        case STATE_ROUTING:  return "ROUTING";
        case STATE_CALLING:  return "CALLING";
        case STATE_RINGING:  return "RINGING";
        case STATE_TALKING:  return "TALKING";
        case STATE_BUSY:     return "BUSY";
        case STATE_UNAVAIL:  return "UNAVAIL";
        case STATE_AUTOANS:  return "AUTOANS";
        default:             return "UNKNOWN";
    }
}

//ダイヤルパルスカウント用Tick処理
//TMR2で1ms毎に実行
void PBX_SystemTick_1ms(void)
{
    // 全回線のタイマーを1msずつ減算する
    for (uint8_t i = 0; i < current_max_lines; i++) {
        if (lines[i].dp_timer > 0) {
            lines[i].dp_timer--;
        }
        if (lines[i].state_timer > 0) {
            lines[i].state_timer--;
        }
        if (lines[i].hangup_timer > 0) {
            lines[i].hangup_timer--;
        }

    }
}

// コマンドライン処理用バッファとインデックス
#define CMDBUFSIZE 64
char rxBuffer[CMDBUFSIZE];
uint8_t rxIndex = 0;

// プロトタイプ宣言
void SoftwareUART_WriteString(const char* str);
void SwitchControl(bool control, uint8_t line1, uint8_t line2);
void SwitchControl_Single(bool control, uint8_t switch1, uint8_t switch2);
void ProcessStateMachine(uint8_t ch);
void ProcessCommandLine(const char *str);

/*
    メイン
*/


int main(void)
{
    __delay_ms(500);

    // MCCでの設定類初期化
    SYSTEM_Initialize();

    // Timer2の割り込み発生時に呼ばれる関数を登録
    TMR2_PeriodMatchCallbackRegister(PBX_SystemTick_1ms);

    // グローバル割り込みを許可
    INTERRUPT_GlobalInterruptEnable(); 

    // 周辺機器割り込みを許可 
    INTERRUPT_PeripheralInterruptEnable(); 

    // 起動・初期化ステージ
    printf("\r\n\r\n");
    printf("PBXCore: Starting\r\n");

     // HALの初期化
    HAL_PBX_Init();

    //現在の最大回線数を取得
    current_max_lines = HAL_GetMaxLines();
    printf(" Active LINES: %d\r\n", current_max_lines);

    // スイッチボード初期化処理
    // 電源投入時の信号乱れがあるので全スイッチを初期化
    // 複数回送信することで各UARTを完全に正常化させる
    printf("PBXCore: Initializing Switchboard...");
    printf("1.");
    SoftwareUART_WriteString("RFFFF\r");
    __delay_ms(1000);
    printf("2.");
    SoftwareUART_WriteString("RFFFF\r");
    __delay_ms(1000);
    printf("3.");
    SoftwareUART_WriteString("RFFFF\r");
    __delay_ms(1000);
    printf("done.\r\n");
    

    // スイッチボードテスト
    // 全スイッチを順番にONしてからOFFする
    // 異常がある場合にはスイッチボードのLEDを確認する
    printf("PBXCore: Testing Switchs.\r\n");

    
    uint8_t i,j;
    for(i = 1; i <= current_max_lines; i++){
        for(j = 1; j <= current_max_lines; j++){
            CLRWDT();
            SwitchControl(true, i, j);
            printf(" ON:%d-%d\r\n",i,j);
            __delay_ms(50);
        }

    }
    for(i = 1; i <= current_max_lines; i++){
        for(j = 1; j <= current_max_lines; j++){
            CLRWDT();
            SwitchControl(false, i, j);
            printf(" OFF:%d-%d\r\n",i,j);
            __delay_ms(50);
        }

    }
    printf("PBXCore: Switch test done.\r\n");
    

    // 全ステートの初期化処理
    printf("PBXCore: Reseting All States");
    for (uint8_t i = 0; i < current_max_lines; i++) {
        lines[i].state = STATE_IDLE;
        lines[i].current_hook = HAL_GetHook(i);
        lines[i].last_hook = lines[i].current_hook;
        lines[i].dp_count = 0;
        lines[i].dialed_digits = 0;
        lines[i].dp_timer = 0;
        lines[i].state_timer = 0;
        lines[i].target_port = 0xFF; // 未接続
    }
    __delay_ms(100);
    printf(" - done.\r\n");

    // この時点でオフフック(H)になっているポートはSLICユニットが
    // 接続されていないので使用不可に設定する
    // 注: PBX起動時に「受話器外し」していると使用不可になるが受話器
    //     上げ下げでIDLEに戻せる。モジュールが繋がっていないポート
    //     は上げ下げできないのでUNAVAILのままになる
    printf("PBXCore: Checking LINE modules\r\n");
    for(uint8_t i = 0; i < current_max_lines; i++){
        printf(" Port %2d", i + 1);
        if(lines[i].current_hook == 0){
            printf("- OK\r\n");
        }
        else{
            lines[i].state = STATE_UNAVAIL;
            printf("- NG\r\n");
        }
    }

    // EEPROMからの内線設定読み出し
    printf("PBXCore: Loading settings from EEPROM.\r\n");
    LoadSettings();
    for(uint8_t i = 0; i < current_max_lines; i++){
        printf(" Port %2d : Ext %d\r\n", i + 1, port_configs[i].extension);
    }


    // 初期化完了PBX処理開始
    printf("\r\n");
    printf("PBXCore: === PBX Ready ===\r\n");

    while(1)
    {

        //ウオッチドッグクリア
        CLRWDT();

        // 各回線のステートマシンを実行
        for(uint8_t i = 0; i < current_max_lines; i++){
            ProcessStateMachine(i);
        }

        // シリアルコンソール
        if(EUSART_IsRxReady()){
            uint8_t rxData = EUSART_Read();
            
            // 改行コードでコマンド処理へ
            if(rxData == '\r' || rxData == '\n'){
                EUSART_Write(rxData); // 改行のエコーバック
                rxBuffer[rxIndex] = '\0';
                ProcessCommandLine(rxBuffer);
                printf("\r\n");
                printf("PBX> ");
                rxIndex = 0;
            }
            else { // 通常文字または制御文字
                if(rxData == '\b' || rxData == 0x7f){
                    if(rxIndex > 0){
                        rxIndex--;
                        printf("\b \b"); 
                    }
                }
                // バックスペースの時は以下を実行させない
                else if(rxIndex < CMDBUFSIZE -1 ){ 
                    EUSART_Write(rxData); // 通常文字のエコーバック
                    
                    if(rxData >= 'a' && rxData <='z'){
                        rxData -= 0x20;
                    }
                    rxBuffer[rxIndex++] = (char)rxData;
                }
            }
        }
    }
}

// ソフトウェアシリアルでの文字列送信
// "底"はHALのWriteByte
void SoftwareUART_WriteString(const char* str)
{
    while (*str) {
        HAL_SoftwareUART_WriteByte(*str);
        str++;
    }
}

// スイッチボード制御処理
// VinとVoutを相互に繋ぐので2か所オン/オフが必要
// control:true=on(connect), false=off(release)
void SwitchControl(bool control, uint8_t line1, uint8_t line2)
{
    char buf[8];

    if(control == true) buf[0] = 'C';
    else buf[0] = 'R';

    buf[5] = 0x0d;
    buf[6] = 0x00;

    // 片側の接続・切断データ
    buf[1] = (line1 / 10) + 0x30;
    buf[2] = (line1 % 10) + 0x30;
    buf[3] = (line2 / 10) + 0x30;
    buf[4] = (line2 % 10) + 0x30;

    SoftwareUART_WriteString(buf);
    // スイッチボード後段転送への安全のため待機
    __delay_ms(1);

    //　もう片側の接続・切断データ
    buf[1] = (line2 / 10) + 0x30;
    buf[2] = (line2 % 10) + 0x30;
    buf[3] = (line1 / 10) + 0x30;
    buf[4] = (line1 % 10) + 0x30;

    SoftwareUART_WriteString(buf);
    // スイッチボード後段転送への安全のため待機
    __delay_ms(1);
}
// シングルスイッチ制御処理
// ミュート制御用などスイッチボードの機能を単一スイッチで行う場合の処理
void SwitchControl_Single(bool control, uint8_t switch1, uint8_t switch2)
{
    char buf[8];

    if(control == true) buf[0] = 'C';
    else buf[0] = 'R';

        // 片側の接続・切断データ
    buf[1] = (switch1 / 10) + 0x30;
    buf[2] = (switch1 % 10) + 0x30;
    buf[3] = (switch2 / 10) + 0x30;
    buf[4] = (switch2 % 10) + 0x30;

    buf[5] = 0x0d;
    buf[6] = 0x00;

    SoftwareUART_WriteString(buf);
    // スイッチボード後段転送への安全のため待機
    __delay_ms(1);

}

// 各回線処理のステートマシン
void ProcessStateMachine(uint8_t ch) {
    LineContext *line = &lines[ch]; // ポインタでアクセスしやすくする
    
    // 物理ピンから現在のフック状態を取得
    line->current_hook = HAL_GetHook(ch);
   
    // ==========================================
    // 全ステート共通の処理：ハングアップ（確実な切断）検知
    // ==========================================
    if (line->current_hook == false) { // 現在L（オンフック）なら
        if (line->last_hook == true) {
            line->hangup_timer = 1000;
        }
        
        if (line->hangup_timer == 0) {
            // 着信中（RINGING）はオンフックが正常なのでハングアップから除外する
            if (line->state != STATE_IDLE && line->state != STATE_RINGING) {
                
                // --- 相手がいる場合の連動処理 ---
                if (line->target_port != 0xFF) {
                    uint8_t t_ch = line->target_port;
                    
                    // 発信者が呼び出し中(CALLING)に諦めて切った場合、相手のベルを止める
                    if (line->state == STATE_CALLING) {
                        HAL_SetRing(t_ch, false);
                        lines[t_ch].state = STATE_IDLE;
                        lines[t_ch].target_port = 0xFF;
                        printf("Port %d: Caller aborted. Port %d stopped ringing.\r\n", ch + 1, t_ch + 1);
                    }
                    // 通話中(TALKING)に自分が切った場合、残された相手を話中(BUSY)にする
                    // ただし相手がAUTO ANSWERモードの場合にはBUSYにしない
                    else if (line->state == STATE_TALKING) {
                        SwitchControl(false, ch + 1, t_ch + 1); // スイッチボード切断
                        if (lines[t_ch].state != STATE_AUTOANS){ //AUTO ANSWERモード以外の場合
                            HAL_SetTone(t_ch, TONE_BUSY);           // 相手にツーツー音
                            lines[t_ch].state = STATE_BUSY;
                            lines[t_ch].target_port = 0xFF;
                            printf("Port %d: Hung up during talk. Port %d is now BUSY.\r\n", ch + 1, t_ch + 1);
                        }
                        else { //AUTO ANSWERモードの場合はターゲットポートだけ書き換え相手に対しては何もしない
                            lines[t_ch].target_port = 0xFF;
                            printf("Port %d: Hung up during talk. Port %d is now AUTO ANSWER.\r\n", ch + 1, t_ch + 1);
                        }
                    }
                }

                // --- 自分自身の切断（クリーンアップ）処理 ---
                HAL_SetTone(ch, TONE_OFF);
                HAL_SetRing(ch, false);
                // SLIC接続されている場合かつAUTO ANSWERに設定されている場合にはIDLEに戻さない
                if(line->state != STATE_AUTOANS){
                    line->state = STATE_IDLE;
                    line->target_port = 0xFF; // 紐付け解除
                    printf("Port %d: Hung up. -> IDLE\r\n", ch + 1);
                }
            }
        }
    }

    // ==========================================
    // ステートごとの個別処理
    // ==========================================
    switch (line->state) {
        case STATE_IDLE:
            // オフフックされたらダイヤルトーンへ遷移
            if (line->last_hook == false && line->current_hook == true) {
                if (port_configs[ch].hotline_ext != 0xFF) {
                    // ダイアルされた番号としてバッファにセットする (2桁前提)
                    line->dialed_number[0] = port_configs[ch].hotline_ext / 10;
                    line->dialed_number[1] = port_configs[ch].hotline_ext % 10;
                    line->dialed_digits = MAX_DIGITS;
                    line->state = STATE_ROUTING; // そのままルーティングへ直行
                    printf("Port %d: Off-Hook -> HOTLINE to Ext %d -> ROUTING\r\n", ch + 1, port_configs[ch].hotline_ext);
                } 
                else {
                    HAL_SetTone(ch, TONE_DIAL);
                    line->dp_count = 0;
                    line->dialed_digits = 0;
                    line->state = STATE_DIALTONE;
                    printf("Port %d: Off-Hook -> DIALTONE\r\n", ch + 1);
                }
            }
            break;

        case STATE_DIALTONE:
            // 最初のダイヤルパルス（立ち下がり）を検知したらトーンを止めてダイヤル受信へ
            if (line->last_hook == true && line->current_hook == false) {
                HAL_SetTone(ch, TONE_OFF); // ツー音停止
                line->dp_count = 1;        // 1パルス目
                line->dp_timer = 600;      // タイムアウトタイマーセット (600ms)
                line->state = STATE_DIALING;
                printf("Port %d: Dialing started -> DIALING\r\n", ch + 1);
            }
            break;

        case STATE_DIALING:
            // パルスカウント処理
            if (line->last_hook == true && line->current_hook == false) {
                line->dp_count++;
                line->dp_timer = 600; // パルスが来るたびにタイマーをリセット
            }
            
            // 桁確定のタイムアウト判定 (H状態が続いてタイマーが0になったら)
            if (line->current_hook == true && line->dp_timer == 0 && line->dp_count > 0) {
                uint8_t digit = (line->dp_count == 10) ? 0 : line->dp_count;
                line->dialed_number[line->dialed_digits] = digit;
                line->dialed_digits++;
                line->dp_count = 0;
                
                printf("Port %d: Digit %d received\r\n", ch + 1, digit);

                // 全桁（MAX_DIGITSで定義）受信完了したらルーティング処理へ
                if (line->dialed_digits >= MAX_DIGITS) {
                    line->state = STATE_ROUTING;
                    printf("Port %d: Number complete -> ROUTING\r\n", ch + 1);
                }
            }
            break;

        case STATE_ROUTING:
        {
            // ダイヤルされた2桁の配列を数値に変換 (例: [1][2] -> 12)
            uint8_t dialed_val = (line->dialed_number[0] * 10) + line->dialed_number[1];
            uint8_t target_ch = 0xFF; // 見つからなかった場合の初期値

            // ルーティングテーブルを検索
            for (uint8_t i = 0; i < current_max_lines; i++) {
                if (port_configs[i].extension == dialed_val) {
                    target_ch = i;
                    break;
                }
            }

            // 存在しない番号、または自分自身にかけた場合
            if (target_ch == 0xFF || target_ch == ch) {
                HAL_SetTone(ch, TONE_BUSY); // ビジートーン
                line->state = STATE_BUSY;
                printf("Port %d: Invalid number %d -> BUSY\r\n", ch + 1, dialed_val);
            } 
            else {
                // 相手の状態を確認 (STATE_IDLEなら呼び出し可能)
                if (lines[target_ch].state == STATE_IDLE) {
                    
                    // お互いのtarget_portを紐づける
                    line->target_port = target_ch;
                    lines[target_ch].target_port = ch;

                    // 発信側(自分)の処理: リングバックトーンを鳴らし、CALLING状態へ
                    HAL_SetTone(ch, TONE_RINGBACK);
                    line->state = STATE_CALLING;

                    // 着信側(相手)の処理: ベルを鳴らし、RINGING状態へ
                    HAL_SetRing(target_ch, true);
                    lines[target_ch].state = STATE_RINGING;

                    printf("Port %d: Calling Port %d -> CALLING\r\n", ch + 1, target_ch + 1);
                } 
                // 相手側がAUTO ANSWERの場合
                else if(lines[target_ch].state == STATE_AUTOANS){
                    line->target_port = target_ch;
                    lines[target_ch].target_port = ch;
                    // 発信側(自分)の処理: トーンをオフにしTALKING状態に
                    HAL_SetTone(ch, TONE_OFF);
                    line->state = STATE_TALKING;
                    // 着信側(相手)の処理: 何もせず音声を繋ぎこむ
                    // スイッチボードを接続する (内線番号ではなく内部ポート+1を渡す)
                    uint8_t caller = line->target_port;
                    SwitchControl(true, ch + 1, caller + 1);
                    printf("Port %d: Called Port %d -> AUTO ANSWERed\r\n", ch + 1, target_ch + 1);

                }
                else {
                    // 相手が話中などの場合
                    HAL_SetTone(ch, TONE_BUSY);
                    line->state = STATE_BUSY;
                    printf("Port %d: Port %d is busy -> BUSY\r\n", ch + 1, target_ch + 1);
                }
            }
            break;
        }

        case STATE_BUSY:
            // ビジートーン鳴動中。利用者が受話器を置く（ハングアップタイマーが働く）まで何もしない
            break;

        case STATE_RINGING:
            // 着信側が受話器を上げた（オフフックした）場合の処理
            if (line->last_hook == false && line->current_hook == true) {
                // 自分のベルを止める
                HAL_SetRing(ch, false);
                
                // 発信側（CALLINGで待っている相手）のリングバックトーンを止める
                uint8_t caller = line->target_port;
                HAL_SetTone(caller, TONE_OFF);
                
                // スイッチボードを接続する (内線番号ではなく内部ポート+1を渡す)
                SwitchControl(true, ch + 1, caller + 1);
                // お互いをTALKING状態へ遷移
                line->state = STATE_TALKING;
                lines[caller].state = STATE_TALKING;
                
                printf("Port %d: Answered Port %d -> TALKING\r\n", ch + 1, caller + 1);
            }
            break;

        case STATE_CALLING:
            // リングバックトーンを聞きながら相手が出るのを待つステート。
            break;

        case STATE_TALKING:
            // 通話中。どちらかが受話器を置くまで（ハングアップ検知されるまで）何もしない。
            break;

        case STATE_UNAVAIL:
            // 回線使用不可。BUSYと同じ扱いなので何もしない。
            break;
        case STATE_AUTOANS:
            break;
    }

    // フック状態を履歴に保存
    line->last_hook = line->current_hook;
}

// コマンドライン処理
void ProcessCommandLine(const char* str){
    // 空だったら何もしない
    if(str[0] == 0) return;
    
    printf("\r\n"); //入力されたコマンド行を画面上書きしないように
    // STATコマンドの処理
    // 注意：ポート番号(物理回線番号)は内部的には 0 - (MAX_LINES-1)だが
    //       コマンド等の「見た目」は1-の番号としているので-1,+1で表示さ
    //       せているので注意
    if (strcmp(str, "STAT") == 0) {
        printf("--- Line Status -------\r\n");
        for (uint8_t i = 0; i < current_max_lines; i++) {
            printf("Port %d [Ext:%d] : %-8s", 
                   i + 1, 
                   port_configs[i].extension, 
                   GetStateString(lines[i].state));
            
            // 誰かと接続・呼び出し中であれば相手のポートも表示
            if (lines[i].target_port != 0xFF) {
                printf(" (Target: Port %d)", lines[i].target_port + 1);
            }
            
            // デバッグ用に現在のフック状態（物理ピン）も表示
            printf(" | Hook:%s", lines[i].current_hook ? "OFF(H)" : "ON(L)");
            //ホットライン設定表示
            if(port_configs[i].hotline_ext != 0xff){
                printf("  | Hotline:%02d", port_configs[i].hotline_ext);           
            }
            
            printf("\r\n");
        }
        printf("-----------------------\r\n");
    }
    // SET EXT コマンドの処理 (例: "SET EXT 0 11")
    else if (strncmp(str, "SET EXT ", 8) == 0) {
        // 簡易的なパース (str[8]がポート番号、str[10]とstr[11]が内線番号)
        uint8_t port = str[8] - '1';
        
        // フォーマットと範囲のチェック
        if (port < current_max_lines && str[9] == ' ' && str[10] >= '0' && str[10] <= '9') {
            uint8_t ext = (str[10] - '0') * 10 + (str[11] - '0'); // 文字列から数値へ変換
            
            // このコマンドではメモリ上の設定のみ更新
            port_configs[port].extension = ext;
            
            printf("Success: Port %d extension set to %d\r\n", port + 1, ext);
        } else {
            printf("Error: Invalid format.\r\n");
            printf("Usage: SET EXT <port:1-%d> <ext:10-99>\r\n", current_max_lines);
        }
    }
    // SET AA コマンドの処理 (例: "SET AA 0 ON/OFF")
    // 特定ポートをAUTO ANSWERにする。ページングやラジオを聞くのような処理に使う
    // ポートにSLICが実装されていなくとも動作する(オーディオのスイッチング用)
    else if (strncmp(str, "SET AA ", 7) == 0) {
        // 簡易的なパース (str[7]がポート番号)
        uint8_t port = str[7] - '1';
        uint8_t tmp_mode = 0;
        //str[9]からON/OFFが入る
        if(strncmp(str+9, "ON" ,2) == 0){
            tmp_mode = 2;
        }
        else if(strncmp(str+9, "OFF", 3) == 0){
            tmp_mode = 1;
        }
        
        // ポート範囲とモードのチェック
        if (port < current_max_lines && tmp_mode != 0) {
            if(tmp_mode == 1){
                lines[port].state = STATE_IDLE;                     
                printf("Success: Port %d state set to IDLE\r\n", port + 1);
            }
            else if(tmp_mode == 2){
                lines[port].state = STATE_AUTOANS;
                printf("Success: Port %d state set to AUTO ANSWER\r\n", port + 1);
            }
            else {
                printf("Error: Port %d state error\r\n", port + 1);
            }
        } else {
            printf("Error: Invalid format.\r\n");
            printf("Usage: SET AA <port:1-%d> <ON/OFF>\r\n", current_max_lines);
        }
    }
    else if (strcmp(str, "DO_FULL_RESET") == 0) {
        printf("Resetting PBXCore...\r\n");
        __delay_ms(1000);
        RESET();
    }
    // スイッチボード手動接続・開放処理
    // SBCTL コマンドの処理 (例: "SBCTL CON 1 2")
    else if (strncmp(str, "SBCTL ", 6) == 0) {
        // サブコマンド "CON " (接続)
        if (strncmp(&str[6], "CON ", 4) == 0) {
            uint8_t line1 = str[10] - '0';
            uint8_t line2 = str[12] - '0';
            
            // 実装回線分までしか使用させない場合には以下
            //if (line1 >= 1 && line1 <= MAX_LINES && line2 >= 1 && line2 <= MAX_LINES && str[11] == ' ') {
            // デバッグ用に実装回線分以上のスイッチも制御可能にする場合
            if (str[11] == ' ') {
            printf("Connect Switch: %d-%d %d-%d\r\n",line1, line2, line2, line1);
                SwitchControl(true, line1, line2);
                printf("Executed: SwitchControl(true, %d, %d)\r\n", line1, line2);
            } else {
                printf("Error: Invalid arguments.\r\n");
                printf("Usage: SBCTL CON <line1> <line2> (1-%d)\r\n", current_max_lines);
            }
        }
        // サブコマンド "REL " (切断)
        else if (strncmp(&str[6], "REL ", 4) == 0) {
            uint8_t line1 = str[10] - '0';
            uint8_t line2 = str[12] - '0';
            
            // 引数のチェック
            // if (line1 >= 1 && line1 <= MAX_LINES && line2 >= 1 && line2 <= MAX_LINES && str[11] == ' ') {
            if (str[11] == ' ') {
                printf("Release Switch: %d-%d %d-%d\r\n",line1, line2, line2, line1);
                SwitchControl(false, line1, line2);
                printf("Executed: SwitchControl(false, %d, %d)\r\n", line1, line2);
            } else {
                printf("Error: Invalid arguments.\r\n");
                printf("Usage: SBCTL REL <line1> <line2> (1-%d)\r\n", current_max_lines);
            }
        }
        // サブコマンド "FULL_RESET"
        else if (strcmp(&str[6], "FULL_RESET") == 0) {
            SoftwareUART_WriteString("RFFFF\r"); 
            printf("Executed: Switchboard Full Reset\r\n");
        }
        // 未知のサブコマンド
        else {
            printf("Error: Unknown SBCTL subcommand.\r\n");
            printf("Available: CON, REL, FULL_RESET\r\n");
        }
    }
    else if (strncmp(str, "SET HL ", 7) == 0) {
        uint8_t port = str[7] - '1';
        if (port < current_max_lines && str[8] == ' ') {
            if (strncmp(str + 9, "OFF", 3) == 0) {
                port_configs[port].hotline_ext = 0xFF; // 無効化
                printf("Success: Port %d hotline disabled\r\n", port + 1);
            } else if (str[9] >= '0' && str[9] <= '9') {
                uint8_t ext = (str[9] - '0') * 10 + (str[10] - '0');
                port_configs[port].hotline_ext = ext;
                printf("Success: Port %d hotline set to Ext %d\r\n", port + 1, ext);
            }
        } else {
            printf("Usage: SET HL <port:1-%d> <ext:10-99 or OFF>\r\n", current_max_lines);
        }
    }
    else if(strcmp(str, "SAVE_TO_EEPROM") == 0 ){ //メモリ上の設定をEEPROMに保存
        SaveSettings();
        printf("Success: Settings saved to EEPROM.\r\n");
    }
    else if(strcmp(str, "HELP") == 0){ //ヘルプコマンド
        printf("---Commands---\r\n");
        printf("STAT    : Display current Status.\r\n");
        printf("SET EXT : Set extension(number) for each port.\r\n");
        printf("          Usage: SET EXT <port:1-%d> <ext:10-99>\r\n", current_max_lines);
        printf("SET AA  : Set port to AUTO ANSWER mode.\r\n");
        printf("          Usage: SET AA  <port:1-%d> <ON/OFF>\r\n", current_max_lines);
        printf("SET HL  : Set port HOTLINE number\r\n");
        printf("          Usage: SET HL <port:1-%d> <ext:10-99 or OFF>\r\n", current_max_lines);
        printf("SBCTL   : Manually ON/OFF/FULL_RESET Switchboard.\r\n");
        printf("          Usage : SBCTL CON/REL <port1> <port2>\r\n");
        printf("          Example: SBCTL CON 1 2   - Connect 1 and 2 Switch.\r\n");
        printf("          Example: SBCTL REL 1 2   - Release 1 and 2 Switch.\r\n");
        printf("          Example: SBCTL FULL_RESET  - Reset Switchboard.\r\n");
        printf("\r\n");
        printf("SAVE_TO_EEPROM : Save current settings to EEPROM.\r\n");
        printf("DO_FULL_RESET  : Reset PBXCore program.\r\n");
        printf("--------------\r\n");
        
    }
    // 未知のコマンドの場合
    else {
        printf("Unknown Command: %s\r\n", str);
    }
}