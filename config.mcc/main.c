#include "mcc_generated_files/nvm/nvm.h"
#include "mcc_generated_files/system/system.h"
#include "hal_pbx.h"
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <xc.h>

/*
    PBXメイン(core)プログラム
     ハードウェア依存部分はhal_pbx.hとhal_pbx.c,hal_cid.cで定義する
     2ライン、SLIC直結専用にサイズダウン

    このプログラムに依存する部分としてMCCで以下を設定すること
     Clock : HFINTOSC 32MHz (クロックを変えるとモデムやトーンが正しく動作しないので注意)
     EUSART : 9600bps割り込み有、printfリダイレクト有
     TMR2 : 1ms tickを生成し割り込み有
     TMR4 : 1ms tickを生成し割り込み有(HALが専用に使う)
     NVM : Flash(NVM) APIあり 設定データ領域をFlashに変更した
     注意:XC8のOptimize OptionでAddress Qualifierを"Require"に設定すること
          Flashの領域定義はhal_pbx.hで行っている
     WDT : HFINTOSCで1:522499
*/

// XC8コンパイラのバージョン取得（整数値で返る 例: 2310 = v2.31）
// ※XC8のバージョンによっては __XC8_VERSION または __XC8_VERSION__
#ifdef __XC8_VERSION
    #define COMPILER_VER __XC8_VERSION
#else
    #define COMPILER_VER 0
#endif

// バージョン番号
char* version_string = "1.4.0_MINI-PBX";

// 内線番号の最大桁数は4桁
// ただし特番処理等で4桁以上を扱うので+2している個所に注意
#define MAX_EXT_DIGITS 4
// ダイアルタイムアウト(発信への移行時間:ミリ秒)
#define DIAL_TIMEOUT 3000

// 接続されている回線数(最大値をデフォルト:後で上書き)
uint8_t current_max_lines = 4;

// 各ポート毎の設定保持構造体 (Flash保存用・多桁ホットライン対応版)
typedef struct {
    uint16_t extension;     // 内線番号 (例: 11～)
    uint8_t initial_state; // 初期ステート (STATE_AUTOANSなど)
    uint8_t hotline_len;                  // ホットライン番号の桁数 (0の場合は無効)
    uint8_t hotline_num[MAX_EXT_DIGITS];   // ホットライン用配列
    uint8_t caller_id_enabled;
    uint8_t reserved;      // 予約・パディング用
} PortConfig;

// 設定データの実体
PortConfig port_configs[TOTAL_MAX_LINES];

// ステート定義
typedef enum {
    STATE_IDLE = 0,
    STATE_DIALTONE,
    STATE_DIALING,
    STATE_ROUTING,
    STATE_CALLING,
    STATE_RINGING,
    STATE_TALKING,
    STATE_BUSY,
    STATE_UNAVAIL,
    STATE_AUTOANS,
    STATE_SP_NUMBER,
    STATE_SP_NUMBER_WAIT1,
    STATE_SP_NUMBER_WAIT2,
    STATE_SP_NUMBER_WAIT3
} PBX_State;

typedef struct {
    PBX_State state;        // 現在のステート
    bool current_hook;      // 現在のフック状態
    bool last_hook;         // 前回のフック状態
    
    // ダイヤル処理用
    uint8_t dp_count;       // 現在カウント中のパルス数
    uint8_t dialed_digits;  // 受信完了した桁数
    uint8_t dialed_number[MAX_EXT_DIGITS+2]; // 受信した番号を格納
    
    // タイマー（1msTickで減算）
    volatile uint16_t dp_timer;    
    volatile uint16_t state_timer; 
    volatile uint16_t hangup_timer;

    // 通話相手の物理ポート番号 (0-3)、未接続時は 0xFF などを入れる
    uint8_t target_port;

    // 特番処理用
    volatile uint16_t special_number;
    volatile uint8_t indication_tones;
    
} LineContext;

LineContext lines[TOTAL_MAX_LINES];

// Configレジスタ読み出し
uint16_t read_configuration_space(uint16_t address) {
    // 1. Load the target address into the NVM Address registers
    NVMADRH = (uint8_t)((address >> 8) & 0xFF);
    NVMADRL = (uint8_t)(address & 0xFF);
    
    // 2. Configure NVMREG access for Configuration/Device ID space
    NVMCON1bits.NVMREGS = 1; // Access Configuration, User ID, Device ID, or EEPROM

    // 3. Initiate the read operation
    NVMCON1bits.RD = 1;     // Setting this bit initiates a hardware read cycle
    NOP();                  // Required NOP execution directly after read flag
    NOP();                  // Required NOP execution directly after read flag
    
    // 4. Combine the High and Low result registers into a 14-bit word
    return ((uint16_t)NVMDATH << 8) | NVMDATL;
}

// Flash(NVM)への設定保存
void SaveSettings(void) {
    uint16_t total_bytes = 1 + sizeof(port_configs); 
    flash_address_t current_addr = HAL_STORAGE_START_ADDR;
    uint16_t bytes_written = 0;
    
    flash_data_t row_buf[PROGMEM_PAGE_SIZE];

    // NVMのアンロックキーをセットする
    NVM_UnlockKeySet(UNLOCK_KEY);

    while (bytes_written < total_bytes) {
        for (int i = 0; i < PROGMEM_PAGE_SIZE; i++) {
            uint8_t data_byte = 0xFF; 
            
            if (bytes_written == 0) {
                data_byte = NVM_MAGIC_VAL;
            } else if ((bytes_written - 1) < sizeof(port_configs)) {
                uint8_t *ptr = (uint8_t *)port_configs;
                data_byte = ptr[bytes_written - 1];
            }
            
            row_buf[i] = data_byte; 
            bytes_written++;
        }
        
        // タイミングクリティカルなNVM操作の直前で全割り込みを一時停止
        INTERRUPT_GlobalInterruptDisable();
        
        FLASH_PageErase(current_addr);
        FLASH_RowWrite(current_addr, row_buf);
        
        // NVM操作が終わったら割り込みを直ちに再開
        INTERRUPT_GlobalInterruptEnable();
        
        current_addr += PROGMEM_PAGE_SIZE; 
    }
}

// Flash(NVM)からの設定取得
void LoadSettings(void) {
    flash_address_t current_addr = HAL_STORAGE_START_ADDR;
    
    // 先頭1バイト(マジックナンバー)を読み出し
    uint8_t magic = (uint8_t)(FLASH_Read(current_addr++) & 0xFF);
    
    if (magic == NVM_MAGIC_VAL) {
        
        // 構造体配列の復元
        uint8_t *ptr = (uint8_t *)port_configs;
        for (uint16_t i = 0; i < sizeof(port_configs); i++) {
            ptr[i] = (uint8_t)(FLASH_Read(current_addr++) & 0xFF);
        }
        
        // 読み込んだ設定をステートに反映
        for(uint8_t i = 0; i < current_max_lines; i++){
            if(port_configs[i].initial_state == STATE_AUTOANS){
                lines[i].state = STATE_AUTOANS;
            }
        }
    } else {
        // 初回起動時（またはフォーマット時）：デフォルト値を設定してFlashに保存
        for(uint8_t i = 0; i < TOTAL_MAX_LINES; i++){
            port_configs[i].extension = 11 + i;
            port_configs[i].initial_state = 0;
            port_configs[i].hotline_len = 0; // ホットライン無効化
            port_configs[i].reserved = 0;
            port_configs[i].caller_id_enabled = 0;
            memset(port_configs[i].hotline_num, 0, MAX_EXT_DIGITS);
        }
        SaveSettings();
        printf("PBXCore: Storage initialized\r\n");
    }
}
// ==========================================================


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
        case STATE_SP_NUMBER: return "SPECIAL_N";
        default:             return "UNKNOWN";
    }
}

// ダイヤルパルスカウント用Tick処理
void PBX_SystemTick_1ms(void) {
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

// 1mS割り込みハンドラ
volatile uint8_t system_tick_count = 0;

void TMR2_Tick_Handler(void) {
    system_tick_count++;
}

#define CMDBUFSIZE 64
char rxBuffer[CMDBUFSIZE];
uint8_t rxIndex = 0;

// ヒストリ機能用
char lastCmdBuffer[CMDBUFSIZE] = {0}; 
uint8_t escState = 0;

// プロトタイプ宣言
void ProcessStateMachine(uint8_t ch);
void ProcessCommandLine(const char *str);

/*
    メイン
*/
int main(void)
{
    __delay_ms(500);

    // システム初期化
    SYSTEM_Initialize();
    uint16_t device_id = read_configuration_space(0x8006);
    uint16_t revision_id = read_configuration_space(0x8007);
 
    INTERRUPT_GlobalInterruptEnable(); 
    INTERRUPT_PeripheralInterruptEnable(); 

    printf("\r\n\r\n");
    printf("MINI-PBXCore: Starting (%s)\r\n",version_string);
    printf(" PIC DeviceID: %x(%x)\r\n",device_id,revision_id);
    printf(" Built at: %s %s (XC8 v%d)\r\n", __DATE__, __TIME__, COMPILER_VER);
    printf(" Flash Size: %u Words\r\n", PROGMEM_SIZE);

    uint16_t tmp_st_size = HAL_STORAGE_SIZE;
    uint16_t tmp_st_addr = HAL_STORAGE_START_ADDR;
    printf(" Storage start address = %0x\r\n",tmp_st_addr);
    printf(" Storage size %d words.\r\n",tmp_st_size);

    HAL_PBX_Init();
    current_max_lines = HAL_GetMaxLines();
    printf("PBXCore: Active LINES: %d\r\n", current_max_lines);

    printf("PBXCore: Initializing Switchboard ");
    HAL_Switch_Control(false);
    __delay_ms(100);

    printf("done.\r\n");
    
    printf("PBXCore: Testing Switch.\r\n");
    HAL_Switch_Control(true);
    __delay_ms(500);
    HAL_Switch_Control(false);

    printf("PBXCore: Resetting All States");
    for (uint8_t i = 0; i < current_max_lines; i++) {
        lines[i].state = STATE_IDLE;
        lines[i].current_hook = HAL_GetHook(i);
        lines[i].last_hook = lines[i].current_hook;
        lines[i].dp_count = 0;
        lines[i].dialed_digits = 0;
        lines[i].dp_timer = 0;
        lines[i].state_timer = 0;
        lines[i].target_port = 0xFF; 
        lines[i].special_number = 0;
        lines[i].indication_tones = 0;
    }
    __delay_ms(100);
    printf(" - done.\r\n");

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

    // Flashからの設定読み出し
    printf("PBXCore: Loading settings from NVM.\r\n");
    LoadSettings();
    for(uint8_t i = 0; i < current_max_lines; i++){
        printf(" Port %2d : Ext %d\r\n", i + 1, port_configs[i].extension);
    }

    printf("\r\n");
    printf("PBXCore: === PBX Ready ===\r\n");

    TMR2_PeriodMatchCallbackRegister(TMR2_Tick_Handler);
 
    while(1)
    {
        CLRWDT();

        // TickタイマをみてPBX_SstemTickを実行
        // メインの処理にTickを同期させる
        while (system_tick_count > 0) {
            system_tick_count--;
            PBX_SystemTick_1ms();
        }

        for(uint8_t i = 0; i < current_max_lines; i++){
            ProcessStateMachine(i);
        }

        if(EUSART_IsRxReady()){
            uint8_t rxData = EUSART_Read();
            
            // エスケープシーケンス処理
            if (escState == 0 && rxData == 0x1B) { 
                escState = 1;
                continue; 
            } else if (escState == 1) {
                if (rxData == '[') {
                    escState = 2;
                } else {
                    escState = 0; 
                }
                continue;
            } else if (escState == 2) {
                if (rxData == 'A') { 
                    for (uint8_t i = 0; i < rxIndex; i++) {
                        printf("\b \b");
                    }
                    strcpy(rxBuffer, lastCmdBuffer);
                    rxIndex = (uint8_t) strlen(rxBuffer);
                    printf("%s", rxBuffer);
                }
                escState = 0;
                continue;
            }

            if(rxData == '\r' || rxData == '\n'){
                EUSART_Write(rxData); 
                rxBuffer[rxIndex] = '\0';
                
                if (rxIndex > 0) {
                    strcpy(lastCmdBuffer, rxBuffer);
                }

                ProcessCommandLine(rxBuffer);
                printf("\r\n");
                printf("PBX> ");
                rxIndex = 0;
            }
            else { 
                if(rxData == '\b' || rxData == 0x7f){
                    if(rxIndex > 0){
                        rxIndex--;
                        printf("\b \b"); 
                    }
                }
                else if(rxIndex < CMDBUFSIZE -1 ){ 
                    EUSART_Write(rxData); 
                    
                    if(rxData >= 'a' && rxData <='z'){
                        rxData -= 0x20; 
                    }
                    rxBuffer[rxIndex++] = (char)rxData;
                }
            }
        }
    }
}


// 各回線処理のステートマシン
void ProcessStateMachine(uint8_t ch) {
    LineContext *line = &lines[ch]; 
    line->current_hook = HAL_GetHook(ch);
   
    if (line->current_hook == false) { 
        if (line->last_hook == true) {
            line->hangup_timer = 1000;
        }
        
        if (line->hangup_timer == 0) {
            if (line->state != STATE_IDLE &&
                line->state != STATE_RINGING ) {
                
                if (line->target_port != 0xFF) {
                    uint8_t t_ch = line->target_port;
                    
                    if (line->state == STATE_CALLING) {
                        HAL_SetRing(t_ch, false);
                        lines[t_ch].state = STATE_IDLE;
                        lines[t_ch].target_port = 0xFF;
                        printf("Port %d: Caller aborted. Port %d stopped ringing.\r\n", ch + 1, t_ch + 1);
                    }
                    else if (line->state == STATE_TALKING) {
                        HAL_Switch_Control(false); 
                        if (lines[t_ch].state != STATE_AUTOANS){ 
                            HAL_SetTone(t_ch, TONE_BUSY);           
                            lines[t_ch].state = STATE_BUSY;
                            lines[t_ch].target_port = 0xFF;
                            printf("Port %d: Hung up during talk. Port %d is now BUSY.\r\n", ch + 1, t_ch + 1);
                        }
                        else { 
                            lines[t_ch].target_port = 0xFF;
                            printf("Port %d: Hung up during talk. Port %d is now AUTO ANSWER.\r\n", ch + 1, t_ch + 1);
                        }
                    }
                }

                HAL_SetTone(ch, TONE_OFF);
                HAL_SetRing(ch, false);
                if(line->state != STATE_AUTOANS){
                    line->state = STATE_IDLE;
                    line->target_port = 0xFF; 
                    printf("Port %d: Hung up. -> IDLE\r\n", ch + 1);
                }
            }
        }
    }

    switch (line->state) {
        case STATE_IDLE:
            if (line->last_hook == false && line->current_hook == true) {
                
                // 多桁ホットライン発信処理
                if (port_configs[ch].hotline_len > 0) {
                    for (uint8_t i = 0; i < port_configs[ch].hotline_len; i++) {
                        line->dialed_number[i] = port_configs[ch].hotline_num[i];
                    }
                    line->dialed_digits = port_configs[ch].hotline_len;                   
                    line->state = STATE_ROUTING; 
                    
                    printf("Port %d: Off-Hook -> HOTLINE to ", ch + 1);
                    for(uint8_t i = 0; i < line->dialed_digits; i++) printf("%d", line->dialed_number[i]);
                    printf(" -> ROUTING\r\n");
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
            if (line->last_hook == true && line->current_hook == false) {
                HAL_SetTone(ch, TONE_OFF); 
                line->dp_count = 1;        
                line->dp_timer = 600;      
                line->state = STATE_DIALING;
                printf("Port %d: Dialing started -> DIALING\r\n", ch + 1);
            }
            break;

        case STATE_DIALING:
            if (line->last_hook == true && line->current_hook == false) {
                line->dp_count++;
                line->dp_timer = 600;
            }
            
            if (line->current_hook == true && line->dp_timer == 0 && line->dp_count > 0) {
                uint8_t digit = (line->dp_count == 10) ? 0 : line->dp_count;
                line->dialed_number[line->dialed_digits] = digit;
                line->dialed_digits++;
                line->dp_count = 0;
                
                line->state_timer = DIAL_TIMEOUT;

                printf("Port %d: Digit %d received\r\n", ch + 1, digit);

                if (line->dialed_digits >= MAX_EXT_DIGITS+2) {
                    line->state = STATE_ROUTING;
                }
            }

            if (line->current_hook == true && line->dp_timer == 0 && line->dp_count == 0 
                && line->dialed_digits > 1) {
                    if(line->state_timer == 0){ 
                    printf("Port %d: Dialing complete -> ROUTING\r\n", ch + 1);
                    line->state = STATE_ROUTING;
                }
            }
            break;

        case STATE_ROUTING:
        {
            uint16_t dialed_val = 0;
            for(uint8_t i = 0; i < line->dialed_digits; i++) {
                dialed_val = (dialed_val * 10) + line->dialed_number[i];
            }

            //特番処理
            if(dialed_val == 24370 || dialed_val == 24371){
                line->special_number = dialed_val;
                line->state = STATE_SP_NUMBER;
                break;
            }

            uint8_t target_ch = 0xFF; 

            for (uint8_t i = 0; i < current_max_lines; i++) {
                if (port_configs[i].extension == dialed_val) {
                    target_ch = i;
                    break;
                }
            }

            if (target_ch == 0xFF || target_ch == ch ) {
                HAL_SetTone(ch, TONE_BUSY); 
                line->state = STATE_BUSY;
                printf("Port %d: Invalid number %d -> BUSY\r\n", ch + 1, dialed_val);
            } 
            else {
                if (lines[target_ch].state == STATE_IDLE) {
                    line->target_port = target_ch;
                    lines[target_ch].target_port = ch;
                    
                    if (port_configs[target_ch].caller_id_enabled == 1) {
                        char cid_string[MAX_EXT_DIGITS+2];
                        sprintf(cid_string, "%d", port_configs[ch].extension);
                        printf("Port %d: Sending CID to Port %d (CID: %s)\r\n", ch + 1, target_ch + 1, cid_string);
                        // UART 掃き出し待ち
                        __delay_ms(100);
                        // CID送出。モデムタイミング維持のため割り込みブロック
                        // ここで割り込みが全てブロックされるのでこのコアは多回線処理には向かない
                        INTERRUPT_GlobalInterruptDisable(); 
                        INTERRUPT_PeripheralInterruptDisable();
                        Send_NTT_CallerID(target_ch, cid_string);
                        // 送出完了後は割り込み復旧
                        INTERRUPT_GlobalInterruptEnable(); 
                        INTERRUPT_PeripheralInterruptEnable();
                    }

                    HAL_SetTone(ch, TONE_RINGBACK);
                    line->state = STATE_CALLING;

                    HAL_SetRing(target_ch, true);
                    lines[target_ch].state = STATE_RINGING;

                    printf("Port %d: Calling Port %d -> CALLING\r\n", ch + 1, target_ch + 1);
                } 
                else if(lines[target_ch].state == STATE_AUTOANS){
                    line->target_port = target_ch;
                    lines[target_ch].target_port = ch;
                    HAL_SetTone(ch, TONE_OFF);
                    line->state = STATE_TALKING;
                    uint8_t caller = line->target_port;
                    HAL_Switch_Control(true);
                    printf("Port %d: Called Port %d -> AUTO ANSWERed\r\n", ch + 1, target_ch + 1);
                }
                else {
                    HAL_SetTone(ch, TONE_BUSY);
                    line->state = STATE_BUSY;
                    printf("Port %d: Port %d is busy -> BUSY\r\n", ch + 1, target_ch + 1);
                }
            }
            break;
        }

        case STATE_BUSY:
            break;

        case STATE_RINGING:
            if (line->last_hook == false && line->current_hook == true) {
                HAL_SetRing(ch, false);
                uint8_t caller = line->target_port;
                HAL_SetTone(caller, TONE_OFF);
                
                HAL_Switch_Control(true);
                line->state = STATE_TALKING;
                lines[caller].state = STATE_TALKING;
                
                printf("Port %d: Answered Port %d -> TALKING\r\n", ch + 1, caller + 1);
            }
            break;

        case STATE_SP_NUMBER: // 特番処理開始 (800Hz ON)
            printf("  Special #%d", line->special_number);
            if(line->special_number == 24371){
                printf(" Port %d CID support set to ON\r\n",ch);
                port_configs[ch].caller_id_enabled = 1;
                line->indication_tones = 1;
            }
            else if(line->special_number == 24370){
                printf(" Port %d CID support set to OFF\r\n",ch);
                port_configs[ch].caller_id_enabled = 0;
                line->indication_tones = 2;
            }
            else {
                printf(" Unkown # \r\n");
                line->indication_tones = 3;
            }
            line->state = STATE_SP_NUMBER_WAIT1;
            break;
        case STATE_SP_NUMBER_WAIT1: //インディケーショントーン回数制御のためトーン用STATE
            NCO1_SetIncrement(TONE_800Hz);
            HardwareToneControl(ch, true); 
            
            line->state_timer = 100; // 200msのタイマーをセット
            line->state = STATE_SP_NUMBER_WAIT2; // 次の待機ステートへ遷移
            break;

        case STATE_SP_NUMBER_WAIT2: // 200ms経過待ち
            if (line->state_timer == 0) {
                HardwareToneControl(ch, false); // 800Hz OFF
                NCO1_SetIncrement(TONE_400HZ);  // 400Hzに戻す
                
                line->state_timer = 500; // 500msのタイマーをセット
                line->state = STATE_SP_NUMBER_WAIT3;
            }
            break;

        case STATE_SP_NUMBER_WAIT3: // 500ms経過待ち
            if (line->state_timer == 0) {
                line->indication_tones--;
                if(line->indication_tones > 0){ //鳴らす回数に応じて次のSTATE切替え
                    line->state = STATE_SP_NUMBER_WAIT1;
                }
                else { // 特番処理が終わったらBUSYで電話を切らせる
                    HAL_SetTone(ch, TONE_BUSY);
                    line->state = STATE_BUSY;
                }
            }
            break;

        case STATE_CALLING:
            break;
        case STATE_TALKING:
            break;
        case STATE_UNAVAIL:
            break;
        case STATE_AUTOANS:
            break;

    }
    line->last_hook = line->current_hook;
}

// コマンドライン処理
void ProcessCommandLine(const char* str){
    if(str[0] == 0) return;
    
    printf("\r\n");
    if (strcmp(str, "STAT") == 0) {
        printf("** MINI-PBX Core Ver. %s\r\n",version_string);
        printf("--- Line Status -------\r\n");
        for (uint8_t i = 0; i < current_max_lines; i++) {
            printf("Port %d [Ext:%u] (SLIC) : %-8s", 
                   i + 1, 
                   port_configs[i].extension,
                   GetStateString(lines[i].state));
            
            if (lines[i].target_port != 0xFF) {
                printf(" (Target: Port %d)", lines[i].target_port + 1);
            }
            
            printf(" | Hook:%s", lines[i].current_hook ? "OFF(H)" : "ON(L)");
            
            // ホットライン設定表示 (多桁対応)
            if(port_configs[i].hotline_len > 0){
                printf("  | Hotline:");           
                for(uint8_t d = 0; d < port_configs[i].hotline_len; d++){
                    printf("%d", port_configs[i].hotline_num[d]);
                }
            }
            if(port_configs[i].caller_id_enabled == 1){
                printf(" | CID:ON");
            } else {
                printf(" | CID:OFF");
            }
            printf("\r\n");
        }
        printf("-----------------------\r\n");
    }
    else if (strncmp(str, "SET EXT ", 8) == 0) {
        uint8_t port = str[8] - '1';
        if (port < current_max_lines && str[9] == ' ') {
            uint16_t ext = 0;
            uint8_t len = 0;
            
            // 1〜4桁の数字を可変長でパースする
            while (str[10 + len] >= '0' && str[10 + len] <= '9' && len < 4) {
                ext = (ext * 10) + (str[10 + len] - '0');
                len++;
            }
            
            if (len > 0) {
                port_configs[port].extension = ext;
                printf("Success: Port %d extension set to %u\r\n", port + 1, ext);
            } else {
                printf("Error: Invalid format.\r\n");
                printf("Usage: SET EXT <port:1-%d> <ext:1-9999>\r\n", current_max_lines);
            }
        } else {
            printf("Error: Invalid format.\r\n");
            printf("Usage: SET EXT <port:1-%d> <ext:1-9999>\r\n", current_max_lines);
        }
    }
    else if (strncmp(str, "SET AA ", 7) == 0) {
        uint8_t port = str[7] - '1';
        uint8_t tmp_mode = 0;
        if(strncmp(str+9, "ON" ,2) == 0){
            tmp_mode = 2;
        }
        else if(strncmp(str+9, "OFF", 3) == 0){
            tmp_mode = 1;
        }
        
        if (port < current_max_lines && tmp_mode != 0) {
            if(tmp_mode == 1){
                lines[port].state = STATE_IDLE;                     
                printf("Success: Port %d state set to IDLE\r\n", port + 1);
            }
            else if(tmp_mode == 2){
                lines[port].state = STATE_AUTOANS;
                printf("Success: Port %d state set to AUTO ANSWER\r\n", port + 1);
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
    else if (strncmp(str, "SBCTL ", 6) == 0) {
        if (strncmp(&str[6], "CON ", 4) == 0) {
            uint8_t line1 = str[10] - '0';
            uint8_t line2 = str[12] - '0';
            if (str[11] == ' ') {
            printf("Connect Switch: %d-%d %d-%d\r\n",line1, line2, line2, line1);
                HAL_Switch_Control(true);
                printf("Executed: SwitchControl(true, %d, %d)\r\n", line1, line2);
            } else {
                printf("Error: Invalid arguments.\r\n");
                printf("Usage: SBCTL CON <line1> <line2> (1-%d)\r\n", current_max_lines);
            }
        }
        else if (strncmp(&str[6], "REL ", 4) == 0) {
            uint8_t line1 = str[10] - '0';
            uint8_t line2 = str[12] - '0';
            if (str[11] == ' ') {
                printf("Release Switch: %d-%d %d-%d\r\n",line1, line2, line2, line1);
                HAL_Switch_Control(false);
                printf("Executed: SwitchControl(false, %d, %d)\r\n", line1, line2);
            } else {
                printf("Error: Invalid arguments.\r\n");
                printf("Usage: SBCTL REL <line1> <line2> (1-%d)\r\n", current_max_lines);
            }
        }
        else if (strcmp(&str[6], "FULL_RESET") == 0) {
            HAL_Switch_Control(false);
            printf("Executed: Switchboard Full Reset\r\n");
        }
        else {
            printf("Error: Unknown SBCTL subcommand.\r\n");
            printf("Available: CON, REL, FULL_RESET\r\n");
        }
    }
    // ホットライン設定コマンドのパース処理
    else if (strncmp(str, "SET HL ", 7) == 0) {
        uint8_t port = str[7] - '1';
        if (port < current_max_lines && str[8] == ' ') {
            if (strncmp(str + 9, "OFF", 3) == 0) {
                port_configs[port].hotline_len = 0; // 無効化
                printf("Success: Port %d hotline disabled\r\n", port + 1);
            } else {
                // 文字列から数値をパースして配列に格納する
                uint8_t len = 0;
                while (str[9 + len] >= '0' && str[9 + len] <= '9' && len < MAX_EXT_DIGITS) {
                    port_configs[port].hotline_num[len] = str[9 + len] - '0';
                    len++;
                }
                
                if (len > 0) {
                    port_configs[port].hotline_len = len;
                    printf("Success: Port %d hotline set to ", port + 1);
                    for(uint8_t i = 0; i < len; i++) printf("%d", port_configs[port].hotline_num[i]);
                    printf("\r\n");
                } else {
                    printf("Error: Invalid hotline number\r\n");
                }
            }
        } else {
            printf("Usage: SET HL <port:1-%d> <number or OFF>\r\n", current_max_lines);
        }
    }
    else if (strncmp(str, "SET CID ", 8) == 0) { //ナンバーディスプレイ対応設定
        uint8_t port = str[8] - '1';
        if (port < current_max_lines && str[9] == ' ') {
            if (strncmp(str + 10, "ON", 2) == 0) {
                port_configs[port].caller_id_enabled = 1;
                printf("Success: Port %d Caller ID enabled\r\n", port + 1);
            } else if (strncmp(str + 10, "OFF", 3) == 0) {
                port_configs[port].caller_id_enabled = 0;
                printf("Success: Port %d Caller ID disabled\r\n", port + 1);
            } else {
                goto cid_error;
            }
        } else {
cid_error:
            printf("Usage: SET CID <port:1-%d> <ON or OFF>\r\n", current_max_lines);
        }
    }
    else if(strcmp(str, "SAVE_SETTINGS") == 0){ 
        SaveSettings();
        printf("Success: Settings saved to NVM Flash.\r\n");
    }
    else if(strcmp(str, "HELP") == 0 || strcmp(str, "?") == 0){ 
        printf("---Commands---\r\n");
        printf("STAT    : Display current Status.\r\n");
        printf("SET EXT : Set extension(number) for each port.\r\n");
        printf("          Usage: SET EXT <port:1-%d> <ext:1-9999>\r\n", current_max_lines);
        printf("SET AA  : Set port to AUTO ANSWER mode.\r\n");
        printf("          Usage: SET AA  <port:1-%d> <ON/OFF>\r\n", current_max_lines);
        printf("SET HL  : Set port HOTLINE number (up to 16 digits)\r\n");
        printf("          Usage: SET HL <port:1-%d> <number or OFF>\r\n", current_max_lines);
        printf("SET CID : Set Caller ID (Number Display) enable/disable\r\n");
        printf("          Usage: SET CID <port:1-%d> <ON/OFF>\r\n", current_max_lines);
        printf("SBCTL   : Manually ON/OFF/FULL_RESET Switchboard.\r\n");
        printf("          Usage : SBCTL CON/REL <port1> <port2>\r\n");
        printf("          Example: SBCTL CON 1 2   - Connect 1 and 2 Switch.\r\n");
        printf("          Example: SBCTL REL 1 2   - Release 1 and 2 Switch.\r\n");
        printf("          Example: SBCTL FULL_RESET  - Reset Switchboard.\r\n");
        printf("\r\n");
        printf("SAVE_SETTINGS  : Save current settings to NVM Storage.\r\n");
        printf("DO_FULL_RESET  : Reset PBXCore program.\r\n");
        printf("--------------\r\n");
        
    }
    else {
        printf("Unknown Command: %s\r\n", str);
    }
}